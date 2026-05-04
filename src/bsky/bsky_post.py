#!/usr/bin/env python3

"""Bluesky posting helper adapted from Bryan Newbold's original script:

 https://gist.github.com/bnewbold
 https://bsky.app/profile/bnewbold.net

Supports hashtags, per-image alt text, and Pillow-derived aspect ratios.
Requires: requests, beautifulsoup4, pillow
    $ pip install requests beautifulsoup4 pillow
"""

from __future__ import annotations

import argparse
import io
import ipaddress
import json
import os
import re
import socket
import sys
import warnings
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional
from urllib.parse import ParseResult, urljoin, urlparse, urlunparse

import requests
from bs4 import BeautifulSoup
from PIL import Image, UnidentifiedImageError


DEFAULT_PDS_URL = "https://bsky.social"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 1_000_000
MAX_POST_GRAPHEMES = 300
MAX_EMBED_HTML_BYTES = 2_000_000
MAX_EMBED_IMAGE_BYTES = 1_000_000
MAX_IMAGE_PIXELS = 40_000_000
MAX_IMAGE_DIMENSION = 16_384
MAX_EXTERNAL_TITLE_CHARS = 300
MAX_EXTERNAL_DESCRIPTION_CHARS = 1_000
MAX_REDIRECTS = 3
USER_AGENT = "bsky-post/1.1 (+https://bsky.app)"

MENTION_REGEX = re.compile(
    rb"(?:^|\W)(@([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)"
)
URL_REGEX = re.compile(rb"(?:^|[^\w/])((?:https?://)[^\s<>'\"`]+)", re.IGNORECASE)
HASHTAG_REGEX = re.compile(rb"(?:^|\W)(#[\w\-]+)")

IMAGE_FORMAT_MIMETYPES = {
    "PNG": "image/png",
    "JPEG": "image/jpeg",
    "WEBP": "image/webp",
    "GIF": "image/gif",
}

EXTENSION_MIMETYPES = {
    "png": "image/png",
    "jpeg": "image/jpeg",
    "jpg": "image/jpeg",
    "webp": "image/webp",
    "gif": "image/gif",
}

_SESSION = requests.Session()
_SESSION.headers["User-Agent"] = USER_AGENT
_SESSION.trust_env = False


def _api_url(pds_url: str, method: str) -> str:
    return f"{pds_url.rstrip('/')}/xrpc/{method}"


def _parse_url(url: str, *, schemes: tuple[str, ...]) -> ParseResult:
    if not url or any(ch.isspace() for ch in url):
        raise ValueError(f"Invalid URL: {url!r}")
    parsed = urlparse(url)
    if parsed.scheme.lower() not in schemes:
        joined = ", ".join(schemes)
        raise ValueError(f"URL must use one of these schemes ({joined}): {url!r}")
    if not parsed.hostname:
        raise ValueError(f"URL has no host: {url!r}")
    if parsed.username or parsed.password:
        raise ValueError(f"URL must not include credentials: {url!r}")
    try:
        parsed.port
    except ValueError as exc:
        raise ValueError(f"URL has an invalid port: {url!r}") from exc
    return parsed


def normalize_pds_url(pds_url: str, *, allow_insecure: bool = False) -> str:
    parsed = _parse_url(pds_url.strip(), schemes=("http", "https"))
    if parsed.path not in ("", "/") or parsed.params or parsed.query or parsed.fragment:
        raise ValueError(
            "PDS URL must be only a scheme and host, without path/query/fragment"
        )
    if parsed.scheme.lower() != "https" and not allow_insecure:
        raise ValueError(
            "Refusing to send credentials to a non-HTTPS PDS URL "
            "(use --allow-insecure-pds only for local testing)"
        )
    return urlunparse((parsed.scheme.lower(), parsed.netloc, "", "", "", "")).rstrip(
        "/"
    )


def _assert_public_url(url: str) -> None:
    """Reject non-HTTP(S) URLs or URLs that resolve to a non-public address.

    Why: mitigates SSRF when we fetch attacker-supplied embed URLs. A DNS-rebinding
    attacker can still flip the host between this check and the actual connect, so
    this is a best-effort guard, not a guarantee.
    """
    parsed = _parse_url(url, schemes=("http", "https"))
    try:
        infos = socket.getaddrinfo(parsed.hostname, None)
    except socket.gaierror as exc:
        raise ValueError(f"Could not resolve {parsed.hostname!r}: {exc}") from exc
    for info in infos:
        ip = ipaddress.ip_address(info[4][0])
        ip = getattr(ip, "ipv4_mapped", None) or ip
        if not ip.is_global:
            raise ValueError(
                f"Refusing to fetch non-public address {ip} for host {parsed.hostname!r}"
            )


