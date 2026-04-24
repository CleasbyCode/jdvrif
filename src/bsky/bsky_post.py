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
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable, Dict, List, Optional
from urllib.parse import urljoin, urlparse

import requests
from bs4 import BeautifulSoup
from PIL import Image


DEFAULT_PDS_URL = "https://bsky.social"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 1_000_000
MAX_POST_GRAPHEMES = 300
MAX_EMBED_HTML_BYTES = 2_000_000
MAX_EMBED_IMAGE_BYTES = 1_000_000
MAX_REDIRECTS = 3
USER_AGENT = "bsky-post/1.1 (+https://bsky.app)"

MENTION_REGEX = re.compile(
    rb"(?:^|\W)(@([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)"
)
URL_REGEX = re.compile(
    rb"(?:^|\W)(https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*[-a-zA-Z0-9@%_\+~#//=])?)"
)
HASHTAG_REGEX = re.compile(rb"(?:^|\W)(#[\w\-]+)")

MIMETYPES = {
    "png": "image/png",
    "jpeg": "image/jpeg",
    "jpg": "image/jpeg",
    "webp": "image/webp",
    "gif": "image/gif",
}

_SESSION = requests.Session()
_SESSION.headers["User-Agent"] = USER_AGENT


def _assert_public_url(url: str) -> None:
    """Reject non-HTTP(S) URLs or URLs that resolve to a non-public address.

    Why: mitigates SSRF when we fetch attacker-supplied embed URLs. A DNS-rebinding
    attacker can still flip the host between this check and the actual connect, so
    this is a best-effort guard, not a guarantee.
    """
    parsed = urlparse(url)
    if parsed.scheme not in ("http", "https"):
        raise ValueError(f"Refusing to fetch non-HTTP(S) URL: {url!r}")
    if not parsed.hostname:
        raise ValueError(f"URL has no host: {url!r}")
    try:
        infos = socket.getaddrinfo(parsed.hostname, None)
    except socket.gaierror as exc:
        raise ValueError(f"Could not resolve {parsed.hostname!r}: {exc}") from exc
    for info in infos:
        ip = ipaddress.ip_address(info[4][0])
        if (
            ip.is_private
            or ip.is_loopback
            or ip.is_link_local
            or ip.is_reserved
            or ip.is_multicast
            or ip.is_unspecified
        ):
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
        resp = _SESSION.get(current, timeout=timeout, stream=True, allow_redirects=False)
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
                total += len(chunk)
                if total > max_bytes:
                    raise ValueError(f"Remote response exceeds {max_bytes} bytes")
                chunks.append(chunk)
            return b"".join(chunks)
        finally:
            resp.close()
    raise ValueError(f"Too many redirects (> {MAX_REDIRECTS}) following {url!r}")


