#!/usr/bin/env python3

"""Self-tests for create_bsky_post.py.

Run either way (both use the same suite, network-free):

    $ python3 test_create_bsky_post.py
    $ python3 create_bsky_post.py --self-test

The tests call the script's functions by their bare names, so the script's
module namespace is mirrored into this one below. Patching always targets the
script's own module (see _patched_globals), because that is where its
functions look names up at call time.
"""

from __future__ import annotations

from bs4 import BeautifulSoup
from urllib3.response import HTTPResponse as _Urllib3Response

import create_bsky_post as bsky

globals().update(
    {name: value for name, value in vars(bsky).items() if not name.startswith("__")}
)


def _check(condition: bool, message: str = "self-test check failed") -> None:
    if not condition:
        raise AssertionError(message)


def _check_equal(actual: Any, expected: Any) -> None:
    if actual != expected:
        raise AssertionError(f"self-test mismatch: {actual!r} != {expected!r}")


@contextmanager
def _check_raises(
    exc_type: type[BaseException],
    message: str,
    *,
    contains: str = "",
) -> Iterator[None]:
    """Assert the wrapped block raises `exc_type`; `message` says what should fail.

    `contains` additionally pins a substring of the raised message, for the
    cases where the wrong-but-still-raised error would hide the bug.
    """
    try:
        yield
    except exc_type as exc:
        if contains and contains not in str(exc):
            raise AssertionError(f"{message}: {exc}") from exc
        return
    raise AssertionError(message)


@contextmanager
def _patched_globals(**replacements: Any) -> Iterator[None]:
    """Swap module-level names for the duration of a self-test, then restore them."""
    namespace = vars(bsky)
    originals = {name: namespace[name] for name in replacements}
    namespace.update(replacements)
    try:
        yield
    finally:
        namespace.update(originals)


@contextmanager
def _patched_attr(target: Any, name: str, replacement: Any) -> Iterator[Any]:
    """Same, for an attribute on an imported module or class."""
    original = getattr(target, name)
    setattr(target, name, replacement)
    try:
        yield replacement
    finally:
        setattr(target, name, original)


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
    # Handles are ASCII: Unicode case folding must not admit look-alikes such
    # as the Kelvin sign (which .lower() would turn into "k.com").
    for lookalike in ("\u212a", "\u0130", "\u0131", "\u017f"):
        _check_equal(parse_mentions(f"hi @{lookalike}.com"), [])
        _check(HANDLE_REGEX.fullmatch(f"{lookalike}.com") is None, lookalike)
    _check_equal(parse_mentions("hi @Alice.Example.COM"), [
        {"start": 3, "end": 21, "handle": "Alice.Example.COM"}
    ])
    # The boundary is still Unicode-aware: a letter before "@" blocks it.
    _check_equal(parse_mentions("é@example.com"), [])


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


def test_hashtag_grapheme_limit_keeps_invalid_tags_plaintext():
    # U+102B has category Mc but GCB=Other, so every occurrence starts a
    # separate grapheme. The complete post is comfortably below 300 graphemes.
    oversized_tag = "x" + "\u102b" * 65
    text = "hello #" + oversized_tag + " #ok"
    _check(len(text) < 300)
    _validate_text_length(text)
    args = argparse.Namespace(text=text, lang=None, reply_to=None,
                              pds_url="https://pds.example")
    record = _build_post_record(args)
    _check_equal(record["text"], text)
    _check_equal(record["facets"], [{
        "index": {"byteStart": len(text[:-3].encode("UTF-8")),
                  "byteEnd": len(text.encode("UTF-8"))},
        "features": [{"$type": "app.bsky.richtext.facet#tag", "tag": "ok"}],
    }])


def test_hashtag_grapheme_boundaries():
    for letter in ("a", "\u102b", "\u102c", "\u1038", "\u1a61", "\U0001f600"):
        _check(_valid_tag_value(letter * 64))
        _check(not _valid_tag_value(letter * 65))

    # Preserve ordinary decomposed accents and enclosing marks even when the
    # codepoint count exceeds 64. A leading cluster of marks also counts once.
    for combining_mark in ("\u0301", "\u20dd"):
        _check(_valid_tag_value(("e" + combining_mark) * 64))
        _check(not _valid_tag_value(("e" + combining_mark) * 65))
        _check(_valid_tag_value(combining_mark * 65))
        _check(_valid_tag_value(combining_mark + "a" * 63))
        _check(not _valid_tag_value(combining_mark + "a" * 64))
    _check_equal(parse_hashtags("#cafe\u0301 #\U0001f44d\U0001f3fd"), [
        {"start": 0, "end": 7, "tag": "cafe\u0301"},
        {"start": 8, "end": 17, "tag": "\U0001f44d\U0001f3fd"},
    ])
    for separator in ("\n", "\u2028", "\u2029"):
        _check(not _valid_tag_value("a" + separator + "\u0301"))


def test_parse_facets_skips_overlaps():
    calls = []

    def fake_resolve(_pds_url: str, handle: str) -> str:
        calls.append(handle)
        return "did:plc:test"

    with _patched_globals(_resolve_handle=fake_resolve):
        facets = parse_facets(
            "https://pds.example",
            "see https://example.com/@alice.test/#topic and @bob.test #ok",
        )

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
    with _check_raises(ValueError, "expected non-bsky URL to fail"):
        parse_uri("https://example.com/profile/example.com/post/abc")


def test_normalize_service_url():
    _check_equal(
        normalize_service_url("https://bsky.social/", service_name="PDS"),
        "https://bsky.social",
    )
    with _check_raises(ValueError, "expected insecure PDS URL to fail"):
        normalize_service_url("http://bsky.social", service_name="PDS")
    _check_equal(
        normalize_service_url(
            "http://localhost:2583", service_name="PDS", allow_insecure=True
        ),
        "http://localhost:2583",
    )


def test_url_security_checks():
    for url in ("http://127.0.0.1/", "http://[::1]/"):
        with _check_raises(ValueError, f"expected local URL to fail: {url}"):
            _public_url_addresses(url)
    with _check_raises(ValueError, "expected URL credentials to fail"):
        _parse_url("https://user:pass@example.com/", schemes=("https",))


def test_pinned_url_transport():
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

    with _patched_attr(socket, "getaddrinfo", public_getaddrinfo):
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

        with _patched_attr(socket, "getaddrinfo", mixed_getaddrinfo), _check_raises(
            ValueError,
            "expected mixed public/private DNS answers to fail",
        ):
            _public_url_addresses("https://example.com/")

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


def test_idn_host_resolves_by_its_a_label():
    """DNS must be asked for the exact name the certificate is checked against.

    Passing the raw Unicode name to getaddrinfo would encode it with CPython's
    IDNA2003 codec, which disagrees with the IDNA2008/UTS46 encoding used for
    SNI and assert_hostname on labels such as 'faß'.
    """
    asked = []

    def recording_getaddrinfo(host, port, **_kwargs):
        asked.append(host)
        return [
            (socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, "", ("8.8.8.8", port)),
        ]

    with _patched_attr(socket, "getaddrinfo", recording_getaddrinfo):
        parsed, addresses = _public_url_addresses("https://faß.example/x")

    _check_equal(addresses, ["8.8.8.8"])
    expected = _ascii_hostname("faß.example")
    _check_equal(asked, [expected])
    _check_equal(_original_authority(parsed), expected)

    adapter = _PinnedHTTPSAdapter(parsed.hostname or "")
    try:
        _check_equal(
            adapter.poolmanager.connection_pool_kw["assert_hostname"],
            expected,
        )
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

    sessions = []

    def fake_resolver(url: str):
        _check_equal(url, "https://example.com:8443/path")
        return urlparse(url), ["8.8.8.8"]

    def fake_session_factory():
        session = FakeSession()
        sessions.append(session)
        return session

    with _patched_globals(_public_url_addresses=fake_resolver), _patched_globals(
        _NoRedirectSession=fake_session_factory
    ):
        with _open_pinned_response(
            "https://example.com:8443/path",
            timeout=9,
        ) as response:
            _check(not response.closed)

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
            self.status_code = 302 if location is not None else 200
            self.headers = (
                {"Location": location}
                if location is not None
                else {"Content-Type": "text/html; charset=utf-8"}
            )
            self._body = body

        def raise_for_status(self):
            return None

        def iter_content(self, _chunk_size: int):
            yield self._body

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

    with _patched_globals(_open_pinned_response=fake_open):
        body, final_url, content_type = _safe_download(
            "https://example.com/start",
            100,
            timeout=7,
        )

    _check_equal(body, b"finished")
    _check_equal(final_url, "https://example.com/new/page")
    _check_equal(content_type, "text/html; charset=utf-8")
    _check_equal(opened, [
        ("https://example.com/start", 7),
        ("https://example.com/new/page", 7),
    ])


