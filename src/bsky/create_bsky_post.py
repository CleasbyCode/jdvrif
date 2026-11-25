#!/usr/bin/env python3

"""
Standalone Bluesky posting helper.

This is a security-hardened fork of Bryan Newbold's original create_bsky_post.py cookbook script:

 https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py
 https://gist.github.com/bnewbold
 https://bsky.app/profile/bnewbold.net

 Fork:

 https://github.com/CleasbyCode/cookbook/blob/main/python-bsky-post/create_bsky_post.py
 https://gist.github.com/CleasbyCode/1eb678ca1fa1975b1c1e20aeec33637e

Features:

 * Text posts with rich-text facets: links, @mentions (resolved to DIDs),
   hashtags (including fullwidth ＃) and cashtags (e.g. $TSLA)

 * Replies to existing posts (--reply-to)

 * Up to 4 attached images (--image) with per-image alt text (--alt-text)
   and Pillow-derived aspect ratios

 * External link cards (--embed-url) built from Open Graph metadata,
   including the og:image thumbnail

 * Quote records (--embed-ref) for posts, lists, and feed generators,
   optionally combined with images or a link card (record-with-media)

 * BCP 47 language tags (--lang, up to 3)

 * Custom PDS (--pds-url) and record lookup service (--record-service-url)

 * Built-in self-tests (--self-test) and verbose diagnostics (--verbose)

Security hardening over the original script:

 * SSRF protection for all attacker-influenced fetches (embed pages,
   redirects, og:image): DNS is resolved once, every answer must be a
   public unicast address, and connections are pinned to the validated IP
   while still authenticating the original TLS hostname

 * Bounded redirects with per-hop revalidation and HTTPS-to-HTTP
   downgrade refusal; redirects on credential-bearing requests rejected

 * Wall-clock deadlines on every network operation, with response size
   caps applied to decoded bytes so a compressed body cannot inflate past
   them; a Session serves one request at a time and is never reused once a
   call on it is abandoned at its deadline, so an abandoned call never
   shares connection state with a later one

 * The script attempts to revoke the login session (deleteSession) when
   the run ends (best effort: a failure only warns), and posts use a client-chosen record key so an ambiguous createRecord
   timeout is settled by lookup rather than risking a duplicate post

 * Strict validation of AT URIs, record CIDs, handles, language tags,
   image files (symlink-safe reads, preflight dimensions, bounded full
   decoding with unchanged upload bytes), and URLs

Setup:

 Requires Python 3.11+: requests, pillow and chardet (with pinned versions
 in requirements.txt alongside this script; without chardet, charset
 guessing falls back to charset-normalizer, a dependency of requests)
    $ python3 -m pip install -r requirements.txt

 Set credentials in an interactive Bash shell with a hidden password prompt,
 so the password is not typed into a command saved in shell history:
    $ export ATP_AUTH_HANDLE='your-handle.bsky.social'
    $ read -r -s -p 'Bluesky app password: ' ATP_AUTH_PASSWORD
    $ printf '\\n'
    $ export ATP_AUTH_PASSWORD

 Run `unset ATP_AUTH_PASSWORD` when you finish posting.

 IMPORTANT: ATP_AUTH_PASSWORD should be an APP password, created at
 https://bsky.app/settings/app-passwords — do NOT use your main Bluesky
 account password. App passwords can be revoked individually and cannot
 change authentication settings, though they can publish and manage content.

Examples:

    $ python3 create_bsky_post.py "Hello, Bluesky! #greetings"
    $ python3 create_bsky_post.py "Sunset over the bay" --image sunset.jpg --alt-text "Orange sunset over a calm bay"
    $ python3 create_bsky_post.py "Worth a read" --embed-url "https://example.com/article"
    $ python3 create_bsky_post.py "Replying" --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"
    $ python3 create_bsky_post.py "Quoting this post" --embed-ref "https://bsky.app/profile/example.com/post/yyy"
    $ python3 create_bsky_post.py "Quoted with media" --embed-ref "at://did:plc:xxx/app.bsky.feed.post/yyy" --image photo.jpg
"""

from __future__ import annotations

import argparse
import base64
import codecs
import errno
import hashlib
import html
import importlib
import io
import ipaddress
import json
import math
import os
import queue
import re
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
import unicodedata
import warnings
# Pillow, subprocess and secrets are imported where they are used: the parent
# never decodes an image (the worker does), and the others serve one step
# each, so a text-only post or a usage error loads none of them.
from bisect import bisect_left
from contextlib import contextmanager
from contextvars import ContextVar, copy_context
from datetime import datetime, timezone
from email.message import Message as _HeaderMessage
from http.cookiejar import DefaultCookiePolicy
from html.parser import HTMLParser
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional
from urllib.parse import ParseResult, urljoin, urlparse, urlunparse

import idna
import requests
from urllib3 import __version__ as _URLLIB3_VERSION
from requests.adapters import HTTPAdapter
from requests.certs import where as _requests_ca_bundle
from urllib3.util.ssl_ import create_urllib3_context



DEFAULT_PDS_URL = "https://bsky.social"
DEFAULT_RECORD_SERVICE_URL = "https://public.api.bsky.app"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 2_000_000
MAX_POST_BYTES = 3_000
MAX_TAG_GRAPHEMES = 64
MAX_TAG_BYTES = 640
MAX_LANGS = 3
MAX_EMBED_IMAGE_BYTES = 1_000_000
MAX_API_RESPONSE_BYTES = 8_000_000
MAX_IMAGE_PIXELS = 40_000_000
MAX_IMAGE_DIMENSION = 16_384
MAX_IMAGE_DECODE_SECONDS = 5.0
# One worker validates every image in a post. Its interpreter start-up (which
# imports Pillow) gets its own allowance, so a slow machine does not eat into
# the per-image decode budget.
MAX_IMAGE_WORKER_STARTUP_SECONDS = 15.0
MAX_IMAGE_WORKER_MEMORY_BYTES = 768 * 1024 * 1024
MAX_IMAGE_WORKER_REPLY_BYTES = 16 * 1024
MAX_IMAGE_FRAMES = 100
MAX_IMAGE_DECODED_PIXELS = 80_000_000
MAX_EXTERNAL_TITLE_CHARS = 300
MAX_EXTERNAL_DESCRIPTION_CHARS = 1_000
# The app.bsky.embed.images lexicon sets no maxLength on `alt`. This mirrors the
# official composer's limit, so alt text that would be rejected or silently
# truncated by clients fails locally instead.
MAX_ALT_TEXT_CHARS = 2_000
# Server-supplied error text is echoed to the terminal, so keep it short.
MAX_ERROR_DETAIL_CHARS = 300
MAX_ERROR_NAME_CHARS = 100
# How much of a refused response header (a Content-Type, a Content-Encoding)
# an error message quotes.
MAX_ECHOED_HEADER_CHARS = 100
# --verbose prints a failed API call's error body compactly, up to this much:
# indenting an arbitrarily nested body multiplies its size by its depth.
MAX_VERBOSE_ERROR_BODY_CHARS = 64 * 1024
# An image worker's error: an exception name plus at most 300 characters.
MAX_WORKER_ERROR_CHARS = 512
# Escaped in all remote text before printing: C0 controls (including
# CR/LF/TAB), DEL and the C1 block, which a terminal executes; the Unicode
# line and paragraph separators, which some terminals and log viewers break
# lines at; and the bidirectional marks, embeddings, overrides and isolates,
# which reorder what is displayed ("Trojan Source"), so a message could show
# text other than what it holds. Printable non-ASCII is left intact.
TERMINAL_UNSAFE_CHARACTERS = re.compile(
    "[\x00-\x1f\x7f-\x9f\u061c\u200e\u200f\u2028\u2029\u202a-\u202e\u2066-\u2069]"
)
# The link-card parse runs after the download deadline, so it has its own
# budget. Measured: 1 MB of hostile shallow markup takes ~2.5s to scan and
# parse; a real page's head takes milliseconds.
MAX_EMBED_PARSE_SECONDS = 5.0
# Link-card metadata lives in <head>, so only that part of the page is
# downloaded and parsed: reading stops at the end of the head, or after this
# many bytes when it cannot be located (the page's size does not matter).
MAX_EMBED_HEAD_BYTES = 1_000_000
# UTF-8, UTF-16BE and UTF-16LE: the only byte-order marks HTML recognises,
# with the codec that decodes (and drops) each.
HTML_BYTE_ORDER_MARKS = {
    b"\xef\xbb\xbf": "utf-8-sig",
    b"\xfe\xff": "utf-16",
    b"\xff\xfe": "utf-16",
}
# Python codecs that decode bytes to text but are not character sets a page
# can be written in. The non-text transforms (bytes-to-bytes zlib, bz2,
# base64, quopri, ...; str-to-str rot-13) are refused separately, by the flag
# bytes.decode() itself checks.
# UTF-7 is excluded as browsers exclude it (WHATWG Encoding Standard).
NON_CHARSET_TEXT_CODECS = frozenset({
    "charmap", "idna", "punycode", "raw-unicode-escape", "undefined",
    "unicode-escape", "utf-7",
    # Windows only: whatever the machine's ANSI or OEM code page is.
    "mbcs", "oem",
})
# Statistical charset detection only ever looks at this much of a page head;
# over all of a 1 MB head it takes seconds.
HTML_CHARSET_SNIFF_BYTES = 64 * 1024
# An XML declaration is looked for only in the page's first line, within this.
XML_DECLARATION_SNIFF_BYTES = 1024
# A statistical charset guess below this confidence is ignored in favour of
# windows-1252, the web's default for unlabelled pages. chardet 7 still
# answers for a few bytes (cp437 at 0.007 for "a\x81b") and reads a French
# windows-1252 page as MacCyrillic at about 0.1, while its right answers for
# Japanese, Chinese, Korean and Cyrillic pages score 0.22 to 0.43 (chardet 5
# scores those 0.73 to 0.99).
MIN_CHARSET_GUESS_CONFIDENCE = 0.2
# A link card is only built from HTML; any other declared type (a PDF, a video)
# is refused before its body is downloaded.
EMBED_HTML_CONTENT_TYPES = frozenset({"text/html", "application/xhtml+xml"})
# Mentions are resolved concurrently, and all of them share one overall budget
# so a post full of unresolvable handles cannot stall for minutes.
MAX_MENTION_RESOLUTION_SECONDS = 20.0
MENTION_RESOLUTION_WORKERS = 8
# uploadBlob's wall-clock budget grows with the blob, so a 2 MB image still
# fits on a slow (~256 kbit/s) uplink.
UPLOAD_BASE_TIMEOUT_SECONDS = 30.0
UPLOAD_MIN_BYTES_PER_SECOND = 32_000
# Record keys for new posts are client-generated TIDs, which makes a retried
# createRecord idempotent: the same key can never produce a second post.
TID_ALPHABET = "234567abcdefghijklmnopqrstuvwxyz"
MAX_REDIRECTS = 3
# An idle API Session older than this is closed rather than reused; see
# _ApiSessionPool. Well under the 5 s keep-alive timeout Node.js servers use.
API_SESSION_MAX_IDLE_SECONDS = 2.0
# The 3xx statuses that redirect to the Location URL (RFC 9110 section 15.4).
# 300 is a choice, 304 a cache answer, 305 a deprecated "use this proxy" that
# must never be followed as a resource redirect: none of them is the page.
REDIRECT_STATUS_CODES = frozenset({301, 302, 303, 307, 308})
# Pages, redirects and og:image URLs are fetched only on their scheme's
# default port. A public address is not a web server on every port: a URL
# naming :22, :25 or :6379 would aim an HTTP request at SSH, SMTP or Redis
# and let the page's author probe which ports answer. Service URLs (the PDS
# and record service) are chosen by the operator and keep any port.
EXTERNAL_FETCH_PORTS = {"http": 80, "https": 443}
MAX_DOWNLOAD_ADDRESS_ATTEMPTS = 4
# How long a download waits for a TCP (and TLS) connection to one address when
# another is left to try. Without it a black-holed first address (IPv6 that
# silently drops SYNs is common behind VPNs and containers) would use up the
# whole download deadline. Addresses are tried in turn, not raced, so this is
# longer than RFC 8305's 250 ms: a slow but working address must still win.
DOWNLOAD_CONNECT_ATTEMPT_SECONDS = 3.0
USER_AGENT = "bsky-post/1.1 (+https://github.com/CleasbyCode/cookbook)"
DOWNLOAD_CHUNK_SIZE = 64 * 1024

# Handles are ASCII-only, so letters are spelled out rather than matched with
# re.IGNORECASE: Unicode case folding would also accept "K" (Kelvin sign) or
# "ſ" (long s). The mention boundary keeps Unicode-aware \w.
HANDLE_REGEX = re.compile(
    r"^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?"
    r"(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*"
    r"\.[A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?$"
)
# A mention candidate is the whole run of handle characters after "@". It is
# validated as one unit by parse_mentions: matching the handle grammar here
# would let the regex backtrack to an earlier "." and link a different,
# shorter handle ("@alice.bsky.social-" as "@alice.bsky").
MENTION_REGEX = re.compile(r"(?<![\w@])(@[A-Za-z0-9.-]+)")
URL_REGEX = re.compile(
    r"(?<![\w/])((?:https?://)[^\s<>'\"`]+)",
    re.IGNORECASE,
)
# Mirrors the official composer: a tag ends at whitespace or at one of the
# invisible characters below (so "#a​b" tags "a" rather than nothing),
# and may not start with a variation selector.
HASHTAG_REGEX = re.compile(
    r"(^|\s)([#＃])(?!️)([^\s­⁠ ​‌‍⃢]+)"
)
CASHTAG_REGEX = re.compile(
    r"(^|\s|\()\$([A-Za-z][A-Za-z0-9]{0,4})"
    r"(?=\s|$|[.,;:!?)\"'\u2019])"
)
# Possessive runs: the alternatives start with different characters and
# segments are split by ":", so the language is unchanged, but the matcher
# never records a backtrack point per character. Use _is_did, which also
# applies the length limit first.
DID_REGEX = re.compile(
    r"^did:[a-z0-9]++:"
    r"(?:[A-Za-z0-9._-]++|%[0-9A-Fa-f]{2})++"
    r"(?::(?:[A-Za-z0-9._-]++|%[0-9A-Fa-f]{2})++)*+$"
)
# The AT Protocol DID specification's hard limit (W3C DID Core sets none).
# API answers may be megabytes long, so a
# value is measured before it is matched.
MAX_DID_LENGTH = 2048


def _is_did(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) <= MAX_DID_LENGTH
        and DID_REGEX.fullmatch(value) is not None
    )

# Opaque bearer token: one or more visible ASCII characters (a JWT is a subset),
# so it can never be empty or carry whitespace or control characters.
ACCESS_TOKEN_REGEX = re.compile(r"[!-~]+")
# Far above any real session JWT (about 1 KB), far below a header that could
# hurt anything.
MAX_TOKEN_CHARS = 16 * 1024


def _is_bearer_token(value: Any) -> bool:
    """Whether a session token can safely become an Authorization header."""
    return (
        isinstance(value, str)
        and len(value) <= MAX_TOKEN_CHARS
        and ACCESS_TOKEN_REGEX.fullmatch(value) is not None
    )

NSID_REGEX = re.compile(
    r"^[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?"
    r"(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)+"
    r"\.[a-zA-Z][a-zA-Z0-9]{0,62}$"
)
RECORD_KEY_REGEX = re.compile(r"^[A-Za-z0-9._:~-]{1,512}$")
INVALID_PERCENT_ESCAPE_REGEX = re.compile(r"%(?![0-9A-Fa-f]{2})")
# Exactly str.isspace() plus the C0 controls and DEL, checked over every code
# point, but scanned in C rather than one Python call per character.
URL_FORBIDDEN_CHARACTER_REGEX = re.compile(r"[\s\x00-\x1f\x7f]")
# URLs here come from the command line, post text, redirects and og:image
# tags of pages the script does not control; none legitimately approach this,
# and servers commonly refuse request lines far shorter.
MAX_URL_LENGTH = 8192
# The longest DNS name, not counting one trailing root dot.
MAX_HOSTNAME_LENGTH = 253
# One DNS label (underscore allowed: some CDNs use it) of any host that
# reaches the resolver, service or fetched; IDNs are checked by idna instead.
HOST_LABEL_REGEX = re.compile(r"[A-Za-z0-9_](?:[A-Za-z0-9_-]{0,61}[A-Za-z0-9_])?")
# RFC 5646 grandfathered tags: irregular forms predating the current subtag
# grammar, so they are matched literally rather than parsed.
GRANDFATHERED_LANGUAGE_TAGS = frozenset(
    # Only the irregular ones: the regular grandfathered tags (art-lojban,
    # cel-gaulish, no-bok, no-nyn, zh-guoyu, zh-hakka, zh-min, zh-min-nan,
    # zh-xiang) already parse as ordinary tags.
    "en-gb-oed i-ami i-bnn i-default i-enochian i-hak i-klingon i-lux "
    "i-mingo i-navajo i-pwn i-tao i-tay i-tsu sgn-be-fr sgn-be-nl "
    "sgn-ch-de".split()
)
TRAILING_URL_PUNCTUATION = ".,;:!?"
URL_CLOSING_TO_OPENING = {")": "(", "]": "[", "}": "{"}

IMAGE_FORMAT_MIMETYPES = {
    "PNG": "image/png",
    "JPEG": "image/jpeg",
    "WEBP": "image/webp",
    "GIF": "image/gif",
}

# The Pillow plugin that parses each accepted format. The worker imports only
# this one: Image.open(formats=[...]) falls back to Image.init(), which imports
# every format plugin Pillow ships, when the format is not registered yet, and
# Image.preinit() does not register WebP.
IMAGE_FORMAT_PLUGINS = {
    "PNG": "PIL.PngImagePlugin",
    "JPEG": "PIL.JpegImagePlugin",
    "WEBP": "PIL.WebPImagePlugin",
    "GIF": "PIL.GifImagePlugin",
}

BSKY_APP_COLLECTIONS = {
    "post": "app.bsky.feed.post",
    "lists": "app.bsky.graph.list",
    "feed": "app.bsky.feed.generator",
}

# Restrict decoding to zlib-backed codings. Optional Brotli installations can
# fall back to unbounded decompression even with a patched urllib3 release.
# Encoded responses additionally require a patched decoder before any read.
DECODABLE_CONTENT_ENCODINGS = frozenset({"", "identity", "gzip", "x-gzip", "deflate"})


def _terminal_safe(text: Any) -> str:
    """Render text that came from the network safe to print to a terminal.

    Control characters become visible ``\\xNN`` escapes rather than being
    executed by the terminal emulator, and line separators and bidirectional
    controls become ``\\uNNNN`` escapes rather than moving or reordering text. Applied at every print/raise site that
    can carry remote content, so no individual message has to remember to.
    """
    if not isinstance(text, str):
        text = str(text)
    return TERMINAL_UNSAFE_CHARACTERS.sub(
        lambda match: (
            f"\\x{ord(match.group()):02x}"
            if ord(match.group()) <= 0xFF
            else f"\\u{ord(match.group()):04x}"
        ),
        text,
    )


class DeadlineExceeded(requests.Timeout):
    """Raised when this script's own wall-clock deadline expires.

    Distinct from requests' own ConnectTimeout/ReadTimeout because only this one
    implies a blocking call was abandoned in a daemon worker thread that may
    still be using the Session it was handed. That worker owns the Session from
    then on; the openers learn this through `on_abandon` (an interrupted wait
    abandons it the same way), not from catching this exception.
    """


class _NoRedirectSession(requests.Session):
    """Leave redirects entirely to this script, including their unread bodies.

    Requests otherwise consumes a redirect body while constructing Response.next
    even with stream=True and allow_redirects=False, before our byte limits or
    redirect policy can inspect the response.
    """

    def __init__(self) -> None:
        super().__init__()
        # Never store cookies: a Session reused through _ApiSessionPool must
        # send exactly what a fresh one would.
        self.cookies.set_policy(DefaultCookiePolicy(allowed_domains=[]))

    def resolve_redirects(self, resp, req, **kwargs):
        return iter(())


def _new_session() -> requests.Session:
    """Build a Session for one request at a time (see DeadlineExceeded)."""
    session = _NoRedirectSession()
    session.headers["User-Agent"] = USER_AGENT
    session.headers["Accept-Encoding"] = "identity"
    session.trust_env = False
    return session


