# Posting via the Bluesky API — the security-hardened edition

A guide to creating posts via the Bluesky API, including rich-text facets
(mentions, links, hashtags, cashtags), replies, quote posts, image embeds,
and website cards — with the safety rails a script needs when it fetches
untrusted content from the open web.

*July 23, 2026 — updated September 25, 2026*

This post is an updated companion to the AT Protocol team's original
[Posting via the Bluesky API](https://atproto.com/blog/create-post)
(August 2023) and its accompanying `create_bsky_post.py` cookbook script.
The script described here is a **security-hardened fork** of that original:

* Original: <https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py>
* Fork: <https://github.com/CleasbyCode/cookbook/blob/main/python-bsky-post/create_bsky_post.py>

The fork keeps the spirit of the original — a single standalone Python file
that shows what's really going on behind the SDK abstractions — but brings it
up to date with current lexicon limits (including the [April 2026 increase of
the image size limit to 2 MB](https://techcrunch.com/2026/04/23/bluesky-now-supports-better-quality-photos/)),
adds hashtag/cashtag facets, per-image alt text, aspect ratios, language tags,
and record-with-media embeds, and treats every network fetch as potentially
hostile.

It requires Python 3.10+ with `requests`, `beautifulsoup4`, and `pillow`.
`requirements.txt` pins these packages and also `urllib3` and `idna`, which
are transitive dependencies of `requests` used directly by the SSRF/TLS layer:

```bash
python3 -m pip install -r requirements.txt
```

These are selected version pins, not a complete dependency lock.
`certifi`, `charset-normalizer`, `soupsieve`, and `typing-extensions` are
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
the session. Instead, when the run ends, whether the post succeeded or not, it
revokes the session with `com.atproto.server.deleteSession`, so the
long-lived refresh token does not stay valid on the server after the script
exits.

```python
def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    with _open_api_response(
        "POST",
        _api_url(pds_url, "com.atproto.server.createSession"),
        timeout=30,
        operation="waiting for createSession response headers",
        json={"identifier": handle, "password": password},
    ) as resp:
        _reject_redirect(resp, "credential-bearing createSession")
        if not resp.ok:
            body = _response_body(resp)
            error_name = (_api_error_name(body) or "").lower()
            hint = LOGIN_ERROR_HINTS.get(error_name)
            raise ValueError(
                f"Login failed with HTTP {resp.status_code}"
                f"{_api_error_summary(body)}."
                + (f"\n{hint}" if hint else "")
            )
        data = _json_object(resp, "createSession")
    if not isinstance(data.get("accessJwt"), str) or not isinstance(data.get("did"), str):
        raise ValueError("createSession response is missing accessJwt or did")
    return data
```

Three hardening details already show up here. Redirects are disabled and
explicitly refused: a misconfigured or malicious endpoint must never be able
to bounce a request carrying your password somewhere else. The response is
read through a size-capped, deadline-bounded reader rather than trusted
blindly. And the failure path reads the XRPC error body instead of raising a
bare `401 Client Error`, because login is where a first-time user is most
likely to get stuck and the status code alone never says why. The output is
two lines, the server's error and then the hint (wrapped here for width):

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
`--allow-insecure-pds`, and even then only for `localhost`/loopback
addresses, so the flag is useful for local development but can't be abused
to send credentials in cleartext across a network.

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

### Byte offsets, computed once per parser

Facet indices are *byte* offsets into the UTF-8 encoding of the text, not
character offsets. The original worked around this by running regexes over
encoded bytes. The fork instead matches on the decoded string (where Unicode
categories are available) and converts with a precomputed offset table. Each
of the link, mention, and tag parsers builds its own table, so parsing all
facets makes three passes over the text. Each pass is linear, and the CLI
limits the text to 3,000 UTF-8 bytes:

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
  as a mention of `example.com`,
* a 253-character handle length cap and strict `HANDLE_REGEX` fullmatch,
* case-insensitive de-duplication, so the same handle mentioned twice is
  resolved once,
* concurrent lookups (up to 8 at a time) sharing one 20-second overall budget,
  so a post full of unresolvable handles cannot stall for minutes,
* validation that the returned DID actually looks like a DID.

### Links

URLs are matched, then trailing punctuation is trimmed the way people
actually write: `https://bsky.app.` drops the final period, and bracket
trimming is balance-aware, so `(https://example.com/a_(b))` keeps the
parenthesis that belongs to the URL and drops the one that doesn't. Every
candidate is finally re-parsed and must be a well-formed `http(s)` URL with
no embedded credentials before it becomes a facet.

### Hashtags and cashtags

New in the fork. Hashtags follow the official composer's rules: they support
the fullwidth `＃` as well as `#`, end at whitespace or at an invisible
character such as a zero-width space or joiner (so `#foo` followed by a
zero-width space and `bar` tags `foo`), may not start with a variation
selector, strip trailing punctuation, and are rejected when made only of ASCII
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
hashtag, all inside a URL. The fork resolves conflicts with a span
reservation system: links claim their byte ranges first, then mentions, then
tags, and any span overlapping an already-reserved range is dropped. As a
bonus, mentions inside URLs are never even sent to `resolveHandle`.

## Replies

A reply must reference both the immediate **parent** post and the thread's
**root** post, each as a strong ref (`uri` + `cid`). The logic is the same as
the original — fetch the parent via `com.atproto.repo.getRecord`; if the
parent is itself a reply, reuse its root ref, otherwise the parent *is* the
root — with several correctness fixes layered on:

* record lookups go to a network-wide record service
  (`https://public.api.bsky.app` by default, configurable with
  `--record-service-url`), not your own PDS, so you can reply to posts hosted
  on any PDS in the network;
* if the parent's `reply.root` ref already contains a CID, it is reused
  instead of re-fetched;
* every URI is validated as a real `app.bsky.feed.post` reference, and every
  CID is checked to be a canonical base32 CIDv1 (dag-cbor, SHA-256) — the
  fork round-trips the decode/re-encode rather than pattern-matching;
* `getRecord` responses are size-capped and shape-checked before use, and
  the returned record must be the one requested: same collection and record
  key, and the same DID when the request named one. (A request made with a
  handle is answered with the account's DID, which cannot be compared without
  a separate lookup.)

## Quote Posts and Record-with-Media

`--embed-ref` embeds a strong reference to another record
(`app.bsky.embed.record`). You can pass an `at://` URI for any record, or a
`https://bsky.app/profile/…` URL for a post, a list (`app.bsky.graph.list`),
or a feed generator (`app.bsky.feed.generator`); the script maps the web path
to the right collection. A handle in a `bsky.app` URL may be written in any
case, since handles are case-insensitive.

If you combine `--embed-ref` with `--image` or `--embed-url`, the script
produces the `app.bsky.embed.recordWithMedia` union that the original never
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
original blog post was written. That limit is part of the lexicon: the
`app.bsky.embed.images` schema declares `maxSize: 2000000` on the image blob,
and the script enforces it locally so oversized files fail fast with a clear
message instead of being rejected when the post is created. The same
announcement reported a higher maximum resolution (4000×4000, up from 2000px).
That one is a service rule rather than a lexicon constraint, so treat it as "as
announced in April 2026"; the script does not depend on it.

Its own dimension caps (16,384px per side, 40 megapixels) are deliberately
*looser* than the service's — they are local decompression-bomb guards, not a
mirror of the server's rules, and they exist to bound Pillow's memory use
before the service ever sees the file.

Files are read defensively: opened with `O_NOFOLLOW` (with an lstat/fstat
identity check on platforms that lack it) and required to be regular files.
So a path whose final component is a symlink, or a device such as
`/dev/stdin`, is refused. Symlinked *directories* earlier in the path are still
followed; the check guards the file itself, not every component leading to it.

All of a post's images are read and validated before the script logs in, so a
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
```

The worker verifies container integrity and fully decodes each image. JPEG
`verify()` alone does not reject truncated pixel data. The parent kills and
reaps the worker if it runs longer than 15 seconds for interpreter start-up
plus five seconds per image. Where Python's `resource` module is available, the
worker also installs a 768 MiB address-space limit, a matching CPU-time limit,
and a 16 KiB output-file limit before loading the helper. These limits are
enforced on Linux; macOS accepts the address-space limit but, as far as we
know, does not enforce it. The worker receives image bytes without the parent's
posting credentials. Animation is limited to 100 frames and 80 million total
decoded pixels. Image bytes are never re-encoded or rewritten during
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
the post's `app.bsky.embed.images` array.

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
  strings against the original URL);
* the page's HTML is decoded using the charset the server declares in its
  `Content-Type` header (falling back to a BOM, an in-document
  `<meta charset>`, and byte sniffing), so a page served in a non-UTF-8
  encoding still yields correct card text;
* a page that declares a non-HTML `Content-Type` (a PDF, a video) is refused
  before its body is downloaded; one that declares no type is still read;
* the HTML download is capped at 4 MB, and the thumbnail at 1 MB, which is
  also the lexicon's `maxSize` for `app.bsky.embed.external` thumbnails;
* only the page's `<head>` is parsed (everything before `</head>`, or before
  `<body>` when the page omits it, up to 1 MB), since that is where the
  metadata lives;
* `og:` properties are matched case-insensitively, since real pages do emit
  `property="OG:Title"`, and their values are trimmed, so an `og:image`
  padded with newlines still loads and a blank `og:title` falls back to
  `<title>`;
* card titles and descriptions are capped at 300 and 1,000 Unicode code points
  respectively. Truncation makes a best-effort adjustment for combining marks
  and dangling zero-width joiners, but does not implement full Unicode grapheme
  segmentation. A cut can split a skin-tone emoji, a joined emoji, or a flag;
* nothing about the card can cost you the post. A failed thumbnail prints a
  warning and posts the card without a thumb; if the page itself can't be
  read at all — it exceeds the cap, times out, or returns an error — the
  script warns and falls back to a bare card carrying just the URL, which
  clients still render as a link.

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
the address it actually carries. So `64:ff9b::169.254.169.254` is refused as
the cloud-metadata address it really is, while `64:ff9b::8.8.8.8` is allowed,
which is what lets the script keep working on IPv6-only networks where DNS64
synthesizes every answer into that prefix. The deprecated 6to4 and Teredo
prefixes are refused outright, as is the local-use translation prefix
`64:ff9b:1::/48`. The
connection is then made directly to a validated IP literal, while TLS still
authenticates the original hostname via SNI and certificate checks. Because
the connection goes to the address that was checked, a malicious DNS server
can't pass validation with a public IP and then rebind the name to
`169.254.169.254` for the actual request.

Internationalized hostnames are encoded to their A-label *once*, and that
single encoding is what gets resolved, sent as SNI, and checked against the
certificate. Handing the raw Unicode name to `socket.getaddrinfo` would
encode it with CPython's IDNA2003 codec, which disagrees with the IDNA2008 /
UTS46 encoding used everywhere else for labels containing (for example) `ß`
— resolving one name while authenticating another.

**Redirect discipline.** Redirects are never followed automatically. Each
hop (at most 3) is re-validated from scratch — scheme, host, public address —
and HTTPS-to-HTTP downgrades are refused. Credential-bearing API requests
refuse redirects entirely. A private Session subclass also suppresses Requests'
implicit preparation of the next redirect request: even with redirects disabled,
that preparation would otherwise buffer the redirect body before the script
could enforce its size limit. Redirect responses are closed unread.

**Deadlines everywhere, and a Session per request.** Every network operation
runs under a wall-clock deadline that covers DNS resolution, connection, and
body reads, so a tarpit server can't hang the script indefinitely.

Parsing a link card's HTML and extracting all its metadata share a five-second
budget, separate from the download deadline. Title text is traversed iteratively
and copied only up to the card-title bound, so nested markup does not recurse
through Python's stack. Malformed metadata falls back to a URL-only card.
`html.parser` is pure Python and slow on pathological markup, which a hostile
page is free to serve. Parsing only the page's head (at most 1 MB) keeps real
pages far inside the budget, and the budget still bounds a page whose head is
stuffed with hostile markup. A page that exceeds the parse budget loses its
card metadata; the URL remains in the post.

This has a consequence worth spelling out. A blocking call that blows its
deadline is *abandoned* in a daemon thread rather than cancelled — Python
cannot cancel a thread parked in a socket read. That worker keeps running,
still holding the `requests.Session` it was handed. So every request builds
its own Session and hands ownership to the worker if it is abandoned: an
orphaned worker can then only ever touch connection state that nothing else
will use again, and the code unwinding from the timeout simply leaves that
Session alone rather than closing it underneath a live socket read.

This lets failures in optional work degrade gracefully. A `resolveHandle`
timeout leaves that mention as plain text; a thumbnail upload timeout omits
the thumbnail. Required operations, such as login, reply/quote lookups, attached
image uploads, and creating the post, still stop the command on failure.

Deadlines are sized to the work. Mention lookups share one 20-second budget,
and an image upload gets 30 seconds plus one second per 32 KB, so a 2 MB image
still fits on a slow (about 256 kbit/s) uplink.

**Settling an ambiguous `createRecord`.** A timeout, a dropped connection, a
broken response, or a 5xx from a gateway in front of the PDS leaves the
outcome uncertain: the PDS may have committed the post before its response was
lost. Because the script chose the record key, it can find out. It looks the
key up with `com.atproto.repo.getRecord`; if the post exists, that is the
result. If not, it retries once with the *same* key. A key can hold only one
record, so even if the first attempt lands late, one of the two writes is
rejected and a second lookup finds the other: at most one post is ever
created. A definite 4xx rejection is reported as-is, without a retry. If even
the lookups fail, the command reports an error, and the post may or may not
exist, so check your profile before running it again, because a new run uses
a new key.

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
chunk-size line, both of which a hostile server could trigger. Older or
unrecognized versions therefore refuse compressed responses before reading. Requests use
`Accept-Encoding: identity`, but gzip/x-gzip and deflate responses are supported
with the patched decoder. Other codings, including optional Brotli backends,
are refused. A declared `Content-Length` is an early size check only for
unencoded bodies, since otherwise it measures compressed data.

**Nothing remote is printed raw.** Error text from the network reaches your
terminal on several paths — an XRPC error body, an HTTP reason phrase, a
library exception message — and a terminal will happily execute escape
sequences hidden in any of them. A server that can clear your screen and
redraw it can also forge a convincing prompt asking you to re-enter your app
password. Every one of those strings is therefore rendered with control
characters escaped to visible `\xNN` before it is printed, while printable
non-ASCII is left intact so genuine localized messages still read correctly.

None of this changes what gets posted — it changes what a hostile web page,
or a hostile PDS, can do to the machine running the script.

## Putting It All Together

The complete script is a single file, `create_bsky_post.py`. Run
`--help` for the full option list. `--verbose` prints the complete pending
record before it is sent, plus the full body of a failed `createRecord`,
which is the quickest way to see the facets and embeds this post has been
describing. It is not a dry run: the post is still published, so try it with
text you are happy to post:

```bash
python3 create_bsky_post.py "Testing my posting script #greetings" --verbose
```

It also ships with a built-in test suite covering the facet parsers, URI
validation, SSRF checks (including NAT64-translated addresses), IDN
resolution, redirect handling and limits, response size and content-encoding
limits, image file safety, login and logout, error reporting, terminal-safe
rendering of server-supplied text, the link-card parse budget, the card and
thumbnail degradation paths, and the settling of an ambiguous `createRecord`:

```bash
python3 create_bsky_post.py --self-test
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
