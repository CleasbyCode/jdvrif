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
   them; a Session per request, so a call abandoned at its deadline can
   never share connection state with a later one

 * The script attempts to revoke the login session (deleteSession) when
   the run ends (best effort: a failure only warns), and posts use a client-chosen record key so an ambiguous createRecord
   timeout is settled by lookup rather than risking a duplicate post

 * Strict validation of AT URIs, record CIDs, handles, language tags,
   image files (symlink-safe reads, preflight dimensions, bounded full
   decoding with unchanged upload bytes), and URLs

Setup:

 Requires Python 3.10+: requests, beautifulsoup4, pillow (with pinned versions in
 requirements.txt alongside this script)
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
import binascii
import errno
import io
import ipaddress
import json
import math
import os
import queue
import re
import secrets
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
import warnings
from bisect import bisect_left
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from contextvars import ContextVar, copy_context
from datetime import datetime, timezone
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Dict, Iterator, List, Optional
from urllib.parse import ParseResult, urljoin, urlparse, urlunparse

import idna
import requests
from urllib3 import __version__ as _URLLIB3_VERSION
from requests.adapters import HTTPAdapter
from PIL import Image

if TYPE_CHECKING:
    # Imported where the page is parsed instead: the image-validation worker
    # re-runs this module and never parses HTML, so it skips bs4's import cost.
    from bs4 import BeautifulSoup