class _ApiSessionPool:
    """Idle API Sessions by origin, reused within one create_post run.

    Reuse keeps the TCP+TLS connection warm across the run's XRPC calls to the
    PDS and the record service. A Session is leased by one request at a time
    and only given back after a request that completed cleanly; one that was
    abandoned stays its worker's, and one that failed is closed, so the
    ownership rule in DeadlineExceeded is unchanged. A Session idle longer than
    API_SESSION_MAX_IDLE_SECONDS is closed instead of reused: a server may
    drop an idle keep-alive connection (Node.js does after 5 s), and a POST
    sent into one that just closed fails rather than being retried.
    """

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._idle: Dict[str, List[tuple[float, requests.Session]]] = {}
        self._closed = False

    def lease(self, origin: str) -> requests.Session:
        now = time.monotonic()
        stale: List[requests.Session] = []
        session: Optional[requests.Session] = None
        with self._lock:
            idle = self._idle.get(origin, [])
            while idle:
                returned_at, candidate = idle.pop()  # most recently used first
                if now - returned_at <= API_SESSION_MAX_IDLE_SECONDS:
                    session = candidate
                    break
                stale.append(candidate)
        _close_quietly(*stale)
        return session if session is not None else _new_session()

    def give_back(self, origin: str, session: requests.Session) -> None:
        with self._lock:
            if not self._closed:
                self._idle.setdefault(origin, []).append((time.monotonic(), session))
                return
        _close_quietly(session)

    def close(self) -> None:
        with self._lock:
            self._closed = True
            sessions = [session for idle in self._idle.values() for _, session in idle]
            self._idle.clear()
        _close_quietly(*sessions)


# Set by create_post for the length of one run; elsewhere every API request
# builds and closes its own Session.
_API_SESSION_POOL: ContextVar[Optional[_ApiSessionPool]] = ContextVar(
    "bsky_api_session_pool", default=None
)


def _session_origin(url: str) -> str:
    parsed = urlparse(url)
    return f"{parsed.scheme.lower()}://{parsed.netloc.lower()}"


def _close_quietly(*closeables: Any) -> None:
    for closeable in closeables:
        if closeable is None:
            continue
        try:
            closeable.close()
        except Exception:
            pass


_DOWNLOAD_DEADLINE: ContextVar[Optional[float]] = ContextVar(
    "_DOWNLOAD_DEADLINE",
    default=None,
)
_NETWORK_TIMEOUT_SUBJECT: ContextVar[str] = ContextVar(
    "_NETWORK_TIMEOUT_SUBJECT",
    default="External download",
)
# Validated public addresses by A-label host, for one link card: its page,
# every redirect hop (an http:// to https:// one included: the port plays no
# part in which addresses a name has) and its og:image. A host met again is
# neither looked up again nor able to answer differently within the card.
_RESOLVED_ADDRESSES: ContextVar[Optional[Dict[str, List[str]]]] = ContextVar(
    "_RESOLVED_ADDRESSES",
    default=None,
)
class _ResponseCleanup:
    """Closes one streamed response and its Session, exactly once.

    A body read abandoned by its waiter (deadline or interrupt) hands this to
    its worker and marks it handed off, so the code that opened the response
    leaves both open for the worker whatever exception, if any, reaches it.
    """

    def __init__(self, *closeables: Any):
        self._closeables = closeables
        self.handed_off = False

    def close(self) -> None:
        _close_quietly(*self._closeables)

    def hand_off(self) -> None:
        self.handed_off = True


# Set while a streamed response is open (see _ResponseCleanup).
_ABANDONED_RESPONSE_CLEANUP: ContextVar[Optional[_ResponseCleanup]] = ContextVar(
    "_ABANDONED_RESPONSE_CLEANUP",
    default=None,
)
# RFC 6052 well-known NAT64 prefix. Disjoint from the local-use 64:ff9b:1::/48.
_WELL_KNOWN_NAT64_NETWORK = ipaddress.IPv6Network("64:ff9b::/96")
# Special-purpose ranges that no web server is reached at, refused by prefix
# because ipaddress classifies them differently from one Python release to
# the next (CVE-2024-4032 fixed some in 3.11.10 / 3.12.4; ORCHIDv2 and DRIP
# count as global on 3.13; 3fff::/20 and 5f00::/16 are newer than some
# releases' tables). Membership is False across IP versions, never an error.
_NON_PUBLIC_SPECIAL_NETWORKS = tuple(ipaddress.ip_network(network) for network in (
    "192.0.0.0/24",    # IETF protocol assignments
    "192.88.99.0/24",  # deprecated 6to4 relay anycast
    "198.18.0.0/15",   # benchmarking
    "100::/64",        # discard-only
    "2001:2::/48",     # benchmarking
    "2001:10::/28",    # ORCHID (deprecated)
    "2001:20::/28",    # ORCHIDv2: identifiers, not locators
    "2001:30::/28",    # DRIP: drone remote ID identifiers
    "3fff::/20",       # documentation (RFC 9637)
    "5f00::/16",       # SRv6 SIDs
    "::ffff:0:0/96",   # IPv4-mapped (DNS answers are converted to IPv4 first)
    "::ffff:0:0:0/96", # IPv4-translated (SIIT)
    "64:ff9b:1::/48",  # local-use IPv4/IPv6 translation
    # 6to4 and Teredo: whether ipaddress counts 2002::/16 as private depends
    # on the Python release (3.10 and early 3.11 do not).
    "2002::/16",
    "2001::/32",
))


def _bracketed_host(host: str, port: Optional[int]) -> str:
    """A URL authority: an IPv6 literal goes in brackets, then any port."""
    authority = f"[{host}]" if ":" in host else host
    return authority if port is None else f"{authority}:{port}"


def _api_url(pds_url: str, method: str) -> str:
    return f"{pds_url.rstrip('/')}/xrpc/{method}"


def _url_for_log(url: str) -> str:
    try:
        parsed = urlparse(url)
        hostname = parsed.hostname
        port = parsed.port
    except (TypeError, ValueError):
        return "<redacted URL>"
    # Requests ends the authority at a backslash, urlparse does not, so text
    # after one ("https://cdn.example\\token/x") would print as the "host".
    if not parsed.scheme or not hostname or "\\" in parsed.netloc:
        return "<redacted URL>"
    if len(hostname) > MAX_HOSTNAME_LENGTH:  # no real host: keep the log short
        hostname = hostname[:MAX_HOSTNAME_LENGTH] + "…"
    authority = _bracketed_host(hostname, port)
    suffix = "/…" if parsed.path not in ("", "/") else parsed.path
    # Escaped here, so every caller can print it: urlparse keeps C1 controls
    # and bidi overrides in a host ("https://evil\x9b2J\u202e.com/").
    return _terminal_safe(f"{parsed.scheme.lower()}://{authority}{suffix}")


def _parse_url(url: str) -> ParseResult:
    """Parse and check an http(s) URL; every URL the script uses is one."""
    if len(url) > MAX_URL_LENGTH:
        raise ValueError(f"URL exceeds {MAX_URL_LENGTH} characters")
    if (
        not url
        or URL_FORBIDDEN_CHARACTER_REGEX.search(url)
        or INVALID_PERCENT_ESCAPE_REGEX.search(url)
    ):
        raise ValueError("Invalid URL")
    try:
        parsed = urlparse(url)
    except ValueError as exc:
        raise ValueError("Invalid URL") from exc
    scheme = parsed.scheme.lower()
    if scheme not in ("http", "https"):
        raise ValueError("URL must use one of these schemes: http, https")
    # urlparse ends the authority only at "/", "?" or "#", but requests and
    # urllib3 also end it at a backslash: "http://evil.com\.localhost" would
    # be checked as host "evil.com\.localhost" yet sent to evil.com.
    if "\\" in parsed.netloc:
        raise ValueError("URL authority must not contain a backslash")
    if not parsed.hostname:
        raise ValueError("URL has no host")
    if len(parsed.hostname.removesuffix(".")) > MAX_HOSTNAME_LENGTH:
        raise ValueError(f"URL host exceeds {MAX_HOSTNAME_LENGTH} characters")
    # An empty DNS label ("https://.../x", "a..b", ".a") names no host; one
    # trailing root dot is fine. IP literals contain no empty labels either.
    if not all(parsed.hostname.removesuffix(".").split(".")):
        raise ValueError("URL has an invalid host")
    # Any userinfo, even an empty one ("https://@host", "https://:@host"):
    # username and password are "" then, which a truthiness test misses.
    if "@" in parsed.netloc:
        raise ValueError("URL must not include credentials")
    try:
        port = parsed.port
    except ValueError as exc:
        raise ValueError("URL has an invalid port") from exc
    if port == 0:  # urlparse already refuses anything above 65535
        raise ValueError("URL has an invalid port")
    return parsed


def _is_loopback_hostname(hostname: str) -> bool:
    """Whether plain HTTP to this host can only reach this machine.

    Only a loopback IP literal, or "localhost" exactly, which the system's
    hosts file answers. Anything else goes to the resolver, which may ask
    DNS: "127.0.0.1." is no IP literal to getaddrinfo, and neither
    "localhost." nor "pds.localhost" is in a standard hosts file, so a rogue
    DNS answer could send the app password, in clear text, anywhere.
    """
    if hostname.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(hostname).is_loopback
    except ValueError:
        return False


def normalize_service_url(
    service_url: str,
    *,
    service_name: str,
    allow_insecure: bool = False,
) -> str:
    # Service URLs are operator-supplied, never chosen by post content, so
    # they deliberately skip the public-address SSRF check. HTTPS is required
    # unless explicitly opted out for loopback.
    parsed = _parse_url(service_url.strip())
    # urlparse drops an empty ";" parameter (params == ""), so look for it raw.
    if (
        parsed.path not in ("", "/")
        or ";" in service_url  # params, empty or not, only follow a ";"
        or parsed.query
        or parsed.fragment
    ):
        raise ValueError(
            f"{service_name} URL must be only a scheme and host, "
            "without path/query/fragment"
        )
    hostname = _ascii_hostname(parsed.hostname or "")
    scheme = parsed.scheme.lower()
    if scheme != "https" and not allow_insecure:
        raise ValueError(
            f"Refusing to use a non-HTTPS {service_name} URL "
            "(use --allow-insecure-pds only for local testing)"
        )
    if scheme != "https" and not _is_loopback_hostname(hostname):
        raise ValueError(
            f"Insecure {service_name} URLs are limited to localhost or loopback IPs"
        )
    # Rebuilt from the host that was checked, not echoed from the input:
    # "127。0。0。1" (ideographic full stops) is checked as 127.0.0.1, and every
    # later request must name that same host.
    return f"{scheme}://{_bracketed_host(hostname, parsed.port)}"


