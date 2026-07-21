#!/usr/bin/env python3

"""Bluesky posting helper adapted from Bryan Newbold's original script:

 https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py
 https://gist.github.com/bnewbold
 https://bsky.app/profile/bnewbold.net

 Fork:
 https://gist.github.com/CleasbyCode/1eb678ca1fa1975b1c1e20aeec33637e
 
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
from bisect import bisect_left
from contextlib import contextmanager
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional
from urllib.parse import ParseResult, urljoin, urlparse, urlunparse

import requests
from requests.adapters import HTTPAdapter
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
DOWNLOAD_CHUNK_SIZE = 64 * 1024

MENTION_REGEX = re.compile(
    rb"(?:^|\W)(@([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)"
)
URL_REGEX = re.compile(rb"(?:^|[^\w/])((?:https?://)[^\s<>'\"`]+)", re.IGNORECASE)
HASHTAG_REGEX = re.compile(r"(?:^|\W)(#[\w\-]+)")
TRAILING_URL_PUNCTUATION = b".,;:!?"
URL_CLOSING_TO_OPENING = {
    ord(")"): ord("("),
    ord("]"): ord("["),
    ord("}"): ord("{"),
}
URL_BRACKET_BYTES = tuple(URL_CLOSING_TO_OPENING) + tuple(
    URL_CLOSING_TO_OPENING.values()
)

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

BSKY_APP_COLLECTIONS = {
    "post": "app.bsky.feed.post",
    "lists": "app.bsky.graph.list",
    "feed": "app.bsky.feed.generator",
}

EMBED_SOURCE_ATTRS = ("image", "embed_url", "embed_ref")

_SESSION = requests.Session()
_SESSION.headers["User-Agent"] = USER_AGENT
_SESSION.trust_env = False


def _api_url(pds_url: str, method: str) -> str:
    return f"{pds_url.rstrip('/')}/xrpc/{method}"


def _parse_url(url: str, *, schemes: tuple[str, ...]) -> ParseResult:
    if not url or any(ch.isspace() for ch in url):
        raise ValueError(f"Invalid URL: {url!r}")
    parsed = urlparse(url)
    scheme = parsed.scheme.lower()
    allowed_schemes = tuple(candidate.lower() for candidate in schemes)
    if scheme not in allowed_schemes:
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
    scheme = parsed.scheme.lower()
    if scheme != "https" and not allow_insecure:
        raise ValueError(
            "Refusing to send credentials to a non-HTTPS PDS URL "
            "(use --allow-insecure-pds only for local testing)"
        )
    return urlunparse((scheme, parsed.netloc, "", "", "", "")).rstrip("/")


def _public_url_addresses(url: str) -> tuple[ParseResult, List[str]]:
    """Resolve a URL once and return only validated public connection targets."""
    parsed = _parse_url(url, schemes=("http", "https"))
    port = (
        parsed.port
        if parsed.port is not None
        else (443 if parsed.scheme.lower() == "https" else 80)
    )
    try:
        infos = socket.getaddrinfo(
            parsed.hostname,
            port,
            type=socket.SOCK_STREAM,
        )
    except socket.gaierror as exc:
        raise ValueError(f"Could not resolve {parsed.hostname!r}: {exc}") from exc

    addresses: List[str] = []
    for info in infos:
        address = info[4][0]
        ip = ipaddress.ip_address(address)
        ip = getattr(ip, "ipv4_mapped", None) or ip
        if not ip.is_global:
            raise ValueError(
                f"Refusing to fetch non-public address {ip} for host {parsed.hostname!r}"
            )
        normalized = str(ip)
        if normalized not in addresses:
            addresses.append(normalized)
    if not addresses:
        raise ValueError(f"Could not resolve {parsed.hostname!r} to an IP address")
    return parsed, addresses


def _assert_public_url(url: str) -> None:
    """Reject non-HTTP(S) URLs or URLs resolving to any non-public address."""
    _public_url_addresses(url)


def _ascii_hostname(hostname: str) -> str:
    try:
        return str(ipaddress.ip_address(hostname))
    except ValueError:
        return hostname.encode("idna").decode("ascii")


def _original_authority(parsed: ParseResult) -> str:
    hostname = _ascii_hostname(parsed.hostname or "")
    if ":" in hostname:
        hostname = f"[{hostname}]"
    return f"{hostname}:{parsed.port}" if parsed.port is not None else hostname


def _pinned_url(parsed: ParseResult, address: str) -> str:
    """Replace the URL authority with a previously validated IP literal."""
    authority = f"[{address}]" if ":" in address else address
    if parsed.port is not None:
        authority = f"{authority}:{parsed.port}"
    return urlunparse(parsed._replace(netloc=authority))


class _PinnedHTTPSAdapter(HTTPAdapter):
    """Connect to an IP literal while authenticating the URL's original hostname."""

    def __init__(self, hostname: str):
        self._hostname = _ascii_hostname(hostname)
        super().__init__()

    def init_poolmanager(self, connections, maxsize, block=False, **pool_kwargs):
        pool_kwargs["assert_hostname"] = self._hostname
        pool_kwargs["server_hostname"] = self._hostname
        super().init_poolmanager(connections, maxsize, block, **pool_kwargs)


