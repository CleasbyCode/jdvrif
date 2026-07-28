# Posting via the Bluesky API — the security-hardened edition

A guide to creating posts via the Bluesky API, including rich-text facets
(mentions, links, hashtags, cashtags), replies, quote posts, image embeds,
and website cards — with the safety rails a script needs when it fetches
untrusted content from the open web.

*July 23, 2026*

This post is an updated companion to the AT Protocol team's original
[Posting via the Bluesky API](https://atproto.com/blog/create-post)
(August 2023) and its accompanying `create_bsky_post.py` cookbook script.
The script described here is a **security-hardened fork** of that original:

* Original: <https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py>
* Fork: <https://github.com/CleasbyCode/cookbook/blob/main/python-bsky-post/create_bsky_post.py>

The fork keeps the spirit of the original — a single standalone Python file
that shows what's really going on behind the SDK abstractions — but brings it
up to date with current lexicon limits (including the April 2026 increase of
the image size limit to 2 MB), adds hashtag/cashtag facets, per-image alt
text, aspect ratios, language tags, and record-with-media embeds, and treats
every network fetch as potentially hostile.

It requires Python 3.9+ with `requests`, `beautifulsoup4`, and `pillow`:

```bash
pip install requests beautifulsoup4 pillow
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
```

Two hardening details already show up here. Redirects are disabled and
explicitly refused: a misconfigured or malicious endpoint must never be able
to bounce a request carrying your password somewhere else. And the response
is read through a size-capped, deadline-bounded reader rather than trusted
blindly.

The PDS URL itself must be HTTPS. Plain HTTP is only allowed with
`--allow-insecure-pds`, and even then only for `localhost`/loopback
addresses, so the flag is useful for local development but can't be abused
to send credentials in cleartext across a network.

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
`i-klingon` — so a typo like `--lang english` fails locally with a clear
error instead of producing a malformed record.

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
doesn't resolve, it's skipped and simply renders as plain text — same
behavior as the original. The fork adds:

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
be up to **2,000,000 bytes** (the `app.bsky.embed.images` lexicon was raised
from the 1 MB limit that applied when the original blog post was written, and
accepted dimensions went up to 4000×4000). The script enforces the 2 MB limit
locally so oversized files fail fast with a clear message.

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
string when no alt text is given, as the lexicon requires. As with the
original, stripping EXIF metadata before upload remains the client's
responsibility.

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
* the HTML download is capped at 2 MB and the thumbnail at 1 MB;
* card titles and descriptions are trimmed to sane lengths without splitting
  a combining character or emoji sequence at the cut point;
* a failed thumbnail download prints a warning and posts the card without a
  thumb, instead of aborting the whole post.

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

**Redirect discipline.** Redirects are never followed automatically. Each
hop (at most 3) is re-validated from scratch — scheme, host, public address —
and HTTPS-to-HTTP downgrades are refused. Credential-bearing API requests
refuse redirects entirely.

**Deadlines everywhere.** Every network operation runs under a wall-clock
deadline that covers DNS resolution, connection, and body reads, so a
tarpit server can't hang the script indefinitely.

**Size caps and content checks.** Response bodies are streamed with hard
byte limits (declared `Content-Length` is checked first, then actual bytes),
and compressed (`Content-Encoding`) responses are refused so a small gzip
body can't expand into gigabytes.

None of this changes what gets posted — it changes what a hostile web page
can do to the machine running the script.

## Putting It All Together

The complete script is a single file, `create_bsky_post.py`. Run
`--help` for the full option list. It also ships with a built-in test suite
covering the facet parsers, URI validation, SSRF checks, redirect handling,
and response limits:

```bash
python3 create_bsky_post.py --self-test
```

As the original post said: most people should use an SDK for their language
of choice. But sometimes it's helpful to see what's actually going on behind
the abstractions — and, when your script talks to the open web, what it takes
to do so safely.