def _safe_download(url: str, max_bytes: int, timeout: int = 10) -> bytes:
    """Fetch a URL with per-hop SSRF checks and a hard response-size cap."""
    visited: set[str] = set()
    current = url
    for _ in range(MAX_REDIRECTS + 1):
        if current in visited:
            raise ValueError(f"Redirect loop detected at {current!r}")
        visited.add(current)
        _assert_public_url(current)
        resp = _SESSION.get(
            current,
            timeout=timeout,
            stream=True,
            allow_redirects=False,
        )
        try:
            if resp.is_redirect:
                location = resp.headers.get("Location")
                if not location:
                    raise ValueError(f"Redirect from {current!r} missing Location header")
                current = urljoin(current, location)
                continue
            resp.raise_for_status()
            declared = resp.headers.get("Content-Length")
            if declared is not None and declared.isdigit() and int(declared) > max_bytes:
                raise ValueError(
                    f"Remote response declares {declared} bytes, above {max_bytes} limit"
                )
            chunks: list[bytes] = []
            total = 0
            for chunk in resp.iter_content(64 * 1024):
                if not chunk:
                    continue
                total += len(chunk)
                if total > max_bytes:
                    raise ValueError(f"Remote response exceeds {max_bytes} bytes")
                chunks.append(chunk)
            return b"".join(chunks)
        finally:
            resp.close()
    raise ValueError(f"Too many redirects (> {MAX_REDIRECTS}) following {url!r}")


def _json_object(resp: requests.Response, context: str) -> Dict[str, Any]:
    try:
        data = resp.json()
    except ValueError as exc:
        raise ValueError(f"{context} returned a non-JSON response") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{context} returned an unexpected JSON shape")
    return data


def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    resp = _SESSION.post(
        _api_url(pds_url, "com.atproto.server.createSession"),
        json={"identifier": handle, "password": password},
        timeout=30,
    )
    resp.raise_for_status()
    data = _json_object(resp, "createSession")
    if not isinstance(data.get("accessJwt"), str) or not isinstance(data.get("did"), str):
        raise ValueError("createSession response is missing accessJwt or did")
    return data


def parse_spans(
    text: str,
    regex: "re.Pattern[bytes]",
    key: str,
    transform: Callable[[re.Match], str] = lambda m: m.group(1).decode("UTF-8"),
) -> List[Dict]:
    text_bytes = text.encode("UTF-8")
    return [
        {"start": m.start(1), "end": m.end(1), key: transform(m)}
        for m in regex.finditer(text_bytes)
    ]


def parse_mentions(text: str) -> List[Dict]:
    return parse_spans(
        text,
        MENTION_REGEX,
        "handle",
        lambda m: m.group(1)[1:].decode("UTF-8"),
    )


def _trim_url_match(url_bytes: bytes) -> bytes:
    paired = {b")": b"(", b"]": b"[", b"}": b"{"}
    changed = True
    while changed:
        changed = False
        while url_bytes and url_bytes[-1:] in b".,;:!?":
            url_bytes = url_bytes[:-1]
            changed = True
        while url_bytes and url_bytes[-1:] in paired:
            close = url_bytes[-1:]
            open_ = paired[close]
            if url_bytes.count(close) <= url_bytes.count(open_):
                break
            url_bytes = url_bytes[:-1]
            changed = True
    return url_bytes


def parse_urls(text: str) -> List[Dict]:
    text_bytes = text.encode("UTF-8")
    spans: List[Dict] = []
    for match in URL_REGEX.finditer(text_bytes):
        url_bytes = _trim_url_match(match.group(1))
        if not url_bytes:
            continue
        start = match.start(1)
        end = start + len(url_bytes)
        url = url_bytes.decode("UTF-8")
        parsed = urlparse(url)
        if parsed.scheme.lower() in ("http", "https") and parsed.netloc:
            spans.append({"start": start, "end": end, "url": url})
    return spans


def parse_hashtags(text: str) -> List[Dict]:
    return parse_spans(
        text,
        HASHTAG_REGEX,
        "tag",
        lambda m: m.group(1)[1:].decode("UTF-8"),
    )


