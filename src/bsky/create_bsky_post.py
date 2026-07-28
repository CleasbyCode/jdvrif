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

 * Wall-clock deadlines on every network operation, response size caps,
   and refusal of compressed remote bodies

 * Strict validation of AT URIs, record CIDs, handles, language tags,
   image files (symlink-safe reads, dimension/pixel limits), and URLs

Setup:

 Requires: requests, beautifulsoup4, pillow
    $ pip install requests beautifulsoup4 pillow

 Set your credentials as environment variables:
    $ export ATP_AUTH_HANDLE='your-handle.bsky.social'
    $ export ATP_AUTH_PASSWORD='xxxx-xxxx-xxxx-xxxx'

 IMPORTANT: ATP_AUTH_PASSWORD should be an APP password, created at
 https://bsky.app/settings/app-passwords — do NOT use your main Bluesky
 account password. App passwords can be revoked individually and cannot
 change account settings.

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
import io
import ipaddress
import json
import math
import os
import queue
import re
import socket
import stat
import sys
import threading
import time
import unicodedata
import warnings
from bisect import bisect_left
from contextlib import contextmanager
from contextvars import ContextVar
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List, Optional
from urllib.parse import ParseResult, urljoin, urlparse, urlunparse

import requests
from requests.adapters import HTTPAdapter
from bs4 import BeautifulSoup
from PIL import Image, UnidentifiedImageError


DEFAULT_PDS_URL = "https://bsky.social"
DEFAULT_RECORD_SERVICE_URL = "https://public.api.bsky.app"
MAX_IMAGES_PER_POST = 4
MAX_IMAGE_SIZE_BYTES = 2_000_000
MAX_POST_BYTES = 3_000
MAX_TAG_GRAPHEMES = 64
MAX_TAG_BYTES = 640
MAX_LANGS = 3
MAX_EMBED_HTML_BYTES = 2_000_000
MAX_EMBED_IMAGE_BYTES = 1_000_000
MAX_API_RESPONSE_BYTES = 8_000_000
MAX_IMAGE_PIXELS = 40_000_000
MAX_IMAGE_DIMENSION = 16_384
MAX_EXTERNAL_TITLE_CHARS = 300
MAX_EXTERNAL_DESCRIPTION_CHARS = 1_000
MAX_REDIRECTS = 3
MAX_DOWNLOAD_ADDRESS_ATTEMPTS = 4
USER_AGENT = "bsky-post/1.1 (+https://bsky.app)"
DOWNLOAD_CHUNK_SIZE = 64 * 1024