@contextmanager
def _open_pinned_response(
    url: str,
    timeout: int,
) -> Iterator[requests.Response]:
    """Open one response using exactly one of the URL's validated addresses."""
    parsed, addresses = _public_url_addresses(url)
    last_error: Optional[requests.RequestException] = None
    for address in addresses:
        session = requests.Session()
        session.headers.update(_SESSION.headers)
        session.trust_env = False
        if parsed.scheme.lower() == "https":
            session.mount("https://", _PinnedHTTPSAdapter(parsed.hostname or ""))
        try:
            response = session.get(
                _pinned_url(parsed, address),
                headers={"Host": _original_authority(parsed)},
                timeout=timeout,
                stream=True,
                allow_redirects=False,
            )
        except requests.RequestException as exc:
            last_error = exc
            session.close()
            continue

        try:
            yield response
        finally:
            response.close()
            session.close()
        return

    if last_error is not None:
        raise last_error
    raise ValueError(f"Could not connect to any address for {url!r}")


def _declared_content_length(resp: requests.Response) -> Optional[int]:
    declared = resp.headers.get("Content-Length", "").strip()
    if not declared:
        return None
    try:
        return int(declared)
    except ValueError:
        return None


def _read_response_body(resp: requests.Response, max_bytes: int) -> bytes:
    declared = _declared_content_length(resp)
    if declared is not None and declared > max_bytes:
        raise ValueError(
            f"Remote response declares {declared} bytes, above {max_bytes} limit"
        )

    body = bytearray()
    for chunk in resp.iter_content(DOWNLOAD_CHUNK_SIZE):
        if not chunk:
            continue
        if len(body) + len(chunk) > max_bytes:
            raise ValueError(f"Remote response exceeds {max_bytes} bytes")
        body.extend(chunk)
    return bytes(body)


def _redirect_target(resp: requests.Response, current_url: str) -> str:
    location = resp.headers.get("Location")
    if not location:
        raise ValueError(f"Redirect from {current_url!r} missing Location header")
    return urljoin(current_url, location)


def _safe_download(url: str, max_bytes: int, timeout: int = 10) -> tuple[bytes, str]:
    """Fetch a URL through pinned public addresses and return its final URL."""
    visited: set[str] = set()
    current = url
    for redirects_followed in range(MAX_REDIRECTS + 1):
        if current in visited:
            raise ValueError(f"Redirect loop detected at {current!r}")
        visited.add(current)
        with _open_pinned_response(current, timeout) as resp:
            if resp.is_redirect:
                if redirects_followed == MAX_REDIRECTS:
                    raise ValueError(
                        f"Too many redirects (> {MAX_REDIRECTS}) following {url!r}"
                    )
                current = _redirect_target(resp, current)
                continue
            resp.raise_for_status()
            return _read_response_body(resp, max_bytes), current
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
        allow_redirects=False,
    )
    if 300 <= resp.status_code < 400:
        resp.close()
        raise ValueError("Refusing redirect from credential-bearing createSession request")
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
    end = len(url_bytes)
    bracket_counts = {byte: url_bytes.count(byte) for byte in URL_BRACKET_BYTES}
    while end:
        last = url_bytes[end - 1]
        if last in TRAILING_URL_PUNCTUATION:
            end -= 1
            continue
        opening = URL_CLOSING_TO_OPENING.get(last)
        if opening is not None and bracket_counts[last] > bracket_counts[opening]:
            bracket_counts[last] -= 1
            end -= 1
            continue
        break
    return url_bytes[:end]


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
    byte_offsets = [0]
    for character in text:
        byte_offsets.append(byte_offsets[-1] + len(character.encode("UTF-8")))
    return [
        {
            "start": byte_offsets[match.start(1)],
            "end": byte_offsets[match.end(1)],
            "tag": match.group(1)[1:],
        }
        for match in HASHTAG_REGEX.finditer(text)
    ]


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