def make_facet(match: Dict, feature: Dict) -> Dict:
    return {
        "index": {"byteStart": match["start"], "byteEnd": match["end"]},
        "features": [feature],
    }


def _resolve_handle(pds_url: str, handle: str) -> Optional[str]:
    try:
        resp = _SESSION.get(
            _api_url(pds_url, "com.atproto.identity.resolveHandle"),
            params={"handle": handle},
            timeout=10,
        )
    except requests.RequestException:
        return None
    if resp.status_code == 400:
        return None
    try:
        resp.raise_for_status()
        data = resp.json()
    except (requests.RequestException, ValueError):
        return None
    return data.get("did") if isinstance(data, dict) else None


def _overlaps_existing(span: Dict, occupied: List[tuple[int, int]]) -> bool:
    start = span["start"]
    end = span["end"]
    return any(
        start < occupied_end and occupied_start < end
        for occupied_start, occupied_end in occupied
    )


def _reserve_span(span: Dict, occupied: List[tuple[int, int]]) -> bool:
    if _overlaps_existing(span, occupied):
        return False
    occupied.append((span["start"], span["end"]))
    return True


def parse_facets(pds_url: str, text: str) -> List[Dict]:
    facets: List[Dict] = []
    occupied: List[tuple[int, int]] = []
    for u in parse_urls(text):
        if _reserve_span(u, occupied):
            facets.append(
                make_facet(
                    u,
                    {"$type": "app.bsky.richtext.facet#link", "uri": u["url"]},
                )
            )
    for m in parse_mentions(text):
        if _overlaps_existing(m, occupied):
            continue
        did = _resolve_handle(pds_url, m["handle"])
        if did and _reserve_span(m, occupied):
            facets.append(
                make_facet(
                    m,
                    {"$type": "app.bsky.richtext.facet#mention", "did": did},
                )
            )
    facets.extend(
        make_facet(h, {"$type": "app.bsky.richtext.facet#tag", "tag": h["tag"]})
        for h in parse_hashtags(text)
        if _reserve_span(h, occupied)
    )
    return sorted(facets, key=lambda facet: facet["index"]["byteStart"])


def parse_uri(uri: str) -> Dict:
    parsed = urlparse(uri)
    if parsed.scheme == "at":
        if parsed.query or parsed.fragment or not parsed.netloc:
            raise ValueError(f"Invalid AT URI format: {uri}")
        parts = [part for part in parsed.path.split("/") if part]
        if len(parts) != 2:
            raise ValueError(f"Invalid AT URI format: {uri}")
        repo, collection, rkey = parsed.netloc, parts[0], parts[1]
        return {"repo": repo, "collection": collection, "rkey": rkey}
    if parsed.scheme == "https" and parsed.hostname == "bsky.app":
        parts = [part for part in parsed.path.split("/") if part]
        if len(parts) != 4 or parts[0] != "profile":
            raise ValueError(f"Invalid Bluesky URL format: {uri}")
        repo, collection, rkey = parts[1], parts[2], parts[3]
        collection_map = {
            "post": "app.bsky.feed.post",
            "lists": "app.bsky.graph.list",
            "feed": "app.bsky.feed.generator",
        }
        return {
            "repo": repo,
            "collection": collection_map.get(collection, collection),
            "rkey": rkey,
        }
    raise ValueError(f"Unhandled URI format: {uri}")


def get_record(pds_url: str, uri: str) -> Dict:
    resp = _SESSION.get(
        _api_url(pds_url, "com.atproto.repo.getRecord"),
        params=parse_uri(uri),
        timeout=10,
    )
    resp.raise_for_status()
    data = _json_object(resp, "getRecord")
    if not isinstance(data.get("uri"), str) or not isinstance(data.get("cid"), str):
        raise ValueError("getRecord response is missing uri or cid")
    return data


def record_ref(record: Dict) -> Dict:
    uri = record.get("uri")
    cid = record.get("cid")
    if not isinstance(uri, str) or not isinstance(cid, str):
        raise ValueError("Record is missing uri or cid")
    return {"uri": uri, "cid": cid}