class _FakeApiSession:
    """Stand-in for a per-request Session used by the login self-tests."""

    def __init__(self, response: Any):
        self.headers: Dict[str, str] = {}
        self.trust_env = True
        self.response = response
        self.calls: List[tuple[str, str, Dict[str, Any]]] = []
        self.closed = False

    def request(self, method: str, url: str, **kwargs: Any) -> Any:
        self.calls.append((method, url, kwargs))
        return self.response

    def close(self) -> None:
        self.closed = True


@contextmanager
def _patched_api_session(response: Any) -> Iterator[_FakeApiSession]:
    session = _FakeApiSession(response)
    with _patched_globals(_new_session=lambda: session):
        yield session


def test_safe_download_redirect_limit():
    class FakeResponse:
        status_code = 302

        def __init__(self, location: str):
            self.headers = {"Location": location}

        def raise_for_status(self):
            raise AssertionError("a redirect must never be read as a body")

    hops = []

    @contextmanager
    def fake_open(url: str, _timeout: float):
        # Always a fresh target, so the redirect *limit* is what trips, not the
        # loop detector.
        hops.append(url)
        yield FakeResponse(f"/hop{len(hops)}")

    with _patched_globals(_open_pinned_response=fake_open), _check_raises(
        ValueError,
        "expected an endless redirect chain to fail",
        contains="Too many redirects",
    ):
        _safe_download("https://example.com/a", 100, timeout=7)

    # MAX_REDIRECTS hops followed, so MAX_REDIRECTS + 1 requests were made.
    _check_equal(len(hops), MAX_REDIRECTS + 1)


def test_login_rejects_redirects():
    class FakeResponse:
        status_code = 307
        ok = False
        closed = False

        def close(self):
            self.closed = True

        def raise_for_status(self):
            raise AssertionError("redirect must be rejected before status handling")

    response = FakeResponse()
    with _patched_api_session(response) as session:
        with _check_raises(ValueError, "expected createSession redirect to fail"):
            bsky_login_session("https://pds.example", "alice", "secret")

    _check_equal(len(session.calls), 1)
    method, url, options = session.calls[0]
    _check_equal(method, "POST")
    _check_equal(url, "https://pds.example/xrpc/com.atproto.server.createSession")
    _check(options["allow_redirects"] is False)
    _check(response.closed)
    _check(session.closed, "each API call must close the Session it created")
    _check_equal(options["json"], {"identifier": "alice", "password": "secret"})


def test_login_failure_reports_server_error():
    class FakeResponse:
        status_code = 401
        ok = False
        headers: Dict[str, str] = {}
        closed = False

        def close(self):
            self.closed = True

        def iter_content(self, _chunk_size: int):
            yield json.dumps(
                {
                    "error": "AuthFactorTokenRequired",
                    "message": "A sign in code has been sent to your email",
                }
            ).encode("UTF-8")

    with _patched_api_session(FakeResponse()):
        try:
            bsky_login_session("https://pds.example", "alice", "secret")
        except XrpcError as exc:
            message = str(exc)
        else:
            raise AssertionError("expected a failed login to raise")

    _check("HTTP 401" in message, message)
    _check("AuthFactorTokenRequired" in message, message)
    _check("sign in code has been sent" in message, message)
    # The remedy matters more than the error name for this one.
    _check("app-passwords" in message, message)
    _check("secret" not in message, "the password must never appear in an error")


def test_embed_thumbnail_uses_final_page_url():
    attached = {}

    def fake_download(_url: str, _max_bytes: int, **_kwargs):
        return (
            b'<html><head><meta property="og:image" content="../thumb.png">'
            b'<meta property="og:title" content="Title"></head></html>',
            "https://cdn.example/articles/current/page.html",
            "text/html; charset=utf-8",
        )

    def fake_attach(_card, _pds_url, _access_token, page_url, image_url):
        attached["page_url"] = page_url
        attached["image_url"] = urljoin(page_url, image_url)

    with _patched_globals(
        _safe_download=fake_download,
        _attach_external_thumb=fake_attach,
    ):
        embed = fetch_embed_url_card(
            "https://pds.example",
            "token",
            "https://example.com/original",
        )

    _check_equal(embed["external"]["uri"], "https://example.com/original")
    _check_equal(attached, {
        "page_url": "https://cdn.example/articles/current/page.html",
        "image_url": "https://cdn.example/articles/thumb.png",
    })


def test_thumbnail_failures_are_non_fatal():
    """Every og:image failure, timeouts included, still posts the card.

    Per-request Sessions are what make swallowing a timeout safe here: the
    abandoned worker cannot be sharing state with the createRecord call that
    follows.
    """
    originals = {
        name: vars(bsky)[name]
        for name in ("_safe_download", "inspect_image", "upload_file")
    }

    def attach(card: Dict) -> None:
        # The failure paths warn on stderr; keep --self-test output clean.
        with _patched_attr(sys, "stderr", io.StringIO()):
            _attach_external_thumb(
                card,
                "https://pds.example",
                "token",
                "https://example.com/",
                "https://cdn.example/thumb.png",
            )

    def raise_timeout(*_args, **_kwargs):
        raise DeadlineExceeded("timed out")

    try:
        vars(bsky)["inspect_image"] = lambda *_a: {
            "width": 1,
            "height": 1,
            "mimetype": "image/png",
        }

        vars(bsky)["_safe_download"] = raise_timeout
        card: Dict = {}
        attach(card)
        _check("thumb" not in card, "download timeout must not attach a thumb")

        vars(bsky)["_safe_download"] = lambda *_a: (
            b"bytes",
            "https://cdn.example/",
            None,
        )

        vars(bsky)["upload_file"] = raise_timeout
        attach(card)
        _check("thumb" not in card, "timed-out upload must not attach a thumb")

        def reject_blob(*_args, **_kwargs):
            raise requests.RequestException("blob rejected")

        vars(bsky)["upload_file"] = reject_blob
        attach(card)
        _check("thumb" not in card, "rejected upload must not attach a thumb")

        vars(bsky)["upload_file"] = lambda *_a: {"$type": "blob"}
        attach(card)
        _check_equal(card["thumb"], {"$type": "blob"})
    finally:
        vars(bsky).update(originals)


def test_embed_card_degrades_when_page_unreadable():
    def fail_download(*_args, **_kwargs):
        raise ValueError("Remote response exceeds 4000000 bytes")

    with _patched_globals(_safe_download=fail_download), _patched_attr(
        sys, "stderr", io.StringIO()
    ) as captured_stderr:
        embed = fetch_embed_url_card(
            "https://pds.example",
            "token",
            "https://example.com/huge",
        )
        warning = captured_stderr.getvalue()

    # An unreadable page costs the card's metadata, never the whole post.
    _check_equal(embed, {
        "$type": "app.bsky.embed.external",
        "external": {
            "uri": "https://example.com/huge",
            "title": "",
            "description": "",
        },
    })
    _check("warning:" in warning, warning)


def test_inspect_image():
    buf = io.BytesIO()
    Image.new("RGB", (2, 3)).save(buf, format="PNG")
    _check_equal(inspect_image(buf.getvalue(), "test.png"), {
        "width": 2,
        "height": 3,
        "mimetype": "image/png",
    })
    with _check_raises(ValueError, "expected invalid image to fail"):
        inspect_image(b"not an image", "broken.png")