def _span_insert_index(
    start: int,
    end: int,
    occupied: List[tuple[int, int]],
) -> tuple[int, bool]:
    index = bisect_left(occupied, (start, end))
    overlaps = (
        (index > 0 and occupied[index - 1][1] > start)
        or (index < len(occupied) and end > occupied[index][0])
    )
    return index, overlaps


def _overlaps_existing(span: Dict, occupied: List[tuple[int, int]]) -> bool:
    start, end = span["start"], span["end"]
    _, overlaps = _span_insert_index(start, end, occupied)
    return overlaps


def _reserve_span(span: Dict, occupied: List[tuple[int, int]]) -> bool:
    start, end = span["start"], span["end"]
    if start >= end:
        return False
    index, overlaps = _span_insert_index(start, end, occupied)
    if overlaps:
        return False
    occupied.insert(index, (start, end))
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


def _path_parts(parsed: ParseResult) -> tuple[str, ...]:
    return tuple(part for part in parsed.path.split("/") if part)


def _parse_at_uri(parsed: ParseResult, uri: str) -> Dict:
    parts = _path_parts(parsed)
    if parsed.query or parsed.fragment or not parsed.netloc or len(parts) != 2:
        raise ValueError(f"Invalid AT URI format: {uri}")
    return {"repo": parsed.netloc, "collection": parts[0], "rkey": parts[1]}


def _parse_bsky_app_uri(parsed: ParseResult, uri: str) -> Dict:
    parts = _path_parts(parsed)
    if len(parts) != 4 or parts[0] != "profile":
        raise ValueError(f"Invalid Bluesky URL format: {uri}")
    _, repo, collection, rkey = parts
    return {
        "repo": repo,
        "collection": BSKY_APP_COLLECTIONS.get(collection, collection),
        "rkey": rkey,
    }


def parse_uri(uri: str) -> Dict:
    parsed = urlparse(uri)
    if parsed.scheme == "at":
        return _parse_at_uri(parsed, uri)
    if parsed.scheme == "https" and parsed.hostname == "bsky.app":
        return _parse_bsky_app_uri(parsed, uri)
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


def _reply_root_ref(pds_url: str, parent_reply: Dict) -> Dict:
    root_ref = parent_reply.get("root")
    if not isinstance(root_ref, dict) or not isinstance(root_ref.get("uri"), str):
        raise ValueError("Parent reply reference is missing root.uri")
    if isinstance(root_ref.get("cid"), str):
        return record_ref(root_ref)
    return record_ref(get_record(pds_url, root_ref["uri"]))


def get_reply_refs(pds_url: str, parent_uri: str) -> Dict:
    parent = get_record(pds_url, parent_uri)
    value = parent.get("value")
    if not isinstance(value, dict):
        raise ValueError("Parent record response is missing value")
    parent_reply = value.get("reply")
    if parent_reply is None:
        root = record_ref(parent)
    elif isinstance(parent_reply, dict):
        root = _reply_root_ref(pds_url, parent_reply)
    else:
        raise ValueError("Parent reply reference has an unexpected shape")
    return {"root": root, "parent": record_ref(parent)}


def get_mimetype(filename: str) -> str:
    # Strip URL query/fragment so og:image URLs like "photo.jpg?sig=…" still match.
    path_part = urlparse(filename).path or filename
    suffix = Path(path_part).suffix.lower().lstrip(".")
    return EXTENSION_MIMETYPES.get(suffix, "application/octet-stream")


def _validate_image_dimensions(width: int, height: int) -> None:
    if width <= 0 or height <= 0:
        raise ValueError(f"Image has invalid dimensions: {width}x{height}")
    if width > MAX_IMAGE_DIMENSION or height > MAX_IMAGE_DIMENSION:
        raise ValueError(
            f"Image dimensions exceed {MAX_IMAGE_DIMENSION}px limit: {width}x{height}"
        )
    pixels = width * height
    if pixels > MAX_IMAGE_PIXELS:
        raise ValueError(
            f"Image has too many pixels ({pixels:,}; limit is {MAX_IMAGE_PIXELS:,})"
        )


def inspect_image(img_bytes: bytes, source: str) -> Dict[str, Any]:
    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", Image.DecompressionBombWarning)
            with Image.open(io.BytesIO(img_bytes)) as img:
                width, height = img.size
                image_format = img.format
                _validate_image_dimensions(width, height)
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


