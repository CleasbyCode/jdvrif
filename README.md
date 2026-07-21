# jdvrif

***jdvrif*** is a fast, easy-to-use steganography command-line tool for concealing and extracting any file type via a **JPG** image.  

There is also a [***Web edition***](https://cleasbycode.co.uk/jdvrif/app/), which you can use immediately, as a convenient alternative to downloading and compiling the CLI source code. Web file uploads are limited to **20MB**.    

An experimental ***Rust*** port [***jdvrif-rs***](https://github.com/CleasbyCode/jdvrif-rs) is available for those interested in that language. 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_323291.jpg)  
*Demo Image: **"A place of concealment"** / ***PIN: 2190398302048725932****

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***jdvrif*** hides files within ***application segments*** of a ***JPG*** image. 

You can conceal any file type up to ***2GB***, although compatible sites (*listed below*) have their own ***much smaller*** size limits and *other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***libdeflate/zlib*** — unless it's already a compressed file type over 10 MB — and encrypted with ***XChaCha20-Poly1305*** using the ***libsodium*** cryptographic library.

***jdvrif*** partly derives from the ***[technique implemented](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

## Compilation & Usage (Linux)

```console
$ sudo apt-get update
$ sudo apt install g++ cmake ninja-build util-linux libsodium-dev libturbojpeg0-dev zlib1g-dev libdeflate-dev

$ chmod +x compile_jdvrif.sh
$ ./compile_jdvrif.sh

$ sudo cp jdvrif /usr/bin
$ jdvrif 

Usage: jdvrif conceal [-b|-r] <cover_image> <secret_file>
       jdvrif recover <cover_image>  
       jdvrif --info

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
  
Saved "file-embedded" JPG image: jrif_4e87c566c.jpg (143029 bytes).

Recovery PIN: [***2166776980318349924***]

Important: Keep your PIN safe, so that you can extract the hidden file.

Complete!
        
$ jdvrif recover jrif_4e87c566c.jpg

PIN: *******************

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```
## Compatible Platforms
\******************   
Note: ***Bluesky*** now saves images as ***WEBP*** by default. 

To save an image as ***JPG***, so that you can still recover concealed data with ***jdvrif***,  
right-click on an image that you want to save. From the menu, select ***Open image in new tab***.  

Select the new tab and within the address bar, move to the end of the address and add ***@jpg*** then hit enter.  
Right-click the image and from the menu select ***Save image...***  

Your image should now be downloaded as a ***JPG***, which will now work with ***jdvrif***.
         
If you want a tool to conceal data using ***WEBP*** images to post on ***Bluesky*** you can use my ***WEBP*** steganography CLI tool ***[wbpdv](https://github.com/CleasbyCode/wbpdv)***  
\******************

*Posting size limit measured by the ***combined*** size of the ***cover image*** + ***compressed data file:****  

● ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**),  
● ***PostImage*** (**32MB**), ***Reddit*** (**20MB** | ***-r option***), ***Pixelfed*** (**15MB**).

*Size limit measured ***only*** by the ***compressed data file size:****  

● ***Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***X-Twitter*** (**~10KB**).  

For example, with ***Mastodon***, if your cover image is **1MB** you can still embed a data file up to the **~6MB** size limit.

**Other: The ***Bluesky*** platform has ***separate*** size limits for the ***cover image*** and the ***compressed data file:****  

● ***Bluesky*** (***-b option***). Cover image size limit (**800KB**). Compressed data file size limit (**~171KB**).  
● "***bsky_post.py***" script is required to post images on ***Bluesky***. *More info on this further down the page.*

For platforms such as ***X-Twitter*** & ***Tumblr***, which have small size limits, you may want to focus on data that compress well, such as text files, etc.  

https://github.com/user-attachments/assets/c8c38e6d-ea23-4d67-98d9-cebdcd82b449

https://github.com/user-attachments/assets/88aaa5f7-3272-4d0c-aa59-1a5bfe2f08dc
  
jdvrif ***mode*** arguments:
 
  ***conceal*** - Compresses, encrypts and embeds your secret data file within a ***JPG*** cover image.  
  ***recover*** - Decrypts, uncompresses and extracts the concealed data file from a ***JPG*** cover image.
 
jdvrif ***conceal*** mode ***platform*** options:
 
  "***-b***" To create compatible "*file-embedded*" ***JPG*** images for posting on the ***Bluesky*** platform, you must use the ***-b*** option with ***conceal*** mode.
  ```console
  $ jdvrif conceal -b my_image.jpg hidden.doc
```

  These images are only compatible for posting on ***Bluesky***. Your embedded data file will be removed if posted on a different platform.
 
  You are also required to use the Python script ***"bsky_post.py"*** (found in the repo ***src*** folder) to post the image to ***Bluesky***.
  It will not work if you post images to ***Bluesky*** via the browser site or mobile app.  

  To use the script, you will need to create an [***app password***](https://bsky.app/settings/app-passwords) from your ***Bluesky*** account.  

  Here are some basic usage examples for the ***bsky_post.py*** script.  

  Standard image post to your bsky profile:

  ```console
  $ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx --image your_image.jpg --alt-text "alt-text here (optional)" "standard post text here (required)"
  ```
  If you want to post multiple images (Max. 4):  

  ```console 
  $ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx --image img1.jpg --image img2.jpg --alt-text "alt_here" "standard post text..."
  ```
  If you want to post an image as a reply to another post:  

  ```console
  $ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx --image your_image.jpg --alt-text "alt_here" --reply-to https://bsky.app/profile/someone.bsky.social/post/8m2tgw6cgi23i "standard post text..."
  ```

https://github.com/user-attachments/assets/b4c72ea7-40e3-49b0-89aa-ae2dd8ccccb9   

   "***-r***" To create compatible "*file-embedded*" ***JPG*** images for posting on the ***Reddit*** platform, you must use the ***-r*** option with ***conceal*** mode.
   ```console
  $ jdvrif conceal -r my_image.jpg secret.mp3 
   ```
   From the ***Reddit*** site, select "***Create Post***" followed by "***Images & Video***" tab, to attach and post your ***JPG*** image.
  
   These images are only compatible for posting on ***Reddit***. Your embedded data file will be removed if posted on a different platform.
  
 To correctly download images from ***X-Twitter*** or ***Reddit***, click the image in the post to fully expand it, before saving.

https://github.com/user-attachments/assets/f56f54bb-658f-4b0e-a2f3-7d3428333304

## Third-Party Software and Assets

  ### Core applications

  - [libsodium](https://github.com/jedisct1/libsodium) — Cryptographic random generation, password hashing, authenticated encryption, secure
    memory clearing, and Base64 decoding.
    License: [ISC](https://github.com/jedisct1/libsodium/blob/master/LICENSE)

    Copyright (c) 2013–2026 Frank Denis.

 - [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) 2.1.5
    (TurboJPEG API) — JPEG processing and lossless transformation.

      Copyright © 1991–2020 Thomas G. Lane and Guido Vollbeding.
   
      Copyright © 2009–2023 D. R. Commander. All Rights Reserved.
   
      Copyright © 2015 Viktor Szathmáry. All Rights Reserved.
   
      Licenses: [Independent JPEG Group License, Modified BSD 3-Clause License,
      and zlib License](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/2.1.5/LICENSE.md).

      This software is based in part on the work of the Independent JPEG Group.
   
  - [zlib](https://github.com/madler/zlib) — Streaming zlib compression and decompression.
    License: [zlib License](https://github.com/madler/zlib/blob/develop/LICENSE)
    
    Copyright (C) 1995–2026 Jean-loup Gailly and Mark Adler.

  - [libdeflate](https://github.com/ebiggers/libdeflate) — Fast whole-buffer zlib-format compression.
    License: [MIT](https://github.com/ebiggers/libdeflate/blob/master/COPYING)
    
    Copyright 2016 Eric Biggers.
    
    Copyright 2024 Google LLC.

  ### Incorporated code and assets

  - [base64simd](https://github.com/WojciechMula/base64simd) — The AVX2 Base64 encoder is adapted from Wojciech Muła’s vector Base64
    implementation.
    License: [BSD 2-Clause](https://github.com/WojciechMula/base64simd/blob/master/LICENSE)
    
    Copyright (c) 2015–2018, Wojciech Muła. All rights reserved.

  - [Compact ICC Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles) — embedded Adobe-
  compatible ICC profile.
    License: [CC0 1.0 Universal](https://github.com/saucecontrol/Compact-ICC-Profiles/blob/master/license)

  ### Optional Bluesky posting helper

  - Bryan Newbold / ATProto Hacker Cookbook — create_bsky_post.py — Basis for the forked Bluesky posting helper (src/bsky/bsky_post.py). 
    For reference see the [Cookbook copy](https://github.com/bluesky-social/cookbook/blob/main/python-bsky-post/create_bsky_post.py)
    License: [CC0 1.0 Universal](https://github.com/bluesky-social/cookbook/blob/main/LICENSE-CC0).

  - Requests — HTTP and Bluesky API requests.
    License: [Apache 2.0](https://github.com/psf/requests/blob/main/LICENSE)
    [NOTICE](https://github.com/psf/requests/blob/main/NOTICE)
    
    Copyright 2019 Kenneth Reitz.

  - Beautiful Soup 4 — HTML and Open Graph metadata parsing.
    License: [MIT](https://pypi.org/project/beautifulsoup4/)
    
    Copyright (c) Leonard Richardson.

  - Pillow — Image validation, dimensions, and aspect-ratio handling.
    License: [MIT-CMU](https://github.com/python-pillow/Pillow/blob/main/LICENSE)
    
    PIL copyright © 1997–2011 Secret Labs AB and © 1995–2011 Fredrik Lundh and contributors.
    
    Pillow copyright © 2010 Jeffrey “Alex” Clark and contributors.
    
##