def get_reply_refs(pds_url: str, parent_uri: str) -> Dict:
    parent = get_record(pds_url, parent_uri)
    value = parent.get("value")
    if not isinstance(value, dict):
        raise ValueError("Parent record response is missing value")
    parent_reply = value.get("reply")
    if parent_reply is None:
        root = parent
    else:
        if not isinstance(parent_reply, dict):
            raise ValueError("Parent reply reference has an unexpected shape")
        root_ref = parent_reply.get("root")
        if not isinstance(root_ref, dict) or not isinstance(root_ref.get("uri"), str):
            raise ValueError("Parent reply reference is missing root.uri")
        root = get_record(pds_url, root_ref["uri"])
    return {"root": record_ref(root), "parent": record_ref(parent)}


def get_mimetype(filename: str) -> str:
    # Strip URL query/fragment so og:image URLs like "photo.jpg?sig=…" still match.
    path_part = urlparse(filename).path or filename
    suffix = Path(path_part).suffix.lower().lstrip(".")
    return EXTENSION_MIMETYPES.get(suffix, "application/octet-stream")


def inspect_image(img_bytes: bytes, source: str) -> Dict[str, Any]:
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(img_bytes)) as img:
                width, height = img.size
                image_format = img.format
                if width <= 0 or height <= 0:
                    raise ValueError(f"Image has invalid dimensions: {width}x{height}")
                if width > MAX_IMAGE_DIMENSION or height > MAX_IMAGE_DIMENSION:
                    raise ValueError(
                        f"Image dimensions exceed {MAX_IMAGE_DIMENSION}px limit: "
                        f"{width}x{height}"
                    )
                if width * height > MAX_IMAGE_PIXELS:
                    raise ValueError(
                        f"Image has too many pixels ({width * height:,}; "
                        f"limit is {MAX_IMAGE_PIXELS:,})"
                    )
                img.verify()
    except (
        UnidentifiedImageError,
        OSError,
        Image.DecompressionBombError,
        Image.DecompressionBombWarning,
    ) as exc:
        raise ValueError(f"Invalid image {source!r}: {exc}") from exc

    mimetype = IMAGE_FORMAT_MIMETYPES.get(str(image_format))
    if mimetype is None:
        raise ValueError(f"Unsupported image format for {source!r}: {image_format!r}")
    return {"width": width, "height": height, "mimetype": mimetype}


def upload_file(
    pds_url: str,
    access_token: str,
    filename: str,
    img_bytes: bytes,
    mimetype: Optional[str] = None,
) -> Dict:
    content_type = mimetype or get_mimetype(filename)
    resp = _SESSION.post(
        _api_url(pds_url, "com.atproto.repo.uploadBlob"),
        headers={
            "Content-Type": content_type,
            "Authorization": "Bearer " + access_token,
        },
        data=img_bytes,
        timeout=60,
    )
    resp.raise_for_status()
    data = _json_object(resp, "uploadBlob")
    blob = data.get("blob")
    if not isinstance(blob, dict):
        raise ValueError("uploadBlob response is missing blob")
    return blob


def upload_images(
    pds_url: str,
    access_token: str,
    image_paths: List[str],
    alt_texts: Optional[List[str]] = None,
) -> Dict:
    images = []
    for i, ip in enumerate(image_paths):
        path = Path(ip)
        if not path.is_file():
            raise ValueError(f"Image path is not a regular file: {ip}")
        with path.open("rb") as image_file:
            img_bytes = image_file.read(MAX_IMAGE_SIZE_BYTES + 1)
        if len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
            raise ValueError(
                f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum."
            )
        image_info = inspect_image(img_bytes, ip)
        blob = upload_file(
            pds_url,
            access_token,
            ip,
            img_bytes,
            image_info["mimetype"],
        )
        alt = alt_texts[i] if alt_texts and i < len(alt_texts) else ""
        images.append(
            {
                "alt": alt,
                "image": blob,
                "aspectRatio": {
                    "width": image_info["width"],
                    "height": image_info["height"],
                },
            }
        )
    return {"$type": "app.bsky.embed.images", "images": images}


def _trim_card_text(value: Any, limit: int) -> str:
    if not isinstance(value, str):
        return ""
    return value.strip()[:limit]


