#!/usr/bin/env python3

"""
Script demonstrating how to create posts using the Bluesky API, covering most of the features and embed options.

Original script credit:
    This Python script is the work of Bryan Newbold:
        https://gist.github.com/bnewbold
        https://gist.github.com/bnewbold/ebc172c927b6a64d536bdf46bd5c2925
        https://bsky.app/profile/bnewbold.net

Note: Use of AI - Updated by Grok to support hashtags in posts. Some other improvements by Claude Opus 4.6, such as --alt-text now supports per-image alt text.

To run this Python script, you need the 'requests' and 'bs4' (BeautifulSoup) packages installed.
    pip install requests beautifulsoup4
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


# Constants
DEFAULT_PDS_URL = "https://bsky.social"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 1_000_000
MAX_POST_GRAPHEMES = 300


def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    """Authenticate with the Bluesky PDS and return session data."""
    resp = requests.post(
        pds_url + "/xrpc/com.atproto.server.createSession",
        json={"identifier": handle, "password": password},
        timeout=30,
    )
    resp.raise_for_status()
    return resp.json()


def parse_mentions(text: str) -> List[Dict]:
    """Parse @mentions from text and return byte positions and handles."""
    spans = []
    # regex based on: https://atproto.com/specs/handle#handle-identifier-syntax
    mention_regex = rb"(?:^|\W)(@([a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?\.)+[a-zA-Z]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)"
    text_bytes = text.encode("UTF-8")
    for m in re.finditer(mention_regex, text_bytes):
        spans.append(
            {
                "start": m.start(1),
                "end": m.end(1),
                "handle": m.group(1)[1:].decode("UTF-8"),
            }
        )
    return spans


def parse_urls(text: str) -> List[Dict]:
    """Parse URLs from text and return byte positions and URLs."""
    spans = []
    # partial/naive URL regex based on: https://stackoverflow.com/a/3809435
    # tweaked to disallow some trailing punctuation
    url_regex = rb"(?:^|\W)(https?:\/\/(www\.)?[-a-zA-Z0-9@:%._\+~#=]{1,256}\.[a-zA-Z0-9()]{1,6}\b([-a-zA-Z0-9()@:%_\+.~#?&//=]*[-a-zA-Z0-9@%_\+~#//=])?)"
    text_bytes = text.encode("UTF-8")
    for m in re.finditer(url_regex, text_bytes):
        spans.append(
            {
                "start": m.start(1),
                "end": m.end(1),
                "url": m.group(1).decode("UTF-8"),
            }
        )
    return spans


def parse_hashtags(text: str) -> List[Dict]:
    """Parse #hashtags from text and return byte positions and tags."""
    spans = []
    # Regex for hashtags: # followed by word characters or hyphens
    # Must start at beginning or after non-word character
    hashtag_regex = rb"(?:^|\W)(#[\w\-]+)"
    text_bytes = text.encode("UTF-8")
    for m in re.finditer(hashtag_regex, text_bytes):
        spans.append(
            {
                "start": m.start(1),
                "end": m.end(1),
                "tag": m.group(1)[1:].decode("UTF-8"),  # Remove # prefix
            }
        )
    return spans


def parse_facets(pds_url: str, text: str) -> List[Dict]:
    """
    Parse post text and return a list of app.bsky.richtext.facet objects for mentions, URLs, and hashtags.
    Indexing uses UTF-8 encoded bytestring offsets to match Bluesky API expectations.
    """
    facets = []

    # Parse mentions
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
            did = resp.json()["did"]
            facets.append(
                {
                    "index": {
                        "byteStart": m["start"],
                        "byteEnd": m["end"],
                    },
                    "features": [{"$type": "app.bsky.richtext.facet#mention", "did": did}],
                }
            )
        except requests.RequestException:
            # Skip mentions that can't be resolved
            continue

    # Parse URLs
    for u in parse_urls(text):
        facets.append(
            {
                "index": {
                    "byteStart": u["start"],
                    "byteEnd": u["end"],
                },
                "features": [
                    {
                        "$type": "app.bsky.richtext.facet#link",
                        "uri": u["url"],
                    }
                ],
            }
        )

    # Parse hashtags
    for h in parse_hashtags(text):
        facets.append(
            {
                "index": {
                    "byteStart": h["start"],
                    "byteEnd": h["end"],
                },
                "features": [
                    {
                        "$type": "app.bsky.richtext.facet#tag",
                        "tag": h["tag"],
                    }
                ],
            }
        )

    return facets


