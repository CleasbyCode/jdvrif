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

Python 3.9 or newer. Dependencies are pinned in `requirements.txt` — install
from there rather than by hand, as the SSRF/TLS layer depends on specific
`urllib3` and `idna` behaviour:

```shell
pip install -r requirements.txt
```

Credentials
-----------

You need a Bluesky account and an **app password**, created at
<https://bsky.app/settings/app-passwords>. Do *not* use your main account
password: app passwords can be revoked individually and cannot change account
settings.

Prefer the environment variables over `--handle`/`--password`, since command
line arguments are visible to other local users via `ps` (the script warns you
if you pass `--password` anyway):

```shell
export ATP_AUTH_HANDLE='your-handle.bsky.social'
export ATP_AUTH_PASSWORD='xxxx-xxxx-xxxx-xxxx'
```

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
embeds the script built for you.

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
login error reporting, terminal-safe rendering of server-supplied text, the
link-card parse budget, and the card/thumbnail degradation paths:

```shell
python3 create_bsky_post.py --self-test
```

`run_bsky_tests.sh`, in the `tests/` directory of the C++ source tree, runs
that suite under both normal and optimized (`python3 -O`) Python, and checks
that no `assert` statements have crept in that optimization would strip — an
`assert` in a self-test would silently stop testing anything under `-O`.

The companion walkthrough quotes this script directly, so a third check keeps
the two from drifting apart:

```shell
python3 verify_doc_excerpts.py
```

It reports any line of any ```python block in
[posting-via-the-bluesky-api.md](posting-via-the-bluesky-api.md) that no
longer appears in `create_bsky_post.py`, and exits non-zero if any has. It
uses only the standard library, so it runs without installing the posting
script's requirements.

What the hardening covers
-------------------------

* **SSRF.** For every attacker-influenced fetch (the embed page, its
  redirects, its `og:image`), DNS is resolved once, every answer must be a
  public unicast address, and the connection is pinned to the validated IP
  while TLS still authenticates the original hostname.
* **Redirects.** Never followed automatically; each of at most 3 hops is
  revalidated from scratch, HTTPS-to-HTTP downgrades are refused, and
  credential-bearing requests refuse redirects entirely.
* **Resource limits.** Wall-clock deadlines on every network operation, with
  size caps applied to decoded bytes so a compressed body cannot inflate past
  them. Each request gets its own HTTP session, so a call abandoned at its
  deadline can never disturb a later one.
* **Input validation.** AT URIs, record CIDs, handles, BCP 47 language tags
  and URLs are all checked before use; image files are read symlink-safely
  and bounded by size, dimensions and pixel count.
* **Output safety.** Error text that came from the network — an XRPC error
  message, an HTTP reason phrase — is printed with control characters escaped,
  so a hostile server cannot rewrite your terminal or forge a prompt asking
  you to re-enter your app password.
* **Graceful degradation.** A failed thumbnail, an unreadable embed page or
  an unresolvable mention costs you that one feature, never the post.

Credits
-------

Original script and blog post by
[Bryan Newbold](https://bsky.app/profile/bnewbold.net) and the AT Protocol
team: ["Posting via the Bluesky API"](https://atproto.com/blog/create-post).
Fork:
<https://github.com/CleasbyCode/cookbook/blob/main/python-bsky-post/create_bsky_post.py>