def test_inspect_image_rejects_webp_canvas_before_decoder():
    # A real 204-byte, two-frame 2x2 WebP from Pillow/libwebp. Its VP8X
    # canvas alone used to allocate roughly 488 MiB before the pixel check.
    original = base64.b64decode(
        "UklGRsQAAABXRUJQVlA4WAoAAAACAAAAAQAAAQAAQU5JTQYAAAAAAAAAAABBTk1GSgAA"
        "AAAAAAAAAAEAAAEAAGQAAAJWUDggMgAAADABAJ0BKgIAAgABQCYloAADcAD+8ut///mw"
        "P/bz/wR6Af//0uD//pcH//S4P/SkAAAAQU5NRkYAAAAAAAAAAAABAAABAABkAAAAVlA4"
        "IC4AAAA0AQCdASoCAAIAAAAmJaAAA3AA/vtV4///S4P/+lwf/9Lg/9Lg//rV5Vesq6AA"
    )
    _check_equal(len(original), 204)
    _check_equal(inspect_image(original, "small.webp"), {
        "width": 2, "height": 2, "mimetype": "image/webp",
    })
    oversized = bytearray(original)
    oversized[24:27] = (8000 - 1).to_bytes(3, "little")
    oversized[27:30] = (8000 - 1).to_bytes(3, "little")

    def forbidden_decoder(_images: List[bytes]):
        raise AssertionError("oversized WebP reached the native decoder worker")

    with _patched_globals(_inspect_images_in_worker=forbidden_decoder):
        with _check_raises(ValueError, "oversized canvas was accepted", contains="too many pixels"):
            inspect_image(bytes(oversized), "oversized.webp")
        with _check_raises(ValueError, "truncated RIFF was accepted", contains="container length"):
            inspect_image(original[:-1], "truncated.webp")


def test_inspect_image_rejects_truncated_jpeg():
    buf = io.BytesIO()
    Image.new("RGB", (128, 128), "red").save(buf, format="JPEG")
    original = buf.getvalue()
    _check_equal(len(original), 885)
    _check_equal(inspect_image(original, "complete.jpg"), {
        "width": 128, "height": 128, "mimetype": "image/jpeg",
    })
    for truncated in (original[:-2], original[:664]):
        with _check_raises(ValueError, "truncated JPEG was accepted", contains="truncated"):
            inspect_image(truncated, "truncated.jpg")


def test_inspect_image_exif_dimensions_and_upload_preserve_bytes():
    originals = {}
    for orientation in range(1, 9):
        exif = Image.Exif()
        exif[274] = orientation
        buf = io.BytesIO()
        Image.new("RGB", (8, 3), "red").save(
            buf, format="JPEG", exif=exif,
            icc_profile=b"opaque jdvrif payload\x00\x01" * 20,
        )
        encoded = buf.getvalue()
        originals[orientation] = encoded
        expected_width, expected_height = (3, 8) if orientation >= 5 else (8, 3)
        _check_equal(inspect_image(encoded, f"orientation-{orientation}.jpg"), {
            "width": expected_width, "height": expected_height, "mimetype": "image/jpeg",
        })

    uploaded = []

    def fake_upload(_pds: str, _token: str, encoded: bytes, mimetype: str) -> Dict:
        uploaded.append((encoded, mimetype))
        return {"$type": "blob", "ref": {"$link": "test-image"}}

    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "oriented.jpg"
        # Preserve both ICC data and arbitrary transport bytes; correction of
        # the display ratio must never re-encode a steganographic carrier.
        original = originals[6] + b"\x00unmodified transport payload\xff"
        path.write_bytes(original)
        with _patched_globals(upload_file=fake_upload):
            embed = upload_images(
                "https://pds.example", "test-token", prepare_images([str(path)])
            )
        _check_equal(uploaded, [(original, "image/jpeg")])
        _check_equal(embed["images"][0]["aspectRatio"], {"width": 3, "height": 8})
        _check_equal(path.read_bytes(), original)


def test_inspect_image_animation_budgets():
    frames = [Image.new("RGB", (2, 3), color) for color in ("red", "blue", "green")]
    buf = io.BytesIO()
    frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:], duration=10)
    animated = buf.getvalue()
    _check_equal(inspect_image(animated, "small.gif"), {
        "width": 2, "height": 3, "mimetype": "image/gif",
    })
    # Exercise the actual decoder loop with a small equivalent pixel budget.
    # The production worker passes the same immutable input to this function.
    with _patched_globals(MAX_IMAGE_DECODED_PIXELS=12), _check_raises(
        ValueError, "animation exceeded its decoded-pixel budget", contains="decoded-pixel limit"
    ):
        _inspect_image_decoded(animated)

    buf = io.BytesIO()
    repeated = [frames[index % 2] for index in range(MAX_IMAGE_FRAMES + 1)]
    repeated[0].save(buf, format="GIF", save_all=True, append_images=repeated[1:], duration=10)
    with _check_raises(ValueError, "animation exceeded its frame budget", contains="frame limit"):
        inspect_image(buf.getvalue(), "too-many-frames.gif")


def test_inspect_image_worker_timeout_reaps_process_without_credentials():
    buf = io.BytesIO()
    Image.new("RGB", (2, 3)).save(buf, format="PNG")
    processes = []
    original_popen = subprocess.Popen

    def recording_popen(*args, **kwargs):
        _check("ATP_AUTH_PASSWORD" not in kwargs["env"], "worker inherited posting credentials")
        _check("AWS_SECRET_ACCESS_KEY" not in kwargs["env"], "worker inherited unrelated credentials")
        process = original_popen(*args, **kwargs)
        processes.append(process)
        return process

    test_environment = dict(os.environ, ATP_AUTH_PASSWORD="must-not-reach-worker",
                            AWS_SECRET_ACCESS_KEY="must-not-reach-worker")
    with _patched_attr(os, "environ", test_environment), _patched_attr(
        subprocess, "Popen", recording_popen
    ), _patched_globals(
        _IMAGE_WORKER_LAUNCHER="import time; time.sleep(30)",
        MAX_IMAGE_DECODE_SECONDS=0.1,
        MAX_IMAGE_WORKER_STARTUP_SECONDS=0,
    ), _check_raises(ValueError, "image worker exceeded its deadline", contains="time limit"):
        inspect_image(buf.getvalue(), "slow.png")
    _check_equal(len(processes), 1)
    _check(processes[0].poll() is not None, "timed-out image worker was not reaped")


def test_inspect_image_worker_storage_failures_skip_thumbnail():
    buf = io.BytesIO()
    Image.new("RGB", (2, 3)).save(buf, format="PNG")
    encoded = buf.getvalue()

    class FailedReply:
        def __init__(self, stage):
            self.stage = stage

        def __enter__(self):
            return self

        def __exit__(self, *_args):
            return False

        def seek(self, _offset):
            if self.stage == "seek":
                raise OSError(errno.EIO, "reply seek failed")

        def read(self, _size):
            raise OSError(errno.EIO, "reply read failed")

    def fake_download(*_args):
        return encoded, "https://cdn.example/thumb.png", "image/png"

    def forbidden_upload(*_args):
        raise AssertionError("failed image validation attempted a thumbnail upload")

    for stage in ("create", "seek", "read"):
        def failed_temporary_file():
            if stage == "create":
                raise OSError(errno.ENOSPC, "temporary storage unavailable")
            return FailedReply(stage)

        card: Dict = {}
        with _patched_attr(tempfile, "TemporaryFile", failed_temporary_file), _patched_attr(
            subprocess, "run", lambda *_a, **_k: type("Completed", (), {"returncode": 0})()
        ), _patched_globals(_safe_download=fake_download, upload_file=forbidden_upload):
            with _check_raises(ValueError, f"worker {stage} failure escaped validation"):
                inspect_image(encoded, "test.png")
            with _patched_attr(sys, "stderr", io.StringIO()) as captured_stderr:
                _attach_external_thumb(
                    card, "https://pds.example", "test-token", "https://example.com/",
                    "https://cdn.example/thumb.png",
                )
                warning = captured_stderr.getvalue()
        _check("thumb" not in card, "failed validation attached a thumbnail")
        _check("posting the card without it" in warning, warning)


def test_image_worker_ignores_caller_directory_modules():
    buf = io.BytesIO()
    Image.new("RGB", (2, 3)).save(buf, format="PNG")
    original_run = subprocess.run
    with tempfile.TemporaryDirectory() as directory:
        # The main script imports relative to its own directory. A child using
        # plain -c would instead import these files from the caller's directory.
        for name in ("requests.py", "runpy.py", "sitecustomize.py"):
            (Path(directory) / name).write_text(
                "raise RuntimeError('untrusted caller directory was imported')\n",
                encoding="utf-8",
            )

        def from_caller_directory(*args, **kwargs):
            return original_run(*args, cwd=directory, **kwargs)

        with _patched_attr(subprocess, "run", from_caller_directory):
            _check_equal(inspect_image(buf.getvalue(), "image.png"), {
                "width": 2, "height": 3, "mimetype": "image/png",
            })