def _meta_content(soup: BeautifulSoup, property_name: str) -> str:
    tag = soup.find("meta", property=property_name) or soup.find(
        "meta",
        attrs={"name": property_name},
    )
    content = tag.get("content") if tag else None
    return content if isinstance(content, str) else ""


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    card = {"uri": url, "title": "", "description": ""}
    html_bytes = _safe_download(url, MAX_EMBED_HTML_BYTES)
    soup = BeautifulSoup(html_bytes, "html.parser")

    card["title"] = _trim_card_text(
        _meta_content(soup, "og:title") or (soup.title.string if soup.title else ""),
        MAX_EXTERNAL_TITLE_CHARS,
    )
    card["description"] = _trim_card_text(
        _meta_content(soup, "og:description") or _meta_content(soup, "description"),
        MAX_EXTERNAL_DESCRIPTION_CHARS,
    )

    if img_url := _meta_content(soup, "og:image"):
        img_url = img_url if "://" in img_url else urljoin(url, img_url)
        try:
            img_bytes = _safe_download(img_url, MAX_EMBED_IMAGE_BYTES)
            image_info = inspect_image(img_bytes, img_url)
            card["thumb"] = upload_file(
                pds_url,
                access_token,
                img_url,
                img_bytes,
                image_info["mimetype"],
            )
        except (requests.RequestException, ValueError) as exc:
            print(
                f"warning: could not embed og:image {img_url!r}: {exc}",
                file=sys.stderr,
            )

    return {"$type": "app.bsky.embed.external", "external": card}


def get_embed_ref(pds_url: str, ref_uri: str) -> Dict:
    return {
        "$type": "app.bsky.embed.record",
        "record": record_ref(get_record(pds_url, ref_uri)),
    }


def create_post(args: argparse.Namespace) -> None:
    session = bsky_login_session(args.pds_url, args.handle, args.password)
    access_token = session["accessJwt"]
    now = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    post: Dict = {"$type": "app.bsky.feed.post", "text": args.text, "createdAt": now}
    if args.lang:
        post["langs"] = args.lang
    if args.text and (facets := parse_facets(args.pds_url, args.text)):
        post["facets"] = facets
    if args.reply_to:
        post["reply"] = get_reply_refs(args.pds_url, args.reply_to)
    embed = None
    if args.image:
        embed = upload_images(args.pds_url, access_token, args.image, args.alt_text)
    elif args.embed_url:
        embed = fetch_embed_url_card(args.pds_url, access_token, args.embed_url)
    elif args.embed_ref:
        embed = get_embed_ref(args.pds_url, args.embed_ref)
    if embed:
        post["embed"] = embed

    print("Creating post:", file=sys.stderr)
    print(json.dumps(post, indent=2), file=sys.stderr)

    resp = _SESSION.post(
        _api_url(args.pds_url, "com.atproto.repo.createRecord"),
        headers={"Authorization": "Bearer " + access_token},
        json={
            "repo": session["did"],
            "collection": "app.bsky.feed.post",
            "record": post,
        },
        timeout=30,
    )

    try:
        resp_body = resp.json()
    except ValueError:
        resp_body = {"raw": resp.text}
    if not resp.ok:
        print("createRecord error response:", file=sys.stderr)
        print(json.dumps(resp_body, indent=2), file=sys.stderr)
    resp.raise_for_status()

    print(json.dumps(resp_body, indent=2))


def exit_error(*lines: str) -> None:
    raise SystemExit("\n".join(lines))


def test_parse_mentions():
    assert parse_mentions("prefix @handle.example.com @handle.com suffix") == [
        {"start": 7, "end": 26, "handle": "handle.example.com"},
        {"start": 27, "end": 38, "handle": "handle.com"},
    ]
    assert parse_mentions("handle.example.com") == []
    assert parse_mentions("@bare") == []
    assert parse_mentions("💩💩💩 @handle.example.com") == [
        {"start": 13, "end": 32, "handle": "handle.example.com"}
    ]
    assert parse_mentions("email@example.com") == []
    assert parse_mentions("cc:@example.com") == [
        {"start": 3, "end": 15, "handle": "example.com"}
    ]