def _read_image_file(path: Path) -> bytes:
    if not path.is_file():
        raise ValueError(f"Image path is not a regular file: {path}")
    if path.stat().st_size > MAX_IMAGE_SIZE_BYTES:
        raise ValueError(
            f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum."
        )
    with path.open("rb") as image_file:
        img_bytes = image_file.read(MAX_IMAGE_SIZE_BYTES + 1)
    if len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
        raise ValueError(
            f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum."
        )
    return img_bytes


def _image_alt_text(alt_texts: Optional[List[str]], index: int) -> str:
    return alt_texts[index] if alt_texts and index < len(alt_texts) else ""


def _image_embed_item(blob: Dict, image_info: Dict[str, Any], alt: str) -> Dict:
    return {
        "alt": alt,
        "image": blob,
        "aspectRatio": {
            "width": image_info["width"],
            "height": image_info["height"],
        },
    }


def upload_images(
    pds_url: str,
    access_token: str,
    image_paths: List[str],
    alt_texts: Optional[List[str]] = None,
) -> Dict:
    images = []
    for i, ip in enumerate(image_paths):
        path = Path(ip)
        img_bytes = _read_image_file(path)
        image_info = inspect_image(img_bytes, ip)
        blob = upload_file(
            pds_url,
            access_token,
            ip,
            img_bytes,
            image_info["mimetype"],
        )
        images.append(_image_embed_item(blob, image_info, _image_alt_text(alt_texts, i)))
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


def _page_title(soup: BeautifulSoup) -> str:
    return soup.title.string if soup.title and isinstance(soup.title.string, str) else ""


def _external_card_metadata(url: str, soup: BeautifulSoup) -> Dict[str, Any]:
    title = _meta_content(soup, "og:title") or _page_title(soup)
    description = _meta_content(soup, "og:description") or _meta_content(
        soup,
        "description",
    )
    return {
        "uri": url,
        "title": _trim_card_text(title, MAX_EXTERNAL_TITLE_CHARS),
        "description": _trim_card_text(description, MAX_EXTERNAL_DESCRIPTION_CHARS),
    }


def _absolute_url(base_url: str, candidate_url: str) -> str:
    return (
        candidate_url
        if urlparse(candidate_url).scheme
        else urljoin(base_url, candidate_url)
    )


def _attach_external_thumb(
    card: Dict[str, Any],
    pds_url: str,
    access_token: str,
    page_url: str,
    soup: BeautifulSoup,
) -> None:
    img_url = _meta_content(soup, "og:image")
    if not img_url:
        return
    img_url = _absolute_url(page_url, img_url)
    try:
        img_bytes, _ = _safe_download(img_url, MAX_EMBED_IMAGE_BYTES)
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


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    html_bytes, final_url = _safe_download(url, MAX_EMBED_HTML_BYTES)
    soup = BeautifulSoup(html_bytes, "html.parser")
    card = _external_card_metadata(url, soup)
    _attach_external_thumb(card, pds_url, access_token, final_url, soup)
    return {"$type": "app.bsky.embed.external", "external": card}


def get_embed_ref(pds_url: str, ref_uri: str) -> Dict:
    return {
        "$type": "app.bsky.embed.record",
        "record": record_ref(get_record(pds_url, ref_uri)),
    }


def _created_at_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def _build_post_record(args: argparse.Namespace) -> Dict:
    post: Dict = {
        "$type": "app.bsky.feed.post",
        "text": args.text,
        "createdAt": _created_at_now(),
    }
    if args.lang:
        post["langs"] = args.lang
    if args.text and (facets := parse_facets(args.pds_url, args.text)):
        post["facets"] = facets
    if args.reply_to:
        post["reply"] = get_reply_refs(args.pds_url, args.reply_to)
    return post


def _build_embed(args: argparse.Namespace, access_token: str) -> Optional[Dict]:
    if args.image:
        return upload_images(args.pds_url, access_token, args.image, args.alt_text)
    if args.embed_url:
        return fetch_embed_url_card(args.pds_url, access_token, args.embed_url)
    if args.embed_ref:
        return get_embed_ref(args.pds_url, args.embed_ref)
    return None


def _response_body(resp: requests.Response) -> Any:
    try:
        return resp.json()
    except ValueError:
        return {"raw": resp.text}


def create_post(args: argparse.Namespace) -> None:
    session = bsky_login_session(args.pds_url, args.handle, args.password)
    access_token = session["accessJwt"]
    post = _build_post_record(args)
    if embed := _build_embed(args, access_token):
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

    resp_body = _response_body(resp)
    if not resp.ok:
        print("createRecord error response:", file=sys.stderr)
        print(json.dumps(resp_body, indent=2), file=sys.stderr)
    resp.raise_for_status()

    print(json.dumps(resp_body, indent=2))


