# Posting via the Bluesky API — the security-hardened edition

A guide to creating posts via the Bluesky API, including rich-text facets
(mentions, links, hashtags, cashtags), replies, quote posts, image embeds,
and website cards — with the safety rails a script needs when it fetches
untrusted content from the open web.

*July 23, 2026 — updated August 15, 2026*

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

It requires Python 3.9+ with `requests`, `beautifulsoup4`, and `pillow`.
Dependencies are pinned in `requirements.txt` for reproducibility — it also
pins `urllib3` and `idna`, which are transitive dependencies of `requests` but
which the SSRF/TLS layer relies on directly:

```bash
pip install -r requirements.txt
```

---

## Getting Started

You'll need a Bluesky account and an **app password**. Create one at
<https://bsky.app/settings/app-passwords> — do **not** use your main account
password. App passwords can be revoked individually and cannot change your
account settings, so a leaked one does far less damage.

Set your credentials as environment variables rather than command-line
arguments (arguments are visible to other local users via `ps`; the script
warns you if you pass `--password` anyway):

```bash
export ATP_AUTH_HANDLE='your-handle.bsky.social'
export ATP_AUTH_PASSWORD='xxxx-xxxx-xxxx-xxxx'
```

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
receives an access token (`accessJwt`) plus your account DID. Since the
script publishes a single post, it never needs the refresh token.

```python
def bsky_login_session(pds_url: str, handle: str, password: str) -> Dict:
    with _open_api_response(
        "POST",
        _api_url(pds_url, "com.atproto.server.createSession"),
        timeout=30,
        operation="waiting for createSession response headers",
        json={"identifier": handle, "password": password},
    ) as resp:
        if 300 <= resp.status_code < 400:
            raise ValueError(
                "Refusing redirect from credential-bearing createSession request"
            )
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
likely to get stuck and the status code alone never says why:

```
Error: Login failed with HTTP 401 (AuthFactorTokenRequired: A sign in code
has been sent to your email address).
This account has email two-factor authentication enabled, which the account
password cannot bypass. Use an APP password instead, created at
https://bsky.app/settings/app-passwords.
```

The hint is looked up from the error name for the handful of cases with an
actionable fix (2FA, a revoked password, rate limiting, a takedown); anything
else just reports what the server said. The password itself never appears in
an error message.

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

post = {
    "$type": "app.bsky.feed.post",
    "text": args.text,
    "createdAt": _created_at_now(),
}
```

The finished record goes to `com.atproto.repo.createRecord`, and the response
contains the new post's AT URI and CID.

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

### Byte offsets, done once

Facet indices are *byte* offsets into the UTF-8 encoding of the text, not
character offsets. The original worked around this by running regexes over
encoded bytes. The fork instead matches on the decoded string (where Unicode
categories are available) and converts with a precomputed offset table:

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
doesn't resolve — including because the lookup timed out — it's skipped and
simply renders as plain text, same behavior as the original. The fork adds:

* a Unicode-aware word-boundary check, so `email@example.com` is not treated
  as a mention of `example.com`,
* a 253-character handle length cap and strict `HANDLE_REGEX` fullmatch,
* per-post caching, so the same handle mentioned twice is resolved once,
* validation that the returned DID actually looks like a DID.

### Links

URLs are matched, then trailing punctuation is trimmed the way people
actually write: `https://bsky.app.` drops the final period, and bracket
trimming is balance-aware, so `(https://example.com/a_(b))` keeps the
parenthesis that belongs to the URL and drops the one that doesn't. Every
candidate is finally re-parsed and must be a well-formed `http(s)` URL with
no embedded credentials before it becomes a facet.

### Hashtags and cashtags

New in the fork. Hashtags support the fullwidth `＃` as well as `#`, strip
trailing punctuation, reject tags that are all digits or punctuation, filter
out invisible characters, and enforce the 64-grapheme / 640-byte lexicon
limits. Cashtags like `$TSLA` become tags too:

```python
HASHTAG_REGEX = re.compile(r"(^|\s)([#＃])(\S+)")
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
* `getRecord` responses are size-capped and shape-checked before use.

## Quote Posts and Record-with-Media

`--embed-ref` embeds a strong reference to another record
(`app.bsky.embed.record`). Posts, lists (`app.bsky.graph.list`), and feed
generators (`app.bsky.feed.generator`) are supported, and you can pass either
an `at://` URI or a `https://bsky.app/profile/…` URL — the script maps the
web path to the right collection.

If you combine `--embed-ref` with `--image` or `--embed-url`, the script
produces the `app.bsky.embed.recordWithMedia` union that the original never
supported:

```json
{
  "$type": "app.bsky.embed.recordWithMedia",
  "record": { "$type": "app.bsky.embed.record", "record": { "uri": "…", "cid": "…" } },
  "media": { "$type": "app.bsky.embed.images", "images": [ … ] }
}
```

## Image Embeds