def test_read_image_file_rejects_symlinks():
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        target = root / "real.png"
        Image.new("RGB", (1, 1)).save(target, format="PNG")
        _check(len(_read_image_file(target)) > 0)

        link = root / "link.png"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError):
            return  # No symlink support (e.g. unprivileged Windows); nothing to test.
        # ELOOP from O_NOFOLLOW must not surface as "Too many levels of
        # symbolic links", which reads like a broken filesystem.
        with _check_raises(
            ValueError,
            "expected a symlinked image path to be rejected",
            contains="must not be a symbolic link",
        ):
            _read_image_file(link)

        _check(root.joinpath("missing.png").exists() is False)
        with _check_raises(
            ValueError,
            "expected a missing image path to be rejected",
            contains="Could not read image file",
        ):
            _read_image_file(root / "missing.png")


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
    with _check_raises(ValueError, "expected declared oversized response to fail"):
        _read_response_body(FakeResponse([b"abc"], " 5 "), 4)
    with _check_raises(ValueError, "expected streamed oversized response to fail"):
        _read_response_body(FakeResponse([b"ab", b"cde"]), 4)


def test_read_response_body_content_encodings():
    class FakeResponse:
        def __init__(self, encoding: str, declared: str = ""):
            self.headers = {"Content-Encoding": encoding}
            if declared:
                self.headers["Content-Length"] = declared

        def iter_content(self, _chunk_size: int):
            # iter_content yields decoded bytes, so the cap applies post-decode.
            yield b"decoded"

    # These responses already contain decoded chunks; exercise the caller's
    # accounting independently from the real-decoder regression below.
    with _patched_globals(_urllib3_has_bounded_decoding=lambda: True):
        for encoding in ("", "identity", "gzip", "GZIP", "x-gzip", " deflate "):
            _check_equal(_read_response_body(FakeResponse(encoding), 16), b"decoded")

        # Content-Length describes encoded bytes, not decoded bytes.
        _check_equal(_read_response_body(FakeResponse("gzip", "999999"), 16), b"decoded")
        with _check_raises(ValueError, "expected declared oversized identity body to fail"):
            _read_response_body(FakeResponse("identity", "999999"), 16)
        with _check_raises(ValueError, "expected oversized decoded body to fail"):
            _read_response_body(FakeResponse("gzip"), 3)

    # Anything urllib3 cannot unwrap would reach the parser still encoded.
    with _check_raises(ValueError, "expected undecodable Content-Encoding to fail"):
        _read_response_body(FakeResponse("br-unsupported"), 16)


def test_get_reply_refs_reuses_root_ref():
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

    with _patched_globals(get_record=fake_get_record):
        refs = get_reply_refs(
            "https://pds.example",
            "at://did:plc:parent/app.bsky.feed.post/parent",
        )

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


def test_meta_content_matches_case_insensitively():
    soup = BeautifulSoup(
        b'<meta PROPERTY=" OG:Title " content="Cased title">'
        b'<meta name="Description" content="Cased description">'
        b'<meta property="og:image" content="/thumb.png">',
        "html.parser",
    )
    _check_equal(_meta_content(soup, "og:title"), "Cased title")
    _check_equal(_meta_content(soup, "description"), "Cased description")
    _check_equal(_meta_content(soup, "og:image"), "/thumb.png")
    _check_equal(_meta_content(soup, "og:video"), "")


def test_charset_from_content_type():
    _check_equal(
        _charset_from_content_type("text/html; charset=ISO-8859-1"),
        "ISO-8859-1",
    )
    _check_equal(
        _charset_from_content_type('text/html; charset="utf-8"'),
        "utf-8",
    )
    _check_equal(_charset_from_content_type("text/html"), None)
    _check_equal(_charset_from_content_type(None), None)
    _check_equal(_charset_from_content_type("text/html; charset="), None)


def test_ascii_hostname():
    _check_equal(_ascii_hostname("example.com"), "example.com")
    # Over-long internationalized names never reach idna.encode.
    with _patched_attr(idna, "encode", lambda *_a, **_k: (_ for _ in ()).throw(
        AssertionError("over-long hostname reached idna.encode")
    )), _check_raises(ValueError, "expected an over-long IDN to fail", contains="253"):
        _ascii_hostname("\u0660" * 254)
    # Underscore labels are not valid IDNA2003 but resolve in practice; the
    # ASCII passthrough keeps them usable instead of raising.
    _check_equal(_ascii_hostname("my_cdn.example.com"), "my_cdn.example.com")
    _check_equal(_ascii_hostname("münchen.de"), "xn--mnchen-3ya.de")
    _check_equal(_ascii_hostname("8.8.8.8"), "8.8.8.8")
    with _check_raises(ValueError, "expected invalid internationalized host to fail"):
        _ascii_hostname("\u2764.example")


def test_terminal_safe_neutralizes_control_characters():
    """Remote text must never reach the terminal as live escape sequences."""
    hostile = "bad\r\n\x1b[2Jspoofed\x7f\x9b0m"
    rendered = _terminal_safe(hostile)
    _check(
        not any(ord(ch) < 0x20 or 0x7F <= ord(ch) <= 0x9F for ch in rendered),
        "control characters survived _terminal_safe",
    )
    _check("\\x1b" in rendered and "\\x0d" in rendered)
    # Printable non-ASCII must survive so localized messages stay readable.
    _check_equal(_terminal_safe("naïve ünïcode 日本語"), "naïve ünïcode 日本語")

    summary = _api_error_summary(
        {"error": "Invalid\x1b[31mRequest", "message": "no\r\n\x1b[2Jspoof"}
    )
    _check(
        not any(ord(ch) < 0x20 or 0x7F <= ord(ch) <= 0x9F for ch in summary),
        "XRPC error summary leaked control characters",
    )

    try:
        exit_error("Error: \x1b[2Jspoofed")
    except SystemExit as exc:
        _check("\x1b" not in str(exc), "exit_error leaked an escape sequence")
    else:
        raise AssertionError("exit_error did not raise")


def test_embed_html_parse_is_budgeted():
    """Parsing and metadata traversal share the same deadline."""
    calls = []
    original = _run_before_download_deadline
    original_metadata = _external_card_metadata
    in_budget = [False]

    def recording(action, deadline, operation, **kwargs):
        calls.append((deadline, operation))
        def budgeted_action():
            in_budget[0] = True
            try:
                return action()
            finally:
                in_budget[0] = False
        return original(budgeted_action, deadline, operation, **kwargs)

    def metadata(url, soup):
        _check(in_budget[0], "metadata extraction escaped the parsing budget")
        return original_metadata(url, soup)

    with _patched_globals(
        _run_before_download_deadline=recording,
        _external_card_metadata=metadata,
    ):
        started = time.monotonic()
        card, thumb = _parse_embed_metadata(
            "https://example.com/",
            b"<html><head><meta property='og:title' content='T'>"
            b"<meta property='og:image' content='/thumb.png'></head></html>",
            "text/html; charset=utf-8",
        )

    _check_equal(card["title"], "T")
    _check_equal(thumb, "/thumb.png")
    _check_equal(len(calls), 1)
    deadline, operation = calls[0]
    _check_equal(operation, "parsing the linked page and its metadata")
    _check(
        0 < deadline - started <= MAX_EMBED_PARSE_SECONDS + 1,
        "parse deadline was not derived from MAX_EMBED_PARSE_SECONDS",
    )


def test_nested_page_titles_are_bounded():
    html = (b"<html><head><title>" + b"<span>" * 1200 + b"Nested title"
            + b"</span>" * 1200 + b"</title></head></html>")
    with _patched_globals(
        _safe_download=lambda *_a, **_k: (html, "https://example.com/", "text/html"),
    ):
        embed = fetch_embed_url_card("https://pds.example", "token", "https://example.com/")
    _check_equal(embed["external"]["title"], "Nested title")

    card, _ = _parse_embed_metadata(
        "https://example.com/", b"<title>" + b"a" * 100_000 + b"</title>", "text/html"
    )
    _check_equal(card["title"], "a" * MAX_EXTERNAL_TITLE_CHARS)


