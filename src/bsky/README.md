Create bsky Post (Python)
=========================

A single-file script demonstrating how to create posts with the Bluesky API,
covering rich-text facets, replies, quote posts, image embeds and website
cards — without an SDK in the way.

This is a **security-hardened fork** of Bryan Newbold's original
[cookbook script](https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py).
Beyond the extra features, it treats every network fetch as potentially
hostile, because `--embed-url` makes the script follow URLs you don't
control. See [posting-via-the-bluesky-api.md](posting-via-the-bluesky-api.md)
for the full walkthrough of what it does and why.

Requirements
------------

Python 3.10 or newer. `requirements.txt` pins the directly used packages,
including `urllib3` and `idna`, whose behaviour the SSRF/TLS layer relies on:

```shell
python3 -m pip install -r requirements.txt
```

These are selected pins, not a complete dependency lock: `certifi`,
`charset-normalizer`, `soupsieve`, and `typing-extensions` can still vary
within their dependencies' allowed ranges. Fully repeatable installs require
locking those transitive dependencies and recording the target environment.

The pins include urllib3 2.8.0 for bounded streaming decompression and safe
chunked reads, requests 2.34.2 and idna 3.20 with their security fixes, and
Pillow 12.3.0 for image inspection. Older or unrecognized urllib3 releases
refuse compressed responses before reading them. Accepted response codings are
identity, gzip/x-gzip and deflate; unsupported codings use the existing error
or link-card fallback path.

Credentials
-----------

You need a Bluesky account and an **app password**, created at
<https://bsky.app/settings/app-passwords>. Do *not* use your main account
password: app passwords can be revoked individually and cannot change
authentication settings, though they still grant access to publish and manage
account content.

Use an interactive Bash prompt to populate the environment variable without
typing the password into a command that shell history can retain. Passwords
passed through `--password` are visible to other local users via `ps`, and
the script warns about this:

```shell
export ATP_AUTH_HANDLE='your-handle.bsky.social'
read -r -s -p 'Bluesky app password: ' ATP_AUTH_PASSWORD
printf '\n'
export ATP_AUTH_PASSWORD
```

The variable remains available to processes launched from this shell; use
`unset ATP_AUTH_PASSWORD` when you finish posting.

Usage
-----

```shell
# plain text, with a hashtag facet
python3 create_bsky_post.py "Hello, Bluesky! #greetings"

# up to 4 images, one --alt-text each, in order
python3 create_bsky_post.py "Sunset over the bay" \
    --image sunset.jpg --alt-text "Orange sunset over a calm bay"

# website card built from the page's Open Graph tags
python3 create_bsky_post.py "Worth a read" --embed-url "https://example.com/article"

# reply
python3 create_bsky_post.py "Replying" \
    --reply-to "at://did:plc:xxx/app.bsky.feed.post/yyy"

# quote post -- bsky.app URLs work as well as at:// URIs
python3 create_bsky_post.py "Quoting this post" \
    --embed-ref "https://bsky.app/profile/example.com/post/yyy"

# quote post with attached media (record-with-media)
python3 create_bsky_post.py "Quoted with media" \
    --embed-ref "at://did:plc:xxx/app.bsky.feed.post/yyy" --image photo.jpg

# language tags (at most 3)
python3 create_bsky_post.py "สวัสดีชาวโลก! Hello World!" --lang th --lang en-US
```

For the full list of options and arguments:

```shell
python3 create_bsky_post.py --help
```

`--verbose` prints the complete pending record before it is sent, plus the
body of a failed `createRecord` — the quickest way to inspect the facets and
embeds the script built for you. It is not a dry run: the post is still
published.

**Images are uploaded byte for byte, metadata included.** EXIF data such as
GPS location and camera details becomes part of the public blob, which anyone
can download through `com.atproto.sync.getBlob` even though the Bluesky app
displays re-encoded copies. Remove metadata you don't want published before
posting. Blanket metadata strippers also remove any data you have deliberately
embedded in those segments.

Server error messages are escaped for terminal safety but are not redacted
for secrets. A server could echo credentials, so check diagnostics before
sharing them.

If `createRecord` ends ambiguously (a timeout, a dropped connection, or a 5xx
error), the script settles it itself. It chose the post's record key, so it
looks that key up, and if the post is not there, retries once with the same
key. A key can hold only one record, so at most one post is created. Only if
those lookups fail too is the outcome left unknown; check your profile before
re-running, because a new run uses a new key. Failures in optional work
(mention lookups, card thumbnails) instead let posting continue with those
features omitted.

