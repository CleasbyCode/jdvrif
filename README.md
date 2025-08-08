# jdvrif

A steganography command-line tool used for embedding and extracting any file type via a **JPG** cover image.  

There is also a Web App version, which you can try [***here***](https://cleasbycode.co.uk/jdvrif/index/) as a convenient alternative to downloading and compiling the CLI source code. Web file uploads are limited to **20MB**.    

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_73184.jpg)  
*Image: **"A place of concealment"** / ***PIN: 17129461748195490639****

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***jdvrif*** hides files within ***application segments*** of a ***JPG*** image. 

You can embed any file type up to ***2GB***, although compatible sites (*listed below*) have their own ***much smaller*** size limits and *other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***zlib/deflate*** and encrypted using the ***libsodium*** cryptographic library.  

***jdvrif*** partly derives from the ***[technique implemented](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

https://github.com/user-attachments/assets/8d21cdbb-30f0-424c-aeb8-bcacca8c4255

https://github.com/user-attachments/assets/a7830132-55eb-42d9-88b8-49e16427b39a

## Compatible Platforms
*Posting size limit measured by the combined size of the cover image + compressed data file:*  

● ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** | ***-r option***).  

*Size limit measured only by the compressed data file size:*  

● ***Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***X-Twitter*** (**~10KB**).  

For example, with ***Mastodon***, if your cover image is **1MB** you can still embed a data file up to the **~6MB** size limit.

**Other: The ***Bluesky*** platform has separate size limits for the cover image and the compressed data file:*  

● ***Bluesky*** (***-b option***). Cover image size limit (**800KB**). Compressed data file size limit (**~106KB**).  
● "***bsky_post.py***" script is required to post images on ***Bluesky***. *More info on this further down the page.*

For platforms such as ***X-Twitter*** & ***Tumblr***, which have small size limits, you may want to focus on data files  
that compress well, such as .txt documents, etc.  
  
## Usage (Linux)

```console

user1@mx:~/Downloads/jdvrif-main/src$ sudo apt-get install libsodium-dev
user1@mx:~/Downloads/jdvrif-main/src$ sudo apt-get install libturbojpeg0-dev
user1@mx:~/Downloads/jdvrif-main/src$ chmod +x compile_jdvrif.sh
user1@mx:~/Downloads/jdvrif-main/src$ ./compile_jdvrif.sh
user1@mx:~/Downloads/jdvrif-main/src$ Compilation successful. Executable 'jdvrif' created.
user1@mx:~/Downloads/jdvrif-main/src$ sudo cp jdvrif /usr/bin

user1@mx:~/Desktop$ jdvrif 

Usage: jdvrif conceal [-b|-r] <cover_image> <secret_file>
       jdvrif recover <cover_image>  
       jdvrif --info

user1@mx:~/Desktop$ jdvrif conceal your_cover_image.jpg your_secret_file.doc

Platform compatibility for output image:-

  ✓ X-Twitter
  ✓ Tumblr
  ✓ Mastodon
  ✓ PostImage
  ✓ ImgBB
  ✓ ImgPile
  ✓ Flickr
  
Saved "file-embedded" JPG image: jrif_12462.jpg (143029 bytes).

Recovery PIN: [***2166776980318349924***]

Important: Keep your PIN safe, so that you can extract the hidden file.

Complete!
        
user1@mx:~/Desktop$ jdvrif recover jrif_12462.jpg

PIN: *******************

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```
jdvrif ***mode*** arguments:
 
  ***conceal*** - Compresses, encrypts and embeds your secret data file within a ***JPG*** cover image.  
  ***recover*** - Decrypts, uncompresses and extracts the concealed data file from a ***JPG*** cover image.
 
jdvrif ***conceal*** mode platform options:
 
  "***-b***" - To create compatible "*file-embedded*" ***JPG*** images for posting on the ***Bluesky*** platform, you must use the ***-b*** option with ***conceal*** mode.
  ```console
  $ jdvrif conceal -b my_image.jpg hidden.doc
  ```
  These images are only compatible for posting on ***Bluesky***. Your embedded data file will be removed if posted on a different platform.
 
  You are required to use the Python script ***"bsky_post.py"*** (found in the repo ***src*** folder) to post the image to ***Bluesky***.
  It will not work if you post images to ***Bluesky*** via the browser site or mobile app.

  Script example:
  
  ```console
   $ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka
    --image jrif_11050.jpg --alt-text "text to describe image" "text to appear in main post"
  ```
   You will also need to create an ***app password*** from your ***Bluesky*** account, to use with the ***bsky_post.py*** script. (https://bsky.app/settings/app-passwords).  

https://github.com/user-attachments/assets/b4c72ea7-40e3-49b0-89aa-ae2dd8ccccb9   

https://github.com/user-attachments/assets/a84ffb6d-b5ce-4f66-bf65-7589d2d93a49

   "***-r***" - To create compatible "*file-embedded*" ***JPG*** images for posting on the ***Reddit*** platform, you must use the ***-r*** option with ***conceal*** mode.
   ```console
  $ jdvrif conceal -r my_image.jpg secret.mp3 
   ```
   From the ***Reddit*** site, select "***Create Post***" followed by "***Images & Video***" tab, to attach and post your ***JPG*** image.
  
   These images are only compatible for posting on the ***Reddit***. Your embedded data file will be removed if posted on a different platform.
  
 To correctly download images from ***X-Twitter*** or ***Reddit***, click the image in the post to fully expand it, before saving.

https://github.com/user-attachments/assets/f56f54bb-658f-4b0e-a2f3-7d3428333304

## Third-Party Libraries

This project makes use of the following third-party libraries:

- **libsodium**: For cryptographic functions.
  - [**LICENSE**](https://github.com/jedisct1/libsodium/blob/master/LICENSE)
  - Copyright (C) 2013-2025 Frank Denis (github@pureftpd.org)
- libjpeg-turbo (see [***LICENSE***](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md) file)  
  - {This software is based in part on the work of the Independent JPEG Group.}
  - Copyright (C) 2009-2024 D. R. Commander. All Rights Reserved.
  - Copyright (C) 2015 Viktor Szathmáry. All Rights Reserved.
- **zlib**: General-purpose compression library
  - License: zlib/libpng license (see [***LICENSE***](https://github.com/madler/zlib/blob/develop/LICENSE) file)
  - Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
    
##