def test_embed_metadata_failures_degrade():
    def fail_metadata(*_args):
        raise RecursionError("pathological markup")

    with _patched_globals(
        _safe_download=lambda *_a, **_k: (b"<title>text</title>", "https://example.com/", "text/html"),
        _external_card_metadata=fail_metadata,
    ), _patched_attr(sys, "stderr", io.StringIO()):
        embed = fetch_embed_url_card("https://pds.example", "token", "https://example.com/")
    _check_equal(embed["external"], {"uri": "https://example.com/", "title": "", "description": ""})


def test_nat64_addresses_follow_their_embedded_ipv4():
    """DNS64/NAT64 answers are judged by the IPv4 they carry, not the prefix.

    Python reports all of 64:ff9b::/96 as is_reserved, so without the explicit
    branch every host on an IPv6-only network would be refused.
    """
    public_nat64 = ipaddress.ip_address("64:ff9b::8.8.8.8")
    _check(public_nat64.is_reserved, "fixture no longer exercises the is_reserved path")
    _check(_is_public_unicast_address(public_nat64), "NAT64-wrapped public IPv4 refused")

    for blocked in (
        "64:ff9b::169.254.169.254",   # cloud metadata behind NAT64
        "64:ff9b::127.0.0.1",         # loopback behind NAT64
        "64:ff9b::10.0.0.1",          # RFC1918 behind NAT64
        "64:ff9b:1::7f00:1",          # local-use translation prefix
        "2002:a9fe:a9fe::1",          # 6to4 wrapping metadata
        "2002:0808:0808::1",          # 6to4 stays refused (deprecated)
    ):
        _check(
            not _is_public_unicast_address(ipaddress.ip_address(blocked)),
            f"expected {blocked} to be refused",
        )

    # Older Pythons classify 2002::/16 as global; the refusal must not rely
    # on ipaddress, so simulate that classification.
    class OldPythonAddress(ipaddress.IPv6Address):
        is_global = True
        is_private = False
        is_reserved = False

    for transition in ("2002:0808:0808::1", "2001:0:4136:e378:8000:63bf:3fff:fdd2"):
        _check(
            not _is_public_unicast_address(OldPythonAddress(transition)),
            f"expected {transition} to be refused on any Python",
        )


def test_parse_bsky_app_uri_accepts_default_port():
    expected = {
        "repo": "example.com",
        "collection": "app.bsky.feed.post",
        "rkey": "abc",
    }
    _check_equal(parse_uri("https://bsky.app/profile/example.com/post/abc"), expected)
    _check_equal(
        parse_uri("https://bsky.app:443/profile/example.com/post/abc"),
        expected,
    )
    for rejected in (
        "https://bsky.app:8443/profile/example.com/post/abc",
        "https://bsky.app:80/profile/example.com/post/abc",
    ):
        try:
            parse_uri(rejected)
        except ValueError:
            continue
        raise AssertionError(f"expected {rejected!r} to be rejected")


def test_cashtag_span_matches_text_as_typed():
    """The stored tag is upper-cased; the byte span still covers the raw text."""
    text = "buy $tsla now"
    raw = text.encode("UTF-8")
    spans = [span for span in parse_hashtags(text) if span["tag"].startswith("$")]
    _check_equal(len(spans), 1)
    span = spans[0]
    _check_equal(span["tag"], "$TSLA")
    _check_equal(raw[span["start"]:span["end"]].decode("UTF-8"), "$tsla")


def test_compressed_response_refused_with_unsafe_decoder():
    class UnreadResponse:
        headers = {"Content-Encoding": "gzip"}

        def iter_content(self, _chunk_size):
            raise AssertionError("unsafe decoder must never be entered")

    for version in ("2.3.0", "2.6.3", "2.7.0", "2.8.0rc1", "unknown"):
        with _patched_globals(_URLLIB3_VERSION=version), _check_raises(
            ValueError,
            "expected unsafe decoder version to be refused",
            contains="urllib3 2.8.0 or newer",
        ):
            _read_response_body(UnreadResponse(), 1024)

    for encoding in ("br", "zstd", "gzip, deflate"):
        response = UnreadResponse()
        response.headers = {"Content-Encoding": encoding}
        with _check_raises(ValueError, "expected unsupported decoder to be refused"):
            _read_response_body(response, 1024)


def test_real_gzip_response_allocation_is_bounded():
    import gzip
    import tracemalloc

    def response_for(encoded: bytes) -> requests.Response:
        response = requests.Response()
        response.status_code = 200
        response.headers["Content-Encoding"] = "gzip"
        response.raw = _Urllib3Response(
            body=io.BytesIO(encoded),
            headers=response.headers,
            preload_content=False,
        )
        return response

    # Check real successful decoding on the pinned dependency, while keeping
    # self-tests runnable in an old environment that now safely refuses it.
    normal = response_for(gzip.compress(b"bounded decoded content"))
    try:
        if _urllib3_has_bounded_decoding():
            _check_equal(_read_response_body(normal, 1024), b"bounded decoded content")
        else:
            with _check_raises(ValueError, "expected old decoder to fail closed"):
                _read_response_body(normal, 1024)
    finally:
        normal.close()

    encoded = gzip.compress(b"A" * (32 * 1024 * 1024))
    response = response_for(encoded)
    # Start tracing after fixture creation. The old decoder allocated the
    # entire 32 MiB expansion before the 4 MB application limit could reject it.
    tracemalloc.start()
    try:
        with _check_raises(ValueError, "expected expanded body to exceed the cap"):
            _read_response_body(response, MAX_EMBED_HTML_BYTES)
        peak = tracemalloc.get_traced_memory()[1]
        _check(peak < 16 * 1024 * 1024, f"decoder exceeded allocation budget: {peak}")
    finally:
        tracemalloc.stop()
        response.close()


def test_real_redirect_bodies_stay_unread():
    class CountingBody(io.BytesIO):
        bytes_read = 0

        def read(self, *args):
            data = super().read(*args)
            self.bytes_read += len(data)
            return data

    original_factory = _new_session
    for operation in ("external", "api"):
        redirect_body = CountingBody(b"x" * (8 * 1024 * 1024))
        calls = []

        class OfflineAdapter(HTTPAdapter):
            def send(self, request, **kwargs):
                calls.append(request.url)
                response = requests.Response()
                response.url = request.url
                response.request = request
                response.status_code = 302 if len(calls) == 1 else 200
                if response.status_code == 302:
                    response.headers["Location"] = "/next"
                    response.headers["Content-Length"] = str(8 * 1024 * 1024)
                    body = redirect_body
                else:
                    body = io.BytesIO(b"ok")
                response.raw = _Urllib3Response(
                    body=body, headers=response.headers, preload_content=False
                )
                return response

        def offline_session():
            session = original_factory()
            session.mount("http://", OfflineAdapter())
            return session

        def offline_addresses(url):
            return _parse_url(url, schemes=("http",)), ["8.8.8.8"]

        # The real Requests Session and urllib3 Response run against an adapter
        # that only returns in-memory bodies. No DNS or socket calls are made.
        with _patched_globals(
            _new_session=offline_session,
            _public_url_addresses=offline_addresses,
        ):
            if operation == "external":
                body, final_url, _ = _safe_download("http://example.test/", 2)
                _check_equal(body, b"ok")
                _check_equal(final_url, "http://example.test/next")
                _check_equal(len(calls), 2)
            else:
                with _check_raises(ValueError, "expected API redirect to be refused"):
                    with _open_api_response(
                        "GET", "http://example.test/xrpc/test",
                        timeout=10, operation="testing an offline redirect",
                    ) as response:
                        _raise_for_xrpc(response, "test API")
                _check_equal(len(calls), 1)
        _check_equal(redirect_body.bytes_read, 0)
        _check(redirect_body.closed, "unread redirect body was not closed")


_TEST_CID = "bafyreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"


class _FakeJsonResponse:
    """A streamed API response carrying a JSON body, for the self-tests."""

    def __init__(self, status_code: int, body: Any):
        self.status_code = status_code
        self.ok = status_code < 400
        self.headers: Dict[str, str] = {}
        self.closed = False
        self._body = json.dumps(body).encode("UTF-8")

    def close(self):
        self.closed = True

    def iter_content(self, _chunk_size: int):
        yield self._body

    def raise_for_status(self):
        if not self.ok:
            raise requests.HTTPError(f"HTTP {self.status_code}", response=self)