DEFAULT_PDS_URL = "https://bsky.social"
DEFAULT_RECORD_SERVICE_URL = "https://public.api.bsky.app"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 2_000_000
MAX_POST_BYTES = 3_000
MAX_TAG_GRAPHEMES = 64
MAX_TAG_BYTES = 640
MAX_LANGS = 3
# Generous: real article pages often exceed 1 MB, and the cap only has to stop
# an unbounded read.
MAX_EMBED_HTML_BYTES = 4_000_000
MAX_EMBED_IMAGE_BYTES = 1_000_000
MAX_API_RESPONSE_BYTES = 8_000_000
MAX_IMAGE_PIXELS = 40_000_000
MAX_IMAGE_DIMENSION = 16_384
MAX_IMAGE_DECODE_SECONDS = 5.0
# One worker validates every image in a post. Its interpreter start-up (which
# re-imports this module's dependencies) gets its own allowance, so a slow
# machine does not eat into the per-image decode budget.
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
# C0 controls (including CR/LF/TAB), DEL, and the C1 block: escaped in all
# remote text before printing. Printable non-ASCII is left intact.
TERMINAL_UNSAFE_CHARACTERS = re.compile(r"[\x00-\x1f\x7f-\x9f]")
# The link-card parse runs after the download deadline, so it has its own
# budget. Measured: 4 MB of prose parses in ~1s, 4 MB of hostile shallow
# markup in ~13s.
MAX_EMBED_PARSE_SECONDS = 5.0
# Link-card metadata lives in <head>, so only that part of the page is parsed.
# Pages whose head cannot be located fall back to this many leading bytes.
MAX_EMBED_HEAD_BYTES = 1_000_000
HTML_HEAD_END_REGEX = re.compile(rb"</head[\s>]", re.IGNORECASE)
HTML_BODY_START_REGEX = re.compile(rb"<body[\s>/]", re.IGNORECASE)
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
MAX_DOWNLOAD_ADDRESS_ATTEMPTS = 4
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
MENTION_REGEX = re.compile(
    r"(?<![\w@])"
    r"(@(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+"
    r"[A-Za-z](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)"
    r"(?![A-Za-z0-9-])"
)
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
DID_REGEX = re.compile(
    r"^did:[a-z0-9]+:"
    r"(?:[A-Za-z0-9._-]|%[0-9A-Fa-f]{2})+"
    r"(?::(?:[A-Za-z0-9._-]|%[0-9A-Fa-f]{2})+)*$"
)
NSID_REGEX = re.compile(
    r"^[a-zA-Z](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?"
    r"(?:\.[a-zA-Z0-9](?:[a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)+"
    r"\.[a-zA-Z][a-zA-Z0-9]{0,62}$"
)
RECORD_KEY_REGEX = re.compile(r"^[A-Za-z0-9._:~-]{1,512}$")
INVALID_PERCENT_ESCAPE_REGEX = re.compile(r"%(?![0-9A-Fa-f]{2})")
# RFC 5646 grandfathered tags: irregular forms predating the current subtag
# grammar, so they are matched literally rather than parsed.
GRANDFATHERED_LANGUAGE_TAGS = frozenset(
    "art-lojban cel-gaulish en-gb-oed i-ami i-bnn i-default i-enochian "
    "i-hak i-klingon i-lux i-mingo i-navajo i-pwn i-tao i-tay i-tsu "
    "no-bok no-nyn sgn-be-fr sgn-be-nl sgn-ch-de zh-guoyu zh-hakka zh-min "
    "zh-min-nan zh-xiang".split()
)
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
    executed by the terminal emulator. Applied at every print/raise site that
    can carry remote content, so no individual message has to remember to.
    """
    if not isinstance(text, str):
        text = str(text)
    return TERMINAL_UNSAFE_CHARACTERS.sub(
        lambda match: f"\\x{ord(match.group()):02x}",
        text,
    )


class DeadlineExceeded(requests.Timeout):
    """Raised when this script's own wall-clock deadline expires.

    Distinct from requests' own ConnectTimeout/ReadTimeout because only this one
    implies a blocking call was abandoned in a daemon worker thread that may
    still be using the Session it was handed. Callers that own a Session must
    not close it while unwinding from this exception; the abandoned worker owns
    it from that point on.
    """


class _NoRedirectSession(requests.Session):
    """Leave redirects entirely to this script, including their unread bodies.

    Requests otherwise consumes a redirect body while constructing Response.next
    even with stream=True and allow_redirects=False, before our byte limits or
    redirect policy can inspect the response.
    """

    def resolve_redirects(self, resp, req, **kwargs):
        return iter(())


def _new_session() -> requests.Session:
    """Build a Session used by exactly one request (see DeadlineExceeded)."""
    session = _NoRedirectSession()
    session.headers["User-Agent"] = USER_AGENT
    session.headers["Accept-Encoding"] = "identity"
    session.trust_env = False
    return session


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
# Set while a streamed response is open: closes that response and its Session.
# A body read abandoned at the deadline hands this to its worker, because the
# code that opened the response must then leave both open for the worker.
_ABANDONED_RESPONSE_CLEANUP: ContextVar[Optional[Callable[[], None]]] = ContextVar(
    "_ABANDONED_RESPONSE_CLEANUP",
    default=None,
)
_IPV4_TRANSLATION_NETWORKS = (
    ipaddress.IPv6Network("64:ff9b::/96"),
    ipaddress.IPv6Network("::ffff:0:0:0/96"),
)
_DEPRECATED_6TO4_RELAY_NETWORK = ipaddress.IPv4Network("192.88.99.0/24")
_LOCAL_IPV6_TRANSLATION_NETWORK = ipaddress.IPv6Network("64:ff9b:1::/48")
# 6to4 and Teredo are refused by prefix: whether ipaddress counts 2002::/16 as
# private depends on the Python release (3.10 and early 3.11 do not).
_DEPRECATED_IPV6_TRANSITION_NETWORKS = (
    ipaddress.IPv6Network("2002::/16"),
    ipaddress.IPv6Network("2001::/32"),
)
# RFC 6052 well-known NAT64 prefix. Disjoint from the local-use /48 above.
_WELL_KNOWN_NAT64_NETWORK = ipaddress.IPv6Network("64:ff9b::/96")


def _bracketed_host(host: str) -> str:
    """Wrap an IPv6 literal in brackets so it can sit in a URL authority."""
    return f"[{host}]" if ":" in host else host


def _api_url(pds_url: str, method: str) -> str:
    return f"{pds_url.rstrip('/')}/xrpc/{method}"


def _url_for_log(url: str) -> str:
    try:
        parsed = urlparse(url)
        hostname = parsed.hostname
        port = parsed.port
    except (TypeError, ValueError):
        return "<redacted URL>"
    if not parsed.scheme or not hostname:
        return "<redacted URL>"
    authority = _bracketed_host(hostname)
    if port is not None:
        authority = f"{authority}:{port}"
    suffix = "/…" if parsed.path not in ("", "/") else parsed.path
    return f"{parsed.scheme.lower()}://{authority}{suffix}"


def _parse_url(url: str, *, schemes: tuple[str, ...]) -> ParseResult:
    if (
        not url
        or any(ch.isspace() or ord(ch) < 0x20 or ord(ch) == 0x7F for ch in url)
        or INVALID_PERCENT_ESCAPE_REGEX.search(url)
    ):
        raise ValueError("Invalid URL")
    try:
        parsed = urlparse(url)
    except ValueError as exc:
        raise ValueError("Invalid URL") from exc
    scheme = parsed.scheme.lower()
    allowed_schemes = tuple(candidate.lower() for candidate in schemes)
    if scheme not in allowed_schemes:
        joined = ", ".join(schemes)
        raise ValueError(f"URL must use one of these schemes: {joined}")
    if not parsed.hostname:
        raise ValueError("URL has no host")
    if parsed.username or parsed.password:
        raise ValueError("URL must not include credentials")
    try:
        parsed.port
    except ValueError as exc:
        raise ValueError("URL has an invalid port") from exc
    return parsed


def _is_loopback_hostname(hostname: str) -> bool:
    normalized = hostname.rstrip(".").lower()
    if normalized == "localhost" or normalized.endswith(".localhost"):
        return True
    try:
        return ipaddress.ip_address(normalized).is_loopback
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
    parsed = _parse_url(service_url.strip(), schemes=("http", "https"))
    if parsed.path not in ("", "/") or parsed.params or parsed.query or parsed.fragment:
        raise ValueError(
            f"{service_name} URL must be only a scheme and host, "
            "without path/query/fragment"
        )
    scheme = parsed.scheme.lower()
    if scheme != "https" and not allow_insecure:
        raise ValueError(
            f"Refusing to use a non-HTTPS {service_name} URL "
            "(use --allow-insecure-pds only for local testing)"
        )
    if scheme != "https" and not _is_loopback_hostname(parsed.hostname or ""):
        raise ValueError(
            f"Insecure {service_name} URLs are limited to localhost or loopback IPs"
        )
    return urlunparse((scheme, parsed.netloc, "", "", "", "")).rstrip("/")


def _remaining_download_time(deadline: float, operation: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise DeadlineExceeded(
            f"{_NETWORK_TIMEOUT_SUBJECT.get()} timed out while {operation}"
        )
    return remaining


@contextmanager
def _network_deadline(timeout: float, *, subject: str, label: str) -> Iterator[float]:
    """Scope one network operation to a wall-clock deadline.

    An inner call never extends an outer one: the effective deadline is the
    earlier of the two, so a link-card fetch cannot outlive the budget of the
    operation that started it. Both context variables are restored on exit.
    """
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError(f"{label} timeout must be a positive finite number")
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
) -> Any:
    """Run one blocking operation without allowing it past the total deadline.

    On timeout the blocking call is abandoned in its daemon worker thread while
    the caller raises DeadlineExceeded. The abandoned worker keeps whatever
    Session it was handed, and because every request gets its own Session
    (_new_session), it cannot interfere with any later request. Callers may
    therefore catch a timeout and continue; they must only avoid closing the
    Session the worker was given. Whenever this raises DeadlineExceeded,
    `dispose_abandoned` runs exactly once: with the worker's result (or None
    if it failed) as soon as the worker is done, so the resources it was using
    are released only when nothing is still using them.
    """
    outcome: queue.Queue[tuple[bool, Any]] = queue.Queue(maxsize=1)
    state_lock = threading.Lock()
    abandoned = [False]

    def abandon_pending_outcome() -> None:
        late_outcome: Optional[tuple[bool, Any]] = None
        with state_lock:
            abandoned[0] = True
            try:
                late_outcome = outcome.get_nowait()
            except queue.Empty:
                pass
        # The worker already finished, so it will not dispose of anything.
        if late_outcome is not None and dispose_abandoned is not None:
            try:
                dispose_abandoned(late_outcome[1] if late_outcome[0] else None)
            except Exception:
                pass

    def worker() -> None:
        try:
            value = action()
            succeeded = True
        except Exception as exc:
            value = exc
            succeeded = False

        should_dispose = False
        with state_lock:
            if abandoned[0]:
                should_dispose = dispose_abandoned is not None
            else:
                outcome.put_nowait((succeeded, value))
        if should_dispose and dispose_abandoned is not None:
            try:
                dispose_abandoned(value if succeeded else None)
            except Exception:
                pass

    thread = threading.Thread(
        target=worker,
        name="bsky-download-operation",
        daemon=True,
    )
    # Do not launch work when an inherited deadline has already expired. The
    # caller treats this like any other DeadlineExceeded and leaves cleanup to
    # `dispose_abandoned`, so run it here: no worker will.
    try:
        _remaining_download_time(deadline, operation)
    except DeadlineExceeded:
        if dispose_abandoned is not None:
            try:
                dispose_abandoned(None)
            except Exception:
                pass
        raise
    thread.start()
    try:
        succeeded, value = outcome.get(
            timeout=_remaining_download_time(deadline, operation)
        )
    except DeadlineExceeded:
        abandon_pending_outcome()
        raise
    except queue.Empty as exc:
        abandon_pending_outcome()
        raise DeadlineExceeded(
            f"{_NETWORK_TIMEOUT_SUBJECT.get()} timed out while {operation}"
        ) from exc

    if not succeeded:
        raise value
    return value


def _embedded_ipv4_addresses(
    address: ipaddress.IPv6Address,
) -> List[ipaddress.IPv4Address]:
    embedded: List[ipaddress.IPv4Address] = []
    if address.ipv4_mapped is not None:
        embedded.append(address.ipv4_mapped)
    if address.sixtofour is not None:
        embedded.append(address.sixtofour)
    if address.teredo is not None:
        embedded.extend(address.teredo)
    for network in _IPV4_TRANSLATION_NETWORKS:
        if address in network:
            translated = ipaddress.IPv4Address(int(address) & 0xFFFFFFFF)
            if translated not in embedded:
                embedded.append(translated)
    return embedded


def _is_public_unicast_address(
    address: ipaddress.IPv4Address | ipaddress.IPv6Address,
) -> bool:
    if isinstance(address, ipaddress.IPv6Address):
        # Local-use translation prefix: never a destination we should reach.
        if address in _LOCAL_IPV6_TRANSLATION_NETWORK or any(
            address in network for network in _DEPRECATED_IPV6_TRANSITION_NETWORKS
        ):
            return False
        if address in _WELL_KNOWN_NAT64_NETWORK:
            # On a DNS64/NAT64 network every public host resolves into this
            # prefix, and Python reports the whole /96 as is_reserved -- which
            # would refuse every link-card and og:image fetch on IPv6-only
            # networks. Such an address *is* its embedded IPv4, so judge it by
            # that instead, which keeps 64:ff9b::169.254.169.254 refused. The
            # emptiness guard is load-bearing: all(()) is True.
            embedded = _embedded_ipv4_addresses(address)
            return bool(embedded) and all(
                _is_public_unicast_address(candidate) for candidate in embedded
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
    if (
        isinstance(address, ipaddress.IPv4Address)
        and address in _DEPRECATED_6TO4_RELAY_NETWORK
    ):
        return False
    if isinstance(address, ipaddress.IPv6Address):
        return all(
            _is_public_unicast_address(embedded)
            for embedded in _embedded_ipv4_addresses(address)
        )
    return True


def _public_url_addresses(url: str) -> tuple[ParseResult, List[str]]:
    """Resolve a URL once and return only validated public connection targets."""
    parsed = _parse_url(url, schemes=("http", "https"))
    port = (
        parsed.port
        if parsed.port is not None
        else (443 if parsed.scheme.lower() == "https" else 80)
    )
    # Resolve the same A-label that TLS sends as SNI and authenticates. The raw
    # Unicode name would go through CPython's IDNA2003 codec, which encodes
    # some labels ('ß', ZWJ) differently.
    hostname = _ascii_hostname(parsed.hostname or "")

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
    return parsed, addresses


def _ascii_hostname(hostname: str) -> str:
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
        return hostname
    # A DNS name never exceeds 253 characters. Refusing longer input before
    # idna.encode also keeps attacker-chosen hostnames (redirect targets,
    # og:image URLs) away from idna's slow paths on over-long input
    # (CVE-2024-3651, CVE-2026-45409).
    if len(hostname.rstrip(".")) > 253:
        raise ValueError("Internationalized hostname exceeds 253 characters")
    try:
        return idna.encode(hostname, uts46=True).decode("ascii")
    except idna.IDNAError as exc:
        raise ValueError(f"Invalid internationalized hostname: {hostname!r}") from exc


def _original_authority(parsed: ParseResult) -> str:
    hostname = _bracketed_host(_ascii_hostname(parsed.hostname or ""))
    return f"{hostname}:{parsed.port}" if parsed.port is not None else hostname


def _pinned_url(parsed: ParseResult, address: str) -> str:
    """Replace the URL authority with a previously validated IP literal."""
    authority = _bracketed_host(address)
    if parsed.port is not None:
        authority = f"{authority}:{parsed.port}"
    return urlunparse(parsed._replace(netloc=authority))


class _PinnedHTTPSAdapter(HTTPAdapter):
    """Connect to an IP literal while authenticating the URL's original hostname.

    The whole SSRF pinning scheme rests on urllib3 honouring two pool options:
    `server_hostname` (the SNI name sent in the TLS handshake) and
    `assert_hostname` (the name the presented certificate is verified against).
    Passing these via `connection_pool_kw` is supported by urllib3 >= 2, which
    requirements.txt pins; older releases handle them differently, so the pin is
    load-bearing rather than cosmetic.
    """

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
    timeout: float,
) -> Iterator[requests.Response]:
    """Open one response using one of a bounded set of validated addresses."""
    parsed, addresses = _public_url_addresses(url)
    deadline = _DOWNLOAD_DEADLINE.get()
    last_error: Optional[requests.RequestException] = None
    for address in addresses[:MAX_DOWNLOAD_ADDRESS_ATTEMPTS]:
        session = _new_session()
        if parsed.scheme.lower() == "https":
            session.mount("https://", _PinnedHTTPSAdapter(parsed.hostname or ""))
        try:
            request_timeout = (
                timeout
                if deadline is None
                else _remaining_download_time(
                    deadline,
                    f"connecting to {_url_for_log(url)!r}",
                )
            )

            def request() -> requests.Response:
                return session.get(
                    _pinned_url(parsed, address),
                    headers={"Host": _original_authority(parsed)},
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
                    f"connecting to {_url_for_log(url)!r}",
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
                )
            )
        except DeadlineExceeded:
            # Leave `session` alone: a daemon worker may still be using it.
            raise
        except requests.RequestException as exc:
            # A per-address failure (including requests' own connect/read
            # timeouts) leaves nothing running, so this Session can be closed
            # and the next address tried -- unless the deadline is already gone.
            last_error = exc
            session.close()
            if deadline is not None and time.monotonic() >= deadline:
                raise DeadlineExceeded(
                    "External download timed out while connecting to "
                    f"{_url_for_log(url)!r}"
                ) from exc
            continue

        abandoned = False
        cleanup_token = _ABANDONED_RESPONSE_CLEANUP.set(
            lambda owned_response=response, owned=session: _close_quietly(
                owned_response,
                owned,
            )
        )
        try:
            yield response
        except DeadlineExceeded:
            abandoned = True
            raise
        finally:
            _ABANDONED_RESPONSE_CLEANUP.reset(cleanup_token)
            if not abandoned:
                response.close()
                session.close()
        return

    if last_error is not None:
        raise requests.RequestException(
            "External download request failed for "
            f"{_url_for_log(url)!r} ({type(last_error).__name__})"
        ) from last_error
    raise ValueError(
        f"Could not connect to any address for {_url_for_log(url)!r}"
    )


def _declared_content_length(resp: requests.Response) -> Optional[int]:
    declared = resp.headers.get("Content-Length", "").strip()
    if not declared:
        return None
    try:
        return int(declared)
    except ValueError:
        return None


def _urllib3_has_bounded_decoding() -> bool:
    # Fail closed on older and unrecognized versions, including prereleases.
    # Before 2.7.0, streaming decoders could allocate the expanded body before
    # Requests yields a chunk to the caller's size check; before 2.8.0,
    # chunked deflate streaming could loop forever (GHSA-gh4c-6fx4-qh6g).
    version = re.fullmatch(r"(\d+)\.(\d+)\.(\d+)", _URLLIB3_VERSION)
    return version is not None and tuple(map(int, version.groups())) >= (2, 8, 0)


def _read_response_body(resp: requests.Response, max_bytes: int) -> bytes:
    # Accept-Encoding is advisory: validate the actual response coding before
    # reading. With a patched decoder, the chunk limit also bounds allocation
    # inside urllib3 instead of merely checking an already-expanded body.
    content_encoding = resp.headers.get("Content-Encoding", "").strip().lower()
    if content_encoding not in DECODABLE_CONTENT_ENCODINGS:
        raise ValueError(
            "Refusing remote response with undecodable Content-Encoding "
            f"{content_encoding!r}"
        )
    is_encoded = content_encoding not in ("", "identity")
    if is_encoded and not _urllib3_has_bounded_decoding():
        raise ValueError(
            "Refusing compressed response: urllib3 2.8.0 or newer is required "
            "for bounded decompression"
        )

    # Content-Length describes the encoded body, so it only bounds the decoded
    # size when no coding was applied.
    declared = _declared_content_length(resp)
    if declared is not None and declared < 0:
        raise ValueError("Remote response declares a negative Content-Length")
    if declared is not None and not is_encoded and declared > max_bytes:
        raise ValueError(
            f"Remote response declares {declared} bytes, above {max_bytes} limit"
        )

    def read_body() -> bytes:
        body = bytearray()
        for chunk in resp.iter_content(DOWNLOAD_CHUNK_SIZE):
            if not chunk:
                continue
            if len(body) + len(chunk) > max_bytes:
                raise ValueError(f"Remote response exceeds {max_bytes} bytes")
            body.extend(chunk)
        return bytes(body)

    deadline = _DOWNLOAD_DEADLINE.get()
    if deadline is None:
        return read_body()
    cleanup = _ABANDONED_RESPONSE_CLEANUP.get()
    return _run_before_download_deadline(
        read_body,
        deadline,
        "reading the response body",
        dispose_abandoned=(
            None if cleanup is None else lambda _late_body: cleanup()
        ),
    )


def _redirect_target(resp: requests.Response, current_url: str) -> str:
    location = resp.headers.get("Location")
    if not location:
        raise ValueError(
            f"Redirect from {_url_for_log(current_url)!r} missing Location header"
        )
    return urljoin(current_url, location)


def _charset_from_content_type(content_type: Optional[str]) -> Optional[str]:
    """Extract a charset label from a Content-Type header, if it declares one."""
    if not content_type:
        return None
    for parameter in content_type.split(";")[1:]:
        key, separator, value = parameter.strip().partition("=")
        if separator and key.strip().lower() == "charset":
            charset = value.strip().strip('"').strip("'")
            return charset or None
    return None


def _media_type(content_type: Optional[str]) -> str:
    """Return the lower-cased media type of a Content-Type header, or ''."""
    return (content_type or "").split(";", 1)[0].strip().lower()


def _safe_download(
    url: str,
    max_bytes: int,
    timeout: float = 10,
    *,
    allowed_content_types: Optional[frozenset[str]] = None,
) -> tuple[bytes, str, Optional[str]]:
    """Fetch a URL through pinned public addresses.

    Returns the body, the final URL after any redirects, and the declared
    Content-Type header (or None) so callers can honour a server-declared
    charset when decoding text. With `allowed_content_types`, a response that
    declares any other media type is refused before its body is read; one that
    declares none is still accepted.
    """
    with _network_deadline(
        timeout,
        subject="External download",
        label="Download",
    ) as deadline:
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
                if 300 <= status_code < 400:
                    if redirects_followed >= MAX_REDIRECTS:
                        raise ValueError(
                            f"Too many redirects (> {MAX_REDIRECTS}) "
                            f"following {_url_for_log(url)!r}"
                        )
                    redirects_followed += 1
                    redirect_url = _redirect_target(resp, current)
                    current_parsed = _parse_url(
                        current,
                        schemes=("http", "https"),
                    )
                    redirect_parsed = _parse_url(
                        redirect_url,
                        schemes=("http", "https"),
                    )
                    if (
                        current_parsed.scheme.lower() == "https"
                        and redirect_parsed.scheme.lower() != "https"
                    ):
                        raise ValueError(
                            "Refusing HTTPS-to-HTTP redirect from "
                            f"{_url_for_log(current)!r} to "
                            f"{_url_for_log(redirect_url)!r}"
                        )
                    current = redirect_url
                    continue
                try:
                    resp.raise_for_status()
                except requests.RequestException as exc:
                    raise requests.RequestException(
                        "External download returned HTTP "
                        f"{status_code} for {_url_for_log(current)!r}"
                    ) from exc
                media_type = _media_type(resp.headers.get("Content-Type"))
                if (
                    allowed_content_types is not None
                    and media_type
                    and media_type not in allowed_content_types
                ):
                    raise ValueError(
                        f"Refusing {media_type!r} response from "
                        f"{_url_for_log(current)!r}"
                    )
                return (
                    _read_response_body(resp, max_bytes),
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
    with _network_deadline(timeout, subject="API request", label="API") as deadline:
        session = _new_session()
        response: Optional[requests.Response] = None
        abandoned = False

        def request() -> requests.Response:
            return session.request(
                method,
                url,
                timeout=timeout,
                allow_redirects=False,
                stream=True,
                **request_kwargs,
            )

        try:
            try:
                response = _run_before_download_deadline(
                    request,
                    deadline,
                    operation,
                    dispose_abandoned=(
                        lambda late_response: _close_quietly(late_response, session)
                    ),
                )
                cleanup_token = _ABANDONED_RESPONSE_CLEANUP.set(
                    lambda owned=response: _close_quietly(owned, session)
                )
                try:
                    yield response
                finally:
                    _ABANDONED_RESPONSE_CLEANUP.reset(cleanup_token)
            except DeadlineExceeded:
                # Either the request or a body read inside the caller's block was
                # abandoned mid-flight; the daemon worker still owns `session` and
                # `response`, so neither may be closed from here.
                abandoned = True
                raise
        finally:
            # The context variables are restored by _network_deadline once these
            # closes are done, preserving the original unwind order.
            if not abandoned:
                if response is not None:
                    _close_quietly(response)
                _close_quietly(session)


class XrpcError(requests.HTTPError):
    """An XRPC call answered with an error status; keeps the server's error name."""

    def __init__(
        self,
        message: str,
        *,
        response: requests.Response,
        error_name: Optional[str],
    ):
        super().__init__(message, response=response)
        self.error_name = error_name


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
    lower-cased error names to a remedy printed on the following line.
    """
    if 300 <= resp.status_code < 400:
        raise ValueError(f"Refusing redirect from {method} request")
    if resp.ok:
        return
    body = _error_response_body(resp)
    if verbose:
        print(json.dumps(body, indent=2), file=sys.stderr)
    error_name = _api_error_name(body)
    hint = (hints or {}).get((error_name or "").lower())
    # _api_error_summary already sanitizes the server's text.
    raise XrpcError(
        f"{label or method} failed with HTTP {resp.status_code}"
        f"{_api_error_summary(body)}."
        + (f"\n{hint}" if hint else ""),
        response=resp,
        error_name=error_name,
    )


def _decode_json(body: bytes, context: str) -> Any:
    # The byte cap does not bound nesting depth: a few tens of KB of brackets
    # exhaust the recursion limit, so that failure is reported like any other
    # malformed body. (JSONDecodeError and UnicodeDecodeError are ValueErrors.)
    try:
        return json.loads(body)
    except (ValueError, RecursionError) as exc:
        raise ValueError(f"{context} returned a non-JSON response") from exc


def _json_object(resp: requests.Response, context: str) -> Dict[str, Any]:
    data = _decode_json(_read_response_body(resp, MAX_API_RESPONSE_BYTES), context)
    if not isinstance(data, dict):
        raise ValueError(f"{context} returned an unexpected JSON shape")
    return data


def _error_response_body(resp: requests.Response) -> Any:
    """Best-effort body of an error response; never hides the status itself."""
    try:
        body = _read_response_body(resp, MAX_API_RESPONSE_BYTES)
    except ValueError:
        return None
    try:
        return _decode_json(body, "error response")
    except ValueError:
        return {"raw": body.decode("UTF-8", errors="replace")}


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
    message = body.get("message") if isinstance(body, dict) else None
    if isinstance(message, str) and message.strip():
        detail = _terminal_safe(
            _grapheme_safe_prefix(message.strip(), MAX_ERROR_DETAIL_CHARS)
        )
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
    if not isinstance(data.get("accessJwt"), str) or not isinstance(data.get("did"), str):
        raise ValueError("createSession response is missing accessJwt or did")
    return data


def bsky_logout_session(pds_url: str, refresh_token: str) -> None:
    """Revoke the login's refresh token so it does not outlive this run.

    Best effort: a failure only means the session expires on the PDS's own
    schedule, which is exactly what happened before this call existed.
    """
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
        print(
            "warning: could not end the login session "
            f"({type(exc).__name__}: {_terminal_safe(exc)}); "
            "it will expire on its own.",
            file=sys.stderr,
        )


def _byte_offsets(text: str) -> List[int]:
    offsets = [0]
    for character in text:
        offsets.append(offsets[-1] + len(character.encode("UTF-8")))
    return offsets


def _has_wordlike_prefix(text: str, start: int) -> bool:
    if start == 0:
        return False
    category = unicodedata.category(text[start - 1])
    return category[0] in ("L", "M", "N") or category in ("Pc", "Cf")


def parse_mentions(text: str, byte_offsets: Optional[List[int]] = None) -> List[Dict]:
    if byte_offsets is None:
        byte_offsets = _byte_offsets(text)
    mentions: List[Dict] = []
    for match in MENTION_REGEX.finditer(text):
        start = match.start(1)
        handle = match.group(1)[1:]
        if (
            _has_wordlike_prefix(text, start)
            or len(handle) > 253
            or HANDLE_REGEX.fullmatch(handle) is None
        ):
            continue
        mentions.append(
            {
                "start": byte_offsets[start],
                "end": byte_offsets[match.end(1)],
                "handle": handle,
            }
        )
    return mentions


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


def parse_urls(text: str, byte_offsets: Optional[List[int]] = None) -> List[Dict]:
    if byte_offsets is None:
        byte_offsets = _byte_offsets(text)
    spans: List[Dict] = []
    for match in URL_REGEX.finditer(text):
        if _has_wordlike_prefix(text, match.start(1)):
            continue
        url_bytes = _trim_url_match(match.group(1).encode("UTF-8"))
        if not url_bytes:
            continue
        try:
            url = url_bytes.decode("UTF-8")
            _parse_url(url, schemes=("http", "https"))
        except (UnicodeDecodeError, ValueError):
            continue
        start = byte_offsets[match.start(1)]
        end = start + len(url_bytes)
        spans.append({"start": start, "end": end, "url": url})
    return spans


# HASHTAG_REGEX already ends a tag at these; the check below keeps direct
# callers of _valid_tag_value to the same rule. U+FE0F is only refused as the
# first character, matching the official composer ("#\u2764\ufe0flove" stays a tag).
TAG_FORBIDDEN_CHARACTERS = frozenset(
    "\u00ad\u2060\u200a\u200b\u200c\u200d\u20e2"
)


def _strip_tag_trailing_punctuation(tag: str) -> str:
    while tag and unicodedata.category(tag[-1]).startswith("P"):
        tag = tag[:-1]
    return tag


def _valid_tag_value(tag: str) -> bool:
    if (
        not tag
        or tag[0] == "️"
        or len(tag.encode("UTF-8")) > MAX_TAG_BYTES
        or any(character in TAG_FORBIDDEN_CHARACTERS for character in tag)
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
    return sorted(spans, key=lambda span: span["start"])


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
            if resp.status_code == 400 or 300 <= resp.status_code < 400:
                return None
            resp.raise_for_status()
            data = _json_object(resp, "resolveHandle")
    except (requests.RequestException, ValueError):
        # An unresolvable handle stays plain text, timeouts included.
        return None
    did = data.get("did") if isinstance(data, dict) else None
    return did if isinstance(did, str) and DID_REGEX.fullmatch(did) else None


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
        label="Mention resolution",
    ), ThreadPoolExecutor(
        max_workers=min(MENTION_RESOLUTION_WORKERS, len(handles)),
        thread_name_prefix="bsky-resolve-handle",
    ) as executor:
        futures = {
            handle: executor.submit(
                copy_context().run, _resolve_handle, pds_url, handle
            )
            for handle in handles
        }
    return {handle: future.result() for handle, future in futures.items()}


def parse_facets(pds_url: str, text: str) -> List[Dict]:
    facets: List[Dict] = []
    occupied: List[tuple[int, int]] = []
    byte_offsets = _byte_offsets(text)
    for u in parse_urls(text, byte_offsets):
        if _reserve_span(u, occupied):
            facets.append(
                make_facet(
                    u,
                    {"$type": "app.bsky.richtext.facet#link", "uri": u["url"]},
                )
            )
    # Handles are case-insensitive, so each distinct one is looked up once.
    mentions = [
        m for m in parse_mentions(text, byte_offsets)
        if not _span_insert_index(m["start"], m["end"], occupied)[1]
    ]
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
        print(
            f"warning: could not resolve {shown}{more}; "
            "posting as plain text without a mention link.",
            file=sys.stderr,
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
        if _reserve_span(h, occupied)
    )
    return sorted(facets, key=lambda facet: facet["index"]["byteStart"])


def _path_parts(parsed: ParseResult) -> tuple[str, ...]:
    return tuple(part for part in parsed.path.split("/") if part)


def _validate_at_identifier(identifier: str) -> None:
    if DID_REGEX.fullmatch(identifier):
        return
    if (
        identifier == identifier.lower()
        and len(identifier) <= 253
        and HANDLE_REGEX.fullmatch(identifier)
    ):
        return
    raise ValueError("AT URI authority must be a normalized DID or handle")


def _validate_nsid(nsid: str) -> None:
    authority, separator, _name = nsid.rpartition(".")
    if (
        not separator
        or len(nsid) > 317
        or len(authority) > 253
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
    try:
        uri.encode("ASCII")
    except UnicodeEncodeError as exc:
        raise ValueError("AT URI must contain only ASCII characters") from exc
    if not uri.startswith("at://"):
        raise ValueError("AT URI must use the normalized lowercase at:// scheme")
    if len(uri.encode("ASCII")) > 8 * 1024:
        raise ValueError("AT URI exceeds the 8 KiB limit")
    if parsed.query or parsed.fragment or parsed.params or not parsed.netloc:
        raise ValueError(f"Invalid AT URI format: {uri}")
    path_parts = parsed.path.split("/")
    if len(path_parts) != 3 or path_parts[0] or not all(path_parts[1:]):
        raise ValueError(f"Invalid AT URI path: {uri}")
    repo, collection, rkey = parsed.netloc, path_parts[1], path_parts[2]
    _validate_at_identifier(repo)
    _validate_nsid(collection)
    _validate_record_key(rkey)
    return {"repo": repo, "collection": collection, "rkey": rkey}


def _parse_bsky_app_uri(parsed: ParseResult, uri: str) -> Dict:
    try:
        port = parsed.port
    except ValueError as exc:
        raise ValueError(f"Invalid Bluesky URL format: {uri}") from exc
    # An explicit ":443" is the https default and appears in URLs copied out of
    # some clients; any other port would change where the record is looked up.
    if (
        parsed.username
        or parsed.password
        or port not in (None, 443)
        or parsed.params
        or "//" in parsed.path
        or not parsed.path
        or parsed.path.endswith("/")
    ):
        raise ValueError(f"Invalid Bluesky URL format: {uri}")
    parts = _path_parts(parsed)
    if len(parts) != 4 or parts[0] != "profile":
        raise ValueError(f"Invalid Bluesky URL format: {uri}")
    _, repo, collection, rkey = parts
    # Handles are case-insensitive and links copied by hand may be mixed-case;
    # DIDs are case-sensitive and must be left exactly as written.
    if not repo.startswith("did:"):
        repo = repo.lower()
    mapped_collection = BSKY_APP_COLLECTIONS.get(collection)
    if mapped_collection is None:
        raise ValueError(f"Unsupported Bluesky record path: {collection!r}")
    _validate_at_identifier(repo)
    _validate_record_key(rkey)
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
    try:
        parsed = urlparse(uri)
    except ValueError as exc:
        raise ValueError(f"Invalid URI format: {uri}") from exc
    if parsed.scheme.lower() == "at":
        return _parse_at_uri(parsed, uri)
    if parsed.scheme.lower() == "https" and parsed.hostname == "bsky.app":
        return _parse_bsky_app_uri(parsed, uri)
    raise ValueError(f"Unhandled URI format: {uri}")


def _check_record_matches_request(
    requested: Dict,
    returned_uri: str,
    context: str = "getRecord",
) -> None:
    """Refuse an answer that names a different record than was asked for.

    A handle-addressed request is answered with the account's DID, which cannot
    be compared without a separate resolution, so the repo is only compared
    when the request itself named a DID.
    """
    returned = parse_uri(returned_uri)
    if (
        returned["collection"] != requested["collection"]
        or returned["rkey"] != requested["rkey"]
        or (
            requested["repo"].startswith("did:")
            and returned["repo"] != requested["repo"]
        )
    ):
        raise ValueError(f"{context} returned a different record than was requested")


def _validated_record_response(data: Dict, requested: Dict, context: str) -> Dict:
    """Return {uri, cid} from a response that must name the requested record."""
    if not isinstance(data.get("uri"), str) or not isinstance(data.get("cid"), str):
        raise ValueError(f"{context} response is missing uri or cid")
    try:
        ref = record_ref(data)
    except ValueError as exc:
        raise ValueError(
            f"{context} response contains an invalid record reference"
        ) from exc
    _check_record_matches_request(requested, ref["uri"], context)
    return ref


def get_record(record_service_url: str, uri: str) -> Dict:
    requested = parse_uri(uri)
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


def _is_valid_record_cid(cid: str) -> bool:
    if re.fullmatch(r"b[a-z2-7]{58}", cid) is None:
        return False
    encoded = cid[1:].upper()
    encoded += "=" * ((8 - len(encoded) % 8) % 8)
    try:
        raw_cid = base64.b32decode(encoded)
    except binascii.Error:
        return False
    return (
        len(raw_cid) == 36
        and raw_cid[:4] == b"\x01\x71\x12\x20"
        and "b" + base64.b32encode(raw_cid).decode("ASCII").lower().rstrip("=")
        == cid
    )


def record_ref(record: Dict) -> Dict:
    uri = record.get("uri")
    cid = record.get("cid")
    if not isinstance(uri, str) or not isinstance(cid, str):
        raise ValueError("Record is missing uri or cid")
    if not uri.startswith("at://"):
        raise ValueError("Record URI is not a normalized AT URI")
    parse_uri(uri)
    if not _is_valid_record_cid(cid):
        raise ValueError("Record CID is not a blessed DAG-CBOR SHA-256 CID")
    return {"uri": uri, "cid": cid}


def _require_post_uri(uri: str, context: str) -> None:
    parts = parse_uri(uri)
    if parts["collection"] != "app.bsky.feed.post":
        raise ValueError(f"{context} must reference an app.bsky.feed.post record")


def _require_post_record(record: Dict, context: str) -> Dict:
    uri = record.get("uri")
    value = record.get("value")
    if not isinstance(uri, str):
        raise ValueError(f"{context} is missing uri")
    _require_post_uri(uri, context)
    if not isinstance(value, dict) or value.get("$type") != "app.bsky.feed.post":
        raise ValueError(f"{context} is not an app.bsky.feed.post record")
    return value


def _reply_root_ref(record_service_url: str, parent_reply: Dict) -> Dict:
    root_ref = parent_reply.get("root")
    if not isinstance(root_ref, dict) or not isinstance(root_ref.get("uri"), str):
        raise ValueError("Parent reply reference is missing root.uri")
    _require_post_uri(root_ref["uri"], "Reply root")
    if isinstance(root_ref.get("cid"), str):
        return record_ref(root_ref)
    root_record = get_record(record_service_url, root_ref["uri"])
    _require_post_record(root_record, "Reply root")
    return record_ref(root_record)


def get_reply_refs(record_service_url: str, parent_uri: str) -> Dict:
    _require_post_uri(parent_uri, "Reply parent")
    parent = get_record(record_service_url, parent_uri)
    value = _require_post_record(parent, "Reply parent")
    parent_reply = value.get("reply")
    if parent_reply is None:
        root = record_ref(parent)
    elif isinstance(parent_reply, dict):
        root = _reply_root_ref(record_service_url, parent_reply)
    else:
        raise ValueError("Parent reply reference has an unexpected shape")
    return {"root": root, "parent": record_ref(parent)}


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
    while position < container_end:
        if container_end - position < 8:
            raise ValueError("Truncated WebP chunk header")
        chunk_type = img_bytes[position:position + 4]
        chunk_size = int.from_bytes(img_bytes[position + 4:position + 8], "little")
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


def _preflight_image(img_bytes: bytes) -> str:
    if not img_bytes or len(img_bytes) > MAX_IMAGE_SIZE_BYTES:
        raise ValueError(f"Image must contain 1 to {MAX_IMAGE_SIZE_BYTES:,} bytes")
    if img_bytes.startswith(b"\xff\xd8\xff"):
        return "JPEG"
    if img_bytes.startswith(b"\x89PNG\r\n\x1a\n"):
        return "PNG"
    if img_bytes.startswith((b"GIF87a", b"GIF89a")):
        return "GIF"
    if img_bytes[:4] == b"RIFF" and img_bytes[8:12] == b"WEBP":
        _preflight_webp_dimensions(img_bytes)
        return "WEBP"
    raise ValueError("Unsupported or invalid image format; expected JPEG, PNG, GIF, or WebP")


def _inspect_image_decoded(img_bytes: bytes) -> Dict[str, Any]:
    """Run only in the bounded worker; never rewrite the supplied image."""
    image_format = _preflight_image(img_bytes)
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        # verify checks container integrity (including PNG chunk CRCs), but is
        # a no-op for some formats such as JPEG, so a full decode follows it.
        with Image.open(io.BytesIO(img_bytes), formats=[image_format]) as img:
            _validate_image_dimensions(*img.size)
            img.verify()
        # Some plugins retain native canvases until the image object is freed.
        del img
        with Image.open(io.BytesIO(img_bytes), formats=[image_format]) as img:
            width, height = img.size
            _validate_image_dimensions(width, height)
            frame_count = getattr(img, "n_frames", 1)
            if frame_count > MAX_IMAGE_FRAMES:
                raise ValueError(f"Image exceeds the {MAX_IMAGE_FRAMES}-frame limit")
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
_MAX_IMAGE_BATCH_BYTES = MAX_IMAGES_PER_POST * (4 + MAX_IMAGE_SIZE_BYTES)


def _encode_image_batch(images: List[bytes]) -> bytes:
    return b"".join(len(image).to_bytes(4, "big") + image for image in images)


def _decode_image_batch(payload: bytes) -> List[bytes]:
    images: List[bytes] = []
    position = 0
    while position < len(payload):
        if len(images) >= MAX_IMAGES_PER_POST or len(payload) - position < 4:
            raise ValueError("Malformed image batch")
        size = int.from_bytes(payload[position:position + 4], "big")
        position += 4
        if size > MAX_IMAGE_SIZE_BYTES or len(payload) - position < size:
            raise ValueError("Malformed image batch")
        images.append(payload[position:position + size])
        position += size
    if not images:
        raise ValueError("Empty image batch")
    return images


def _worker_error_text(exc: Exception) -> str:
    # Keep even malformed metadata errors within the bounded reply file.
    return f"{type(exc).__name__}: {str(exc)[:300]}"


def _image_worker_main() -> None:
    try:
        payload = sys.stdin.buffer.read(_MAX_IMAGE_BATCH_BYTES + 1)
        if len(payload) > _MAX_IMAGE_BATCH_BYTES:
            raise ValueError("Image batch is too large")
        results: List[Dict[str, Any]] = []
        # Validation stops at the first bad image: the whole post fails anyway.
        for img_bytes in _decode_image_batch(payload):
            try:
                results.append({"image": _inspect_image_decoded(img_bytes)})
            except Exception as exc:
                results.append({"error": _worker_error_text(exc)})
                break
        reply: Dict[str, Any] = {"results": results}
    except Exception as exc:
        reply = {"error": _worker_error_text(exc)}
    sys.stdout.buffer.write(json.dumps(reply, ensure_ascii=True).encode("ascii"))


_IMAGE_WORKER_LAUNCHER = """\
import sys
try:
    import resource