def test_parse_urls():
    assert parse_urls(
        "prefix https://example.com/index.html http://bsky.app suffix"
    ) == [
        {"start": 7, "end": 37, "url": "https://example.com/index.html"},
        {"start": 38, "end": 53, "url": "http://bsky.app"},
    ]
    assert parse_urls("example.com") == []
    assert parse_urls("💩💩💩 http://bsky.app") == [
        {"start": 13, "end": 28, "url": "http://bsky.app"}
    ]
    assert parse_urls("runonhttp://blah.comcontinuesafter") == []
    assert parse_urls("ref [https://bsky.app]") == [
        {"start": 5, "end": 21, "url": "https://bsky.app"}
    ]
    assert parse_urls("ref (https://bsky.app/)") == [
        {"start": 5, "end": 22, "url": "https://bsky.app/"}
    ]
    assert parse_urls("ends https://bsky.app. what else?") == [
        {"start": 5, "end": 21, "url": "https://bsky.app"}
    ]
    assert parse_urls("new https://example.technology/path?q=1.") == [
        {"start": 4, "end": 39, "url": "https://example.technology/path?q=1"}
    ]
    assert parse_urls("ref (https://example.com/a_(b))") == [
        {"start": 5, "end": 30, "url": "https://example.com/a_(b)"}
    ]


def test_parse_hashtags():
    assert parse_hashtags("prefix #example #test123 suffix") == [
        {"start": 7, "end": 15, "tag": "example"},
        {"start": 16, "end": 24, "tag": "test123"},
    ]
    assert parse_hashtags("#example") == [
        {"start": 0, "end": 8, "tag": "example"}
    ]
    assert parse_hashtags("nohashtag") == []
    assert parse_hashtags("💩💩💩 #emoji") == [
        {"start": 13, "end": 19, "tag": "emoji"}
    ]
    assert parse_hashtags("#123 #abc-def") == [
        {"start": 0, "end": 4, "tag": "123"},
        {"start": 5, "end": 13, "tag": "abc-def"},
    ]


def test_parse_facets_skips_overlaps():
    original_resolve = globals()["_resolve_handle"]
    calls = []

    def fake_resolve(_pds_url: str, handle: str) -> str:
        calls.append(handle)
        return "did:plc:test"

    try:
        globals()["_resolve_handle"] = fake_resolve
        facets = parse_facets(
            "https://pds.example",
            "see https://example.com/@alice.test/#topic and @bob.test #ok",
        )
    finally:
        globals()["_resolve_handle"] = original_resolve

    assert calls == ["bob.test"]
    features = [facet["features"][0]["$type"] for facet in facets]
    assert features == [
        "app.bsky.richtext.facet#link",
        "app.bsky.richtext.facet#mention",
        "app.bsky.richtext.facet#tag",
    ]


def test_parse_uri():
    assert parse_uri("at://did:plc:abc/app.bsky.feed.post/123") == {
        "repo": "did:plc:abc",
        "collection": "app.bsky.feed.post",
        "rkey": "123",
    }
    assert parse_uri("https://bsky.app/profile/example.com/post/abc?x=1") == {
        "repo": "example.com",
        "collection": "app.bsky.feed.post",
        "rkey": "abc",
    }
    try:
        parse_uri("https://example.com/profile/example.com/post/abc")
    except ValueError:
        pass
    else:
        raise AssertionError("expected non-bsky URL to fail")


def test_normalize_pds_url():
    assert normalize_pds_url("https://bsky.social/") == "https://bsky.social"
    try:
        normalize_pds_url("http://bsky.social")
    except ValueError:
        pass
    else:
        raise AssertionError("expected insecure PDS URL to fail")
    assert (
        normalize_pds_url("http://localhost:2583", allow_insecure=True)
        == "http://localhost:2583"
    )


def test_url_security_checks():
    for url in ("http://127.0.0.1/", "http://[::1]/"):
        try:
            _assert_public_url(url)
        except ValueError:
            pass
        else:
            raise AssertionError(f"expected local URL to fail: {url}")
    try:
        _parse_url("https://user:pass@example.com/", schemes=("https",))
    except ValueError:
        pass
    else:
        raise AssertionError("expected URL credentials to fail")


def test_inspect_image():
    buf = io.BytesIO()
    Image.new("RGB", (2, 3)).save(buf, format="PNG")
    assert inspect_image(buf.getvalue(), "test.png") == {
        "width": 2,
        "height": 3,
        "mimetype": "image/png",
    }
    try:
        inspect_image(b"not an image", "broken.png")
    except ValueError:
        pass
    else:
        raise AssertionError("expected invalid image to fail")


def run_self_tests() -> None:
    for test in (
        test_parse_mentions,
        test_parse_urls,
        test_parse_hashtags,
        test_parse_facets_skips_overlaps,
        test_parse_uri,
        test_normalize_pds_url,
        test_url_security_checks,
        test_inspect_image,
    ):
        test()


