# Posting via the Bluesky API — the security-hardened edition

A guide to creating posts via the Bluesky API, including rich-text facets
(mentions, links, hashtags, cashtags), replies, quote posts, image embeds,
and website cards — with the safety rails a script needs when it fetches
untrusted content from the open web.

*July 23, 2026 — updated October 21, 2026*

This post is an updated companion to the AT Protocol team's original
[Posting via the Bluesky API](https://atproto.com/blog/create-post)
(August 2023) and its accompanying `create_bsky_post.py` cookbook script.
The script described here is a **security-hardened fork** of that original:

* Original: <https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py>
* Fork: <https://github.com/CleasbyCode/cookbook/blob/main/python-bsky-post/create_bsky_post.py>
* Fork (gist): <https://gist.github.com/CleasbyCode/1eb678ca1fa1975b1c1e20aeec33637e>

The fork keeps the spirit of the original — a single standalone Python file
that shows what's really going on behind the SDK abstractions — but brings it
up to date with current lexicon limits (including the [April 2026 increase of
the image size limit to 2 MB](https://github.com/bluesky-social/atproto/discussions/4832)),
adds hashtag/cashtag facets, per-image alt text, aspect ratios, language tags,
and record-with-media embeds, and treats every network fetch as potentially
hostile.

It requires Python 3.11+ with `requests`, `pillow` and `chardet`.
`requirements.txt` pins these packages and also `urllib3` and `idna`, which
are transitive dependencies of `requests` used directly by the SSRF/TLS layer:

```bash
python3 -m pip install -r requirements.txt
```

These are selected version pins, not a complete dependency lock.
`certifi` and `charset-normalizer` are
resolved within their dependencies' allowed ranges and may change between
installs. Fully repeatable installs also require locking transitive versions
and recording the target Python/platform environment; see
[pip's repeatable-install guidance](https://pip.pypa.io/en/stable/topics/repeatable-installs/).

---

## Getting Started

You'll need a Bluesky account and an **app password**. Create one at
<https://bsky.app/settings/app-passwords> — do **not** use your main account
password. App passwords can be revoked individually and cannot change your
authentication settings, though they still grant access to publish and manage
account content.

Set your credentials as environment variables. In an interactive Bash shell,
read the password at a hidden prompt so its value is not typed into a command
that shell history can retain. Command-line passwords are visible to other
local users via `ps`; the script warns you if you pass `--password`:

```bash
export ATP_AUTH_HANDLE='your-handle.bsky.social'
read -r -s -p 'Bluesky app password: ' ATP_AUTH_PASSWORD
printf '\n'
export ATP_AUTH_PASSWORD
```

The environment variable remains available to processes launched from this
shell. Run `unset ATP_AUTH_PASSWORD` when you finish posting.

Then posting is a one-liner:

```bash
python3 create_bsky_post.py "Hello, Bluesky! #greetings"
```

Some more examples:

```bash
# an image with alt text (up to 4 images per post)
python3 create_bsky_post.py "Sunset over the bay" \
    --image sunset.jpg --alt-text "Orange sunset over a calm bay"

# a website card
python3 create_bsky_post.py "Worth a read" --embed-url "https://example.com/article"

# a reply
python3 create_bsky_post.py "Replying" \
    --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"

# a quote post -- bsky.app URLs work too
python3 create_bsky_post.py "Quoting this post" \
    --embed-ref "https://bsky.app/profile/example.com/post/yyy"

# a quote post with attached media (record-with-media)
python3 create_bsky_post.py "Quoted with media" \
    --embed-ref "at://did:plc:xxx/app.bsky.feed.post/yyy" --image photo.jpg

# multilingual post
python3 create_bsky_post.py "สวัสดีชาวโลก! Hello World!" --lang th --lang en-US
```

The rest of this post walks through how each piece works, and — where the
fork differs from the original — why.

## Authentication

Posting requires a session. The script calls
`com.atproto.server.createSession` with your handle and app password and
receives an access token (`accessJwt`), a refresh token (`refreshJwt`), and
your account DID. Since the script publishes a single post, it never refreshes
the session. Instead, on normal cleanup, whether the post succeeded or not, it
attempts to revoke the session with `com.atproto.server.deleteSession`. This
is best effort: if revocation fails, the script warns, and the session may stay
valid until it expires. A forcibly killed run skips the attempt entirely.

```python
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
```

Four hardening details already show up here. Redirects are disabled and
explicitly refused: a misconfigured or malicious endpoint must never be able
to bounce a request carrying your password somewhere else. The response is
read through a size-capped, deadline-bounded reader rather than trusted
blindly, and what it returns is checked: a blank, malformed or over-long
(16 KiB) access or refresh token, or a `did` that is not a DID, is refused
(a token with a line break would otherwise end up quoted, in full, in the
error Requests raises for the header it cannot send). That DID is the repo
the post is created in: the `createRecord` answer, and the `getRecord` lookup
that recovers from an interrupted one, are checked against it (reply and quote
targets are checked against their own DIDs). `_is_did` measures the value against the
AT Protocol DID specification's 2,048-character (2 KiB) limit before matching it, so a hostile server cannot
make the check scan a multi-megabyte string. And `_raise_for_xrpc`, which every API call goes through, reads the
XRPC error body instead of raising a bare `401 Client Error`. Only a 2xx
counts as success: Requests' `resp.ok` would also pass a 1xx such as
`101 Switching Protocols`, which `http.client` hands back as the final status,
and anything from 600 up. Login is where a
first-time user is most likely to get stuck and the status code alone never
says why, so it also takes a table of remedies keyed by error name. The output
is two lines, the server's error and then the hint (wrapped here for width):

```
Error: Login failed with HTTP 401 (AuthFactorTokenRequired: A sign in code has
    been sent to your email address).
This account has email two-factor authentication enabled, which the account
    password cannot bypass. Use an APP password instead, created at
    https://bsky.app/settings/app-passwords.
```

The hint is looked up from the error name for the handful of cases with an
actionable fix (2FA, a revoked password, rate limiting, a takedown); anything
else just reports what the server said. The script does not deliberately add
your password to diagnostics, but it does not redact secrets from server
responses. A PDS that echoes credentials in its error message can therefore
cause them to appear in the output. Terminal escaping prevents control-sequence
execution; it does not make diagnostic output safe to share without checking
it for secrets.

The PDS defaults to `https://bsky.social` and can be pointed elsewhere with
`--pds-url` or the `ATP_PDS_HOST` environment variable (the record lookup
service has the same pairing: `--record-service-url` / `ATP_RECORD_SERVICE_HOST`).
Either way the URL must be HTTPS and must be a bare scheme and host, with no
path, query, or fragment. Plain HTTP is only allowed with
`--allow-insecure-pds`, and even then only for `localhost` itself or a
loopback IP literal, so the flag is useful for local development but can't be
abused to send credentials in cleartext across a network. Nothing else counts:
`127.0.0.1.`, with its trailing dot, is no IP literal to `getaddrinfo` but a
name to look up, as are `localhost.` and `pds.localhost`, and a lookup can
reach DNS. The URL that is returned is rebuilt from the host that was checked,
so a host written with look-alike dots (`127。0。0。1`) is used in the same
form it was checked in.

Service URLs are a deliberately different trust boundary from everything in
the next section: they're operator-supplied, never chosen by the content
you're posting, so they are not subject to the public-address SSRF check.
Aiming one at a private address is a local-testing decision, not something a
hostile web page can arrange.

## Post Record Structure

A minimal post record is unchanged from the original guide:

```json
{
  "$type": "app.bsky.feed.post",
  "text": "Hello World!",
  "createdAt": "2026-07-23T17:00:00.000000Z"
}
```

The script builds it like this, using a timezone-aware UTC timestamp with the
preferred trailing `Z`:

```python
def _created_at_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")
```

The required fields inside `_build_post_record()` are:

```python
post: Dict = {
    "$type": "app.bsky.feed.post",
    "text": args.text,
    "createdAt": _created_at_now(),
}
```

The finished record goes to `com.atproto.repo.createRecord`, and the response
contains the new post's AT URI and CID. Unlike the original, the script picks
the record key itself: a fresh TID (a timestamp-based identifier, the same kind
the PDS would otherwise generate). Why that matters is covered under
[the security layer](#the-security-layer).

The script validates the text length against the lexicon's 3,000-byte
`maxLength` before posting. The separate 300-*grapheme* limit is left to the
PDS to enforce, because Python's standard library has no extended grapheme
cluster segmentation — a theme that will come up again with card text.

## Setting the Post's Language

Languages are passed with repeatable `--lang` flags (at most 3, per the
lexicon) and stored in the `langs` field:

```json
{ "langs": ["th", "en-US"] }
```

Where the original script accepted any string, the fork validates each tag
against the actual BCP 47 grammar — language, extlang, script, region,
variants, extensions, private-use subtags, and the grandfathered tags like
`i-klingon` — so a malformed tag like `--lang en--US` fails locally with a
clear error instead of producing a malformed record.

Note that the grammar is deliberately more permissive than the IANA registry:
a 4–8 letter primary subtag is reserved or registered rather than illegal, so
`--lang english` is *syntactically* valid and passes. Validating tags against
the registry itself is out of scope for a single-file script.

## Rich-Text Facets

Facets are annotations over byte ranges of the post text. The fork produces
three kinds: **links**, **mentions**, and **tags** (hashtags and cashtags).

### Byte offsets, computed once

Facet indices are *byte* offsets into the UTF-8 encoding of the text, not
character offsets. The original worked around this by running regexes over
encoded bytes. The fork instead matches on the decoded string (where Unicode
categories are available) and converts with a precomputed offset table.
`parse_facets()` builds the table once and shares it with the internal link
scanner `_scan_urls()` and with `parse_mentions()` and `parse_hashtags()`;
the public `parse_urls()`, `parse_mentions()` and `parse_hashtags()` each
build their own when called directly:

```python
def _byte_offsets(text: str) -> List[int]:
    offsets = [0]
    for character in text:
        offsets.append(offsets[-1] + len(character.encode("UTF-8")))
    return offsets
```

### Mentions

Mentions are matched with a handle regex based on the
[handle syntax spec](https://atproto.com/specs/handle), then each candidate
is resolved to a DID via `com.atproto.identity.resolveHandle`. If a handle
doesn't resolve, it's skipped and simply renders as plain text, with a warning
naming it. The original skipped only a handle the server rejected with HTTP 400;
it set no timeout, so a stalled lookup hung it, and any other error stopped it.
The fork skips a handle whatever the failure, timeouts included, and adds:

* a Unicode-aware word-boundary check, so `email@example.com` is not treated
  as a mention of `example.com`. Invisible BiDi marks and a stray BOM are
  looked through (an RTL post can put U+200E right before `@`), a zero-width
  space counts as a break, and joiners (ZWJ, ZWNJ, soft hyphen, word joiner)
  still glue the two sides into one word; links use the same check,
* a 253-character handle length cap and strict `HANDLE_REGEX` fullmatch,
  applied to the whole candidate after `@` (only trailing dots are trimmed),
  so an invalid handle such as `@alice.bsky.social-` or
  `@alice.com@bob.com` is left as plain text instead of being shortened to a
  different handle (`alice.bsky`, `alice.com`); the same word-boundary check
  applies after the candidate,
* case-insensitive de-duplication, so the same handle mentioned twice is
  resolved once,
* concurrent lookups (up to 8 at a time) sharing one 20-second overall budget,
  so a post full of unresolvable handles cannot stall for minutes,
* validation that the returned DID actually looks like a DID.

### Links

URLs are matched, then trailing punctuation is trimmed the way people
actually write: `https://bsky.app.` drops the final period, typographic
quotes and non-ASCII closing brackets (`“https://example.com”`,
`「https://example.com」`) are dropped, and bracket trimming is
balance-aware: a trailing `)` stays only when it closes a `(` opened earlier
in the URL, so `(https://example.com/a_(b))` keeps the parenthesis that
belongs to the URL and drops the one that doesn't, and
`https://example.com/a)_(b)` keeps its last one. Every candidate is finally
re-parsed and must be a well-formed `http(s)` URL with no embedded
credentials, no empty host label (`https://.../x`), and a port from 1 to
65535 before it becomes a facet.

### Hashtags and cashtags

New in the fork. Hashtags follow the official composer's rules: they support
the fullwidth `＃` as well as `#`, end at whitespace or at an invisible
character such as a zero-width space or joiner (so `#foo` followed by a
zero-width space and `bar` tags `foo`), may not start with the emoji
variation selector U+FE0F, strip trailing punctuation, and are rejected when made only of ASCII
digits and punctuation. They also enforce the 64-grapheme / 640-byte lexicon
limits with a conservative count. Nonspacing and enclosing combining marks
extend the preceding cluster, but spacing marks are counted separately: some,
including Myanmar U+102B, begin new graphemes. Tags that exceed the bound remain
plain text; their presence does not invalidate the post. Cashtags like `$TSLA`
become tags too:

```python
HASHTAG_REGEX = re.compile(
    r"(^|\s)([#＃])(?!️)([^\s­⁠ ​‌‍⃢]+)"
)
CASHTAG_REGEX = re.compile(
    r"(^|\s|\()\$([A-Za-z][A-Za-z0-9]{0,4})"
    r"(?=\s|$|[.,;:!?)\"'\u2019])"
)
```

### No overlapping facets

The three parsers can disagree — `https://example.com/@alice.test/#topic`
contains something that looks like a mention and something that looks like a
hashtag, all inside a URL. The fork resolves conflicts in two steps. One scan
for URLs finds every URL-shaped run of text, and nothing inside one becomes a
mention or tag, even when that text is rejected as a link: so
`https://example.com/50%/@alice.bsky.social` stays plain text rather than
turning into a mention of `alice.bsky.social`. (Links themselves never
overlap: each lies inside its own run.) As a bonus, mentions inside URLs are
never even sent to `resolveHandle`. Then mentions, and after them tags,
reserve their byte ranges, and any span overlapping an already-reserved
range is dropped. Hashtags and cashtags never overlap each other either: in
`#foo($TSLA)` the hashtag wins.

## Replies

A reply must reference both the immediate **parent** post and the thread's
**root** post, each as a strong ref (`uri` + `cid`). The logic is the same as
the original — fetch the parent via `com.atproto.repo.getRecord`; if the
parent is itself a reply, reuse its root ref, otherwise the parent *is* the
root — with several correctness fixes layered on:

* record lookups go to a network-wide record service
  (`https://public.api.bsky.app` by default, configurable with
  `--record-service-url`), not your own PDS, so you can reply to posts hosted
  on any PDS in the network. That service is not authenticated, so it is only
  ever asked for a record by DID and its answer must name that exact record: a
  handle in `--reply-to` or `--embed-ref` (a bsky.app link names one) is first
  resolved to a DID by your own PDS, and every stored reference, including a
  parent's `reply.root`, must be an `at://` URI with a DID, since a handle can
  later be pointed at another account;
* if the parent's `reply.root` ref already contains a CID, it is reused
  instead of re-fetched;
* every URI is validated as a real `app.bsky.feed.post` reference, and every
  CID is checked to be a canonical base32 CIDv1 (dag-cbor, SHA-256) — the
  fork round-trips the decode/re-encode rather than pattern-matching;
* `getRecord` responses are size-capped and shape-checked before use, and
  the returned record must be exactly the one requested: same DID,
  collection and record key. Every lookup names its repository by DID (a
  handle is resolved by your own PDS first), so the answer's URI is compared
  as a string with the request's.

## Quote Posts and Record-with-Media

`--embed-ref` embeds a strong reference to another record
(`app.bsky.embed.record`). You can pass an `at://` URI for any record, or a
`https://bsky.app/profile/…` URL for a post, a list (`app.bsky.graph.list`),
or a feed generator (`app.bsky.feed.generator`); the script maps the web path
to the right collection. A handle in a `bsky.app` URL may be written in any
case, since handles are case-insensitive; a DID must be written exactly, with a
lowercase `did:`, and is never case-folded. An `at://` URI must be written in
its plain canonical form, without even an empty `?` or `#`: one naming a DID
is copied verbatim into the post, and one naming a handle is rewritten with
the DID your PDS resolves that handle to before the record is looked up.

If you combine `--embed-ref` with `--image` or `--embed-url`, the script
produces the `app.bsky.embed.recordWithMedia` embed (one member of the post's
`embed` union) that the original never
supported. This schematic shows the wrapper; the quoted placeholders stand
for a resolved record URI and CID, and the empty image array must be populated
with the uploaded image entries described below before posting:

```json
{
  "$type": "app.bsky.embed.recordWithMedia",
  "record": { "$type": "app.bsky.embed.record", "record": { "uri": "…", "cid": "…" } },
  "media": { "$type": "app.bsky.embed.images", "images": [] }
}
```

## Image Embeds

Each post can carry up to four images. Since April 2026 each image blob may
be up to **2,000,000 bytes**, raised from the 1 MB limit that applied when the
original blog post was written
([announcement](https://github.com/bluesky-social/atproto/discussions/4832)).
That limit is part of the lexicon: the
`app.bsky.embed.images` schema declares `maxSize: 2000000` on the image blob,
and the script enforces it locally so oversized files fail fast with a clear
message instead of being rejected when the post is created. The same
announcement raised the maximum resolution (4000×4000, up from 2000×2000).
That one is a service rule rather than a lexicon constraint, so treat it as "as
announced in April 2026"; the script does not depend on it.

Its own dimension caps (16,384px per side, 40 megapixels) are deliberately
*looser* than the service's — they are local decompression-bomb guards, not a
mirror of the server's rules, and they exist to bound Pillow's memory use
before the service ever sees the file.

Files are read defensively: opened with `O_NOFOLLOW` (with an lstat/fstat
identity check on platforms that lack it) and `O_NOCTTY`, and required to be
regular files.
So a path whose final component is a symlink, or a device such as
`/dev/stdin`, is refused. Symlinked *directories* earlier in the path are still
followed; the check guards the file itself, not every component leading to it.

All local images supplied with `--image` are read and validated before the
script logs in, so a
typo in the fourth filename fails the command before any network request is
made and before a single blob is uploaded. Each image first passes a format
check in the parent process. WebP RIFF canvas dimensions are checked here too,
because its native decoder can allocate canvas buffers during `Image.open()`,
before Pillow returns dimensions to the caller. Then the whole batch is decoded
in one worker process:

```python
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
```

The worker fully decodes each image. Where Pillow's plugin has its own
`verify()` (PNG, whose data-chunk CRCs decoding skips), that pass runs first on
a separate open. For JPEG, GIF and WebP `verify()` checks nothing, so those
are opened once and the full decode does the checking; JPEG `verify()` would
not reject truncated pixel data anyway. The parent kills and
reaps the worker if it runs longer than 15 seconds for interpreter start-up
plus five seconds per image. Where Python's `resource` module is available, the
worker also installs a 768 MiB address-space limit, a matching CPU-time limit,
a 16 KiB output-file limit and a zero core-dump size before restoring
`sys.path` and importing Pillow. The output-file limit does not cover core dumps, so without the last
one a crash in a native decoder could write hundreds of megabytes, image
bytes included, to disk or to a crash handler. Each is lowered to
any stricter limit the script already runs under, never raised. These limits
are enforced on Linux. macOS has no enforced address-space limit (its
`RLIMIT_RSS` is a compatibility alias of `RLIMIT_AS`, which XNU does not enforce and which refuses a finite
value), so the worker skips that one there and installs the others. The
worker receives image bytes without the parent's posting credentials, and
reads them one image at a time, so only the image being decoded counts against
its address-space limit, not the whole batch. It runs
only the decoding code, taken from this script's own definitions, rather than
the whole script: it never imports Requests, TLS, or the HTML parser, which
cuts its start-up from about 215 ms to about 65 ms and leaves roughly 20 MiB
more of its address-space limit to the decoder. Of Pillow's format plugins it
loads only the one for the image's detected format (plus the few Pillow always
registers). Asking Pillow for WebP would otherwise make it import every plugin
it ships, about 40 extra file parsers, and triple the decode time of a small
image. Animation is limited to 100 frames and 80 million total
decoded pixels. Both limits apply before any frame is decoded: a GIF's block
structure is walked only until its 101st image, before the worker starts,
and canvas size times frame count, a lower bound on the decoded pixels,
must fit the pixel budget. The decode loop still counts each frame's real
size, since Pillow can grow a GIF canvas to fit a frame. That growth happens
inside `seek()`, before the loop can look at the new size, so the worker also
lowers Pillow's own decompression-bomb limit from its default of about 89.5
million pixels to the script's 40 million. Otherwise a 72-byte GIF with a 1×1
canvas could make `seek(1)` allocate a 16,385×4,000 canvas, about 250 MiB.
The parent treats whatever the worker replies as untrusted: a reply that is
not JSON, or that nests deeply enough to exhaust Python's recursion limit,
is just an invalid image. Image bytes are never re-encoded or rewritten during
validation.

This provides:

* the actual format (PNG/JPEG/WebP/GIF), so the blob is uploaded with the
  correct MIME type instead of one guessed from the file extension;
* display dimensions accounting for EXIF orientations 5–8, published as the
  `aspectRatio` field so clients can lay out the image before it loads;
* protection against oversized canvases, excessive decoding work, and
  truncated image data.

Alt text is supplied per image with repeatable `--alt-text` flags (the count
must match `--image`), and the `alt` field is always present — an empty
string when no alt text is given, as the lexicon requires. Each string is
capped at 2,000 characters; the lexicon itself sets no `maxLength` on `alt`,
so this simply mirrors the official composer.

**Your image's metadata is published with it.** Because the script uploads
the file byte for byte, any EXIF metadata in it, including GPS location and
camera details, becomes part of the public blob. The Bluesky app displays
re-encoded copies, but the original blob can be downloaded by anyone through
`com.atproto.sync.getBlob`. As with the original script, stripping metadata is
up to you: remove it before posting if it could identify you or your location.
Bear in mind that blanket metadata strippers also remove any data you have
deliberately embedded in those segments.

The upload itself is unchanged in principle: bytes go to
`com.atproto.repo.uploadBlob`, and the returned `blob` object is embedded in
the post's `app.bsky.embed.images` array. The answer is checked first: its
`ref` must be the CID of exactly the bytes sent (CIDv1, raw codec, SHA-256),
and its `mimeType` and `size` must be the ones uploaded, so the PDS cannot
substitute another blob or relabel this one; and only those four fields are
copied into the post, never anything else the answer carries. With several images the uploads run
concurrently, and the array keeps the order the images were given in.

## Website Card Embeds

`--embed-url` builds a "social card": the script downloads the page, parses
the Open Graph tags (`og:title`, `og:description`, falling back to
`<title>` and the `description` meta tag), optionally downloads and uploads
the `og:image` as a thumbnail, and embeds the result as
`app.bsky.embed.external`.

This is the part of the original script that most needed hardening, because
it is the one place where the script fetches **attacker-influenced URLs**:
the page you point it at chooses where redirects go and what `og:image`
points to. The fork's changes:

* relative `og:image` URLs are resolved with a proper `urljoin` against the
  page's **final** URL after redirects (the original naively concatenated
  strings against the original URL), and an HTTPS page's `og:image` must be
  HTTPS too, as a redirect must: fetched over plain HTTP, anyone on the path
  could pick the image that is uploaded and published under your account;
* the page's HTML is decoded as browsers decode it: a UTF-8 or UTF-16
  byte-order mark is definitive; otherwise the charset the server declares in
  its `Content-Type` header (parsed with the header's quoting rules, so a
  `charset=` inside another parameter's quoted value is ignored, and a value
  over 1,024 characters, which no real one approaches, is not parsed at all:
  the parser is quadratic in it, and repeated `Content-Type` headers arrive
  joined into one value of megabytes), then an
  in-document `<meta charset>` (a UTF-16 or UTF-32 label there is ignored, as
  in browsers, recognised by the codec it names so aliases such as `utf16`
  or `u16` count too), then UTF-8, and only then a statistical guess. A
  server charset that decodes the head strictly settles it at once, before
  the head is tokenized for a `<meta>`. The `<meta>` is
  found by tokenizing the first 64 KiB, not by the usual regex search (as
  in BeautifulSoup): those regexes backtrack on hostile markup (cubically,
  for a crafted XML declaration: minutes for 64 KiB), and a regex holds the
  interpreter lock, so no deadline could stop it. The guess tries an XML
  declaration's `encoding=` on the first line (found by a linear scan that
  gives the same answer as BeautifulSoup's regex), then `chardet` (or, where
  it is missing, `charset-normalizer`, which Requests already depends on and
  which misreads East Asian pages) over the first 64 KiB. A guess made with
  less than 0.2 confidence is ignored: chardet 7 still answers for a few
  bytes, and reads a French windows-1252 page as MacCyrillic at about 0.1,
  so such a page falls back to windows-1252, the web's default. The first of
  these that decodes the whole head strictly wins, so a wrong label falls
  through to the next rather than producing mojibake; failing that, a label
  that still mostly decodes (one stray byte in a comment, say) is used with
  replacement characters before any guess is made. The guess is checked
  against the whole head, never against the 64 KiB it was made from, whose
  last multi-byte character the cut may have split. A label must name a real character set:
  Python also has codecs such as `zlib`, `bz2` and `base64` that transform
  bytes into bytes, and text codecs such as `idna` and `unicode_escape` that are no
  page encoding, so `charset=zlib` would otherwise inflate a small head
  without bound or crash the post; such labels, and UTF-7 (as in browsers),
  are skipped. The guess only ever looks at the first
  64 KiB: run over a whole 1 MB head it took about three seconds of the parse
  budget, and now takes under 0.2 s. A page served in a non-UTF-8 encoding
  therefore still yields correct card text;
* a page that declares a non-HTML `Content-Type` (a PDF, a video) is refused
  before its body is downloaded; one that declares no type is still read;
* only the page's `<head>` is downloaded and parsed (everything before the
  first real `</head>` end tag or `<body>` start tag, whichever comes first),
  since that is where the metadata lives. The page is read as it streams in
  and the download stops as soon as the head is complete, so a page of any
  size costs only its head; a 3 MB article with an ordinary head transfers
  tens of kilobytes. The boundary is found by tokenizing the markup, so the
  same text inside a comment, an attribute value, or a `<script>`, `<style>`
  or `<title>` neither cuts the head short nor lets `og:` tags from the body
  in. The tokenizer runs once, as the page streams in, and the byte offset of
  the boundary is tracked as it goes rather than recounted from line numbers.
  A chunked response can arrive a few bytes at a time, and `html.parser`
  rescans an unfinished `<script>`, comment or tag on every feed, so pieces
  are passed on in batches at least as large as what it holds back: ordinary
  markup still goes through as it arrives, and a 1 MB script sent 20 bytes at
  a time costs about 20 ms instead of outlasting the download deadline.
  When neither tag is found in the first 1 MB, as with UTF-16 pages, the
  download stops there and that leading 1 MB is parsed instead;
* the head is then read in one streaming pass that builds nothing: each
  `<meta>` is read as it is tokenized and forgotten, and only the first
  `<title>`'s text is kept, as raw text with character references, the way
  browsers read it (markup inside a title is part of the title). Its
  character references are decoded exactly once: by the script on Python
  3.11 to 3.13, and by `html.parser` itself from 3.14, which reads `<title>`
  the same way. A tree
  builder held every element, and a head of 333,000 `<a>` tags inside a
  `<title>` cost about 190 MB of memory in the posting process. A start tag
  longer than 64 KiB (65,536 characters), which no real page writes, ends the head where it
  starts: `html.parser`'s regex for a tag's end holds about 200 bytes per
  attribute until it finishes, so the end is found one attribute at a time
  instead, in constant memory, and the scan gives up past that length;
* the thumbnail download is capped at 1 MB, which is also the lexicon's
  `maxSize` for `app.bsky.embed.external` thumbnails;
* `og:` properties are matched case-insensitively, since real pages do emit
  `property="OG:Title"`, and their values are trimmed, so an `og:image`
  padded with newlines still loads and a blank `og:title` falls back to
  `<title>`. A blank duplicate never hides a later non-blank one, and a
  `property=` match is preferred over a `name=` one;
* card titles and descriptions are capped at 300 and 1,000 Unicode code points
  respectively. Truncation makes a best-effort adjustment for combining marks
  and dangling zero-width joiners, but does not implement full Unicode grapheme
  segmentation. A cut can split a skin-tone emoji, a joined emoji, or a flag;
* nothing about the card can cost you the post. A failed thumbnail prints a
  warning and posts the card without a thumb; if the page itself can't be
  read at all — it declares a non-HTML type, fails the address or redirect
  checks, times out, or returns an error — the script warns and falls back
  to a bare card carrying just the URL, which clients still render as a link.

That last point is a deliberate ordering choice: the card is a decoration,
and by the time it is being built you are already authenticated and your text
is ready to send. Discarding the post because a remote server misbehaved
would be the worst possible trade.

And, most importantly, every one of these downloads goes through the
SSRF-protected fetcher described next.

## The Security Layer

The original cookbook script was a teaching tool, and it trusted everything:
the DNS answers, the redirects, the response sizes, the image bytes. That's
fine for a demo, but this script is meant to be run unattended against URLs
you don't control. The fork adds a defense layer that's worth understanding
even if you never read the rest of the code.

**SSRF protection with connection pinning.** Before any external fetch, the
hostname is resolved once, and *every* DNS answer must be a public unicast
address — private ranges, loopback, link-local and multicast are rejected,
and an IPv4 address carried inside an IPv6 translation prefix is judged by
the address it actually carries. These checks build on Python's `ipaddress`
classifications. So `64:ff9b::169.254.169.254` is refused as
the cloud-metadata address it really is, while `64:ff9b::8.8.8.8` is allowed,
which is what lets the script keep working on IPv6-only networks where DNS64
synthesizes every answer into that prefix. The deprecated 6to4
(`2002::/16`) and Teredo (`2001::/32`) prefixes are refused by an explicit
prefix check, because older Python releases classify some 6to4 addresses as
global. The local-use translation prefix `64:ff9b:1::/48` is refused too, and
an ISATAP address (`…:5efe:a.b.c.d`) is judged by the IPv4 address it
carries. Several other special-purpose ranges are refused by prefix for the
same reason, so every supported Python release gives the same answer:
`192.0.0.0/24`, the deprecated 6to4 relay anycast `192.88.99.0/24`, `198.18.0.0/15`, `100::/64`, `2001:2::/48`, `2001:10::/28`,
ORCHIDv2 `2001:20::/28` and DRIP `2001:30::/28` (identifiers rather than
locators, which Python 3.13 counts as global), `3fff::/20` and `5f00::/16`,
and the IPv4-mapped and IPv4-translated forms `::ffff:0:0/96` and
`::ffff:0:0:0/96` (a DNS answer in mapped form is converted to plain IPv4
first, and checked as that).
An IPv6 zone (`%eth0`) names a local interface, never a public destination,
so a scoped address is refused, and any `%` in a URL's host is refused before
DNS is asked. Hostnames that reach the resolver must consist of DNS labels
(letters, digits, hyphens and underscores) or be IP literals. The
connection is then made directly to a validated IP literal, while TLS still
authenticates the original hostname via SNI and certificate checks. Because
the connection goes to the address that was checked, a malicious DNS server
can't pass validation with a public IP and then rebind the name to
`169.254.169.254` for the actual request. Within one link card (the page,
its redirects and its `og:image`), each host is resolved and validated only
once, so a host met again (after an `http://` to `https://` redirect, or
spelled with a trailing root dot, say)
reuses those addresses rather than asking DNS a second time.

**Default ports only.** A public address is not necessarily a web server on
every port, so the page, every redirect hop and the `og:image` are fetched
only on their scheme's default port: 443 for `https://`, 80 for `http://`
(spelling it out, as in `https://example.com:443/`, is fine). Without this,
a page could point the script's HTTP request at SSH on `:22`, SMTP on `:25`
or Redis on `:6379` on any public host, and learn from the outcome which ports
answer. A URL naming any other port is refused before DNS is asked, which
for `--embed-url` leaves a card carrying just the URL and for an `og:image`
leaves the card without its thumbnail. Links in the post text still become
facets whatever their port, since nothing fetches them, and the PDS and
record-service URLs, which you choose yourself, keep any port.

Up to four validated addresses are tried in turn, alternating IPv6 and IPv4
(starting with whichever the resolver listed first), and each attempt that has
another address after it gets only three seconds to connect, or an even share
of the time left if that is less (as it may be after slow redirects). A first address
that silently drops connection attempts, such as IPv6 behind many VPNs and
container networks, therefore costs three seconds instead of the whole
ten-second download deadline, and the next address still serves the page.

Each attempt gets its own TLS context, so urllib3's per-connection changes
to it can never reach an API request or an abandoned worker. Building one
means parsing the roughly 150-certificate CA bundle, about 8 ms, so finished
contexts are kept and lent out again: one goes back only after its attempt
closed with no worker left using it, and two attempts in progress never share
one. A three-hop redirect chain (four requests) then costs one bundle load
instead of four.

Internationalized hostnames are converted to their A-label with the same
IDNA2008/UTS46 encoding everywhere: for DNS resolution, for SNI and the
certificate check, and for the `Host` header. Handing the raw Unicode name to `socket.getaddrinfo` would
encode it with CPython's IDNA2003 codec, which disagrees with the IDNA2008 /
UTS46 encoding used everywhere else for labels containing (for example) `ß`
— resolving one name while authenticating another. A trailing root dot
(`https://example.com./`) is kept for DNS but dropped from the name the
pinned adapter hands urllib3: urllib3 strips it from SNI but not from the name
it matches the certificate against, and no certificate names `example.com.`.

Every URL is limited to 8,192 characters and every hostname, ASCII or not, to
the 253 characters of the longest DNS name, before it is resolved, encoded,
or logged. A page's `og:image` URL or redirect target is attacker-chosen: an
800 KB hostname would otherwise reach the resolver and be echoed into the
warning in full.

**Redirect discipline.** Redirects are never followed automatically. Each
hop (at most 3) is re-validated from scratch — scheme, host, public address —
and HTTPS-to-HTTP downgrades are refused. Only 301, 302, 303, 307 and 308 are
followed; any other 3xx (300, 304, or the deprecated 305 Use Proxy, which must
never be followed) fails the download with its status. Credential-bearing API requests
refuse redirects entirely. A private Session subclass also suppresses Requests'
implicit preparation of the next redirect request: even with redirects disabled,
that preparation would otherwise buffer the redirect body before the script
could enforce its size limit. Redirect responses are closed unread.

**Deadlines everywhere, and one request per Session at a time.** Every network
operation runs under a wall-clock deadline that covers DNS resolution,
connection, and body reads, so a tarpit server can't hang the script
indefinitely.

Parsing a link card's HTML and extracting all its metadata share a five-second
budget, separate from the download deadline. Malformed metadata falls back to
a URL-only card. `html.parser` is pure Python and slow on pathological markup,
which a hostile page is free to serve. Parsing only the page's head (at most
1 MB) keeps real pages far inside the budget, and the head is read in the one
streaming pass described above, which builds no tree and keeps nothing but
the metadata it is after. A head stuffed with a million bytes of junk tags
therefore costs about two seconds and no more memory than the head itself,
and the budget still bounds how long posting waits for metadata. A page
that exceeds the parse budget loses its card metadata; the URL remains in the
post.

The budget bounds how long posting waits, not the parse itself. A parse that
overruns is *abandoned*, not terminated: its thread keeps running, using CPU
and memory, while posting continues, and finishes on its own (its input is at
most 1 MB). Image decoding is different: that runs in a separate process,
which is killed and reaped when it overruns.

This has a consequence worth spelling out. A blocking call that blows its
deadline is *abandoned* in a daemon thread rather than cancelled — Python
cannot cancel a thread parked in a socket read. That worker keeps running,
still holding the `requests.Session` it was handed. So a Session serves one
request at a time and is handed to the worker if that request is abandoned:
an orphaned worker can then only ever touch connection state that nothing else
will use again, and the code unwinding from the timeout simply leaves that
Session alone rather than closing it underneath a live socket read. When the
abandoned worker finishes, whether it succeeded or failed, it closes the
response and Session itself, so cleanup does not wait for garbage collection.

Within one run, calls to the PDS and the record service reuse connections, so
a post does not pay a TCP and TLS handshake for every XRPC call. Each origin
has its own small set of idle Sessions: a request leases one and gives it back
only after it completed cleanly with its body fully read. One whose body was
left unread (a 400 whose status was all the caller needed) has lost its
connection, so it is closed too rather than left on top of the warm ones.
One that was abandoned stays its worker's,
one that failed for any other reason is closed, and one idle for more than two
seconds is closed rather than reused, because a server may drop an idle
keep-alive connection (Node.js does after five seconds) and a POST sent into a
connection that just closed would fail. These Sessions store no cookies. The
idle set is closed when the run ends, after logout, which itself reuses the
connection `createRecord` just used. Downloads from the pages a post links to
never share Sessions: each still gets its own, pinned to a validated address.

This lets failures in optional work degrade gracefully. A `resolveHandle`
timeout leaves that mention as plain text; a thumbnail upload timeout omits
the thumbnail. Required operations, such as login, reply/quote lookups, attached
image uploads, and creating the post, still stop the command on failure.

Deadlines are sized to the work. Mention lookups share one 20-second budget,
and an image upload gets 30 seconds plus one second per 32 KB, so a 2 MB image
still fits on a slow (about 256 kbit/s) uplink. Concurrent uploads share that
uplink, so each one is budgeted for the bytes of all of them together.

**Independent work overlaps.** Once logged in, the script runs the mention
lookups, the reply-target lookup, the quoted-record lookup and the media (image
uploads or the link card) at the same time, so a post waits for the slowest of
them rather than their sum. Every stage still runs to completion, and when
several fail, the error reported is the one from the earliest stage in that
list, not whichever happened to fail first. The stages, and the mention
lookups within theirs (at most eight at a time), run in daemon threads, so
Ctrl-C exits at once instead of waiting for them to time out. A stage with
nothing to do gets no thread, and one that has nothing to overlap with runs
in the main thread, so a plain-text post starts none.

**Settling an ambiguous `createRecord`.** A timeout, a dropped connection, a
broken response, a success response that is not a JSON record reference
naming the requested repo, collection, and key, or a 5xx from a gateway in
front of the PDS leaves the outcome uncertain: the PDS may have committed the
post before its response was lost. Because the script chose the record key, it
can find out. It looks the key up with `com.atproto.repo.getRecord`; if the post exists, that is the
result. If not, it retries once with the *same* key. A key can hold only one
record, so even if the first attempt lands late, one of the two writes is
rejected and a second lookup finds the other: at most one post is ever
created. A definite 4xx rejection of the first attempt is reported as-is,
without a retry. If the retry fails and the second lookup still finds nothing,
or the lookups themselves fail, the command reports an error and the post may
or may not exist: even a rejected retry says nothing about a first attempt
that is still in flight. The warning then names the record it would be
(`at://<your DID>/app.bsky.feed.post/<key>`), so check it before running the
command again, because a new run uses a new key.

A distinct `DeadlineExceeded` exception separates an abandoned worker from
Requests' own `ConnectTimeout`/`ReadTimeout`. The latter leave no script worker
running that operation and permit trying the next validated address during an
external GET. That address fallback is separate from the single, same-key
`createRecord` retry above; no other API write is retried.

**Size caps and content checks.** Response bodies are streamed with a byte limit
applied to decoded data. The pinned urllib3 2.8.0 also bounds how much data is
expanded internally before yielding each chunk; checking chunks alone cannot
protect an application using an older unbounded decoder. It also fixes an
infinite loop in chunked deflate streaming and unbounded buffering of a
chunk-size line, both of which a hostile server could trigger. With an older
or unrecognized urllib3, the script therefore refuses compressed responses
before reading them. Requests use
`Accept-Encoding: identity`, but gzip/x-gzip and deflate responses are supported
with the patched decoder. Other codings, including optional Brotli backends,
are refused. A declared `Content-Length` is an early size check only for
unencoded bodies, since otherwise it measures compressed data.

**Nothing remote is printed raw.** Error text from the network reaches your
terminal on several paths — an XRPC error body, a refused response header, a
library exception message — and a terminal will happily execute escape
sequences hidden in any of them. A server that can clear your screen and
redraw it can also forge a convincing prompt asking you to re-enter your app
password. Every one of those strings is therefore rendered with control
characters escaped to visible `\xNN` before it is printed, while printable
non-ASCII is left intact so genuine localized messages still read correctly.
The Unicode line and paragraph separators (U+2028, U+2029) and the
bidirectional marks, embeddings, overrides and isolates (U+061C, U+200E,
U+200F, U+202A–U+202E, U+2066–U+2069) become `\uNNNN` too: the first can
start a new line in some terminals and log viewers, and the second can make
a line display different text from what it holds ("Trojan Source").

The `Error:` line itself is always a single line: a line break inside an
image decoder's message, a file name, or anything a server sent shows up as
`\x0a` and cannot print a fake prompt. Only the script's own text goes on
lines of its own: the remedy hint under a login failure, and the guidance
after a missing-credentials or missing-content error. URI errors
quote only the scheme, host and any explicit port of a web link, never its user name, password
or path, and a URI over 8,192 characters is refused without quoting any of
it. That includes the reply root taken from a parent post, which anyone could
have written. AT URIs, which are ASCII identifiers, are still quoted in full.

None of this changes what gets posted — it changes what a hostile web page,
or a hostile PDS, can do to the machine running the script.

## Putting It All Together

The complete script is a single file, `create_bsky_post.py`. Run
`--help` for the full option list. On success the script prints the new
post's `uri` and `cid` as JSON, after checking that they name the record it
asked for. `--verbose` prints the complete pending
record before it is sent and the server's error response to a failed
`createRecord`, as compact JSON cut at 64 KiB, with a note of how many
characters were left out (indenting arbitrarily nested JSON multiplies its
size); a body that could not be read within the response limits prints
`<error body could not be read>`. It is the quickest way to see the facets and embeds this post has been
describing. It is not a dry run: the post is still published, so try it with
text you are happy to post:

```bash
python3 create_bsky_post.py "Testing my posting script #greetings" --verbose
```

It also ships with a test suite, `test_create_bsky_post.py`, covering:

* the facet parsers, including ASCII-only handle matching;
* URI and CID validation;
* SSRF checks (including NAT64-translated, 6to4/Teredo and ISATAP addresses,
  special-purpose ranges under any Python release's classification, IPv6
  zones, and default-port-only fetches) and IDN resolution;
* redirect handling and limits, unread redirect bodies, and response size,
  content-encoding, and real compressed-response allocation limits;
* deeply nested JSON responses;
* image file safety, bounded batch image decoding, and EXIF display
  dimensions;
* login, logout, and error reporting, including terminal-safe rendering of
  server-supplied text (bidi controls and line separators included),
  single-line error messages, and redacted, length-capped URI errors;
* the link-card parse budget, head-only parsing, deeply nested titles, and the
  card and thumbnail degradation paths;
* concurrent mention resolution, concurrent post assembly and image uploads,
  and cleanup by abandoned workers;
* the settling of an ambiguous `createRecord`, including unusable success
  responses and mismatched record references:

```bash
python3 create_bsky_post.py --self-test   # or: python3 test_create_bsky_post.py
```

The Python excerpts in this post are checked against the script by
`verify_doc_excerpts.py`. Each excerpt's complete statements must match a
contiguous statement sequence in the script's syntax tree. Comments and
formatting are ignored, but statement order, nesting, exception types, and
literal values are checked, and no elisions are accepted. The checker also
parses every JSON example.

As the original post said: most people should use an SDK for their language
of choice. But sometimes it's helpful to see what's actually going on behind
the abstractions — and, when your script talks to the open web, what it takes
to do so safely.