def parse_uri(uri: str) -> Dict:
    """Parse AT Protocol or Bluesky web URIs into component parts."""
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
    else:
        raise ValueError(f"Unhandled URI format: {uri}")


def get_reply_refs(pds_url: str, parent_uri: str) -> Dict:
    """Get the root and parent references needed for a reply."""
    uri_parts = parse_uri(parent_uri)
    resp = requests.get(
        pds_url + "/xrpc/com.atproto.repo.getRecord",
        params=uri_parts,
        timeout=10,
    )
    resp.raise_for_status()
    parent = resp.json()
    root = parent

    parent_reply = parent["value"].get("reply")
    if parent_reply is not None:
        root_uri = parent_reply["root"]["uri"]
        root_parts = parse_uri(root_uri)
        resp = requests.get(
            pds_url + "/xrpc/com.atproto.repo.getRecord",
            params=root_parts,
            timeout=10,
        )
        resp.raise_for_status()
        root = resp.json()

    return {
        "root": {
            "uri": root["uri"],
            "cid": root["cid"],
        },
        "parent": {
            "uri": parent["uri"],
            "cid": parent["cid"],
        },
    }


def get_mimetype(filename: str) -> str:
    """Determine MIME type from filename extension."""
    suffix = Path(filename).suffix.lower().lstrip(".")
    mime_types = {
        "png": "image/png",
        "jpeg": "image/jpeg",
        "jpg": "image/jpeg",
        "webp": "image/webp",
        "gif": "image/gif",
    }
    return mime_types.get(suffix, "application/octet-stream")


def upload_file(pds_url: str, access_token: str, filename: str, img_bytes: bytes) -> Dict:
    """Upload a file blob to the PDS."""
    mimetype = get_mimetype(filename)

    # WARNING: a non-naive implementation would strip EXIF metadata from JPEG files here by default
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
    """Upload images and create an embed object.

    Each image is paired with the alt text at the same index in alt_texts.
    If alt_texts is shorter than image_paths, remaining images get empty alt text.
    """
    images = []
    for i, ip in enumerate(image_paths):
        with open(ip, "rb") as f:
            img_bytes = f.read()

        if len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
            raise ValueError(
                f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum, got: {len(img_bytes):,}"
            )

        alt = ""
        if alt_texts and i < len(alt_texts):
            alt = alt_texts[i]

        blob = upload_file(pds_url, access_token, ip, img_bytes)
        images.append({"alt": alt, "image": blob})

    return {
        "$type": "app.bsky.embed.images",
        "images": images,
    }


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    """Fetch Open Graph metadata and create a link card embed."""
    card = {
        "uri": url,
        "title": "",
        "description": "",
    }

    # Fetch the HTML
    resp = requests.get(url, timeout=10)
    resp.raise_for_status()
    soup = BeautifulSoup(resp.text, "html.parser")

    # Extract Open Graph metadata
    title_tag = soup.find("meta", property="og:title")
    if title_tag and title_tag.get("content"):
        card["title"] = title_tag["content"]

    description_tag = soup.find("meta", property="og:description")
    if description_tag and description_tag.get("content"):
        card["description"] = description_tag["content"]

    image_tag = soup.find("meta", property="og:image")
    if image_tag and image_tag.get("content"):
        img_url = image_tag["content"]
        # Handle relative URLs
        if "://" not in img_url:
            img_url = urljoin(url, img_url)

        try:
            img_resp = requests.get(img_url, timeout=10)
            img_resp.raise_for_status()
            card["thumb"] = upload_file(pds_url, access_token, img_url, img_resp.content)
        except requests.RequestException:
            # Continue without thumbnail if image fetch fails
            pass

    return {
        "$type": "app.bsky.embed.external",
        "external": card,
    }


def get_embed_ref(pds_url: str, ref_uri: str) -> Dict:
    """Get a record reference for embedding (quote posts, etc.)."""
    uri_parts = parse_uri(ref_uri)
    resp = requests.get(
        pds_url + "/xrpc/com.atproto.repo.getRecord",
        params=uri_parts,
        timeout=10,
    )
    resp.raise_for_status()
    record = resp.json()

    return {
        "$type": "app.bsky.embed.record",
        "record": {
            "uri": record["uri"],
            "cid": record["cid"],
        },
    }