def test_exit_for_error_keeps_line_breaks():
    try:
        exit_for_error(ValueError("Login failed.\nUse an APP password\x1b[2J"))
    except SystemExit as exc:
        message = str(exc)
    else:
        raise AssertionError("exit_for_error did not raise")
    _check_equal(message.split("\n"), ["Error: Login failed.", "Use an APP password\\x1b[2J"])


def test_new_tid_is_a_sortable_record_key():
    tid_regex = re.compile(r"^[234567abcdefghij][234567abcdefghijklmnopqrstuvwxyz]{12}$")
    for _ in range(50):
        tid = _new_tid()
        _check(tid_regex.fullmatch(tid) is not None, tid)
        _validate_record_key(tid)
    earlier = _new_tid()
    time.sleep(0.002)
    _check(earlier < _new_tid(), "TIDs must sort by creation time")


def test_publish_post_settles_an_uncertain_create():
    attempts: List[str] = []
    lookups: List[str] = []
    exists = [False]

    def found_ref(rkey: str) -> Dict:
        return {"uri": f"at://did:plc:me/app.bsky.feed.post/{rkey}", "cid": _TEST_CID}

    def lookup(_pds: str, _did: str, rkey: str) -> Optional[Dict]:
        lookups.append(rkey)
        return found_ref(rkey) if exists[0] else None

    def publish() -> Any:
        with _patched_attr(sys, "stderr", io.StringIO()):
            return _publish_post("https://pds.example", "token", "did:plc:me", {}, verbose=False)

    # A timed-out attempt that did land is found, not posted a second time.
    def landed_then_timed_out(_pds, _token, _did, _post, rkey, *, verbose):
        attempts.append(rkey)
        exists[0] = True
        raise DeadlineExceeded("timed out")

    with _patched_globals(_create_post_record=landed_then_timed_out, _existing_post=lookup):
        result = publish()
    _check_equal(len(attempts), 1)
    _check_equal(lookups, attempts)
    _check_equal(result, found_ref(attempts[0]))

    # Not found, so retried once with the same key. The first attempt lands
    # late and the retry is rejected as a duplicate; the lookup settles it.
    attempts.clear(), lookups.clear()
    exists[0] = False

    def dropped_then_duplicate(_pds, _token, _did, _post, rkey, *, verbose):
        attempts.append(rkey)
        if len(attempts) == 1:
            raise requests.ConnectionError("connection reset")
        exists[0] = True
        raise requests.HTTPError("duplicate", response=_FakeJsonResponse(400, {}))

    with _patched_globals(_create_post_record=dropped_then_duplicate, _existing_post=lookup):
        result = publish()
    _check_equal(len(attempts), 2)
    _check_equal(attempts[0], attempts[1])
    _check_equal(lookups, attempts)
    _check_equal(result, found_ref(attempts[0]))

    # A definite 4xx rejection is neither looked up nor retried.
    attempts.clear()

    def rejected(_pds, _token, _did, _post, rkey, *, verbose):
        attempts.append(rkey)
        raise requests.HTTPError("invalid record", response=_FakeJsonResponse(400, {}))

    def forbidden_lookup(*_args):
        raise AssertionError("a definite rejection must not be looked up")

    with _patched_globals(
        _create_post_record=rejected, _existing_post=forbidden_lookup
    ), _check_raises(requests.HTTPError, "expected a 4xx rejection to propagate"):
        publish()
    _check_equal(len(attempts), 1)


def test_publish_post_names_a_post_that_may_exist():
    """When settling fails, say which record to check before posting again."""

    def timed_out(*_args, **_kwargs):
        raise DeadlineExceeded("timed out")

    def lookup_fails(*_args):
        raise requests.ConnectionError("PDS unreachable")

    def not_found(*_args):
        return None

    def rejected(*_args, **_kwargs):
        raise requests.HTTPError("invalid record", response=_FakeJsonResponse(400, {}))

    def publish(create: Callable, lookup: Callable, *, rejected_retry: bool = False) -> str:
        calls = [0]

        def create_record(*args, **kwargs):
            calls[0] += 1
            return (rejected if rejected_retry and calls[0] == 2 else create)(*args, **kwargs)

        with _patched_attr(sys, "stderr", io.StringIO()) as stderr, _patched_globals(
            _create_post_record=create_record, _existing_post=lookup
        ), _check_raises(requests.RequestException, "expected the failure to propagate"):
            _publish_post("https://pds.example", "token", "did:plc:me", {}, verbose=False)
        return stderr.getvalue()

    # The lookup itself failed: the first attempt may have landed.
    output = publish(timed_out, lookup_fails)
    _check("may still have been created as at://did:plc:me/app.bsky.feed.post/" in output, output)
    # Both attempts timed out and the post is not there yet: either may land.
    output = publish(timed_out, not_found)
    _check("may still have been created" in output, output)
    # The retry was rejected outright and the post is not there yet, but the
    # first attempt may still be in flight, so the warning stays.
    output = publish(timed_out, not_found, rejected_retry=True)
    _check("may still have been created" in output, output)


def test_api_errors_report_the_server_explanation():
    body = {"error": "RecordNotFound", "message": "Could not locate record"}
    with _patched_api_session(_FakeJsonResponse(400, body)), _check_raises(
        XrpcError,
        "expected getRecord to report the XRPC error",
        contains="getRecord failed with HTTP 400 (RecordNotFound: Could not locate record).",
    ):
        get_record("https://public.api.bsky.app", "at://did:plc:abc/app.bsky.feed.post/3k")

    body = {"error": "BlobTooLarge", "message": "\x1b[2Jtoo big"}
    with _patched_api_session(_FakeJsonResponse(400, body)):
        try:
            upload_file("https://pds.example", "token", b"bytes", "image/png")
        except XrpcError as exc:
            _check_equal(exc.error_name, "BlobTooLarge")
            _check("\x1b" not in str(exc), "server text must be terminal-safe")
            _check_equal(exc.response.status_code, 400)
        else:
            raise AssertionError("expected uploadBlob to report the XRPC error")


def test_existing_post_lookup():
    with _patched_api_session(_FakeJsonResponse(400, {"error": "RecordNotFound"})) as session:
        _check(_existing_post("https://pds.example", "did:plc:me", "3kabc") is None)
    _check_equal(session.calls[0][2]["params"], {
        "repo": "did:plc:me", "collection": "app.bsky.feed.post", "rkey": "3kabc",
    })
    ref = {"uri": "at://did:plc:me/app.bsky.feed.post/3kabc", "cid": _TEST_CID}
    with _patched_api_session(_FakeJsonResponse(200, dict(ref, value={}))):
        _check_equal(_existing_post("https://pds.example", "did:plc:me", "3kabc"), ref)
    with _patched_api_session(_FakeJsonResponse(400, {"error": "InvalidRequest"})), _check_raises(
        XrpcError, "expected an unrelated 400 to fail", contains="InvalidRequest"
    ):
        _existing_post("https://pds.example", "did:plc:me", "3kabc")


def test_create_post_always_revokes_its_session():
    revoked = []
    args = argparse.Namespace(pds_url="https://pds.example", handle="alice.test",
                              password="secret", verbose=False)

    def failing_publish(*_args, **_kwargs):
        raise requests.HTTPError("createRecord failed")

    for publish in (lambda *_a, **_k: {"uri": "at://ok"}, failing_publish):
        with _patched_globals(
            bsky_login_session=lambda *_a: {
                "accessJwt": "access", "refreshJwt": "refresh", "did": "did:plc:me",
            },
            _build_post_record=lambda _args: {"$type": "app.bsky.feed.post", "text": "t"},
            _build_embed=lambda *_a: None,
            _publish_post=publish,
            bsky_logout_session=lambda pds, token: revoked.append((pds, token)),
        ), _patched_attr(sys, "stdout", io.StringIO()), _patched_attr(sys, "stderr", io.StringIO()):
            try:
                create_post(args)
            except requests.HTTPError:
                pass
    _check_equal(revoked, [("https://pds.example", "refresh")] * 2)


