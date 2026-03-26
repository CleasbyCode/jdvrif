#!/usr/bin/env python3

"""Bluesky posting helper adapted from Bryan Newbold's original script.

https://gist.github.com/bnewbold
https://bsky.app/profile/bnewbold.net

Supports hashtags, per-image alt text, and Pillow-derived aspect ratios.
Requires: requests, beautifulsoup4, pillow
    $ pip install requests beautifulsoup4 pillow
"""

import re
import os
import sys
import json
import argparse
from pathlib import Path
from typing import Dict, List, Optional
from datetime import datetime, timezone
from urllib.parse import urljoin

import requests
from bs4 import BeautifulSoup
from PIL import Image


DEFAULT_PDS_URL = "https://bsky.social"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 1_000_000
MAX_POST_GRAPHEMES = 300
MENTION_REGEX = rb"(?:^|\W)(@([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)"
URL_REGEX = rb"(?:^|\W)(https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*[-a-zA-Z0-9@%_\+~#//=])?)"
HASHTAG_REGEX = rb"(?:^|\W)(#[\w\-]+)"


def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    resp = requests.post(
        pds_url + "/xrpc/com.atproto.server.createSession",
        json={"identifier": handle, "password": password},
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()


def parse_spans(text: str, regex: bytes, key: str, transform=lambda m: m.group(1).decode("UTF-8")) -> List[Dict]:
    text_bytes = text.encode("UTF-8")
    return [
        {"start": m.start(1), "end": m.end(1), key: transform(m)}
        for m in re.finditer(regex, text_bytes)
    ]


def parse_mentions(text: str) -> List[Dict]:
    return parse_spans(text, MENTION_REGEX, "handle", lambda m: m.group(1)[1:].decode("UTF-8"))


def parse_urls(text: str) -> List[Dict]:
    return parse_spans(text, URL_REGEX, "url")


def parse_hashtags(text: str) -> List[Dict]:
    return parse_spans(text, HASHTAG_REGEX, "tag", lambda m: m.group(1)[1:].decode("UTF-8"))


def make_facet(match: Dict, feature: Dict) -> Dict:
    return {"index": {"byteStart": match["start"], "byteEnd": match["end"]}, "features": [feature]}


def parse_facets(pds_url: str, text: str) -> List[Dict]:
    facets = []
    for m in parse_mentions(text):
        try:
            resp = requests.get(
                pds_url + "/xrpc/com.atproto.identity.resolveHandle",
                params={"handle": m["handle"]},
                timeout=10,
            )
            if resp.status_code == 400:
                continue
            resp.raise_for_status()
            facets.append(make_facet(m, {"$type": "app.bsky.richtext.facet#mention", "did": resp.json()["did"]}))
        except requests.RequestException:
            pass
    facets.extend(make_facet(u, {"$type": "app.bsky.richtext.facet#link", "uri": u["url"]}) for u in parse_urls(text))
    facets.extend(make_facet(h, {"$type": "app.bsky.richtext.facet#tag", "tag": h["tag"]}) for h in parse_hashtags(text))
    return facets


def parse_uri(uri: str) -> Dict:
    if uri.startswith("at://"):
        parts = uri.split("/")
        if len(parts) < 5:
            raise ValueError(f"Invalid AT URI format: {uri}")
        repo, collection, rkey = parts[2:5]
        return {"repo": repo, "collection": collection, "rkey": rkey}
    elif uri.startswith("https://bsky.app/"):
        parts = uri.split("/")
        if len(parts) < 7:
            raise ValueError(f"Invalid Bluesky URL format: {uri}")
        repo, collection, rkey = parts[4:7]
        collection_map = {
            "post": "app.bsky.feed.post",
            "lists": "app.bsky.graph.list",
            "feed": "app.bsky.feed.generator",
        }
        collection = collection_map.get(collection, collection)
        return {"repo": repo, "collection": collection, "rkey": rkey}
    raise ValueError(f"Unhandled URI format: {uri}")


def get_record(pds_url: str, uri: str) -> Dict:
    resp = requests.get(
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
    suffix = Path(filename).suffix.lower().lstrip(".")
    return {"png": "image/png", "jpeg": "image/jpeg", "jpg": "image/jpeg", "webp": "image/webp", "gif": "image/gif"}.get(suffix, "application/octet-stream")


def upload_file(pds_url: str, access_token: str, filename: str, img_bytes: bytes) -> Dict:
    mimetype = get_mimetype(filename)
    resp = requests.post(
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
    pds_url: str, access_token: str, image_paths: List[str], alt_texts: Optional[List[str]] = None
) -> Dict:
    images = []
    for i, ip in enumerate(image_paths):
        with open(ip, "rb") as f:
            img_bytes = f.read()

        if len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
            raise ValueError(
                f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum, got: {len(img_bytes):,}"
            )

        blob = upload_file(pds_url, access_token, ip, img_bytes)
        alt = alt_texts[i] if alt_texts and i < len(alt_texts) else ""
        with Image.open(ip) as img:
            width, height = img.size
        images.append({"alt": alt, "image": blob, "aspectRatio": {"width": width, "height": height}})

    return {"$type": "app.bsky.embed.images", "images": images}


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    card = {"uri": url, "title": "", "description": ""}
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    soup = BeautifulSoup(resp.text, "html.parser")

    for field in ("title", "description"):
        if tag := soup.find("meta", property=f"og:{field}"):
            if content := tag.get("content"):
                card[field] = content

    if image_tag := soup.find("meta", property="og:image"):
        if img_url := image_tag.get("content"):
            img_url = img_url if "://" in img_url else urljoin(url, img_url)
            try:
                img_resp = requests.get(img_url, timeout=10)
                img_resp.raise_for_status()
                card["thumb"] = upload_file(pds_url, access_token, img_url, img_resp.content)
            except requests.RequestException:
                pass

    return {"$type": "app.bsky.embed.external", "external": card}


def get_embed_ref(pds_url: str, ref_uri: str) -> Dict:
    return {"$type": "app.bsky.embed.record", "record": record_ref(get_record(pds_url, ref_uri))}


def create_post(args: argparse.Namespace) -> None:
    session = bsky_login_session(args.pds_url, args.handle, args.password)
    access_token = session["accessJwt"]
    now = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
    post = {"$type": "app.bsky.feed.post", "text": args.text, "createdAt": now}
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

    resp = requests.post(
        args.pds_url + "/xrpc/com.atproto.repo.createRecord",
        headers={"Authorization": "Bearer " + access_token},
        json={
            "repo": session["did"],
            "collection": "app.bsky.feed.post",
            "record": post,
        },
        timeout=30,
    )

    resp_body = resp.json()
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
    parser.add_argument("--password", default=os.environ.get("ATP_AUTH_PASSWORD"),
                        help="Bluesky app password (or ATP_AUTH_PASSWORD env var)")
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

    if not args.handle or not args.password:
        exit_error(
            "Error: Both handle and password are required.",
            "Set ATP_AUTH_HANDLE and ATP_AUTH_PASSWORD environment variables,",
            "or use --handle and --password arguments.",
        )

    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        exit_error(f"Error: At most {MAX_IMAGES_PER_POST} images per post.")

    if args.text and len(args.text) > MAX_POST_GRAPHEMES:
        exit_error(f"Error: Post text exceeds {MAX_POST_GRAPHEMES} character limit (got {len(args.text)}).")

    try:
        create_post(args)
    except requests.RequestException as e:
        exit_error(f"Error: API request failed: {e}")
    except ValueError as e:
        exit_error(f"Error: {e}")


if __name__ == "__main__":
    main()