def create_post(args: argparse.Namespace) -> None:
    """Create and publish a post to Bluesky."""
    session = bsky_login_session(args.pds_url, args.handle, args.password)

    # trailing "Z" is preferred over "+00:00"
    now = datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")

    # Required fields for every post
    post = {
        "$type": "app.bsky.feed.post",
        "text": args.text,
        "createdAt": now,
    }

    # Indicate included languages (optional)
    if args.lang:
        post["langs"] = args.lang

    # Parse out mentions, URLs, and hashtags as "facets"
    if args.text:
        facets = parse_facets(args.pds_url, args.text)
        if facets:
            post["facets"] = facets

    # If this is a reply, get references to the parent and root
    if args.reply_to:
        post["reply"] = get_reply_refs(args.pds_url, args.reply_to)

    # Handle embeds (mutually exclusive)
    if args.image:
        post["embed"] = upload_images(
            args.pds_url, session["accessJwt"], args.image, args.alt_text
        )
    elif args.embed_url:
        post["embed"] = fetch_embed_url_card(
            args.pds_url, session["accessJwt"], args.embed_url
        )
    elif args.embed_ref:
        post["embed"] = get_embed_ref(args.pds_url, args.embed_ref)

    print("Creating post:", file=sys.stderr)
    print(json.dumps(post, indent=2), file=sys.stderr)

    resp = requests.post(
        args.pds_url + "/xrpc/com.atproto.repo.createRecord",
        headers={"Authorization": "Bearer " + session["accessJwt"]},
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


# ============== Tests ==============

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
    # note: a better regex would not mangle these:
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
        {"start": 13, "end": 20, "tag": "emoji"}
    ]
    assert parse_hashtags("#123 #abc-def") == [
        {"start": 0, "end": 4, "tag": "123"},
        {"start": 5, "end": 13, "tag": "abc-def"},
    ]


def main():
    parser = argparse.ArgumentParser(
        description="Bluesky post creation example script",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s "Hello, Bluesky!"
  %(prog)s "Check this out!" --image photo.jpg --alt-text "A photo"
  %(prog)s "Replying to a post" --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"
        """,
    )
    parser.add_argument(
        "--pds-url",
        default=os.environ.get("ATP_PDS_HOST", DEFAULT_PDS_URL),
        help=f"PDS URL (default: {DEFAULT_PDS_URL} or ATP_PDS_HOST env var)",
    )
    parser.add_argument(
        "--handle",
        default=os.environ.get("ATP_AUTH_HANDLE"),
        help="Bluesky handle (or ATP_AUTH_HANDLE env var)",
    )
    parser.add_argument(
        "--password",
        default=os.environ.get("ATP_AUTH_PASSWORD"),
        help="Bluesky app password (or ATP_AUTH_PASSWORD env var)",
    )
    parser.add_argument(
        "text",
        nargs="?",
        default="",
        help="Post text content",
    )
    parser.add_argument(
        "--image",
        action="append",
        metavar="PATH",
        help="Image file to attach (can be specified up to 4 times)",
    )
    parser.add_argument(
        "--alt-text",
        action="append",
        metavar="TEXT",
        help="Alt text for images (one per --image, in order)",
    )
    parser.add_argument(
        "--lang",
        action="append",
        metavar="CODE",
        help="Language code (e.g., 'en', can be specified multiple times)",
    )
    parser.add_argument(
        "--reply-to",
        metavar="URI",
        help="URI of post to reply to",
    )
    parser.add_argument(
        "--embed-url",
        metavar="URL",
        help="URL to embed as a link card",
    )
    parser.add_argument(
        "--embed-ref",
        metavar="URI",
        help="URI of post/record to embed (quote post)",
    )

    args = parser.parse_args()

    # Validation
    if not args.handle or not args.password:
        print("Error: Both handle and password are required.", file=sys.stderr)
        print("Set ATP_AUTH_HANDLE and ATP_AUTH_PASSWORD environment variables,", file=sys.stderr)
        print("or use --handle and --password arguments.", file=sys.stderr)
        sys.exit(1)

    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        print(f"Error: At most {MAX_IMAGES_PER_POST} images per post.", file=sys.stderr)
        sys.exit(1)

    if args.text and len(args.text) > MAX_POST_GRAPHEMES:
        print(f"Error: Post text exceeds {MAX_POST_GRAPHEMES} grapheme limit "
              f"(got {len(args.text)}).", file=sys.stderr)
        sys.exit(1)

    try:
        create_post(args)
    except requests.RequestException as e:
        print(f"Error: API request failed: {e}", file=sys.stderr)
        sys.exit(1)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