HANDLE_REGEX = re.compile(
    r"^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
    r"(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)*"
    r"\.[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$",
    re.IGNORECASE,
)
MENTION_REGEX = re.compile(
    r"(?<![\w@])"
    r"(@(?:[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\.)+"
    r"[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?)"
    r"(?![a-z0-9-])",
    re.IGNORECASE,
)
URL_REGEX = re.compile(
    r"(?<![\w/])((?:https?://)[^\s<>'\"`]+)",
    re.IGNORECASE,
)
HASHTAG_REGEX = re.compile(r"(^|\s)([#＃])(\S+)")
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
GRANDFATHERED_LANGUAGE_TAGS = frozenset(
    {
        "art-lojban",
        "cel-gaulish",
        "en-gb-oed",
        "i-ami",
        "i-bnn",
        "i-default",
        "i-enochian",
        "i-hak",
        "i-klingon",
        "i-lux",
        "i-mingo",
        "i-navajo",
        "i-pwn",
        "i-tao",
        "i-tay",
        "i-tsu",
        "no-bok",
        "no-nyn",
        "sgn-be-fr",
        "sgn-be-nl",
        "sgn-ch-de",
        "zh-guoyu",
        "zh-hakka",
        "zh-min",
        "zh-min-nan",
        "zh-xiang",
    }
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
_SESSION.headers["Accept-Encoding"] = "identity"
_SESSION.trust_env = False

_DOWNLOAD_DEADLINE: ContextVar[Optional[float]] = ContextVar(
    "_DOWNLOAD_DEADLINE",
    default=None,
)
_NETWORK_TIMEOUT_SUBJECT: ContextVar[str] = ContextVar(
    "_NETWORK_TIMEOUT_SUBJECT",
    default="External download",
)
_IPV4_TRANSLATION_NETWORKS = (
    ipaddress.IPv6Network("64:ff9b::/96"),
    ipaddress.IPv6Network("::ffff:0:0:0/96"),
)
_DEPRECATED_6TO4_RELAY_NETWORK = ipaddress.IPv4Network("192.88.99.0/24")
_LOCAL_IPV6_TRANSLATION_NETWORK = ipaddress.IPv6Network("64:ff9b:1::/48")


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
    authority = f"[{hostname}]" if ":" in hostname else hostname
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
    # Service URLs (PDS, record service) are a trusted boundary: they are
    # operator-supplied endpoints, never attacker-influenced, so they are only
    # scheme/host normalized and are deliberately NOT run through the public-IP
    # SSRF check that guards attacker-influenced fetches (embed URLs, redirects,
    # og:image). The PDS additionally receives the user's credentials; the
    # record service carries none (getRecord is unauthenticated) but is still
    # operator-chosen, so aiming either at a private address is a deliberate
    # operator decision (e.g. local testing), not an SSRF vector reachable by
    # post content. Enforce HTTPS unless explicitly opted out for loopback.
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


def normalize_pds_url(pds_url: str, *, allow_insecure: bool = False) -> str:
    return normalize_service_url(
        pds_url,
        service_name="PDS",
        allow_insecure=allow_insecure,
    )


def _remaining_download_time(deadline: float, operation: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise requests.Timeout(
            f"{_NETWORK_TIMEOUT_SUBJECT.get()} timed out while {operation}"
        )
    return remaining


def _run_before_download_deadline(
    action: Callable[[], Any],
    deadline: float,
    operation: str,
    *,
    dispose_abandoned: Optional[Callable[[Any], None]] = None,
) -> Any:
    """Run one blocking operation without allowing it past the total deadline.

    On timeout the blocking call is abandoned in its daemon worker thread while
    the caller raises requests.Timeout. That worker may still be touching the
    shared _SESSION, which is not thread-safe, so any timeout MUST unwind all
    the way to main() and terminate the process rather than be caught and
    followed by another request that reuses _SESSION. _resolve_handle relies on
    this by re-raising Timeout instead of swallowing it.
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
        if (
            late_outcome is not None
            and late_outcome[0]
            and dispose_abandoned is not None
        ):
            try:
                dispose_abandoned(late_outcome[1])
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
                should_dispose = succeeded and dispose_abandoned is not None
            else:
                outcome.put_nowait((succeeded, value))
        if should_dispose and dispose_abandoned is not None:
            try:
                dispose_abandoned(value)
            except Exception:
                pass

    thread = threading.Thread(
        target=worker,
        name="bsky-download-operation",
        daemon=True,
    )
    # Do not launch work when an inherited deadline has already expired.
    _remaining_download_time(deadline, operation)
    thread.start()
    try:
        succeeded, value = outcome.get(
            timeout=_remaining_download_time(deadline, operation)
        )
    except requests.Timeout:
        abandon_pending_outcome()
        raise
    except queue.Empty as exc:
        abandon_pending_outcome()
        raise requests.Timeout(
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
    if (
        isinstance(address, ipaddress.IPv6Address)
        and address in _LOCAL_IPV6_TRANSLATION_NETWORK
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

    def resolve() -> Any:
        return socket.getaddrinfo(
            parsed.hostname,
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
                f"resolving {parsed.hostname!r}",
            )
        )
    except socket.gaierror as exc:
        raise ValueError(f"Could not resolve {parsed.hostname!r}: {exc}") from exc

    addresses: List[str] = []
    for info in infos:
        address = info[4][0]
        ip = ipaddress.ip_address(address)
        ip = getattr(ip, "ipv4_mapped", None) or ip
        if not _is_public_unicast_address(ip):
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
    timeout: float,
) -> Iterator[requests.Response]:
    """Open one response using one of a bounded set of validated addresses."""
    parsed, addresses = _public_url_addresses(url)
    deadline = _DOWNLOAD_DEADLINE.get()
    last_error: Optional[requests.RequestException] = None
    for address in addresses[:MAX_DOWNLOAD_ADDRESS_ATTEMPTS]:
        session = requests.Session()
        session.headers.update(_SESSION.headers)
        session.headers["Accept-Encoding"] = "identity"
        session.trust_env = False
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
                    dispose_abandoned=lambda late_response: late_response.close(),
                )
            )
        except requests.RequestException as exc:
            last_error = exc
            session.close()
            if deadline is not None and time.monotonic() >= deadline:
                raise requests.Timeout(
                    "External download timed out while connecting to "
                    f"{_url_for_log(url)!r}"
                ) from exc
            continue

        try:
            yield response
        finally:
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


def _read_response_body(resp: requests.Response, max_bytes: int) -> bytes:
    content_encoding = resp.headers.get("Content-Encoding", "").strip().lower()
    if content_encoding not in ("", "identity"):
        raise ValueError(
            "Refusing compressed remote response with Content-Encoding "
            f"{content_encoding!r}"
        )

    declared = _declared_content_length(resp)
    if declared is not None and declared < 0:
        raise ValueError("Remote response declares a negative Content-Length")
    if declared is not None and declared > max_bytes:
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
    return _run_before_download_deadline(
        read_body,
        deadline,
        "reading the response body",
    )


def _redirect_target(resp: requests.Response, current_url: str) -> str:
    location = resp.headers.get("Location")
    if not location:
        raise ValueError(
            f"Redirect from {_url_for_log(current_url)!r} missing Location header"
        )
    return urljoin(current_url, location)


def _safe_download(
    url: str,
    max_bytes: int,
    timeout: float = 10,
) -> tuple[bytes, str]:
    """Fetch a URL through pinned public addresses and return its final URL."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("Download timeout must be a positive finite number")

    requested_deadline = time.monotonic() + timeout
    outer_deadline = _DOWNLOAD_DEADLINE.get()
    deadline = (
        requested_deadline
        if outer_deadline is None
        else min(requested_deadline, outer_deadline)
    )
    deadline_token = _DOWNLOAD_DEADLINE.set(deadline)
    subject_token = _NETWORK_TIMEOUT_SUBJECT.set("External download")
    try:
        visited: set[str] = set()
        current = url
        for redirects_followed in range(MAX_REDIRECTS + 1):
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
                status_code = getattr(resp, "status_code", None)
                if status_code is None:
                    status_code = 302 if getattr(resp, "is_redirect", False) else 200
                if 300 <= status_code < 400:
                    if redirects_followed == MAX_REDIRECTS:
                        raise ValueError(
                            f"Too many redirects (> {MAX_REDIRECTS}) "
                            f"following {_url_for_log(url)!r}"
                        )
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
                return _read_response_body(resp, max_bytes), current
        raise ValueError(
            f"Too many redirects (> {MAX_REDIRECTS}) "
            f"following {_url_for_log(url)!r}"
        )
    finally:
        _NETWORK_TIMEOUT_SUBJECT.reset(subject_token)
        _DOWNLOAD_DEADLINE.reset(deadline_token)


@contextmanager
def _open_api_response(
    request_method: Callable[..., requests.Response],
    url: str,
    *,
    timeout: float,
    operation: str,
    **request_kwargs: Any,
) -> Iterator[requests.Response]:
    """Open and close one streamed API response under a wall-clock deadline."""
    if not math.isfinite(timeout) or timeout <= 0:
        raise ValueError("API timeout must be a positive finite number")
    requested_deadline = time.monotonic() + timeout
    outer_deadline = _DOWNLOAD_DEADLINE.get()
    deadline = (
        requested_deadline
        if outer_deadline is None
        else min(requested_deadline, outer_deadline)
    )
    deadline_token = _DOWNLOAD_DEADLINE.set(deadline)
    subject_token = _NETWORK_TIMEOUT_SUBJECT.set("API request")
    response: Optional[requests.Response] = None

    def request() -> requests.Response:
        return request_method(
            url,
            timeout=timeout,
            allow_redirects=False,
            stream=True,
            **request_kwargs,
        )

    try:
        response = _run_before_download_deadline(
            request,
            deadline,
            operation,
            dispose_abandoned=lambda late_response: late_response.close(),
        )
        yield response
    finally:
        try:
            if response is not None:
                response.close()
        finally:
            _NETWORK_TIMEOUT_SUBJECT.reset(subject_token)
            _DOWNLOAD_DEADLINE.reset(deadline_token)


def _json_object(resp: requests.Response, context: str) -> Dict[str, Any]:
    try:
        data = json.loads(_read_response_body(resp, MAX_API_RESPONSE_BYTES))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{context} returned a non-JSON response") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{context} returned an unexpected JSON shape")
    return data


def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    with _open_api_response(
        _SESSION.post,
        _api_url(pds_url, "com.atproto.server.createSession"),
        timeout=30,
        operation="waiting for createSession response headers",
        json={"identifier": handle, "password": password},
    ) as resp:
        if 300 <= resp.status_code < 400:
            raise ValueError(
                "Refusing redirect from credential-bearing createSession request"
            )
        resp.raise_for_status()
        data = _json_object(resp, "createSession")
    if not isinstance(data.get("accessJwt"), str) or not isinstance(data.get("did"), str):
        raise ValueError("createSession response is missing accessJwt or did")
    return data


def parse_spans(
    text: str,
    regex: "re.Pattern[str]",
    key: str,
    transform: Callable[[re.Match[str]], str] = lambda m: m.group(1),
) -> List[Dict]:
    byte_offsets = _byte_offsets(text)
    return [
        {
            "start": byte_offsets[match.start(1)],
            "end": byte_offsets[match.end(1)],
            key: transform(match),
        }
        for match in regex.finditer(text)
    ]


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


def parse_mentions(text: str) -> List[Dict]:
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


def parse_urls(text: str) -> List[Dict]:
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


TAG_FORBIDDEN_CHARACTERS = frozenset(
    "\ufe0f\u00ad\u2060\u200a\u200b\u200c\u200d\u20e2"
)


def _strip_tag_trailing_punctuation(tag: str) -> str:
    while tag and unicodedata.category(tag[-1]).startswith("P"):
        tag = tag[:-1]
    return tag


def _valid_tag_value(tag: str) -> bool:
    if (
        not tag
        or len(tag.encode("UTF-8")) > MAX_TAG_BYTES
        or any(character in TAG_FORBIDDEN_CHARACTERS for character in tag)
        or any(unicodedata.category(character).startswith("C") for character in tag)
    ):
        return False
    # Combining marks extend the preceding cluster. Other multi-codepoint
    # clusters (flags, Hangul, emoji modifiers) remain deliberately overcounted,
    # so this cannot admit a tag above the server's grapheme limit.
    grapheme_upper_bound = sum(
        not unicodedata.category(character).startswith("M")
        for character in tag
    ) + int(unicodedata.category(tag[0]).startswith("M"))
    if grapheme_upper_bound > MAX_TAG_GRAPHEMES:
        return False
    return any(
        not character.isdigit()
        and not unicodedata.category(character).startswith("P")
        for character in tag
    )


def parse_hashtags(text: str) -> List[Dict]:
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
            _SESSION.get,
            _api_url(pds_url, "com.atproto.identity.resolveHandle"),
            timeout=10,
            operation="waiting for resolveHandle response headers",
            params={"handle": handle},
        ) as resp:
            if resp.status_code == 400 or 300 <= resp.status_code < 400:
                return None
            resp.raise_for_status()
            data = _json_object(resp, "resolveHandle")
    except requests.Timeout:
        # A timed-out header request can still be unwinding in its daemon
        # worker. Abort the post instead of immediately reusing the Session.
        raise
    except (requests.RequestException, ValueError):
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
    resolved_handles: Dict[str, Optional[str]] = {}
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
        cache_key = m["handle"].lower()
        if cache_key not in resolved_handles:
            resolved_handles[cache_key] = _resolve_handle(pds_url, m["handle"])
        did = resolved_handles[cache_key]
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
    if (
        parsed.username
        or parsed.password
        or port is not None
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


def get_record(record_service_url: str, uri: str) -> Dict:
    with _open_api_response(
        _SESSION.get,
        _api_url(record_service_url, "com.atproto.repo.getRecord"),
        timeout=10,
        operation="waiting for getRecord response headers",
        params=parse_uri(uri),
    ) as resp:
        if 300 <= resp.status_code < 400:
            raise ValueError("Refusing redirect from getRecord request")
        resp.raise_for_status()
        data = _json_object(resp, "getRecord")
    if not isinstance(data.get("uri"), str) or not isinstance(data.get("cid"), str):
        raise ValueError("getRecord response is missing uri or cid")
    try:
        record_ref(data)
    except ValueError as exc:
        raise ValueError(
            "getRecord response contains an invalid record reference"
        ) from exc
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
    with _open_api_response(
        _SESSION.post,
        _api_url(pds_url, "com.atproto.repo.uploadBlob"),
        timeout=60,
        operation="waiting for uploadBlob response headers",
        headers={
            "Content-Type": content_type,
            "Authorization": "Bearer " + access_token,
        },
        data=img_bytes,
    ) as resp:
        if 300 <= resp.status_code < 400:
            raise ValueError("Refusing redirect from uploadBlob request")
        resp.raise_for_status()
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
        raise ValueError(f"Could not read image file {path!s}: {exc}") from exc
    finally:
        if file_descriptor >= 0:
            os.close(file_descriptor)
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
    """Return at most `limit` code points without splitting a grapheme cluster.

    This is a conservative, stdlib-only approximation (regional-indicator flag
    pairs, for example, are not tracked); the PDS enforces the authoritative
    grapheme count, so this only needs to avoid emitting an obviously broken
    cluster at the truncation boundary.
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
    try:
        img_url = _absolute_url(page_url, img_url)
        img_bytes, _ = _safe_download(img_url, MAX_EMBED_IMAGE_BYTES)
        image_info = inspect_image(img_bytes, img_url)
        card["thumb"] = upload_file(
            pds_url,
            access_token,
            img_url,
            img_bytes,
            image_info["mimetype"],
        )
    except (requests.RequestException, ValueError):
        # Swallowing requests.Timeout here is safe (and intentional), unlike the
        # API paths that must let it propagate: _safe_download uses its own
        # freshly-created sessions, so an abandoned daemon worker from a timed-out
        # download cannot be touching the shared _SESSION that createRecord reuses
        # next. A failed thumbnail must not abort an otherwise valid post.
        print(
            f"warning: could not embed og:image {_url_for_log(img_url)!r}.",
            file=sys.stderr,
        )


def fetch_embed_url_card(pds_url: str, access_token: str, url: str) -> Dict:
    html_bytes, final_url = _safe_download(url, MAX_EMBED_HTML_BYTES)
    soup = BeautifulSoup(html_bytes, "html.parser")
    card = _external_card_metadata(url, soup)
    _attach_external_thumb(card, pds_url, access_token, final_url, soup)
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
        media_embed = upload_images(
            args.pds_url,
            access_token,
            args.image,
            args.alt_text,
        )
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


def _response_body(resp: requests.Response) -> Any:
    body = _read_response_body(resp, MAX_API_RESPONSE_BYTES)
    try:
        return json.loads(body)
    except (UnicodeDecodeError, json.JSONDecodeError):
        return {"raw": body.decode("UTF-8", errors="replace")}


def create_post(args: argparse.Namespace) -> None:
    session = bsky_login_session(args.pds_url, args.handle, args.password)
    access_token = session["accessJwt"]
    post = _build_post_record(args)
    if embed := _build_embed(args, access_token):
        post["embed"] = embed

    print("Creating post.", file=sys.stderr)
    if getattr(args, "verbose", False):
        print(json.dumps(post, indent=2), file=sys.stderr)

    with _open_api_response(
        _SESSION.post,
        _api_url(args.pds_url, "com.atproto.repo.createRecord"),
        timeout=30,
        operation="waiting for createRecord response headers",
        headers={"Authorization": "Bearer " + access_token},
        json={
            "repo": session["did"],
            "collection": "app.bsky.feed.post",
            "record": post,
        },
    ) as resp:
        if 300 <= resp.status_code < 400:
            raise ValueError("Refusing redirect from createRecord request")
        resp_body = _response_body(resp)
        if not resp.ok:
            error_name = resp_body.get("error") if isinstance(resp_body, dict) else None
            summary = f" ({error_name})" if isinstance(error_name, str) else ""
            print(
                f"createRecord failed with HTTP {resp.status_code}{summary}.",
                file=sys.stderr,
            )
            if getattr(args, "verbose", False):
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
        {"start": 5, "end": 13, "tag": "abc-def"},
    ])
    _check_equal(parse_hashtags("##double ＃東京 $tsla"), [
        {"start": 0, "end": 8, "tag": "#double"},
        {"start": 9, "end": 18, "tag": "東京"},
        {"start": 19, "end": 24, "tag": "$TSLA"},
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
    test_cid = "bafyreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

    def fake_get_record(_pds_url: str, uri: str) -> Dict:
        calls.append(uri)
        if uri == "at://did:plc:parent/app.bsky.feed.post/parent":
            return {
                "uri": uri,
                "cid": test_cid,
                "value": {
                    "$type": "app.bsky.feed.post",
                    "reply": {
                        "root": {
                            "uri": "at://did:plc:root/app.bsky.feed.post/root",
                            "cid": test_cid,
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
            "cid": test_cid,
        },
        "parent": {
            "uri": "at://did:plc:parent/app.bsky.feed.post/parent",
            "cid": test_cid,
        },
    })


def test_trim_card_text():
    _check_equal(_trim_card_text("  hello  ", 300), "hello")
    _check_equal(_trim_card_text(None, 300), "")
    _check_equal(_trim_card_text("abcdef", 4), "abcd")
    # Do not slice between a base letter and its combining acute accent.
    _check_equal(_trim_card_text("abce\u0301", 4), "abc")
    # Do not leave a dangling zero-width joiner at the boundary.
    _check_equal(_trim_card_text("ab\u200dcd", 3), "ab")
    # A base + variation selector cluster is kept together when it fits...
    _check_equal(_trim_card_text("c\ufe0fdef", 3), "c\ufe0fd")
    # ...and the bare base is dropped rather than split when it does not.
    _check_equal(_trim_card_text("abc\ufe0f", 3), "ab")


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
        test_trim_card_text,
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
    if args.image and args.embed_url:
        raise ValueError(
            "Use only one media source: --image or --embed-url. "
            "Either may be combined with --embed-ref."
        )
    if args.embed_url:
        _parse_url(args.embed_url, schemes=("http", "https"))
    if args.embed_ref:
        parse_uri(args.embed_ref)
    return len(selected_sources)


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
    args.pds_url = normalize_pds_url(
        args.pds_url,
        allow_insecure=args.allow_insecure_pds,
    )
    args.record_service_url = normalize_service_url(
        getattr(args, "record_service_url", DEFAULT_RECORD_SERVICE_URL),
        service_name="record service",
        allow_insecure=args.allow_insecure_pds,
    )

    _validate_image_args(args)
    embed_sources = _validate_embed_args(args)
    _validate_language_args(args.lang)

    if not args.text and embed_sources == 0:
        raise ValueError("Post text or an embed is required.")

    if args.reply_to:
        _require_post_uri(args.reply_to, "Reply target")
    _validate_text_length(args.text)


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
    parser.add_argument("--pds-url", default=os.environ.get("ATP_PDS_HOST", DEFAULT_PDS_URL),
                        help=f"PDS URL (default: {DEFAULT_PDS_URL} or ATP_PDS_HOST env var)")
    parser.add_argument(
        "--record-service-url",
        default=os.environ.get("ATP_RECORD_SERVICE_HOST", DEFAULT_RECORD_SERVICE_URL),
        help=(
            "Network-wide record lookup service used for replies and quotes "
            f"(default: {DEFAULT_RECORD_SERVICE_URL} or ATP_RECORD_SERVICE_HOST)"
        ),
    )
    parser.add_argument("--allow-insecure-pds", action="store_true",
                        help="Allow HTTP service URLs on localhost/loopback only")
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
        help="Print the complete pending record and API error bodies to stderr",
    )
    parser.add_argument("--self-test", "--test", action="store_true", dest="self_test",
                        help="Run local parser/validation tests and exit")

    args = parser.parse_args()

    if args.self_test:
        run_self_tests()
        print("self-tests passed")
        return

    try:
        validate_args(args)
    except ValueError as e:
        exit_error(f"Error: {e}")

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
        create_post(args)
    except requests.RequestException as e:
        exit_error(f"Error: API request failed: {e}")
    except ValueError as e:
        exit_error(f"Error: {e}")


if __name__ == "__main__":
    main()