The login session is revoked (`com.atproto.server.deleteSession`) when the
run ends, whether or not the post succeeded.

### Services

| Option | Environment variable | Default |
| --- | --- | --- |
| `--pds-url` | `ATP_PDS_HOST` | `https://bsky.social` |
| `--record-service-url` | `ATP_RECORD_SERVICE_HOST` | `https://public.api.bsky.app` |

Replies and quotes are looked up through the record service rather than your
own PDS, so you can reference posts hosted anywhere in the network. Both URLs
must be HTTPS and must be a bare scheme and host. `--allow-insecure-pds`
permits plain HTTP for local development, and only for `localhost`/loopback
addresses.

Tests
-----

The script carries its own test suite — facet parsers, URI and CID
validation, SSRF and IDN handling (including NAT64-translated addresses),
redirect limits, response size and content-encoding limits, image file safety,
login, logout and error reporting, terminal-safe rendering of server-supplied
text, the link-card metadata budget and head-only parsing, deeply nested
titles, real compressed-response allocation limits, unread redirect bodies,
bounded batch image decoding, EXIF display dimensions, concurrent mention
resolution, the settling of an ambiguous `createRecord`, and the
card/thumbnail degradation paths:

```shell
python3 create_bsky_post.py --self-test
```

`run_bsky_tests.sh`, in the `tests/` directory of the C++ source tree, runs
that suite under both normal and optimized (`python3 -O`) Python, and checks
that no `assert` statements have crept in that optimization would strip — an
`assert` in a self-test would silently stop testing anything under `-O`.
It also runs the documentation-excerpt checker, its regression tests, and the
Rust mirror comparison.

The companion walkthrough quotes this script directly. The documentation
check that keeps the two from drifting apart can also be run on its own:

```shell
python3 verify_doc_excerpts.py
```

It compares each Python excerpt in
[posting-via-the-bluesky-api.md](posting-via-the-bluesky-api.md) against a
contiguous statement sequence in the script's syntax tree, including statement
order, nesting, exception types, and literal values. Comments and formatting
are ignored; excerpts must contain complete statements without elisions.
It also parses every JSON example and exits non-zero on drift or invalid
syntax. It uses only the standard library, so it runs without installing the
posting script's requirements.

What the hardening covers
-------------------------

* **SSRF.** For every attacker-influenced fetch (the embed page, its
  redirects, its `og:image`), DNS is resolved once, every answer must be a
  public unicast address, and the connection is pinned to the validated IP
  while TLS still authenticates the original hostname.
* **Redirects.** Never followed automatically; each of at most 3 hops is
  revalidated from scratch, HTTPS-to-HTTP downgrades are refused, and
  credential-bearing requests refuse redirects entirely. Redirect responses
  are closed unread before Requests can buffer their bodies.
* **Resource limits.** Wall-clock deadlines on every network operation, with
  size caps applied to decoded bytes so a compressed body cannot inflate past
  them. Each request gets its own HTTP session, so a call abandoned at its
  deadline can never disturb a later one.
* **Input validation.** AT URIs, record CIDs, handles, BCP 47 language tags
  and URLs are all checked before use. Image files are refused if the final
  path component is a symlink or the file is not a regular file, and are
  bounded by size, dimensions and pixel count. All images are read and
  validated before login. WebP canvas dimensions are checked before native
  decoder construction. A post's images are fully decoded in one separate
  process, allowed 15 seconds for start-up plus five seconds per image; where
  Python's `resource` module is available it also sets a 768 MiB address-space
  limit (enforced on Linux; not, as far as we know, on macOS) and CPU and
  output-file limits. Animation is capped at 100 frames and 80 million total
  decoded pixels. EXIF orientation determines display dimensions, while the
  original bytes are uploaded unchanged.
* **Output safety.** Error text that came from the network — an XRPC error
  message, an HTTP reason phrase — is printed with control characters escaped,
  so a hostile server cannot rewrite your terminal or forge a prompt asking
  you to re-enter your app password.
* **Graceful degradation.** A failed thumbnail, an unreadable or non-HTML
  embed page, or an unresolvable mention costs you that one feature, never the
  post. Mention lookups run concurrently under one shared deadline.

Credits
-------

Original script and blog post by
[Bryan Newbold](https://bsky.app/profile/bnewbold.net) and the AT Protocol
team: ["Posting via the Bluesky API"](https://atproto.com/blog/create-post).
Fork:
<https://github.com/CleasbyCode/cookbook/blob/main/python-bsky-post/create_bsky_post.py>