def test_logout_sends_refresh_token_and_tolerates_failure():
    with _patched_api_session(_FakeJsonResponse(200, {})) as session:
        bsky_logout_session("https://pds.example", "refresh-token")
    method, url, options = session.calls[0]
    _check_equal((method, url), ("POST", "https://pds.example/xrpc/com.atproto.server.deleteSession"))
    _check_equal(options["headers"], {"Authorization": "Bearer refresh-token"})

    with _patched_api_session(_FakeJsonResponse(500, {})), _patched_attr(
        sys, "stderr", io.StringIO()
    ) as captured_stderr:
        bsky_logout_session("https://pds.example", "refresh-token")
    _check("could not end the login session" in captured_stderr.getvalue())


def test_link_card_metadata_is_trimmed():
    soup = BeautifulSoup(
        '<meta property="og:title" content="  "><title>Real title</title>'
        '<meta property="og:image" content="\n  https://cdn.example/a.jpg\n">',
        "html.parser",
    )
    _check_equal(_external_card_metadata("https://example.com/", soup)["title"], "Real title")
    _check_equal(_meta_content(soup, "og:image"), "https://cdn.example/a.jpg")


def test_html_head_bytes():
    _check_equal(
        _html_head_bytes(b"<html><HEAD><title>t</title></HEAD ><body>x</body>"),
        b"<html><HEAD><title>t</title>",
    )
    # </header> is not </head>; without </head>, the head ends at <body>.
    _check_equal(
        _html_head_bytes(b"<title>t</title><header>h</header><BODY class=x>y"),
        b"<title>t</title><header>h</header>",
    )
    _check_equal(len(_html_head_bytes(b"a" * (MAX_EMBED_HEAD_BYTES + 10))), MAX_EMBED_HEAD_BYTES)
    card, _ = _parse_embed_metadata(
        "https://example.com/",
        b"<head><title>Head title</title></head><body><title>Body</title>",
        "text/html",
    )
    _check_equal(card["title"], "Head title")


def test_safe_download_refuses_non_html_before_reading_body():
    class FakeResponse:
        status_code = 200

        def __init__(self, content_type: Optional[str]):
            self.headers = {"Content-Type": content_type} if content_type else {}

        def raise_for_status(self):
            return None

        def iter_content(self, _chunk_size: int):
            if self.headers.get("Content-Type") == "application/pdf":
                raise AssertionError("a refused body must never be read")
            yield b"ok"

    for content_type, allowed in (
        ("application/pdf", False),
        ("Text/HTML; charset=utf-8", True),
        ("application/xhtml+xml", True),
        (None, True),
    ):
        @contextmanager
        def fake_open(_url: str, _timeout: float):
            yield FakeResponse(content_type)

        with _patched_globals(_open_pinned_response=fake_open):
            if allowed:
                body, _, _ = _safe_download(
                    "https://example.com/", 100, allowed_content_types=EMBED_HTML_CONTENT_TYPES
                )
                _check_equal(body, b"ok")
            else:
                with _check_raises(ValueError, "expected a PDF to be refused", contains="application/pdf"):
                    _safe_download(
                        "https://example.com/", 100, allowed_content_types=EMBED_HTML_CONTENT_TYPES
                    )


def test_hashtags_follow_the_official_composer():
    _check_equal(parse_hashtags("#foo​bar"), [{"start": 0, "end": 4, "tag": "foo"}])
    _check_equal(parse_hashtags("#\U0001f468‍\U0001f469"), [
        {"start": 0, "end": 5, "tag": "\U0001f468"},
    ])
    _check_equal(parse_hashtags("#❤️love"), [
        {"start": 0, "end": 11, "tag": "❤️love"},
    ])
    _check_equal(parse_hashtags("#️x"), [])
    # Only ASCII digits make a tag "numeric only".
    _check_equal(parse_hashtags("#² #123"), [{"start": 0, "end": 3, "tag": "²"}])


def test_parse_bsky_app_uri_lowercases_handles_only():
    _check_equal(
        parse_uri("https://bsky.app/profile/Alice.Example.COM/post/abc")["repo"],
        "alice.example.com",
    )
    _check_equal(parse_uri("https://bsky.app/profile/did:plc:AbC/post/abc")["repo"], "did:plc:AbC")


def test_get_record_rejects_a_different_record():
    def answer(uri: str) -> _FakeJsonResponse:
        return _FakeJsonResponse(200, {"uri": uri, "cid": _TEST_CID, "value": {}})

    requested = "at://did:plc:abc/app.bsky.feed.post/one"
    with _patched_api_session(answer(requested)):
        _check_equal(get_record("https://api.example", requested)["uri"], requested)
    # A handle-addressed request is answered with the account's DID.
    with _patched_api_session(answer(requested)):
        get_record("https://api.example", "https://bsky.app/profile/alice.test/post/one")
    for wrong in (
        "at://did:plc:abc/app.bsky.feed.post/other",
        "at://did:plc:xyz/app.bsky.feed.post/one",
        "at://did:plc:abc/app.bsky.graph.list/one",
    ):
        with _patched_api_session(answer(wrong)), _check_raises(
            ValueError, f"accepted {wrong}", contains="different record"
        ):
            get_record("https://api.example", requested)


def test_mentions_resolve_concurrently_under_one_deadline():
    seen = []

    def slow_resolve(_pds_url: str, handle: str) -> Optional[str]:
        seen.append(_DOWNLOAD_DEADLINE.get())
        time.sleep(0.2)
        return None if handle.startswith("nobody") else "did:plc:" + handle.split(".")[0]

    handles = [f"user{index}.test" for index in range(MENTION_RESOLUTION_WORKERS)]
    with _patched_globals(_resolve_handle=slow_resolve):
        started = time.monotonic()
        resolved = _resolve_handles("https://pds.example", handles)
        elapsed = time.monotonic() - started
    _check(elapsed < 0.2 * len(handles) / 2, f"mentions were resolved serially ({elapsed:.2f}s)")
    _check_equal(resolved, {handle: "did:plc:" + handle.split(".")[0] for handle in handles})
    _check(None not in seen and len(set(seen)) == 1, "lookups did not share one deadline")

    with _patched_globals(_resolve_handle=slow_resolve), _patched_attr(
        sys, "stderr", io.StringIO()
    ) as captured_stderr:
        facets = parse_facets("https://pds.example", "hi @nobody.test and @Bob.test and @bob.test")
    _check("@nobody.test" in captured_stderr.getvalue())
    _check_equal([facet["features"][0]["did"] for facet in facets], ["did:plc:bob"] * 2)


def test_upload_timeout_scales_with_size():
    _check_equal(_upload_timeout(0), UPLOAD_BASE_TIMEOUT_SECONDS)
    # A maximum-size image must fit on a 256 kbit/s uplink.
    _check(_upload_timeout(MAX_IMAGE_SIZE_BYTES) > MAX_IMAGE_SIZE_BYTES * 8 / 256_000)
    body = b"\x89PNG\r\n\x1a\n" + b"x" * 1000
    with _patched_api_session(_FakeJsonResponse(200, {"blob": {"$type": "blob"}})) as session:
        upload_file("https://pds.example", "token", body, "image/png")
    _check_equal(session.calls[0][2]["timeout"], _upload_timeout(len(body)))


def test_prepare_images_uses_one_worker_per_batch():
    runs = []
    original_run = subprocess.run

    def recording_run(*args, **kwargs):
        runs.append(kwargs["timeout"])
        return original_run(*args, **kwargs)

    with tempfile.TemporaryDirectory() as directory:
        paths = []
        for index, size in enumerate(((2, 3), (4, 5))):
            path = Path(directory) / f"image{index}.png"
            Image.new("RGB", size).save(path, format="PNG")
            paths.append(str(path))
        with _patched_attr(subprocess, "run", recording_run):
            prepared = prepare_images(paths, ["first", "second"])
        _check_equal(len(runs), 1)
        _check_equal(runs[0], MAX_IMAGE_WORKER_STARTUP_SECONDS + 2 * MAX_IMAGE_DECODE_SECONDS)
        _check_equal(
            [(info["width"], info["height"], alt) for _, info, alt in prepared],
            [(2, 3, "first"), (4, 5, "second")],
        )

        broken = Path(directory) / "broken.png"
        broken.write_bytes(b"\x89PNG\r\n\x1a\n" + b"not really a PNG")
        with _check_raises(ValueError, "a bad image in a batch was not named",
                           contains=f"{str(broken)!r}"):
            prepare_images([paths[0], str(broken)])