def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    resp = _SESSION.post(
        pds_url + "/xrpc/com.atproto.server.createSession",
        json={"identifier": handle, "password": password},
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()


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
    return parse_spans(text, MENTION_REGEX, "handle", lambda m: m.group(1)[1:].decode("UTF-8"))


def parse_urls(text: str) -> List[Dict]:
    return parse_spans(text, URL_REGEX, "url")


def parse_hashtags(text: str) -> List[Dict]:
    return parse_spans(text, HASHTAG_REGEX, "tag", lambda m: m.group(1)[1:].decode("UTF-8"))


def make_facet(match: Dict, feature: Dict) -> Dict:
    return {
        "index": {"byteStart": match["start"], "byteEnd": match["end"]},
        "features": [feature],
    }


def _resolve_handle(pds_url: str, handle: str) -> Optional[str]:
    try:
        resp = _SESSION.get(
            pds_url + "/xrpc/com.atproto.identity.resolveHandle",
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


def parse_facets(pds_url: str, text: str) -> List[Dict]:
    facets: List[Dict] = []
    for m in parse_mentions(text):
        did = _resolve_handle(pds_url, m["handle"])
        if did:
            facets.append(
                make_facet(m, {"$type": "app.bsky.richtext.facet#mention", "did": did})
            )
    facets.extend(
        make_facet(u, {"$type": "app.bsky.richtext.facet#link", "uri": u["url"]})
        for u in parse_urls(text)
    )
    facets.extend(
        make_facet(h, {"$type": "app.bsky.richtext.facet#tag", "tag": h["tag"]})
        for h in parse_hashtags(text)
    )
    return facets


def parse_uri(uri: str) -> Dict:
    if uri.startswith("at://"):
        parts = uri.split("/")
        if len(parts) < 5:
            raise ValueError(f"Invalid AT URI format: {uri}")
        repo, collection, rkey = parts[2:5]
        return {"repo": repo, "collection": collection, "rkey": rkey}
    if uri.startswith("https://bsky.app/"):
        parts = uri.split("/")
        if len(parts) < 7:
            raise ValueError(f"Invalid Bluesky URL format: {uri}")
        repo, collection, rkey = parts[4:7]
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
        pds_url + "/xrpc/com.atproto.repo.getRecord",
        params=parse_uri(uri),
        timeout=10,
    )
    resp.raise_for_status()
    return resp.json()


def record_ref(record: Dict) -> Dict:
    return {"uri": record["uri"], "cid": record["cid"]}


def get_reply_refs(pds_url: str, parent_uri: str) -> Dict:
    parent = get_record(pds_url, parent_uri)
    parent_reply = parent["value"].get("reply")
    root = parent if parent_reply is None else get_record(pds_url, parent_reply["root"]["uri"])
    return {"root": record_ref(root), "parent": record_ref(parent)}


def get_mimetype(filename: str) -> str:
    # Strip URL query/fragment so og:image URLs like "photo.jpg?sig=…" still match.
    path_part = urlparse(filename).path or filename
    suffix = Path(path_part).suffix.lower().lstrip(".")
    return MIMETYPES.get(suffix, "application/octet-stream")


def upload_file(pds_url: str, access_token: str, filename: str, img_bytes: bytes) -> Dict:
    mimetype = get_mimetype(filename)
    resp = _SESSION.post(
        pds_url + "/xrpc/com.atproto.repo.uploadBlob",
        headers={
            "Content-Type": mimetype,
            "Authorization": "Bearer " + access_token,
        },
        data=img_bytes,
        timeout=60,
    )
    resp.raise_for_status()
    return resp.json()["blob"]


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
        with Image.open(io.BytesIO(img_bytes)) as img:
            width, height = img.size
        blob = upload_file(pds_url, access_token, ip, img_bytes)
        alt = alt_texts[i] if alt_texts and i < len(alt_texts) else ""
        images.append(
            {"alt": alt, "image": blob, "aspectRatio": {"width": width, "height": height}}
        )
    return {"$type": "app.bsky.embed.images", "images": images}


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    card = {"uri": url, "title": "", "description": ""}
    html_bytes = _safe_download(url, MAX_EMBED_HTML_BYTES)
    soup = BeautifulSoup(html_bytes, "html.parser")

    for field in ("title", "description"):
        if tag := soup.find("meta", property=f"og:{field}"):
            if content := tag.get("content"):
                card[field] = content

    if image_tag := soup.find("meta", property="og:image"):
        if img_url := image_tag.get("content"):
            img_url = img_url if "://" in img_url else urljoin(url, img_url)
            try:
                img_bytes = _safe_download(img_url, MAX_EMBED_IMAGE_BYTES)
                card["thumb"] = upload_file(pds_url, access_token, img_url, img_bytes)
            except (requests.RequestException, ValueError) as exc:
                print(f"warning: could not embed og:image {img_url!r}: {exc}", file=sys.stderr)

    return {"$type": "app.bsky.embed.external", "external": card}


def get_embed_ref(pds_url: str, ref_uri: str) -> Dict:
    return {"$type": "app.bsky.embed.record", "record": record_ref(get_record(pds_url, ref_uri))}


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
    embed = (
        upload_images(args.pds_url, access_token, args.image, args.alt_text) if args.image else
        fetch_embed_url_card(args.pds_url, access_token, args.embed_url) if args.embed_url else
        get_embed_ref(args.pds_url, args.embed_ref) if args.embed_ref else None
    )
    if embed:
        post["embed"] = embed

    print("Creating post:", file=sys.stderr)
    print(json.dumps(post, indent=2), file=sys.stderr)

    resp = _SESSION.post(
        args.pds_url + "/xrpc/com.atproto.repo.createRecord",
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


def main():
    parser = argparse.ArgumentParser(
        description="Bluesky post creation example script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Examples:\n  %(prog)s "Hello, Bluesky!"\n  %(prog)s "Check this out!" --image photo.jpg --alt-text "A photo"\n  %(prog)s "Replying to a post" --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"',
    )
    parser.add_argument("--pds-url", default=os.environ.get("ATP_PDS_HOST", DEFAULT_PDS_URL),
                        help=f"PDS URL (default: {DEFAULT_PDS_URL} or ATP_PDS_HOST env var)")
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

    args = parser.parse_args()

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

    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        exit_error(f"Error: At most {MAX_IMAGES_PER_POST} images per post.")

    # Bluesky's real cap is graphemes (plus a byte cap); Python's len() counts
    # codepoints, so emoji clusters may over-count. Kept as a client-side sanity
    # check — the server enforces the true limit.
    if args.text and len(args.text) > MAX_POST_GRAPHEMES:
        exit_error(
            f"Error: Post text exceeds {MAX_POST_GRAPHEMES}-codepoint client-side limit "
            f"(got {len(args.text)})."
        )

    try:
        create_post(args)
    except requests.RequestException as e:
        exit_error(f"Error: API request failed: {e}")
    except ValueError as e:
        exit_error(f"Error: {e}")


if __name__ == "__main__":
    main()
