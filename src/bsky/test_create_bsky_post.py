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

try:  # only a reference for the <meta> rules; the script does not use it
    from bs4 import BeautifulSoup
except ImportError:
    BeautifulSoup = None
from urllib3.response import HTTPResponse as _Urllib3Response

import signal as _signal

import create_bsky_post as bsky

globals().update(
    {name: value for name, value in vars(bsky).items() if not name.startswith("__")}
)

# The script imports these only where it uses them.
import contextlib
import subprocess
import types
from PIL import Image, ImageFile


# Test helpers over the script's card parsing (no production caller needs them).
def _meta_contents(head_text: str, keys: tuple[str, ...]) -> Dict[str, str]:
    return _card_metadata(head_text, keys)[0]


def _meta_content(head_text: str, property_name: str) -> str:
    return _meta_contents(head_text, (property_name,))[property_name]


def _html_head_bytes(html_bytes: bytes) -> bytes:
    """The part of a page that can hold link-card metadata (see _HeadPrefix)."""
    head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
    head.feed(html_bytes)
    return head.result()


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

    # An invalid candidate is never shortened to a different, valid handle.
    for text in (
        "@alice.bsky.social-",
        "@alice.bsky.123",
        "@alice.bsky.-foo",
        "@alice.bsky." + "a" * 64,
        "@alice.bsky.social_2",
        "@alice.bsky.socialé",
        "@alice.bsky.social\u0301",
        "@alice.bsky.social\u200d",
        "@alice.bsky.social.é",
        "@alice.com-.",
        "@alice.com@bob.com",
    ):
        _check_equal((text, parse_mentions(text)), (text, []))
    # Sentence punctuation after a handle is not part of it.
    _check_equal(parse_mentions("hi @alice.bsky.social."), [
        {"start": 3, "end": 21, "handle": "alice.bsky.social"}
    ])
    _check_equal(parse_mentions("hi @alice.bsky.social... ok"), [
        {"start": 3, "end": 21, "handle": "alice.bsky.social"}
    ])
    _check_equal(parse_mentions("(@alice.com), @bob.com!"), [
        {"start": 1, "end": 11, "handle": "alice.com"},
        {"start": 14, "end": 22, "handle": "bob.com"},
    ])
    longest_label = "a" * 63
    _check_equal(parse_mentions(f"@alice.{longest_label}"), [
        {"start": 0, "end": 70, "handle": f"alice.{longest_label}"}
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


def test_grapheme_safe_prefix_keeps_every_kind_of_mark():
    # A cut never lands before a mark of any kind: nonspacing (Mn, which
    # includes the variation selectors), spacing (Mc) or enclosing (Me).
    for mark in ("\u0301", "\ufe0f", "\u093e", "\u20dd"):
        _check_equal(_grapheme_safe_prefix("ab" + mark + "cd", 2), "a")
    _check_equal(_grapheme_safe_prefix("ab\u200dcd", 3), "ab")  # nor after a ZWJ
    _check_equal(_grapheme_safe_prefix("abcd", 2), "ab")


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


def _facet_texts(text: str, spans: List[Dict]) -> List[str]:
    raw = text.encode("UTF-8")
    return [raw[span["start"]:span["end"]].decode("UTF-8") for span in spans]


def test_invisible_marks_do_not_hide_or_glue_facets():
    """BiDi marks, a BOM and ZWSP are boundaries; joiners still glue words."""
    for prefix in ("\u200e", "\u200f", "\u202a", "\u2066", "\ufeff", "\u200b",
                   "hi \u200e", "(\u200e"):
        text = prefix + "@alice.com"
        _check_equal(_facet_texts(text, parse_mentions(text)), ["@alice.com"])
        text = prefix + "https://example.com/"
        _check_equal(_facet_texts(text, parse_urls(text)), ["https://example.com/"])
    # A mark between a word and the candidate is looked through, so this is
    # still the address-like "foo@alice.com", not a mention.
    for prefix in ("foo\u200e", "foo\ufeff\u200e", "a\u200d", "a\u200c",
                   "a\u00ad", "a\u2060", "\u200d", "\u00ad"):
        text = prefix + "@alice.com"
        _check(parse_mentions(text) == [], f"mention after {prefix!r}")
        text = prefix + "https://example.com/"
        _check(parse_urls(text) == [], f"link after {prefix!r}")
    _check_equal(_facet_texts("@alice.com\u200e.", parse_mentions("@alice.com\u200e.")),
                 ["@alice.com"])
    _check_equal(parse_mentions("@alice.com\u200eb"), [])
    _check_equal(parse_mentions("@alice.com\u200e@x.com"), [])
    _check_equal(_facet_texts("@alice.com\u200bb", parse_mentions("@alice.com\u200bb")),
                 ["@alice.com"])


def test_url_trimming_balances_brackets_and_quotes():
    for text, expected in (
        ("https://example.com/a)_(b)", "https://example.com/a)_(b)"),
        ("(see https://example.com/a)", "https://example.com/a"),
        ("https://en.wikipedia.org/wiki/Foo_(bar)).", "https://en.wikipedia.org/wiki/Foo_(bar)"),
        ("https://example.com/a)(b)", "https://example.com/a)(b)"),
        ("[https://example.com/x[1]]", "https://example.com/x[1]"),
        ("{https://example.com/a}}", "https://example.com/a"),
        ("https://example.com/a))", "https://example.com/a"),
        ("See “https://example.com”!", "https://example.com"),
        ("‘https://example.com/x’.", "https://example.com/x"),
        ("«https://example.com/x»", "https://example.com/x"),
        ("「https://example.com/x」", "https://example.com/x"),
        ("（https://example.com/x）", "https://example.com/x"),
        ("https://example.com/caf\u00e9", "https://example.com/caf\u00e9"),
    ):
        _check_equal(_facet_texts(text, parse_urls(text)), [expected])
        _check_equal([span["url"] for span in parse_urls(text)], [expected])


def test_only_http_and_https_urls_are_parsed():
    for url in ("http://example.com/", "HTTPS://example.com/", "Http://example.com/"):
        _parse_url(url)
    for url in ("ftp://example.com/", "file:///etc/passwd", "javascript:alert(1)", "//example.com/"):
        with _check_raises(ValueError, f"accepted {url!r}", contains="http, https"):
            _parse_url(url)


def test_link_hosts_and_ports_must_be_real():
    for text in ("https://.../x", "https://./x", "https://a..b/x", "https://.a/x",
                 "https://a../x", "https://example.com:0/path", "https://example.com:65536/"):
        _check(parse_urls(text) == [], text)
        with _check_raises(ValueError, f"expected {text!r} to fail"):
            _parse_url(text)
    for text in ("https://example.com./ok", "https://example.com:1/", "https://example.com:65535/",
                 "https://[::1]:8443/", "https://127.0.0.1/"):
        _check_equal([span["url"] for span in parse_urls(text)], [text])


def test_parse_hashtags_never_overlaps():
    _check_equal(parse_hashtags("#foo($TSLA)"), [{"start": 0, "end": 10, "tag": "foo($TSLA"}])
    spans = parse_hashtags("#a($B) ($C) #d")
    _check_equal([span["tag"] for span in spans], ["a($B", "$C", "d"])
    for left, right in zip(spans, spans[1:]):
        _check(left["end"] <= right["start"], f"overlapping spans {spans}")


def test_rejected_urls_hide_their_handles():
    calls: List[str] = []

    def fake_resolve(_pds_url: str, handle: str) -> str:
        calls.append(handle)
        return "did:plc:test"

    with _patched_globals(_resolve_handle=fake_resolve):
        for text in (
            "https://user:@alice.bsky.social/path",
            "https://example.com/50%/@alice.bsky.social",
            "https://.../@alice.bsky.social",
            "x https://example.com:0/($TSLA",
        ):
            _check(parse_facets("https://pds.example", text) == [], text)
        facets = parse_facets("https://pds.example", "https://example.com/ @alice.bsky.social")
    _check_equal(calls, ["alice.bsky.social"])
    _check_equal([facet["features"][0]["$type"] for facet in facets], [
        "app.bsky.richtext.facet#link",
        "app.bsky.richtext.facet#mention",
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
    # Empty userinfo and an empty ";" parameter parse as "" and must still fail.
    for uri in (
        "https://@bsky.app/profile/alice.bsky.social/post/123",
        "https://:@bsky.app/profile/alice.bsky.social/post/123",
        "https://bsky.app/profile/alice.bsky.social/post/123;",
        "https://bsky.app/profile/alice.bsky.social/post/123;a=b",
        "https://bsky.app/profile/alice.bsky.social;/post/123",
        "at://did:plc:abc/app.bsky.feed.post/3k;",
        "at://did:plc:abc/app.bsky.feed.post/3k;a=b",
    ):
        with _check_raises(ValueError, f"expected {uri!r} to fail"):
            parse_uri(uri)
    _check_equal(parse_uri("https://bsky.app:443/profile/alice.bsky.social/post/123"), {
        "repo": "alice.bsky.social", "collection": "app.bsky.feed.post", "rkey": "123",
    })
    # An empty "?" or "#" parses as no query or fragment at all, but the URI
    # would still travel verbatim into a reply or quote, so it must fail.
    for suffix in ("?", "#", "?#", "?a", "#a"):
        uri = "at://did:plc:abc/app.bsky.feed.post/123" + suffix
        with _check_raises(ValueError, f"expected {uri!r} to fail"):
            parse_uri(uri)
        with _check_raises(ValueError, f"expected record_ref to refuse {uri!r}"):
            record_ref({"uri": uri, "cid": "bafyreie5737gdxlw5i64vzichcalba3z2v5n6icifvx5xytvske7mr3hpm"})
    _check_equal(
        record_ref({
            "uri": "at://did:plc:abc/app.bsky.feed.post/123",
            "cid": "bafyreie5737gdxlw5i64vzichcalba3z2v5n6icifvx5xytvske7mr3hpm",
        })["uri"],
        "at://did:plc:abc/app.bsky.feed.post/123",
    )
    # Handles fold to lowercase; DIDs never do. A "DID:" prefix in any case is
    # refused, not lowercased into a different, valid-looking DID.
    _check_equal(parse_uri("https://bsky.app/profile/Alice.Bsky.Social/post/123")["repo"],
                 "alice.bsky.social")
    _check_equal(parse_uri("https://bsky.app/profile/did:key:zQ3shAbC/post/123")["repo"],
                 "did:key:zQ3shAbC")
    # Nor is non-ASCII folded into some other account's ASCII handle.
    for repo in ("\u212aate.bsky.social", "alice.bs\u212ay.social"):
        with _check_raises(ValueError, f"expected repo {repo!r} to fail"):
            parse_uri(f"https://bsky.app/profile/{repo}/post/123")
    for repo in ("DID:key:zQ3shAbC", "Did:plc:abc", "dID:plc:abc", "DID:plc:abc"):
        with _check_raises(ValueError, f"expected repo {repo!r} to fail"):
            parse_uri(f"https://bsky.app/profile/{repo}/post/123")


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
    # requests sends these to evil.com / a path under bsky.social, so the
    # loopback and no-path checks must not see a different host.
    for url in (
        "http://evil.com\\.localhost",
        "http://evil.com\\.127.0.0.1",
        "https://bsky.social\\prefix",
    ):
        with _check_raises(ValueError, f"expected backslash host to fail: {url!r}"):
            normalize_service_url(url, service_name="PDS", allow_insecure=True)
    # Plain HTTP only where the resolver cannot be asked: a loopback IP
    # literal, or localhost itself. A trailing dot makes "127.0.0.1." a name.
    for url in ("http://127.0.0.1.:2583", "http://localhost.:2583", "http://pds.localhost:2583",
                "http://[::1].:2583", "http://127.0.0.1.example:2583"):
        with _check_raises(ValueError, f"insecure {url!r} was allowed"):
            normalize_service_url(url, service_name="PDS", allow_insecure=True)
    _check_equal(normalize_service_url("https://pds.localhost.", service_name="PDS"),
                 "https://pds.localhost.")
    # A host urlparse accepts but that is no DNS name or IP is refused too.
    for url in ('https://ev"il.com', "https://a..b", "https://-a.example",
                "http://evil.com%2f.localhost", "https://:@bsky.social",
                "https://@bsky.social", "https://bsky.social/;", "https://bsky.social/;a"):
        with _check_raises(ValueError, f"expected bad service host to fail: {url!r}"):
            normalize_service_url(url, service_name="PDS", allow_insecure=True)
    for url, expected in (
        ("http://[::1]:2583/", "http://[::1]:2583"),
        ("http://127.0.0.1", "http://127.0.0.1"),
        ("https://pds_1.example.com.", "https://pds_1.example.com."),
        ("https://bücher.example", "https://xn--bcher-kva.example"),
        ("http://LOCALHOST:2583", "http://localhost:2583"),
        ("http://[0:0::1]:2583", "http://[::1]:2583"),
        ("http://127\u30020\u30020\u30021:2583", "http://127.0.0.1:2583"),
    ):
        _check_equal(
            normalize_service_url(url, service_name="PDS", allow_insecure=True),
            expected,
        )


def test_url_security_checks():
    for url in ("http://127.0.0.1/", "http://[::1]/"):
        with _check_raises(ValueError, f"expected local URL to fail: {url}"):
            _public_url_addresses(url)
    for url in ("https://user:pass@example.com/", "https://@example.com/",
                "https://:@example.com/", "https://user@example.com/"):
        with _check_raises(ValueError, f"expected URL credentials to fail: {url!r}",
                           contains="credentials"):
            _parse_url(url)
    # Every fetched URL (link cards, redirects, images) shares this check.
    for url in ("https://example.com\\@127.0.0.1/", "https://example.com\\x/",
                "https://example.com:443\\/"):
        with _check_raises(ValueError, f"expected backslash authority to fail: {url!r}"):
            _parse_url(url)
    _check_equal(
        _parse_url("https://example.com/a\\b").path, "/a\\b"
    )


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


def test_pinned_adapter_drops_the_dns_root_dot():
    # urllib3 strips a root dot from SNI but matches the certificate against
    # assert_hostname as given, and "example.com." matches no certificate.
    from urllib3.connection import _match_hostname

    for host, expected in (
        ("example.com.", "example.com"),
        ("münchen.de.", "xn--mnchen-3ya.de"),
        ("example.com", "example.com"),
    ):
        adapter = _PinnedHTTPSAdapter(host)
        try:
            pool_options = adapter.poolmanager.connection_pool_kw
            _check_equal(pool_options["assert_hostname"], expected)
            _check_equal(pool_options["server_hostname"], expected)
            cert = {"subjectAltName": (("DNS", expected),)}
            _match_hostname(cert, pool_options["assert_hostname"])
        finally:
            adapter.close()


def test_url_and_hostname_lengths_are_capped_before_use():
    # The character check is exactly the old per-character rule.
    every = "".join(map(chr, range(sys.maxunicode + 1)))
    found = set(URL_FORBIDDEN_CHARACTER_REGEX.findall(every))
    _check_equal(found, {ch for ch in every if ch.isspace() or ord(ch) < 0x20 or ord(ch) == 0x7F})
    for bad in ("https://example.com/a b", "https://example.com/\u3000", "https://example.com/\x7f"):
        with _check_raises(ValueError, f"accepted {bad!r}", contains="Invalid URL"):
            _parse_url(bad)

    _check_equal(MAX_URL_LENGTH, 8192)
    base = "https://example.com/"
    _parse_url(base + "a" * (MAX_URL_LENGTH - len(base)))
    with _check_raises(ValueError, "accepted an over-long URL", contains="exceeds 8192"):
        _parse_url(base + "a" * (MAX_URL_LENGTH - len(base) + 1))

    # 253 characters is the longest DNS name, ASCII or not; one root dot is free.
    _check_equal(MAX_HOSTNAME_LENGTH, 253)
    longest = ".".join(["a" * 63] * 3 + ["b" * 61])
    _check_equal(len(longest), 253)
    for host in (longest, longest + "."):
        _check_equal(_parse_url(f"https://{host}/").hostname, host)
        _check_equal(_ascii_hostname(host), host)
    for host in ("c" + longest, "c" + longest + ".", "ü" + longest[1:] + "a"):
        with _check_raises(ValueError, f"accepted a {len(host)}-character host", contains="253"):
            _ascii_hostname(host)
        with _check_raises(ValueError, f"parsed a {len(host)}-character host", contains="253"):
            _parse_url(f"https://{host}/")

    # An over-long host never reaches the resolver.
    asked: List[str] = []
    with _patched_attr(socket, "getaddrinfo", lambda host, *_a, **_k: asked.append(host)):
        with _check_raises(ValueError, "resolved an over-long host", contains="253"):
            _public_url_addresses(f"https://c{longest}/x.png")
    _check_equal(asked, [])

    # Logs keep at most a host's worth of an over-long authority.
    _check_equal(_url_for_log(f"https://{longest}/p"), f"https://{longest}/…")
    logged = _url_for_log("https://" + "a." * 400_000 + "com/x.png")
    _check_equal(logged, "https://" + ("a." * 400_000)[:253] + "…/…")


def test_https_page_never_fetches_its_thumbnail_over_http():
    fetched: List[str] = []

    def fake_download(url: str, *_args: Any, **_kwargs: Any) -> Any:
        fetched.append(url)
        raise requests.ConnectionError("not fetched in this test")

    cases = (
        ("https://example.com/a/", "http://cdn.example/x.png", []),
        ("https://example.com/a/", "HTTP://cdn.example/x.png", []),
        ("https://example.com/a/", "ftp://cdn.example/x.png", []),
        ("HTTPS://example.com/a/", "http://cdn.example/x.png", []),
        ("https://example.com/a/", "//cdn.example/x.png", ["https://cdn.example/x.png"]),
        ("https://example.com/a/", "x.png", ["https://example.com/a/x.png"]),
        ("https://example.com/a/", "https://cdn.example/x.png", ["https://cdn.example/x.png"]),
        # A page that was itself plain HTTP loses nothing more.
        ("http://example.com/a/", "http://cdn.example/x.png", ["http://cdn.example/x.png"]),
    )
    for page_url, img_url, expected in cases:
        fetched.clear()
        card: Dict[str, Any] = {"uri": page_url, "title": "", "description": ""}
        with _patched_globals(_safe_download=fake_download), \
                _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            _attach_external_thumb(card, "https://pds.example", "token", page_url, img_url)
        _check_equal((page_url, img_url, fetched), (page_url, img_url, expected))
        _check("thumb" not in card, "a thumb was attached")
        if not expected:
            _check("HTTP og:image on an HTTPS page" in stderr.getvalue(), stderr.getvalue())


def test_hostile_og_image_url_costs_one_short_warning():
    card: Dict[str, Any] = {"uri": "https://example.com/", "title": "", "description": ""}
    def no_fetch(*_args: Any, **_kwargs: Any) -> Any:
        raise AssertionError("an over-long og:image URL was fetched")

    with _patched_attr(socket, "getaddrinfo", no_fetch), \
            _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        _attach_external_thumb(card, "https://pds.example", "token", "https://example.com/",
                               "https://" + "a." * 400_000 + "com/x.png")
    _check("exceeds 8192" in stderr.getvalue(), stderr.getvalue()[:300])
    # Measured before urljoin, which is slow on a long run of "../".
    joined: List[str] = []
    real_urljoin = urljoin

    def recording_urljoin(base: str, url: str) -> str:
        joined.append(url)
        return real_urljoin(base, url)

    with _patched_globals(urljoin=recording_urljoin), _patched_attr(socket, "getaddrinfo", no_fetch), \
            _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        _attach_external_thumb(card, "https://pds.example", "token", "https://example.com/a/",
                               "../" * 333_333 + "x.png")
    _check_equal(joined, [])
    _check("exceeds 8192" in stderr.getvalue(), stderr.getvalue()[:300])
    _check("thumb" not in card, "a card thumb was attached")
    _check(len(stderr.getvalue()) < 1000, f"{len(stderr.getvalue())}-character warning")


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

    # TLS and Host name the A-label of an internationalized host, and every
    # address tried sends the same one.
    for url, addresses, host in (
        ("https://example.com:8443/path", ["8.8.8.8"], "example.com"),
        ("https://faß.example:8443/path", ["8.8.8.8"], "xn--fa-hia.example"),
        ("https://faß.example/path", ["8.8.8.8", "8.8.4.4"], "xn--fa-hia.example"),
    ):
        sessions = []

        def fake_resolver(requested: str, url=url, addresses=addresses):
            _check_equal(requested, url)
            return urlparse(requested), list(addresses)

        def fake_session_factory():
            session = FakeSession()
            sessions.append(session)
            if len(sessions) < len(addresses):
                session.get = lambda *_args, **_kwargs: (_ for _ in ()).throw(
                    requests.ConnectionError("refused")
                )
            return session

        with _patched_globals(_public_url_addresses=fake_resolver), _patched_globals(
            _NoRedirectSession=fake_session_factory
        ):
            with _open_pinned_response(url, timeout=9) as response:
                _check(not response.closed)

        _check_equal(len(sessions), len(addresses))
        session = sessions[-1]
        port = urlparse(url).port
        _check(all(tried.closed for tried in sessions))
        _check(session.trust_env is False)
        _check_equal(len(session.mounts), 1)
        prefix, adapter = session.mounts[0]
        _check_equal(prefix, "https://")
        for tried in sessions:
            options = tried.mounts[0][1].poolmanager.connection_pool_kw
            _check_equal(options["assert_hostname"], host)
            _check_equal(options["server_hostname"], host)
        _check_equal(session.get_calls, [
            (
                f"https://{addresses[-1]}{'' if port is None else f':{port}'}/path",
                {
                    "headers": {"Host": host if port is None else f"{host}:{port}"},
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


def test_safe_download_refuses_https_to_http_redirects():
    class FakeResponse:
        def __init__(self, location: Optional[str]):
            self.status_code = 302 if location else 200
            self.headers = {"Location": location} if location else {}

        def iter_content(self, _chunk_size: int):
            yield b"page"

    for start, target, allowed in (
        ("https://example.com/a", "http://example.com/b", False),
        ("HTTPS://example.com/a", "HTTP://example.com/b", False),
        ("https://example.com/a", "https://cdn.example.com/b", True),
        ("http://example.com/a", "https://example.com/b", True),
        ("http://example.com/a", "http://example.com/b", True),
    ):
        opened = []

        @contextmanager
        def fake_open(url: str, _timeout: float, start=start, target=target):
            opened.append(url)
            yield FakeResponse(target if url == start else None)

        with _patched_globals(_open_pinned_response=fake_open):
            if allowed:
                _check_equal(_safe_download(start, 100)[1], target)
            else:
                with _check_raises(ValueError, f"followed {start} -> {target}",
                                   contains="HTTPS-to-HTTP"):
                    _safe_download(start, 100)
        # A refused target is never even opened.
        _check_equal(opened, [start, target] if allowed else [start])


@contextmanager
def _python_calls_every_address_global() -> Iterator[None]:
    """Make ipaddress call everything a public address, as an old (or
    differently tabled) Python release does for some special ranges."""
    patches = []
    for cls in (ipaddress.IPv4Address, ipaddress.IPv6Address):
        for name, value in (("is_global", True), ("is_private", False), ("is_reserved", False),
                            ("is_loopback", False), ("is_link_local", False),
                            ("is_multicast", False), ("is_unspecified", False),
                            ("is_site_local", False)):
            if name in vars(cls) or any(name in vars(base) for base in cls.__mro__):
                patches.append(_patched_attr(cls, name, property(lambda _self, v=value: v)))
    with contextlib.ExitStack() as stack:
        for patch in patches:
            stack.enter_context(patch)
        yield


def test_special_ranges_isatap_and_zones_are_never_public():
    """Low-severity SSRF hardening: the same answer on every Python release."""
    public = _is_public_unicast_address
    special = ["192.0.0.0", "192.0.0.9", "192.0.0.255", "198.18.0.0", "198.19.255.255",
               "100::", "100::ffff:ffff:ffff:ffff", "2001:2::1", "2001:10::1", "2001:20::1",
               "2001:2f:ffff::1", "2001:30::1", "3fff::1", "3fff:fff::1", "5f00::1",
               "::ffff:8.8.8.8", "::ffff:127.0.0.1", "::ffff:0:8.8.8.8", "::ffff:0:127.0.0.1"]
    neighbours = ["192.0.1.1", "198.17.255.1", "198.20.0.1", "8.8.8.8",
                  "2606:4700:4700::1111", "2a00:1450::1"]
    for simulate_old_python in (False, True):
        with (_python_calls_every_address_global() if simulate_old_python
              else contextlib.nullcontext()):
            for text in special:
                _check(not public(ipaddress.ip_address(text)), f"{text} counted as public")
            # The simulation is real: Python's own tables no longer refuse 10/8.
            _check_equal(public(ipaddress.ip_address("10.0.0.1")), simulate_old_python)
    for text in neighbours:
        _check(public(ipaddress.ip_address(text)), f"{text} was refused")

    # ISATAP interface IDs carry an IPv4 address, with or without the u bit.
    for text, expected in (("2001:4860::5efe:7f00:1", False), ("2001:4860::200:5efe:a00:1", False),
                           ("2001:4860::5efe:c0a8:101", False), ("2001:4860::5efe:808:808", True),
                           ("2001:4860::200:5efe:808:808", True),
                           ("2001:4860::100:5efe:7f00:1", True)):  # not an ISATAP ID
        _check_equal((text, public(ipaddress.ip_address(text))), (text, expected))

    # A DNS answer in IPv4-mapped form is judged as the IPv4 address it is.
    for answer, expected in (("::ffff:8.8.8.8", ["8.8.8.8"]), ("::ffff:127.0.0.1", None)):
        def mapped(_host: str, port: int, answer: str = answer, **_kwargs: Any) -> Any:
            return [(socket.AF_INET6, socket.SOCK_STREAM, 6, "", (answer, port, 0, 0))]

        with _patched_attr(socket, "getaddrinfo", mapped):
            if expected is None:
                with _check_raises(ValueError, f"{answer} was accepted", contains="non-public"):
                    _public_url_addresses("https://example.com/")
            else:
                _check_equal(_public_url_addresses("https://example.com/")[1], expected)

    # A zone never belongs to a public destination, whether in an answer...
    _check(not public(ipaddress.ip_address("2606:4700:4700::1111%eth0")))
    asked: List[str] = []

    def getaddrinfo(host: str, port: int, **_kwargs: Any) -> Any:
        asked.append(host)
        return [(socket.AF_INET6, socket.SOCK_STREAM, 6, "", ("2606:4700:4700::1111%1", port, 0, 1))]

    with _patched_attr(socket, "getaddrinfo", getaddrinfo):
        with _check_raises(ValueError, "a scoped answer was accepted", contains="non-public"):
            _public_url_addresses("https://example.com/")
        # ...or in the URL, where it is refused before DNS (it would also go
        # out in the Host header), as is any other "%" in a host.
        asked.clear()
        for url in ("http://[2606:4700:4700::1111%01]/", "https://[2606:4700:4700::1111%25eth0]/",
                    "https://exa%41mple.com/"):
            with _check_raises(ValueError, f"{url} was fetched", contains="invalid host"):
                _public_url_addresses(url)
        _check_equal(asked, [])


def test_low_severity_hardening_batch():
    # A backslash ends the authority for Requests, not urlparse: never log
    # what follows it as if it were the host.
    for url in ("https://cdn.example.com\\secret_token/img.png", "https://a\\b@c/x"):
        _check_equal(_url_for_log(url), "<redacted URL>")
    _check_equal(_url_for_log("https://cdn.example.com/secret_token/img.png"),
                 "https://cdn.example.com/…")

    # A Location over the URL cap is refused before urljoin sees it.
    class Redirect:
        def __init__(self, location: str):
            self.headers = {"Location": location}

    def forbidden_urljoin(*_args: Any) -> str:
        raise AssertionError("urljoin ran on an oversized Location")

    with _patched_globals(urljoin=forbidden_urljoin), _check_raises(
        ValueError, "an oversized Location was followed", contains="exceeds"
    ):
        _redirect_target(Redirect("/" + "../" * MAX_URL_LENGTH), "https://example.com/")
    at_cap = "https://example.com/" + "a" * (MAX_URL_LENGTH - 20)
    _check_equal(len(at_cap), MAX_URL_LENGTH)
    _check_equal(_redirect_target(Redirect(at_cap), "https://example.com/"), at_cap)

    # Refused header values are quoted only in part.
    with _check_raises(ValueError, "an undecodable encoding was read") as _:
        try:
            _read_response_body(_FakeRawResponse(200, b"x", {"Content-Encoding": "z" * 100_000}), 100)
        except ValueError as exc:
            _check(len(str(exc)) < 2 * MAX_ECHOED_HEADER_CHARS, str(len(str(exc))))
            raise

    class Page:
        status_code = 200
        headers = {"Content-Type": "text/" + "x" * 100_000}

        def raise_for_status(self):
            return None

    @contextmanager
    def fake_open(_url: str, _timeout: float) -> Iterator[Any]:
        yield Page()

    with _patched_globals(_open_pinned_response=fake_open):
        try:
            _safe_download("https://example.com/", 100, allowed_content_types=EMBED_HTML_CONTENT_TYPES)
        except ValueError as exc:
            _check(len(str(exc)) < 2 * MAX_ECHOED_HEADER_CHARS + 60, str(len(str(exc))))
        else:
            raise AssertionError("an unexpected media type was read")

    # A UTF-16 or UTF-32 <meta> label is ignored by the codec it names, so
    # aliases cannot make ASCII markup decode as CJK.
    for label in ("utf-16", "utf16", "u16", "utf_16le", "UTF-16BE", "utf32", "u32", "utf_32_le"):
        head = f'<meta charset="{label}"><title>caf\u00e9</title>'.encode()
        _check("<title>caf\u00e9</title>" in _decode_html_head(head, None), label)
    # A server's own UTF-16 header is still honoured.
    utf16 = "<title>x</title>".encode("utf-16-le")
    _check_equal(_decode_html_head(utf16, "text/html; charset=utf-16le"), "<title>x</title>")

    # Windows' locale-dependent code pages are no page's charset.
    real_lookup = codecs.lookup
    for name in ("mbcs", "oem"):
        windows_codec = codecs.CodecInfo(lambda *_a: (b"", 0), lambda *_a: ("", 0), name=name)
        with _patched_attr(codecs, "lookup",
                           lambda label, c=windows_codec: c if label in ("mbcs", "oem", "dbcs")
                           else real_lookup(label)):
            _check(_html_charset_codec(name) is None, name)
    _check(_html_charset_codec("windows-1252") is not None)

    # Image files are opened without becoming a controlling terminal.
    opened: List[int] = []
    real_open = os.open

    def recording_open(path: Any, flags: int, *args: Any) -> int:
        opened.append(flags)
        return real_open(path, flags, *args)

    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / "a.png"
        Image.new("RGB", (1, 1)).save(target, format="PNG")
        with _patched_attr(os, "open", recording_open):
            _read_image_file(target)
    if hasattr(os, "O_NOCTTY"):
        _check_equal(len(opened), 1)
        _check(opened[0] & os.O_NOCTTY, "image opened without O_NOCTTY")

    # One host with and without its root dot is resolved once per card.
    asked: List[str] = []

    def getaddrinfo(host: str, port: int, **_kwargs: Any) -> Any:
        asked.append(host)
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("93.184.216.34", port))]

    with _patched_attr(socket, "getaddrinfo", getaddrinfo):
        token = _RESOLVED_ADDRESSES.set({})
        try:
            for url in ("https://example.com./a", "https://example.com/b", "https://Example.COM./c"):
                _check_equal(_public_url_addresses(url)[1], ["93.184.216.34"])
        finally:
            _RESOLVED_ADDRESSES.reset(token)
    _check_equal(len(asked), 1)

    # Only DNS labels (or IP literals) reach the resolver.
    for host in ("exa!mple.com", "a*b.example", "a(b).example", "a'b.example", "a,b.example",
                 "a;b.example", "a=b.example", "a+b.example", "a$b.example", "-a.example",
                 "a-.example", "a" * 64 + ".example"):
        with _check_raises(ValueError, f"{host!r} was accepted", contains="Invalid hostname"):
            _ascii_hostname(host)
    for host in ("_dmarc.example.com", "xn--bcher-kva.example", "example.com.", "a-b.c",
                 "8.8.8.8", "2606:4700:4700::1111"):
        _ascii_hostname(host)

    # --verbose prints an error body compactly and boundedly, however nested.
    for body, limit in ((_nested_list(5_000), 10_001), ("x" * 1_000_000, None),
                        (_nested_list(200_000), None)):
        with _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            _print_verbose_error_body(body)
        printed = stderr.getvalue()
        _check(len(printed) <= MAX_VERBOSE_ERROR_BODY_CHARS + 64, str(len(printed)))
        _check_equal(printed.count("\n"), 1)
        if limit is not None:
            _check_equal(len(printed), limit + 1)
    # Past the cap, the cut is marked with what was left out.
    with _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        _print_verbose_error_body("x" * MAX_VERBOSE_ERROR_BODY_CHARS)
    _check(stderr.getvalue().endswith("… (2 more characters)\n"), stderr.getvalue()[-40:])
    # A body that could not be read is said so, not printed as a bare "null".
    with _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        _print_verbose_error_body(None)
    _check_equal(stderr.getvalue(), "<error body could not be read>\n")


def _nested_list(depth: int) -> Any:
    nested: Any = 0
    for _ in range(depth):
        nested = [nested]
    return nested


def test_url_for_log_output_is_terminal_safe():
    """Every caller may print _url_for_log's result as it is."""
    for url in ("https://evil\x9b2J\u202e.com/x", "https://a\u2028b.example/",
                "https://\u2066x\u2069.example:443/path"):
        shown = _url_for_log(url)
        _check(not TERMINAL_UNSAFE_CHARACTERS.search(shown), repr(shown))
        _check("\\x9b" in shown or "\\u2028" in shown or "\\u2066" in shown, shown)
        message = str(_raised(ValueError, parse_uri, url))
        _check(not TERMINAL_UNSAFE_CHARACTERS.search(message), repr(message))
    _check_equal(_url_for_log("https://example.com:8443/a?b"), "https://example.com:8443/…")


def test_card_title_is_unescaped_exactly_once():
    """html.parser from Python 3.14 decodes <title> references itself."""
    head = "<head><title>&lt;b&gt; &amp;amp; caf&eacute;</title></head>"
    expected = "<b> &amp; caf\u00e9"
    _check_equal(_card_metadata(head)[1], expected)

    class EscapableTitleParser(_CardMetadataParser):
        # What Python 3.14's html.parser does with escapable raw text.
        RCDATA_CONTENT_ELEMENTS = ("textarea", "title")

        def handle_data(self, data: str) -> None:
            super().handle_data(html.unescape(data) if self._in_title else data)

    with _patched_globals(_CardMetadataParser=EscapableTitleParser):
        _check_equal(_card_metadata(head)[1], expected)


def test_external_fetches_use_only_default_ports():
    """Pages, redirects and og:image URLs are fetched on 80/443 only (security #9)."""
    asked: List[tuple[str, int]] = []
    fetched: List[str] = []

    def getaddrinfo(host: str, port: int, **_kwargs: Any) -> Any:
        asked.append((host, port))
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("93.184.216.34", port))]

    with _patched_attr(socket, "getaddrinfo", getaddrinfo):
        # A named port that is not the scheme's default is refused before DNS,
        # whatever service it names, and even with a warm per-card DNS cache.
        token = _RESOLVED_ADDRESSES.set({"example.com": ["93.184.216.34"]})
        try:
            for url in ("https://example.com:22/", "https://example.com:8443/x",
                        "https://example.com:80/", "http://example.com:443/",
                        "http://example.com:6379/", "http://example.com:25/",
                        "https://93.184.216.34:9200/", "https://[2001:db8::1]:22/",
                        "https://example.com:65535/", "https://example.com:1/"):
                with _check_raises(ValueError, f"{url} was fetchable", contains="port"):
                    _public_url_addresses(url)
        finally:
            _RESOLVED_ADDRESSES.reset(token)
        _check_equal(asked, [])
        # The default port, implicit or spelled out, is still fetched.
        for url, port in (("https://example.com/", 443), ("https://example.com:443/", 443),
                          ("http://example.com/", 80), ("HTTP://example.com:80/", 80)):
            asked.clear()
            _check_equal(_public_url_addresses(url)[1], ["93.184.216.34"])
            _check_equal(asked, [("example.com", port)])

    class FakeResponse:
        _content_consumed = True

        def __init__(self, status: int, headers: Dict[str, str], body: bytes = b""):
            self.status_code, self.headers, self._body = status, headers, body
            self.raw = io.BytesIO(body)

        def raise_for_status(self):
            return None

        def iter_content(self, _chunk_size: int):
            yield self._body

        def close(self):
            return None

    pages = {
        "/start": FakeResponse(302, {"Location": "https://example.com:25/mail"}),
        "/page": FakeResponse(200, {"Content-Type": "text/html"}, (
            b'<html><head><title>T</title>'
            b'<meta property="og:image" content="https://example.com:6379/x.png">'
            b'</head><body></body></html>'
        )),
    }

    class FakeSession:
        def __init__(self):
            self.headers: Dict[str, str] = {}
            self.trust_env = True

        def mount(self, _prefix: str, adapter: Any):
            self._adapter = adapter

        def get(self, url: str, **_kwargs: Any):
            fetched.append(url)
            return pages[urlparse(url).path]

        def close(self):
            self._adapter.close()

    with _patched_attr(socket, "getaddrinfo", getaddrinfo), _patched_globals(
        _NoRedirectSession=FakeSession
    ):
        # A redirect to another port is refused without connecting to it.
        asked.clear()
        with _check_raises(ValueError, "a redirect to port 25 was followed", contains="port 25"):
            _safe_download("https://example.com/start", 100)
        _check_equal(fetched, ["https://93.184.216.34/start"])
        _check_equal(asked, [("example.com", 443)])

        # An og:image on another port costs only the thumbnail.
        fetched.clear()
        with _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            embed = fetch_embed_url_card("https://pds.example", "token", "https://example.com/page")
        _check_equal(embed["external"]["title"], "T")
        _check("thumb" not in embed["external"])
        _check("port 6379" in stderr.getvalue())
        _check_equal(fetched, ["https://93.184.216.34/page"])

        # So does an --embed-url naming one: the card keeps just the URL.
        fetched.clear()
        with _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            embed = fetch_embed_url_card("https://pds.example", "token", "https://example.com:22/")
        _check_equal(embed["external"], {"uri": "https://example.com:22/", "title": "", "description": ""})
        _check("port 22" in stderr.getvalue())
        _check_equal(fetched, [])

    # Service URLs are the operator's choice and keep their port.
    _check_equal(normalize_service_url("https://pds.example:8443", service_name="PDS"), "https://pds.example:8443")


@contextmanager
def _local_tls_server(hostname: str) -> Iterator[Optional[tuple[int, str]]]:
    """Serve HTTP 200 over TLS on 127.0.0.1 with a throwaway cert for `hostname`.

    Yields (port, cert_path), or None when no openssl binary is available.
    """
    import shutil
    import ssl

    if shutil.which("openssl") is None:
        print("note: openssl not found; skipping local TLS checks", file=sys.stderr)
        yield None
        return
    with tempfile.TemporaryDirectory() as directory:
        cert, key = os.path.join(directory, "cert.pem"), os.path.join(directory, "key.pem")
        subprocess.run(
            ["openssl", "req", "-x509", "-newkey", "ec",
             "-pkeyopt", "ec_paramgen_curve:prime256v1", "-nodes", "-days", "2",
             "-subj", f"/CN={hostname}", "-addext", f"subjectAltName=DNS:{hostname}",
             "-keyout", key, "-out", cert],
            check=True, capture_output=True,
        )
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(cert, key)
        listener = socket.create_server(("127.0.0.1", 0))
        listener.settimeout(0.2)
        stop = threading.Event()

        def serve() -> None:
            while not stop.is_set():
                try:
                    raw, _ = listener.accept()
                except OSError:
                    continue
                try:
                    raw.settimeout(5)
                    with server_context.wrap_socket(raw, server_side=True) as conn:
                        conn.recv(65536)
                        conn.sendall(
                            b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                            b"Connection: close\r\n\r\nok"
                        )
                except (OSError, ssl.SSLError):
                    raw.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        try:
            yield listener.getsockname()[1], cert
        finally:
            stop.set()
            thread.join(5)
            listener.close()


def test_pinned_adapter_keeps_tls_state_private():
    import requests.adapters as requests_adapters

    shared = requests_adapters._preloaded_ssl_context
    first, second = _PinnedHTTPSAdapter("example.com"), _PinnedHTTPSAdapter("example.com")
    for adapter in (first, second):
        context = adapter.poolmanager.connection_pool_kw["ssl_context"]
        _check(context is not shared, "pinned adapter uses the process-wide context")
    _check(
        first.poolmanager.connection_pool_kw["ssl_context"]
        is not second.poolmanager.connection_pool_kw["ssl_context"],
        "pinned adapters share one SSLContext",
    )

    with _local_tls_server("example.com") as server:
        if server is None:
            return
        port, cert = server

        def fetch(adapter_host: str) -> requests.Response:
            session = _new_session()
            try:
                session.mount("https://", _PinnedHTTPSAdapter(adapter_host))
                return session.get(f"https://127.0.0.1:{port}/", timeout=5)
            finally:
                session.close()

        shared_check = None if shared is None else shared.check_hostname
        # Untrusted by the default bundle: verification is still on.
        with _check_raises(requests.exceptions.SSLError, "expected an untrusted cert to fail"):
            fetch("example.com")
        if shared is not None:
            # urllib3 clears check_hostname on the context it is handed.
            _check(shared.check_hostname, "process-wide context lost its hostname check")
        with _patched_globals(_requests_ca_bundle=lambda: cert):
            _check_equal(fetch("example.com").status_code, 200)
            # The pinned name is what the certificate is checked against.
            with _check_raises(requests.exceptions.SSLError, "expected a name mismatch to fail"):
                fetch("other.example")
        if shared is not None:
            _check_equal(shared.check_hostname, shared_check)


class _PinnedContextSession:
    """A Session stand-in that records the adapter mounted on it."""

    def __init__(self, behaviour: Callable[[], Any]):
        self.headers: Dict[str, str] = {}
        self.trust_env = True
        self.adapter: Optional[Any] = None
        self.behaviour = behaviour
        self.closed = False

    def mount(self, _prefix: str, adapter: Any) -> None:
        self.adapter = adapter

    def get(self, *_args: Any, **_kwargs: Any) -> Any:
        return self.behaviour()

    def close(self) -> None:
        self.closed = True


def _adapter_context(adapter: Any) -> Any:
    return adapter.poolmanager.connection_pool_kw["ssl_context"]


def test_pinned_ssl_context_pool_leases_one_adapter_at_a_time():
    pool = _PinnedSslContextPool(max_idle=2)
    with _patched_globals(_PINNED_SSL_CONTEXTS=pool):
        first = _PinnedHTTPSAdapter("example.com")
        second = _PinnedHTTPSAdapter("example.com")
        _check(_adapter_context(first) is not _adapter_context(second),
               "two live adapters share one SSLContext")
        _check_equal((first._pool_connections, first._pool_maxsize), (1, 1))
        context = _adapter_context(first)
        first.close()
        first.release()
        first.release()  # a second release must not hand the context out twice
        third = _PinnedHTTPSAdapter("example.com")
        _check(_adapter_context(third) is context, "a released context was not reused")
        fourth = _PinnedHTTPSAdapter("example.com")
        _check(_adapter_context(fourth) is not context, "a leased context was handed out twice")
        # Idle contexts are kept per CA bundle, up to the cap.
        for adapter in (second, third, fourth):
            adapter.release()
        _check_equal(len(pool._idle[_requests_ca_bundle()]), 2)
    other_bundle = _PinnedSslContextPool()
    other_bundle._idle[_requests_ca_bundle() + ".other"] = [context]
    with _patched_globals(_PINNED_SSL_CONTEXTS=other_bundle):
        other = _PinnedHTTPSAdapter("example.com")
    _check(_adapter_context(other) is not context, "a context crossed CA bundles")


def test_pinned_response_reuses_context_only_after_clean_close():
    ok = _FakeRawResponse(200, b"ok")
    sessions: List[_PinnedContextSession] = []

    def opener(*behaviours: Callable[[], Any]) -> Callable[[], _PinnedContextSession]:
        queued = list(behaviours)

        def new_session() -> _PinnedContextSession:
            sessions.append(_PinnedContextSession(queued.pop(0)))
            return sessions[-1]
        return new_session

    def refused() -> Any:
        raise requests.exceptions.ConnectionError("refused")

    release = threading.Event()

    def stalled() -> Any:
        release.wait(5)
        return ok

    pool = _PinnedSslContextPool()
    addresses = lambda url: (urlparse(url), ["93.184.216.34", "93.184.216.35"])
    with _patched_globals(_PINNED_SSL_CONTEXTS=pool, _public_url_addresses=addresses):
        # A refused first address and a clean finish: one context throughout.
        with _patched_globals(_new_session=opener(refused, lambda: ok)):
            with _open_pinned_response("https://example.com/", 5) as response:
                _check(response is ok, "the second address was not used")
        _check_equal([session.closed for session in sessions], [True, True])
        first, second = (_adapter_context(session.adapter) for session in sessions)
        _check(first is second, "a cleanly refused attempt did not pass its context on")
        _check_equal(pool._idle[_requests_ca_bundle()], [first])

        # An attempt abandoned at the deadline keeps its context out of the pool.
        sessions.clear()
        with _patched_globals(_new_session=opener(stalled)), \
                _network_deadline(0.2, subject="External download"):
            with _check_raises(DeadlineExceeded, "a stalled connect was not abandoned"):
                with _open_pinned_response("https://example.com/", 5):
                    pass
        abandoned = _adapter_context(sessions[0].adapter)
        _check(abandoned is first, "the idle context was not leased")
        release.set()
        finish_by = time.monotonic() + 5
        while not sessions[0].closed and time.monotonic() < finish_by:
            time.sleep(0.01)  # the worker finishes and disposes of its Session
        _check_equal(pool._idle.get(_requests_ca_bundle()), [])
        _check(sessions[0].closed, "the abandoned Session was never closed")

        # A body read abandoned inside the block: also kept out.
        sessions.clear()
        with _patched_globals(_new_session=opener(lambda: ok)):
            with _open_pinned_response("https://example.com/", 5):
                _ABANDONED_RESPONSE_CLEANUP.get().hand_off()
        _check(not sessions[0].closed, "an abandoned body's Session was closed")
        _check_equal(pool._idle.get(_requests_ca_bundle()), [])


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


def test_interleave_address_families():
    _check_equal(_interleave_address_families(["2001:db8::1", "2001:db8::2", "2001:db8::3",
                                               "2001:db8::4", "192.0.2.1", "192.0.2.2"]),
                 ["2001:db8::1", "192.0.2.1", "2001:db8::2", "192.0.2.2",
                  "2001:db8::3", "2001:db8::4"])
    _check_equal(_interleave_address_families(["192.0.2.1", "2001:db8::1", "2001:db8::2"]),
                 ["192.0.2.1", "2001:db8::1", "2001:db8::2"])
    _check_equal(_interleave_address_families(["192.0.2.1", "192.0.2.2"]), ["192.0.2.1", "192.0.2.2"])
    _check_equal(_interleave_address_families([]), [])
    # Resolution applies it: with four IPv6 answers ahead of IPv4, IPv4 would
    # otherwise never be among the attempts.
    v6 = ["2001:4860:4860::8888", "2001:4860:4860::8844", "2606:4700:4700::1111",
          "2606:4700:4700::1001"]
    answers = [(socket.AF_INET6, socket.SOCK_STREAM, 6, "", (a, 443, 0, 0)) for a in v6]
    answers.append((socket.AF_INET, socket.SOCK_STREAM, 6, "", ("8.8.8.8", 443)))
    with _patched_attr(socket, "getaddrinfo", lambda *_a, **_k: answers):
        _, addresses = _public_url_addresses("https://example.com/")
    _check_equal(addresses[:2], [v6[0], "8.8.8.8"])


def test_pinned_connect_timeout_leaves_time_for_other_addresses():
    """Every attempt but the last gets a short connect allowance; the last
    may use all the time that remains."""
    timeouts: List[Any] = []

    class FakeSession:
        def mount(self, *_args):
            pass

        def get(self, url, **kwargs):
            timeouts.append(kwargs["timeout"])
            raise requests.ConnectTimeout("no answer")

        def close(self):
            pass

    addresses = ["2001:db8::1", "192.0.2.1", "2001:db8::2"]
    with _patched_globals(
        _new_session=FakeSession,
        _public_url_addresses=lambda url: (urlparse(url), addresses),
    ), _check_raises(requests.RequestException, "expected every address to fail"):
        with _open_pinned_response("https://example.com/", 10):
            pass
    _check_equal(timeouts, [(DOWNLOAD_CONNECT_ATTEMPT_SECONDS, 10),
                            (DOWNLOAD_CONNECT_ATTEMPT_SECONDS, 10), 10])
    timeouts.clear()
    with _patched_globals(
        _new_session=FakeSession,
        _public_url_addresses=lambda url: (urlparse(url), addresses[:1]),
    ), _check_raises(requests.RequestException, "expected the address to fail"):
        with _open_pinned_response("https://example.com/", 10):
            pass
    _check_equal(timeouts, [10])
    # Never more than an even share of the remaining time: four addresses and
    # 10 s give 2.5 s each; with 2 s left for two, 1 s.
    for found, budget, expected in ((addresses + ["192.0.2.2"], 10, [2.5, 3, 3]),
                                    (addresses[:2], 2, [1.0])):
        timeouts.clear()
        with _patched_globals(
            _new_session=FakeSession,
            _public_url_addresses=lambda url, found=found: (urlparse(url), found),
        ), _check_raises(requests.RequestException, "expected every address to fail"):
            with _open_pinned_response("https://example.com/", budget):
                pass
        _check_equal([t[0] for t in timeouts[:-1]], expected)
        _check_equal(timeouts[-1], budget)


def test_blackholed_first_address_falls_back_in_time():
    """A first address that silently drops connection attempts costs only its
    connect allowance; the next address still serves the download."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_args):
            pass

        def do_GET(self):
            body = b"<head><title>second address</title></head>"
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    hole = socket.socket()
    fillers: List[socket.socket] = []
    try:
        try:
            hole.bind(("127.0.0.2", port))
        except OSError:
            print("note: no 127.0.0.2; skipping the black-hole check", file=sys.stderr)
            return
        hole.listen(0)
        # With its accept queue full, Linux drops further SYNs: a black hole.
        for _ in range(4):
            filler = socket.socket()
            filler.setblocking(False)
            try:
                filler.connect(("127.0.0.2", port))
            except BlockingIOError:
                pass
            fillers.append(filler)
        time.sleep(0.1)
        probe = socket.socket()
        probe.settimeout(0.5)
        try:
            probe.connect(("127.0.0.2", port))
        except socket.timeout:
            pass
        else:
            print("note: full backlog still accepts; skipping the black-hole check",
                  file=sys.stderr)
            return
        finally:
            probe.close()

        started = time.monotonic()
        with _patched_globals(
            DOWNLOAD_CONNECT_ATTEMPT_SECONDS=0.5,
            _public_url_addresses=lambda url: (urlparse(url), ["127.0.0.2", "127.0.0.1"]),
        ):
            body, _, _ = _safe_download(f"http://example.com:{port}/", 1000, timeout=5)
        elapsed = time.monotonic() - started
        _check_equal(body, b"<head><title>second address</title></head>")
        _check(0.4 < elapsed < 3, f"fallback took {elapsed:.2f}s")

        # With less time left than the connect allowance (after slow
        # redirects, say), the attempts still share it: the first may not
        # use it all up.
        started = time.monotonic()
        with _patched_globals(
            _public_url_addresses=lambda url: (urlparse(url), ["127.0.0.2", "127.0.0.1"]),
        ):
            body, _, _ = _safe_download(f"http://example.com:{port}/", 1000, timeout=1.5)
        elapsed = time.monotonic() - started
        _check_equal(body, b"<head><title>second address</title></head>")
        _check(0.6 < elapsed < 1.4, f"fallback with 1.5 s left took {elapsed:.2f}s")
    finally:
        for filler in fillers:
            filler.close()
        hole.close()
        server.shutdown()
        server.server_close()


def test_safe_download_follows_only_redirect_statuses():
    """Only 301/302/303/307/308 redirect; Location loses its surrounding OWS."""

    class FakeResponse:
        def __init__(self, status_code: int, headers: Dict[str, str]):
            self.status_code = status_code
            self.headers = headers
            self.body = b"<head><title>final</title></head>"

        def raise_for_status(self):
            if self.status_code >= 400:
                raise requests.HTTPError(f"HTTP {self.status_code}")

    def download(first: FakeResponse) -> tuple[List[str], Any]:
        hops: List[str] = []

        @contextmanager
        def fake_open(url: str, _timeout: float):
            hops.append(url)
            yield first if len(hops) == 1 else FakeResponse(200, {"Content-Type": "text/html"})

        with _patched_globals(
            _open_pinned_response=fake_open, _read_response_body=lambda resp, _n, **_k: resp.body
        ):
            try:
                return hops, _safe_download("https://example.com/start", 1000)
            except (ValueError, requests.RequestException) as exc:
                return hops, exc

    for status in (301, 302, 303, 307, 308):
        for location in ("https://example.com/next", "https://example.com/next ",
                         " https://example.com/next\t", "\t/next  "):
            hops, result = download(FakeResponse(status, {"Location": location}))
            _check_equal(hops, ["https://example.com/start", "https://example.com/next"])
            _check_equal(result[1], "https://example.com/next")
    # Not redirects: never followed, and reported by status, not as a redirect
    # missing its Location (nor read as if the 3xx body were the page).
    for status in (300, 304, 305, 306, 399):
        for headers in ({}, {"Location": "https://proxy.example/"}):
            hops, result = download(FakeResponse(status, headers))
            _check_equal(hops, ["https://example.com/start"])
            _check(isinstance(result, requests.RequestException), repr(result))
            _check(f"HTTP {status}" in str(result), str(result))
    hops, result = download(FakeResponse(302, {"Location": " \t "}))
    _check(isinstance(result, ValueError) and "missing Location" in str(result), repr(result))


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
        except XrpcError as raised:
            error = raised
            message = str(raised)
        else:
            raise AssertionError("expected a failed login to raise")

    _check("HTTP 401" in message, message)
    _check("AuthFactorTokenRequired" in message, message)
    _check("sign in code has been sent" in message, message)
    # The remedy matters more than the error name for this one. It is kept
    # apart, so it alone is printed on a line of its own.
    _check("app-passwords" in (error.hint or ""), str(error.hint))
    _check("\n" not in message, message)
    _check_equal(_exit_message(error).split("\n")[1:], [error.hint])
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


def test_webp_preflight_reads_chunk_sizes_unsigned():
    vp8x = b"VP8X" + (10).to_bytes(4, "little") + bytes(4) + (1).to_bytes(3, "little") * 2
    for size in (0x7FFFFFFF, 0x80000000, 0xFFFFFFF0, 0xFFFFFFFF):
        body = b"WEBP" + vp8x + b"JUNK" + size.to_bytes(4, "little") + bytes(16)
        webp = b"RIFF" + len(body).to_bytes(4, "little") + body
        with _check_raises(ValueError, f"chunk size {size:#x} was accepted",
                           contains="Truncated WebP chunk"):
            _preflight_webp_dimensions(webp)
    # A container of many small chunks is still walked to its end.
    body = b"WEBP" + vp8x + (b"JUNK" + bytes(4)) * 1000 + b"VP8X" + (10).to_bytes(4, "little") + bytes(10)
    with _check_raises(ValueError, "a late canvas header was missed", contains="misplaced"):
        _preflight_webp_dimensions(b"RIFF" + len(body).to_bytes(4, "little") + body)


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


def test_inspect_image_checks_png_idat_crc():
    """Decoding skips PNG data-chunk CRCs, so the verify() pass must still run."""
    buf = io.BytesIO()
    Image.new("RGB", (8, 8), "blue").save(buf, format="PNG")
    original = buf.getvalue()
    idat = original.index(b"IDAT")
    length = int.from_bytes(original[idat - 4:idat], "big")
    crc_end = idat + 4 + length + 4
    corrupted = original[:crc_end - 1] + bytes([original[crc_end - 1] ^ 0xFF]) + original[crc_end:]
    with Image.open(io.BytesIO(corrupted)) as img:
        img.load()  # the premise: a plain decode accepts the bad CRC
    with _check_raises(ValueError, "a PNG with a corrupt IDAT CRC was accepted"):
        inspect_image(corrupted, "bad-crc.png")


def test_inspect_image_opens_once_unless_the_plugin_verifies():
    images = {}
    for image_format in ("JPEG", "PNG", "GIF", "WEBP"):
        buf = io.BytesIO()
        Image.new("RGB", (4, 3), "red").save(buf, format=image_format)
        images[image_format] = buf.getvalue()
    original_open = Image.open
    opened: List[str] = []

    def counting_open(*args, **kwargs):
        opened.append(kwargs["formats"][0])
        return original_open(*args, **kwargs)

    with _patched_attr(Image, "open", counting_open):
        for image_format, data in images.items():
            _inspect_image_decoded(data)
    _check_equal(opened, ["JPEG", "PNG", "PNG", "GIF", "WEBP"])

    # A format whose plugin gains a real verify() gets the verify pass too.
    from PIL import JpegImagePlugin

    def failing_verify(self) -> None:
        raise SyntaxError("broken JPEG container")

    with _patched_attr(JpegImagePlugin.JpegImageFile, "verify", failing_verify):
        with _check_raises(SyntaxError, "a plugin's own verify() was skipped",
                           contains="broken JPEG container"):
            _inspect_image_decoded(images["JPEG"])


def _raw_gif(frames: int, canvas: tuple[int, int] = (1, 1)) -> bytes:
    """A GIF of `frames` 1x1 images on a `canvas`, each a few bytes long."""
    width, height = (value.to_bytes(2, "little") for value in canvas)
    frame = (b"\x21\xf9\x04\x00\x01\x00\x00\x00"           # graphic control
             b",\0\0\0\0\x01\x00\x01\x00\x00"                # 1x1 descriptor
             b"\x02\x02\x44\x01\x00")                         # LZW data
    return (b"GIF89a" + width + height + b"\x80\x00\x00\x00\x00\x00\xff\xff\xff"
            + frame * frames + b";")


def test_gif_image_count_matches_pillow():
    import random

    def pillow_frames(data: bytes) -> Optional[int]:
        try:
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                with Image.open(io.BytesIO(data), formats=["GIF"]) as img:
                    return img.n_frames
        except Exception:
            return None

    rng = random.Random(10)
    seeds = [_raw_gif(3)]
    for count, options in ((1, {}), (2, {"comment": b"note"}), (5, {"loop": 0}),
                           (7, {"optimize": False, "disposal": 2})):
        frames = [Image.new("RGB", (rng.randint(1, 9), rng.randint(1, 9)),
                            tuple(rng.randrange(256) for _ in range(3))) for _ in range(count)]
        buf = io.BytesIO()
        frames[0].save(buf, format="GIF", save_all=True, append_images=frames[1:],
                       duration=10, **options)
        seeds.append(buf.getvalue())
    for data in seeds:
        count = pillow_frames(data)
        _check(_gif_image_count_exceeds(data, count - 1), f"{count} frames not seen")
        _check(not _gif_image_count_exceeds(data, count), f"more than {count} frames seen")
    # Damaged files: the walk may give up early, but never counts more images
    # than Pillow does, so it can only refuse what Pillow would also refuse.
    for _ in range(1500):
        data = bytearray(rng.choice(seeds))
        for _ in range(rng.randint(1, 3)):
            if len(data) <= 6:
                break
            position = rng.randrange(6, len(data))
            choice = rng.random()
            if choice < 0.5:
                data[position] = rng.randrange(256)
            elif choice < 0.75:
                del data[position:]
            else:
                data[position:position] = data[rng.randrange(len(data)):][:rng.randint(1, 30)]
        count = pillow_frames(bytes(data))
        if count is not None:
            _check(not _gif_image_count_exceeds(bytes(data), count), data.hex())
    _check(not _gif_image_count_exceeds(b"GIF89a", 0), "a bare header has images")


def test_many_frame_gif_refused_before_decoding():
    data = _raw_gif(50_000)
    _check(_gif_image_count_exceeds(data, MAX_IMAGE_FRAMES), "50,000 frames not seen")
    _check(not _gif_image_count_exceeds(_raw_gif(MAX_IMAGE_FRAMES), MAX_IMAGE_FRAMES),
           "a GIF at the frame limit was counted over it")

    def no_worker(images: Any) -> Any:
        raise AssertionError("the decoder worker was started")

    with _patched_globals(_inspect_images_in_worker=no_worker):
        with _check_raises(ValueError, "a 50,000-frame GIF reached the worker",
                           contains="100-frame limit"):
            inspect_images([(data, "many.gif")])


def test_oversized_animation_refused_before_any_frame_decodes():
    webp = io.BytesIO()
    frames = [Image.new("RGB", (4, 3), color) for color in ("red", "blue", "green")]
    frames[0].save(webp, format="WEBP", save_all=True, append_images=frames[1:], lossless=True)
    # Three 1x1 frames on a 4x3 canvas: Pillow decodes each frame onto the canvas.
    animations = {"WEBP": webp.getvalue(), "GIF": _raw_gif(3, canvas=(4, 3))}
    original_load = ImageFile.ImageFile.load
    loads: List[str] = []

    def counting_load(self: Any) -> Any:
        loads.append(self.format)
        return original_load(self)

    with _patched_attr(ImageFile.ImageFile, "load", counting_load):
        for image_format, data in animations.items():
            with _patched_globals(MAX_IMAGE_DECODED_PIXELS=4 * 3 * 3):
                _check_equal(_inspect_image_decoded(data)["width"], 4)
            loads.clear()
            with _patched_globals(MAX_IMAGE_DECODED_PIXELS=4 * 3 * 3 - 1):
                with _check_raises(ValueError, f"oversized {image_format} animation accepted",
                                   contains="decoded-pixel limit"):
                    _inspect_image_decoded(data)
            _check_equal(loads, [])


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

    def fake_upload(_pds: str, _token: str, encoded: bytes, mimetype: str, **_kwargs) -> Dict:
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
        _image_worker_program=lambda: "import time; time.sleep(30)",
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


def test_thumb_warning_redacts_the_image_url():
    """A failed og:image never prints its query string, which may be a token."""

    def forbidden_upload(*_args, **_kwargs):
        raise AssertionError("an invalid thumbnail was uploaded")

    secret = "token=SECRET123&sig=abc"
    for img_url in (f"/img.png?{secret}", f"https://cdn.example/a/b.png?{secret}#frag"):
        downloaded: List[str] = []

        def fake_download(url: str, *_args, **_kwargs):
            downloaded.append(url)
            return b"not an image", url, "image/png"

        card: Dict = {}
        with _patched_globals(_safe_download=fake_download, upload_file=forbidden_upload), \
                _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            _attach_external_thumb(card, "https://pds.example", "test-token",
                                   "https://example.com/page", img_url)
        warning = stderr.getvalue()
        _check("thumb" not in card, "an invalid thumbnail was attached")
        _check("Invalid image" in warning, warning)
        _check("SECRET123" not in warning and "/a/b.png" not in warning, warning)
        # Only the label is redacted: the real URL is still what was fetched.
        _check_equal(len(downloaded), 1)
        _check(secret in downloaded[0], downloaded[0])


def test_image_worker_limits_never_loosen_or_break_on_macos():
    namespace: Dict[str, Any] = {}
    exec(_IMAGE_WORKER_LIMITS, namespace)
    apply_limits = namespace["_apply_worker_limits"]
    infinity = -1

    class FakeResource:
        RLIM_INFINITY = infinity
        RLIMIT_AS, RLIMIT_CPU, RLIMIT_FSIZE = "as", "cpu", "fsize"

        def __init__(self, current: Dict[str, tuple[int, int]], refuse: str = ""):
            self.current = current
            self.refuse = refuse
            self.installed: Dict[str, tuple[int, int]] = {}

        def getrlimit(self, kind: str) -> tuple[int, int]:
            return self.current[kind]

        def setrlimit(self, kind: str, limits: tuple[int, int]) -> None:
            if kind == self.refuse:
                raise ValueError("current limit exceeds maximum limit")
            self.installed[kind] = limits

    requested = (("RLIMIT_AS", 1000), ("RLIMIT_CPU", 60), ("RLIMIT_FSIZE", 500))
    # The lowest of requested, soft and hard wins; an inherited soft limit is
    # never raised, and unlimited values do not count.
    fake = FakeResource({
        "as": (infinity, infinity), "cpu": (7, infinity), "fsize": (800, 300),
    })
    apply_limits(fake, "linux", requested)
    _check_equal(fake.installed, {"as": (1000, 1000), "cpu": (7, 7), "fsize": (300, 300)})

    # macOS refuses a finite RLIMIT_AS (an unenforced RLIMIT_RSS alias): it is
    # skipped there, and the other limits are still installed.
    unlimited = {kind: (infinity, infinity) for kind in ("as", "cpu", "fsize")}
    fake = FakeResource(dict(unlimited), refuse="as")
    apply_limits(fake, "darwin", requested)
    _check_equal(fake.installed, {"cpu": (60, 60), "fsize": (500, 500)})
    # Anywhere else, a limit that cannot be installed stops the worker.
    for platform, refused in (("linux", "as"), ("darwin", "cpu")):
        with _check_raises(ValueError, f"expected a refused {refused} limit to fail on {platform}"):
            apply_limits(FakeResource(dict(unlimited), refuse=refused), platform, requested)


def test_image_worker_program_is_self_contained():
    """The worker runs only its own definitions, and they need nothing else."""
    import builtins
    import dis

    body = _image_worker_body()
    module = compile(_image_worker_program(), "<image worker>", "exec")
    defined = set(dir(builtins))
    for instruction in dis.get_instructions(module):
        if instruction.opname in ("STORE_NAME", "IMPORT_NAME", "IMPORT_FROM"):
            defined.add(instruction.argval.split(".")[0])

    def global_loads(code: Any) -> Iterator[str]:
        for instruction in dis.get_instructions(code):
            if instruction.opname in ("LOAD_GLOBAL", "LOAD_NAME"):
                yield instruction.argval
        for constant in code.co_consts:
            if hasattr(constant, "co_code"):
                yield from global_loads(constant)

    missing = sorted(set(global_loads(module)) - defined)
    _check_equal(missing, [])
    for name in _IMAGE_WORKER_FUNCTIONS:
        _check(f"def {name}(" in body, f"{name} missing from the worker")

    # Run it: only the decoder's imports are loaded, never the posting stack.
    png = io.BytesIO()
    Image.new("RGB", (2, 3)).save(png, format="PNG")
    probe = body.replace(
        "\n_image_worker_main()\n",
        f"\n_inspect_image_decoded({png.getvalue()!r})"
        "\nprint(json.dumps(sorted(sys.modules)))\n",
    )
    import_paths = [str(Path(entry).resolve()) for entry in sys.path if entry]
    completed = subprocess.run(
        [sys.executable, "-I", "-S", "-B", "-c", _image_worker_program(probe),
         str(MAX_IMAGE_WORKER_MEMORY_BYTES), "60", "1000000", json.dumps(import_paths)],
        capture_output=True, text=True, timeout=30, check=False,
    )
    _check_equal(completed.returncode, 0)
    modules = set(json.loads(completed.stdout))
    _check("PIL.Image" in modules, "the worker did not load Pillow")
    loaded = sorted(modules & {"requests", "urllib3", "ssl", "idna", "bs4", "html.parser",
                               "argparse", "concurrent.futures", "runpy"})
    _check_equal(loaded, [])


def test_image_worker_loads_only_the_accepted_format_plugins():
    """Decoding never falls back to Image.init(), which imports every plugin."""
    images = []
    for image_format in ("JPEG", "PNG", "GIF", "WEBP"):
        buf = io.BytesIO()
        Image.new("RGB", (4, 3), "red").save(buf, format=image_format)
        images.append(buf.getvalue())
    probe = _image_worker_body().replace(
        "\n_image_worker_main()\n",
        f"\nfor data in {images!r}:\n"
        "    _inspect_image_decoded(data)\n"
        "from PIL import Image\n"
        "print(json.dumps([Image._initialized, sorted(sys.modules)]))\n",
    )
    import_paths = [str(Path(entry).resolve()) for entry in sys.path if entry]
    completed = subprocess.run(
        [sys.executable, "-I", "-S", "-B", "-c", _image_worker_program(probe),
         str(MAX_IMAGE_WORKER_MEMORY_BYTES), "60", "1000000", json.dumps(import_paths)],
        capture_output=True, text=True, timeout=30, check=False,
    )
    _check_equal(completed.returncode, 0)
    initialized, modules = json.loads(completed.stdout)
    _check(initialized < 2, "the worker ran Image.init()")
    plugins = {name for name in modules if name.startswith("PIL.") and name.endswith("ImagePlugin")}
    _check("PIL.WebPImagePlugin" in plugins, "the WebP plugin was not loaded")
    # Image.preinit() registers BMP and PPM too; nothing beyond that is loaded.
    unexpected = sorted(plugins - set(IMAGE_FORMAT_PLUGINS.values())
                        - {"PIL.BmpImagePlugin", "PIL.PpmImagePlugin"})
    _check_equal(unexpected, [])


def test_inspect_image_decoded_refuses_unregistered_format_without_init():
    buf = io.BytesIO()
    Image.new("RGB", (4, 3)).save(buf, format="WEBP")

    def no_init() -> bool:
        raise AssertionError("Image.init() was called")

    registered = {name: entry for name, entry in Image.OPEN.items() if name != "WEBP"}
    with _patched_attr(Image, "OPEN", registered), _patched_attr(Image, "init", no_init), \
            _patched_attr(importlib, "import_module", lambda name: None):
        with _check_raises(ValueError, "an unregistered format was decoded",
                           contains="cannot decode WEBP"):
            _inspect_image_decoded(buf.getvalue())
    _check_equal(_inspect_image_decoded(buf.getvalue())["mimetype"], "image/webp")


def test_script_import_skips_single_use_modules():
    """A text-only post or a usage error never loads these."""
    probe = (
        "import json, sys; sys.path.insert(0, sys.argv[1]); import create_bsky_post\n"
        "print(json.dumps(sorted(m for m in ('PIL', 'subprocess', 'secrets',"
        " 'concurrent.futures') if m in sys.modules)))\n"
    )
    completed = subprocess.run(
        [sys.executable, "-I", "-c", probe, str(Path(bsky.__file__).resolve().parent)],
        capture_output=True, text=True, timeout=60, check=False,
    )
    _check_equal(completed.returncode, 0)
    _check_equal(json.loads(completed.stdout), [])


def test_image_batch_frames_round_trip_and_stream():
    images = [b"a" * 5, b"", b"bc", b"d" * 300]
    payload = _encode_image_batch(images)
    _check_equal(payload, b"".join(len(i).to_bytes(4, "big") + i for i in images))
    _check_equal(list(_read_image_batch(io.BytesIO(payload))), images)

    frame = lambda size: size.to_bytes(4, "big")
    for bad, message in (
        (b"", "Empty image batch"),
        (b"\x00\x00\x00", "Malformed"),
        (frame(MAX_IMAGE_SIZE_BYTES + 1), "Malformed"),
        (frame(10) + b"short", "Malformed"),
        (_encode_image_batch([b"x"] * (MAX_IMAGES_PER_POST + 1)), "Malformed"),
        (_encode_image_batch([b"x"]) + b"\x00", "Malformed"),
    ):
        with _check_raises(ValueError, f"accepted {bad[:12]!r}", contains=message):
            list(_read_image_batch(io.BytesIO(bad)))

    # Read lazily, and each image is released before the next is read.
    freed: List[int] = []

    class Tracked(bytes):
        def __del__(self) -> None:
            freed.append(len(self))

    class Stream:
        def __init__(self, data: bytes):
            self.data, self.position, self.freed_at_reads = data, 0, []

        def read(self, size: int) -> bytes:
            chunk = self.data[self.position:self.position + size]
            self.position += len(chunk)
            if size > 4:
                self.freed_at_reads.append(len(freed))
                return Tracked(chunk)
            return chunk

    # An oversized frame is refused from its header, before any body is read.
    stream = Stream(frame(MAX_IMAGE_SIZE_BYTES + 1) + bytes(MAX_IMAGE_SIZE_BYTES + 1))
    with _check_raises(ValueError, "accepted an oversized frame", contains="Malformed"):
        next(_read_image_batch(stream))
    _check_equal(stream.position, 4)

    stream = Stream(_encode_image_batch([b"1" * 10, b"2" * 20, b"3" * 30]))
    reader = _read_image_batch(stream)
    first = next(reader)
    _check_equal((first, stream.position), (b"1" * 10, 14))
    del first
    rest: List[bytes] = []
    for image in reader:  # as the worker does: each one dropped before the next
        rest.append(bytes(image))
        del image
    _check_equal(rest, [b"2" * 20, b"3" * 30])
    _check_equal(stream.freed_at_reads, [0, 1, 2])

    # The worker stops at the first bad image without reading on.
    stream = Stream(_encode_image_batch([b"not an image", b"x" * 1000]))
    out = io.BytesIO()
    with _patched_attr(sys, "stdin", types.SimpleNamespace(buffer=stream)), \
            _patched_attr(sys, "stdout", types.SimpleNamespace(buffer=out)):
        _image_worker_main()
    reply = json.loads(out.getvalue())
    _check_equal(len(reply["results"]), 1)
    _check("error" in reply["results"][0], repr(reply))
    _check_equal(stream.position, 4 + len(b"not an image"))
    # And it lets go of each decoded image before reading the next.
    png = io.BytesIO()
    Image.new("RGB", (2, 3)).save(png, format="PNG")
    stream = Stream(_encode_image_batch([png.getvalue()] * 2))
    out = io.BytesIO()
    with _patched_attr(sys, "stdin", types.SimpleNamespace(buffer=stream)), \
            _patched_attr(sys, "stdout", types.SimpleNamespace(buffer=out)):
        _image_worker_main()
    _check_equal([sorted(r) for r in json.loads(out.getvalue())["results"]], [["image"], ["image"]])
    _check_equal(stream.freed_at_reads[-2:], [len(freed) - 2, len(freed) - 1])


def test_image_worker_keeps_stricter_inherited_limits():
    import resource

    probe = (
        "import resource\n"
        "print(json.dumps({n: resource.getrlimit(getattr(resource, n))\n"
        "                  for n in ('RLIMIT_CPU', 'RLIMIT_FSIZE', 'RLIMIT_CORE')}))\n"
    )

    def stricter_soft_limits() -> None:
        for kind, soft in ((resource.RLIMIT_CPU, 7), (resource.RLIMIT_FSIZE, 4096)):
            _, hard = resource.getrlimit(kind)
            resource.setrlimit(kind, (soft, hard))
        # Many shells already start with no core dumps (soft 0); allow them,
        # so it is the worker itself that is seen to switch them off.
        _, hard = resource.getrlimit(resource.RLIMIT_CORE)
        resource.setrlimit(resource.RLIMIT_CORE, (hard, hard))

    import_paths = [str(Path(entry).resolve()) for entry in sys.path if entry]
    completed = subprocess.run(
        [sys.executable, "-I", "-S", "-B", "-c", _image_worker_program(probe),
         str(MAX_IMAGE_WORKER_MEMORY_BYTES), "60", "1000000", json.dumps(import_paths)],
        preexec_fn=stricter_soft_limits, capture_output=True, text=True,
        timeout=30, check=False,
    )
    _check_equal(completed.returncode, 0)
    # No core dump either, whatever the caller allowed (security #10).
    _check_equal(json.loads(completed.stdout), {
        "RLIMIT_CPU": [7, 7], "RLIMIT_FSIZE": [4096, 4096], "RLIMIT_CORE": [0, 0],
    })


def test_deeply_nested_worker_reply_is_refused():
    """A worker reply nested past the recursion limit fails like bad JSON."""
    buf = io.BytesIO()
    Image.new("RGB", (2, 2)).save(buf, format="PNG")
    encoded = buf.getvalue()
    # Python 3.11 and 3.12 stop at the 1000-frame recursion limit, 3.13 at
    # about 10,000 levels; the brackets need not close for either, so the
    # reply cap alone leaves room for this.
    nested = [prefix + b"[" * (MAX_IMAGE_WORKER_REPLY_BYTES - len(prefix))
              for prefix in (b"", b'{"results": ', b'{"results": [{"image": {"width": ')]
    for reply in nested:
        _check(len(reply) <= MAX_IMAGE_WORKER_REPLY_BYTES, str(len(reply)))
        with _check_raises(RecursionError, "the reply does not nest past the recursion limit"):
            json.loads(reply)
    # Plain malformed replies still fail the same way.
    for reply in nested + [b"", b"not json", b"\xff\xfe{}", b'{"results": [1, 2']:

        def worker_replying(*_args, stdout, **_kwargs):
            stdout.write(reply)
            return type("Completed", (), {"returncode": 0})()

        def forbidden_upload(*_args, **_kwargs):
            raise AssertionError("an unvalidated thumbnail was uploaded")

        def fake_download(*_args, **_kwargs):
            return encoded, "https://cdn.example/thumb.png", "image/png"

        with _patched_attr(subprocess, "run", worker_replying):
            with _check_raises(ValueError, "a nested reply escaped validation",
                               contains="Image decoder returned a non-JSON response"):
                inspect_image(encoded, "test.png")
            card: Dict = {}
            with _patched_globals(_safe_download=fake_download, upload_file=forbidden_upload), \
                    _patched_attr(sys, "stderr", io.StringIO()) as stderr:
                _attach_external_thumb(card, "https://pds.example", "test-token",
                                       "https://example.com/", "https://cdn.example/thumb.png")
        _check("thumb" not in card)
        _check("posting the card without it" in stderr.getvalue(), stderr.getvalue())


def _gif_with_growing_second_frame(width: int, height: int) -> bytes:
    """A 1x1 GIF whose second frame (disposal 2) grows the canvas in seek()."""
    frame = lambda w, h: (  # noqa: E731
        b"\x21\xf9\x04\x08\x00\x00\x00\x00"  # disposal method 2
        + b"\x2c" + struct.pack("<HHHH", 0, 0, w, h) + b"\x80"
        + b"\x00\x00\x00\xff\xff\xff" + b"\x02\x02\x44\x01\x00"
    )
    return (b"GIF89a" + struct.pack("<HH", 1, 1) + b"\x00\x00\x00"
            + frame(1, 1) + frame(width, height) + b"\x3b")


def test_gif_frame_canvas_is_bounded_by_the_pixel_limit():
    """Pillow's bomb check uses this script's limit, so seek() cannot grow a
    canvas past it before the dimension checks run (security #10)."""
    bomb = _gif_with_growing_second_frame(16385, 4000)
    _check(len(bomb) < 100)
    # Under Pillow's default limit, seek(1) does grow the canvas this far.
    with Image.open(io.BytesIO(bomb)) as img, \
            _patched_attr(Image, "MAX_IMAGE_PIXELS", 89_478_485):
        _check_equal(img.size, (1, 1))
        img.seek(1)
        _check_equal(img.size, (16385, 4000))
        _check(16385 * 4000 > MAX_IMAGE_PIXELS)
    # In the worker, the same seek is refused, and nothing near the canvas's
    # 250 MiB is ever allocated.
    probe = _image_worker_body().replace(
        "\n_image_worker_main()\n",
        "\nimport resource\n"
        "before = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss\n"
        "try:\n"
        f"    _inspect_image_decoded({bomb!r})\n"
        "    outcome = 'accepted'\n"
        "except Exception as exc:\n"
        "    outcome = _worker_error_text(exc)\n"
        "grown = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss - before\n"
        "print(json.dumps([outcome, grown]))\n",
    )
    import_paths = [str(Path(entry).resolve()) for entry in sys.path if entry]
    completed = subprocess.run(
        [sys.executable, "-I", "-S", "-B", "-c", _image_worker_program(probe),
         str(MAX_IMAGE_WORKER_MEMORY_BYTES), "60", "1000000", json.dumps(import_paths)],
        capture_output=True, text=True, timeout=30, check=False,
    )
    _check(completed.returncode == 0, completed.stderr)
    outcome, grown_kib = json.loads(completed.stdout)
    _check(outcome.startswith("DecompressionBomb"), outcome)
    _check(grown_kib < 32 * 1024, f"the refused GIF grew the worker by {grown_kib} KiB")
    # And end to end it is just an invalid image.
    with _check_raises(ValueError, "the GIF bomb was accepted", contains="Invalid image"):
        inspect_image(bomb, "bomb.gif")
    # A frame at the limit itself still seeks under the same settings.
    with Image.open(io.BytesIO(_gif_with_growing_second_frame(8000, 5000))) as img, \
            _patched_attr(Image, "MAX_IMAGE_PIXELS", MAX_IMAGE_PIXELS), \
            warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        img.seek(1)
        _check_equal(img.width * img.height, MAX_IMAGE_PIXELS)


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


@contextmanager
def _without_os_attrs(*names: str) -> Iterator[None]:
    """Hide os attributes, to exercise the code path of platforms without them."""
    saved = {name: getattr(os, name) for name in names if hasattr(os, name)}
    for name in saved:
        delattr(os, name)
    try:
        yield
    finally:
        for name, value in saved.items():
            setattr(os, name, value)


def test_read_image_file_reads_in_blocking_mode():
    """O_NONBLOCK guards open() against FIFOs only; the read itself blocks."""
    if not hasattr(os, "O_NONBLOCK"):
        return
    modes: List[bool] = []
    real_fdopen = os.fdopen

    def spying_fdopen(fd, *args, **kwargs):
        modes.append(os.get_blocking(fd))
        return real_fdopen(fd, *args, **kwargs)

    with tempfile.TemporaryDirectory() as directory:
        target = Path(directory) / "real.png"
        Image.new("RGB", (1, 1)).save(target, format="PNG")
        with _patched_attr(os, "fdopen", spying_fdopen):
            data = _read_image_file(target)
        _check_equal(data, target.read_bytes())
    _check_equal(modes, [True])


def test_read_image_file_refuses_special_files_before_open_without_nofollow():
    """Without O_NOFOLLOW/O_NONBLOCK, a FIFO must be refused before open()."""
    if not hasattr(os, "mkfifo"):
        return
    real_open = os.open
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        fifo = root / "pipe.png"
        os.mkfifo(fifo)
        target = root / "real.png"
        Image.new("RGB", (1, 1)).save(target, format="PNG")

        def guarded_open(path, *args, **kwargs):
            # Opening a FIFO with no writer and no O_NONBLOCK would hang.
            if Path(path) == fifo:
                raise AssertionError("open() reached a FIFO")
            return real_open(path, *args, **kwargs)

        with _without_os_attrs("O_NOFOLLOW", "O_NONBLOCK"), _patched_attr(
            os, "open", guarded_open
        ):
            with _check_raises(ValueError, "expected a FIFO to be refused",
                               contains="not a regular file"):
                _read_image_file(fifo)
            _check_equal(_read_image_file(target), target.read_bytes())
            link = root / "link.png"
            try:
                link.symlink_to(target)
            except (OSError, NotImplementedError):
                return
            with _check_raises(ValueError, "expected a symlink to be refused",
                               contains="must not be a symbolic link"):
                _read_image_file(link)


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
    with _check_raises(ValueError, "expected a negative length to fail", contains="negative"):
        _read_response_body(FakeResponse([b"ab"], "-1"), 4)
    # A malformed length declares nothing; the streamed cap still applies.
    _check_equal(_read_response_body(FakeResponse([b"ab"], "2x"), 4), b"ab")


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
        for encoding in ("", "identity", "gzip", "GZIP", "x-gzip", "deflate"):
            _check_equal(_read_response_body(FakeResponse(encoding), 16), b"decoded")
        # urllib3 picks its decoder from the lower-cased but unstripped value,
        # so a padded coding would get no decoder: refuse it rather than pass
        # compressed bytes on as if they were decoded.
        for encoding in ("gzip ", " deflate ", "x-gzip\t", "identity "):
            with _check_raises(ValueError, f"expected padded coding {encoding!r} to fail"):
                _read_response_body(FakeResponse(encoding), 16)

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
    soup = (
        '<meta PROPERTY=" OG:Title " content="Cased title">'
        '<meta name="Description" content="Cased description">'
        '<meta property="og:image" content="/thumb.png">'
    )
    _check_equal(_meta_content(soup, "og:title"), "Cased title")
    _check_equal(_meta_content(soup, "description"), "Cased description")
    _check_equal(_meta_content(soup, "og:image"), "/thumb.png")
    _check_equal(_meta_content(soup, "og:video"), "")
    # A blank duplicate never hides a real value later in the head, and a
    # property= match still outranks a name= match.
    soup = (
        '<meta property="og:title" content="">'
        '<meta property="og:title">'
        '<meta property="og:title" content=" \n ">'
        '<meta name="og:title" content="Named title">'
        '<meta property="og:title" content="Real Title">'
        '<meta property="og:image" content="">'
        '<meta name="og:image" content=" https://example.com/i.png ">'
    )
    _check_equal(_meta_content(soup, "og:title"), "Real Title")
    _check_equal(_meta_content(soup, "og:image"), "https://example.com/i.png")


def test_language_tags():
    for tag in ("en", "en-US", "zh-Hant-TW", "sr-Latn-RS", "de-CH-1901", "qaa",
                "en-a-bbb-x-yyy", "x-private", "i-klingon", "I-KLINGON"):
        _check(_is_valid_language_tag(tag), f"expected {tag!r} to be valid")
    for tag in ("", "e", "en-", "-en", "en--US", "x", "en_US", "toolonglang1", "én"):
        _check(not _is_valid_language_tag(tag), f"expected {tag!r} to be invalid")
    # Non-ASCII that lower() folds into ASCII (U+212A KELVIN SIGN -> "k") must
    # not pass as the ASCII tag it imitates, including grandfathered tags.
    for tag in ("en-\u212ar", "\u212ao", "x-\u212a", "i-\u212alingon", "en-US\u0130"):
        _check(not _is_valid_language_tag(tag), f"expected {tag!r} to be invalid")
        with _check_raises(ValueError, f"expected --lang {tag!r} to fail", contains="BCP 47"):
            _validate_language_args([tag])


def test_every_grandfathered_language_tag_is_accepted():
    # RFC 5646's 26: the irregular ones by table, the regular ones by parsing.
    regular = ("art-lojban", "cel-gaulish", "no-bok", "no-nyn", "zh-guoyu", "zh-hakka",
               "zh-min", "zh-min-nan", "zh-xiang")
    _check_equal(len(GRANDFATHERED_LANGUAGE_TAGS) + len(regular), 26)
    for tag in (*GRANDFATHERED_LANGUAGE_TAGS, *regular):
        for spelling in (tag, tag.upper()):
            _check(_is_valid_language_tag(spelling), spelling)
    _check(not set(regular) & GRANDFATHERED_LANGUAGE_TAGS)
    # Private use needs at least one subtag after "x", of 1 to 8 characters.
    for tag, valid in (("en-x", False), ("en-x-a", True), ("en-x-a-abcdefgh", True),
                       ("en-x-abcdefghi", False), ("x", False), ("x-a-b", True),
                       # A 4-character variant starts with a digit; 5 to 8 need not.
                       ("en-US-1996", True), ("en-US-abcd", False), ("en-US-abcde", True),
                       # An extension singleton needs subtags of 2 to 8 characters.
                       ("en-a-bb", True), ("en-a-b", False), ("en-a-bb-c", False),
                       ("en-a-bb-c-dd", True)):
        _check_equal((tag, _is_valid_language_tag(tag)), (tag, valid))


def test_title_text_is_capped():
    parser = _CardMetadataParser(CARD_META_KEYS)
    parser.feed("<head><title>   " + "t" * MAX_RAW_TITLE_CHARS + "</b>x" * 5_000 + "</title>")
    _check_equal(parser.title(), "t" * MAX_RAW_TITLE_CHARS)
    # As Python 3.14 may hand it over: in pieces, split at every character
    # reference. Pieces past the cap are dropped, not kept as empty strings.
    parser = _CardMetadataParser(CARD_META_KEYS)
    parser.feed("<head><title>")
    for _ in range(MAX_RAW_TITLE_CHARS + 50_000):
        parser.handle_data("&")
    _check_equal(len(parser._title), MAX_RAW_TITLE_CHARS)
    _check_equal(parser.title(), "&" * MAX_RAW_TITLE_CHARS)


def test_charset_label_with_a_nul_is_skipped():
    """codecs.lookup raises ValueError, not LookupError, on an embedded NUL."""
    _check(_html_charset_codec("utf-8\x00") is None, "a NUL label named a codec")
    _check(not _is_utf16_or_utf32_label("utf-16\x00"), "a NUL label counted as UTF-16")
    for head, content_type in (
        ('<meta charset="koi8-r\x00"><title>Té</title>'.encode(), None),
        ('<?xml encoding="koi8-r\x00"?><title>Té</title>'.encode(), None),
        ("<title>Té</title>".encode(), 'text/html; charset="koi8-r\x00"'),
    ):
        # The label is ignored, so the head decodes as the UTF-8 it is.
        _check_equal(_decode_html_head(head, content_type)[-len("Té</title>"):], "Té</title>")


def test_meta_contents_matches_the_per_key_rules_in_one_pass():
    """One walk over the <meta> tags gives what a per-key search would."""
    import random

    def per_key(soup: Any, key: str) -> str:
        # The rule, spelled out one key at a time: first non-blank, property=
        # matches before name= matches, keys case-insensitive, values stripped.
        for attribute in ("property", "name"):
            for tag in soup.find_all("meta"):
                value, content = tag.get(attribute), tag.get("content")
                if (isinstance(value, str) and value.strip().lower() == key
                        and isinstance(content, str) and content.strip()):
                    return content.strip()
        return ""

    rng = random.Random(1234)
    spellings = ["og:title", "OG:Title", " og:title ", "og:description", "description",
                 "Description", "og:image", "twitter:title", "og:titles", ""]
    contents = ["", " ", "\n", "A", " B ", "C\n", "https://example.com/i.png"]
    for _ in range(400):
        tags = []
        for _ in range(rng.randrange(0, 8)):
            attributes = []
            for attribute in ("property", "name"):
                if rng.random() < 0.6:
                    attributes.append(f'{attribute}="{rng.choice(spellings)}"')
            if rng.random() < 0.9:
                attributes.append(f'content="{rng.choice(contents)}"')
            rng.shuffle(attributes)
            tags.append("<meta " + " ".join(attributes) + ">")
        head = "<head>" + "".join(tags) + "</head>"
        if BeautifulSoup is None:
            print("note: bs4 not installed; skipping the <meta> reference comparison",
                  file=sys.stderr)
            break
        # BeautifulSoup's tree, searched one key at a time, is the reference.
        reference = BeautifulSoup(head, "html.parser")
        expected = {key: per_key(reference, key) for key in CARD_META_KEYS}
        _check_equal(_meta_contents(head, CARD_META_KEYS), expected)
        _check_equal({key: _meta_content(head, key) for key in CARD_META_KEYS}, expected)
    # Of a repeated attribute the first counts, as in browsers.
    _check_equal(_meta_content("<meta property=og:title content=First content=Second>", "og:title"),
                 "First")


def test_card_parse_streams_without_building_a_tree():
    """Junk markup is tokenized and forgotten; nothing grows with the head."""
    import tracemalloc

    page = (b"<html><head><title>An <span>article</span> title</title>"
            + b"<a><b>junk</b></a>" * 500
            + b"<noscript><meta property='og:title' content='Nested OG'></noscript>"
            + b"<div><meta name='description' content='Nested desc'></div>"
            + b"<meta property='og:image' content=' /i.png '></head><body><p>x</p>")
    card, image = _parse_embed_metadata("https://example.com/", page, "text/html")
    _check_equal((card["title"], card["description"], image), ("Nested OG", "Nested desc", "/i.png"))
    # <title> is raw text with character references, as browsers read it:
    # markup inside it is part of the title, not elements.
    page = b"<head><title> An <span>article</span> &amp; &#x263A; title</title>" + b"<a>x</a>" * 50 + b"</head>"
    card, _ = _parse_embed_metadata("https://example.com/", page, "text/html")
    _check_equal(card["title"], "An <span>article</span> & \u263a title")
    # Neither a title full of tags nor one huge <meta> costs memory to match.
    for head in (b"<title>" + b"<a>" * 300_000, b"<meta " + b"a=b " * 100_000 + b">"):
        tracemalloc.start()
        try:
            started = tracemalloc.get_traced_memory()[0]
            _parse_embed_metadata("https://example.com/", head, "text/html")
            peak = tracemalloc.get_traced_memory()[1] - started
        finally:
            tracemalloc.stop()
        _check(peak < 40 * len(head), f"{head[:12]!r}: {peak / 2**20:.0f} MiB peak for {len(head)} bytes")
    # Only the first <title> counts, <title/> included, and its text is capped.
    _check_equal(_card_metadata("<title/><title>Later</title>")[1], "")
    _check_equal(_card_metadata("<title>First</title><title>Later</title>")[1], "First")
    _check_equal(_card_metadata("<title>   \n" + " " * 50_000 + "Late start")[1], "Late start")
    _check_equal(len(_card_metadata("<title>" + "a" * 1_000_000)[1]), MAX_RAW_TITLE_CHARS)


def test_oversized_start_tag_ends_the_head():
    import html.parser
    import tracemalloc

    _check_equal(MAX_PARSED_START_TAG_CHARS, 64 * 1024)
    # On the html.parser this was written against, the start-tag scan is
    # html.parser's own regex match, attribute by attribute, on any markup.
    import random

    _check(EXACT_START_TAG_SCAN or sys.version_info >= (3, 14), "html.parser's start-tag regex changed")
    rng = random.Random(8)
    pieces = ["a", "=", " ", '"', "'", ">", "/", "x=1", ' b=">"', " c = '>'", "\n", "<", "&amp;",
              "\t", "==", "/>", "\x00", "é", "a b"]
    plain = html.parser.HTMLParser()
    ours = _HeadBoundaryParser()
    for _ in range(4000):
        text = ("<" + rng.choice(["m", "meta", 'a"b', "x/", "A"])
                + "".join(rng.choice(pieces) for _ in range(rng.randint(0, 60))) * rng.choice((1, 2, 7))
                + rng.choice(("", ">", "/>", " x", "'", '"', "/")))
        plain.rawdata = ours.rawdata = text
        _check_equal((text[:60], ours.check_for_whole_start_tag(0)), (text[:60], plain.check_for_whole_start_tag(0)))
        if EXACT_START_TAG_SCAN:
            _check_equal(_start_tag_match_end(text, 0),
                         html.parser.locatestarttagend_tolerant.match(text, 0).end())
    # And it gives up on a tag once it is past the limit.
    huge = "<meta " + "a=b " * 250_000 + ">"
    started = time.monotonic()
    _check(_start_tag_match_end(huge, 0, MAX_PARSED_START_TAG_CHARS) - 1 > MAX_PARSED_START_TAG_CHARS,
           "the limit was not reported")
    _check(time.monotonic() - started < 0.5, "the scan did not stop at the limit")
    matched = [0]

    class CountingAttribute:
        def match(self, *args: Any) -> Any:
            matched[0] += 1
            return _START_TAG_ATTRIBUTE.match(*args)

    ours.rawdata = huge
    with _patched_globals(_START_TAG_ATTRIBUTE=CountingAttribute()):
        _check(ours.check_for_whole_start_tag(0) > MAX_PARSED_START_TAG_CHARS, "an over-long tag was not reported")
    # Each "a=b " is four characters: no more were matched than fit the limit.
    _check(matched[0] <= MAX_PARSED_START_TAG_CHARS // 4 + 2, f"{matched[0]} attributes scanned")
    before = b"<head><title>T</title><meta property='og:description' content='kept'>"
    # Long but plausible: still read in full.
    fits = b"<meta property='og:title' content='" + b"x" * (MAX_PARSED_START_TAG_CHARS - 100) + b"'>"
    card, _ = _parse_embed_metadata("https://example.com/", before + fits + b"</head>", "text/html")
    _check_equal(card["title"], "x" * MAX_EXTERNAL_TITLE_CHARS)
    # A tag longer than any real one ends the head where it starts: what
    # follows (here a spoofed title, then the body) is never read.
    for huge in (b"<meta " + b"a=b " * 50_000 + b">",
                 b"<meta " + b'a=">" ' * 30_000 + b">",
                 b'<meta a = ">" ' + b"a=b " * 50_000 + b">",
                 b"<meta content='" + b"y" * 100_000 + b"'>"):
        page = (before + huge + b"<meta property='og:title' content='spoof'></head>"
                + b"<body><meta property='og:title' content='body'>")
        _check_equal(_html_head_bytes(page), before)
        response = _ChunkedHtmlResponse([page[i:i + 4096] for i in range(0, len(page), 4096)])
        tracemalloc.start()
        try:
            head = _read_response_body(response, MAX_EMBED_HEAD_BYTES, html_head_only=True)
            card, _ = _parse_embed_metadata("https://example.com/", page, "text/html")
            peak = tracemalloc.get_traced_memory()[1]
        finally:
            tracemalloc.stop()
        _check_equal(head, before)
        _check_equal((card["title"], card["description"]), ("T", "kept"))
        _check(peak < 32 * 2**20, f"{peak / 2**20:.0f} MiB peak for a {len(huge)}-byte tag")


def test_start_tag_window_fallback_for_other_html_parsers():
    """Where html.parser's regex is not the known one, its own check runs over
    a bounded window: right for real tags, and a tag past the limit ends the head."""
    import html.parser

    plain = html.parser.HTMLParser()
    with _patched_globals(EXACT_START_TAG_SCAN=False):
        ours = _HeadBoundaryParser()
        for text in ("<a>", "<meta charset=utf-8>", "<a b='x>y' c=\"d\"/>", "<a b=", "<a b", "<a/",
                     "<img src=x alt='" + "y" * 60_000 + "'>"):
            plain.rawdata = ours.rawdata = text
            _check_equal((text[:30], ours.check_for_whole_start_tag(0)),
                         (text[:30], plain.check_for_whole_start_tag(0)))
        before = b"<head><title>T</title>"
        import tracemalloc

        for huge in (b"<meta " + b"a=b " * 50_000 + b">", b"<meta content='" + b"y" * 100_000 + b"'>"):
            page = before + huge + b"<meta property='og:title' content='spoof'></head>"
            tracemalloc.start()
            try:
                _check_equal(_html_head_bytes(page), before)
                card, _ = _parse_embed_metadata("https://example.com/", page, "text/html")
                peak = tracemalloc.get_traced_memory()[1]
            finally:
                tracemalloc.stop()
            _check_equal(card["title"], "T")
            _check(peak < 32 * 2**20, f"window fallback peaked at {peak / 2**20:.0f} MiB")


def test_decode_html_head_order_and_fallthrough():
    cafe = "<title>Café “x”</title>"
    latin = cafe.replace("“x”", "x").encode("latin-1")
    for page, content_type, expected in (
        # A byte-order mark beats everything, and is dropped.
        (b"\xef\xbb\xbf" + cafe.encode(), "text/html; charset=iso-8859-1", cafe),
        (b"\xff\xfe" + cafe.encode("utf-16-le"), "text/html; charset=iso-8859-1", cafe),
        (b"\xfe\xff" + cafe.encode("utf-16-be"), None, cafe),
        # Then the header, then <meta charset>, then UTF-8.
        (cafe.encode("cp1252"), "text/html; charset=windows-1252", cafe),
        (b"<meta charset=windows-1252>" + cafe.encode("cp1252"), "text/html",
         "<meta charset=windows-1252>" + cafe),
        (cafe.encode(), "text/html", cafe),
        (b"<meta charset=x-bogus>" + cafe.encode(), "text/html; charset=x-bogus",
         "<meta charset=x-bogus>" + cafe),
        # Only the header can say CP437 (a guess would read \x82 as "‚").
        (b"<title>caf\x82</title>", "text/html; charset=cp437", "<title>café</title>"),
        # A UTF-16 label inside the document is ignored, as browsers do, even
        # where even-length ASCII would "decode" as UTF-16 without error.
        (b"<meta charset=utf-16le><title>Cafe</title>", None,
         "<meta charset=utf-16le><title>Cafe</title>"),
        (b"<meta charset=utf-16>" + cafe.encode(), None, "<meta charset=utf-16>" + cafe),
        # A UTF-16 page without a byte-order mark is still recognised.
        (("<title>Café crème brûlée</title>" * 20).encode("utf-16-be"), None,
         "<title>Café crème brûlée</title>" * 20),
        # A character cut off by the 1 MB cap is dropped, not fatal.
        (cafe.encode() + "é".encode()[:1], "text/html; charset=utf-8", cafe),
    ):
        _check_equal(_decode_html_head(page, content_type), expected)
    # A wrong label falls through to the guess rather than producing
    # replacement characters (which single-byte charset is the detector's call).
    decoded = _decode_html_head(latin, "text/html; charset=utf-8")
    _check("\ufffd" not in decoded, decoded)
    _check_single_byte_decode(latin, decoded)
    # Undecodable by every candidate: windows-1252 with replacements.
    _check_equal(_decode_html_head(b"a\x81b", "text/html; charset=utf-8"), "a\ufffdb")


def test_decode_html_head_keeps_a_right_label_despite_stray_bytes():
    title = "Café — 東京"
    dirty = (b'<head><meta property="og:title" content="' + title.encode()
             + b'"><!-- \x92 --></head>')
    for page, content_type in (
        (b"\xef\xbb\xbf" + dirty, "text/html; charset=utf-8"),
        (b"\xef\xbb\xbf" + dirty, "text/html; charset=windows-1252"),  # the BOM decides
        (dirty, "text/html; charset=utf-8"),
        (b'<meta charset="utf-8">' + dirty, "text/html"),
        (dirty, "text/html"),
    ):
        card, _ = _parse_embed_metadata("https://example.com/", page, content_type)
        _check_equal((content_type, card["title"]), (content_type, title))
    # Even where windows-1252 would strictly "decode" the bytes.
    page = "<head><title>Café — crème</title>".encode() + b"<!-- \x92 --></head>"
    page.decode("cp1252")  # the premise: strict windows-1252 accepts these bytes
    card, _ = _parse_embed_metadata("https://example.com/", page, "text/html; charset=utf-8")
    _check_equal(card["title"], "Café — crème")
    # Any declared charset, not only UTF-8.
    page = "<head><title>日本語のタイトル</title>".encode("shift_jis") + b"<!-- \xa0\xff --></head>"
    card, _ = _parse_embed_metadata("https://example.com/", page, "text/html; charset=shift_jis")
    _check_equal(card["title"], "日本語のタイトル")
    # A label that is wrong for the whole page still falls through to the guess.
    page = "<head><title>Café crème brûlée à la carte</title></head>".encode("latin-1")
    card, _ = _parse_embed_metadata("https://example.com/", page, "text/html; charset=utf-8")
    _check_equal(card["title"], "Café crème brûlée à la carte")

    # A UTF-16 <meta> label stays ignored when the guess gets its turn.
    page = b"<meta charset=utf-16le><title>caf\xe9 cr\xe8me</title>"
    _check_equal(len(page) % 2, 0)  # so UTF-16 would decode it without error
    _check_single_byte_decode(page, _decode_html_head(page, "text/html"))
    _check(_mostly_decodes("Café — 東\ufffd"), "one replacement among four was refused")
    _check(not _mostly_decodes("Caf\ufffd cr\ufffdme"), "all-replacement text was accepted")
    _check(not _mostly_decodes("caf\ufffd \u00e9"), "an even split was accepted")


def _check_single_byte_decode(page: bytes, text: str) -> None:
    """`text` read `page` one byte per character, its markup intact: not as
    UTF-16 or UTF-32. Which single-byte charset the guess picks for two
    accented words is the detector's call (chardet 5 and 7 differ)."""
    _check_equal(len(text), len(page))
    _check_equal([c for c in text if c.isascii()], [chr(b) for b in page if b < 0x80])


def test_meta_charset_is_found_anywhere_in_the_sniff_prefix():
    for encoding, text in (("windows-1250", "Zażółć gęślą jaźń"), ("windows-1257", "Ąčęėįšųūž"),
                           ("iso-8859-2", "Příliš žluťoučký kůň")):
        page = (b"<html><head><script>" + b"// x\n" * 1000 + b"</script><meta charset='"
                + encoding.encode() + b"'><title>" + text.encode(encoding) + b"</title></head>")
        _check(page.index(b"<meta") > 4096, "the <meta> is not past bs4's default window")
        card, _ = _parse_embed_metadata("https://example.com/", page, "text/html")
        _check_equal((encoding, card["title"]), (encoding, text))


def test_meta_charset_scan_is_linear_and_follows_the_tokenizer():
    for page, expected in (
        (b"<meta charset=utf-8>", "utf-8"),
        (b"<META CHARSET='Windows-1250'/>", "Windows-1250"),
        (b'<meta http-equiv="Content-Type" content="text/html; charset=koi8-r">', "koi8-r"),
        (b"<meta http-equiv=content-type content='charset = \"gbk\"'>", "gbk"),
        (b"<meta name=x><meta charset=' '><meta charset=latin1><meta charset=utf-8>", "latin1"),
        (b"<meta charset=big5 charset=utf-8>", "big5"),
        (b"<!-- <meta charset=utf-8> --><script><meta charset=gbk></script>", None),
        (b"<title><meta charset=gbk></title><meta charset=euc-kr>", "euc-kr"),
        (b"<meta http-equiv=refresh content='charset=gbk'>", None),
        (b"<head></head><meta charset=gbk>", None),
    ):
        _check_equal((page, _meta_charset(page)), (page, expected))

    # No declared-encoding regex runs at all: they backtrack on hostile
    # markup, inside the C regex engine, with the GIL held, past any deadline.
    searched: List[int] = []
    real = _xml_declared_encoding

    def recording(prefix: bytes) -> Optional[str]:
        searched.append(len(prefix))
        return real(prefix)

    hostile = {
        "xml declaration": (b"<?" + b'encoding="" ' * 6000)[:HTML_CHARSET_SNIFF_BYTES],
        "unclosed xml declaration": (b"<?" + b'encoding="x ' * 6000)[:HTML_CHARSET_SNIFF_BYTES],
        "meta run": (b"<meta " * 20000)[:HTML_CHARSET_SNIFF_BYTES],
        "lone <": b"<" * HTML_CHARSET_SNIFF_BYTES,
    }
    with _patched_globals(_xml_declared_encoding=recording):
        for name, prefix in hostile.items():
            started = time.monotonic()
            _decode_html_head(prefix + b"\xe9x" * 10, "text/html")
            elapsed = time.monotonic() - started
            _check(elapsed < 1.0, f"{name}: decoding took {elapsed:.2f}s")
    _check(searched, "the guess never ran")

    # A UTF-16 label in an XML declaration is ignored like one in <meta>.
    # (utf-16le: Python's plain "utf-16" refuses BOM-less input by itself.)
    page = b'<?xml version="1.0" encoding="utf-16le"?><title>caf\xe9 cr\xe8me</title>'
    _check_equal(len(page) % 2, 0)
    _check_single_byte_decode(page, _decode_html_head(page, "text/html"))


def test_charset_guess_survives_a_character_cut_by_the_prefix():
    for encoding, text in (("shift_jis", "日本語のタイトルテスト"), ("euc_jp", "日本語のタイトルテスト"),
                           ("gb18030", "简体中文的网页标题测试"), ("big5", "繁體中文的網頁標題測試")):
        page = b"<head><title>" + text.encode(encoding) * 4000 + b"</title></head>"
        prefix = page[:HTML_CHARSET_SNIFF_BYTES]
        # The premise: the sniff prefix ends mid-character.
        with _check_raises(UnicodeDecodeError, f"{encoding}: the prefix ends on a whole character"):
            prefix.decode(encoding)
        card, _ = _parse_embed_metadata("https://example.com/", page, "text/html")
        _check_equal((encoding, card["title"][:len(text)]), (encoding, text))


def test_decode_html_head_guesses_from_a_bounded_prefix():
    """No statistical detection ever runs over more than the sniff prefix."""
    guessed: List[int] = []
    real_detector = _charset_detector()

    class RecordingDetector:
        @staticmethod
        def detect(markup: bytes) -> Dict[str, Any]:
            guessed.append(len(markup))
            return real_detector.detect(markup)

    head = b"<title>" + ("Café crème brûlée. " * 30000).encode("latin-1")[:500_000] + b"</title>"
    with _patched_globals(_charset_detector=lambda: RecordingDetector):
        card, _ = _parse_embed_metadata("https://example.com/", head, "text/html")
        _check(card["title"].startswith("Café crème brûlée."), card["title"][:40])
        _check(guessed and max(guessed) <= HTML_CHARSET_SNIFF_BYTES, repr(guessed))
        # Valid UTF-8 needs no guess at all.
        guessed.clear()
        _decode_html_head(("Café " * 100_000).encode(), "text/html")
        _check_equal(guessed, [])


# BeautifulSoup's declared-encoding regex (bs4.dammit.xml_encoding), the
# reference _xml_declared_encoding must agree with.
_BS4_XML_ENCODING = re.compile(rb"^\s*<\?.*encoding=['\"](.*?)['\"].*\?>", re.I)


def _bs4_xml_declared_encoding(prefix: bytes) -> Optional[str]:
    match = _BS4_XML_ENCODING.search(prefix, endpos=1024)
    value = match.group(1).decode("ascii", "replace").lower() if match else ""
    return value or None


def test_xml_declared_encoding_matches_bs4_without_backtracking():
    for page, expected in (
        (b'<?xml version="1.0" encoding="ISO-8859-2"?><a/>', "iso-8859-2"),
        (b"  \n\t<?xml encoding='koi8-r' ?>", "koi8-r"),
        (b'<?xml ENCODING="Big5"?>', "big5"),
        (b'<?xml encoding="a" encoding="b"?>', "b"),
        (b'<?xml encoding="x"?> encoding="y"', "x"),
        (b'<?xml encoding=""?>', None),
        (b'<?xml encoding="utf-8"', None),
        (b'<?xml\nencoding="utf-8"?>', None),
        (b'x<?xml encoding="utf-8"?>', None),
        (b'<?xml encoding = "utf-8"?>', None),
        (b'<?xml encoding="ut\'f"?>', "ut"),
        (b" " * 1020 + b'<?xml encoding="utf-8"?>', None),
        (b"", None),
    ):
        _check_equal((page, _xml_declared_encoding(page)), (page, expected))
        _check_equal((page, _bs4_xml_declared_encoding(page)), (page, expected))

    import random

    generator = random.Random(9)
    pieces = [b"<?", b"?>", b"encoding=", b"ENCODING=", b"'", b'"', b"a", b"utf-8", b" ",
              b"\n", b"\t", b"=", b"?", b"<", b">", b"\xe9"]
    for _ in range(20_000):
        page = b"".join(generator.choice(pieces) for _ in range(generator.randrange(0, 16)))
        if generator.random() < 0.5:
            page = b"<?" + page
        _check_equal((page, _xml_declared_encoding(page)), (page, _bs4_xml_declared_encoding(page)))
    # Long inputs too, around the 1 KB window and the first line's end.
    for _ in range(300):
        page = (b" " * generator.randrange(0, 1030) + b"<?x " + b"a" * generator.randrange(0, 1030)
                + generator.choice([b'encoding="k"?>', b"\nencoding='k'?>", b'encoding="k" ?>']))
        _check_equal((len(page), _xml_declared_encoding(page)),
                     (len(page), _bs4_xml_declared_encoding(page)))
    # Linear where the regex backtracks: 1 KB of "encoding=" candidates.
    hostile = (b"<?" + b'encoding="x ' * 100)[:XML_DECLARATION_SNIFF_BYTES]
    started = time.monotonic()
    for _ in range(100):
        _xml_declared_encoding(hostile)
    _check(time.monotonic() - started < 1.0)


def test_link_cards_work_without_beautifulsoup():
    """The script never imports bs4, even when it has to guess a charset."""
    page = b'<?xml version="1.0" encoding="windows-1250"?><head><title>Za\xbf\xf3\xb3\xe6</title>'
    probe = (
        "import sys\n"
        "sys.modules['bs4'] = None  # any import of it now fails\n"
        f"sys.path.insert(0, {str(Path(bsky.__file__).resolve().parent)!r})\n"
        "import create_bsky_post as b\n"
        f"card, _ = b._parse_embed_metadata('https://example.com/', {page!r}, 'text/html')\n"
        # No declaration at all: only the statistical guess can decode this.
        "guessed = []\n"
        "real = b._charset_detector\n"
        "b._charset_detector = lambda: guessed.append(1) or real()\n"
        f"text = b._decode_html_head({b'<title>' + 'Привет мир'.encode('koi8-r') * 50!r}, None)\n"
        "print(card['title'], text[:7], guessed)\n"
    )
    completed = subprocess.run([sys.executable, "-c", probe], capture_output=True, text=True,
                               timeout=60, check=False)
    _check(completed.returncode == 0, completed.stderr[-2000:])
    _check_equal(completed.stdout.strip(), "Zażółć <title> [1]")


def test_low_confidence_charset_guesses_are_ignored():
    page = "<head><title>Café crème brûlée à la carte, déjà vu</title></head>".encode("cp1252")

    def detector(confidence: Optional[float]) -> Any:
        return types.SimpleNamespace(detect=lambda _prefix: {
            "encoding": "mac-cyrillic", "confidence": confidence})

    # chardet 7 reads this French page as MacCyrillic, at about 0.1.
    for confidence in (None, 0.0, 0.119, MIN_CHARSET_GUESS_CONFIDENCE - 0.001):
        with _patched_globals(_charset_detector=lambda c=confidence: detector(c)):
            _check_equal(_decode_html_head(page, None), page.decode("cp1252"))
    for confidence in (MIN_CHARSET_GUESS_CONFIDENCE, 0.99):
        with _patched_globals(_charset_detector=lambda c=confidence: detector(c)):
            _check_equal(_decode_html_head(page, None), page.decode("mac-cyrillic"))
    _check_equal(MIN_CHARSET_GUESS_CONFIDENCE, 0.2)


def test_header_charset_settles_the_decode_before_any_meta_scan():
    scanned: List[bytes] = []
    real = _meta_charset

    def recording(prefix: bytes) -> Optional[str]:
        scanned.append(prefix)
        return real(prefix)

    page = "<meta charset=utf-8><title>Привет</title>".encode("koi8-r")
    with _patched_globals(_meta_charset=recording):
        _check_equal(_decode_html_head(page, "text/html; charset=koi8-r"),
                     page.decode("koi8-r"))
        _check_equal(scanned, [])
        # A header label that does not decode still lets the <meta> decide.
        page = "<meta charset=koi8-r><title>Привет</title>".encode("koi8-r")
        _check_equal(_decode_html_head(page, "text/html; charset=utf-8"), page.decode("koi8-r"))
        _check_equal(len(scanned), 1)


def test_content_type_charset_parse_is_bounded():
    _check_equal(MAX_CONTENT_TYPE_CHARS, 1024)
    longest = "text/html; charset=koi8-r; x=" + "y" * (MAX_CONTENT_TYPE_CHARS - 29)
    _check_equal(len(longest), MAX_CONTENT_TYPE_CHARS)
    _check_equal(_charset_from_content_type(longest), "koi8-r")
    built: List[int] = []

    class RecordingMessage(_HeaderMessage):
        def __init__(self) -> None:
            built.append(1)
            super().__init__()

    hostile = 'text/html; a="' + ";" * 6_400_000  # 100 joined 64 KiB headers
    with _patched_globals(_HeaderMessage=RecordingMessage):
        started = time.monotonic()
        _check_equal(_charset_from_content_type(longest + "y"), None)
        _check_equal(_charset_from_content_type(hostile), None)
        _decode_html_head(b"<title>T\xc3\xa9</title>", hostile)
        _check(time.monotonic() - started < 1, "a long Content-Type was parsed")
    _check_equal(built, [])
    # The page still decodes by its other candidates.
    _check_equal(_decode_html_head(b"<title>T\xc3\xa9</title>", hostile), "<title>Té</title>")


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
    # Parameters are split as the header grammar does: a quoted value of
    # another parameter is opaque, and the first charset wins.
    for content_type, expected in (
        ('text/html; foo="x; charset=latin1"; charset=utf-8', "utf-8"),
        ('text/html; foo="a\\"; charset=latin1"; charset=utf-8', "utf-8"),
        ("text/html; charset=utf-8; charset=latin1", "utf-8"),
        ("text/html;CHARSET=Shift_JIS", "Shift_JIS"),
        ("text/html; charset='utf-8'", "utf-8"),
        ("text/html; charset*=utf-8''latin1", None),
        ('text/html; x="unterminated; charset=latin1', None),
    ):
        _check_equal(_charset_from_content_type(content_type), expected)


def test_byte_order_mark_outranks_the_declared_charset():
    """A BOM decides the encoding even when the server declares another."""
    declared = "text/html; charset=iso-8859-1"
    for bom, encoding in (
        (b"\xef\xbb\xbf", "utf-8"),
        (b"\xff\xfe", "utf-16-le"),
        (b"\xfe\xff", "utf-16-be"),
    ):
        page = bom + "<head><title>Café</title></head>".encode(encoding)
        card, _ = _parse_embed_metadata("https://example.com/", page, declared)
        _check_equal(card["title"], "Café")
    # Without a BOM the declared charset still decides, and a quoted
    # parameter cannot smuggle in a different one.
    page = "<head><title>Café</title></head>".encode("iso-8859-1")
    card, _ = _parse_embed_metadata("https://example.com/", page, declared)
    _check_equal(card["title"], "Café")
    page = "<head><title>Café</title></head>".encode()
    card, _ = _parse_embed_metadata(
        "https://example.com/", page, 'text/html; foo="x; charset=latin1"; charset=utf-8'
    )
    _check_equal(card["title"], "Café")


def test_ascii_hostname():
    _check_equal(_ascii_hostname("example.com"), "example.com")
    # Over-long internationalized names never reach idna.encode.
    with _patched_attr(idna, "encode", lambda *_a, **_k: (_ for _ in ()).throw(
        AssertionError("over-long hostname reached idna.encode")
    )), _check_raises(ValueError, "expected an over-long IDN to fail", contains="253"):
        _ascii_hostname("\u0660" * 254)
    # Only one trailing root dot is exempt from the cap: a run of dots must not
    # smuggle a huge string into idna.encode or the error message.
    never_encode = lambda *_a, **_k: (_ for _ in ()).throw(
        AssertionError("dot-padded hostname reached idna.encode")
    )
    for padded in ("\u00fc" + "." * 500_000, "\u00fc" * 253 + ".."):
        with _patched_attr(idna, "encode", never_encode), _check_raises(
            ValueError, "expected a dot-padded IDN to fail", contains="253"
        ):
            _ascii_hostname(padded)
    try:
        _ascii_hostname("\u00fc" + "." * 500_000)
    except ValueError as exc:
        _check(len(str(exc)) < 300, f"error message is {len(str(exc))} chars")
    # Exactly one trailing dot still keeps a 253-character name valid.
    _check_equal(_ascii_hostname("\u00fc."), "xn--tda.")
    boundary = "\u00fc" + ".a" * 126  # 253 characters before the root dot
    with _patched_attr(idna, "encode", lambda *_a, **_k: b"reached"):
        _check_equal(_ascii_hostname(boundary + "."), "reached")
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

    def metadata(*args):
        _check(in_budget[0], "metadata extraction escaped the parsing budget")
        return original_metadata(*args)

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
    # Raw text, as browsers show it: the markup is the title (trimmed).
    _check_equal(embed["external"]["title"], ("<span>" * 1200)[:MAX_EXTERNAL_TITLE_CHARS])

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

    def response_for(encoded: bytes, coding: str = "gzip") -> requests.Response:
        response = requests.Response()
        response.status_code = 200
        response.headers["Content-Encoding"] = coding
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

    # The real decoder ignores a padded coding and yields the raw gzip stream;
    # that must be refused, not returned as the decoded body.
    padded = response_for(gzip.compress(b"bounded decoded content"), "gzip ")
    try:
        with _patched_globals(_urllib3_has_bounded_decoding=lambda: True), _check_raises(
            ValueError, "expected a padded coding to fail", contains="undecodable"
        ):
            _read_response_body(padded, 1024)
    finally:
        padded.close()

    encoded = gzip.compress(b"A" * (32 * 1024 * 1024))
    response = response_for(encoded)
    # Start tracing after fixture creation. The old decoder allocated the
    # entire 32 MiB expansion before the 4 MB application limit could reject it.
    tracemalloc.start()
    try:
        with _check_raises(ValueError, "expected expanded body to exceed the cap"):
            _read_response_body(response, 4_000_000)
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
            return _parse_url(url), ["8.8.8.8"]

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


def _exit_message(exc: Exception) -> str:
    try:
        exit_for_error(exc)
    except SystemExit as stop:
        return str(stop)
    raise AssertionError("exit_for_error did not raise")


def _raised(exc_type: type[BaseException], call: Any, *args: Any) -> BaseException:
    try:
        call(*args)
    except exc_type as exc:
        return exc
    raise AssertionError(f"{getattr(call, '__name__', call)} did not raise {exc_type.__name__}")


def test_exit_for_error_starts_a_line_only_for_its_hint():
    """No text inside an error can print a line of its own (security #11)."""
    # A line break inside the message, wherever it came from, stays escaped.
    for forged in ("bad\nPassword: ", "bad\r\nPassword: ", "bad\u2028Password: ",
                   "bad\u2029Password: ", "bad\x85Password: "):
        message = _exit_message(ValueError(f"Invalid image 'x.png': OSError: {forged}"))
        _check_equal(len(message.splitlines()), 1)
        _check(message.startswith("Error: Invalid image"), message)
    # The remedy hint, this script's own text, is the only second line, and it
    # is escaped too.
    error = XrpcError("Login failed with HTTP 401.", response=_FakeJsonResponse(401, {}),
                      error_name="AuthenticationRequired", hint="Use an APP password\x1b[2J")
    _check_equal(_exit_message(error).split("\n"),
                 ["Error: Login failed with HTTP 401.", "Use an APP password\\x1b[2J"])
    _check_equal(_exit_message(XrpcError("Failed.", response=_FakeJsonResponse(400, {}),
                                         error_name=None)), "Error: Failed.")

    # A file name with a line break in it, end to end.
    with tempfile.TemporaryDirectory() as directory:
        path = os.path.join(directory, "x\nError: Enter your app password again:")
        message = _exit_message(_raised(ValueError, prepare_images, [path], []))
    _check_equal(len(message.splitlines()), 1)
    _check("\\x0aError: Enter your app password again:" in message, message)

    # A worker error carrying a line break and a bidi override, whether for
    # one image or the whole batch, is escaped and bounded before it is
    # raised, so the warning a thumbnail failure prints is safe too.
    buf = io.BytesIO()
    Image.new("RGB", (2, 2)).save(buf, format="PNG")
    hostile = "OSError: a\nPassword: \u202e" + "x" * 12_000
    for reply in ({"results": [{"error": hostile}]}, {"error": hostile}):
        def worker_replying(*_args, stdout, reply=reply, **_kwargs):
            stdout.write(json.dumps(reply).encode())
            return type("Completed", (), {"returncode": 0})()

        with _patched_attr(subprocess, "run", worker_replying):
            text = str(_raised(ValueError, inspect_image, buf.getvalue(), "x.png"))
        _check("\n" not in text and "\u202e" not in text, text[:200])
        _check("\\x0aPassword" in text and "\\u202e" in text, text[:200])
        _check(len(text) < MAX_WORKER_ERROR_CHARS + 100, str(len(text)))


def test_missing_self_tests_message_is_escaped():
    with tempfile.TemporaryDirectory() as directory:
        script = os.path.join(directory, "a\nError: fake\u202e", "create_bsky_post.py")
        with _patched_attr(bsky, "__file__", script):
            try:
                bsky.run_self_tests()
            except SystemExit as stop:
                message = str(stop)
            else:
                raise AssertionError("missing self-tests did not exit")
    _check(message.startswith("Self-tests not found:"), message)
    _check(len(message.splitlines()) == 1 and "\u202e" not in message, message)


def test_parse_uri_never_echoes_credentials_or_unbounded_text():
    """URI errors are redacted and bounded (security #11)."""
    for uri in ("https://alice:s3cr3t_pass@bsky.app/profile/a.example/post/abc",
                "https://:s3cr3t_pass@bsky.app/profile/a.example/post/abc",
                "https://bsky.app:8443/profile/s3cr3t_pass/post/abc",
                "https://bsky.app:s3cr3t_pass/profile/a.example/post/abc",
                "https://bsky.app/profile/a.example/post/abc/s3cr3t_pass",
                "https://bsky.app/profile/caf\u00e9s3cr3t_pass/post/abc",
                "https://bsky.app/profile/a.example/post/abc;s3cr3t_pass",
                "https://bsky.app/profile/a.example/s3cr3t_pass/abc",
                "https://bsky.app/profile/a.example/post/s3cr3t_pass!",
                "https://bsky.app/profile/a.example/post/s3cr3t_pass" + "x" * 600,
                "gopher://bsky.app/s3cr3t_pass", "mailto:s3cr3t_pass@example.com",
                "https://[s3cr3t_pass/x"):
        message = str(_raised(ValueError, parse_uri, uri))
        _check("s3cr3t" not in message, message)
    # Too long to be a URI at all: refused without quoting any of it, and
    # likewise for a reply root taken from someone else's post.
    for uri in ("https://bsky.app/" + "\u202e" * 10 + "x" * MAX_URL_LENGTH,
                "at://did:plc:" + "a" * MAX_URL_LENGTH + "/app.bsky.feed.post/abc",
                "gopher://" + "y" * 8 * 1024 * 1024):
        message = str(_raised(ValueError, parse_uri, uri))
        _check_equal(message, f"URI exceeds {MAX_URL_LENGTH} characters")
        root = {"root": {"uri": uri, "cid": _TEST_CID}}
        message = str(_raised(ValueError, _reply_root_ref, "https://records.example", root))
        _check(len(message) < 100, message[:200])
    # The bsky.app errors still say what was wrong, just not with the text.
    _check_equal(
        str(_raised(ValueError, parse_uri, "https://bsky.app/profile/a.example/likes/abc")),
        "Unsupported Bluesky record path (expected post, lists or feed): "
        "https://bsky.app/…",
    )
    _check_equal(
        str(_raised(ValueError, parse_uri, "https://bsky.app/profile/a.example/post/a!b")),
        "Invalid record key in Bluesky URL: https://bsky.app/…",
    )
    # An AT URI is all identifiers and is still quoted in full.
    _check_equal(
        str(_raised(ValueError, parse_uri, "at://did:plc:abc/app.bsky.feed.post/a!b")),
        "Invalid record key in AT URI: 'a!b'",
    )
    # Below the cap, an unhandled URI is shown only as scheme and host.
    _check_equal(str(_raised(ValueError, parse_uri, "gopher://example.com/" + "y" * 8000)),
                 "Unhandled URI format: gopher://example.com/…")
    # A URI exactly at the cap is not refused for its length.
    at_cap = "https://bsky.app/profile/a.example/post/" + "a" * (MAX_URL_LENGTH - 40)
    _check_equal(len(at_cap), MAX_URL_LENGTH)
    _check("exceeds" not in str(_raised(ValueError, parse_uri, at_cap)))
    # AT URIs, public ASCII identifiers, are still quoted so the mistake shows.
    _check("at://did:plc:abc/x" in str(_raised(ValueError, parse_uri, "at://did:plc:abc/x")))
    _check_equal(parse_uri("https://bsky.app/profile/a.example/post/abc")["rkey"], "abc")


def test_terminal_safe_escapes_bidi_controls_and_line_separators():
    """Remote text can neither reorder what is shown nor break the line."""
    unsafe = ["\u061c", "\u200e", "\u200f", "\u2028", "\u2029",
              *map(chr, range(0x202A, 0x202F)), *map(chr, range(0x2066, 0x206A))]
    for character in unsafe:
        _check_equal(_terminal_safe(f"a{character}b"), f"a\\u{ord(character):04x}b")
    rendered = _terminal_safe("".join(unsafe) + "\x1b\x9b")
    _check(not any(character in rendered for character in unsafe + ["\x1b", "\x9b"]))
    # Printable text in any script, joiners included, is left alone.
    for text in ("\u0645\u0631\u062d\u0628\u0627", "\u05e9\u05dc\u05d5\u05dd",
                 "\U0001f469\u200d\U0001f4bb", "caf\u00e9 \u65e5\u672c\u8a9e"):
        _check_equal(_terminal_safe(text), text)
    # Escaping is idempotent, so text escaped twice reads the same.
    _check_equal(_terminal_safe(rendered), rendered)


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

    def publish(
        create: Callable, lookup: Callable, *, rejected_retry: bool = False, did: str = "did:plc:me"
    ) -> str:
        calls = [0]

        def create_record(*args, **kwargs):
            calls[0] += 1
            return (rejected if rejected_retry and calls[0] == 2 else create)(*args, **kwargs)

        with _patched_attr(sys, "stderr", io.StringIO()) as stderr, _patched_globals(
            _create_post_record=create_record, _existing_post=lookup
        ), _check_raises(requests.RequestException, "expected the failure to propagate"):
            _publish_post("https://pds.example", "token", did, {}, verbose=False)
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
    # The URI is escaped before it reaches the terminal, whatever the DID holds.
    output = publish(timed_out, lookup_fails, did="did:plc:\x1b[2Jme")
    _check("may still have been created" in output, output)
    _check("\x1b" not in output, repr(output))


def test_login_session_requires_a_token_and_a_did():
    """A blank token or a non-DID repo from createSession is never used."""

    def login(body: Dict) -> Dict:
        response = _FakeJsonResponse(200, body)
        with _patched_api_session(response):
            return bsky_login_session("https://pds.example", "alice", "secret")

    good = {"accessJwt": "eyJh.eyJz.c2ln", "did": "did:plc:abc123", "refreshJwt": "r"}
    _check_equal(login(good), good)
    _check_equal(login(dict(good, did="did:web:example.com%3A8080"))["did"], "did:web:example.com%3A8080")
    for token in (None, 7, "", " ", "tok en", "tok\r\nX-Evil: 1", "tok\x00", "t\u00f6k"):
        with _check_raises(ValueError, f"expected accessJwt {token!r} to fail", contains="accessJwt"):
            login(dict(good, accessJwt=token))
    for did in (None, "", "alice.bsky.social", "did:plc:", "DID:plc:abc", "did:plc:abc\n",
                "\x1b[2Jdid:plc:abc", "did:plc:ab c", "did:plc:abc:"):
        try:
            login(dict(good, did=did))
        except ValueError as exc:
            _check("did" in str(exc), str(exc))
            _check("\x1b" not in str(exc) and "abc" not in str(exc), str(exc))
        else:
            raise AssertionError(f"expected did {did!r} to fail")
    # The credential is never echoed in the refusal.
    try:
        login(dict(good, accessJwt="secret token"))
    except ValueError as exc:
        _check("secret" not in str(exc), str(exc))
    else:
        raise AssertionError("expected a spaced token to fail")


def test_did_check_is_bounded_and_unchanged():
    import random

    # The pattern before possessive quantifiers, as the reference language.
    reference = re.compile(
        r"^did:[a-z0-9]+:(?:[A-Za-z0-9._-]|%[0-9A-Fa-f]{2})+"
        r"(?::(?:[A-Za-z0-9._-]|%[0-9A-Fa-f]{2})+)*$"
    )
    rng = random.Random(16)
    valid = ["a", "Z", "0", ".", "_", "-", "%4a", "%2F", ":"]
    pieces = valid * 6 + ["%", "%4", "%G1", "::", "!", "\n", "é", " "]
    accepted = 0
    for _ in range(20_000):
        value = (rng.choice(["did:", "did:", "DID:", "did", ""])
                 + rng.choice(["plc", "web", "x9", "", "Plc", "p-c"]) + rng.choice([":", ":", ""])
                 + "".join(rng.choice(pieces) for _ in range(rng.randint(0, 6))))
        expected = reference.fullmatch(value) is not None
        _check_equal((value, DID_REGEX.fullmatch(value) is not None), (value, expected))
        accepted += expected
    _check(accepted > 800, f"only {accepted} valid DIDs generated")

    # The specification's length limit (2 KB) applies before any matching.
    _check_equal(MAX_DID_LENGTH, 2048)
    longest = "did:plc:" + "a" * (MAX_DID_LENGTH - len("did:plc:"))
    _check(_is_did(longest), "a DID at the length limit was refused")
    matched: List[int] = []

    class Spy:
        def fullmatch(self, value: str) -> Any:
            matched.append(len(value))
            return DID_REGEX.fullmatch(value)

    with _patched_globals(DID_REGEX=Spy()):
        for value in (longest + "a", "did:plc:" + "a" * 8_000_000 + "!"):
            _check(not _is_did(value), "an over-long DID was accepted")
    _check_equal(matched, [])
    _check(not _is_did(None) and not _is_did(b"did:plc:abc"), "a non-string DID passed")

    # Every place a DID arrives from outside goes through the bounded check.
    def login(did: str) -> Dict:
        body = {"accessJwt": "eyJh.eyJz.c2ln", "did": did, "refreshJwt": "r"}
        with _patched_api_session(_FakeJsonResponse(200, body)):
            return bsky_login_session("https://pds.example", "alice", "secret")

    def resolve(did: str) -> Optional[str]:
        with _patched_api_session(_FakeJsonResponse(200, {"did": did})):
            return _resolve_handle("https://pds.example", "alice.example.com")

    _check_equal(login(longest)["did"], longest)
    _check_equal(resolve(longest), longest)
    _validate_at_identifier(longest)
    with _check_raises(ValueError, "an over-long login DID was accepted", contains="did"):
        login(longest + "a")
    _check_equal(resolve(longest + "a"), None)
    with _check_raises(ValueError, "an over-long DID identifier was accepted"):
        _validate_at_identifier(longest + "a")


def test_link_card_resolves_each_host_once():
    answers = {"example.com": "93.184.216.34", "cdn.example.net": "151.101.1.69"}
    asked: List[tuple[str, int]] = []

    def getaddrinfo(host: str, port: int, **_kwargs: Any) -> Any:
        asked.append((host, port))
        return [(socket.AF_INET, socket.SOCK_STREAM, 6, "", (answers[host], port))]

    with _patched_attr(socket, "getaddrinfo", getaddrinfo):
        # Without a card in progress, every call resolves.
        _public_url_addresses("https://example.com/a")
        _public_url_addresses("https://example.com/b")
        _check_equal(len(asked), 2)
        asked.clear()
        token = _RESOLVED_ADDRESSES.set({})
        try:
            for url in ("http://example.com/", "https://example.com/", "https://example.com/r1",
                        "https://example.com/og.png", "https://example.com:443/",
                        "https://cdn.example.net/og.png", "https://EXAMPLE.com/again"):
                _, addresses = _public_url_addresses(url)
                addresses.append("mutated by a caller")
            # Within one card, a host is never asked twice nor answers anew.
            answers["example.com"] = "10.0.0.1"
            _check_equal(_public_url_addresses("https://example.com/late")[1], ["93.184.216.34"])
        finally:
            _RESOLVED_ADDRESSES.reset(token)
        # The port plays no part: an http:// to https:// redirect asks once.
        _check_equal(asked, [("example.com", 80), ("cdn.example.net", 443)])
        # The next card resolves afresh, and validates what it gets.
        with _check_raises(ValueError, "a private answer was accepted", contains="non-public"):
            _public_url_addresses("https://example.com/")

    seen: List[Any] = []
    with _patched_globals(_fetch_embed_url_card=lambda *_a: seen.append(_RESOLVED_ADDRESSES.get())):
        fetch_embed_url_card("https://pds.example", "token", "https://example.com/")
        fetch_embed_url_card("https://pds.example", "token", "https://example.com/")
    _check(seen[0] == {} and seen[0] is not seen[1], "cards shared one address cache")
    _check(_RESOLVED_ADDRESSES.get() is None, "the address cache outlived its card")


def test_error_body_text_is_kept_only_for_verbose():
    def failing(verbose: bool) -> tuple[str, str]:
        response = _FakeRawResponse(502, b"<html>" + b"x" * 1000 + b"</html>")
        with _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            try:
                _raise_for_xrpc(response, "createRecord", verbose=verbose)
            except requests.HTTPError as exc:
                return str(exc), stderr.getvalue()
        raise AssertionError("a 502 was not raised")

    message, printed = failing(False)
    _check_equal((message, printed), ("createRecord failed with HTTP 502.", ""))
    response = _FakeRawResponse(502, b"<html>boom</html>")
    _check_equal(_error_response_body(response), None)
    response = _FakeRawResponse(502, b"<html>boom</html>")
    _check_equal(_error_response_body(response, keep_raw=True), {"raw": "<html>boom</html>"})
    message, printed = failing(True)
    _check_equal(message, "createRecord failed with HTTP 502.")
    _check(json.loads(printed)["raw"].startswith("<html>xxx"), printed[:80])
    # A message is stripped once and dropped when blank.
    _check_equal(_api_error_summary({"error": "E", "message": "  why \n"}), " (E: why)")
    _check_equal(_api_error_summary({"error": "E", "message": " \t "}), " (E)")
    _check_equal(_api_error_summary({"error": "E", "message": 7}), " (E)")


def test_url_trim_shortcut_matches_the_full_scan():
    import random

    def full_scan(url: str) -> str:  # the trim without its early return
        depth = dict.fromkeys(URL_CLOSING_TO_OPENING.values(), 0)
        unmatched = set()
        for index, character in enumerate(url):
            if character in depth:
                depth[character] += 1
            elif (opening := URL_CLOSING_TO_OPENING.get(character)) is not None:
                if depth[opening]:
                    depth[opening] -= 1
                else:
                    unmatched.add(index)
        end = len(url)
        while end and (_is_trailing_url_punctuation(url[end - 1]) or end - 1 in unmatched):
            end -= 1
        return url[:end]

    rng = random.Random(18)
    alphabet = list("ab/.,;:!?()[]{}'\"") + ["\u201d", "\u00bb", "\u300d", "\u2019", "é"]
    for _ in range(20_000):
        url = "https://e.com/" + "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 8)))
        _check_equal((url, _trim_url_match(url)), (url, full_scan(url)))
    _check_equal(_trim_url_match(""), "")
    tag = "tag" + "!" * 5000
    _check_equal(_strip_tag_trailing_punctuation(tag), "tag")
    _check_equal(_strip_tag_trailing_punctuation("!?"), "")


def test_session_tokens_are_validated_and_never_echoed():
    def login(**fields: Any) -> Dict:
        body = dict({"accessJwt": "eyJh.eyJz.c2ln", "did": "did:plc:abc123", "refreshJwt": "r.e.f"}, **fields)
        body = {key: value for key, value in body.items() if value is not ...}
        with _patched_api_session(_FakeJsonResponse(200, body)):
            return bsky_login_session("https://pds.example", "alice", "secret")

    _check_equal(MAX_TOKEN_CHARS, 16 * 1024)
    longest = "t" * MAX_TOKEN_CHARS
    _check_equal(login(accessJwt=longest, refreshJwt=longest)["refreshJwt"], longest)
    _check("refreshJwt" not in login(refreshJwt=...), "a missing refresh token was invented")
    for name in ("accessJwt", "refreshJwt"):
        for bad in ("SECRET\r\nX-Evil: 1", "SECRET token", "SECRET\x00", "SECRET\x1b[2J", "SECRET\x7f",
                    "", 7, None, "S" * (MAX_TOKEN_CHARS + 1)):
            if bad is None and name == "refreshJwt":
                bad = {"nested": "SECRET"}
            with _check_raises(ValueError, f"{name}={bad!r}"[:60] + " was accepted", contains=name):
                try:
                    login(**{name: bad})
                except ValueError as exc:
                    _check("SECRET" not in str(exc), str(exc))
                    raise

    # Logout never sends a malformed token, and never prints one.
    sent: List[Any] = []
    with _patched_globals(_open_api_response=lambda *a, **k: sent.append(k)), \
            _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        bsky_logout_session("https://pds.example", "SECRET\r\nX-Evil: 1")
    _check_equal(sent, [])
    _check("SECRET" not in stderr.getvalue() and "malformed" in stderr.getvalue(), stderr.getvalue())

    # Errors that quote request headers are shown by type only; the server's
    # explanation and transport errors keep their text.
    for exc, shown in (
        (requests.exceptions.InvalidHeader("bad value 'Bearer SECRET'"), "(InvalidHeader)"),
        (ValueError("Invalid header value b'Bearer SECRET'"), "(ValueError)"),
        (requests.exceptions.ConnectionError("pds.example refused"), "ConnectionError: pds.example refused"),
    ):
        def failing(*_args: Any, exc: BaseException = exc, **_kwargs: Any) -> Any:
            raise exc

        with _patched_globals(_open_api_response=failing), \
                _patched_attr(sys, "stderr", io.StringIO()) as stderr:
            bsky_logout_session("https://pds.example", "r.e.f")
        _check("SECRET" not in stderr.getvalue() and shown in stderr.getvalue(), stderr.getvalue())
    body = {"error": "InvalidToken", "message": "Token has expired"}
    with _patched_api_session(_FakeJsonResponse(400, body)), \
            _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        bsky_logout_session("https://pds.example", "r.e.f")
    _check("InvalidToken: Token has expired" in stderr.getvalue(), stderr.getvalue())


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
                              password="secret", verbose=False, image=None,
                              embed_url=None, embed_ref=None)

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


@contextmanager
def _keepalive_server() -> Iterator[Dict[str, Any]]:
    """A local HTTP/1.1 keep-alive JSON server that counts its connections.

    Yields {"url": base URL, "connections": [...], "cookies": [...]}; each
    response sets a cookie, and any Cookie header received is recorded.
    """
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    state: Dict[str, Any] = {"connections": [], "cookies": []}

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_args):
            pass

        def setup(self):
            state["connections"].append(1)
            super().setup()

        def do_GET(self):
            if self.headers.get("Cookie"):
                state["cookies"].append(self.headers["Cookie"])
            status = 400 if "/fail" in self.path else 200
            body = json.dumps({"ok": status == 200}).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Set-Cookie", "track=1; Path=/")
            self.end_headers()
            self.wfile.write(body)

    class Server(ThreadingHTTPServer):
        daemon_threads = True

        def handle_error(self, *_args):
            pass

    server = Server(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        state["url"] = f"http://127.0.0.1:{server.server_address[1]}"
        yield state
    finally:
        server.shutdown()
        server.server_close()


def test_raise_for_xrpc_accepts_only_2xx():
    # requests' Response.ok only rejects 400-599, so 1xx (http.client hands
    # back a 101 as the final status) and 600-999 would pass as success.
    def response(status: int) -> requests.Response:
        resp = requests.Response()
        resp.status_code = status
        resp.raw = io.BytesIO(b'{"error":"Odd","message":"odd status"}')
        resp.url = "https://pds.example/xrpc/test"
        return resp

    for status in (200, 201, 204, 299):
        _raise_for_xrpc(response(status), "test")
    for status in (100, 101, 199, 600, 999):
        with _check_raises(
            XrpcError, f"HTTP {status} passed as success", contains=f"HTTP {status}"
        ):
            _raise_for_xrpc(response(status), "test")


def _api_get(base: str, path: str = "/xrpc/x") -> Dict:
    with _open_api_response("GET", base + path, timeout=5, operation="test") as resp:
        _raise_for_xrpc(resp, "test")
        return _json_object(resp, "test")


def test_api_session_pool_reuses_only_clean_sessions():
    with _keepalive_server() as server:
        base = server["url"]
        # Without a pool (outside create_post) every request is a new connection.
        for _ in range(3):
            _check_equal(_api_get(base), {"ok": True})
        _check_equal(len(server["connections"]), 3)

        pool = _ApiSessionPool()
        token = _API_SESSION_POOL.set(pool)
        try:
            server["connections"].clear()
            for _ in range(3):
                _api_get(base)
            _check_equal(len(server["connections"]), 1)
            # Cookies the server sets are never sent back on a reused Session.
            _check_equal(server["cookies"], [])

            # A request that fails in the caller's block is not given back:
            # it reused the warm Session, which is then closed, so the next
            # request has to connect again.
            with _check_raises(XrpcError, "expected the 400 to raise"):
                _api_get(base, "/xrpc/fail")
            _check_equal(len(server["connections"]), 1)
            _api_get(base)
            _check_equal(len(server["connections"]), 2)

            # Neither is one abandoned by its deadline: its worker owns it.
            def abandoned(*_args, **kwargs):
                kwargs["on_abandon"]()
                raise DeadlineExceeded("timed out")

            with _patched_globals(_run_before_download_deadline=abandoned), _check_raises(
                DeadlineExceeded, "expected the abandoned request to time out"
            ):
                _api_get(base)
            _api_get(base)
            _check_equal(len(server["connections"]), 3)

            # A Session idle past the cap is closed, not reused.
            with _patched_globals(API_SESSION_MAX_IDLE_SECONDS=-1.0):
                _api_get(base)
            _check_equal(len(server["connections"]), 4)

            # Concurrent requests never share a leased Session.
            leased: List[int] = []
            real_lease = pool.lease

            def recording_lease(origin: str):
                session = real_lease(origin)
                leased.append(id(session))
                return session

            barrier = threading.Barrier(3, timeout=5)

            def concurrent_get() -> Dict:
                barrier.wait()
                return _api_get(base)

            with _patched_attr(pool, "lease", recording_lease):
                _run_concurrently([concurrent_get] * 3)
            _check_equal(len(set(leased)), 3)
        finally:
            _API_SESSION_POOL.reset(token)
            pool.close()

        # Sessions are per origin (scheme, host and port; case-insensitive).
        _check_equal(_session_origin("HTTPS://PDS.Example/xrpc/a?b=1"), "https://pds.example")
        _check_equal(len({_session_origin(url) for url in (
            "https://pds.example/x", "http://pds.example/x",
            "https://pds.example:8443/x", "https://records.example/x",
        )}), 4)
        other = _ApiSessionPool()
        sessions = []
        with _patched_globals(_new_session=lambda: sessions.append(object()) or sessions[-1]):
            first = other.lease("https://pds.example")
            other.give_back("https://pds.example", first)
            _check(other.lease("https://records.example") is not first)
            _check(other.lease("https://pds.example") is first)
        # After close(), a late give_back closes the Session instead.
        closed: List[Any] = []

        class Closable:
            def close(self):
                closed.append(self)

        other.close()
        late = Closable()
        other.give_back("https://pds.example", late)
        _check_equal(closed, [late])


def test_api_session_with_unread_body_is_not_pooled():
    """A Session whose response body was left unread has lost its connection;
    pooled on top, it would hide the warm Sessions beneath it."""

    def unread(base: str, path: str) -> int:
        # As _resolve_handle does for a 400: the status is all it needs.
        with _open_api_response("GET", base + path, timeout=5, operation="test") as resp:
            return resp.status_code

    with _keepalive_server() as server:
        base = server["url"]
        pool = _ApiSessionPool()
        token = _API_SESSION_POOL.set(pool)
        try:
            # Two warm Sessions: the second is leased while the first is out.
            with _open_api_response("GET", base + "/xrpc/x", timeout=5, operation="test") as resp:
                _json_object(resp, "test")
                _api_get(base)
            _check_equal(len(server["connections"]), 2)
            _check_equal(unread(base, "/xrpc/fail"), 400)
            _check_equal(unread(base, "/xrpc/x"), 200)  # an unread 200 as well
            # Both unread calls used (and closed) a warm Session; neither came
            # back, so these find no cold Session to connect afresh.
            server["connections"].clear()
            _check_equal(pool._idle.get(_session_origin(base), []), [])
            _api_get(base)
            _api_get(base)
            _check_equal(len(server["connections"]), 1)
        finally:
            _API_SESSION_POOL.reset(token)
            pool.close()

    # The warm one beneath stays usable: with three warm Sessions, an unread
    # call costs only the Session it used.
    with _keepalive_server() as server:
        base = server["url"]
        pool = _ApiSessionPool()
        token = _API_SESSION_POOL.set(pool)
        try:
            with _open_api_response("GET", base + "/xrpc/x", timeout=5, operation="test") as resp:
                _json_object(resp, "test")
                _api_get(base)
            unread(base, "/xrpc/fail")
            server["connections"].clear()
            _api_get(base)
            _check_equal(len(server["connections"]), 0)
        finally:
            _API_SESSION_POOL.reset(token)
            pool.close()


def test_create_post_shares_one_pool_and_closes_it():
    seen: List[Optional[_ApiSessionPool]] = []

    def record(result: Any) -> Callable[..., Any]:
        def run(*_args, **_kwargs):
            seen.append(_API_SESSION_POOL.get())
            return result
        return run

    closes: List[Any] = []
    real_close = _ApiSessionPool.close

    def tracking_close(self):
        closes.append(self)
        real_close(self)

    args = argparse.Namespace(pds_url="https://pds.example", handle="alice.test",
                              password="secret", verbose=False, image=None,
                              embed_url=None, embed_ref=None)
    for publish in (record({"uri": "at://ok"}), lambda *_a, **_k: (_ for _ in ()).throw(
            requests.HTTPError("createRecord failed"))):
        seen.clear()
        closes.clear()
        with _patched_globals(
            bsky_login_session=record({"accessJwt": "a", "refreshJwt": "r", "did": "did:plc:me"}),
            _build_post_record=record({"$type": "app.bsky.feed.post", "text": "t"}),
            _build_embed=record(None),
            _publish_post=publish,
            bsky_logout_session=record(None),
        ), _patched_attr(_ApiSessionPool, "close", tracking_close), _patched_attr(
            sys, "stdout", io.StringIO()
        ), _patched_attr(sys, "stderr", io.StringIO()):
            try:
                create_post(args)
            except requests.HTTPError:
                pass
        _check(seen and all(pool is seen[0] for pool in seen) and seen[0] is not None, repr(seen))
        _check_equal(closes, [seen[0]])
        _check(_API_SESSION_POOL.get() is None, "the pool outlived create_post")


def test_run_concurrently_overlaps_and_reports_in_task_order():
    # All three must be running at once to pass the barrier.
    barrier = threading.Barrier(3, timeout=5)
    label: ContextVar[str] = ContextVar("label", default="unset")
    label.set("caller")

    def task(value: int) -> Callable[[], tuple[int, str]]:
        def run() -> tuple[int, str]:
            barrier.wait()
            time.sleep(0.01 * (3 - value))  # finish in reverse order
            return value, label.get()
        return run

    _check_equal(_run_concurrently([task(0), task(1), task(2)]),
                 [(0, "caller"), (1, "caller"), (2, "caller")])
    _check_equal(_run_concurrently([]), [])
    _check_equal(_run_concurrently([threading.get_ident]), [threading.get_ident()])

    # Every task finishes; the earliest failing task's error is raised, even
    # when a later task fails first.
    finished: List[int] = []

    def fails(index: int, delay: float) -> Callable[[], None]:
        def run() -> None:
            time.sleep(delay)
            finished.append(index)
            raise ValueError(f"task {index}")
        return run

    def succeeds() -> str:
        time.sleep(0.03)
        finished.append(9)
        return "ok"

    with _check_raises(ValueError, "expected the first task's error", contains="task 0"):
        _run_concurrently([fails(0, 0.05), fails(1, 0.0), succeeds])
    _check_equal(sorted(finished), [0, 1, 9])
    with _check_raises(KeyboardInterrupt, "a BaseException in a stage must propagate"):
        _run_concurrently([lambda: None, lambda: (_ for _ in ()).throw(KeyboardInterrupt())])


def test_run_concurrently_wait_is_interruptible():
    """Ctrl-C while stages run returns at once and leaves them to finish."""
    started = threading.Event()
    release = threading.Event()

    def stalled() -> None:
        started.set()
        release.wait(10)

    began = time.monotonic()
    try:
        with _sigint_main_thread_when(started), _check_raises(
            KeyboardInterrupt, "expected Ctrl-C to interrupt the wait"
        ):
            _run_concurrently([stalled, stalled])
        _check(time.monotonic() - began < 3, "interrupt waited for the stages")
        _check(all(thread.daemon for thread in threading.enumerate()
                   if thread.name.startswith("bsky-task-")),
               "stage threads must never delay exit")
    finally:
        release.set()


def test_mention_lookups_are_bounded_daemon_tasks():
    active, peak, seen = [0], [0], []
    lock = threading.Lock()

    def fake_resolve(_pds: str, handle: str) -> Optional[str]:
        with lock:
            active[0] += 1
            peak[0] = max(peak[0], active[0])
        thread = threading.current_thread()
        seen.append((thread.daemon, _NETWORK_TIMEOUT_SUBJECT.get(), _DOWNLOAD_DEADLINE.get() is not None))
        time.sleep(0.02)
        with lock:
            active[0] -= 1
        return None if handle.startswith("x") else f"did:plc:{handle[:3]}"

    handles = [f"{'x' if i % 3 == 0 else 'h'}{i:02d}.example.com" for i in range(20)]
    with _patched_globals(_resolve_handle=fake_resolve):
        resolved = _resolve_handles("https://pds.example", handles)
    _check_equal(list(resolved), handles)
    _check_equal([resolved[h] is None for h in handles], [h.startswith("x") for h in handles])
    _check_equal(peak[0], MENTION_RESOLUTION_WORKERS)
    _check_equal(set(seen), {(True, "Mention resolution", True)})

    # More tasks than workers still run each exactly once, in order.
    ran: List[int] = []
    results = _run_concurrently([lambda i=i: ran.append(i) or i for i in range(9)], max_workers=2)
    _check_equal((results, sorted(ran)), (list(range(9)), list(range(9))))

    # And Ctrl-C during hung lookups exits at once, not after their timeouts.
    probe = """
import os, signal, socket, sys, threading, time
sys.path.insert(0, sys.argv[1])
import create_bsky_post as m
listener = socket.create_server(("127.0.0.1", 0))
held = []
threading.Thread(target=lambda: [held.append(listener.accept()) for _ in iter(int, 1)],
                 daemon=True).start()
# A real SIGINT, as Ctrl-C sends: interrupt_main() would not wake a blocked join.
threading.Timer(0.3, os.kill, (os.getpid(), signal.SIGINT)).start()
url = "http://127.0.0.1:%d" % listener.getsockname()[1]
try:
    m._run_concurrently([lambda: m._resolve_handles(url, ["a.example.com", "b.example.com"]),
                         lambda: time.sleep(30)])
except KeyboardInterrupt:
    pass
"""
    started = time.monotonic()
    completed = subprocess.run([sys.executable, "-c", probe, str(Path(bsky.__file__).resolve().parent)],
                               capture_output=True, timeout=60, check=False)
    elapsed = time.monotonic() - started
    _check_equal(completed.returncode, 0)
    _check(elapsed < 5, f"exit after Ctrl-C took {elapsed:.1f}s")


def test_upload_images_runs_concurrently_in_order():
    barrier = threading.Barrier(3, timeout=5)
    calls: List[tuple[bytes, Optional[int]]] = []

    def fake_upload(_pds, _token, img_bytes, mimetype, *, concurrent_bytes=None):
        barrier.wait()
        calls.append((img_bytes, concurrent_bytes))
        time.sleep(0.01 * (3 - len(img_bytes)))  # the first one finishes last
        return {"ref": img_bytes.decode(), "mimeType": mimetype}

    prepared = [
        (b"a", {"mimetype": "image/png", "width": 1, "height": 1}, "first"),
        (b"bb", {"mimetype": "image/jpeg", "width": 2, "height": 1}, "second"),
        (b"ccc", {"mimetype": "image/webp", "width": 3, "height": 1}, "third"),
    ]
    with _patched_globals(upload_file=fake_upload):
        embed = upload_images("https://pds.example", "token", prepared)
    _check_equal([(item["image"]["ref"], item["alt"], item["aspectRatio"]["width"])
                  for item in embed["images"]],
                 [("a", "first", 1), ("bb", "second", 2), ("ccc", "third", 3)])
    # Each concurrent upload is budgeted for all the bytes sharing the uplink.
    _check_equal(sorted(calls), [(b"a", 6), (b"bb", 6), (b"ccc", 6)])

    calls.clear()
    barrier = threading.Barrier(1)
    with _patched_globals(upload_file=fake_upload):
        upload_images("https://pds.example", "token", prepared[:1])
    _check_equal(calls, [(b"a", None)])
    body = b"x" * 1000
    with _patched_api_session(_blob_answer(body, "image/png")) as session:
        upload_file("https://pds.example", "token", body, "image/png", concurrent_bytes=5000)
    _check_equal(session.calls[0][2]["timeout"], _upload_timeout(5000))


def test_post_assembly_stages_overlap():
    """Mentions, the reply lookup, the quote lookup and the media all run at once."""
    barrier = threading.Barrier(4, timeout=5)

    def stage(result: Any) -> Callable[..., Any]:
        def run(*_args, **_kwargs):
            barrier.wait()
            return result
        return run

    args = argparse.Namespace(
        pds_url="https://pds.example", record_service_url="https://records.example",
        handle="alice.test", password="secret", verbose=False, text="hi @bob.test",
        lang=None, reply_to="at://did:plc:a/app.bsky.feed.post/1",
        embed_ref="at://did:plc:a/app.bsky.feed.post/2", embed_url=None,
        image=["a.png"], prepared_images=[],
    )
    published: List[Dict] = []
    with _patched_globals(
        bsky_login_session=lambda *_a: {"accessJwt": "access", "did": "did:plc:me"},
        parse_facets=stage(["facet"]),
        get_reply_refs=stage({"root": "r", "parent": "p"}),
        get_embed_ref=stage({"$type": "app.bsky.embed.record"}),
        upload_images=stage({"$type": "app.bsky.embed.images"}),
        _publish_post=lambda _pds, _token, _did, post, verbose: published.append(post) or {},
    ), _patched_attr(sys, "stdout", io.StringIO()), _patched_attr(sys, "stderr", io.StringIO()):
        create_post(args)
    post = published[0]
    _check_equal((post["facets"], post["reply"]), (["facet"], {"root": "r", "parent": "p"}))
    _check_equal(post["embed"], {
        "$type": "app.bsky.embed.recordWithMedia",
        "record": {"$type": "app.bsky.embed.record"},
        "media": {"$type": "app.bsky.embed.images"},
    })

    # With several stages failing, the record side's error is the one reported.
    def broken(message: str) -> Callable[..., Any]:
        def run(*_args, **_kwargs):
            raise ValueError(message)
        return run

    with _patched_globals(
        bsky_login_session=lambda *_a: {"accessJwt": "access", "did": "did:plc:me"},
        parse_facets=lambda *_a: [],
        get_reply_refs=broken("reply target"),
        get_embed_ref=broken("quoted record"),
        upload_images=broken("upload"),
        _publish_post=lambda *_a, **_k: (_ for _ in ()).throw(AssertionError("published")),
    ), _check_raises(ValueError, "expected the reply error", contains="reply target"):
        create_post(args)


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
    head = ('<meta property="og:title" content="  "><title>Real title</title>'
            '<meta property="og:image" content="\n  https://cdn.example/a.jpg\n">')
    meta, title = _card_metadata(head)
    _check_equal(_external_card_metadata("https://example.com/", meta, title)["title"], "Real title")
    _check_equal(_meta_content(head, "og:image"), "https://cdn.example/a.jpg")


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
        _html_head_bytes(b"<head><title>Head title</title></head><body><title>Body</title>"),
        "text/html",
    )
    _check_equal(card["title"], "Head title")


def test_html_head_bytes_follows_real_tags_only():
    """</head> and <body> count only as tags, never as text or comments."""
    real = b"<meta property='og:title' content='real'>"
    # Text that merely looks like a boundary must not end the head early.
    for decoy in (
        b"<script>s = '</head><body>';</script>",
        b"<style>/* </head> <body> */</style>",
        b"<!-- </head> <body> -->",
        b"<title>a </head> <body> b</title>",
        b"<TITLE>a </HEAD> b</TITLE>",
        b"<noframes></head></noframes>",
        b"<meta name='x' content='</head> <body>'>",
        b"<link href=\"/x?a=</head>\">",
    ):
        page = b"<html><head>" + decoy + real + b"</head><body>x"
        _check_equal(_html_head_bytes(page), b"<html><head>" + decoy + real)
        card, _ = _parse_embed_metadata("https://example.com/", _html_head_bytes(page), "text/html")
        _check_equal(card["title"], "real")
    # Without </head>, the head ends at the first real <body>, even when a
    # "</head>" appears later: body markup must never feed the card.
    spoof = b"<meta property='og:title' content='spoof'>"
    for body in (b"<body>", b"<BODY class=x>", b"<body/>", b"\n<body\n>"):
        page = b"<html><head><title>Real</title>" + body + spoof + b"<script>'</head>'</script>"
        _check_equal(_html_head_bytes(page), b"<html><head><title>Real</title>" + body.split(b"<")[0])
        card, _ = _parse_embed_metadata("https://example.com/", _html_head_bytes(page), "text/html")
        _check_equal(card["title"], "Real")
    # A self-closing text-only tag has no contents, and a malformed character
    # reference is just text: neither may stall the scan so that it misses
    # the real </head> and hands the whole window, body included, to the card.
    body = b"<body><meta property='og:description' content='BODY_DESC'>BODY</body>"
    for decoy in (
        b"<script src='a.js' />", b"<title />", b"<style/>", b"<iframe />",
        b"<textarea/>", b"&#;", b"&#foo;", b"&#x;", b"&;", b"&#",
        b"<script>&#;</script>", b"<!-- &#; -->",
    ):
        page = b"<html><head>" + decoy + real + b"</head>" + body
        _check_equal(_html_head_bytes(page), b"<html><head>" + decoy + real)
        card, _ = _parse_embed_metadata("https://example.com/", _html_head_bytes(page), "text/html")
        _check_equal(card["description"], "")
        # BeautifulSoup itself drives html.parser with convert_charrefs off,
        # so it loses what follows a bare "&#<"; the head slice is still right.
        if decoy != b"&#":
            _check_equal(card["title"], "real")
    # Without </head>, a self-closing <body/> still ends the head.
    page = b"<head><title />" + real + b"<body/>" + spoof
    _check_equal(_html_head_bytes(page), b"<head><title />" + real)
    # Offsets stay byte-exact across lines and multi-byte characters.
    page = "<head>\n<title>é</title>\n\n<meta name=a content='ü'>\n</head>".encode()
    _check_equal(_html_head_bytes(page), page[: page.index(b"</head>")])


def _head_test_pages() -> List[bytes]:
    real = b"<meta property='og:title' content='real'>"
    body = b"<body><meta property='og:description' content='BODY_DESC'>BODY</body>"
    pages = [
        b"<html><head>" + decoy + real + b"</head>" + body
        for decoy in (
            b"<script>s = '</head><body>';</script>", b"<!-- </head> <body> -->",
            b"<title>a </head> <body> b</title>", b"<meta name='x' content='</head>'>",
            b"<script src='a.js' />", b"<title />", b"&#;", b"&#foo;", b"<style/>",
        )
    ]
    pages += [
        b"<html><head><title>Real</title>\n<BODY class=x>" + real,
        "<head>\n<title>é</title>\n\n<meta name=a content='ü'>\n</head>x".encode(),
        b"<title>no boundary at all</title>" + real,
        b"<head><title>split </he" + b"ad> inside</title></head><body>",
    ]
    return pages


def test_head_prefix_parses_tiny_chunks_in_linear_time():
    scanned = [0]

    class CountingParser(_HeadOffsetParser):
        def feed(self, data: str) -> None:
            # What html.parser rescans per feed: all it held back, plus the new text.
            scanned[0] += len(self.rawdata) + len(data)
            super().feed(data)

    def stream(page: bytes, size: int) -> tuple[bytes, int, bool]:
        scanned[0] = 0
        with _patched_globals(_HeadOffsetParser=CountingParser):
            head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
            consumed, done = 0, False
            for start in range(0, len(page), size):
                consumed += len(page[start:start + size])
                if head.feed(page[start:start + size]):
                    done = True
                    break
            return head.result(), consumed, done

    for opener, closer in ((b"<script>", b"</script>"), (b"<!--", b"-->"),
                           (b'<meta content="', b'">'), (b"<title>", b"</title>")):
        # Inside a tag, just under the length that ends the head anyway.
        filler = b"x" * (60_000 if opener.startswith(b"<meta") else 150_000)
        page = b"<html><head>" + opener + filler + closer + b"</head><body>" + b"y" * 300_000
        end = page.index(b"</head>")
        head, consumed, done = stream(page, 20)
        _check_equal(head, page[:end])
        # The parser really was counted: it saw every byte of the head at least once.
        _check(end <= scanned[0] <= 4 * end,
               f"{opener!r}: scanned {scanned[0]} for a {end}-byte head")
        # Batching may read past the boundary, but by no more than about as much again.
        _check(done and consumed <= 2 * end + 40, f"{opener!r}: read {consumed} of {len(page)}")

    # Ordinary markup is passed on as it arrives: the boundary is noticed
    # within a chunk or two of arriving.
    page = b"<html><head>" + b"<meta name='a' content='b'>" * 400 + b"</head><body>" + b"z" * 200_000
    end = page.index(b"</head>") + len(b"</head>")
    head, consumed, done = stream(page, 64)
    _check_equal(head, page[:page.index(b"</head>")])
    _check(done and consumed <= end + 2 * 64, f"read {consumed} for a head ending at {end}")

    # Bytes still waiting when the page ends are parsed then.
    page = b"<head><script>" + b"s" * 5000 + b"</script></head>"
    _check_equal(stream(page, 1)[0], page[:page.index(b"</head>")])
    # A boundary in the last batch before the window fills is still found:
    # the card must not get the whole window, body included.
    page = (b"<head><script>" + b"s" * (MAX_EMBED_HEAD_BYTES - 200) + b"</script></head><body>"
            + b"<meta property='og:title' content='spoof'>" * 100)
    _check_equal(stream(page, 20)[0], page[:page.index(b"</head>")])

    # The same through the real download path, the page ending mid-batch.
    page = b"<head><script>" + b"s" * 5000 + b"</script><title>T</title></head><body><title>B</title>"
    response = _ChunkedHtmlResponse([page[i:i + 20] for i in range(0, len(page), 20)])
    _check_equal(_read_response_body(response, MAX_EMBED_HEAD_BYTES, html_head_only=True),
                 page[:page.index(b"</head>")])


def test_head_prefix_is_independent_of_chunking():
    """Fed in any pieces, the head ends at exactly the byte a whole feed finds."""
    for page in _head_test_pages():
        expected = _html_head_bytes(page)
        for size in (1, 2, 3, 7, 64, len(page)):
            head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
            done = False
            for start in range(0, len(page), size):
                if head.feed(page[start:start + size]):
                    done = True
                    break
            _check_equal(head.result(), expected)
            # Pieces are batched while the parser holds an unfinished construct,
            # so completion may be noticed a batch later (or at the final flush)
            # but is never claimed early; fed whole, it is noticed at once.
            if done or size == len(page):
                _check(done == (len(expected) < len(page)) or len(page) >= MAX_EMBED_HEAD_BYTES,
                       f"completion reported wrongly for {page[:40]!r}")
    # The window cap applies however the bytes arrive.
    head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
    chunk = b"a" * 65536
    fed = 0
    while not head.feed(chunk):
        fed += 1
    _check_equal(len(head.result()), MAX_EMBED_HEAD_BYTES)
    _check_equal(fed, MAX_EMBED_HEAD_BYTES // len(chunk))


class _ChunkedHtmlResponse:
    """A streamed 200 response that records how many chunks were pulled."""

    def __init__(self, chunks: List[bytes], declared: Optional[int] = None):
        self.status_code = 200
        self.headers = {"Content-Type": "text/html"}
        if declared is not None:
            self.headers["Content-Length"] = str(declared)
        self._chunks = chunks
        self.pulled = 0

    def raise_for_status(self):
        return None

    def iter_content(self, _chunk_size: int):
        for chunk in self._chunks:
            self.pulled += 1
            yield chunk


def test_head_boundary_offset_is_tracked_not_recounted():
    pages = [
        b"<html><head>" + b"\n" * 200_000 + b"<title>t</title></head><body>",
        b"<head>\r\n<meta content='a\nb'>\n<!-- </head>\n -->\n&amp\n&#x;\n<title>\n</title></HEAD\n>",
        b"<head><script>\n</head>\n</script>\n<style>\n</style>\n<body\nclass=x>",
        b"\n\n<title>x</title>\n&am" + b"p;" * 3 + b"<body>",
    ]
    for page, tag in zip(pages, (b"</head>", b"</HEAD\n>", b"<body\n", b"<body>")):
        boundary = page.rindex(tag)
        for size in (1, 2, 3, 7, 64, len(page)):
            head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
            for start in range(0, len(page), size):
                if head.feed(page[start:start + size]):
                    break
            _check_equal(head.result(), page[:boundary])
            _check_equal(head._parser.boundary_offset, boundary)

    class CountingWindow(bytearray):
        walked = 0

        def index(self, *args: Any) -> int:
            CountingWindow.walked += 1
            return super().index(*args)

    page = pages[0]
    head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
    head._window = CountingWindow()
    _check(head.feed(page), "no boundary found")
    _check_equal((head.result(), CountingWindow.walked), (page[:page.index(b"</head>")], 0))
    # Should the tracked offset ever disagree with getpos(), the line walk decides.
    for wrong in (0, 1, head._parser.boundary_offset - 1, head._parser.boundary_offset + 1):
        head._head = None
        head._parser.boundary_offset = wrong
        _check_equal(head._before_boundary(), page[:page.index(b"</head>")])
    _check(CountingWindow.walked > 0, "the fallback walk never ran")
    # Equal-length lines: the same column on an earlier line is still refused.
    page = b"<head>\n" + b"abcdefghij\n" * 5 + b"abcdefghij</head>"
    head = _HeadPrefix(MAX_EMBED_HEAD_BYTES)
    _check(head.feed(page), "no boundary found")
    head._parser.boundary_offset = len(b"<head>\nabcdefghij\n") + 10
    _check_equal(head._before_boundary(), page[:page.index(b"</head>")])


def test_each_start_tag_is_scanned_once():
    scanned: List[int] = []
    real = _start_tag_match_end

    def counting(rawdata: str, i: int, limit: Optional[int] = None) -> int:
        scanned.append(i)
        return real(rawdata, i, limit)

    head = "<head><meta property=og:title content=A><title>T</title><meta name=x></head>"
    with _patched_globals(_start_tag_match_end=counting):
        if EXACT_START_TAG_SCAN:
            meta, title = _card_metadata(head)
            _check_equal((meta["og:title"], title), ("A", "T"))
            _check_equal(len(scanned), 4)  # <head>, <meta>, <title>, <meta>
            _check_equal(len(set(scanned)), 4)
        # An incomplete tag is answered once too, as html.parser would.
        scanned.clear()
        parser = _CardMetadataParser(CARD_META_KEYS)
        parser.feed("<head><meta property=og:title content='A")
        _check_equal(len(scanned), 2 if EXACT_START_TAG_SCAN else 0)
        parser.feed("'>")
        _check_equal(parser.by_property, {"og:title": "A"})


def test_parsers_build_only_what_they_use():
    parser = _CardMetadataParser(CARD_META_KEYS)
    parser.feed("<meta name=x>")
    # The window parser exists only for an html.parser this script does not know.
    _check((parser._window_parser is None) == EXACT_START_TAG_SCAN, "window parser built eagerly")
    with _patched_globals(EXACT_START_TAG_SCAN=False):
        parser = _CardMetadataParser(CARD_META_KEYS)
        parser.feed("<meta property=og:title content=A>")
        _check(parser._window_parser is not None, "the fallback never built its window parser")
        _check_equal(parser.by_property, {"og:title": "A"})
    # Only the head-boundary parser tracks byte offsets.
    for cls in (_HeadBoundaryParser, _CardMetadataParser, _MetaCharsetParser):
        _check(cls.updatepos is HTMLParser.updatepos, f"{cls.__name__} tracks offsets")
    _check(_HeadOffsetParser.updatepos is not HTMLParser.updatepos)


def test_plain_posts_start_no_threads():
    """Concurrency only where two stages have work (Optional tasks)."""
    started: List[str] = []
    real_thread = threading.Thread

    class CountingThread(real_thread):
        def start(self) -> None:
            started.append(self.name)
            super().start()

    base = dict(pds_url="https://pds.example", record_service_url="https://records.example",
                handle="alice.test", password="secret", verbose=False, text="Hello, Bluesky!",
                lang=None, reply_to=None, embed_ref=None, embed_url=None, image=None,
                prepared_images=None)
    published: List[Dict] = []
    stubs = dict(
        bsky_login_session=lambda *_a: {"accessJwt": "access", "did": "did:plc:me"},
        _publish_post=lambda _pds, _token, _did, post, **_k: published.append(post) or {"uri": "u"},
        get_reply_refs=lambda *_a: {"root": "r", "parent": "p"},
        get_embed_ref=lambda *_a: {"$type": "app.bsky.embed.record"},
        fetch_embed_url_card=lambda *_a: {"$type": "app.bsky.embed.external"},
        _did_addressed_uri=lambda _pds, uri: uri,
    )
    for extra, threads in (({}, 0), ({"text": ""}, 0), ({"reply_to": "at://r"}, 2),
                           ({"text": "", "reply_to": "at://r"}, 0),
                           ({"embed_ref": "at://q"}, 2),
                           ({"embed_ref": "at://q", "embed_url": "https://e"}, 4),
                           ({"text": "", "embed_url": "https://e"}, 2)):
        started.clear()
        published.clear()
        args = argparse.Namespace(**dict(base, **extra))
        with _patched_globals(**stubs), _patched_attr(threading, "Thread", CountingThread), \
                _patched_attr(sys, "stdout", io.StringIO()), _patched_attr(sys, "stderr", io.StringIO()):
            create_post(args)
        _check_equal((extra, len(started)), (extra, threads))
        post = published[0]
        _check_equal(("reply" in post, "embed" in post),
                     (bool(extra.get("reply_to")), bool(extra.get("embed_ref") or extra.get("embed_url"))))

    # None tasks yield None and never reach a thread; order is kept.
    _check_equal(_run_concurrently([None, lambda: 1, None]), [None, 1, None])
    _check_equal(_run_concurrently([None, None]), [None, None])
    _check_equal(_run_concurrently([]), [])


def test_link_card_scans_for_the_head_once():
    page = (b"<html><head><title>T</title><meta content='&amp;&amp;'></head>"
            b"<body><meta property='og:description' content='spoof'><title>B</title>")
    response = _ChunkedHtmlResponse([page], len(page))
    scans: List[int] = []

    class CountingParser(_HeadOffsetParser):
        def __init__(self) -> None:
            scans.append(1)
            super().__init__()

    @contextmanager
    def fake_open(_url: str, _timeout: float):
        yield response

    with _patched_globals(_open_pinned_response=fake_open, _HeadOffsetParser=CountingParser,
                          _attach_external_thumb=lambda *_a, **_k: None), \
            _patched_attr(sys, "stderr", io.StringIO()):
        card = fetch_embed_url_card("https://pds.example", "token", "https://example.com/")
    _check_equal((card["external"]["title"], card["external"]["description"]), ("T", ""))
    _check_equal(len(scans), 1)


def test_page_charset_must_name_a_character_set():
    import encodings
    import encodings.aliases
    import pkgutil
    import zlib

    page = b"<html><head><title>T\xc3\xa9</title></head>"
    labels = set(encodings.aliases.aliases) | set(encodings.aliases.aliases.values())
    labels |= {module.name for module in pkgutil.iter_modules(encodings.__path__)}
    for label in sorted(labels):
        for content_type, head in ((f"text/html; charset={label}", page),
                                   ("text/html", b'<meta charset="' + label.encode() + b'">' + page)):
            text = _decode_html_head(head, content_type)
            # Always text, never longer than its bytes: no codec inflates it.
            _check(isinstance(text, str) and len(text) <= len(head), f"{label}: {text!r}"[:200])
    for name in ("zlib", "bz2", "base64", "hex", "quopri", "rot_13", "uu", "idna", "punycode",
                 "unicode_escape", "raw_unicode_escape", "undefined", "utf-7", "charmap"):
        _check(_html_charset_codec(name) is None, f"{name} accepted as a charset")
    for name in ("utf-8", "utf-16le", "windows-1252", "latin-1", "shift_jis", "gb18030", "koi8-r"):
        _check(_html_charset_codec(name) is not None, f"{name} refused")
    _check_equal(_decode_html_head(page, "text/html; charset=zlib"), page.decode())

    # Behind the filter: a codec that slipped through still cannot raise past
    # the decode or hand back bytes.
    with _patched_globals(_html_charset_codec=codecs.lookup):
        for label in ("undefined", "zlib"):
            head = zlib.compress(page) if label == "zlib" else page
            _check(isinstance(_decode_html_head(head, f"text/html; charset={label}"), str), label)

    # A compressed head declared charset=zlib is never inflated.
    bomb = zlib.compress(b"A" * 5_000_000)
    for content_type in ("text/html; charset=zlib", "text/html; charset=x-zlib", "text/html; charset=zip"):
        _check(len(_decode_html_head(bomb, content_type)) <= len(bomb), content_type)

    # And a hostile label costs the card nothing, end to end.
    for label in ("zlib", "bz2", "rot_13", "undefined", "base64", "hex", "idna", "quopri"):
        for headers, head in (({"Content-Type": f"text/html; charset={label}"}, page),
                              ({"Content-Type": "text/html"}, b'<meta charset="' + label.encode() + b'">' + page)):
            response = _ChunkedHtmlResponse([head + b"<body>x"])
            response.headers.update(headers)

            @contextmanager
            def fake_open(_url: str, _timeout: float):
                yield response

            with _patched_globals(_open_pinned_response=fake_open,
                                  _attach_external_thumb=lambda *_a, **_k: None), \
                    _patched_attr(sys, "stderr", io.StringIO()) as stderr:
                card = fetch_embed_url_card("https://pds.example", "token", "https://example.com/")
            _check_equal((label, card["external"]["title"]), (label, "Té"))
            _check_equal(stderr.getvalue(), "")


def test_html_head_only_reads_stop_at_the_head():
    head = b"<html><head><title>T</title><meta property='og:title' content='Big page'></head>"
    filler = b"<p>" + b"x" * 65533
    chunks = [head + b"<body>"] + [filler] * 200  # about 13 MB declared
    declared = sum(map(len, chunks))
    response = _ChunkedHtmlResponse(chunks, declared)
    _check_equal(_read_response_body(response, MAX_EMBED_HEAD_BYTES, html_head_only=True),
                 head[:-len(b"</head>")])
    _check_equal(response.pulled, 1)
    # A plain read still refuses the declared size before reading anything.
    response = _ChunkedHtmlResponse(chunks, declared)
    with _check_raises(ValueError, "expected the declared size to be refused", contains="declares"):
        _read_response_body(response, MAX_EMBED_HEAD_BYTES)
    _check_equal(response.pulled, 0)
    # No boundary: stop once the window is full, never reading the rest.
    response = _ChunkedHtmlResponse([filler] * 200, declared)
    _check_equal(len(_read_response_body(response, MAX_EMBED_HEAD_BYTES, html_head_only=True)),
                 MAX_EMBED_HEAD_BYTES)
    _check_equal(response.pulled, -(-MAX_EMBED_HEAD_BYTES // len(filler)))
    # The cap is the caller's max_bytes, not a fixed constant.
    for cap in (100, len(filler) + 7):
        response = _ChunkedHtmlResponse([filler] * 200, declared)
        _check_equal(len(_read_response_body(response, cap, html_head_only=True)), cap)
        _check_equal(response.pulled, -(-cap // len(filler)))

    # End to end: a page far above any whole-page cap still gets its card,
    # and only its first chunk is read.
    response = _ChunkedHtmlResponse(chunks, declared)

    @contextmanager
    def fake_open(_url: str, _timeout: float):
        yield response

    with _patched_globals(_open_pinned_response=fake_open,
                          _attach_external_thumb=lambda *_a, **_k: None), \
            _patched_attr(sys, "stderr", io.StringIO()) as stderr:
        card = fetch_embed_url_card("https://pds.example", "token", "https://example.com/big")
    _check_equal(card["external"]["title"], "Big page")
    _check_equal(stderr.getvalue(), "")
    _check_equal(response.pulled, 1)


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
        (" text/html ; charset=utf-8", True),  # whitespace around the type
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
    # Only DID-addressed requests are made, so the repo is always compared.
    for target in ("https://bsky.app/profile/alice.test/post/one", "at://alice.test/app.bsky.feed.post/one",
                   "https://bsky.app/profile/did:plc:abc/post/one"):
        with _patched_api_session(answer(requested)) as session, _check_raises(
            ValueError, f"looked up {target}", contains="by DID"
        ):
            get_record("https://api.example", target)
        _check_equal(session.calls, [])
    for wrong in (
        "at://did:plc:abc/app.bsky.feed.post/other",
        "at://did:plc:xyz/app.bsky.feed.post/one",
        "at://did:plc:abc/app.bsky.graph.list/one",
    ):
        with _patched_api_session(answer(wrong)), _check_raises(
            ValueError, f"accepted {wrong}", contains="different record"
        ):
            get_record("https://api.example", requested)


def test_reply_and_quote_targets_are_pinned_to_a_did():
    resolved: List[tuple[str, str]] = []

    def pds_resolve(pds_url: str, handle: str) -> Optional[str]:
        resolved.append((pds_url, handle))
        return {"alice.bsky.social": "did:plc:alice"}.get(handle)

    with _patched_globals(_resolve_handle=pds_resolve):
        _check_equal(_did_addressed_uri("https://pds.example",
                                        "https://bsky.app/profile/Alice.bsky.social/post/3kabc"),
                     "at://did:plc:alice/app.bsky.feed.post/3kabc")
        _check_equal(_did_addressed_uri("https://pds.example", "at://alice.bsky.social/app.bsky.feed.post/3kabc"),
                     "at://did:plc:alice/app.bsky.feed.post/3kabc")
        # A DID needs no lookup, whichever form names it.
        _check_equal(_did_addressed_uri("https://pds.example", "https://bsky.app/profile/did:plc:bob/post/3k"),
                     "at://did:plc:bob/app.bsky.feed.post/3k")
        with _check_raises(ValueError, "an unresolvable handle was used", contains="through the PDS"):
            _did_addressed_uri("https://pds.example", "https://bsky.app/profile/nobody.example/post/3k")
    # Resolved by the user's PDS, never by the record service.
    _check_equal(resolved, [("https://pds.example", "alice.bsky.social")] * 2
                 + [("https://pds.example", "nobody.example")])

    # A rogue record service cannot swap in another account's post of the
    # same record key, for a reply or a quote.
    stolen = {"uri": "at://did:plc:attacker/app.bsky.feed.post/3kabc", "cid": _TEST_CID,
              "value": {"$type": "app.bsky.feed.post", "text": "x", "createdAt": "2026-01-01T00:00:00Z"}}
    args = argparse.Namespace(text="Replying", lang=None, pds_url="https://pds.example",
                              record_service_url="https://records.example",
                              reply_to="https://bsky.app/profile/alice.bsky.social/post/3kabc",
                              embed_ref="https://bsky.app/profile/alice.bsky.social/post/3kabc")
    with _patched_globals(_resolve_handle=pds_resolve), _patched_api_session(_FakeJsonResponse(200, stolen)) as session:
        with _check_raises(ValueError, "the reply took another account's post", contains="different record"):
            _build_post_record(args)
        with _check_raises(ValueError, "the quote took another account's post", contains="different record"):
            get_embed_ref(args.record_service_url, _did_addressed_uri(args.pds_url, args.embed_ref))
    _check(all(call[2]["params"]["repo"] == "did:plc:alice" for call in session.calls), repr(session.calls))

    # Stored references always name a DID: a handle could later point anywhere.
    for uri in ("at://alice.bsky.social/app.bsky.feed.post/3k", "https://bsky.app/profile/did:plc:a/post/3k"):
        with _check_raises(ValueError, f"record_ref kept {uri}", contains="by DID"):
            record_ref({"uri": uri, "cid": _TEST_CID})
    # Nor may a parent's own reply.root (anyone's content) smuggle one in,
    # with or without a cid, before anything is looked up.
    looked_up: List[str] = []
    with _patched_globals(get_record=lambda _service, uri: looked_up.append(uri)):
        for root in ({"uri": "at://evil.example/app.bsky.feed.post/3k", "cid": _TEST_CID},
                     {"uri": "https://bsky.app/profile/evil.example/post/3k"},
                     {"uri": "https://bsky.app/profile/did:plc:x/post/3k"}):
            with _check_raises(ValueError, f"reply root {root['uri']} was accepted", contains="by DID"):
                _reply_root_ref("https://records.example", {"root": root})
    _check_equal(looked_up, [])


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


def _blob_answer(body: bytes, mimetype: str, **changes: Any) -> "_FakeJsonResponse":
    """An uploadBlob answer for `body`, as a PDS gives it, with `changes`."""
    raw_cid = b"\x01\x55\x12\x20" + __import__("hashlib").sha256(body).digest()
    cid = "b" + base64.b32encode(raw_cid).decode("ASCII").lower().rstrip("=")
    blob = {"$type": "blob", "ref": {"$link": cid}, "mimeType": mimetype, "size": len(body)}
    blob.update(changes)
    return _FakeJsonResponse(200, {"blob": {key: value for key, value in blob.items() if value is not ...}})


def test_upload_blob_answer_must_describe_the_bytes_sent():
    body = b"\x89PNG\r\n\x1a\n" + b"stego payload"
    # A known vector: the CID of the empty blob.
    _check_equal(_raw_blob_cid(b""), "bafkreihdwdcefgh4dqkjv67uzcmw7ojee6xedzdetojuzjevtenxquvyku")
    good = _blob_answer(body, "image/png")
    expected = json.loads(good._body)["blob"]
    with _patched_api_session(good):
        _check_equal(upload_file("https://pds.example", "token", body, "image/png"), expected)
    # Extra fields are dropped, not published in the post.
    with _patched_api_session(_blob_answer(body, "image/png", extra={"x": "y" * 1000}, original="z")):
        _check_equal(upload_file("https://pds.example", "token", body, "image/png"), expected)
    other_cid = json.loads(_blob_answer(b"other bytes", "image/png")._body)["blob"]["ref"]
    for name, changes in (
        ("another blob", {"ref": other_cid}),
        ("a CIDv1 dag-cbor link", {"ref": {"$link": _TEST_CID}}),
        ("a ref that is no object", {"ref": "bafy"}),
        ("another type", {"mimeType": "image/svg+xml"}),
        ("another size", {"size": len(body) + 1}),
        ("a float size", {"size": float(len(body))}),
        ("a bool size", {"size": True}),
        ("no $type", {"$type": ...}),
        ("a legacy blob", {"$type": "image"}),
    ):
        with _patched_api_session(_blob_answer(body, "image/png", **changes)), _check_raises(
            ValueError, f"accepted {name}", contains="does not describe the uploaded bytes"
        ):
            upload_file("https://pds.example", "token", body, "image/png")


def test_upload_timeout_scales_with_size():
    _check_equal(_upload_timeout(0), UPLOAD_BASE_TIMEOUT_SECONDS)
    # A maximum-size image must fit on a 256 kbit/s uplink.
    _check(_upload_timeout(MAX_IMAGE_SIZE_BYTES) > MAX_IMAGE_SIZE_BYTES * 8 / 256_000)
    body = b"\x89PNG\r\n\x1a\n" + b"x" * 1000
    with _patched_api_session(_blob_answer(body, "image/png")) as session:
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


def test_main_refuses_explicit_empty_flag_values():
    """An explicit "" is refused, never treated as an omitted flag."""

    def forbidden_post(_args):
        raise AssertionError("posted despite an empty flag value")

    def exit_message(argv: List[str], credentials: Optional[Dict[str, str]] = None) -> str:
        environment = dict(os.environ)
        for name in ("ATP_PDS_HOST", "ATP_RECORD_SERVICE_HOST", "ATP_AUTH_HANDLE",
                     "ATP_AUTH_PASSWORD"):
            environment.pop(name, None)
        environment.update(
            {"ATP_AUTH_HANDLE": "alice.test", "ATP_AUTH_PASSWORD": "secret"}
            if credentials is None else credentials
        )
        with _patched_attr(os, "environ", environment), _patched_attr(
            sys, "argv", ["create_bsky_post.py", *argv]
        ), _patched_attr(sys, "stderr", io.StringIO()), _patched_globals(
            create_post=forbidden_post
        ):
            try:
                main()
            except SystemExit as exc:
                return str(exc)
        raise AssertionError(f"main() accepted {argv!r}")

    with tempfile.TemporaryDirectory() as directory:
        image = Path(directory) / "a.png"
        Image.new("RGB", (4, 4)).save(image)
        for flag in ("--reply-to", "--embed-url", "--embed-ref", "--handle", "--password"):
            for blank in ("", " "):
                message = exit_message(["Reply", flag, blank])
                _check(f"{flag} was given an empty value" in message, message)
        # Blank embed flags are reported as such, not as missing content, and
        # an empty --embed-url cannot sneak past the one-media-source rule.
        for argv in (["", "--embed-url", ""], ["", "--embed-ref", ""],
                     ["Hi", "--image", str(image), "--embed-url", ""]):
            message = exit_message(argv)
            _check("was given an empty value" in message, message)
        # A blank --image is named as such, with or without credentials, and
        # never read as Path("") == "." (the current directory).
        for argv in (["hi", "--image", ""], ["hi", "--image", str(image), "--image", " "],
                     ["", "--image", "\t"]):
            for credentials in (None, {}):
                message = exit_message(argv, credentials)
                _check("--image was given an empty value" in message, message)
        # Credentials from the environment are never blamed on a flag, and a
        # blank variable means unset, as an empty one always did.
        for credentials in ({"ATP_AUTH_HANDLE": "   ", "ATP_AUTH_PASSWORD": "secret"},
                            {"ATP_AUTH_HANDLE": "alice.test", "ATP_AUTH_PASSWORD": " \t"},
                            {"ATP_AUTH_HANDLE": "", "ATP_AUTH_PASSWORD": ""}):
            message = exit_message(["Hi"], credentials)
            _check("Both handle and password are required" in message, message)
            _check("given an empty value" not in message, message)

    # A real --password still overrides the environment, with the ps warning;
    # the handle still comes from the environment, or from --handle over it.
    def posted_with(argv: List[str]) -> tuple[Dict[str, Any], str]:
        seen: Dict[str, Any] = {}
        environment = dict(os.environ, ATP_AUTH_HANDLE="alice.test", ATP_AUTH_PASSWORD="env")
        for name in ("ATP_PDS_HOST", "ATP_RECORD_SERVICE_HOST"):
            environment.pop(name, None)
        with _patched_attr(os, "environ", environment), _patched_attr(
            sys, "argv", ["create_bsky_post.py", "Hi", *argv]
        ), _patched_attr(sys, "stderr", io.StringIO()) as stderr, _patched_globals(
            create_post=lambda args: seen.update(handle=args.handle, password=args.password)
        ):
            main()
        return seen, stderr.getvalue()

    seen, warnings = posted_with(["--password", "flag"])
    _check_equal(seen, {"handle": "alice.test", "password": "flag"})
    _check("`ps`" in warnings, warnings)
    seen, warnings = posted_with(["--handle", "bob.test"])
    _check_equal(seen, {"handle": "bob.test", "password": "env"})
    _check("`ps`" not in warnings, warnings)


def test_pinned_adapter_failure_closes_the_session():
    """Building the TLS adapter can fail (an unreadable CA bundle, an
    interrupt); the Session made for that attempt must still be closed."""
    sessions: List[Any] = []

    class FakeSession:
        closed = False

        def __init__(self):
            sessions.append(self)

        def mount(self, *_args):
            raise AssertionError("mounted an adapter that failed to build")

        def close(self):
            self.closed = True

    for failure in (OSError("CA bundle unreadable"), KeyboardInterrupt()):
        def failing_adapter(*_args, **_kwargs):
            raise failure

        sessions.clear()
        with _patched_globals(
            _new_session=FakeSession,
            _public_url_addresses=lambda url: (urlparse(url), ["93.184.216.34"]),
            _PinnedHTTPSAdapter=failing_adapter,
        ), _check_raises(type(failure), f"expected {failure!r} to propagate"):
            with _open_pinned_response("https://example.com/", 5):
                pass
        _check_equal([session.closed for session in sessions], [True])


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

    # Only the validated reference is kept (and later printed): the rest of
    # the body, however deep, is dropped.
    nested: Any = 0
    for _ in range(900):
        nested = [nested]
    for extra in ({"validationStatus": "valid"}, {"commit": nested}):
        _check_equal(create(_FakeJsonResponse(200, dict(_created_ref(), **extra))),
                     _created_ref())
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


class _BrokenBodyResponse(_FakeRawResponse):
    """An error response whose body stream fails with a transport error."""

    def __init__(self, status_code: int, error: Exception):
        super().__init__(status_code, b"")
        self._error = error

    def iter_content(self, _chunk_size: int):
        yield b'{"error": "Inval'
        raise self._error


def test_broken_error_body_keeps_the_http_status():
    errors = (
        requests.exceptions.ChunkedEncodingError("truncated chunk"),
        requests.exceptions.ContentDecodingError("bad gzip"),
        requests.ConnectionError("reset mid-body"),
    )

    def forbidden_lookup(*_args):
        raise AssertionError("a definite rejection must not be looked up")

    for error in errors:
        # A definite 400 from createRecord is neither retried nor reported as
        # a post that may exist, even when its error body breaks off.
        with _patched_api_session(_BrokenBodyResponse(400, error)) as session, \
                _patched_attr(sys, "stderr", io.StringIO()) as stderr, \
                _patched_globals(_existing_post=forbidden_lookup), \
                _check_raises(XrpcError, f"expected HTTP 400 for {error!r}", contains="HTTP 400"):
            _publish_post("https://pds.example", "token", "did:plc:me", {}, verbose=False)
        _check_equal(len(session.calls), 1)
        _check("may still have been created" not in stderr.getvalue(), stderr.getvalue())
        _check(session.closed and session.response.closed, "error response left open")
        # A 404 from getRecord still means "not found".
        with _patched_api_session(_BrokenBodyResponse(404, error)):
            _check(_existing_post("https://pds.example", "did:plc:me", "3kabc") is None)


def test_timed_out_error_body_keeps_status_and_defers_close():
    release = threading.Event()
    reading = threading.Event()

    class SlowErrorBody(_FakeRawResponse):
        def iter_content(self, _chunk_size: int):
            reading.set()
            release.wait(5)
            yield b"{}"

    response = SlowErrorBody(400, b"")
    with _patched_api_session(response) as session, _check_raises(
        XrpcError, "expected the 400 to be reported", contains="HTTP 400"
    ):
        with _open_api_response(
            "GET", "https://pds.example/xrpc/test", timeout=0.2, operation="testing"
        ) as resp:
            _raise_for_xrpc(resp, "test")
    _check(reading.is_set(), "the body read never started")
    # The abandoned worker still reads the body, so the opener must not close
    # it even though the timeout was caught before reaching it.
    _check(not response.closed and not session.closed, "closed while still in use")
    release.set()
    _wait_until(lambda: response.closed and session.closed, "abandoned body cleanup")


@contextmanager
def _sigint_main_thread_when(started: threading.Event) -> Iterator[None]:
    """Deliver a real SIGINT to the main thread once `started` is set.

    A signal (unlike _thread.interrupt_main) wakes the main thread out of the
    blocking lock wait inside queue.get, so the KeyboardInterrupt lands exactly
    where a Ctrl-C during a network wait would.
    """
    main_ident = threading.main_thread().ident
    _check(threading.get_ident() == main_ident, "SIGINT tests need the main thread")
    _check(
        _signal.getsignal(_signal.SIGINT) is _signal.default_int_handler,
        "SIGINT tests need Python's default SIGINT handler",
    )

    def interrupt() -> None:
        if started.wait(5):
            _signal.pthread_kill(main_ident, _signal.SIGINT)

    thread = threading.Thread(target=interrupt, daemon=True)
    thread.start()
    try:
        yield
    finally:
        thread.join(5)


def test_interrupted_wait_hands_the_worker_off():
    started, release = threading.Event(), threading.Event()
    abandoned: List[bool] = []
    disposed: List[Any] = []

    def action() -> str:
        started.set()
        release.wait(5)
        return "late result"

    with _sigint_main_thread_when(started), _check_raises(
        KeyboardInterrupt, "expected the interrupt to propagate"
    ):
        _run_before_download_deadline(
            action, time.monotonic() + 30, "testing",
            dispose_abandoned=disposed.append,
            on_abandon=lambda: abandoned.append(True),
        )
    _check_equal(abandoned, [True])
    _check_equal(disposed, [])
    release.set()
    _wait_until(lambda: disposed == ["late result"], "late-result disposal")

    # An interrupt while thread.start() waits for the new thread to run (it
    # is already running by then) hands the worker off the same way.
    release.clear(), abandoned.clear(), disposed.clear()
    real_thread = threading.Thread

    class InterruptedStart(real_thread):
        def start(self) -> None:
            super().start()
            raise KeyboardInterrupt()

    with _patched_attr(threading, "Thread", InterruptedStart), _check_raises(
        KeyboardInterrupt, "expected the interrupt to propagate"
    ):
        _run_before_download_deadline(
            action, time.monotonic() + 30, "testing",
            dispose_abandoned=disposed.append,
            on_abandon=lambda: abandoned.append(True),
        )
    _check_equal(abandoned, [True])
    release.set()
    _wait_until(lambda: disposed == ["late result"], "late-result disposal")

    # A BaseException raised in the worker reaches the caller at once instead
    # of leaving it to wait out the whole deadline.
    class WorkerStop(BaseException):
        pass

    def stops() -> None:
        raise WorkerStop()

    begun = time.monotonic()
    with _check_raises(WorkerStop, "expected the worker's BaseException"):
        _run_before_download_deadline(stops, time.monotonic() + 30, "testing")
    _check(time.monotonic() - begun < 5, "caller waited for the deadline")


def test_interrupted_api_response_is_left_to_its_worker():
    for phase in ("request", "body"):
        started, release = threading.Event(), threading.Event()

        class SlowBody(_FakeRawResponse):
            def iter_content(self, _chunk_size: int):
                started.set()
                release.wait(5)
                yield b"{}"

        class SlowSession(_FakeApiSession):
            def request(self, method: str, url: str, **kwargs: Any) -> Any:
                started.set()
                release.wait(5)
                return self.response

        response = SlowBody(200, b"") if phase == "body" else _FakeJsonResponse(200, {})
        session = (_FakeApiSession if phase == "body" else SlowSession)(response)
        with _patched_globals(_new_session=lambda: session), \
                _sigint_main_thread_when(started), \
                _check_raises(KeyboardInterrupt, f"expected an interrupt ({phase})"):
            with _open_api_response(
                "GET", "https://pds.example/xrpc/test", timeout=30, operation="testing"
            ) as resp:
                _json_object(resp, "test")
        # The worker is still using both, so the unwind must not close them.
        _check(not session.closed, f"session closed while in use ({phase})")
        _check(not response.closed, f"response closed while in use ({phase})")
        release.set()
        _wait_until(lambda: session.closed and response.closed, f"{phase} cleanup")

    # An interrupt in the caller's own code, outside any read, still closes.
    response = _FakeJsonResponse(200, {})
    with _patched_api_session(response) as session, _check_raises(
        KeyboardInterrupt, "expected the interrupt to propagate"
    ):
        with _open_api_response(
            "GET", "https://pds.example/xrpc/test", timeout=30, operation="testing"
        ):
            raise KeyboardInterrupt()
    _check(session.closed and response.closed, "interrupted response left open")


def test_interrupted_pinned_response_is_left_to_its_worker():
    for phase in ("connect", "body"):
        started, release = threading.Event(), threading.Event()

        class SlowBody(_FakeRawResponse):
            def iter_content(self, _chunk_size: int):
                started.set()
                release.wait(5)
                yield b"{}"

        response = SlowBody(200, b"")

        class PinnedSession(_FakeApiSession):
            def mount(self, _prefix: str, _adapter: Any) -> None:
                pass

            def get(self, _url: str, **_kwargs: Any) -> Any:
                if phase == "connect":
                    started.set()
                    release.wait(5)
                return response

        session = PinnedSession(response)
        with _patched_globals(
            _public_url_addresses=lambda url: (urlparse(url), ["8.8.8.8"]),
            _new_session=lambda: session,
        ), _sigint_main_thread_when(started), _check_raises(
            KeyboardInterrupt, f"expected an interrupt ({phase})"
        ):
            with _network_deadline(30, subject="External download"):
                with _open_pinned_response("https://example.com/", 30) as resp:
                    _read_response_body(resp, 1024)
        _check(not session.closed, f"session closed while in use ({phase})")
        _check(not response.closed, f"response closed while in use ({phase})")
        release.set()
        _wait_until(lambda: session.closed and response.closed, f"{phase} cleanup")


@contextmanager
def _expired_outer_deadline() -> Iterator[None]:
    token = _DOWNLOAD_DEADLINE.set(time.monotonic() - 1)
    try:
        yield
    finally:
        _DOWNLOAD_DEADLINE.reset(token)


def test_api_socket_timeout_respects_the_outer_budget():
    response = _FakeJsonResponse(200, {})
    with _patched_api_session(response) as session:
        with _network_deadline(0.5, subject="Mention lookups"):
            with _open_api_response(
                "GET", "https://pds.example/xrpc/test", timeout=15, operation="testing"
            ):
                pass
    socket_timeout = session.calls[0][2]["timeout"]
    _check(0 < socket_timeout <= 0.5, f"socket timeout {socket_timeout} exceeds the budget")
    # With no outer budget, the caller's timeout is passed through unchanged.
    with _patched_api_session(_FakeJsonResponse(200, {})) as session:
        with _open_api_response(
            "GET", "https://pds.example/xrpc/test", timeout=15, operation="testing"
        ):
            pass
    _check_equal(session.calls[0][2]["timeout"], 15)


def test_expired_deadline_closes_the_unused_session():
    with _patched_api_session(_FakeJsonResponse(200, {})) as session, \
            _expired_outer_deadline(), \
            _check_raises(DeadlineExceeded, "expected an expired API deadline"):
        with _open_api_response(
            "GET", "https://pds.example/xrpc/test", timeout=15, operation="testing"
        ):
            raise AssertionError("no response should be yielded")
    _check_equal(session.calls, [])
    _check(session.closed, "unused API session left open")

    class PinnedSession(_FakeApiSession):
        def mount(self, _prefix: str, _adapter: Any) -> None:
            pass

        def get(self, _url: str, **_kwargs: Any) -> Any:
            raise AssertionError("no request may start after the deadline")

    session = PinnedSession(None)
    with _patched_globals(
        _public_url_addresses=lambda url: (urlparse(url), ["8.8.8.8"]),
        _new_session=lambda: session,
    ), _expired_outer_deadline(), _check_raises(
        DeadlineExceeded, "expected an expired download deadline"
    ):
        with _open_pinned_response("https://example.com/", 30):
            raise AssertionError("no response should be yielded")
    _check(session.closed, "unused pinned session left open")


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


def test_folded_special_prefixes_are_refused():
    for literal in ("192.88.99.1", "64:ff9b:1::808:808", "64:ff9b:1:ffff::1"):
        _check(not _is_public_unicast_address(ipaddress.ip_address(literal)),
               f"{literal} counted as public")
    # The well-known NAT64 /96 next to it is still judged by the IPv4 inside.
    _check(_is_public_unicast_address(ipaddress.ip_address("64:ff9b::808:808")),
           "NAT64-wrapped public IPv4 refused")


def test_resolve_handle_only_trusts_a_2xx_answer():
    did = "did:plc:abcdefghijklmnopqrstuvwx"
    for status in (200, 201):
        with _patched_api_session(_FakeJsonResponse(status, {"did": did})):
            _check_equal(_resolve_handle("https://pds.example", "alice.test"), did)
    # A redirect is never followed, and no error body is taken for an answer.
    for status in (301, 302, 304, 400, 401, 404, 500, 503):
        with _patched_api_session(_FakeJsonResponse(status, {"did": did})):
            _check(_resolve_handle("https://pds.example", "alice.test") is None,
                   f"HTTP {status} resolved the handle")


def test_reply_root_is_checked_before_use():
    test_cid = "bafyreiaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    parent_uri = "at://did:plc:parent/app.bsky.feed.post/parent"

    def run(root: Dict, root_record: Optional[Dict] = None) -> Dict:
        def fake_get_record(_url: str, uri: str) -> Dict:
            if uri == parent_uri:
                return {"uri": uri, "cid": test_cid, "value": {
                    "$type": "app.bsky.feed.post", "reply": {"root": root}}}
            if root_record is not None and uri == root.get("uri"):
                return root_record
            raise AssertionError(f"unexpected get_record call: {uri}")

        with _patched_globals(get_record=fake_get_record):
            return get_reply_refs("https://pds.example", parent_uri)

    like_uri = "at://did:plc:root/app.bsky.feed.like/root"
    with _check_raises(ValueError, "a non-post reply root was accepted",
                       contains="app.bsky.feed.post"):
        run({"uri": like_uri, "cid": test_cid})
    root_uri = "at://did:plc:root/app.bsky.feed.post/root"
    with _check_raises(ValueError, "an invalid root CID was accepted", contains="CID"):
        run({"uri": root_uri, "cid": "bafyrei"})
    # Without a CID the root is fetched, and must be a post record itself.
    fetched = {"uri": root_uri, "cid": test_cid, "value": {"$type": "app.bsky.feed.like"}}
    with _check_raises(ValueError, "a fetched non-post root was accepted",
                       contains="Reply root is not an app.bsky.feed.post record"):
        run({"uri": root_uri}, fetched)
    fetched["value"] = {"$type": "app.bsky.feed.post"}
    _check_equal(run({"uri": root_uri}, fetched)["root"], {"uri": root_uri, "cid": test_cid})


def test_tag_refuses_enclosing_keycap_and_leading_variation_selector():
    _check(not _valid_tag_value("a⃢"), "U+20E2 accepted in a tag")
    _check(not _valid_tag_value("️a"), "leading U+FE0F accepted in a tag")
    _check(_valid_tag_value("❤️love"), "a later U+FE0F refused")


def test_record_cid_refuses_nonzero_trailing_bits():
    raw = b"\x01\x71\x12\x20" + bytes(range(32))
    cid = "b" + base64.b32encode(raw).decode("ASCII").lower().rstrip("=")
    _check(_is_valid_record_cid(cid), "a canonical CID was refused")
    alphabet = "abcdefghijklmnopqrstuvwxyz234567"
    # 36 bytes fill 58 characters but for 2 bits, which must be zero.
    last = alphabet.index(cid[-1])
    _check_equal(last & 3, 0)
    for padding_bits in (1, 2, 3):
        altered = cid[:-1] + alphabet[last | padding_bits]
        _check(not _is_valid_record_cid(altered), f"{altered} was accepted")


def test_image_worker_reply_must_be_an_object():
    replies = iter([b"[]", b'"x"', b"null"])

    def worker_replying(*_args, stdout, **_kwargs):
        stdout.write(next(replies))
        return type("Completed", (), {"returncode": 0})()

    with _patched_attr(subprocess, "run", worker_replying):
        for _ in range(3):
            with _check_raises(ValueError, "a non-object reply was accepted",
                               contains="Image decoder returned an unexpected JSON shape"):
                _inspect_images_in_worker([b"x"])


def test_image_worker_import_paths_leave_out_the_working_directory():
    commands = []

    def recording_run(command, **_kwargs):
        commands.append(command)
        return type("Completed", (), {"returncode": 1})()

    elsewhere = str(Path(tempfile.gettempdir()).resolve())
    with _patched_attr(sys, "path", ["", os.getcwd(), ".", elsewhere]), \
            _patched_attr(subprocess, "run", recording_run), \
            _check_raises(ValueError, "expected the failed worker to be reported"):
        _inspect_images_in_worker([b"x"])
    _check_equal(json.loads(commands[0][-1]), [elsewhere])


def test_download_deadline_runner_abandonment_paths():
    # Deadline already gone: nothing starts, the caller is told it owns
    # nothing, and the disposal still runs (with no result).
    events = []
    with _check_raises(DeadlineExceeded, "expected an expired deadline to fail"):
        _run_before_download_deadline(
            lambda: events.append("ran"), time.monotonic() - 1, "testing",
            dispose_abandoned=lambda value: events.append(("dispose", value)),
            on_abandon=lambda: events.append("abandoned"),
        )
    _check_equal(events, ["abandoned", ("dispose", None)])

    # A worker that fails after being abandoned hands over None, not its error.
    release = threading.Event()
    disposed = queue.Queue()

    def failing_late():
        release.wait(5)
        raise RuntimeError("late failure")

    with _check_raises(DeadlineExceeded, "expected the deadline to pass"):
        _run_before_download_deadline(failing_late, time.monotonic() + 0.05, "testing",
                                      dispose_abandoned=disposed.put)
    release.set()
    _check(disposed.get(timeout=5) is None, "the late error was disposed of")

    # Interrupted after the worker already finished: its result is disposed of
    # by the caller, since no worker is left to do it.
    disposed = queue.Queue()
    real_remaining = bsky._remaining_download_time
    calls = []

    def interrupting_remaining(deadline, operation):
        calls.append(operation)
        if len(calls) == 2:  # the wait itself, after the worker started
            for _ in range(500):
                if not any(t.name == "bsky-download-operation" and t.is_alive()
                           for t in threading.enumerate()):
                    break
                time.sleep(0.01)
            raise KeyboardInterrupt
        return real_remaining(deadline, operation)

    with _patched_globals(_remaining_download_time=interrupting_remaining), \
            _check_raises(KeyboardInterrupt, "expected the interrupt to propagate"):
        _run_before_download_deadline(lambda: "result", time.monotonic() + 5, "testing",
                                      dispose_abandoned=disposed.put)
    _check_equal(disposed.get(timeout=5), "result")


def test_failed_attempt_past_the_deadline_is_a_timeout():
    """A connect error that lands after the deadline reports the deadline."""
    clock = {"skew": 0.0}
    real_monotonic = time.monotonic

    class FailingSession:
        headers: Dict[str, str] = {}
        trust_env = True

        def mount(self, *_args):
            pass

        def get(self, *_args, **_kwargs):
            raise requests.ConnectionError("refused")

        def close(self):
            # Closed only once the failure is back in the calling thread.
            clock["skew"] = 1000.0

    with _patched_globals(
        _public_url_addresses=lambda url: (urlparse(url), ["8.8.8.8"]),
        _NoRedirectSession=FailingSession,
    ), _patched_attr(time, "monotonic", lambda: real_monotonic() + clock["skew"]), \
            _check_raises(DeadlineExceeded, "expected a deadline error",
                          contains="timed out while connecting"):
        with _network_deadline(30, subject="External download"):
            with _open_pinned_response("http://example.com/", timeout=9):
                pass


def test_identifier_length_caps_are_the_hostname_cap():
    def name(length: int) -> str:
        labels = []
        while sum(len(label) + 1 for label in labels) < length:
            labels.append("a" * 60)
        joined = ".".join(labels)[:length - 4]
        return joined.rstrip(".") + ".com"

    for length in (MAX_HOSTNAME_LENGTH, MAX_HOSTNAME_LENGTH + 1):
        handle = name(length)
        _check_equal(len(handle), length)
        fits = length <= MAX_HOSTNAME_LENGTH
        _check_equal(bool(parse_mentions(f"hi @{handle}")), fits)
        uri = f"at://{handle}/app.bsky.feed.post/3k"
        if fits:
            parse_uri(uri)
        else:
            with _check_raises(ValueError, f"a {length}-character handle was accepted"):
                parse_uri(uri)
        nsid = f"{'.'.join(reversed(handle.split('.')))}.x"
        if fits:
            _validate_nsid(nsid)
        else:
            with _check_raises(ValueError, f"a {length}-character NSID authority passed"):
                _validate_nsid(nsid)


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
