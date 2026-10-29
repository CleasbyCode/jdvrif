# jdvrif

***jdvrif*** is a steganography command-line tool for concealing and extracting any file type via a **JPG** image.

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_fd123e565.jpg)  
***Demo Image -*** ***KEY: Wtub-TzgD-VCvf-aLHW-UwY2-UBoF***

Your data file is compressed with ***libdeflate/zlib***, then encrypted with ***XChaCha20-Poly1305 (libsodium secretstream)*** under a ***256-bit encryption key derived by Argon2id*** from a randomly generated ***128-bit recovery KEY***, and finally embedded in the cover image.  

The KEY is displayed once, at the end of conceal, printed as 24 Base58 characters (*letters and digits*) in six groups of four (*XXXX-XXXX-XXXX-XXXX-XXXX-XXXX*) and is not saved by the program. Without it, the concealed file cannot be recovered. Its last two characters form a checksum, so most typing mistakes are reported straight away.

Images created before ***jdvrif v10*** used a numeric ***recovery PIN*** instead. They remain recoverable with that PIN: ***recover*** detects the image's version where it can and prompts for a ***KEY*** or ***PIN*** to match (*see [Usage](#usage)*).

Using the ***default conceal mode***, you can conceal any file type, with the finished image capped at ***2GiB***. The other platform conceal modes and the compatible social media sites (*listed below*) have their own ***much smaller*** size limits and other requirements.