except ImportError:
    resource = None
if resource is not None:
    for name, requested in (("RLIMIT_AS", int(sys.argv[2])),
                            ("RLIMIT_CPU", int(sys.argv[3])),
                            ("RLIMIT_FSIZE", int(sys.argv[4]))):
        if hasattr(resource, name):
            kind = getattr(resource, name)
            _, hard = resource.getrlimit(kind)
            limit = requested if hard == resource.RLIM_INFINITY else min(requested, hard)
            resource.setrlimit(kind, (limit, limit))
import json, runpy
sys.path[:] = json.loads(sys.argv[5])
runpy.run_path(sys.argv[1], run_name="bsky_image_worker")["_image_worker_main"]()
"""


def _validated_worker_image(info: Any) -> Dict[str, Any]:
    if not isinstance(info, dict) or type(info.get("width")) is not int or type(info.get("height")) is not int:
        raise ValueError("Image decoder returned invalid dimensions")
    _validate_image_dimensions(info["width"], info["height"])
    if info.get("mimetype") not in IMAGE_FORMAT_MIMETYPES.values():
        raise ValueError("Image decoder returned an invalid MIME type")
    return info


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
        str(Path(entry).resolve()) for entry in sys.path
        if entry and Path(entry).resolve() != working_directory
    ]
    command = [
        sys.executable, "-I", "-S", "-B", "-c", _IMAGE_WORKER_LAUNCHER,
        str(Path(__file__).resolve()), str(MAX_IMAGE_WORKER_MEMORY_BYTES),
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
    environment["PYTHONIOENCODING"] = "utf-8"
    # A regular anonymous file permits RLIMIT_FSIZE to bound the child's reply;
    # stderr is discarded rather than accumulating native decoder diagnostics.
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
    try:
        result = json.loads(encoded)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("Image decoder returned an invalid result") from exc
    if not isinstance(result, dict):
        raise ValueError("Image decoder returned an invalid result")
    if isinstance(result.get("error"), str):
        raise ValueError(result["error"])
    entries = result.get("results")
    if not isinstance(entries, list) or not 1 <= len(entries) <= len(images):
        raise ValueError("Image decoder returned an invalid result")
    validated: List[Dict[str, Any]] = []
    for entry in entries:
        if isinstance(entry, dict) and isinstance(entry.get("error"), str):
            validated.append({"error": entry["error"]})
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
        except (OSError, ValueError) as exc:
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
) -> Dict:
    # The mimetype is always the one Pillow decoded from the bytes themselves.
    # Guessing it from a filename or URL extension would let a remote page pick
    # the Content-Type of a blob we vouch for, so no such fallback exists.
    with _open_api_response(
        "POST",
        _api_url(pds_url, "com.atproto.repo.uploadBlob"),
        timeout=_upload_timeout(len(img_bytes)),
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
    return blob


def _read_image_file(path: Path) -> bytes:
    flags = os.O_RDONLY
    for flag_name in ("O_CLOEXEC", "O_NONBLOCK", "O_NOFOLLOW"):
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
        file_descriptor = os.open(path, flags)
        file_info = os.fstat(file_descriptor)
        if pre_open_info is not None and (
            file_info.st_dev != pre_open_info.st_dev
            or file_info.st_ino != pre_open_info.st_ino
        ):
            raise ValueError(f"Image path changed while opening: {path}")
        if not stat.S_ISREG(file_info.st_mode):
            raise ValueError(f"Image path is not a regular file: {path}")
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


def _image_embed_item(blob: Dict, image_info: Dict[str, Any], alt: str) -> Dict:
    return {
        "alt": alt,
        "image": blob,
        "aspectRatio": {
            "width": image_info["width"],
            "height": image_info["height"],
        },
    }


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
    # Blobs left unreferenced by a later failure are garbage collected by the PDS.
    images = [
        _image_embed_item(
            upload_file(pds_url, access_token, img_bytes, image_info["mimetype"]),
            image_info,
            alt,
        )
        for img_bytes, image_info, alt in prepared
    ]
    return {"$type": "app.bsky.embed.images", "images": images}


_ZERO_WIDTH_JOINER = "\u200d"
_VARIATION_SELECTORS = frozenset(chr(cp) for cp in range(0xFE00, 0xFE10))


def _extends_previous_cluster(character: str) -> bool:
    """Best-effort test for whether `character` attaches to the preceding base.

    The stdlib has no UAX #29 grapheme segmentation, so this only recognizes the
    backward-attaching extenders that would make an obviously broken boundary if
    left behind: combining marks (category M*) and emoji variation selectors. The
    zero-width joiner joins forward, so a dropped leading ZWJ does not corrupt the
    kept prefix; a dangling trailing ZWJ is handled separately by the caller.
    """
    return character in _VARIATION_SELECTORS or unicodedata.category(
        character
    ).startswith("M")


def _grapheme_safe_prefix(text: str, limit: int) -> str:
    """Return at most `limit` code points with best-effort boundary adjustments.

    This stdlib-only approximation handles combining marks and dangling ZWJs.
    It can still split emoji modifier sequences, joined emoji, and regional-
    indicator flags; it is not full Unicode grapheme segmentation.
    """
    if len(text) <= limit:
        return text
    cut = limit
    while cut > 0 and (
        _extends_previous_cluster(text[cut])
        or text[cut - 1] == _ZERO_WIDTH_JOINER
    ):
        cut -= 1
    return text[:cut]


def _trim_card_text(value: Any, limit: int) -> str:
    if not isinstance(value, str):
        return ""
    return _grapheme_safe_prefix(value.strip(), limit)


def _meta_content(soup: BeautifulSoup, property_name: str) -> str:
    # Match the attribute value case-insensitively: HTML attribute values are
    # case-sensitive to the parser, but real pages do emit `property="OG:Title"`.
    wanted = property_name.lower()

    def matches(value: Any) -> bool:
        return isinstance(value, str) and value.strip().lower() == wanted

    tag = soup.find("meta", property=matches) or soup.find(
        "meta",
        attrs={"name": matches},
    )
    content = tag.get("content") if tag else None
    # Stripped here so a whitespace-only og:title still falls back to <title>
    # and an og:image padded with newlines is still a fetchable URL.
    return content.strip() if isinstance(content, str) else ""


def _page_title(soup: BeautifulSoup) -> str:
    title = soup.find("title")
    if title is None:
        return ""
    # Tag.string recurses through single-child tags. Walk text iteratively so
    # deeply nested markup cannot exhaust the Python stack, and stop copying
    # after enough characters to apply the normal card-title truncation.
    parts: List[str] = []
    remaining = MAX_EXTERNAL_TITLE_CHARS + 1
    for text in title.strings:
        if not parts:
            text = text.lstrip()
        if not text:
            continue
        part = text[:remaining]
        parts.append(part)
        remaining -= len(part)
        if remaining == 0:
            break
    return "".join(parts)


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


def _attach_external_thumb(
    card: Dict[str, Any],
    pds_url: str,
    access_token: str,
    page_url: str,
    img_url: str,
) -> None:
    if not img_url:
        return

    def warn_and_skip_thumb(reason: Exception) -> None:
        print(
            f"warning: could not embed og:image {_url_for_log(img_url)!r} "
            f"({type(reason).__name__}: {_terminal_safe(reason)}); "
            "posting the card without it.",
            file=sys.stderr,
        )

    try:
        img_url = urljoin(page_url, img_url)
        img_bytes, _, _ = _safe_download(img_url, MAX_EMBED_IMAGE_BYTES)
        image_info = inspect_image(img_bytes, img_url)
    except (requests.RequestException, ValueError) as exc:
        # A failed thumbnail never aborts the post.
        warn_and_skip_thumb(exc)
        return

    try:
        card["thumb"] = upload_file(
            pds_url,
            access_token,
            img_bytes,
            image_info["mimetype"],
        )
    except (requests.RequestException, ValueError) as exc:
        # Upload failures (a rejected blob, an HTTP error, a timeout) are all
        # recoverable here: the card is still posted, just without a thumbnail.
        warn_and_skip_thumb(exc)


def _html_head_bytes(html_bytes: bytes) -> bytes:
    """Return the part of a page that can hold link-card metadata.

    That is everything before the first </head>, or, when the page omits it
    (which HTML permits), before the first <body>. Pages where neither is
    found in the leading MAX_EMBED_HEAD_BYTES (for example UTF-16 pages) fall
    back to that prefix.
    """
    window = html_bytes[:MAX_EMBED_HEAD_BYTES]
    match = HTML_HEAD_END_REGEX.search(window) or HTML_BODY_START_REGEX.search(window)
    return window[:match.start()] if match else window


def _parse_embed_metadata(
    url: str,
    html_bytes: bytes,
    content_type: Optional[str],
) -> tuple[Dict[str, Any], str]:
    """Parse HTML and extract all link-card metadata under one wall-clock budget.

    `_safe_download` releases its deadline when it returns, so without this the
    parse is the one unbounded step in building a card. Only the page's head is
    parsed, which keeps ordinary pages far inside the budget. On timeout the
    worker is abandoned (Python cannot cancel a running parse) and the caller
    degrades to a bare card; the abandoned thread finishes on its own, bounded
    by MAX_EMBED_HEAD_BYTES, and is a daemon so it never delays exit.
    """
    def parse() -> tuple[Dict[str, Any], str]:
        # Prefer the server-declared charset; BeautifulSoup falls back to a
        # BOM, <meta charset>, and sniffing.
        from bs4 import BeautifulSoup

        soup = BeautifulSoup(
            _html_head_bytes(html_bytes),
            "html.parser",
            from_encoding=_charset_from_content_type(content_type),
        )
        return _external_card_metadata(url, soup), _meta_content(soup, "og:image")

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
    # From here on, any failure degrades to a bare card carrying just the URL.
    card: Dict[str, Any] = {"uri": url, "title": "", "description": ""}
    try:
        html_bytes, final_url, content_type = _safe_download(
            url,
            MAX_EMBED_HTML_BYTES,
            allowed_content_types=EMBED_HTML_CONTENT_TYPES,
        )
        card, image_url = _parse_embed_metadata(url, html_bytes, content_type)
    except (requests.RequestException, ValueError, RecursionError) as exc:
        print(
            f"warning: could not read {_url_for_log(url)!r} for the link card "
            f"({type(exc).__name__}: {_terminal_safe(exc)}); "
            "posting a card with the URL only.",
            file=sys.stderr,
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
    if args.text and (facets := parse_facets(args.pds_url, args.text)):
        post["facets"] = facets
    if args.reply_to:
        post["reply"] = get_reply_refs(
            args.record_service_url,
            args.reply_to,
        )
    return post


def _build_embed(args: argparse.Namespace, access_token: str) -> Optional[Dict]:
    record_embed = (
        get_embed_ref(args.record_service_url, args.embed_ref)
        if args.embed_ref
        else None
    )
    media_embed: Optional[Dict] = None
    if args.image:
        media_embed = upload_images(args.pds_url, access_token, args.prepared_images)
    elif args.embed_url:
        media_embed = fetch_embed_url_card(
            args.pds_url,
            access_token,
            args.embed_url,
        )

    if record_embed and media_embed:
        return {
            "$type": "app.bsky.embed.recordWithMedia",
            "record": record_embed,
            "media": media_embed,
        }
    return record_embed or media_embed


def _new_tid() -> str:
    """Return a fresh TID record key: microsecond timestamp + random clock id."""
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
            _validated_record_response(data, requested, "createRecord")
        except ValueError as exc:
            raise UnusableCreateResponse(str(exc)) from exc
    return data


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
        print(
            "warning: the post may still have been created as "
            f"at://{did}/app.bsky.feed.post/{rkey}; check before posting again.",
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
    session = bsky_login_session(args.pds_url, args.handle, args.password)
    try:
        access_token = session["accessJwt"]
        post = _build_post_record(args)
        if embed := _build_embed(args, access_token):
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


def exit_error(*lines: str) -> None:
    # Lines routinely interpolate exception text that originated at a remote
    # server (an XRPC message, an HTTP reason phrase), so sanitize here rather
    # than trusting every call site to remember.
    raise SystemExit("\n".join(_terminal_safe(line) for line in lines))


def exit_for_error(exc: Exception) -> None:
    """Exit with this script's own error message, keeping its line breaks.

    For a ValueError or an XrpcError. Some of these messages span lines (a
    login failure and its remedy). Remote fragments inside them are escaped where they are
    interpolated, and _terminal_safe still neutralizes every other control
    character in each line, so only a plain line break is honoured here.
    """
    exit_error(*f"Error: {exc}".split("\n"))


def run_self_tests() -> None:
    """Run test_create_bsky_post.py, which ships alongside this script."""
    import importlib.util

    path = Path(__file__).resolve().with_name("test_create_bsky_post.py")
    if not path.is_file():
        raise SystemExit(f"Self-tests not found: {path}")
    # Loaded by path, so it works under `python -I` too, and sharing this
    # module keeps the tests from importing and patching a second copy of it.
    sys.modules.setdefault("create_bsky_post", sys.modules[__name__])
    spec = importlib.util.spec_from_file_location("test_create_bsky_post", path)
    tests = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tests)
    tests.run_self_tests()


def _validate_image_args(args: argparse.Namespace) -> None:
    if args.image and len(args.image) > MAX_IMAGES_PER_POST:
        raise ValueError(f"At most {MAX_IMAGES_PER_POST} images per post.")
    if args.alt_text and not args.image:
        raise ValueError("--alt-text requires --image.")
    if args.alt_text and args.image and len(args.alt_text) != len(args.image):
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
        _parse_url(args.embed_url, schemes=("http", "https"))
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
    ):
        return False
    normalized = language_tag.lower()
    if normalized in GRANDFATHERED_LANGUAGE_TAGS:
        return True
    subtags = normalized.split("-")
    if any(
        not subtag
        or len(subtag) > 8
        or not subtag.isascii()
        or not subtag.isalnum()
        for subtag in subtags
    ):
        return False

    if subtags[0] == "x":
        return len(subtags) > 1

    language = subtags[0]
    if not language.isalpha() or not 2 <= len(language) <= 8:
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
        is_variant = (
            5 <= len(subtag) <= 8
            or (len(subtag) == 4 and subtag[0].isdigit())
        )
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
        while index < len(subtags) and 2 <= len(subtags[index]) <= 8:
            index += 1
        if index == extension_start:
            return False

    if index < len(subtags) and subtags[index] == "x":
        index += 1
        private_use_start = index
        while index < len(subtags) and 1 <= len(subtags[index]) <= 8:
            index += 1
        if index == private_use_start:
            return False

    return index == len(subtags)


def _validate_language_args(language_tags: Optional[List[str]]) -> None:
    if not language_tags:
        return
    if len(language_tags) > MAX_LANGS:
        raise ValueError(f"At most {MAX_LANGS} language tags may be supplied.")
    invalid = [tag for tag in language_tags if not _is_valid_language_tag(tag)]
    if invalid:
        raise ValueError(f"Invalid BCP 47 language tag: {invalid[0]!r}")


def validate_args(args: argparse.Namespace) -> None:
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
    parser.add_argument("--handle", default=os.environ.get("ATP_AUTH_HANDLE") or None,
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
    if not (args.text.strip() or args.image or args.embed_url or args.embed_ref):
        exit_error(*_missing_content_usage(parser.prog))

    try:
        validate_args(args)
    except ValueError as e:
        exit_for_error(e)

    if args.password:
        print(
            "warning: passing --password on the command line exposes it to other local "
            "users via `ps`; prefer the ATP_AUTH_PASSWORD environment variable.",
            file=sys.stderr,
        )
    else:
        args.password = os.environ.get("ATP_AUTH_PASSWORD") or None

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