Each post can carry up to four images. Since April 2026 each image blob may
be up to **2,000,000 bytes**, raised from the 1 MB limit that applied when the
original blog post was written; the maximum resolution went up at the same
time, to 4000×4000 from 2000px. The script enforces the 2 MB limit locally so
oversized files fail fast with a clear message.

Its own dimension caps (16,384px per side, 40 megapixels) are deliberately
*looser* than the service's — they are local decompression-bomb guards, not a
mirror of the server's rules, and they exist to bound Pillow's memory use
before the service ever sees the file.

Files are read defensively — opened with `O_NOFOLLOW` (with an
lstat/fstat identity check on platforms that lack it) and required to be
regular files, so the script can't be tricked into uploading
`/dev/stdin` or a symlink target. Each image is then inspected with Pillow
before upload:

```python
def inspect_image(img_bytes: bytes, source: str) -> Dict[str, Any]:
    with warnings.catch_warnings():
        warnings.simplefilter("error", Image.DecompressionBombWarning)
        with Image.open(io.BytesIO(img_bytes)) as img:
            width, height = img.size
            image_format = img.format
            _validate_image_dimensions(width, height)
            img.verify()
    ...
```

This yields three things the original didn't have:

* the actual format (PNG/JPEG/WebP/GIF), so the blob is uploaded with the
  correct MIME type instead of one guessed from the file extension;
* the pixel dimensions, published as the `aspectRatio` field so clients can
  lay out the image before it loads;
* protection against decompression bombs and absurd dimensions.

Alt text is supplied per image with repeatable `--alt-text` flags (the count
must match `--image`), and the `alt` field is always present — an empty
string when no alt text is given, as the lexicon requires. Each string is
capped at 2,000 characters; the lexicon itself sets no `maxLength` on `alt`,
so this simply mirrors the official composer. As with the original, stripping
EXIF metadata before upload remains the client's responsibility.

All four images are read and validated *before* the first blob is uploaded,
so a typo in the fourth filename fails the post without leaving three
orphaned blobs behind.

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
* the HTML download is capped at 4 MB and the thumbnail at 1 MB;
* `og:` properties are matched case-insensitively, since real pages do emit
  `property="OG:Title"`;
* card titles and descriptions are trimmed to sane lengths without splitting
  a combining character or emoji sequence at the cut point;
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
address — private ranges, loopback, link-local, multicast, and even IPv4
addresses smuggled inside IPv6 translation prefixes are rejected. The
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
refuse redirects entirely.

**Deadlines everywhere, and a Session per request.** Every network operation
runs under a wall-clock deadline that covers DNS resolution, connection, and
body reads, so a tarpit server can't hang the script indefinitely.

This has a consequence worth spelling out. A blocking call that blows its
deadline is *abandoned* in a daemon thread rather than cancelled — Python
cannot cancel a thread parked in a socket read. That worker keeps running,
still holding the `requests.Session` it was handed. So every request builds
its own Session and hands ownership to the worker if it is abandoned: an
orphaned worker can then only ever touch connection state that nothing else
will use again, and the code unwinding from the timeout simply leaves that
Session alone rather than closing it underneath a live socket read.

The payoff is that timeouts are ordinary, recoverable errors everywhere. A
`resolveHandle` that hangs costs you one mention, which falls back to plain
text; a thumbnail upload that hangs costs you the thumbnail. Neither costs
you the post. The alternative — a single shared Session — forces the opposite
rule, where any timeout has to terminate the process to stay safe, and that
rule is invisible to the next person editing the file. A distinct
`DeadlineExceeded` exception type keeps this honest: it separates "our
deadline expired and a worker was abandoned" from requests' own
`ConnectTimeout`/`ReadTimeout`, which leave nothing running and are safely
retried against the host's next address.

**Size caps and content checks.** Response bodies are streamed with a hard
byte limit applied to *decoded* bytes, which is exactly where a decompression
bomb has to be stopped. Requests go out with `Accept-Encoding: identity`, but
servers do ignore that, so any coding urllib3 unwraps transparently while
streaming (gzip, deflate) is accepted and only codings that would reach the
parser still encoded are refused. A declared `Content-Length` is used as an
early-out too, but only when no coding was applied — otherwise it describes
the compressed body and says nothing about what the response inflates to.

None of this changes what gets posted — it changes what a hostile web page
can do to the machine running the script.

## Putting It All Together

The complete script is a single file, `create_bsky_post.py`. Run
`--help` for the full option list. `--verbose` prints the complete pending
record before it is sent, plus the full body of a failed `createRecord`,
which is the quickest way to see the facets and embeds this post has been
describing:

```bash
python3 create_bsky_post.py "Hello, @alice.test! #greetings" --verbose
```

It also ships with a built-in test suite covering the facet parsers, URI
validation, SSRF checks, IDN resolution, redirect handling and limits,
response size and content-encoding limits, image file safety, login error
reporting, and the card and thumbnail degradation paths:

```bash
python3 create_bsky_post.py --self-test
```

As the original post said: most people should use an SDK for their language
of choice. But sometimes it's helpful to see what's actually going on behind
the abstractions — and, when your script talks to the open web, what it takes
to do so safely.