There is a [***Web edition***](https://cleasbycode.co.uk/jdvrif/app/) of ***jdvrif***, which you can use immediately, as a convenient alternative to downloading and compiling the CLI source code. Web file uploads are limited to **20MiB**.

An experimental ***Rust*** port [***jdvrif-rs***](https://github.com/CleasbyCode/jdvrif-rs) is also available for those interested in that language. It is format-compatible: either build can recover the other's images.

***jdvrif*** partly derives from the ***[technique implemented](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).***

## How jdvrif conceals data

Unlike the common [***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/) (*Least Significant Bit*) steganography method of concealing data within the pixels of a cover image, ***jdvrif*** mostly hides data within ***application segments*** of a ***JPG*** image (ICC, EXIF, XMP, etc).

| Conceal&nbsp;Mode | Storage Method | Compatible Platforms |
| :---: | :---: | :---: |
| *(no option)* | APP2/ICC profile segments | X-Twitter, Tumblr, Mastodon,<br>Pixelfed, PostImage, ImgBB,<br>ImgPile, Flickr |
| ***-b*** | Framed EXIF / Photoshop / XMP segments | ***Bluesky*** only |
| ***-r*** | JPEG DCT coefficients (***QIM***) | ***Reddit*** only |
| ***-x*** | JPEG DCT coefficients (***J-UNIWARD/STC***) | ***X-Twitter*** only |

The two platform exceptions to the default segment storage method are ***Reddit*** and ***X-Twitter***. Both have their own conceal mode, and neither of those modes uses metadata segments at all: ***-r*** and ***-x*** carry the payload in the ***JPG*** image's DCT coefficients instead.  

***Reddit*** re-encodes uploaded images and discards the metadata segments the default mode relies on, so ***-r*** is the only mode that works there. ***X-Twitter*** does preserve a single, small ICC segment, so the default mode still works on that platform, but only for a tiny payload.

For the ***Reddit*** conceal mode (***-r***), we use the [***QIM steganography method***](https://www.google.com/search?q=QIM+steganography+method&sourceid=chrome&ie=UTF-8&source=chrome.ctxt) (*JPEG DCT-domain Quantization Index Modulation*), as this is the only storage method that currently works for ***Reddit***. The cover is transcoded to baseline Q75 4:2:0 and the payload is carried in its luminance DCT blocks.

To maximise storage capacity for the ***Reddit*** platform, use a cover image with large dimension sizes, **2048x2048**, **4096x4096**, **8192x8192 (max)**, etc.  

Quality of cover image is not important for this method and should be kept basic for the largest dimensions to help minimise cover image file size.

While the ***X-Twitter*** platform can use the default method provided by ***jdvrif***, where data is concealed within APP2/ICC segments, ***X-Twitter*** limits this to a single ICC segment with a maximum size of just **~10KiB**.

To carry more than that **~10KiB**, use the ***X-Twitter*** platform conceal mode (***-x***). It abandons metadata segments entirely — nothing is written to an ICC profile — and instead uses the [***adaptive J-UNIWARD steganography method with Syndrome-Trellis Coding (STC)***](https://www.google.com/search?q=adaptive+J-UNIWARD+steganography+method+with+Syndrome-Trellis+Coding+(STC)). The cover is transcoded to progressive 4:2:0 at its source-derived quality (capped at **Q97**).

To maximise storage capacity for the ***X-Twitter*** platform, use a high quality/detailed cover image with large dimension sizes, **1024x1024**, **2048x2048**, **4096x4096 (max)**, etc.

The ***-r*** and ***-x*** carriers encrypt your data and key its location to your recovery KEY using ***Argon2id***, but embedding still alters the image's DCT statistics, so dedicated steganalysis may reveal that an image carries hidden data.

Images created with ***jdvrif v9.x***, including ***-r*** and ***-x*** images from ***v9.0*** and ***v9.1***, remain recoverable with their numeric PINs, but ***v9.x*** cannot recover images created with ***v10***.

Both DCT modes carry far less data than the default mode, so use ***capsize*** to measure a cover image before choosing a payload (see [Checking capacity](#checking-capacity-with-capsize)).

## Requirements & Compilation (Linux)

Building requires **CMake 3.20 or newer**, `flock` from **util-linux**, and either **Ninja** (preferred) or **Make**. The compiler must be **GCC 14 or newer**, or **Clang 18 or newer** paired with a C++23 standard library that implements features such as `std::format` and `std::print`. The native libraries required are **libsodium**, **libjpeg-turbo** (*both the `turbojpeg` and `libjpeg` APIs*), **zlib** and **libdeflate**. **OpenMP** is optional: when present it parallelises the ***-x*** J-UNIWARD cost pass.

```console
$ sudo apt update
$ sudo apt install g++ cmake ninja-build util-linux libsodium-dev libturbojpeg0-dev libjpeg-dev zlib1g-dev libdeflate-dev

$ g++ --version   # confirm the reported version is 14 or newer

$ chmod +x compile_jdvrif.sh
$ ./compile_jdvrif.sh

$ sudo cp jdvrif /usr/bin
```

If your distribution ships GCC 14 as a versioned package, install `g++-14` and build with `CXX=g++-14 ./compile_jdvrif.sh`.

The wrapper keeps dependency-tracked object files under `src/build/`, so later invocations rebuild only what changed, and it replaces the published `jdvrif` binary only after a complete, successful build. Set `JDVRIF_JOBS=<count>` to change the parallelism limit (default: CPU count, capped at 8), `JDVRIF_BUILD_DIR=<path>` to use a separate build cache, or `BUILD_MODE=sanitize` for an ASan/UBSan build.

## Usage

```console
$ jdvrif

Usage: jdvrif conceal [-b|-r|-x] <cover_image> <secret_file>
       jdvrif recover <cover_image>
       jdvrif capsize [-r|-x] <cover_image>
       jdvrif --info
```

Run `jdvrif --info` for the full built-in guide to modes, platform options and size limits.

```console
$ jdvrif conceal your_cover_image.jpg your_secret_file.doc

Platform compatibility for output image:-

 ✓ X-Twitter
 ✓ Tumblr
 ✓ Mastodon
 ✓ Pixelfed
 ✓ PostImage
 ✓ ImgBB
 ✓ ImgPile
 ✓ Flickr

Recovery KEY: [***Jgoe-TsV4-hWLD-SMP9-PJt7-nPTd***]

Important: Keep your KEY safe, so that you can extract the hidden file.

Saved "file-embedded" JPG image: jrif_4e87c566c.jpg (143029 bytes).

Complete!

$ jdvrif recover jrif_4e87c566c.jpg

KEY: ****-****-****-****-****-****

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```

jdvrif ***mode*** arguments:

  ***conceal*** - Compresses, encrypts and embeds your secret data file within a ***JPG*** cover image.  
  ***recover*** - Decrypts, uncompresses and extracts the concealed data file from a ***JPG*** cover image (*recovery KEY/PIN required*).  
  ***capsize*** - Reports the carrier capacity of a cover image for ***-r*** or ***-x*** mode. No image is saved.

The ***recover*** prompt matches the image where it can:

| Image | Prompt |
| --- | --- |
| Default or ***-b*** image made with ***v10*** or later | `KEY:` |
| Default or ***-b*** image made before ***v10*** | `PIN:` |
| ***-r*** or ***-x*** image (*any version*) | `KEY/PIN:` |

The ***-r*** and ***-x*** carriers cannot be identified until the KEY or PIN has been entered, because it is what locates the hidden data. Error messages follow the same rule.

Requirements for the cover image:

● JPEG only, at least **400x400** pixels, and either grayscale or YCbCr colour — CMYK/YCCK images must be converted to RGB first.

● Default and ***-b*** upper dimensions are **4096x4096px**; ***-r*** uses **8192x8192px**, while ***-x*** is also capped at **4096x4096px**.

● The default mode (*no option*) also rejects cover images whose estimated JPEG quality is above **Q97**; re-save the image at a lower quality if it is refused.

Requirements for the secret data file:

● The embedded filename must be no longer than **20 bytes**, must not begin with `.` or `-` or end with a space or `.`, and must be valid UTF-8 without control or bidirectional-formatting characters or any of `/ \ : * ? " < > |`. The name is stored in the image and restored on ***recover***.

● Your data file is compressed before encryption, except for recognised already-compressed file types (`.zip`, `.7z`, `.mp4`, `.jpg`, `.png`, etc). In the default and ***-b*** modes those types skip compression only when larger than **10MiB**; in ***-r*** and ***-x*** modes they always skip it. For anything else destined for a small platform limit, consider compressing it yourself first (*zip, rar, 7z, etc.*) so that you know its exact stored size.

## Compatible Platforms

\******************   
Note: ***Bluesky*** now saves images as ***WEBP*** by default. 

To save an image as ***JPG***, so that you can still recover concealed data with ***jdvrif***:-  

First click the image in the post to open it, then right-click on the image. From the menu, select ***Open image in new tab***.  

Select the new tab and within the address bar, move to the end of the address and add ***@jpg*** then hit enter.  
Right-click the image and from the menu select ***Save image...***  

Your image should now be downloaded as a ***JPG***, which will now work with ***jdvrif***.
         
If you want a tool to conceal data using ***WEBP*** images to post on ***Bluesky*** you can use my ***WEBP*** steganography CLI tool ***[wbpdv](https://github.com/CleasbyCode/wbpdv)***  
\******************

*Posting size limit measured by the ***combined*** size of the ***cover image*** + ***compressed data file:****  

● ***Flickr*** (**200MiB**), ***ImgPile*** (**100MiB**), ***ImgBB*** (**32MiB**), ***PostImage*** (**32MiB**), ***Pixelfed*** (**15MiB**).

*Size limit measured ***only*** by the ***compressed data file size:****  

● ***Mastodon*** (**~6MiB**), ***Tumblr*** (**~64KiB**), ***X-Twitter*** (**~10KiB / default method**).  

For example, with ***Mastodon***, if your cover image is **1MiB** you can still embed a data file up to the **~6MiB** size limit.

**Other: platforms with their own conceal mode:**

● ***Bluesky*** (***-b option***). The finished "*file-embedded*" ***JPG*** must not exceed **2,000,000 bytes (~1.9MiB)**, so the cover image and the compressed data file share one budget. The compressed data file on its own must not exceed **~171KiB**. A cover image already at 2,000,000 bytes leaves no room at all, so keep the cover smaller than the limit by at least the size of your compressed data file. The "***create_bsky_post.py***" script is required to post these images on ***Bluesky***. *More info on this script further down the page.*

● ***Reddit*** (***-r option***). The cover image and the data file must each be no larger than **20MiB**, but the actual carrier capacity of the cover image is ***much smaller*** and depends on its dimension sizes. Use `jdvrif capsize -r` to measure it.

● ***X-Twitter*** (***-x option***). The cover image and the data file must each be no larger than **5MiB**, and the cover must not exceed **4096x4096** pixels. The actual carrier capacity is ***much smaller*** and depends on image quality and dimension sizes. Use `jdvrif capsize -x` to measure it.

In the default and ***-b*** modes, the cover image is also losslessly optimized before use and must not exceed **4MiB** after that step.

For platforms such as ***X-Twitter***, ***Reddit*** & ***Tumblr***, which have small data size limits, you may want to focus on data that compresses well, such as text files, etc.  

https://github.com/user-attachments/assets/af330d09-f65d-4f75-8290-883078be34b3

https://github.com/user-attachments/assets/fc454d42-0240-4864-b44b-ce5ef7cfd94c

## Checking capacity with capsize

***capsize*** prepares the cover image exactly as ***conceal*** would, then reports how much encrypted payload it can carry. Nothing is written to disk. Use ***-r*** for the ***Reddit*** carrier and ***-x*** for the ***X-Twitter*** carrier (***-x*** is the default if no option is given).

```console
$ jdvrif capsize -r basic_img_large_dims.jpg

Reddit capacity check for conceal -r mode only.

Cover Image: 384KiB, 8192x8192, Baseline YCbCr 4:2:0, Standard Q75 quantization (C3).

Theoretical C3 capacity limit for this cover image:                    436906 bytes (~426KiB).
Conservative maximum compressed capacity with a 20-character filename: 436792 bytes (~426KiB).
Recommended  maximum compressed capacity with a 20-character filename: 435768 bytes (~425KiB).
```

The figure reported is the total encrypted ***envelope*** capacity, not a raw secret-file limit: the filename, encryption and recovery metadata consume 95 to 114 bytes for a single-frame payload, and larger payloads add framing overhead. Don't aim at the theoretical limit — where capacity allows, keep the compressed payload at least **1KiB** below the conservative maximum. The size check performed by ***conceal*** is the authoritative one.

## Conceal mode platform options

To create compatible "*data-concealed*" ***JPG*** images for posting on the ***Reddit*** platform, you must use the ***-r*** option with ***conceal*** mode.
  ```console
  $ jdvrif conceal -r my_image.jpg hidden.doc
```

  These images are only compatible for posting on ***Reddit***. Your embedded data file will be lost if posted on a different platform.  
  
  When saving/downloading an image from ***Reddit*** make sure to click on the image within the post to fully expand it before saving.  

https://github.com/user-attachments/assets/ead3c34c-dd9a-4018-9ea3-0746fbdfdb8b

To create compatible "*data-concealed*" ***JPG*** images for posting on the ***X-Twitter*** platform using the J-UNIWARD steganography method, you must use the ***-x*** option with ***conceal*** mode.
  ```console
  $ jdvrif conceal -x my_image.jpg hidden.doc
```

  These images are only compatible for posting on ***X-Twitter***. Your embedded data file will be lost if posted on a different platform.  
  
  When saving/downloading an image from ***X-Twitter*** make sure to click on the image within the post to fully expand it before saving.  

To create compatible "*file-embedded*" ***JPG*** images for posting on the ***Bluesky*** platform, you must use the ***-b*** option with ***conceal*** mode.
  ```console
  $ jdvrif conceal -b my_image.jpg hidden.doc
```

  These images are only compatible for posting on ***Bluesky***. Your embedded data file will be removed if posted on a different platform.
 
  You are also required to use the Python script [create_bsky_post.py](https://github.com/CleasbyCode/jdvrif/blob/main/src/bsky/create_bsky_post.py) (found in the repo ***src/bsky*** folder) to post the image to ***Bluesky***.
  It will not work if you post images to ***Bluesky*** via the browser site or mobile app.  

  To use the script, you will need to create an [***app password***](https://bsky.app/settings/app-passwords) from your ***Bluesky*** account. Pass your credentials through the environment rather than on the command line, where they would be visible to other local users via tools such as `ps`:

  ```console
  $ pip install -r bsky/requirements.txt

  $ export ATP_AUTH_HANDLE='you.bsky.social'
  $ read -rsp 'Bluesky app password: ' ATP_AUTH_PASSWORD && export ATP_AUTH_PASSWORD

  $ python3 bsky/create_bsky_post.py \
      --image jrif_4e87c566c.jpg \
      --alt-text "alt-text here [optional]" \
      "standard post text here [required]"

  $ unset ATP_AUTH_PASSWORD
```

  See [src/bsky/README.md](https://github.com/CleasbyCode/jdvrif/blob/main/src/bsky/README.md) for the full set of options (*multiple images, replies, quote posts, link cards*) and for what the hardened fork of the script protects against.

https://github.com/user-attachments/assets/1daef508-d304-491f-bfe2-2cdbb5d62081

## Recovery KEY format

This section is for anyone writing a compatible implementation. Everything below is fixed by jdvrif v10.

***The KEY*** is 16 bytes from a cryptographic random number generator (*never all zero*). It is written as:

1. ***Body*** — the 16 bytes read as one 128-bit big-endian integer, written in Base58 as exactly 22 digits, most significant first, padded on the left with `1` (*the zero digit*). Alphabet (*Bitcoin's*): `123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz`.
2. ***Check*** — two more characters:

   ```
   digest = BLAKE2b-256(ASCII("jdvrif pin check v1") || key_bytes)   (19-byte label, no NUL; unkeyed)
   value  = big_endian_u32(digest[0..4]) mod 3364
   check  = alphabet[value / 58] || alphabet[value mod 58]
   ```
3. ***Display*** — the 24 characters in six groups of four, joined with `-`.

When a KEY is entered, spaces, tabs and `-` are ignored and letters are case-sensitive. A numeric entry of 1–20 digits (*no leading zero, at most 18446744073709551615*) is an old (*pre-v10*) PIN.

| Key bytes (hex) | KEY |
| --- | --- |
| `00000000000000000000000000000001` | `1111-1111-1111-1111-1111-126X` |
| `000000000000000000000000000003d3` | `1111-1111-1111-1111-1111-Ht11` (*check value 0*) |
| `00000000000000000000000000000abf` | `1111-1111-1111-1111-1111-pSzz` (*check value 3363*) |
| `000102030405060708090a0b0c0d0e0f` | `112d-rXXU-ifSr-RnXL-GbXg-8Egs` |
| `8f3a1c5e9b2d4f60718293a4b5c6d7e8` | `Jgoe-TsV4-hWLD-SMP9-PJt7-nPTd` |
| `ffffffffffffffffffffffffffffffff` | `YcVf-xkQb-6JRz-qk5k-F2tN-LvUM` |

***Encryption key*** — Argon2id (*libsodium `crypto_pwhash`, ALG_ARGON2ID13*) with the 16 raw key bytes as the password and the salt and costs stored in the image (*metadata version KDF5*).

***Carrier key (-r and -x)*** — the first 16 bytes of Argon2id(password = the 16 key bytes, salt = ASCII `jdvrif-carrierv3`, opslimit 2, memlimit 64 MiB, 32-byte output). For key bytes `000102…0f` it is `f3559f3f621bca57b955a0c2a772c7e1`.

***Carrier positions (-r RQSTEG3, -x JXSTEG4)*** — every keyed value is `PRF(d, x) = SipHash-2-4(carrier_key, LE64(d) || LE64(x))`. Positions are a front-to-back Fisher–Yates shuffle of all candidate coefficients in scan order, drawing each swap from its own PRF call:

```
counter = 0
for i = 0 .. min(count, n - 1) - 1:
    bound     = n - i
    threshold = (2^64 - bound) mod bound
    repeat: w = PRF(layout_domain, counter); counter += 1   until w >= threshold
    swap(items[i], items[i + (w mod bound)])
```

The first `count` items are the positions used, in order (*-x: the header's positions, then the payload's, from one shuffle; -r: one shuffle each for header and payload*). Each draw depends on the full 128-bit carrier key. Position draws use their own domain constants, separate from those for whitening and dither, so no position draw reuses a whitening or dither value. This hides *where* the data is. It does not stop statistical steganalysis from detecting that an image carries hidden data.

## Tests

The test scripts in `src/tests` each take the binary to exercise:

```console
$ bash tests/run_golden_tests.sh --bin ./jdvrif        # recover pre-built images with known PINs
$ bash tests/run_roundtrip_tests.sh --bin ./jdvrif     # fresh conceal/recover round-trips
$ bash tests/run_security_smoke.sh --bin ./jdvrif      # negative-path and boundary checks
$ bash tests/run_reddit_tests.sh --bin ./jdvrif        # -r QIM carrier
$ bash tests/run_twitter_tests.sh --bin ./jdvrif       # -x J-UNIWARD/STC carrier
$ bash tests/run_legacy_fixture_tests.sh --bin ./jdvrif # recover v9.0/v9.1 images with their numeric PINs
$ bash tests/run_io_security_tests.sh --bin ./jdvrif   # input-size bounds, KEY/PIN entry signal/job-control handling
$ bash tests/run_jpeg_safety_tests.sh                  # JPEG parser/decoder safety (builds its own test binary)
$ bash tests/run_bsky_tests.sh                         # Bluesky posting helper
```

## Third-Party Software and Assets

  ### Core applications

  - [libsodium](https://github.com/jedisct1/libsodium) — cryptographic random generation, Argon2id
  key derivation and XChaCha20-Poly1305 secret streams. Dynamically linked as a system library.
      
      License: [ISC License](https://github.com/jedisct1/libsodium/blob/master/LICENSE)
    
      Copyright (c) 2013–2026 Frank Denis.
    
 - [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) — JPEG processing and lossless transformation. Dynamically linked as a system library.

      This software is based in part on the work of the Independent JPEG Group.

      Licenses: [Independent JPEG Group License, Modified BSD 3-Clause License,
      and zlib License](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/2.1.5/LICENSE.md).

      Copyright © 1991–2020 Thomas G. Lane and Guido Vollbeding.
   
      Copyright © 2009–2023 D. R. Commander. All Rights Reserved.
   
      Copyright © 2015 Viktor Szathmáry. All Rights Reserved.
   
  - [zlib](https://github.com/madler/zlib) — Streaming zlib compression and decompression. Dynamically linked as a system library.

    License: [zlib License](https://github.com/madler/zlib/blob/develop/LICENSE)
    
    Copyright (C) 1995–2026 Jean-loup Gailly and Mark Adler.

  - [libdeflate](https://github.com/ebiggers/libdeflate) — Fast whole-buffer zlib-format compression. Dynamically linked as a system library.

    License: [MIT](https://github.com/ebiggers/libdeflate/blob/master/COPYING)
    
    Copyright 2016 Eric Biggers.
    
    Copyright 2024 Google LLC.

  ### Incorporated code and assets

  - [Compact ICC Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles) — embedded Adobe-
  compatible ICC profile.

    License: [CC0 1.0 Universal](https://github.com/saucecontrol/Compact-ICC-Profiles/blob/master/license)

  ### Optional Bluesky posting helper

  - Bryan Newbold / ATProto Hacker Cookbook — create_bsky_post.py — Basis for the [forked](https://gist.github.com/CleasbyCode/1eb678ca1fa1975b1c1e20aeec33637e) Bluesky posting helper (src/bsky/create_bsky_post.py). 
    For reference see the [Cookbook copy](https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py)

    License: [CC0 1.0 Universal](https://github.com/bluesky-social/cookbook/blob/main/LICENSE-CC0).

  - Requests — HTTP and Bluesky API requests.

    License: [Apache 2.0](https://github.com/psf/requests/blob/main/LICENSE) / [NOTICE](https://github.com/psf/requests/blob/main/NOTICE)
    
    Copyright 2019 Kenneth Reitz.

  - Beautiful Soup 4 — HTML and Open Graph metadata parsing.

    License: [MIT](https://pypi.org/project/beautifulsoup4/)
    
    Copyright (c) Leonard Richardson.

  - Pillow — Image validation, dimensions, and aspect-ratio handling.

    License: [MIT-CMU](https://github.com/python-pillow/Pillow/blob/main/LICENSE)
    
    PIL copyright © 1997–2011 Secret Labs AB and © 1995–2011 Fredrik Lundh and contributors.
    
    Pillow copyright © 2010 Jeffrey “Alex” Clark and contributors.
    
##