def validate_args(args: argparse.Namespace) -> None:
    args.pds_url = normalize_pds_url(
        args.pds_url,
        allow_insecure=args.allow_insecure_pds,
    )

    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        raise ValueError(f"At most {MAX_IMAGES_PER_POST} images per post.")

    if args.alt_text and not args.image:
        raise ValueError("--alt-text requires --image.")

    if args.alt_text and args.image and len(args.alt_text) != len(args.image):
        raise ValueError("--alt-text count must match --image count.")

    embed_sources = sum(
        bool(source)
        for source in (args.image, args.embed_url, args.embed_ref)
    )
    if embed_sources > 1:
        raise ValueError(
            "Use only one embed source: --image, --embed-url, or --embed-ref."
        )

    if not args.text and embed_sources == 0:
        raise ValueError("Post text or an embed is required.")

    if args.embed_url:
        _parse_url(args.embed_url, schemes=("http", "https"))
    if args.reply_to:
        parse_uri(args.reply_to)
    if args.embed_ref:
        parse_uri(args.embed_ref)

    # Bluesky's real cap is graphemes (plus a byte cap); Python's len() counts
    # codepoints, so emoji clusters may over-count. Kept as a client-side sanity
    # check — the server enforces the true limit.
    if args.text and len(args.text) > MAX_POST_GRAPHEMES:
        raise ValueError(
            f"Post text exceeds {MAX_POST_GRAPHEMES}-codepoint client-side limit "
            f"(got {len(args.text)})."
        )


def main():
    parser = argparse.ArgumentParser(
        description="Bluesky post creation example script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Examples:\n  %(prog)s "Hello, Bluesky!"\n  %(prog)s "Check this out!" --image photo.jpg --alt-text "A photo"\n  %(prog)s "Replying to a post" --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"',
    )
    parser.add_argument("--pds-url", default=os.environ.get("ATP_PDS_HOST", DEFAULT_PDS_URL),
                        help=f"PDS URL (default: {DEFAULT_PDS_URL} or ATP_PDS_HOST env var)")
    parser.add_argument("--allow-insecure-pds", action="store_true",
                        help="Allow non-HTTPS PDS URLs for local testing only")
    parser.add_argument("--handle", default=os.environ.get("ATP_AUTH_HANDLE"),
                        help="Bluesky handle (or ATP_AUTH_HANDLE env var)")
    parser.add_argument("--password", default=None,
                        help="Bluesky app password (prefer ATP_AUTH_PASSWORD env var; "
                             "values passed on the command line are visible to other local users via `ps`)")
    parser.add_argument("text", nargs="?", default="", help="Post text content")
    parser.add_argument("--image", action="append", metavar="PATH",
                        help="Image file to attach (can be specified up to 4 times)")
    parser.add_argument("--alt-text", action="append", metavar="TEXT",
                        help="Alt text for images (one per --image, in order)")
    parser.add_argument("--lang", action="append", metavar="CODE",
                        help="Language code (e.g., 'en', can be specified multiple times)")
    parser.add_argument("--reply-to", metavar="URI", help="URI of post to reply to")
    parser.add_argument("--embed-url", metavar="URL", help="URL to embed as a link card")
    parser.add_argument("--embed-ref", metavar="URI", help="URI of post/record to embed (quote post)")
    parser.add_argument("--self-test", "--test", action="store_true", dest="self_test",
                        help="Run local parser/validation tests and exit")

    args = parser.parse_args()

    if args.self_test:
        run_self_tests()
        print("self-tests passed")
        return

    if args.password:
        print(
            "warning: passing --password on the command line exposes it to other local "
            "users via `ps`; prefer the ATP_AUTH_PASSWORD environment variable.",
            file=sys.stderr,
        )
    else:
        args.password = os.environ.get("ATP_AUTH_PASSWORD")

    if not args.handle or not args.password:
        exit_error(
            "Error: Both handle and password are required.",
            "Set ATP_AUTH_HANDLE and ATP_AUTH_PASSWORD environment variables,",
            "or use --handle and --password arguments.",
        )

    try:
        validate_args(args)
    except ValueError as e:
        exit_error(f"Error: {e}")

    try:
        create_post(args)
    except requests.RequestException as e:
        exit_error(f"Error: API request failed: {e}")
    except ValueError as e:
        exit_error(f"Error: {e}")


if __name__ == "__main__":
    main()