def exit_error(*lines: str) -> None:
    raise SystemExit("\n".join(lines))


def _check(condition: bool, message: str = "self-test check failed") -> None:
    if not condition:
        raise AssertionError(message)


def _check_equal(actual: Any, expected: Any) -> None:
    if actual != expected:
        raise AssertionError(f"self-test mismatch: {actual!r} != {expected!r}")


def test_parse_mentions():
    _check_equal(parse_mentions("prefix @handle.example.com @handle.com suffix"), [
        {"start": 7, "end": 26, "handle": "handle.example.com"},
        {"start": 27, "end": 38, "handle": "handle.com"},
    ])
    _check_equal(parse_mentions("handle.example.com"), [])
    _check_equal(parse_mentions("@bare"), [])
    _check_equal(parse_mentions("💩💩💩 @handle.example.com"), [
        {"start": 13, "end": 32, "handle": "handle.example.com"}
    ])
    _check_equal(parse_mentions("email@example.com"), [])
    _check_equal(parse_mentions("cc:@example.com"), [
        {"start": 3, "end": 15, "handle": "example.com"}
    ])


def test_parse_urls():
    _check_equal(parse_urls(
        "prefix https://example.com/index.html http://bsky.app suffix"
    ), [
        {"start": 7, "end": 37, "url": "https://example.com/index.html"},
        {"start": 38, "end": 53, "url": "http://bsky.app"},
    ])
    _check_equal(parse_urls("example.com"), [])
    _check_equal(parse_urls("💩💩💩 http://bsky.app"), [
        {"start": 13, "end": 28, "url": "http://bsky.app"}
    ])
    _check_equal(parse_urls("runonhttp://blah.comcontinuesafter"), [])
    _check_equal(parse_urls("ref [https://bsky.app]"), [
        {"start": 5, "end": 21, "url": "https://bsky.app"}
    ])
    _check_equal(parse_urls("ref (https://bsky.app/)"), [
        {"start": 5, "end": 22, "url": "https://bsky.app/"}
    ])
    _check_equal(parse_urls("ends https://bsky.app. what else?"), [
        {"start": 5, "end": 21, "url": "https://bsky.app"}
    ])
    _check_equal(parse_urls("new https://example.technology/path?q=1."), [
        {"start": 4, "end": 39, "url": "https://example.technology/path?q=1"}
    ])
    _check_equal(parse_urls("ref (https://example.com/a_(b))"), [
        {"start": 5, "end": 30, "url": "https://example.com/a_(b)"}
    ])


def test_parse_hashtags():
    _check_equal(parse_hashtags("prefix #example #test123 suffix"), [
        {"start": 7, "end": 15, "tag": "example"},
        {"start": 16, "end": 24, "tag": "test123"},
    ])
    _check_equal(parse_hashtags("#example"), [
        {"start": 0, "end": 8, "tag": "example"}
    ])
    _check_equal(parse_hashtags("nohashtag"), [])
    _check_equal(parse_hashtags("💩💩💩 #emoji"), [
        {"start": 13, "end": 19, "tag": "emoji"}
    ])
    _check_equal(parse_hashtags("#123 #abc-def"), [
        {"start": 0, "end": 4, "tag": "123"},
        {"start": 5, "end": 13, "tag": "abc-def"},
    ])
    _check_equal(parse_hashtags("#café #東京 #naïve"), [
        {"start": 0, "end": 6, "tag": "café"},
        {"start": 7, "end": 14, "tag": "東京"},
        {"start": 15, "end": 22, "tag": "naïve"},
    ])
    _check_equal(parse_hashtags("café#joined #separate"), [
        {"start": 13, "end": 22, "tag": "separate"},
    ])


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

    _check_equal(calls, ["bob.test"])
    features = [facet["features"][0]["$type"] for facet in facets]
    _check_equal(features, [
        "app.bsky.richtext.facet#link",
        "app.bsky.richtext.facet#mention",
        "app.bsky.richtext.facet#tag",
    ])


def test_span_reservations_stay_sorted():
    occupied: List[tuple[int, int]] = []
    _check(_reserve_span({"start": 10, "end": 20}, occupied))
    _check(_reserve_span({"start": 0, "end": 5}, occupied))
    _check(not _reserve_span({"start": 4, "end": 12}, occupied))
    _check(_reserve_span({"start": 20, "end": 25}, occupied))
    _check_equal(occupied, [(0, 5), (10, 20), (20, 25)])