def _remaining_download_time(deadline: float, operation: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise DeadlineExceeded(
            f"{_NETWORK_TIMEOUT_SUBJECT.get()} timed out while {operation}"
        )
    return remaining


@contextmanager
def _network_deadline(timeout: float, *, subject: str) -> Iterator[float]:
    """Scope one network operation to a wall-clock deadline.

    An inner call never extends an outer one: the effective deadline is the
    earlier of the two, so a link-card fetch cannot outlive the budget of the
    operation that started it. Both context variables are restored on exit.
    """
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError(f"{subject} timeout must be a positive finite number")
    outer_deadline = _DOWNLOAD_DEADLINE.get()
    requested_deadline = time.monotonic() + timeout
    deadline = (
        requested_deadline
        if outer_deadline is None
        else min(requested_deadline, outer_deadline)
    )
    deadline_token = _DOWNLOAD_DEADLINE.set(deadline)
    subject_token = _NETWORK_TIMEOUT_SUBJECT.set(subject)
    try:
        yield deadline
    finally:
        _NETWORK_TIMEOUT_SUBJECT.reset(subject_token)
        _DOWNLOAD_DEADLINE.reset(deadline_token)


def _run_before_download_deadline(
    action: Callable[[], Any],
    deadline: float,
    operation: str,
    *,
    dispose_abandoned: Optional[Callable[[Any], None]] = None,
    on_abandon: Optional[Callable[[], None]] = None,
) -> Any:
    """Run one blocking operation without allowing it past the total deadline.

    On timeout the blocking call is abandoned in its daemon worker thread while
    the caller raises DeadlineExceeded. The abandoned worker keeps whatever
    Session it was handed, and because a Session serves one request at a time
    and an abandoned one is never reused (_new_session, _ApiSessionPool), it
    cannot interfere with any later request. Callers may
    therefore catch a timeout and continue; they must only avoid closing the
    Session the worker was given. The worker is abandoned on DeadlineExceeded
    and also when the wait itself is interrupted (KeyboardInterrupt or any
    other BaseException in the calling thread). Whenever it is, `on_abandon`
    runs first, synchronously, before anything propagates, so the caller knows
    the worker's resources are no longer its to close; and `dispose_abandoned`
    runs exactly once: with the worker's result (or None if it failed) as soon
    as the worker is done, so those resources are released only when nothing
    is still using them.
    """
    def dispose(succeeded: bool, value: Any) -> None:
        if dispose_abandoned is not None:
            try:
                dispose_abandoned(value if succeeded else None)
            except Exception:
                pass

    # Do not launch work when an inherited deadline has already expired. The
    # caller treats this like any other DeadlineExceeded and leaves cleanup to
    # `dispose_abandoned`, so run it here: no worker will.
    try:
        _remaining_download_time(deadline, operation)
    except DeadlineExceeded:
        if on_abandon is not None:
            on_abandon()
        dispose(False, None)
        raise

    outcome: queue.Queue[tuple[bool, Any]] = queue.Queue(maxsize=1)
    state_lock = threading.Lock()
    abandoned = False

    def abandon_pending_outcome() -> None:
        nonlocal abandoned
        if on_abandon is not None:
            on_abandon()
        with state_lock:
            abandoned = True
            try:
                late_outcome = outcome.get_nowait()
            except queue.Empty:
                return
        # The worker already finished, so it will not dispose of anything.
        dispose(*late_outcome)

    def worker() -> None:
        try:
            value = action()
            succeeded = True
        except BaseException as exc:
            # Even a BaseException must reach the waiting caller (or the
            # disposal), or the caller would wait out the whole deadline.
            value = exc
            succeeded = False
        with state_lock:
            if not abandoned:
                outcome.put_nowait((succeeded, value))
                return
        dispose(succeeded, value)

    thread = threading.Thread(
        target=worker,
        name="bsky-download-operation",
        daemon=True,
    )
    try:
        # Started inside the try: thread.start() itself blocks until the
        # thread is running, and an interrupt there leaves a live worker.
        thread.start()
        succeeded, value = outcome.get(
            timeout=_remaining_download_time(deadline, operation)
        )
    except queue.Empty as exc:
        abandon_pending_outcome()
        raise DeadlineExceeded(
            f"{_NETWORK_TIMEOUT_SUBJECT.get()} timed out while {operation}"
        ) from exc
    except BaseException:
        # Our own deadline expired (DeadlineExceeded), or the wait was
        # interrupted: the worker keeps running, so hand its resources over.
        abandon_pending_outcome()
        raise

    if not succeeded:
        raise value
    return value


def _isatap_ipv4_address(
    address: ipaddress.IPv6Address,
) -> Optional[ipaddress.IPv4Address]:
    """The IPv4 address an ISATAP address tunnels to, if it is one.

    ISATAP (RFC 5214) interface ID: ::0:5efe:a.b.c.d or ::200:5efe:a.b.c.d
    (the u bit). IPv4-mapped and -translated addresses, 6to4 and Teredo are
    refused by prefix, and NAT64 judged by its own IPv4, before this is asked.
    """
    if (int(address) >> 32) & 0xFDFFFFFF != 0x00005EFE:
        return None
    return ipaddress.IPv4Address(int(address) & 0xFFFFFFFF)


def _is_public_unicast_address(
    address: ipaddress.IPv4Address | ipaddress.IPv6Address,
) -> bool:
    if any(address in network for network in _NON_PUBLIC_SPECIAL_NETWORKS):
        return False
    if isinstance(address, ipaddress.IPv6Address):
        # A zone ("%eth0") names a local interface: only meaningful for
        # link-local scope, and never part of a public destination.
        if address.scope_id is not None:
            return False
        if address in _WELL_KNOWN_NAT64_NETWORK:
            # On a DNS64/NAT64 network every public host resolves into this
            # prefix, and Python reports the whole /96 as is_reserved -- which
            # would refuse every link-card and og:image fetch on IPv6-only
            # networks. Such an address *is* its embedded IPv4, so judge it by
            # that instead, which keeps 64:ff9b::169.254.169.254 refused.
            return _is_public_unicast_address(
                ipaddress.IPv4Address(int(address) & 0xFFFFFFFF)
            )
    excluded = (
        address.is_private,
        address.is_loopback,
        address.is_link_local,
        address.is_multicast,
        address.is_reserved,
        address.is_unspecified,
        getattr(address, "is_site_local", False),
    )
    if not address.is_global or any(excluded):
        return False
    if isinstance(address, ipaddress.IPv6Address):
        tunnelled = _isatap_ipv4_address(address)
        return tunnelled is None or _is_public_unicast_address(tunnelled)
    return True


def _public_url_addresses(url: str) -> tuple[ParseResult, List[str]]:
    """Resolve a URL once and return only validated public connection targets."""
    parsed = _parse_url(url)
    # "%" only ever starts an IPv6 zone ("[fe80::1%eth0]") or a percent-escape
    # in a host; neither names a public host, and a zone would go out in Host.
    if "%" in (parsed.hostname or ""):
        raise ValueError(f"Refusing to fetch {_url_for_log(url)!r}: invalid host")
    port = EXTERNAL_FETCH_PORTS[parsed.scheme.lower()]
    if parsed.port not in (None, port):
        raise ValueError(
            f"Refusing to fetch port {parsed.port} of {_url_for_log(url)!r}: "
            f"only the {parsed.scheme.lower()} default port {port} is fetched"
        )
    # Resolve the same A-label that TLS sends as SNI and authenticates. The raw
    # Unicode name would go through CPython's IDNA2003 codec, which encodes
    # some labels ('ß', ZWJ) differently.
    hostname = _ascii_hostname(parsed.hostname or "")
    # "example.com." and "example.com" are one host within a card.
    cache_key = hostname.removesuffix(".")
    cache = _RESOLVED_ADDRESSES.get()
    cached = None if cache is None else cache.get(cache_key)
    if cached is not None:
        return parsed, list(cached)

    def resolve() -> Any:
        return socket.getaddrinfo(
            hostname,
            port,
            type=socket.SOCK_STREAM,
        )

    try:
        deadline = _DOWNLOAD_DEADLINE.get()
        infos = (
            resolve()
            if deadline is None
            else _run_before_download_deadline(
                resolve,
                deadline,
                f"resolving {hostname!r}",
            )
        )
    except socket.gaierror as exc:
        raise ValueError(f"Could not resolve {hostname!r}: {exc}") from exc

    addresses: List[str] = []
    for info in infos:
        address = info[4][0]
        ip = ipaddress.ip_address(address)
        ip = getattr(ip, "ipv4_mapped", None) or ip
        if not _is_public_unicast_address(ip):
            raise ValueError(
                f"Refusing to fetch non-public address {ip} for host {hostname!r}"
            )
        normalized = str(ip)
        if normalized not in addresses:
            addresses.append(normalized)
    if not addresses:
        raise ValueError(f"Could not resolve {hostname!r} to an IP address")
    addresses = _interleave_address_families(addresses)
    if cache is not None:
        cache[cache_key] = list(addresses)
    return parsed, addresses


def _interleave_address_families(addresses: List[str]) -> List[str]:
    """Alternate IPv6 and IPv4, keeping the resolver's order within each.

    The family the resolver listed first stays first (RFC 8305, section 4), so
    a broken family costs one connect timeout rather than every attempt:
    only the first MAX_DOWNLOAD_ADDRESS_ATTEMPTS addresses are ever tried.
    """
    first_is_v6 = bool(addresses) and ":" in addresses[0]
    primary = [address for address in addresses if (":" in address) == first_is_v6]
    secondary = [address for address in addresses if (":" in address) != first_is_v6]
    interleaved: List[str] = []
    for index in range(max(len(primary), len(secondary))):
        interleaved += primary[index:index + 1] + secondary[index:index + 1]
    return interleaved


def _ascii_hostname(hostname: str) -> str:
    # A DNS name never exceeds 253 characters, whatever its script. Refusing
    # longer input first keeps attacker-chosen hostnames (redirect targets,
    # og:image URLs) away from the resolver, from error messages, and from
    # idna's slow paths on over-long input (CVE-2024-3651, CVE-2026-45409).
    # Only one trailing dot (the DNS root) is free; rstrip(".") would let
    # "ü" + "." * 500_000 slip past the cap into idna and the error message.
    if len(hostname.removesuffix(".")) > MAX_HOSTNAME_LENGTH:
        raise ValueError(f"Hostname exceeds {MAX_HOSTNAME_LENGTH} characters")
    try:
        return str(ipaddress.ip_address(hostname))
    except ValueError:
        pass
    # Leave already-ASCII hostnames untouched: the stdlib "idna" codec is
    # IDNA2003 and rejects perfectly resolvable names (e.g. underscore labels
    # used by some CDNs), so only internationalized names are A-label encoded.
    # Use the IDNA2008 `idna` package (a transitive dependency of requests) and
    # surface a clear error instead of a cryptic UnicodeError on bad input.
    if hostname.isascii():
        # Only DNS labels reach the resolver: urlparse lets "!$&'()*+,;=" and
        # the like through in a host, and getaddrinfo might try anything.
        if not all(
            HOST_LABEL_REGEX.fullmatch(label)
            for label in hostname.removesuffix(".").split(".")
        ):
            raise ValueError(f"Invalid hostname: {hostname!r}")
        return hostname
    try:
        return idna.encode(hostname, uts46=True).decode("ascii")
    except idna.IDNAError as exc:
        raise ValueError(f"Invalid internationalized hostname: {hostname!r}") from exc


def _pinned_url(parsed: ParseResult, address: str) -> str:
    """Replace the URL authority with a previously validated IP literal."""
    return urlunparse(parsed._replace(netloc=_bracketed_host(address, parsed.port)))


class _PinnedSslContextPool:
    """Idle SSLContexts for pinned adapters, keyed by CA bundle path.

    Loading the CA bundle (about 150 certificates) takes about 8 ms, and every
    address attempt on every redirect hop builds an adapter. A context is
    leased to one adapter at a time and comes back only when that adapter is
    done with it, so live and abandoned adapters never share one.
    """

    def __init__(self, max_idle: int = 2):
        self._lock = threading.Lock()
        self._idle: Dict[str, List[Any]] = {}
        self._max_idle = max_idle

    def lease(self, ca_bundle: str) -> Any:
        with self._lock:
            idle = self._idle.get(ca_bundle)
            if idle:
                return idle.pop()
        context = create_urllib3_context()
        context.load_verify_locations(ca_bundle)
        return context

    def give_back(self, ca_bundle: str, context: Any) -> None:
        with self._lock:
            idle = self._idle.setdefault(ca_bundle, [])
            if len(idle) < self._max_idle:
                idle.append(context)


_PINNED_SSL_CONTEXTS = _PinnedSslContextPool()


class _PinnedHTTPSAdapter(HTTPAdapter):
    """Connect to an IP literal while authenticating the URL's original hostname.

    The whole SSRF pinning scheme rests on urllib3 honouring two pool options:
    `server_hostname` (the SNI name sent in the TLS handshake) and
    `assert_hostname` (the name the presented certificate is verified against).
    Passing these via `connection_pool_kw` is supported by urllib3 >= 2, which
    requirements.txt pins; older releases handle them differently, so the pin is
    load-bearing rather than cosmetic.

    Each adapter also brings its own SSLContext. Without one, requests >= 2.32
    hands every pool its process-wide `_preloaded_ssl_context`, and urllib3
    sets `check_hostname = False` on whatever context it gets once
    `assert_hostname` is set: that would switch OpenSSL's own hostname check
    off for every later API request, and mutate a context shared with
    abandoned workers. A private context is touched by this adapter alone:
    it is leased from _PINNED_SSL_CONTEXTS, and only `release()`, called once
    the adapter's Session closed with no abandoned worker left using it, makes
    it available to a later adapter. It trusts the same CA bundle that
    requests' default context loads.
    """

    def __init__(self, hostname: str):
        # _ascii_hostname keeps a DNS root dot; urllib3 strips it from SNI but
        # not from assert_hostname, so no certificate would match "host.".
        self._hostname = _ascii_hostname(hostname).removesuffix(".")
        self._ca_bundle = _requests_ca_bundle()
        self._ssl_context: Optional[Any] = _PINNED_SSL_CONTEXTS.lease(self._ca_bundle)
        # One pinned address serves one request, so one connection suffices.
        super().__init__(pool_connections=1, pool_maxsize=1)

    def release(self) -> None:
        """Return the context for reuse; only once nothing can still use it."""
        context, self._ssl_context = self._ssl_context, None
        if context is not None:
            _PINNED_SSL_CONTEXTS.give_back(self._ca_bundle, context)

    def init_poolmanager(self, connections, maxsize, block=False, **pool_kwargs):
        pool_kwargs["ssl_context"] = self._ssl_context
        pool_kwargs["assert_hostname"] = self._hostname
        pool_kwargs["server_hostname"] = self._hostname
        super().init_poolmanager(connections, maxsize, block, **pool_kwargs)


@contextmanager
def _open_pinned_response(
    url: str,
    timeout: float,
) -> Iterator[requests.Response]:
    """Open one response using one of a bounded set of validated addresses."""
    parsed, addresses = _public_url_addresses(url)
    deadline = _DOWNLOAD_DEADLINE.get()
    attempts = addresses[:MAX_DOWNLOAD_ADDRESS_ATTEMPTS]
    # The same for every address: the name TLS authenticates and Host sends.
    hostname = _ascii_hostname(parsed.hostname or "")
    host_header = _bracketed_host(hostname, parsed.port)
    connecting_to = f"connecting to {_url_for_log(url)!r}"
    for attempt, address in enumerate(attempts, start=1):
        session = _new_session()
        # Only its flag is used: marked handed off once an abandoned worker
        # owns `session`.
        connecting = _ResponseCleanup()
        adapter: Optional[_PinnedHTTPSAdapter] = None

        def close_session() -> None:
            # Called only within this attempt, when no worker can still be
            # using `session`, so the adapter's SSLContext may go back for the
            # next attempt or hop.
            session.close()
            if adapter is not None:
                adapter.release()

        try:
            # Inside the try: building the adapter reads the CA bundle and can
            # fail (or be interrupted), which must still close `session`.
            if parsed.scheme.lower() == "https":
                adapter = _PinnedHTTPSAdapter(hostname)
                session.mount("https://", adapter)
            remaining = (
                timeout
                if deadline is None
                else _remaining_download_time(deadline, connecting_to)
            )
            # (connect, read): with another address left, connecting gets only
            # its own short allowance, and never more than an even share of
            # what remains (little may, after slow redirects), so every
            # address gets its turn; the last one may use all that remains.
            left = len(attempts) - attempt + 1
            request_timeout: Any = (
                (min(DOWNLOAD_CONNECT_ATTEMPT_SECONDS, remaining / left), remaining)
                if attempt < len(attempts)
                else remaining
            )

            def request() -> requests.Response:
                return session.get(
                    _pinned_url(parsed, address),
                    headers={"Host": host_header},
                    timeout=request_timeout,
                    stream=True,
                    allow_redirects=False,
                )

            response = (
                request()
                if deadline is None
                else _run_before_download_deadline(
                    request,
                    deadline,
                    connecting_to,
                    # The abandoned worker owns `session` from here on, so both
                    # it and the late response are released together once that
                    # worker finishes. `session` is bound as a default so the
                    # callback can never see a later iteration's Session.
                    dispose_abandoned=(
                        lambda late_response, owned=session: _close_quietly(
                            late_response,
                            owned,
                        )
                    ),
                    on_abandon=connecting.hand_off,
                )
            )
        except DeadlineExceeded:
            # Leave `session` alone if a daemon worker may still be using it.
            if not connecting.handed_off:
                close_session()
            raise
        except requests.RequestException as exc:
            # A per-address failure (including requests' own connect/read
            # timeouts) leaves nothing running, so this Session can be closed
            # and the next address tried -- unless the deadline is already gone.
            close_session()
            if deadline is not None:
                _remaining_download_time(deadline, connecting_to)
            if attempt < len(attempts):
                continue
            raise requests.RequestException(
                "External download request failed for "
                f"{_url_for_log(url)!r} ({type(exc).__name__})"
            ) from exc
        except BaseException:
            # Interrupted (e.g. KeyboardInterrupt): same ownership rule.
            if not connecting.handed_off:
                close_session()
            raise

        cleanup = _ResponseCleanup(response, session)
        cleanup_token = _ABANDONED_RESPONSE_CLEANUP.set(cleanup)
        try:
            yield response
        finally:
            _ABANDONED_RESPONSE_CLEANUP.reset(cleanup_token)
            # Only an abandoned body read keeps them in use; any other exit,
            # a KeyboardInterrupt included, releases them here.
            if not cleanup.handed_off:
                response.close()
                close_session()
        return


def _urllib3_has_bounded_decoding() -> bool:
    # Fail closed on older and unrecognized versions, including prereleases.
    # Before 2.7.0, streaming decoders could allocate the expanded body before
    # Requests yields a chunk to the caller's size check; before 2.8.0,
    # chunked deflate streaming could loop forever (GHSA-gh4c-6fx4-qh6g).
    version = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", _URLLIB3_VERSION)
    return version is not None and tuple(map(int, version.groups())) >= (2, 8, 0)


def _read_response_body(
    resp: requests.Response,
    max_bytes: int,
    *,
    html_head_only: bool = False,
) -> bytes:
    """Read a response body of at most `max_bytes`.

    With `html_head_only`, return just the page's head (see _HeadPrefix),
    or its first `max_bytes` when no head ends within them: reading stops
    there, so a large page is neither downloaded in full nor refused for its
    size.
    """
    # Accept-Encoding is advisory: validate the actual response coding before
    # reading. With a patched decoder, the chunk limit also bounds allocation
    # inside urllib3 instead of merely checking an already-expanded body.
    # Looked up exactly as urllib3 picks its decoder (lower-cased, NOT
    # stripped): "gzip " would otherwise pass here yet get no decoder, and
    # raw compressed bytes would reach the parsers past the size pre-check.
    content_encoding = resp.headers.get("Content-Encoding", "").lower()
    if content_encoding not in DECODABLE_CONTENT_ENCODINGS:
        raise ValueError(
            "Refusing remote response with undecodable Content-Encoding "
            f"{content_encoding[:MAX_ECHOED_HEADER_CHARS]!r}"
        )
    is_encoded = content_encoding not in ("", "identity")
    if is_encoded and not _urllib3_has_bounded_decoding():
        raise ValueError(
            "Refusing compressed response: urllib3 2.8.0 or newer is required "
            "for bounded decompression"
        )

    # Content-Length describes the encoded body, so it only bounds the decoded
    # size when no coding was applied.
    try:
        declared: Optional[int] = int(resp.headers.get("Content-Length", ""))
    except ValueError:  # absent, blank or malformed: nothing declared
        declared = None
    if declared is not None and declared < 0:
        raise ValueError("Remote response declares a negative Content-Length")
    if (
        declared is not None
        and not is_encoded
        and declared > max_bytes
        and not html_head_only
    ):
        raise ValueError(
            f"Remote response declares {declared} bytes, above {max_bytes} limit"
        )

    def read_head() -> bytes:
        head = _HeadPrefix(max_bytes)
        for chunk in resp.iter_content(DOWNLOAD_CHUNK_SIZE):
            if chunk and head.feed(chunk):
                break  # the opener closes the response, unread remainder and all
        return head.result()

    def read_body() -> bytes:
        body = bytearray()
        for chunk in resp.iter_content(DOWNLOAD_CHUNK_SIZE):
            if not chunk:
                continue
            if len(body) + len(chunk) > max_bytes:
                raise ValueError(f"Remote response exceeds {max_bytes} bytes")
            body.extend(chunk)
        return bytes(body)

    reader = read_head if html_head_only else read_body
    deadline = _DOWNLOAD_DEADLINE.get()
    if deadline is None:
        return reader()
    cleanup = _ABANDONED_RESPONSE_CLEANUP.get()
    # Once abandoned, the worker (or the expired-deadline path) owns the
    # close, even if a caller catches the error and the opener unwinds.
    return _run_before_download_deadline(
        reader,
        deadline,
        "reading the response body",
        dispose_abandoned=(
            None if cleanup is None else lambda _late_body: cleanup.close()
        ),
        on_abandon=None if cleanup is None else cleanup.hand_off,
    )


def _redirect_target(resp: requests.Response, current_url: str) -> str:
    # http.client keeps trailing whitespace in a header value; HTTP says the
    # optional whitespace (space, tab) around it is not part of the URL.
    location = (resp.headers.get("Location") or "").strip(" \t")
    if not location:
        raise ValueError(
            f"Redirect from {_url_for_log(current_url)!r} missing Location header"
        )
    # Before urljoin, as for og:image: collapsing "../" segments is slow.
    if len(location) > MAX_URL_LENGTH:
        raise ValueError(f"Redirect target exceeds {MAX_URL_LENGTH} characters")
    return urljoin(current_url, location)


# Real Content-Type values are a few dozen characters. Python's email header
# parser is quadratic in the ";"s inside an unclosed quote, and a server may
# send many Content-Type headers that urllib3 joins into one multi-megabyte
# value: hours of CPU, in a parse the deadline cannot stop.
MAX_CONTENT_TYPE_CHARS = 1024


def _charset_from_content_type(content_type: Optional[str]) -> Optional[str]:
    """Extract a charset label from a Content-Type header, if it declares one.

    An implausibly long header declares nothing; the page's own <meta> or
    the later candidates decide instead.
    """
    if not content_type or len(content_type) > MAX_CONTENT_TYPE_CHARS:
        return None
    # The email parser honours quoted-strings, so a ";" or "charset=" inside
    # another parameter's quoted value is not mistaken for this one, and the
    # first charset wins, as in browsers.
    header = _HeaderMessage()
    header["Content-Type"] = content_type
    charset = header.get_param("charset")
    if not isinstance(charset, str):  # absent, or an RFC 2231 tuple
        return None
    return charset.strip().strip("'") or None


def _safe_download(
    url: str,
    max_bytes: int,
    timeout: float = 10,
    *,
    allowed_content_types: Optional[frozenset[str]] = None,
    html_head_only: bool = False,
) -> tuple[bytes, str, Optional[str]]:
    """Fetch a URL through pinned public addresses.

    With `html_head_only`, only the page's head is read and returned (see
    _read_response_body); `max_bytes` then caps the head instead of the page.

    Returns the body, the final URL after any redirects, and the declared
    Content-Type header (or None) so callers can honour a server-declared
    charset when decoding text. With `allowed_content_types`, a response that
    declares any other media type is refused before its body is read; one that
    declares none is still accepted.
    """
    with _network_deadline(timeout, subject="External download") as deadline:
        visited: set[str] = set()
        current = url
        redirects_followed = 0
        while True:
            _remaining_download_time(
                deadline,
                f"fetching {_url_for_log(current)!r}",
            )
            if current in visited:
                raise ValueError(
                    f"Redirect loop detected at {_url_for_log(current)!r}"
                )
            visited.add(current)
            with _open_pinned_response(current, timeout) as resp:
                status_code = resp.status_code
                if status_code in REDIRECT_STATUS_CODES:
                    if redirects_followed >= MAX_REDIRECTS:
                        raise ValueError(
                            f"Too many redirects (> {MAX_REDIRECTS}) "
                            f"following {_url_for_log(url)!r}"
                        )
                    redirects_followed += 1
                    redirect_url = _redirect_target(resp, current)
                    # `current` was validated when it was fetched.
                    if (
                        urlparse(current).scheme.lower() == "https"
                        and _parse_url(redirect_url).scheme.lower() != "https"
                    ):
                        raise ValueError(
                            "Refusing HTTPS-to-HTTP redirect from "
                            f"{_url_for_log(current)!r} to "
                            f"{_url_for_log(redirect_url)!r}"
                        )
                    current = redirect_url
                    continue
                # Any other non-2xx (a 304 or 305, say) has no page in its
                # body either. No HTTPError is raised to chain: its message
                # would carry the unredacted URL.
                if not 200 <= status_code < 300:
                    raise requests.RequestException(
                        "External download returned HTTP "
                        f"{status_code} for {_url_for_log(current)!r}"
                    )
                media_type = (
                    resp.headers.get("Content-Type", "").split(";", 1)[0].strip().lower()
                )
                if (
                    allowed_content_types is not None
                    and media_type
                    and media_type not in allowed_content_types
                ):
                    raise ValueError(
                        f"Refusing {media_type[:MAX_ECHOED_HEADER_CHARS]!r} response from "
                        f"{_url_for_log(current)!r}"
                    )
                return (
                    _read_response_body(resp, max_bytes, html_head_only=html_head_only),
                    current,
                    resp.headers.get("Content-Type"),
                )


@contextmanager
def _open_api_response(
    method: str,
    url: str,
    *,
    timeout: float,
    operation: str,
    **request_kwargs: Any,
) -> Iterator[requests.Response]:
    """Open and close one streamed API response under a wall-clock deadline.

    The Session is used by this request alone, so callers may catch a timeout
    and keep going.
    """
    outer_deadline = _DOWNLOAD_DEADLINE.get()
    pool = _API_SESSION_POOL.get()
    origin = _session_origin(url)
    with _network_deadline(timeout, subject="API request") as deadline:
        session = pool.lease(origin) if pool is not None else _new_session()
        completed = False
        response: Optional[requests.Response] = None
        # Only its flag is used: marked handed off once an abandoned worker
        # owns `session`.
        connecting = _ResponseCleanup()
        cleanup: Optional[_ResponseCleanup] = None
        request_timeout = timeout

        def request() -> requests.Response:
            return session.request(
                method,
                url,
                timeout=request_timeout,
                allow_redirects=False,
                stream=True,
                **request_kwargs,
            )

        try:
            # An outer budget (e.g. all mention lookups) may leave less than
            # `timeout`; the socket must not outlive it in an abandoned worker.
            if outer_deadline is not None:
                request_timeout = min(
                    timeout, _remaining_download_time(deadline, operation)
                )
            response = _run_before_download_deadline(
                request,
                deadline,
                operation,
                dispose_abandoned=(
                    lambda late_response: _close_quietly(late_response, session)
                ),
                on_abandon=connecting.hand_off,
            )
            cleanup = _ResponseCleanup(response, session)
            cleanup_token = _ABANDONED_RESPONSE_CLEANUP.set(cleanup)
            try:
                yield response
            finally:
                _ABANDONED_RESPONSE_CLEANUP.reset(cleanup_token)
            completed = True
        finally:
            # If the request or a body read inside the caller's block was
            # abandoned (deadline or interrupt), its daemon worker still owns
            # `session` and `response`, so neither may be closed from here.
            # The context variables are restored by _network_deadline once these
            # closes are done, preserving the original unwind order.
            if not connecting.handed_off and not (
                cleanup is not None and cleanup.handed_off
            ):
                if response is not None:
                    # A fully read body has already returned its connection to
                    # the Session's pool; an unread one is closed, not reused.
                    _close_quietly(response)
                # Only a Session whose connection went back to it is worth
                # keeping: one whose body was left unread (a 400 the caller
                # only needed the status of) has just lost its connection, and
                # on top of the pool it would hide the warm ones beneath.
                if (
                    completed
                    and pool is not None
                    and getattr(response, "_content_consumed", False)
                ):
                    pool.give_back(origin, session)
                else:
                    _close_quietly(session)


class XrpcError(requests.HTTPError):
    """An XRPC call answered with an error status; keeps the server's error name."""

    def __init__(
        self,
        message: str,
        *,
        response: requests.Response,
        error_name: Optional[str],
        hint: Optional[str] = None,
    ):
        super().__init__(message, response=response)
        self.error_name = error_name
        # A remedy of this script's own, printed on a line of its own after
        # the message (which never contains a line break of its own).
        self.hint = hint


def _print_verbose_error_body(body: Any) -> None:
    """Print an error body for --verbose: compact, and bounded in size."""
    if body is None:  # cut off, timed out, or over the response cap
        print("<error body could not be read>", file=sys.stderr)
        return
    try:
        text = json.dumps(body)
    except (ValueError, RecursionError):  # nesting only json.loads accepted
        text = "<error body too deeply nested to print>"
    if len(text) > MAX_VERBOSE_ERROR_BODY_CHARS:
        omitted = len(text) - MAX_VERBOSE_ERROR_BODY_CHARS
        text = f"{text[:MAX_VERBOSE_ERROR_BODY_CHARS]}… ({omitted:,} more characters)"
    print(text, file=sys.stderr)


def _raise_for_xrpc(
    resp: requests.Response,
    method: str,
    *,
    label: Optional[str] = None,
    hints: Optional[Dict[str, str]] = None,
    verbose: bool = False,
) -> None:
    """Refuse a redirected or failed API response, reporting what the server said.

    A 3xx is never followed: the request may carry credentials or a blob.
    Errors are raised with the XRPC error name and message; `hints` maps
    lower-cased error names to a remedy, kept as the error's `hint`.
    """
    if 300 <= resp.status_code < 400:
        raise ValueError(f"Refusing redirect from {method} request")
    # Not resp.ok: that passes 1xx (e.g. 101) and 600-999 as success.
    if 200 <= resp.status_code < 300:
        return
    body = _error_response_body(resp, keep_raw=verbose)
    if verbose:
        _print_verbose_error_body(body)
    error_name = _api_error_name(body)
    hint = (hints or {}).get((error_name or "").lower())
    # _api_error_summary already sanitizes the server's text.
    raise XrpcError(
        f"{label or method} failed with HTTP {resp.status_code}"
        f"{_api_error_summary(body)}.",
        response=resp,
        error_name=error_name,
        hint=hint,
    )


def _decode_json(body: bytes, context: str) -> Any:
    # The byte cap does not bound nesting depth: a few tens of KB of brackets
    # exhaust the recursion limit, so that failure is reported like any other
    # malformed body. (JSONDecodeError and UnicodeDecodeError are ValueErrors.)
    try:
        return json.loads(body)
    except (ValueError, RecursionError) as exc:
        raise ValueError(f"{context} returned a non-JSON response") from exc


def _decode_json_object(body: bytes, context: str) -> Dict[str, Any]:
    data = _decode_json(body, context)
    if not isinstance(data, dict):
        raise ValueError(f"{context} returned an unexpected JSON shape")
    return data


def _json_object(resp: requests.Response, context: str) -> Dict[str, Any]:
    return _decode_json_object(
        _read_response_body(resp, MAX_API_RESPONSE_BYTES), context
    )


def _error_response_body(resp: requests.Response, *, keep_raw: bool = False) -> Any:
    """Best-effort body of an error response; never hides the status itself.

    A body that is not JSON is decoded as text only with `keep_raw` (--verbose
    prints it); nothing else reads it.
    """
    # The status line already arrived, so a body that cannot be read (cut
    # off, undecodable, timed out) must not turn a definite 4xx into a
    # transport error: createRecord would retry a rejected post, and
    # getRecord's 404 would no longer mean "not found". A timed-out read
    # marks its response handed off, so the opener will not close it.
    try:
        body = _read_response_body(resp, MAX_API_RESPONSE_BYTES)
    except (ValueError, requests.RequestException):
        return None
    try:
        return _decode_json(body, "error response")
    except ValueError:
        return {"raw": body.decode("UTF-8", errors="replace")} if keep_raw else None


def _api_error_name(body: Any) -> Optional[str]:
    if not isinstance(body, dict):
        return None
    error_name = body.get("error")
    return error_name if isinstance(error_name, str) and error_name else None


def _api_error_summary(body: Any) -> str:
    """Render an XRPC error body as a short ` (Name: message)` suffix."""
    error_name = _api_error_name(body)
    if error_name is None:
        return ""
    safe_name = _terminal_safe(_grapheme_safe_prefix(error_name, MAX_ERROR_NAME_CHARS))
    message = body.get("message")  # a dict: _api_error_name found an error name
    message = message.strip() if isinstance(message, str) else ""
    if message:
        detail = _terminal_safe(_grapheme_safe_prefix(message, MAX_ERROR_DETAIL_CHARS))
        return f" ({safe_name}: {detail})"
    return f" ({safe_name})"


# Login failures are the most common thing a first-time user hits, and the raw
# XRPC error name rarely explains the fix.
LOGIN_ERROR_HINTS = {
    "authfactortokenrequired": (
        "This account has email two-factor authentication enabled, which the "
        "account password cannot bypass. Use an APP password instead, created "
        "at https://bsky.app/settings/app-passwords."
    ),
    "authenticationrequired": (
        "Check ATP_AUTH_HANDLE, and that ATP_AUTH_PASSWORD is a current, "
        "unrevoked app password from https://bsky.app/settings/app-passwords."
    ),
    "accounttakedown": "This account has been taken down by its host.",
    "ratelimitexceeded": "The PDS is rate limiting sign-ins; wait and retry.",
}


def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    with _open_api_response(
        "POST",
        _api_url(pds_url, "com.atproto.server.createSession"),
        timeout=30,
        operation="waiting for createSession response headers",
        json={"identifier": handle, "password": password},
    ) as resp:
        # Surface what the PDS actually said. Without this an expired app
        # password and an account needing a 2FA code look identical.
        _raise_for_xrpc(resp, "createSession", label="Login", hints=LOGIN_ERROR_HINTS)
        data = _json_object(resp, "createSession")
    access_token, did = data.get("accessJwt"), data.get("did")
    # The tokens become Authorization headers and the DID the repo that
    # createRecord writes to, which its answer and any recovery getRecord
    # lookup are checked against, so a blank or malformed token or a non-DID
    # value is refused here rather than sent on or echoed. None is quoted: two
    # are credentials. A missing refresh token only means no logout.
    if not _is_bearer_token(access_token):
        raise ValueError("createSession response has a missing or malformed accessJwt")
    if "refreshJwt" in data and not _is_bearer_token(data["refreshJwt"]):
        raise ValueError("createSession response has a malformed refreshJwt")
    if not _is_did(did):
        raise ValueError("createSession response has a missing or malformed did")
    return data


def bsky_logout_session(pds_url: str, refresh_token: str) -> None:
    """Revoke the login's refresh token so it does not outlive this run.

    Best effort: a failure only means the session expires on the PDS's own
    schedule, which is exactly what happened before this call existed.
    """
    if not _is_bearer_token(refresh_token):
        _warn("warning: could not end the login session (malformed refresh token); "
              "it will expire on its own.")
        return
    try:
        with _open_api_response(
            "POST",
            _api_url(pds_url, "com.atproto.server.deleteSession"),
            timeout=10,
            operation="waiting for deleteSession response headers",
            headers={"Authorization": "Bearer " + refresh_token},
        ) as resp:
            _raise_for_xrpc(resp, "deleteSession")
    except (requests.RequestException, ValueError) as exc:
        # The request carries the refresh token in a header, and errors about
        # header values (Requests' InvalidHeader, http.client's ValueError)
        # quote them: only the server's own (already sanitized) explanation
        # and transport errors are shown.
        quotes_headers = isinstance(exc, (requests.exceptions.InvalidHeader, ValueError))
        detail = type(exc).__name__ if quotes_headers else (
            f"{type(exc).__name__}: {_terminal_safe(exc)}"
        )
        _warn(f"warning: could not end the login session ({detail}); it will expire on its own.")


def _warn(message: str) -> None:
    """Print one warning line from any thread.

    Post assembly runs its stages concurrently; print() issues the text and
    the newline as separate writes, so two warnings could interleave.
    """
    sys.stderr.write(message + "\n")


def _run_concurrently(
    tasks: List[Optional[Callable[[], Any]]],
    *,
    max_workers: Optional[int] = None,
) -> List[Any]:
    """Run independent network tasks at once; return their results in order.

    A None task has no work and yields None; when at most one task has work,
    it runs in the caller's thread.

    Each task runs inside its own copy of the caller's context, so deadlines
    and labels set by the caller still apply, on at most `max_workers` daemon
    threads (one per task by default). Every task is waited for, then the
    first failure in task order is raised, so the error reported does not
    depend on which task happened to fail first. Daemon threads (not a
    ThreadPoolExecutor, whose exit hook joins its workers) keep Ctrl-C prompt:
    an interrupted wait leaves the tasks to their own deadlines and never
    delays exit.
    """
    active = [index for index, task in enumerate(tasks) if task is not None]
    if len(active) <= 1:
        # Nothing to overlap: run it here, without a thread.
        return [None if task is None else task() for task in tasks]
    results: List[Any] = [None] * len(tasks)
    failures: List[Optional[BaseException]] = [None] * len(tasks)
    contexts = {index: copy_context() for index in active}
    next_index = iter(active)
    index_lock = threading.Lock()

    def run() -> None:
        while True:
            with index_lock:
                index = next(next_index, None)
            if index is None:
                return
            try:
                results[index] = contexts[index].run(tasks[index])
            except BaseException as exc:  # re-raised in the caller's thread
                failures[index] = exc

    workers = len(active) if max_workers is None else max(1, min(max_workers, len(active)))
    threads = []
    for number in range(workers):
        thread = threading.Thread(target=run, name=f"bsky-task-{number}", daemon=True)
        thread.start()
        threads.append(thread)
    for thread in threads:
        thread.join()
    for failure in failures:
        if failure is not None:
            raise failure
    return results


def _byte_offsets(text: str) -> List[int]:
    offsets = [0]
    for character in text:
        offsets.append(offsets[-1] + len(character.encode("UTF-8")))
    return offsets


# Invisible format characters that glue the text on both sides into one word.
WORD_GLUE_CHARACTERS = frozenset("\u00ad\u200c\u200d\u2060")
# A zero-width space is a break opportunity: it ends a word as a space would.
ZERO_WIDTH_SPACE = "\u200b"


def _visible_neighbour(text: str, index: int, step: int) -> Optional[str]:
    """The character at `index`, looking past invisible marks in direction `step`.

    BiDi marks, a stray BOM and other format characters are transparent (an
    RTL post routinely puts U+200E before a link), so the character beyond
    them decides; a joiner is returned itself, and a zero-width space or the
    end of the text gives None, a boundary.
    """
    while 0 <= index < len(text):
        character = text[index]
        if character in WORD_GLUE_CHARACTERS:
            return character
        if character == ZERO_WIDTH_SPACE:
            return None
        if unicodedata.category(character) != "Cf":
            return character
        index += step
    return None


def _is_wordlike(character: Optional[str]) -> bool:
    if character is None:
        return False
    category = unicodedata.category(character)
    return (
        category[0] in ("L", "M", "N")
        or category == "Pc"
        or character in WORD_GLUE_CHARACTERS
    )


def _has_wordlike_prefix(text: str, start: int) -> bool:
    return _is_wordlike(_visible_neighbour(text, start - 1, -1))


def _has_wordlike_suffix(text: str, end: int) -> bool:
    """Whether the candidate ending at `end` runs on into a word or address."""
    following = _visible_neighbour(text, end, 1)
    return following == "@" or _is_wordlike(following)


def parse_mentions(text: str, byte_offsets: Optional[List[int]] = None) -> List[Dict]:
    if byte_offsets is None:
        byte_offsets = _byte_offsets(text)
    mentions: List[Dict] = []
    for match in MENTION_REGEX.finditer(text):
        start, end = match.span(1)
        if _has_wordlike_prefix(text, start) or _has_wordlike_suffix(text, end):
            continue
        # Only sentence punctuation is trimmed; anything else invalid in the
        # candidate rejects it whole rather than shortening it to another handle.
        handle = match.group(1)[1:].rstrip(".")
        end = start + 1 + len(handle)
        if len(handle) > MAX_HOSTNAME_LENGTH or HANDLE_REGEX.fullmatch(handle) is None:
            continue
        mentions.append(
            {
                "start": byte_offsets[start],
                "end": byte_offsets[end],
                "handle": handle,
            }
        )
    return mentions


def _is_trailing_url_punctuation(character: str) -> bool:
    if character in TRAILING_URL_PUNCTUATION:
        return True
    # Typographic quotes (“…”, «…», ‘…’) and non-ASCII closing brackets
    # (）」】) wrap a link in prose; they are never its last character.
    category = unicodedata.category(character)
    return not character.isascii() and category in ("Pi", "Pf", "Pe")


def _trim_url_match(url: str) -> str:
    # Nothing to trim unless the URL ends in punctuation or a closing bracket.
    if not url or not (
        _is_trailing_url_punctuation(url[-1]) or url[-1] in URL_CLOSING_TO_OPENING
    ):
        return url
    # A closing bracket stays only when it closes a bracket opened inside the
    # URL. Matching is left to right, so whether a closer is matched never
    # depends on what follows it, and trimming the tail cannot change it.
    depth = dict.fromkeys(URL_CLOSING_TO_OPENING.values(), 0)
    unmatched_closers = set()
    for index, character in enumerate(url):
        if character in depth:
            depth[character] += 1
        elif (opening := URL_CLOSING_TO_OPENING.get(character)) is not None:
            if depth[opening]:
                depth[opening] -= 1
            else:
                unmatched_closers.add(index)
    end = len(url)
    while end and (
        _is_trailing_url_punctuation(url[end - 1]) or end - 1 in unmatched_closers
    ):
        end -= 1
    return url[:end]


def parse_urls(text: str) -> List[Dict]:
    return _scan_urls(text, _byte_offsets(text))[0]


def _scan_urls(
    text: str,
    byte_offsets: List[int],
) -> tuple[List[Dict], List[tuple[int, int]]]:
    """Link spans, and the byte spans of every URL-shaped run, in one pass.

    Nothing inside a URL-shaped run, valid link or not, is a mention or tag:
    a rejected URL such as "https://example.com/50%/@alice.bsky.social" must
    stay plain text, not lose its link and gain a mention of the handle in
    its path. Every link lies inside its own run.
    """
    spans: List[Dict] = []
    url_like: List[tuple[int, int]] = []
    for match in URL_REGEX.finditer(text):
        url_like.append((byte_offsets[match.start(1)], byte_offsets[match.end(1)]))
        if _has_wordlike_prefix(text, match.start(1)):
            continue
        url = _trim_url_match(match.group(1))
        try:
            _parse_url(url)
        except ValueError:
            continue
        start = match.start(1)
        spans.append(
            {"start": byte_offsets[start], "end": byte_offsets[start + len(url)], "url": url}
        )
    return spans, url_like


def _strip_tag_trailing_punctuation(tag: str) -> str:
    end = len(tag)
    while end and unicodedata.category(tag[end - 1]).startswith("P"):
        end -= 1
    return tag[:end]


def _valid_tag_value(tag: str) -> bool:
    if (
        not tag
        # U+FE0F only as the first character, as in the official composer
        # ("#\u2764\ufe0flove" stays a tag).
        or tag[0] == "\ufe0f"
        or len(tag.encode("UTF-8")) > MAX_TAG_BYTES
        # HASHTAG_REGEX already ends a tag at U+20E2 and at the format and
        # space characters (soft hyphen, word joiner, zero-width ones) that
        # the categories catch; this keeps direct callers to the same rule.
        or "\u20e2" in tag
        or any(
            unicodedata.category(character).startswith(("C", "Z"))
            for character in tag
        )
    ):
        return False
    # Only nonspacing/enclosing marks can safely be discounted here. Spacing
    # marks (Mc) include exceptions such as U+102B that start new graphemes.
    # With controls/separators excluded above, Mn/Me join the preceding cluster;
    # a leading run of marks still counts once. Other multi-codepoint clusters
    # (flags, Hangul, emoji modifiers) remain conservatively overcounted.
    grapheme_upper_bound = sum(
        unicodedata.category(character) not in ("Mn", "Me")
        for character in tag
    ) + int(unicodedata.category(tag[0]) in ("Mn", "Me"))
    if grapheme_upper_bound > MAX_TAG_GRAPHEMES:
        return False
    # Only ASCII 0-9 count as digits here, as in the official composer's regex:
    # str.isdigit() would also reject tags such as "#²".
    return any(
        not "0" <= character <= "9"
        and not unicodedata.category(character).startswith("P")
        for character in tag
    )


def parse_hashtags(text: str, byte_offsets: Optional[List[int]] = None) -> List[Dict]:
    if byte_offsets is None:
        byte_offsets = _byte_offsets(text)
    spans: List[Dict] = []
    for match in HASHTAG_REGEX.finditer(text):
        tag = _strip_tag_trailing_punctuation(match.group(3))
        if not _valid_tag_value(tag):
            continue
        start_character = match.start(2)
        end_character = start_character + len(match.group(2)) + len(tag)
        spans.append(
            {
                "start": byte_offsets[start_character],
                "end": byte_offsets[end_character],
                "tag": tag,
            }
        )

    for match in CASHTAG_REGEX.finditer(text):
        # Two intentional asymmetries with hashtags (whose byte span covers the
        # leading '#' but whose tag value omits it, per the Bluesky convention):
        # cashtags keep the leading '$' in both the span and the stored value,
        # and the ticker is upper-cased to match the official composer, so
        # "$tsla" is stored as "$TSLA". The span still covers the text as
        # typed -- the regex is ASCII-only, so case folding cannot change the
        # byte length and the offsets stay exact.
        ticker = match.group(2).upper()
        start_character = match.start(2) - 1
        end_character = match.end(2)
        spans.append(
            {
                "start": byte_offsets[start_character],
                "end": byte_offsets[end_character],
                "tag": "$" + ticker,
            }
        )
    # A cashtag can sit inside a hashtag ("#foo($TSLA)"); facets must never
    # overlap, so the earlier span wins (the hashtag, on a tie: stable sort).
    occupied: List[tuple[int, int]] = []
    return [
        span
        for span in sorted(spans, key=lambda span: span["start"])
        if _reserve_span(span, occupied)
    ]


def make_facet(match: Dict, feature: Dict) -> Dict:
    return {
        "index": {"byteStart": match["start"], "byteEnd": match["end"]},
        "features": [feature],
    }


def _resolve_handle(pds_url: str, handle: str) -> Optional[str]:
    try:
        with _open_api_response(
            "GET",
            _api_url(pds_url, "com.atproto.identity.resolveHandle"),
            timeout=10,
            operation="waiting for resolveHandle response headers",
            params={"handle": handle},
        ) as resp:
            # A redirect is never followed, and an error means "unresolved".
            if not 200 <= resp.status_code < 300:
                return None
            data = _json_object(resp, "resolveHandle")
    except (requests.RequestException, ValueError):
        # An unresolvable handle stays plain text, timeouts included.
        return None
    did = data.get("did")
    return did if _is_did(did) else None


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


def _reserve_span(span: Dict, occupied: List[tuple[int, int]]) -> bool:
    start, end = span["start"], span["end"]
    if start >= end:
        return False
    index, overlaps = _span_insert_index(start, end, occupied)
    if overlaps:
        return False
    occupied.insert(index, (start, end))
    return True


def _resolve_handles(pds_url: str, handles: List[str]) -> Dict[str, Optional[str]]:
    """Resolve distinct handles concurrently under one overall deadline.

    Each task runs in a copy of the caller's context, so every lookup inherits
    the shared deadline; _network_deadline never lets an inner call outlive
    it. Once it passes, lookups still pending fail fast and resolve to None.
    """
    if not handles:
        return {}
    with _network_deadline(
        MAX_MENTION_RESOLUTION_SECONDS,
        subject="Mention resolution",
    ):
        # Daemon threads, as for every concurrent stage: Ctrl-C during the
        # lookups exits at once instead of waiting out their timeouts.
        dids = _run_concurrently(
            [lambda handle=handle: _resolve_handle(pds_url, handle) for handle in handles],
            max_workers=MENTION_RESOLUTION_WORKERS,
        )
    return dict(zip(handles, dids))


def parse_facets(pds_url: str, text: str) -> List[Dict]:
    byte_offsets = _byte_offsets(text)
    links, url_like = _scan_urls(text, byte_offsets)
    # Links need no reservation: no two URL-shaped runs overlap, and mentions
    # and tags are kept out of every run.
    facets = [
        make_facet(u, {"$type": "app.bsky.richtext.facet#link", "uri": u["url"]})
        for u in links
    ]
    occupied: List[tuple[int, int]] = []

    def outside_urls(span: Dict) -> bool:
        return not _span_insert_index(span["start"], span["end"], url_like)[1]

    # Handles are case-insensitive, so each distinct one is looked up once.
    mentions = [m for m in parse_mentions(text, byte_offsets) if outside_urls(m)]
    resolved_handles = _resolve_handles(
        pds_url,
        list(dict.fromkeys(m["handle"].lower() for m in mentions)),
    )
    unresolved = [
        handle for handle, did in resolved_handles.items() if did is None
    ]
    if unresolved:
        shown = ", ".join(f"@{handle}" for handle in unresolved[:5])
        more = f" and {len(unresolved) - 5} more" if len(unresolved) > 5 else ""
        _warn(
            f"warning: could not resolve {shown}{more}; "
            "posting as plain text without a mention link."
        )
    for m in mentions:
        did = resolved_handles[m["handle"].lower()]
        if did and _reserve_span(m, occupied):
            facets.append(
                make_facet(
                    m,
                    {"$type": "app.bsky.richtext.facet#mention", "did": did},
                )
            )
    facets.extend(
        make_facet(h, {"$type": "app.bsky.richtext.facet#tag", "tag": h["tag"]})
        for h in parse_hashtags(text, byte_offsets)
        if outside_urls(h) and _reserve_span(h, occupied)
    )
    return sorted(facets, key=lambda facet: facet["index"]["byteStart"])


def _validate_at_identifier(identifier: str) -> None:
    if _is_did(identifier):
        return
    if (
        identifier == identifier.lower()
        and len(identifier) <= MAX_HOSTNAME_LENGTH
        and HANDLE_REGEX.fullmatch(identifier)
    ):
        return
    raise ValueError("AT URI authority must be a normalized DID or handle")


def _validate_nsid(nsid: str) -> None:
    authority, separator, _name = nsid.rpartition(".")
    if (
        not separator
        or len(nsid) > 317
        or len(authority) > MAX_HOSTNAME_LENGTH
        or NSID_REGEX.fullmatch(nsid) is None
    ):
        raise ValueError(f"Invalid NSID in AT URI: {nsid!r}")


def _validate_record_key(record_key: str) -> None:
    if (
        record_key in (".", "..")
        or RECORD_KEY_REGEX.fullmatch(record_key) is None
    ):
        raise ValueError(f"Invalid record key in AT URI: {record_key!r}")


def _parse_at_uri(parsed: ParseResult, uri: str) -> Dict:
    # parse_uri has capped the length at MAX_URL_LENGTH (8 KiB of ASCII).
    if not uri.isascii():
        raise ValueError("AT URI must contain only ASCII characters")
    if not uri.startswith("at://"):
        raise ValueError("AT URI must use the normalized lowercase at:// scheme")
    if parsed.query or parsed.fragment or parsed.params or not parsed.netloc:
        raise ValueError(f"Invalid AT URI format: {uri}")
    path_parts = parsed.path.split("/")
    if len(path_parts) != 3 or path_parts[0] or not all(path_parts[1:]):
        raise ValueError(f"Invalid AT URI path: {uri}")
    repo, collection, rkey = parsed.netloc, path_parts[1], path_parts[2]
    _validate_at_identifier(repo)
    _validate_nsid(collection)
    _validate_record_key(rkey)
    # urlparse drops an empty "?" or "#" (query and fragment both read ""), and
    # record_ref passes the URI on verbatim into replies and quote embeds, so
    # only the exact canonical spelling of the parsed parts is accepted.
    if uri != f"at://{repo}/{collection}/{rkey}":
        raise ValueError(f"Invalid AT URI format: {uri}")
    return {"repo": repo, "collection": collection, "rkey": rkey}


def _parse_bsky_app_uri(parsed: ParseResult, uri: str) -> Dict:
    try:
        port = parsed.port
    except ValueError as exc:
        raise ValueError(f"Invalid Bluesky URL format: {_url_for_log(uri)}") from exc
    # An explicit ":443" is the https default and appears in URLs copied out of
    # some clients; any other port would change where the record is looked up.
    # Empty userinfo and an empty ";" parameter parse as "", so test the raw
    # forms: neither "@" nor ";" belongs in a bsky.app record link.
    if (
        "@" in parsed.netloc
        or port not in (None, 443)
        or ";" in uri
        or "//" in parsed.path
        or not parsed.path
        or parsed.path.endswith("/")
    ):
        raise ValueError(f"Invalid Bluesky URL format: {_url_for_log(uri)}")
    parts = tuple(part for part in parsed.path.split("/") if part)
    if len(parts) != 4 or parts[0] != "profile":
        raise ValueError(f"Invalid Bluesky URL format: {_url_for_log(uri)}")
    _, repo, collection, rkey = parts
    # Handles are case-insensitive and links copied by hand may be mixed-case;
    # DIDs are case-sensitive and must be left exactly as written; a "DID:"
    # prefix in any case is left alone too, so it is refused (DIDs use a
    # lowercase scheme) rather than folded into some other, lowercased DID.
    # Non-ASCII is refused before lower(), which would otherwise fold some of
    # it into ASCII ("\u212aate" -> "kate") and name a different account.
    if not repo.isascii():
        raise ValueError(f"Invalid Bluesky URL format: {_url_for_log(uri)}")
    if not repo.lower().startswith("did:"):
        repo = repo.lower()
    # Unlike an AT URI, a web link's path is not quoted back: the collection
    # and record key segments are only named, never echoed.
    mapped_collection = BSKY_APP_COLLECTIONS.get(collection)
    if mapped_collection is None:
        raise ValueError(
            "Unsupported Bluesky record path (expected post, lists or feed): "
            f"{_url_for_log(uri)}"
        )
    _validate_at_identifier(repo)
    try:
        _validate_record_key(rkey)
    except ValueError:
        raise ValueError(
            f"Invalid record key in Bluesky URL: {_url_for_log(uri)}"
        ) from None
    return {
        "repo": repo,
        "collection": mapped_collection,
        "rkey": rkey,
    }


def parse_uri(uri: str) -> Dict:
    if (
        not isinstance(uri, str)
        or not uri
        or uri != uri.strip()
        or any(ord(character) < 0x20 or ord(character) == 0x7F for character in uri)
    ):
        raise ValueError("Invalid URI format")
    # A reply's root URI comes from the parent post, which anyone could have
    # written; none legitimately approaches this.
    if len(uri) > MAX_URL_LENGTH:
        raise ValueError(f"URI exceeds {MAX_URL_LENGTH} characters")
    try:
        parsed = urlparse(uri)
    except ValueError as exc:
        raise ValueError(f"Invalid URI format: {_url_for_log(uri)}") from exc
    if parsed.scheme.lower() == "at":
        return _parse_at_uri(parsed, uri)
    if parsed.scheme.lower() == "https" and parsed.hostname == "bsky.app":
        return _parse_bsky_app_uri(parsed, uri)
    raise ValueError(f"Unhandled URI format: {_url_for_log(uri)}")


def _require_did_at_uri(uri: Any, context: str) -> Dict:
    """Parse a record URI that must be at://<DID>/<collection>/<rkey>.

    A handle can be re-pointed at another account at any time, and a
    bsky.app link names one, so neither may stand in for a record in a stored
    reference or in a lookup whose answer is checked against the request.
    """
    parts = parse_uri(uri)
    if not uri.startswith("at://") or not _is_did(parts["repo"]):
        raise ValueError(f"{context} is not an at:// URI that names its repository by DID")
    return parts


def _did_addressed_uri(pds_url: str, uri: str) -> str:
    """The at://<DID>/... form of a reply or quote target given on the command line.

    A handle is resolved by the user's own PDS, not by the record service
    (which is not authenticated and can be any host): the record service is
    then asked for, and checked against, that exact DID, so it cannot answer
    with another account's post of the same record key.
    """
    parts = parse_uri(uri)
    repo = parts["repo"]
    if not _is_did(repo):
        did = _resolve_handle(pds_url, repo)
        if did is None:
            raise ValueError(f"Could not resolve @{_terminal_safe(repo)} through the PDS")
        repo = did
    return f"at://{repo}/{parts['collection']}/{parts['rkey']}"


def _validated_record_response(data: Dict, requested: Dict, context: str) -> Dict:
    """Return {uri, cid} from a response that must name the requested record.

    Every request names its repository by DID, and record_ref accepts only
    the canonical spelling of a URI, so the strings match exactly or the
    answer names another record.
    """
    if not isinstance(data.get("uri"), str) or not isinstance(data.get("cid"), str):
        raise ValueError(f"{context} response is missing uri or cid")
    try:
        ref = record_ref(data)
    except ValueError as exc:
        raise ValueError(
            f"{context} response contains an invalid record reference"
        ) from exc
    if ref["uri"] != (
        f"at://{requested['repo']}/{requested['collection']}/{requested['rkey']}"
    ):
        raise ValueError(f"{context} returned a different record than was requested")
    return ref


def get_record(record_service_url: str, uri: str) -> Dict:
    requested = _require_did_at_uri(uri, "getRecord target")
    with _open_api_response(
        "GET",
        _api_url(record_service_url, "com.atproto.repo.getRecord"),
        timeout=10,
        operation="waiting for getRecord response headers",
        params=requested,
    ) as resp:
        _raise_for_xrpc(resp, "getRecord")
        data = _json_object(resp, "getRecord")
    _validated_record_response(data, requested, "getRecord")
    return data


def _base32_cid(raw_cid: bytes) -> str:
    """The multibase base32 (lower-case, unpadded) string form of a CID."""
    return "b" + base64.b32encode(raw_cid).decode("ASCII").lower().rstrip("=")


def _is_valid_record_cid(cid: str) -> bool:
    # A 36-byte CIDv1 is 58 base32 characters, which take 6 "=" of padding.
    if re.fullmatch(r"b[a-z2-7]{58}", cid) is None:
        return False
    # Never a binascii.Error: the regex admits only base32 letters.
    raw_cid = base64.b32decode(cid[1:].upper() + "======")
    # The round trip refuses non-zero trailing bits, which decoding ignores.
    return raw_cid[:4] == b"\x01\x71\x12\x20" and _base32_cid(raw_cid) == cid


def _checked_record_cid(cid: str) -> str:
    if not _is_valid_record_cid(cid):
        raise ValueError("Record CID is not a blessed DAG-CBOR SHA-256 CID")
    return cid


def record_ref(record: Dict) -> Dict:
    uri = record.get("uri")
    cid = record.get("cid")
    if not isinstance(uri, str) or not isinstance(cid, str):
        raise ValueError("Record is missing uri or cid")
    _require_did_at_uri(uri, "Record URI")
    return {"uri": uri, "cid": _checked_record_cid(cid)}


def _require_post_uri(uri: str, context: str) -> None:
    parts = parse_uri(uri)
    if parts["collection"] != "app.bsky.feed.post":
        raise ValueError(f"{context} must reference an app.bsky.feed.post record")


def _require_post_record(record: Dict, context: str) -> Dict:
    """The value of a record get_record returned for an app.bsky.feed.post URI.

    get_record has already checked that the answer names exactly the URI
    asked for, so only the record's own $type is left to check.
    """
    value = record.get("value")
    if not isinstance(value, dict) or value.get("$type") != "app.bsky.feed.post":
        raise ValueError(f"{context} is not an app.bsky.feed.post record")
    return value


def _reply_root_ref(record_service_url: str, parent_reply: Dict) -> Dict:
    root_ref = parent_reply.get("root")
    if not isinstance(root_ref, dict) or not isinstance(root_ref.get("uri"), str):
        raise ValueError("Parent reply reference is missing root.uri")
    # Taken from the parent post's own content, which anyone could have written.
    uri = root_ref["uri"]
    if _require_did_at_uri(uri, "Reply root")["collection"] != "app.bsky.feed.post":
        raise ValueError("Reply root must reference an app.bsky.feed.post record")
    cid = root_ref.get("cid")
    if not isinstance(cid, str):
        # get_record checks that the answer names exactly this URI.
        root_record = get_record(record_service_url, uri)
        _require_post_record(root_record, "Reply root")
        cid = root_record["cid"]
    return {"uri": uri, "cid": _checked_record_cid(cid)}


def get_reply_refs(record_service_url: str, parent_uri: str) -> Dict:
    _require_post_uri(parent_uri, "Reply parent")
    parent = get_record(record_service_url, parent_uri)
    value = _require_post_record(parent, "Reply parent")
    parent_ref = record_ref(parent)
    parent_reply = value.get("reply")
    if parent_reply is None:
        root = parent_ref
    elif isinstance(parent_reply, dict):
        root = _reply_root_ref(record_service_url, parent_reply)
    else:
        raise ValueError("Parent reply reference has an unexpected shape")
    return {"root": root, "parent": parent_ref}


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


def _preflight_webp_dimensions(img_bytes: bytes) -> None:
    """Check the RIFF canvas before libwebp can allocate animation buffers."""
    if len(img_bytes) < 20:
        raise ValueError("Truncated WebP container")
    container_end = int.from_bytes(img_bytes[4:8], "little") + 8
    if container_end < 20 or container_end > len(img_bytes):
        raise ValueError("Invalid WebP container length")
    position = 12
    first_chunk = True
    # One unpack per chunk header, without slicing out its two fields.
    read_chunk_header = struct.Struct("<4sI").unpack_from
    while position < container_end:
        if container_end - position < 8:
            raise ValueError("Truncated WebP chunk header")
        chunk_type, chunk_size = read_chunk_header(img_bytes, position)
        payload = position + 8
        chunk_end = payload + chunk_size
        next_position = chunk_end + (chunk_size & 1)
        if next_position > container_end:
            raise ValueError("Truncated WebP chunk")
        if first_chunk:
            if chunk_type == b"VP8X" and chunk_size == 10:
                width = 1 + int.from_bytes(img_bytes[payload + 4:payload + 7], "little")
                height = 1 + int.from_bytes(img_bytes[payload + 7:payload + 10], "little")
            elif chunk_type == b"VP8 " and chunk_size >= 10:
                if img_bytes[payload + 3:payload + 6] != b"\x9d\x01\x2a":
                    raise ValueError("Invalid WebP frame header")
                width = int.from_bytes(img_bytes[payload + 6:payload + 8], "little") & 0x3FFF
                height = int.from_bytes(img_bytes[payload + 8:payload + 10], "little") & 0x3FFF
            elif chunk_type == b"VP8L" and chunk_size >= 5 and img_bytes[payload] == 0x2F:
                packed = int.from_bytes(img_bytes[payload + 1:payload + 5], "little")
                width = 1 + (packed & 0x3FFF)
                height = 1 + ((packed >> 14) & 0x3FFF)
            else:
                raise ValueError("WebP container has no valid initial image header")
            _validate_image_dimensions(width, height)
            first_chunk = False
        elif chunk_type == b"VP8X":
            raise ValueError("WebP container has a misplaced canvas header")
        position = next_position


def _gif_image_count_exceeds(img_bytes: bytes, limit: int) -> bool:
    """Whether the GIF's block structure holds more than `limit` images.

    Pillow's n_frames walks every block in the file before a frame limit can
    apply; this walk stops at the first image past the limit. It follows the
    GIF block grammar and gives up (False) at anything else, such as a
    truncated block or an unknown introducer, leaving Pillow's count the
    authority for those files.
    """
    end = len(img_bytes)
    if end < 13:
        return False
    flags = img_bytes[10]
    position = 13 + (3 << ((flags & 7) + 1) if flags & 0x80 else 0)
    images = 0
    while position < end:
        introducer = img_bytes[position]
        if introducer == 0x21:  # extension: label, then data sub-blocks
            position += 2
        elif introducer == 0x2C:  # image descriptor, local palette, LZW size
            if position + 10 > end:
                return False
            flags = img_bytes[position + 9]
            position += 11 + (3 << ((flags & 7) + 1) if flags & 0x80 else 0)
        else:  # the trailer, or a structure this walk does not follow
            return False
        while True:  # data sub-blocks, up to the zero-length terminator
            if position >= end:
                return False
            size = img_bytes[position]
            position += 1 + size
            if size == 0:
                break
        if introducer == 0x2C:
            images += 1
            if images > limit:
                return True
    return False


def _preflight_image(img_bytes: bytes) -> str:
    if not img_bytes or len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
        raise ValueError(f"Image must contain 1 to {MAX_IMAGE_SIZE_BYTES:,} bytes")
    if img_bytes.startswith(b"\xff\xd8\xff"):
        return "JPEG"
    if img_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
        return "PNG"
    if img_bytes.startswith((b"GIF87a", b"GIF89a")):
        if _gif_image_count_exceeds(img_bytes, MAX_IMAGE_FRAMES):
            raise ValueError(f"Image exceeds the {MAX_IMAGE_FRAMES}-frame limit")
        return "GIF"
    if img_bytes[:4] == b"RIFF" and img_bytes[8:12] == b"WEBP":
        _preflight_webp_dimensions(img_bytes)
        return "WEBP"
    raise ValueError("Unsupported or invalid image format; expected JPEG, PNG, GIF, or WebP")


def _inspect_image_decoded(img_bytes: bytes) -> Dict[str, Any]:
    """Run only in the bounded worker; never rewrite the supplied image."""
    from PIL import Image, ImageFile

    # Pillow's own bomb check (89.5M pixels by default) also guards the
    # canvas a GIF frame grows to inside seek(), which is allocated before
    # the dimension checks below see it. At this script's limit, a 72-byte
    # GIF can no longer make seek(1) allocate a 16385x4000 canvas.
    Image.MAX_IMAGE_PIXELS = MAX_IMAGE_PIXELS
    image_format = _preflight_image(img_bytes)
    importlib.import_module(IMAGE_FORMAT_PLUGINS[image_format])
    if image_format not in Image.OPEN:
        raise ValueError(f"This Pillow build cannot decode {image_format} images")
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        img = Image.open(io.BytesIO(img_bytes), formats=[image_format])
        if type(img).verify is not ImageFile.ImageFile.verify:
            # A plugin's own verify() checks what decoding skips (PNG chunk
            # CRCs) and uses up the file, so a fresh open does the full decode.
            # The inherited verify() checks nothing, so JPEG, GIF and WebP are
            # opened once rather than building their native decoders twice.
            with img:
                _validate_image_dimensions(*img.size)
                img.verify()
            # Some plugins retain native canvases until the image object is freed.
            del img
            img = Image.open(io.BytesIO(img_bytes), formats=[image_format])
        with img:
            width, height = img.size
            _validate_image_dimensions(width, height)
            frame_count = getattr(img, "n_frames", 1)
            if frame_count > MAX_IMAGE_FRAMES:
                raise ValueError(f"Image exceeds the {MAX_IMAGE_FRAMES}-frame limit")
            # No frame is smaller than the opening canvas (Pillow only ever
            # grows a GIF canvas to fit a frame), so this lower bound refuses
            # an oversized animation before any frame is decoded. The loop
            # below still counts each frame's real size.
            if width * height * frame_count > MAX_IMAGE_DECODED_PIXELS:
                raise ValueError("Image exceeds the total decoded-pixel limit")
            decoded_pixels = 0
            orientation = 1
            for frame in range(frame_count):
                img.seek(frame)
                _validate_image_dimensions(*img.size)
                decoded_pixels += img.width * img.height
                if decoded_pixels > MAX_IMAGE_DECODED_PIXELS:
                    raise ValueError("Image exceeds the total decoded-pixel limit")
                img.load()
                if frame == 0:
                    orientation = img.getexif().get(274, 1)
    # EXIF rotations/transpositions change display dimensions. The uploaded
    # bytes remain untouched so ICC payloads and JPEG coefficients survive.
    if orientation in (5, 6, 7, 8):
        width, height = height, width
    return {"width": width, "height": height, "mimetype": IMAGE_FORMAT_MIMETYPES[image_format]}


# Each image in a worker batch is framed by a 4-byte big-endian length.
def _encode_image_batch(images: List[bytes]) -> bytes:
    # One join over the flat parts: no per-image header+image copy first.
    parts: List[bytes] = []
    for image in images:
        parts += (len(image).to_bytes(4, "big"), image)
    return b"".join(parts)


def _read_image_batch(stream: Any) -> Iterator[bytes]:
    """Yield the batch's images from `stream` one at a time.

    The worker holds only the image being decoded, not the whole batch and a
    second copy of it, which leaves that much more of its address-space
    limit to the decoder. Every frame is checked before it is read, so a
    malformed batch never reads more than one image's worth of bytes.
    """
    count = 0
    while True:
        header = stream.read(4)
        if not header:
            if count == 0:
                raise ValueError("Empty image batch")
            return
        if len(header) < 4 or count >= MAX_IMAGES_PER_POST:
            raise ValueError("Malformed image batch")
        size = int.from_bytes(header, "big")
        if size > MAX_IMAGE_SIZE_BYTES:
            raise ValueError("Malformed image batch")
        image = stream.read(size)
        if len(image) < size:
            raise ValueError("Malformed image batch")
        count += 1
        yield image
        del image  # not still held while the next one is read


def _worker_error_text(exc: Exception) -> str:
    # Keep even malformed metadata errors within the bounded reply file.
    return f"{type(exc).__name__}: {str(exc)[:300]}"


def _image_worker_main() -> None:
    try:
        results: List[Dict[str, Any]] = []
        # Validation stops at the first bad image (the whole post fails
        # anyway), without reading the rest of the batch.
        for img_bytes in _read_image_batch(sys.stdin.buffer):
            try:
                results.append({"image": _inspect_image_decoded(img_bytes)})
            except Exception as exc:
                results.append({"error": _worker_error_text(exc)})
                break
            del img_bytes  # free it before the next image is read
        reply: Dict[str, Any] = {"results": results}
    except Exception as exc:
        reply = {"error": _worker_error_text(exc)}
    sys.stdout.buffer.write(json.dumps(reply, ensure_ascii=True).encode("ascii"))


# Runs in the worker before anything else is imported. Each limit is the
# lowest of the requested value and the inherited soft and hard limits, so a
# stricter limit the parent already runs under is kept, never raised. It is
# installed as both soft and hard, so the decoder cannot raise it either.
# macOS defines RLIMIT_RSS as a compatibility alias of RLIMIT_AS, a limit XNU
# neither enforces nor accepts a finite value for (setrlimit fails with EINVAL), so
# it is skipped there; any other failure to install a limit stops the worker.
_IMAGE_WORKER_LIMITS = """\
def _apply_worker_limits(resource, platform, requested_limits):
    for name, requested in requested_limits:
        if not hasattr(resource, name):
            continue
        if name == "RLIMIT_AS" and platform == "darwin":
            continue
        kind = getattr(resource, name)
        limit = requested
        for current in resource.getrlimit(kind):
            if current != resource.RLIM_INFINITY:
                limit = min(limit, current)
        resource.setrlimit(kind, (limit, limit))
"""

_IMAGE_WORKER_PRELUDE = """\
from __future__ import annotations
""" + _IMAGE_WORKER_LIMITS + """\
import sys
try:
    import resource
except ImportError:
    resource = None
if resource is not None:
    _apply_worker_limits(resource, sys.platform, (
        ("RLIMIT_AS", int(sys.argv[1])),
        ("RLIMIT_CPU", int(sys.argv[2])),
        ("RLIMIT_FSIZE", int(sys.argv[3])),
        # RLIMIT_FSIZE does not bound a core dump, and a native decoder crash
        # would otherwise write up to the address-space limit to disk or to a
        # crash handler, along with the image bytes in memory.
        ("RLIMIT_CORE", 0),
    ))
import json
sys.path[:] = json.loads(sys.argv[4])
"""

# What the worker runs, taken from this script itself: only these definitions
# and the values of these constants, so the worker never compiles the whole
# script or imports what only posting needs (requests, urllib3, ssl and its CA
# bundle, html.parser, ...). That saves start-up time and leaves more of the
# worker's address-space limit to the decoder.
_IMAGE_WORKER_CONSTANTS = (
    "MAX_IMAGE_SIZE_BYTES", "MAX_IMAGE_PIXELS", "MAX_IMAGE_DIMENSION",
    "MAX_IMAGE_FRAMES", "MAX_IMAGE_DECODED_PIXELS", "MAX_IMAGES_PER_POST",
    "IMAGE_FORMAT_MIMETYPES", "IMAGE_FORMAT_PLUGINS",
)
_IMAGE_WORKER_FUNCTIONS = (
    "_validate_image_dimensions", "_preflight_webp_dimensions", "_gif_image_count_exceeds",
    "_preflight_image",
    "_inspect_image_decoded", "_read_image_batch", "_worker_error_text",
    "_image_worker_main",
)


def _image_worker_body() -> str:
    """Source of the worker's own code, without the limits prelude."""
    import inspect

    namespace = globals()
    parts = ["import importlib, io, json, struct, sys, warnings"]
    parts += [f"{name} = {namespace[name]!r}" for name in _IMAGE_WORKER_CONSTANTS]
    parts += [inspect.getsource(namespace[name]) for name in _IMAGE_WORKER_FUNCTIONS]
    return "\n".join(parts) + "\n_image_worker_main()\n"


def _image_worker_program(body: Optional[str] = None) -> str:
    """The worker's -c program: install the limits, then run `body`."""
    return _IMAGE_WORKER_PRELUDE + (_image_worker_body() if body is None else body)


def _validated_worker_image(info: Any) -> Dict[str, Any]:
    if not isinstance(info, dict) or type(info.get("width")) is not int or type(info.get("height")) is not int:
        raise ValueError("Image decoder returned invalid dimensions")
    _validate_image_dimensions(info["width"], info["height"])
    if info.get("mimetype") not in IMAGE_FORMAT_MIMETYPES.values():
        raise ValueError("Image decoder returned an invalid MIME type")
    return info


def _worker_reported_error(text: str) -> str:
    # The worker caps its own messages, but they quote decoder text (and a
    # compromised decoder could say anything), so bound and escape them here.
    return _terminal_safe(text[:MAX_WORKER_ERROR_CHARS])


def _inspect_images_in_worker(images: List[bytes]) -> List[Dict[str, Any]]:
    """Decode a whole batch in one bounded process.

    Returns one entry per image up to and including the first failure, each
    either {"image": info} or {"error": message}. One process per post pays
    the interpreter start-up once, and that start-up has its own allowance
    rather than consuming the per-image decode budget.
    """
    budget = MAX_IMAGE_WORKER_STARTUP_SECONDS + MAX_IMAGE_DECODE_SECONDS * len(images)
    # Isolate Python startup from the caller's working directory, environment,
    # and site hooks. Restore the parent's explicit import directories only
    # after limits are installed, so pip --user and virtualenv installs work
    # without adding -c's implicit current-directory import entry.
    working_directory = Path.cwd().resolve()
    import_paths = [
        str(resolved) for resolved in (Path(entry).resolve() for entry in sys.path if entry)
        if resolved != working_directory
    ]
    command = [
        sys.executable, "-I", "-S", "-B", "-c", _image_worker_program(),
        str(MAX_IMAGE_WORKER_MEMORY_BYTES),
        str(max(1, math.ceil(budget))), str(MAX_IMAGE_WORKER_REPLY_BYTES),
        json.dumps(import_paths),
    ]
    # The child needs Python's installation settings, not posting credentials
    # or the parent process's other application secrets.
    environment = {
        key: os.environ[key]
        for key in ("PATH", "SYSTEMROOT", "WINDIR", "LANG", "LC_ALL")
        if key in os.environ
    }
    # A regular anonymous file permits RLIMIT_FSIZE to bound the child's reply;
    # stderr is discarded rather than accumulating native decoder diagnostics.
    import subprocess

    with tempfile.TemporaryFile() as reply:
        try:
            completed = subprocess.run(
                command, input=_encode_image_batch(images), stdout=reply,
                stderr=subprocess.DEVNULL, env=environment, timeout=budget, check=False,
            )
        except subprocess.TimeoutExpired as exc:
            raise ValueError("Image validation exceeded its time limit") from exc
        except OSError as exc:
            raise ValueError("Could not start bounded image validation") from exc
        if completed.returncode != 0:
            raise ValueError("Image decoder failed or exceeded its resource limits")
        reply.seek(0)
        encoded = reply.read(MAX_IMAGE_WORKER_REPLY_BYTES + 1)
    if len(encoded) > MAX_IMAGE_WORKER_REPLY_BYTES:
        raise ValueError("Image decoder returned an oversized result")
    result = _decode_json_object(encoded, "Image decoder")
    if isinstance(result.get("error"), str):
        raise ValueError(_worker_reported_error(result["error"]))
    entries = result.get("results")
    if not isinstance(entries, list) or not 1 <= len(entries) <= len(images):
        raise ValueError("Image decoder returned an invalid result")
    validated: List[Dict[str, Any]] = []
    for entry in entries:
        if isinstance(entry, dict) and isinstance(entry.get("error"), str):
            validated.append({"error": _worker_reported_error(entry["error"])})
            return validated
        validated.append({"image": _validated_worker_image(
            entry.get("image") if isinstance(entry, dict) else None
        )})
    if len(validated) != len(images):
        raise ValueError("Image decoder returned an incomplete result")
    return validated


def inspect_images(images: List[tuple[bytes, str]]) -> List[Dict[str, Any]]:
    """Validate (bytes, source) pairs; the sources only label errors."""
    if not images:
        return []
    for img_bytes, source in images:
        try:
            # This check runs before even starting a native decoder process.
            _preflight_image(img_bytes)
        except ValueError as exc:
            raise ValueError(f"Invalid image {source!r}: {exc}") from exc
    try:
        results = _inspect_images_in_worker([img_bytes for img_bytes, _ in images])
    except (OSError, ValueError) as exc:
        noun = "image" if len(images) == 1 else "images"
        sources = ", ".join(repr(source) for _, source in images)
        raise ValueError(f"Invalid {noun} {sources}: {exc}") from exc
    infos: List[Dict[str, Any]] = []
    for (_, source), result in zip(images, results):
        if "error" in result:
            raise ValueError(f"Invalid image {source!r}: {result['error']}")
        infos.append(result["image"])
    return infos


def inspect_image(img_bytes: bytes, source: str) -> Dict[str, Any]:
    return inspect_images([(img_bytes, source)])[0]


def _upload_timeout(size: int) -> float:
    return UPLOAD_BASE_TIMEOUT_SECONDS + size / UPLOAD_MIN_BYTES_PER_SECOND


def upload_file(
    pds_url: str,
    access_token: str,
    img_bytes: bytes,
    mimetype: str,
    *,
    concurrent_bytes: Optional[int] = None,
) -> Dict:
    """Upload one blob.

    `concurrent_bytes` is the total of all uploads sharing the uplink at once:
    each then gets the budget for that total, since it may only receive a
    share of the bandwidth.
    """
    # The mimetype is always the one Pillow decoded from the bytes themselves.
    # Guessing it from a filename or URL extension would let a remote page pick
    # the Content-Type of a blob we vouch for, so no such fallback exists.
    with _open_api_response(
        "POST",
        _api_url(pds_url, "com.atproto.repo.uploadBlob"),
        timeout=_upload_timeout(
            len(img_bytes) if concurrent_bytes is None else concurrent_bytes
        ),
        operation="waiting for uploadBlob response headers",
        headers={
            "Content-Type": mimetype,
            "Authorization": "Bearer " + access_token,
        },
        data=img_bytes,
    ) as resp:
        _raise_for_xrpc(resp, "uploadBlob")
        data = _json_object(resp, "uploadBlob")
    blob = data.get("blob")
    if not isinstance(blob, dict):
        raise ValueError("uploadBlob response is missing blob")
    # The answer must describe exactly the bytes sent: a PDS (or anything
    # between) cannot swap in another blob, relabel its type, or add fields
    # that would be published in the post. Only the four checked ones are.
    expected = _raw_blob_cid(img_bytes)
    ref = blob.get("ref")
    if (
        blob.get("$type") != "blob"
        or not isinstance(ref, dict)
        or ref.get("$link") != expected
        or blob.get("mimeType") != mimetype
        or type(blob.get("size")) is not int
        or blob["size"] != len(img_bytes)
    ):
        raise ValueError("uploadBlob response does not describe the uploaded bytes")
    return {"$type": "blob", "ref": {"$link": expected}, "mimeType": mimetype, "size": len(img_bytes)}


def _raw_blob_cid(data: bytes) -> str:
    """The CID a PDS stores a blob under: CIDv1, raw codec, SHA-256, base32."""
    return _base32_cid(b"\x01\x55\x12\x20" + hashlib.sha256(data).digest())


def _read_image_file(path: Path) -> bytes:
    flags = os.O_RDONLY
    # O_NOCTTY: a terminal device is refused below, but opening one could
    # first make it the controlling terminal of a process that has none.
    for flag_name in ("O_CLOEXEC", "O_NONBLOCK", "O_NOFOLLOW", "O_NOCTTY"):
        flags |= getattr(os, flag_name, 0)
    have_nofollow = hasattr(os, "O_NOFOLLOW")
    pre_open_info: Optional[os.stat_result] = None
    file_descriptor = -1
    try:
        if not have_nofollow:
            # No O_NOFOLLOW on this platform, so the open() below would follow a
            # symlink. Reject any symlink up front, then re-check after opening
            # that we got the very inode we lstat'd. Comparing (st_dev, st_ino)
            # closes the lstat->open TOCTOU window: if the path was swapped for a
            # symlink (or anything else) in between, the identity check fails.
            pre_open_info = os.lstat(path)
            if stat.S_ISLNK(pre_open_info.st_mode):
                raise ValueError(f"Image path must not be a symbolic link: {path}")
            # Such platforms typically lack O_NONBLOCK too, so opening a named
            # pipe or device could block: refuse anything but a regular file
            # before open(); the identity check below still catches a swap.
            if not stat.S_ISREG(pre_open_info.st_mode):
                raise ValueError(f"Image path is not a regular file: {path}")
        file_descriptor = os.open(path, flags)
        file_info = os.fstat(file_descriptor)
        if pre_open_info is not None and (
            file_info.st_dev != pre_open_info.st_dev
            or file_info.st_ino != pre_open_info.st_ino
        ):
            raise ValueError(f"Image path changed while opening: {path}")
        if not stat.S_ISREG(file_info.st_mode):
            raise ValueError(f"Image path is not a regular file: {path}")
        # O_NONBLOCK was only there so open() cannot hang on a FIFO. Reads must
        # block: on a FUSE or network file a non-blocking read can fail with
        # EAGAIN, which a buffered read turns into None or a silently short
        # result rather than an error.
        if flags & getattr(os, "O_NONBLOCK", 0):
            os.set_blocking(file_descriptor, True)
        if file_info.st_size > MAX_IMAGE_SIZE_BYTES:
            raise ValueError(
                f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum."
            )
        image_file = os.fdopen(file_descriptor, "rb")
        file_descriptor = -1
        with image_file:
            img_bytes = image_file.read(MAX_IMAGE_SIZE_BYTES + 1)
    except OSError as exc:
        if have_nofollow and exc.errno == errno.ELOOP:
            # O_NOFOLLOW reports a symlinked final component as ELOOP, whose
            # stock message ("Too many levels of symbolic links") reads like a
            # broken filesystem rather than the actual, fixable problem.
            raise ValueError(
                f"Image path must not be a symbolic link: {path}"
            ) from exc
        raise ValueError(f"Could not read image file {path!s}: {exc}") from exc
    finally:
        if file_descriptor >= 0:
            os.close(file_descriptor)
    if len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
        raise ValueError(
            f"Image file size too large. {MAX_IMAGE_SIZE_BYTES:,} bytes maximum."
        )
    return img_bytes


# (unchanged upload bytes, validated image info, alt text)
PreparedImage = tuple[bytes, Dict[str, Any], str]


def prepare_images(
    image_paths: List[str],
    alt_texts: Optional[List[str]] = None,
) -> List[PreparedImage]:
    """Read and validate every image locally, in one bounded worker.

    main() runs this before logging in, so a missing or malformed file fails
    before any network request is made or any blob is written.
    """
    images = [(_read_image_file(Path(path)), path) for path in image_paths]
    infos = inspect_images(images)
    # validate_args has already matched the alt-text count to the images.
    alts = alt_texts or [""] * len(images)
    return [
        (img_bytes, info, alt)
        for (img_bytes, _), info, alt in zip(images, infos, alts)
    ]


def upload_images(
    pds_url: str,
    access_token: str,
    prepared: List[PreparedImage],
) -> Dict:
    # Uploaded concurrently, in input order. Blobs left unreferenced by a
    # failure are garbage collected by the PDS.
    total_bytes = sum(len(img_bytes) for img_bytes, _, _ in prepared)
    blobs = _run_concurrently([
        lambda img_bytes=img_bytes, mimetype=image_info["mimetype"]: upload_file(
            pds_url,
            access_token,
            img_bytes,
            mimetype,
            concurrent_bytes=total_bytes if len(prepared) > 1 else None,
        )
        for img_bytes, image_info, _ in prepared
    ])
    images = [
        {
            "alt": alt,
            "image": blob,
            "aspectRatio": {"width": image_info["width"], "height": image_info["height"]},
        }
        for blob, (_, image_info, alt) in zip(blobs, prepared)
    ]
    return {"$type": "app.bsky.embed.images", "images": images}


_ZERO_WIDTH_JOINER = "\u200d"


def _grapheme_safe_prefix(text: str, limit: int) -> str:
    """Return at most `limit` code points with best-effort boundary adjustments.

    The stdlib has no UAX #29 grapheme segmentation, so this only avoids the
    obviously broken boundaries: before a mark (category M*, which includes
    the variation selectors), which attaches to the character before it, and
    after a zero-width joiner, which joins the next one. It can still split
    emoji modifier sequences, joined emoji, and regional-indicator flags.
    """
    if len(text) <= limit:
        return text
    cut = limit
    while cut > 0 and (
        unicodedata.category(text[cut]).startswith("M")
        or text[cut - 1] == _ZERO_WIDTH_JOINER
    ):
        cut -= 1
    return text[:cut]


def _trim_card_text(value: Any, limit: int) -> str:
    if not isinstance(value, str):
        return ""
    return _grapheme_safe_prefix(value.strip(), limit)


# Every <meta> key a link card reads; _card_metadata finds them in one pass.
CARD_META_KEYS = ("og:title", "og:description", "description", "og:image")


def _attach_external_thumb(
    card: Dict[str, Any],
    pds_url: str,
    access_token: str,
    page_url: str,
    img_url: str,
) -> None:
    if not img_url:
        return
    try:
        # Before urljoin, which takes ~85 ms to collapse a 1 MB "../../..".
        if len(img_url) > MAX_URL_LENGTH:
            raise ValueError(f"URL exceeds {MAX_URL_LENGTH} characters")
        img_url = urljoin(page_url, img_url)
        # As for a redirect: an HTTPS page's thumbnail is never fetched over
        # plain HTTP, where anyone on the path could choose the image that
        # gets uploaded and published.
        if (
            urlparse(page_url).scheme.lower() == "https"
            and urlparse(img_url).scheme.lower() != "https"
        ):
            raise ValueError("Refusing an HTTP og:image on an HTTPS page")
        img_bytes, _, _ = _safe_download(img_url, MAX_EMBED_IMAGE_BYTES)
        # The URL only labels errors, which reach stderr: redact it like every
        # other logged URL, since a query string can carry a signed token.
        image_info = inspect_image(img_bytes, _url_for_log(img_url))
        card["thumb"] = upload_file(
            pds_url,
            access_token,
            img_bytes,
            image_info["mimetype"],
        )
    except (requests.RequestException, ValueError) as exc:
        # A failed thumbnail (download, validation, or a rejected or timed-out
        # upload) never aborts the post: the card goes without it.
        _warn(
            f"warning: could not embed og:image {_url_for_log(img_url)!r} "
            f"({type(exc).__name__}: {_terminal_safe(exc)}); "
            "posting the card without it."
        )


class _HeadBoundaryFound(Exception):
    pass


# A start tag longer than this ends the head. Real tags are far shorter,
# and html.parser lists every attribute (about 200 bytes each) before any
# handler sees the tag. 64 KiB leaves room for the config blobs some heads
# carry in a <meta>.
MAX_PARSED_START_TAG_CHARS = 64 * 1024

# html.parser finds the end of a start tag with this regex. Its backtracking
# state grows about 200 bytes per attribute, all held until the match ends:
# one 1 MB <meta> of short attributes held over 200 MB. Nothing after the
# attribute loop can fail, so the engine never backtracks into an earlier
# attribute; matching one attribute at a time therefore ends exactly where
# the whole regex does, in constant memory. That is done only where the
# installed html.parser uses exactly this pattern (the suite compares the
# two); elsewhere the regex runs over a bounded window instead.
_HTML_PARSER_START_TAG_END = r"""
  <[a-zA-Z][^\t\n\r\f />\x00]*       # tag name
  (?:[\s/]*                          # optional whitespace before attribute name
    (?:(?<=['"\s/])[^\s/>][^\s/=>]*  # attribute name
      (?:\s*=+\s*                    # value indicator
        (?:'[^']*'                   # LITA-enclosed value
          |"[^"]*"                   # LIT-enclosed value
          |(?!['"])[^>\s]*           # bare value
         )
        \s*                          # possibly followed by a space
       )?(?:\s|/(?!>))*
     )*
   )?
  \s*                                # trailing whitespace
"""
_installed_start_tag_end = getattr(html.parser, "locatestarttagend_tolerant", None)
EXACT_START_TAG_SCAN = (
    _installed_start_tag_end is not None
    and _installed_start_tag_end.pattern == _HTML_PARSER_START_TAG_END
    and _installed_start_tag_end.flags == re.compile("", re.VERBOSE).flags
)
_START_TAG_NAME = re.compile(r"<[a-zA-Z][^\t\n\r\f />\x00]*")
_START_TAG_LEAD = re.compile(r"[\s/]*")
_START_TAG_ATTRIBUTE = re.compile(r"""
    (?<=['"\s/])[^\s/>][^\s/=>]*
    (?:\s*=+\s*(?:'[^']*'|"[^"]*"|(?!['"])[^>\s]*)\s*)?(?:\s|/(?!>))*
""", re.VERBOSE)
_TRAILING_SPACE = re.compile(r"\s*")


def _start_tag_match_end(rawdata: str, i: int, limit: Optional[int] = None) -> int:
    """Where html.parser's start-tag regex match at `i` ends, one attribute at a time.

    With `limit`, gives up (returning a position past it) once the tag is
    known to be longer than `limit`.
    """
    name = _START_TAG_NAME.match(rawdata, i)
    if name is None:
        raise AssertionError("not a start tag")  # html.parser's own reaction
    position = _START_TAG_LEAD.match(rawdata, name.end()).end()
    while limit is None or position - i <= limit:
        attribute = _START_TAG_ATTRIBUTE.match(rawdata, position)
        if attribute is None or attribute.end() == position:
            break
        position = attribute.end()
    return _TRAILING_SPACE.match(rawdata, position).end()


class _HeadBoundaryParser(HTMLParser):
    """Stop at the first real </head> end tag or <body> start tag.

    A tokenizer, not a byte search: the same text inside a comment, an
    attribute value, or the contents of <script>, <style>, <title> and the
    other text-only elements is not a tag and must neither end the head early
    (dropping its og: tags) nor be skipped over into the body.
    """

    # Elements whose contents are text up to their own end tag. html.parser
    # itself only knows script and style before Python 3.14.
    TEXT_ONLY_ELEMENTS = frozenset(
        {"script", "style", "title", "textarea", "xmp", "iframe", "noembed", "noframes"}
    )

    def __init__(self) -> None:
        # convert_charrefs must stay on: with it off, html.parser stops
        # tokenizing at a malformed reference such as "&#;" and never reaches
        # the </head> after it.
        super().__init__(convert_charrefs=True)
        # Runs check_for_whole_start_tag on windows, for an unknown html.parser.
        self._window_parser: Optional[HTMLParser] = None

    def reset(self) -> None:
        super().reset()
        # (position, answer) of the start tag parse_starttag has just checked.
        self._checked_start_tag: Optional[tuple[int, int]] = None

    def check_for_whole_start_tag(self, i: int) -> int:
        """html.parser's answer, found without its memory-hungry regex match."""
        checked = self._checked_start_tag
        if checked is not None and checked[0] == i:
            return checked[1]  # html.parser asking again for the tag just checked
        if not EXACT_START_TAG_SCAN:
            return self._windowed_start_tag_check(i)
        # html.parser's own logic from here (Python 3.11 to 3.13).
        rawdata = self.rawdata
        j = _start_tag_match_end(rawdata, i, MAX_PARSED_START_TAG_CHARS)
        if j - i > MAX_PARSED_START_TAG_CHARS:
            return j  # longer than any real tag: parse_starttag ends the head here
        # CPython's branches, without the ones it can never reach: the name
        # matched, so j > i, and the match takes every "/" not before a ">".
        following = rawdata[j:j + 1]
        if following == ">":
            return j + 1
        if rawdata.startswith("/>", j):
            return j + 2
        if not following or following in "abcdefghijklmnopqrstuvwxyz=ABCDEFGHIJKLMNOPQRSTUVWXYZ":
            return -1  # end of input in or before an attribute value
        return j

    def _windowed_start_tag_check(self, i: int) -> int:
        """For an html.parser this script does not know: its own check over a
        window of at most MAX_PARSED_START_TAG_CHARS, so its regex stays
        bounded; a tag still open at the end of the window is treated as
        ending there (parse_starttag then ends the head at it)."""
        window = self.rawdata[i:i + MAX_PARSED_START_TAG_CHARS + 1]
        if self._window_parser is None:
            self._window_parser = HTMLParser()
        self._window_parser.rawdata = window
        end = self._window_parser.check_for_whole_start_tag(0)
        if len(window) > MAX_PARSED_START_TAG_CHARS and (end < 0 or end > MAX_PARSED_START_TAG_CHARS):
            return i + len(window)
        return end if end < 0 else i + end

    def parse_starttag(self, i: int) -> int:
        end = self.check_for_whole_start_tag(i)
        length = (end if end >= 0 else len(self.rawdata)) - i
        if length > MAX_PARSED_START_TAG_CHARS:
            # A tag no real page writes: the head ends where it starts, so
            # only what precedes it is used, never anything after.
            self._stop()
        # html.parser's parse_starttag begins by asking the same question;
        # it gets this answer rather than a second scan of the tag.
        self._checked_start_tag = (i, end)
        try:
            return super().parse_starttag(i)
        finally:
            self._checked_start_tag = None

    def handle_starttag(self, tag: str, attrs: Any) -> None:
        if tag == "body":
            self._stop()
        if tag in self.TEXT_ONLY_ELEMENTS:
            self.set_cdata_mode(tag)

    def handle_startendtag(self, tag: str, attrs: Any) -> None:
        # Not handle_starttag: "<script src=x />" has no contents as
        # html.parser reads it, so entering text-only mode here would
        # swallow the rest of the page and its </head>.
        if tag == "body":
            self._stop()

    def handle_endtag(self, tag: str) -> None:
        if tag == "head":
            self._stop()

    def _stop(self) -> None:
        raise _HeadBoundaryFound


class _HeadOffsetParser(_HeadBoundaryParser):
    """_HeadBoundaryParser that also records where the boundary was found.

    Only _HeadPrefix needs the position; the parsers that read <meta> tags
    and the title skip the per-token bookkeeping.
    """

    def __init__(self) -> None:
        super().__init__()
        self.boundary: Optional[tuple[int, int]] = None
        self.boundary_offset = 0

    def reset(self) -> None:
        super().reset()
        # Characters consumed so far. html.parser moves its position only
        # through updatepos, a span at a time, and trims its buffer only up
        # to that position, so summing the spans gives an absolute offset
        # without the line/column arithmetic getpos() needs.
        self._position = 0

    def updatepos(self, i: int, j: int) -> int:
        if i < j:
            self._position += j - i
        return super().updatepos(i, j)

    def _stop(self) -> None:
        self.boundary = self.getpos()
        self.boundary_offset = self._position
        super()._stop()


# The most raw <title> text kept: enough for MAX_EXTERNAL_TITLE_CHARS
# characters even were every one written as a long character reference.
MAX_RAW_TITLE_CHARS = (MAX_EXTERNAL_TITLE_CHARS + 1) * 40


class _CardMetadataParser(_HeadBoundaryParser):
    """Read a link card's metadata from a page head in one streaming pass.

    Nothing is built: each <meta> is read as it is tokenized and then
    forgotten, and only the first <title>'s text is kept, at most
    MAX_RAW_TITLE_CHARS of it. A tree builder holds every element, so a head
    of hostile markup cost hundreds of megabytes in the (unlimited) parent
    process. <title> is raw text with character references, as in browsers
    (and html.parser from Python 3.14): markup inside it is part of the title.

    The <meta> rule: the first non-blank content wins, property= before
    name=, so an empty duplicate never hides a real one later in the head.
    Keys match case-insensitively (real pages emit property="OG:Title"), and
    values are stripped, so a whitespace-only og:title still falls back to
    <title> and an og:image padded with newlines is still a fetchable URL.
    Of a repeated attribute the first counts, as in browsers.
    """

    def __init__(self, keys: tuple[str, ...]) -> None:
        super().__init__()
        self._wanted = frozenset(key.lower() for key in keys)
        self.by_property: Dict[str, str] = {}
        self.by_name: Dict[str, str] = {}
        self._title: Optional[List[str]] = None  # None until the first <title>
        self._title_chars = 0
        self._in_title = False

    def handle_starttag(self, tag: str, attrs: Any) -> None:
        if tag == "meta":
            self._read_meta(attrs)
        elif tag == "title" and self._title is None:
            self._title = []
            self._in_title = True
        super().handle_starttag(tag, attrs)

    def handle_startendtag(self, tag: str, attrs: Any) -> None:
        if tag == "meta":
            self._read_meta(attrs)
        elif tag == "title" and self._title is None:
            self._title = []  # <title/>: the page's title is empty
        super().handle_startendtag(tag, attrs)

    def handle_endtag(self, tag: str) -> None:
        if tag == "title":
            self._in_title = False
        super().handle_endtag(tag)

    def handle_data(self, data: str) -> None:
        # _in_title is only ever set together with a fresh _title list.
        # html.parser up to 3.13 hands the whole <title> over in one call;
        # 3.14 may split it at character references, so stop once it is full
        # rather than append an empty string for every later piece.
        if not self._in_title or self._title_chars >= MAX_RAW_TITLE_CHARS:
            return
        if not self._title_chars:
            data = data.lstrip()  # leading whitespace never uses up the allowance
        data = data[: MAX_RAW_TITLE_CHARS - self._title_chars]
        self._title.append(data)
        self._title_chars += len(data)

    def _read_meta(self, attrs: Any) -> None:
        values: Dict[str, Optional[str]] = {}
        for name, value in attrs:
            values.setdefault(name, value)
        content = values.get("content")
        if not isinstance(content, str) or not (content := content.strip()):
            return
        for attribute, found in (("property", self.by_property), ("name", self.by_name)):
            value = values.get(attribute)
            if isinstance(value, str):
                key = value.strip().lower()
                if key in self._wanted and key not in found:
                    found[key] = content

    def title(self) -> str:
        text = "".join(self._title or ())
        # Python 3.14's html.parser reads <title> as escapable raw text and
        # decodes its character references before handle_data(); decoding
        # again would turn "&amp;lt;" into "<". Earlier releases pass it raw.
        if "title" in getattr(self, "RCDATA_CONTENT_ELEMENTS", ()):
            return text
        return html.unescape(text)


def _card_metadata(
    head_text: str,
    keys: tuple[str, ...] = (),
) -> tuple[Dict[str, str], str]:
    """The `keys` <meta> contents ("" if absent) and the <title> text of a head."""
    keys = keys or CARD_META_KEYS
    parser = _CardMetadataParser(keys)
    try:
        parser.feed(head_text)
        parser.close()
    except _HeadBoundaryFound:
        pass  # a whole page was given: its head ends here
    meta = {
        key: parser.by_property.get(key.lower()) or parser.by_name.get(key.lower(), "")
        for key in keys
    }
    return meta, parser.title()



def _external_card_metadata(url: str, meta: Dict[str, str], title: str) -> Dict[str, Any]:
    """Build the card text from _card_metadata's results."""
    description = meta["og:description"] or meta["description"]
    return {
        "uri": url,
        "title": _trim_card_text(meta["og:title"] or title, MAX_EXTERNAL_TITLE_CHARS),
        "description": _trim_card_text(description, MAX_EXTERNAL_DESCRIPTION_CHARS),
    }


# The charset in a <meta http-equiv="Content-Type" content="..."> value. No
# nested or overlapping repetition, so a search is linear in the value.
META_CONTENT_CHARSET_REGEX = re.compile(r"""charset\s*=\s*["']?([^\s"';,]+)""", re.IGNORECASE)


class _MetaCharsetParser(_HeadBoundaryParser):
    """Find the first <meta> that declares a charset, as the page tokenizes.

    A tokenizer, not a regex search: the usual declared-encoding regexes (as
    in BeautifulSoup) backtrack quadratically on hostile markup, and the C
    regex engine holds the GIL, so no deadline could interrupt one. This is linear, and a <meta> inside a comment or
    <script> is not mistaken for one.
    """

    def __init__(self) -> None:
        super().__init__()
        self.charset: Optional[str] = None

    def handle_starttag(self, tag: str, attrs: Any) -> None:
        if tag == "meta":
            self._check(attrs)
        super().handle_starttag(tag, attrs)

    def handle_startendtag(self, tag: str, attrs: Any) -> None:
        if tag == "meta":
            self._check(attrs)
        super().handle_startendtag(tag, attrs)

    def _check(self, attrs: Any) -> None:
        values: Dict[str, str] = {}
        for name, value in attrs:  # the first of a repeated attribute counts
            values.setdefault(name, value or "")
        charset = values.get("charset", "").strip()
        if not charset and values.get("http-equiv", "").strip().lower() == "content-type":
            match = META_CONTENT_CHARSET_REGEX.search(values.get("content", ""))
            charset = match.group(1) if match else ""
        if charset:
            self.charset = charset
            self._stop()


def _meta_charset(prefix: bytes) -> Optional[str]:
    parser = _MetaCharsetParser()
    try:
        parser.feed(prefix.decode("latin-1"))
    except _HeadBoundaryFound:
        pass
    return parser.charset


class _HeadPrefix:
    """Collect a page incrementally until its head is complete.

    The head is everything before the first real </head>, or, when the page
    omits it (which HTML permits), before the first real <body>, whichever
    comes first. Pages where neither is found in the leading `max_bytes`
    (for example UTF-16 pages) fall back to that prefix.
    """

    def __init__(self, max_bytes: int) -> None:
        self._max_bytes = max_bytes
        self._parser = _HeadOffsetParser()
        self._window = bytearray()
        self._fed = 0  # how much of the window the parser has been given
        self._head: Optional[bytes] = None

    def feed(self, chunk: bytes) -> bool:
        """Add the next bytes; True once the head is complete (stop feeding then)."""
        chunk = chunk[: self._max_bytes - len(self._window)]
        self._window += chunk
        full = len(self._window) >= self._max_bytes
        pending = len(self._window) - self._fed
        # html.parser keeps an unfinished construct (an open <script>, comment
        # or tag) and rescans all of it on every feed, and a chunked response
        # can arrive in pieces of a few bytes: fed one by one, a long script
        # costs quadratic time. So bytes are passed on once at least as many
        # are waiting as the parser holds back: at once when it holds nothing
        # (ordinary markup), otherwise in doubling batches, which keeps the
        # total work linear.
        held = len(getattr(self._parser, "rawdata", ""))
        if pending and (full or pending >= held) and self._feed_pending():
            return True
        if full:
            self._head = bytes(self._window)
            return True
        return False

    def result(self) -> bytes:
        if self._head is None and self._fed < len(self._window):
            self._feed_pending()  # the page ended with bytes still waiting
        return self._head if self._head is not None else bytes(self._window)

    def _feed_pending(self) -> bool:
        data = bytes(self._window[self._fed:])
        self._fed = len(self._window)
        try:
            # Latin-1 maps each byte to one character, so positions are byte
            # offsets; markup delimiters are ASCII in every encoding this can
            # locate a head in. html.parser carries partial markup across
            # feeds, so chunk boundaries do not matter.
            self._parser.feed(data.decode("latin-1"))
        except _HeadBoundaryFound:
            self._head = self._before_boundary()
            return True
        return False

    def _before_boundary(self) -> bytes:
        line, column = self._parser.boundary
        offset = self._parser.boundary_offset
        # Confirm the tracked offset is html.parser's own (line, column), with
        # two C-speed scans; should a future html.parser move its position
        # some other way, walk the lines instead.
        line_start = offset - column
        if (
            self._window.count(b"\n", 0, offset) != line - 1
            or self._window.rfind(b"\n", 0, offset) + 1 != line_start
        ):
            line_start = 0
            for _ in range(line - 1):
                line_start = self._window.index(b"\n", line_start) + 1
        return bytes(self._window[:line_start + column])



def _html_charset_codec(label: str) -> Optional[codecs.CodecInfo]:
    """The codec for a page's charset label, or None if it names no charset.

    The label is the page's own choice (header, <meta>, or a guess from its
    bytes). bytes.decode() refuses codecs that are not text encodings, but an
    incremental decoder does not: "zlib" would inflate a small head without
    bound and return bytes, and others raise errors that are no
    UnicodeDecodeError. Only real character sets are accepted.
    """
    try:
        codec = codecs.lookup(label)
    except (LookupError, ValueError):  # ValueError: a NUL inside the label
        return None
    if not getattr(codec, "_is_text_encoding", True) or codec.name in NON_CHARSET_TEXT_CODECS:
        return None
    return codec


def _decode_with(codec: codecs.CodecInfo, head: bytes, errors: str) -> Optional[str]:
    """The head decoded by `codec`, or None if it does not decode to text.

    _html_charset_codec already keeps non-charset codecs out; any failure of
    one that slips through still only means "not this candidate".
    """
    try:
        # final=False: the head may end mid-character at the 1 MB cap.
        text = codec.incrementaldecoder(errors).decode(head, final=False)
    except Exception:
        return None
    return text if isinstance(text, str) else None


def _mostly_decodes(text: str) -> bool:
    """Whether replacement characters are the exception in a decoded head.

    A page in its declared charset with a stray bad byte (a pasted comment, a
    truncated script) still decodes to far more real non-ASCII characters
    than replacements; a page in some other charset does not.
    """
    replaced = text.count("\ufffd")
    non_ascii = len(text) - len(text.encode("ascii", "ignore"))
    return non_ascii - replaced > replaced


def _is_utf16_or_utf32_label(label: str) -> bool:
    # By the codec it names, so aliases ("utf16", "u16", "utf_16le") count.
    codec = _html_charset_codec(label)
    return codec is not None and codec.name.startswith(("utf-16", "utf-32"))


def _xml_declared_encoding(prefix: bytes) -> Optional[str]:
    """The encoding an XML declaration on the page's first line names, if any.

    The same answer as BeautifulSoup's regex for it
    (^\\s*<\\?.*encoding=['"](.*?)['"].*\\?> over the first 1 KB, ignoring
    case), found without its backtracking: the last "encoding=" on that line
    whose quoted value is followed by a "?>".
    """
    line = prefix[:XML_DECLARATION_SNIFF_BYTES].lstrip(b" \t\n\r\x0b\x0c")
    if not line.startswith(b"<?"):
        return None
    # bytes.lower() folds only A-Z, and only ASCII survives the decode below
    # (anything else becomes U+FFFD), so one fold serves search and value.
    line = line[2:].split(b"\n", 1)[0].lower()
    position = len(line)
    while (position := line.rfind(b"encoding=", 0, position)) >= 0:
        start = position + len(b"encoding=")
        if line[start:start + 1] not in (b"'", b'"'):
            continue
        quotes = [line.find(mark, start + 1) for mark in (b"'", b'"')]
        close = min((found for found in quotes if found >= 0), default=-1)
        if close >= 0 and line.find(b"?>", close + 1) >= 0:
            value = line[start + 1:close]
            return value.decode("ascii", "replace") or None
    return None


def _guessed_charsets(prefix: bytes) -> Iterator[str]:
    """Labels worth trying once the declared ones have failed, best first."""
    declared = _xml_declared_encoding(prefix)
    if declared and not _is_utf16_or_utf32_label(declared):
        yield declared  # a UTF-16/32 one is ignored, as for <meta>
    guess = _charset_detector().detect(prefix)
    if guess["encoding"] and (guess.get("confidence") or 0) >= MIN_CHARSET_GUESS_CONFIDENCE:
        yield guess["encoding"]


def _charset_detector() -> Any:
    """chardet where installed, else charset-normalizer (a Requests dependency).

    chardet (pinned in requirements.txt) guesses East Asian pages, especially
    ones whose prefix ends mid-character, far more reliably; charset-normalizer
    keeps the script working where only Requests' dependencies are installed.
    Imported only when a guess is needed: either takes tens of milliseconds.
    """
    try:
        import chardet
    except ImportError:
        import charset_normalizer

        return charset_normalizer
    return chardet


def _decode_html_head(head: bytes, content_type: Optional[str]) -> str:
    """Decode a page head, preferring what the page says about itself.

    A byte-order mark is definitive, as in browsers. Otherwise the server's
    charset, then a <meta charset> near the start (a UTF-16 or UTF-32 label
    there is ignored, as browsers do: the document could not have been read
    to find it were it really so encoded), then UTF-8, are tried, first
    strictly, so a wrong label falls through instead of producing mojibake,
    and then with replacement where that still mostly decodes, so one stray
    byte does not discard a right label. Only then does a statistical guess,
    made from a bounded prefix, get a turn; windows-1252 with replacement is
    the last resort.
    """
    for mark, encoding in HTML_BYTE_ORDER_MARKS.items():
        if head.startswith(mark):
            return _decode_with(codecs.lookup(encoding), head, "replace") or ""

    header_label = _charset_from_content_type(content_type)
    header_codec = _html_charset_codec(header_label) if header_label else None
    # A header charset that decodes the head strictly settles it before the
    # head is tokenized for a <meta>, which would then never be consulted.
    if header_codec is not None:
        text = _decode_with(header_codec, head, "strict")
        if text is not None:
            return text

    prefix = head[:HTML_CHARSET_SNIFF_BYTES]
    # Anywhere in the bounded prefix, not just its first few KB, which a long
    # leading script or comment can push a real <meta> past.
    in_document = _meta_charset(prefix)
    if in_document and _is_utf16_or_utf32_label(in_document):
        in_document = None
    candidates: List[codecs.CodecInfo] = [] if header_codec is None else [header_codec]
    for label in (in_document, "utf-8"):
        codec = _html_charset_codec(label) if label else None
        if codec is not None and all(codec.name != seen.name for seen in candidates):
            candidates.append(codec)

    for codec in candidates:
        if codec is header_codec:
            continue  # already tried strictly, above
        text = _decode_with(codec, head, "strict")
        if text is not None:
            return text
    for codec in candidates:
        text = _decode_with(codec, head, "replace")
        if text is not None and _mostly_decodes(text):
            return text
    # The guess is never checked against the prefix it was made from (whose
    # last character the cut may have split), only against the whole head.
    tried = {codec.name for codec in candidates}
    for label in _guessed_charsets(prefix):
        codec = _html_charset_codec(label)
        if codec is None or codec.name in tried:
            continue
        tried.add(codec.name)
        text = _decode_with(codec, head, "strict")
        if text is not None:
            return text
    return head.decode("windows-1252", errors="replace")


def _parse_embed_metadata(
    url: str,
    head_bytes: bytes,
    content_type: Optional[str],
) -> tuple[Dict[str, Any], str]:
    """Parse a page's head and extract all link-card metadata under one budget.

    `_safe_download` releases its deadline when it returns, so without this the
    parse is the one unbounded step in building a card. `head_bytes` is the
    page already cut to its head (_safe_download's html_head_only, or
    _HeadPrefix), which keeps ordinary pages far inside the budget. On timeout the
    worker is abandoned (Python cannot cancel a running parse) and the caller
    degrades to a bare card; the abandoned thread finishes on its own, bounded
    by MAX_EMBED_HEAD_BYTES, and is a daemon so it never delays exit.
    """
    def parse() -> tuple[Dict[str, Any], str]:
        # Streamed, never built into a tree (see _CardMetadataParser).
        meta, title = _card_metadata(_decode_html_head(head_bytes, content_type))
        return _external_card_metadata(url, meta, title), meta["og:image"]

    subject_token = _NETWORK_TIMEOUT_SUBJECT.set("Link card")
    try:
        return _run_before_download_deadline(
            parse,
            time.monotonic() + MAX_EMBED_PARSE_SECONDS,
            "parsing the linked page and its metadata",
        )
    finally:
        _NETWORK_TIMEOUT_SUBJECT.reset(subject_token)


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    token = _RESOLVED_ADDRESSES.set({})
    try:
        return _fetch_embed_url_card(pds_url, access_token, url)
    finally:
        _RESOLVED_ADDRESSES.reset(token)


def _fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    # From here on, any failure degrades to a bare card carrying just the URL.
    card: Dict[str, Any] = {"uri": url, "title": "", "description": ""}
    try:
        head_bytes, final_url, content_type = _safe_download(
            url,
            MAX_EMBED_HEAD_BYTES,
            allowed_content_types=EMBED_HTML_CONTENT_TYPES,
            html_head_only=True,
        )
        card, image_url = _parse_embed_metadata(url, head_bytes, content_type)
    except (requests.RequestException, ValueError, RecursionError) as exc:
        _warn(
            f"warning: could not read {_url_for_log(url)!r} for the link card "
            f"({type(exc).__name__}: {_terminal_safe(exc)}); "
            "posting a card with the URL only."
        )
        return {"$type": "app.bsky.embed.external", "external": card}

    _attach_external_thumb(card, pds_url, access_token, final_url, image_url)
    return {"$type": "app.bsky.embed.external", "external": card}


def get_embed_ref(record_service_url: str, ref_uri: str) -> Dict:
    return {
        "$type": "app.bsky.embed.record",
        "record": record_ref(get_record(record_service_url, ref_uri)),
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
    # Mention resolution and the reply lookup are independent network work.
    facets, reply = _run_concurrently([
        (lambda: parse_facets(args.pds_url, args.text)) if args.text else None,
        (lambda: get_reply_refs(
            args.record_service_url, _did_addressed_uri(args.pds_url, args.reply_to)
        )) if args.reply_to else None,
    ])
    if facets:
        post["facets"] = facets
    if reply:
        post["reply"] = reply
    return post


def _build_embed(args: argparse.Namespace, access_token: str) -> Optional[Dict]:
    media: Optional[Callable[[], Dict]] = None
    if args.image:
        media = lambda: upload_images(args.pds_url, access_token, args.prepared_images)  # noqa: E731
    elif args.embed_url:
        media = lambda: fetch_embed_url_card(args.pds_url, access_token, args.embed_url)  # noqa: E731

    # The quoted record's lookup and the media (uploads or a link card) are
    # independent network work.
    record_embed, media_embed = _run_concurrently([
        (lambda: get_embed_ref(
            args.record_service_url, _did_addressed_uri(args.pds_url, args.embed_ref)
        )) if args.embed_ref else None,
        media,
    ])

    if record_embed and media_embed:
        return {
            "$type": "app.bsky.embed.recordWithMedia",
            "record": record_embed,
            "media": media_embed,
        }
    return record_embed or media_embed


def _new_tid() -> str:
    """Return a fresh TID record key: microsecond timestamp + random clock id."""
    import secrets

    value = ((time.time_ns() // 1_000) << 10) | secrets.randbelow(1024)
    return "".join(
        TID_ALPHABET[(value >> shift) & 31] for shift in range(60, -1, -5)
    )


class UnusableCreateResponse(ValueError):
    """createRecord answered with success, but its body could not be accepted.

    The PDS may well have created the post, so this is settled like a timeout
    rather than reported as a definite failure.
    """


def _post_record_key(did: str, rkey: str) -> Dict[str, str]:
    return {"repo": did, "collection": "app.bsky.feed.post", "rkey": rkey}


def _create_post_record(
    pds_url: str,
    access_token: str,
    did: str,
    post: Dict,
    rkey: str,
    *,
    verbose: bool,
) -> Dict:
    requested = _post_record_key(did, rkey)
    with _open_api_response(
        "POST",
        _api_url(pds_url, "com.atproto.repo.createRecord"),
        timeout=30,
        operation="waiting for createRecord response headers",
        headers={"Authorization": "Bearer " + access_token},
        json=dict(requested, record=post),
    ) as resp:
        _raise_for_xrpc(resp, "createRecord", verbose=verbose)
        try:
            data = _json_object(resp, "createRecord")
            # Only the validated {uri, cid} is kept and printed: the rest of
            # the body is the server's, unchecked and of any depth.
            return _validated_record_response(data, requested, "createRecord")
        except ValueError as exc:
            raise UnusableCreateResponse(str(exc)) from exc


def _existing_post(pds_url: str, did: str, rkey: str) -> Optional[Dict]:
    """Return {uri, cid} if the post with this record key exists, else None."""
    requested = _post_record_key(did, rkey)
    with _open_api_response(
        "GET",
        _api_url(pds_url, "com.atproto.repo.getRecord"),
        timeout=15,
        operation="waiting for getRecord response headers",
        params=requested,
    ) as resp:
        try:
            _raise_for_xrpc(resp, "getRecord")
        except XrpcError as exc:
            if resp.status_code == 404 or exc.error_name == "RecordNotFound":
                return None
            raise
        data = _json_object(resp, "getRecord")
    return _validated_record_response(data, requested, "getRecord")


# Raised by Requests before anything is sent, so the post cannot exist.
_UNSENT_REQUEST_ERRORS = (
    requests.exceptions.InvalidJSONError,
    requests.exceptions.InvalidHeader,
    requests.exceptions.InvalidURL,
    requests.exceptions.InvalidSchema,
    requests.exceptions.MissingSchema,
)


def _outcome_uncertain(exc: Exception) -> bool:
    """Whether a failed createRecord may nonetheless have created the post.

    A definite 4xx rejection or a request that was never sent did not; a
    timeout, a dropped connection, an unusable success response, or a 5xx
    from a gateway in front of the PDS all might have.
    """
    if isinstance(exc, _UNSENT_REQUEST_ERRORS):
        return False
    if isinstance(exc, requests.HTTPError):
        response = exc.response
        return response is None or response.status_code >= 500
    return True


def _publish_post(
    pds_url: str,
    access_token: str,
    did: str,
    post: Dict,
    *,
    verbose: bool,
) -> Any:
    """Create the post exactly once, even when the first attempt is ambiguous.

    The record key is chosen here, so an uncertain first attempt can be
    settled by looking the key up, and a retry with the same key can never
    produce a second post: if the first attempt lands after all, one of the
    two is rejected as a duplicate and the lookup finds the other.
    """
    rkey = _new_tid()
    try:
        return _create_post_record(pds_url, access_token, did, post, rkey, verbose=verbose)
    except (requests.RequestException, UnusableCreateResponse) as exc:
        if not _outcome_uncertain(exc):
            raise
        print(
            f"warning: createRecord did not complete ({type(exc).__name__}); "
            "checking whether the post was created.",
            file=sys.stderr,
        )

    def warn_post_may_exist() -> None:
        # A new run picks a new record key, so re-running blindly could post
        # twice. Name the record so the user can check it first.
        uri = _terminal_safe(f"at://{did}/app.bsky.feed.post/{rkey}")
        print(
            f"warning: the post may still have been created as {uri}; "
            "check before posting again.",
            file=sys.stderr,
        )

    try:
        existing = _existing_post(pds_url, did, rkey)
        if existing is not None:
            return existing
        print(
            "warning: the post was not created; retrying once with the same record key.",
            file=sys.stderr,
        )
        try:
            return _create_post_record(pds_url, access_token, did, post, rkey, verbose=verbose)
        except (requests.RequestException, UnusableCreateResponse) as exc:
            retry_error = exc
        existing = _existing_post(pds_url, did, rkey)
    except (requests.RequestException, ValueError):
        warn_post_may_exist()
        raise
    if existing is not None:
        return existing
    # Even a definite rejection of the retry says nothing about the first
    # attempt, which may still be in flight and land after this lookup.
    warn_post_may_exist()
    raise retry_error


def create_post(args: argparse.Namespace) -> None:
    pool = _ApiSessionPool()
    pool_token = _API_SESSION_POOL.set(pool)
    try:
        session = bsky_login_session(args.pds_url, args.handle, args.password)
        try:
            access_token = session["accessJwt"]
            # The record's text-side lookups and the embed's are independent,
            # so they run at once; a failure in the record side is reported
            # first.
            has_embed = bool(args.image or args.embed_url or args.embed_ref)
            post, embed = _run_concurrently([
                lambda: _build_post_record(args),
                (lambda: _build_embed(args, access_token)) if has_embed else None,
            ])
            if embed:
                post["embed"] = embed

            print("Creating post.", file=sys.stderr)
            if args.verbose:
                print(json.dumps(post, indent=2), file=sys.stderr)
            resp_body = _publish_post(
                args.pds_url, access_token, session["did"], post, verbose=args.verbose
            )
            print(json.dumps(resp_body, indent=2))
        finally:
            # Revoke the refresh token whether or not the post succeeded, so a
            # long-lived credential never outlives this run.
            refresh_token = session.get("refreshJwt")
            if isinstance(refresh_token, str) and refresh_token:
                bsky_logout_session(args.pds_url, refresh_token)
    finally:
        _API_SESSION_POOL.reset(pool_token)
        pool.close()


def _environment_credential(name: str) -> Optional[str]:
    """A credential variable's value; one exported empty or blank is unset."""
    value = os.environ.get(name)
    return value if value and value.strip() else None


def exit_error(*lines: str) -> None:
    # Lines routinely interpolate exception text that originated at a remote
    # server (an XRPC message, a refused response header), so sanitize here rather
    # than trusting every call site to remember.
    raise SystemExit("\n".join(_terminal_safe(line) for line in lines))


def exit_for_error(exc: Exception) -> None:
    """Exit with an error's message, then its remedy hint on its own line.

    For a ValueError or an XrpcError. The message is one line however much
    of it came from elsewhere (an image decoder's error, a file name, a
    server's reply): a line break inside it is escaped like any other control
    character, so it can never print a line of its own, such as a fake
    prompt. Only the hint, this script's own text, gets a new line.
    """
    hint = getattr(exc, "hint", None)
    exit_error(f"Error: {exc}", *([hint] if isinstance(hint, str) and hint else []))


def run_self_tests() -> None:
    """Run test_create_bsky_post.py, which ships alongside this script."""
    import importlib.util

    path = Path(__file__).resolve().with_name("test_create_bsky_post.py")
    if not path.is_file():
        exit_error(f"Self-tests not found: {path}")
    # Loaded by path, so it works under `python -I` too, and sharing this
    # module keeps the tests from importing and patching a second copy of it.
    sys.modules.setdefault("create_bsky_post", sys.modules[__name__])
    spec = importlib.util.spec_from_file_location("test_create_bsky_post", path)
    tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tests)
    tests.run_self_tests()


def _validate_image_args(args: argparse.Namespace) -> None:
    # Path("") is ".", so a blank --image would otherwise surface much later
    # as a confusing "not a regular file: ." (or not at all, before login).
    if any(not path.strip() for path in args.image or ()):
        raise ValueError("--image was given an empty value.")
    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        raise ValueError(f"At most {MAX_IMAGES_PER_POST} images per post.")
    if args.alt_text and not args.image:
        raise ValueError("--alt-text requires --image.")
    if args.alt_text and len(args.alt_text) != len(args.image):
        raise ValueError("--alt-text count must match --image count.")
    for alt_text in args.alt_text or ():
        if len(alt_text) > MAX_ALT_TEXT_CHARS:
            raise ValueError(
                f"Alt text exceeds the {MAX_ALT_TEXT_CHARS:,}-character limit "
                f"(got {len(alt_text):,} characters)."
            )


def _validate_embed_args(args: argparse.Namespace) -> None:
    if args.image and args.embed_url:
        raise ValueError(
            "Use only one media source: --image or --embed-url. "
            "Either may be combined with --embed-ref."
        )
    if args.embed_url:
        _parse_url(args.embed_url)
    if args.embed_ref:
        parse_uri(args.embed_ref)


def _validate_text_length(text: str) -> None:
    byte_length = len(text.encode("UTF-8"))
    if byte_length > MAX_POST_BYTES:
        raise ValueError(
            f"Post text exceeds the {MAX_POST_BYTES:,}-byte limit "
            f"(got {byte_length:,} bytes)."
        )
    # Extended grapheme segmentation is not available in Python's standard
    # library, so the separate 300-grapheme schema limit is enforced by the PDS.


def _is_valid_language_tag(language_tag: str) -> bool:
    if (
        not isinstance(language_tag, str)
        or not language_tag
        or len(language_tag) > 255
        # Before lower(): Unicode case mapping folds some non-ASCII letters
        # into ASCII ("\u212a".lower() == "k"), which would let them through.
        or not language_tag.isascii()
    ):
        return False
    normalized = language_tag.lower()
    if normalized in GRANDFATHERED_LANGUAGE_TAGS:
        return True
    subtags = normalized.split("-")
    # From here on every subtag is 1 to 8 alphanumerics.
    if any(
        not subtag
        or len(subtag) > 8
        or not subtag.isalnum()
        for subtag in subtags
    ):
        return False

    if subtags[0] == "x":
        return len(subtags) > 1

    language = subtags[0]
    if not language.isalpha() or len(language) < 2:
        return False
    index = 1

    if 2 <= len(language) <= 3:
        extlang_count = 0
        while (
            index < len(subtags)
            and len(subtags[index]) == 3
            and subtags[index].isalpha()
            and extlang_count < 3
        ):
            index += 1
            extlang_count += 1

    if (
        index < len(subtags)
        and len(subtags[index]) == 4
        and subtags[index].isalpha()
    ):
        index += 1

    if index < len(subtags) and (
        (len(subtags[index]) == 2 and subtags[index].isalpha())
        or (len(subtags[index]) == 3 and subtags[index].isdigit())
    ):
        index += 1

    variants: set[str] = set()
    while index < len(subtags):
        subtag = subtags[index]
        is_variant = len(subtag) >= 5 or (len(subtag) == 4 and subtag[0].isdigit())
        if not is_variant:
            break
        if subtag in variants:
            return False
        variants.add(subtag)
        index += 1

    extension_singletons: set[str] = set()
    while (
        index < len(subtags)
        and len(subtags[index]) == 1
        and subtags[index] != "x"
    ):
        singleton = subtags[index]
        if singleton in extension_singletons:
            return False
        extension_singletons.add(singleton)
        index += 1
        extension_start = index
        while index < len(subtags) and len(subtags[index]) >= 2:
            index += 1
        if index == extension_start:
            return False

    if index < len(subtags) and subtags[index] == "x":
        return index + 1 < len(subtags)  # private use: any subtags, at least one

    return index == len(subtags)


def _validate_language_args(language_tags: Optional[List[str]]) -> None:
    if not language_tags:
        return
    if len(language_tags) > MAX_LANGS:
        raise ValueError(f"At most {MAX_LANGS} language tags may be supplied.")
    invalid = [tag for tag in language_tags if not _is_valid_language_tag(tag)]
    if invalid:
        raise ValueError(f"Invalid BCP 47 language tag: {invalid[0]!r}")


# Flags whose explicit empty value must be refused rather than read as "not
# given": a blank --reply-to would otherwise post at top level, a blank embed
# flag would silently drop the embed (and slip past the media exclusivity
# check), and a blank --password would quietly fall back to the environment.
NON_EMPTY_FLAGS = (
    ("--reply-to", "reply_to"),
    ("--embed-url", "embed_url"),
    ("--embed-ref", "embed_ref"),
    ("--handle", "handle"),
    ("--password", "password"),
)


def _reject_empty_flag_values(args: argparse.Namespace) -> None:
    for flag, name in NON_EMPTY_FLAGS:
        value = getattr(args, name, None)
        if value is not None and not value.strip():
            raise ValueError(f"{flag} was given an empty value.")


def validate_args(args: argparse.Namespace) -> None:
    _reject_empty_flag_values(args)
    args.pds_url = normalize_service_url(
        args.pds_url,
        service_name="PDS",
        allow_insecure=args.allow_insecure_pds,
    )
    args.record_service_url = normalize_service_url(
        args.record_service_url,
        service_name="record service",
        allow_insecure=args.allow_insecure_pds,
    )

    _validate_image_args(args)
    _validate_embed_args(args)
    _validate_language_args(args.lang)

    if args.reply_to:
        _require_post_uri(args.reply_to, "Reply target")
    _validate_text_length(args.text)


def _missing_content_usage(prog: str) -> tuple[str, ...]:
    return (
        "Error: Post text or an embed is required.",
        "",
        "First, set your credentials as environment variables. The hidden",
        "prompt keeps the password out of your shell history:",
        "  $ export ATP_AUTH_HANDLE='your-handle.bsky.social'",
        "  $ read -r -s -p 'Bluesky app password: ' ATP_AUTH_PASSWORD",
        "  $ printf '\\n'",
        "  $ export ATP_AUTH_PASSWORD",
        "",
        "Run 'unset ATP_AUTH_PASSWORD' when you finish posting.",
        "",
        "ATP_AUTH_PASSWORD must be an APP password, created at",
        "https://bsky.app/settings/app-passwords - do NOT use your main",
        "account password.",
        "",
        "Then create a post. For example, an image post with alt text and",
        "standard post body text:",
        f'  $ python3 {prog} "Sunset over the bay" --image sunset.jpg --alt-text "Orange sunset over a calm bay"',
        "",
        "Other examples:",
        f'  $ python3 {prog} "Hello, Bluesky! #greetings"',
        f'  $ python3 {prog} "Worth a read" --embed-url "https://example.com/article"',
        "",
        f"Run 'python3 {prog} --help' for the full list of options.",
    )


def main():
    parser = argparse.ArgumentParser(
        description="Create a Bluesky post with optional facets, replies, and embeds",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            'Examples:\n  %(prog)s "Hello, Bluesky!"\n'
            '  %(prog)s "Check this out!" --image photo.jpg --alt-text "A photo"\n'
            '  %(prog)s "Quoted with media" --embed-ref "at://did:plc:xxx/app.bsky.feed.post/yyy" --image photo.jpg\n'
            '  %(prog)s "Replying" --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"'
        ),
    )
    # `or DEFAULT` rather than a get() default: a variable exported as empty
    # ("export ATP_PDS_HOST=") should mean "unset", not an empty URL that fails
    # later with an unattributable "Invalid URL".
    parser.add_argument("--pds-url",
                        default=os.environ.get("ATP_PDS_HOST") or DEFAULT_PDS_URL,
                        help=f"PDS URL (default: {DEFAULT_PDS_URL} or ATP_PDS_HOST env var)")
    parser.add_argument(
        "--record-service-url",
        default=(
            os.environ.get("ATP_RECORD_SERVICE_HOST") or DEFAULT_RECORD_SERVICE_URL
        ),
        help=(
            "Network-wide record lookup service used for replies and quotes "
            f"(default: {DEFAULT_RECORD_SERVICE_URL} or ATP_RECORD_SERVICE_HOST)"
        ),
    )
    parser.add_argument("--allow-insecure-pds", action="store_true",
                        help="Allow HTTP service URLs on localhost/loopback only")
    # Read from ATP_AUTH_HANDLE after validation, like the password, so only
    # an explicit --handle can be reported as an empty flag value.
    parser.add_argument("--handle", default=None,
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
                        help="BCP 47 language tag (e.g., 'en'; at most 3)")
    parser.add_argument("--reply-to", metavar="URI", help="URI of post to reply to")
    parser.add_argument("--embed-url", metavar="URL", help="URL to embed as a link card")
    parser.add_argument(
        "--embed-ref",
        metavar="URI",
        help="URI of post/record to quote; may be combined with image or link card",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print the complete pending record, and the body of a failed "
             "createRecord, to stderr",
    )
    parser.add_argument("--self-test", "--test", action="store_true", dest="self_test",
                        help="Run local parser/validation tests and exit")

    args = parser.parse_args()

    if args.self_test:
        run_self_tests()
        print("self-tests passed")
        return

    # Whitespace-only text is not text: it carries no content and the PDS
    # rejects the resulting record. Blank text alongside an embed is fine.
    # An explicit but empty embed flag counts as given here, so validate_args
    # reports it instead of this check calling it a missing embed.
    if not (
        args.text.strip()
        or args.image
        or args.embed_url is not None
        or args.embed_ref is not None
    ):
        exit_error(*_missing_content_usage(parser.prog))

    try:
        validate_args(args)
    except ValueError as e:
        exit_for_error(e)

    if args.password is not None:  # never blank: validate_args refused that
        print(
            "warning: passing --password on the command line exposes it to other local "
            "users via `ps`; prefer the ATP_AUTH_PASSWORD environment variable.",
            file=sys.stderr,
        )
    else:
        args.password = _environment_credential("ATP_AUTH_PASSWORD")
    if args.handle is None:
        args.handle = _environment_credential("ATP_AUTH_HANDLE")

    if not args.handle or not args.password:
        exit_error(
            "Error: Both handle and password are required.",
            "Set ATP_AUTH_HANDLE and ATP_AUTH_PASSWORD environment variables,",
            "or use --handle and --password arguments.",
        )

    # Local files are read and decoded before logging in, so a missing or
    # malformed image fails without any network request having been made.
    args.prepared_images = None
    if args.image:
        try:
            args.prepared_images = prepare_images(args.image, args.alt_text)
        except ValueError as e:
            exit_for_error(e)

    try:
        create_post(args)
    except (XrpcError, ValueError) as e:
        exit_for_error(e)
    except requests.RequestException as e:
        exit_error(f"Error: API request failed: {e}")


if __name__ == "__main__":
    main()