def test_main_validates_images_before_logging_in():
    def forbidden_login(*_args):
        raise AssertionError("logged in before the local images were validated")

    environment = dict(os.environ, ATP_AUTH_HANDLE="alice.test", ATP_AUTH_PASSWORD="secret")
    for name in ("ATP_PDS_HOST", "ATP_RECORD_SERVICE_HOST"):
        environment.pop(name, None)
    with tempfile.TemporaryDirectory() as directory:
        missing = str(Path(directory) / "missing.jpg")
        with _patched_attr(os, "environ", environment), _patched_attr(
            sys, "argv", ["create_bsky_post.py", "hello", "--image", missing]
        ), _patched_globals(bsky_login_session=forbidden_login):
            try:
                main()
            except SystemExit as exc:
                message = str(exc)
            else:
                raise AssertionError("main() accepted a missing image")
    _check("Could not read image file" in message, message)


class _FakeRawResponse(_FakeJsonResponse):
    """A streamed API response with an arbitrary raw body and headers."""

    def __init__(self, status_code: int, body: bytes, headers: Optional[Dict[str, str]] = None):
        super().__init__(status_code, None)
        self._body = body
        self.headers = dict(headers or {})


def _created_ref(rkey: str = "3kabc", did: str = "did:plc:me") -> Dict:
    return {"uri": f"at://{did}/app.bsky.feed.post/{rkey}", "cid": _TEST_CID}


def test_create_post_record_requires_a_matching_record_reference():
    def create(response: Any) -> Any:
        with _patched_api_session(response):
            return _create_post_record(
                "https://pds.example", "token", "did:plc:me", {}, "3kabc", verbose=False
            )

    body = dict(_created_ref(), validationStatus="valid")
    _check_equal(create(_FakeJsonResponse(200, body)), body)
    unusable = [
        _FakeRawResponse(200, b"<html>maintenance</html>"),
        _FakeRawResponse(200, b"x" * 64, {"Content-Encoding": "br"}),
        _FakeRawResponse(200, b"[" * 50_000 + b"]" * 50_000),
        _FakeJsonResponse(200, {"uri": "at://did:plc:me/app.bsky.feed.post/3kabc"}),
        _FakeJsonResponse(200, _created_ref(rkey="3kother")),
        _FakeJsonResponse(200, _created_ref(did="did:plc:someone-else")),
    ]
    for response in unusable:
        with _check_raises(UnusableCreateResponse, "expected an unusable success response"):
            create(response)
    _check(_outcome_uncertain(UnusableCreateResponse("bad body")))
    _check(not _outcome_uncertain(requests.exceptions.InvalidJSONError("unsent")))


def test_existing_post_rejects_a_different_record():
    for ref in (_created_ref(rkey="3kother"), _created_ref(did="did:plc:someone-else")):
        with _patched_api_session(_FakeJsonResponse(200, dict(ref, value={}))), _check_raises(
            ValueError, "expected a mismatched lookup to fail", contains="different record"
        ):
            _existing_post("https://pds.example", "did:plc:me", "3kabc")


def test_publish_post_settles_an_unusable_create_response():
    lookups: List[str] = []

    def unusable(*_args, **_kwargs):
        raise UnusableCreateResponse("createRecord returned a non-JSON response")

    def found(_pds, _did, rkey):
        lookups.append(rkey)
        return _created_ref(rkey)

    with _patched_attr(sys, "stderr", io.StringIO()), _patched_globals(
        _create_post_record=unusable, _existing_post=found
    ):
        result = _publish_post("https://pds.example", "token", "did:plc:me", {}, verbose=False)
    _check_equal(len(lookups), 1)
    _check_equal(result, _created_ref(lookups[0]))

    with _patched_attr(sys, "stderr", io.StringIO()) as stderr, _patched_globals(
        _create_post_record=unusable, _existing_post=lambda *_a: None
    ), _check_raises(UnusableCreateResponse, "expected the failure to propagate"):
        _publish_post("https://pds.example", "token", "did:plc:me", {}, verbose=False)
    _check("may still have been created" in stderr.getvalue(), stderr.getvalue())


def test_deeply_nested_json_is_a_controlled_failure():
    nested = b"[" * 50_000 + b"]" * 50_000
    with _check_raises(ValueError, "expected deep nesting to fail", contains="non-JSON"):
        _json_object(_FakeRawResponse(200, nested), "test")
    with _patched_api_session(_FakeRawResponse(200, b'{"did":' + nested + b"}")):
        _check(_resolve_handle("https://pds.example", "alice.test") is None)
    # An error body is only decoration: the status is still reported.
    with _patched_api_session(_FakeRawResponse(400, nested)), _check_raises(
        XrpcError, "expected the HTTP status to be reported", contains="HTTP 400"
    ):
        get_record("https://public.api.bsky.app", "at://did:plc:abc/app.bsky.feed.post/3k")
    with _patched_api_session(_FakeRawResponse(400, b"x", {"Content-Encoding": "br"})), _check_raises(
        XrpcError, "expected an undecodable error body to keep its status", contains="HTTP 400"
    ):
        get_record("https://public.api.bsky.app", "at://did:plc:abc/app.bsky.feed.post/3k")


def test_api_error_summary_bounds_the_error_name():
    summary = _api_error_summary({"error": "E" * 1_000_000, "message": "m"})
    _check(len(summary) < MAX_ERROR_NAME_CHARS + MAX_ERROR_DETAIL_CHARS + 10, str(len(summary)))


def _wait_until(condition: Callable[[], bool], what: str) -> None:
    give_up = time.monotonic() + 5
    while not condition():
        if time.monotonic() > give_up:
            raise AssertionError(f"timed out waiting for {what}")
        time.sleep(0.01)


def test_abandoned_body_read_closes_its_response_and_session():
    release = threading.Event()
    reading = threading.Event()

    class SlowBody(_FakeRawResponse):
        def iter_content(self, _chunk_size: int):
            reading.set()
            release.wait(5)
            yield b"{}"

    response = SlowBody(200, b"")
    with _patched_api_session(response) as session, _check_raises(
        DeadlineExceeded, "expected the body read to time out"
    ):
        with _open_api_response(
            "GET", "https://pds.example/xrpc/test", timeout=0.2, operation="testing"
        ) as resp:
            _json_object(resp, "test")
    _check(reading.is_set(), "the body read never started")
    # Still in use by the abandoned worker, so nothing may be closed yet.
    _check(not response.closed and not session.closed, "closed while still in use")
    release.set()
    _wait_until(lambda: response.closed and session.closed, "abandoned body cleanup")


def test_abandoned_request_that_fails_late_closes_its_session():
    release = threading.Event()

    class FailingSession(_FakeApiSession):
        def request(self, method: str, url: str, **kwargs: Any) -> Any:
            release.wait(5)
            raise requests.ConnectionError("reset after the deadline")

    session = FailingSession(None)
    with _patched_globals(_new_session=lambda: session), _check_raises(
        DeadlineExceeded, "expected the request to time out"
    ):
        with _open_api_response(
            "GET", "https://pds.example/xrpc/test", timeout=0.2, operation="testing"
        ):
            raise AssertionError("no response should be yielded")
    _check(not session.closed, "closed while still in use")
    release.set()
    _wait_until(lambda: session.closed, "late-failure cleanup")

    # A deadline that expired before launch starts no worker, so the caller's
    # disposal runs at once.
    disposed: List[Any] = []
    with _check_raises(DeadlineExceeded, "expected an expired deadline"):
        _run_before_download_deadline(
            lambda: "never run", time.monotonic() - 1, "testing",
            dispose_abandoned=disposed.append,
        )
    _check_equal(disposed, [None])


def run_self_tests() -> None:
    # Discovered rather than listed: a new test_* function is picked up by
    # writing it, and one can never be silently dropped from a hand-kept list.
    # Each test restores whatever it patches, so the order is irrelevant.
    tests = [
        value
        for name, value in sorted(globals().items())
        if name.startswith("test_") and callable(value)
    ]
    if len(tests) < 30:
        raise AssertionError(f"self-test discovery found only {len(tests)} tests")
    for test in tests:
        test()


if __name__ == "__main__":
    run_self_tests()
    print("self-tests passed")