def test_parse_uri():
    _check_equal(parse_uri("at://did:plc:abc/app.bsky.feed.post/123"), {
        "repo": "did:plc:abc",
        "collection": "app.bsky.feed.post",
        "rkey": "123",
    })
    _check_equal(parse_uri("https://bsky.app/profile/example.com/post/abc?x=1"), {
        "repo": "example.com",
        "collection": "app.bsky.feed.post",
        "rkey": "abc",
    })
    try:
        parse_uri("https://example.com/profile/example.com/post/abc")
    except ValueError:
        pass
    else:
        raise AssertionError("expected non-bsky URL to fail")


def test_normalize_pds_url():
    _check_equal(normalize_pds_url("https://bsky.social/"), "https://bsky.social")
    try:
        normalize_pds_url("http://bsky.social")
    except ValueError:
        pass
    else:
        raise AssertionError("expected insecure PDS URL to fail")
    _check_equal(
        normalize_pds_url("http://localhost:2583", allow_insecure=True),
        "http://localhost:2583",
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


def test_pinned_url_transport():
    original_getaddrinfo = socket.getaddrinfo

    def public_getaddrinfo(host, port, **kwargs):
        _check_equal(host, "example.com")
        _check_equal(port, 443)
        _check_equal(kwargs, {"type": socket.SOCK_STREAM})
        return [
            (
                socket.AF_INET,
                socket.SOCK_STREAM,
                socket.IPPROTO_TCP,
                "",
                ("8.8.8.8", port),
            ),
            (
                socket.AF_INET,
                socket.SOCK_STREAM,
                socket.IPPROTO_TCP,
                "",
                ("8.8.8.8", port),
            ),
        ]

    try:
        socket.getaddrinfo = public_getaddrinfo
        parsed, addresses = _public_url_addresses(
            "https://example.com:443/path?q=1"
        )
        _check_equal(addresses, ["8.8.8.8"])
        _check_equal(
            _pinned_url(parsed, addresses[0]),
            "https://8.8.8.8:443/path?q=1",
        )
        _check_equal(_original_authority(parsed), "example.com:443")

        def mixed_getaddrinfo(_host, port, **_kwargs):
            return [
                (
                    socket.AF_INET,
                    socket.SOCK_STREAM,
                    socket.IPPROTO_TCP,
                    "",
                    ("8.8.8.8", port),
                ),
                (
                    socket.AF_INET,
                    socket.SOCK_STREAM,
                    socket.IPPROTO_TCP,
                    "",
                    ("127.0.0.1", port),
                ),
            ]

        socket.getaddrinfo = mixed_getaddrinfo
        try:
            _public_url_addresses("https://example.com/")
        except ValueError:
            pass
        else:
            raise AssertionError("expected mixed public/private DNS answers to fail")
    finally:
        socket.getaddrinfo = original_getaddrinfo

    ipv6 = urlparse("https://example.com/path")
    _check_equal(
        _pinned_url(ipv6, "2001:4860:4860::8888"),
        "https://[2001:4860:4860::8888]/path",
    )

    adapter = _PinnedHTTPSAdapter("example.com")
    try:
        pool_options = adapter.poolmanager.connection_pool_kw
        _check_equal(pool_options["assert_hostname"], "example.com")
        _check_equal(pool_options["server_hostname"], "example.com")
    finally:
        adapter.close()


def test_open_pinned_response_uses_validated_ip():
    class FakeResponse:
        closed = False

        def close(self):
            self.closed = True

    class FakeSession:
        def __init__(self):
            self.headers = {}
            self.trust_env = True
            self.mounts = []
            self.get_calls = []
            self.closed = False

        def mount(self, prefix: str, adapter: HTTPAdapter):
            self.mounts.append((prefix, adapter))

        def get(self, url: str, **kwargs):
            self.get_calls.append((url, kwargs))
            return FakeResponse()

        def close(self):
            self.closed = True
            for _prefix, adapter in self.mounts:
                adapter.close()

    original_resolver = globals()["_public_url_addresses"]
    original_session = requests.Session
    sessions = []

    def fake_resolver(url: str):
        _check_equal(url, "https://example.com:8443/path")
        return urlparse(url), ["8.8.8.8"]

    def fake_session_factory():
        session = FakeSession()
        sessions.append(session)
        return session

    try:
        globals()["_public_url_addresses"] = fake_resolver
        requests.Session = fake_session_factory
        with _open_pinned_response(
            "https://example.com:8443/path",
            timeout=9,
        ) as response:
            _check(not response.closed)
    finally:
        globals()["_public_url_addresses"] = original_resolver
        requests.Session = original_session

    _check_equal(len(sessions), 1)
    session = sessions[0]
    _check(session.closed)
    _check(session.trust_env is False)
    _check_equal(len(session.mounts), 1)
    prefix, adapter = session.mounts[0]
    _check_equal(prefix, "https://")
    _check_equal(adapter.poolmanager.connection_pool_kw["assert_hostname"], "example.com")
    _check_equal(adapter.poolmanager.connection_pool_kw["server_hostname"], "example.com")
    _check_equal(session.get_calls, [
        (
            "https://8.8.8.8:8443/path",
            {
                "headers": {"Host": "example.com:8443"},
                "timeout": 9,
                "stream": True,
                "allow_redirects": False,
            },
        )
    ])


def test_safe_download_redirect_returns_final_url():
    class FakeResponse:
        def __init__(self, *, location: Optional[str] = None, body: bytes = b""):
            self.is_redirect = location is not None
            self.headers = {"Location": location} if location is not None else {}
            self._body = body

        def raise_for_status(self):
            return None

        def iter_content(self, _chunk_size: int):
            yield self._body

    original_open = globals()["_open_pinned_response"]
    opened = []

    @contextmanager
    def fake_open(url: str, timeout: int):
        opened.append((url, timeout))
        if url == "https://example.com/start":
            yield FakeResponse(location="/new/page")
        elif url == "https://example.com/new/page":
            yield FakeResponse(body=b"finished")
        else:
            raise AssertionError(f"unexpected URL: {url}")

    try:
        globals()["_open_pinned_response"] = fake_open
        body, final_url = _safe_download(
            "https://example.com/start",
            100,
            timeout=7,
        )
    finally:
        globals()["_open_pinned_response"] = original_open

    _check_equal(body, b"finished")
    _check_equal(final_url, "https://example.com/new/page")
    _check_equal(opened, [
        ("https://example.com/start", 7),
        ("https://example.com/new/page", 7),
    ])


def test_login_rejects_redirects():
    class FakeResponse:
        status_code = 307
        closed = False

        def close(self):
            self.closed = True

        def raise_for_status(self):
            raise AssertionError("redirect must be rejected before status handling")

    original_post = _SESSION.post
    call_options = {}

    response = FakeResponse()

    def fake_post(*_args, **kwargs):
        call_options.update(kwargs)
        return response

    try:
        _SESSION.post = fake_post
        try:
            bsky_login_session("https://pds.example", "alice", "secret")
        except ValueError:
            pass
        else:
            raise AssertionError("expected createSession redirect to fail")
    finally:
        _SESSION.post = original_post

    _check(call_options["allow_redirects"] is False)
    _check(response.closed)
    _check_equal(
        call_options["json"],
        {"identifier": "alice", "password": "secret"},
    )


def test_embed_thumbnail_uses_final_page_url():
    original_download = globals()["_safe_download"]
    original_attach = globals()["_attach_external_thumb"]
    attached = {}

    def fake_download(_url: str, _max_bytes: int):
        return (
            b'<html><head><meta property="og:image" content="../thumb.png">'
            b'<meta property="og:title" content="Title"></head></html>',
            "https://cdn.example/articles/current/page.html",
        )

    def fake_attach(_card, _pds_url, _access_token, page_url, soup):
        attached["page_url"] = page_url
        attached["image_url"] = _absolute_url(
            page_url,
            _meta_content(soup, "og:image"),
        )

    try:
        globals()["_safe_download"] = fake_download
        globals()["_attach_external_thumb"] = fake_attach
        embed = fetch_embed_url_card(
            "https://pds.example",
            "token",
            "https://example.com/original",
        )
    finally:
        globals()["_safe_download"] = original_download
        globals()["_attach_external_thumb"] = original_attach

    _check_equal(embed["external"]["uri"], "https://example.com/original")
    _check_equal(attached, {
        "page_url": "https://cdn.example/articles/current/page.html",
        "image_url": "https://cdn.example/articles/thumb.png",
    })


def test_inspect_image():
    buf = io.BytesIO()
    Image.new("RGB", (2, 3)).save(buf, format="PNG")
    _check_equal(inspect_image(buf.getvalue(), "test.png"), {
        "width": 2,
        "height": 3,
        "mimetype": "image/png",
    })
    try:
        inspect_image(b"not an image", "broken.png")
    except ValueError:
        pass
    else:
        raise AssertionError("expected invalid image to fail")


def test_read_response_body_limits():
    class FakeResponse:
        def __init__(self, chunks: List[bytes], declared: str = ""):
            self.headers = {"Content-Length": declared} if declared else {}
            self._chunks = chunks

        def iter_content(self, _chunk_size: int):
            yield from self._chunks

    _check_equal(
        _read_response_body(FakeResponse([b"ab", b"", b"cd"]), 4),
        b"abcd",
    )
    try:
        _read_response_body(FakeResponse([b"abc"], " 5 "), 4)
    except ValueError:
        pass
    else:
        raise AssertionError("expected declared oversized response to fail")
    try:
        _read_response_body(FakeResponse([b"ab", b"cde"]), 4)
    except ValueError:
        pass
    else:
        raise AssertionError("expected streamed oversized response to fail")


def test_get_reply_refs_reuses_root_ref():
    original_get_record = globals()["get_record"]
    calls = []

    def fake_get_record(_pds_url: str, uri: str) -> Dict:
        calls.append(uri)
        if uri == "at://did:plc:parent/app.bsky.feed.post/parent":
            return {
                "uri": uri,
                "cid": "parent-cid",
                "value": {
                    "reply": {
                        "root": {
                            "uri": "at://did:plc:root/app.bsky.feed.post/root",
                            "cid": "root-cid",
                        }
                    }
                },
            }
        raise AssertionError(f"unexpected get_record call: {uri}")

    try:
        globals()["get_record"] = fake_get_record
        refs = get_reply_refs(
            "https://pds.example",
            "at://did:plc:parent/app.bsky.feed.post/parent",
        )
    finally:
        globals()["get_record"] = original_get_record

    _check_equal(calls, ["at://did:plc:parent/app.bsky.feed.post/parent"])
    _check_equal(refs, {
        "root": {
            "uri": "at://did:plc:root/app.bsky.feed.post/root",
            "cid": "root-cid",
        },
        "parent": {
            "uri": "at://did:plc:parent/app.bsky.feed.post/parent",
            "cid": "parent-cid",
        },
    })


def run_self_tests() -> None:
    for test in (
        test_parse_mentions,
        test_parse_urls,
        test_parse_hashtags,
        test_parse_facets_skips_overlaps,
        test_span_reservations_stay_sorted,
        test_parse_uri,
        test_normalize_pds_url,
        test_url_security_checks,
        test_pinned_url_transport,
        test_open_pinned_response_uses_validated_ip,
        test_safe_download_redirect_returns_final_url,
        test_login_rejects_redirects,
        test_embed_thumbnail_uses_final_page_url,
        test_inspect_image,
        test_read_response_body_limits,
        test_get_reply_refs_reuses_root_ref,
    ):
        test()


def _selected_embed_sources(args: argparse.Namespace) -> List[str]:
    return [name for name in EMBED_SOURCE_ATTRS if bool(getattr(args, name))]


def _validate_image_args(args: argparse.Namespace) -> None:
    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        raise ValueError(f"At most {MAX_IMAGES_PER_POST} images per post.")
    if args.alt_text and not args.image:
        raise ValueError("--alt-text requires --image.")
    if args.alt_text and args.image and len(args.alt_text) != len(args.image):
        raise ValueError("--alt-text count must match --image count.")


def _validate_embed_args(args: argparse.Namespace) -> int:
    selected_sources = _selected_embed_sources(args)
    if len(selected_sources) > 1:
        raise ValueError(
            "Use only one embed source: --image, --embed-url, or --embed-ref."
        )
    if args.embed_url:
        _parse_url(args.embed_url, schemes=("http", "https"))
    if args.embed_ref:
        parse_uri(args.embed_ref)
    return len(selected_sources)


def _validate_text_length(text: str) -> None:
    # Bluesky's real cap is graphemes (plus a byte cap); Python's len() counts
    # codepoints, so emoji clusters may over-count. Kept as a client-side sanity
    # check; the server enforces the true limit.
    if text and len(text) > MAX_POST_GRAPHEMES:
        raise ValueError(
            f"Post text exceeds {MAX_POST_GRAPHEMES}-codepoint client-side limit "
            f"(got {len(text)})."
        )


def validate_args(args: argparse.Namespace) -> None:
    args.pds_url = normalize_pds_url(
        args.pds_url,
        allow_insecure=args.allow_insecure_pds,
    )

    _validate_image_args(args)
    embed_sources = _validate_embed_args(args)

    if not args.text and embed_sources == 0:
        raise ValueError("Post text or an embed is required.")

    if args.reply_to:
        parse_uri(args.reply_to)
    _validate_text_length(args.text)


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
