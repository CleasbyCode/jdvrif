# jdvrif

***jdvrif*** is a *"steganography-like"* utility for ***Linux*** and ***Windows***. It consists of two CLI tools, ***jdvin***, *used for embedding a data file within a ***JPG*** cover image*, and ***jdvout***, *used for extracting the hidden file from the cover image.*  

There is also a [***jdvrif Web App,***](https://cleasbycode.co.uk/jdvrif/index/) available to use, if you don't want to download and compile the CLI source code.  
*Web file uploads are limited to 20MB.*    

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_60228.jpg)  
*Image credit: **Camouflage** is the work of [***@carochan_me***](https://x.com/carochan_me) / ***PIN: 11455761492008362387****

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***jdvrif*** hides files within ***application segments*** of a ***JPG*** image. You can embed any file type up to ***2GB***, although compatible hosting sites (listed below) have their own ***much smaller*** size limits and *other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***zlib/deflate*** (*if not already a compressed file type*) and encrypted using the ***libsodium*** cryptographic library.  

***jdvrif*** partly derives from the ***[technique implemented](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

*Limit measured by the combined size of the cover image + compressed data file:*  
● ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** | ***-r option***).  

*Limit measured by just the compressed data file size:*  
● ***Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***Twitter*** (**~10KB**).  

**Other:*  
● ***Bluesky*** (***Image:*** **800KB** | ***Compressed data file:*** **~106KB** | ***-b option***).  
*Use the "***bsky_post.py***" script, found within the ***src folder*** of this repo, to post images on ***Bluesky***.*
  
## Usage (Linux - jdvin / jdvout)

```console

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo apt-get install libsodium-dev
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo apt-get install libturbojpeg-dev
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ g++ main.cpp -O2 -lz -lsodium -lturbojpeg -s -o jdvin
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo cp jdvin /usr/bin

user1@linuxbox:~/Desktop$ jdvin 

Usage: jdvin [-b|-r] <cover_image> <secret_file>  
       jdvin --info

user1@linuxbox:~/Desktop$ jdvin Cover_Image.jpg Hidden_File.zip
  
Saved "file-embedded" JPG image: jrif_12462.jpg (143029 bytes).

Recovery PIN: [***2166776980318349924***]

Important: Keep your PIN safe, so that you can extract the hidden file.

Complete!

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ g++ main.cpp -O2 -lz -lsodium -s -o jdvout
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ sudo cp jdvout /usr/bin

user1@linuxbox:~/Desktop$ jdvout

Usage: jdvout <file_embedded_image>
       jdvout --info
        
user1@linuxbox:~/Desktop$ jdvout jrif_12462.jpg

PIN: *******************

Extracted hidden file: Hidden_File.zip (6165 bytes).

Complete! Please check your file.

```
To correctly download images from ***X/Twitter*** or ***Reddit***, click the image in the post to ***fully expand it***, before saving.  

https://github.com/user-attachments/assets/7b6485f2-969d-47d4-86a7-c9b22920ee0a

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Reddit***, use the ***-r*** option with ***jdvin***.  
From the ***Reddit*** site, click "*Create Post*" then select "*Images & Video*" tab, to post your ***JPG*** image.  

https://github.com/user-attachments/assets/28553eaa-4162-43c5-b596-f6ab676c1b61

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Bluesky***, use the ***-b*** option with ***jdvin***.

For ***Bluesky***, you are required to use the ***Python*** script "*bsky_post.py*" (found in the repo ***src*** folder), to post the image.
It will not work if you post images via the ***Bluesky*** browser site or mobile app.

Bluesky script example:
```console
$ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka --image jrif_11050.jpg --alt-text "*text to describe image, here...*" "*standard text to appear in main post, here...*"
```
You will need to create an app password from your ***Bluesky*** account. (*https://bsky.app/settings/app-passwords*)

https://github.com/user-attachments/assets/dcc7c31d-4bec-4741-81e5-3b70fd6c29f5

https://github.com/user-attachments/assets/b4dee070-2325-4fbc-bcc2-62eea24b2a69

With ***X/Twitter,*** ***Bluesky,*** & ***Tumblr***, the small size limits are measured by the ***data file size*** and not the combined image + data file size.
As the embedded data file is compressed with ***jdvin*** using ***zlib/deflate*** (*if not already a compressed file type*), you should be able to get significantly more than the default size limit, especially for text documents and other file types that compress well. You may wish to compress the data file yourself (***zip, rar, 7z***, etc) before embedding it with ***jdvin***, so as to know exactly what the compressed file size will be.

Also with ***Mastodon***, the size limit is measured by the ***data file size*** and not the combined image + data file size.  
For example, if your cover image is **1MB** you can still embed a data file up to the **~6MB** ***Mastodon*** size limit.

https://github.com/user-attachments/assets/ba338a2b-5c38-4cb7-808b-83a642fc618c

https://github.com/user-attachments/assets/5a9fb804-3354-44ce-ab09-064d446bde42

To correctly download an image from ***Flickr***, click the download arrow near the bottom right-hand corner of the page and select ***Original*** for the size of image to download.

https://github.com/user-attachments/assets/3f393e2c-145f-49ab-a952-d2b120bad9f9

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

