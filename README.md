# jdvrif
**JDVRIF IS CURRENTLY BEING UPDATED!....

A steganography command-line tool used for embedding and extracting any file type via a **JPG** cover image.  

There is also a ***jdvrif Web App***, which you can try [***here***](https://cleasbycode.co.uk/jdvrif/index/) as a convenient alternative to downloading and compiling the CLI source code. Web file uploads are limited to **20MB**.    

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_60228.jpg)  
*Image credit: **"Camouflage"** is the work of [***@carochan_me***](https://x.com/carochan_me) / ***PIN: 11455761492008362387****

Unlike the common steganography method of concealing data within the pixels of a cover image ([***LSB***](https://ctf101.org/forensics/what-is-stegonagraphy/)), ***jdvrif*** hides files within ***application segments*** of a ***JPG*** image. 

You can embed any file type up to ***2GB***, although compatible sites (*listed below*) have their own ***much smaller*** size limits and *other requirements.  

For increased storage capacity and better security, your embedded data file is compressed with ***zlib/deflate*** and encrypted using the ***libsodium*** cryptographic library.  

***jdvrif*** partly derives from the ***[technique implemented](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 
## Compatible Platforms
*Posting size limit measured by the combined size of the cover image + compressed data file:*  

● ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** | ***-r option***).  

*Size limit measured only by the compressed data file size:*  

● ***Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***X-Twitter*** (**~10KB**).  

For example, with ***Mastodon***, if your cover image is **1MB** you can still embed a data file up to the **~6MB** size limit.

**Other: The ***Bluesky*** platform has seperate size limits for the cover image and the compressed data file:*  

● ***Bluesky*** (***-b option***). Cover image size limit (**800KB**). Compressed data file size limit (**106KB**).  
● "***bsky_post.py***" Python script is required to post images on ***Bluesky***. *More info on this further down the page.*

Even though ***jdvrif*** will compress your data file, you may wish to compress the file yourself (zip, rar, 7z, etc.)  
before embedding it with ***jdvrif***, so as to know exactly what the compressed data file size will be.   

For platforms such as ***X-Twitter*** & ***Tumblr***, which have small size limits, you may want to focus on data files  
that compress well, such as .txt documents, etc.
  
## Usage (Linux)

```console

user1@mx:~/Downloads/jdvrif-main/src/jdvrif$ sudo apt-get install libsodium-dev
user1@mx:~/Downloads/jdvrif-main/src/jdvrif$ sudo apt-get install libturbojpeg0-dev
user1@mx:~/Downloads/jdvrif-main/src/jdvrif$ chmod +x compile_jdvrif.sh
user1@mx:~/Downloads/jdvrif-main/src/jdvrif$ ./compile_jdvrif.sh
user1@mx:~/Downloads/jdvrif-main/src/jdvrif$ Compilation successful. Executable 'jdvrif' created.
user1@mx:~/Downloads/jdvrif-main/src/jdvrif$ sudo cp jdvrif /usr/bin

user1@mx:~/Desktop$ jdvrif 

Usage: jdvrif conceal [-b|-r] <cover_image> <secret_file>
       jdvrif recover <cover_image>  
       jdvrif --info

user1@mx:~/Desktop$ jdvrif conceal your_cover_image.jpg your_secret_file.doc
  
Saved "file-embedded" JPG image: jrif_12462.jpg (143029 bytes).

Recovery PIN: [***2166776980318349924***]

Important: Keep your PIN safe, so that you can extract the hidden file.

Complete!
        
user1@mx:~/Desktop$ jdvrif recover jrif_12462.jpg

PIN: *******************

Extracted hidden file: your_secret_file.doc (6165 bytes).

Complete! Please check your file.

```
To correctly download images from ***X-Twitter*** or ***Reddit***, click the image in the post to ***fully expand it***, before saving.  

https://github.com/user-attachments/assets/7b6485f2-969d-47d4-86a7-c9b22920ee0a

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Reddit***, use the ***-r*** option with ***jdvin***.  
From the ***Reddit*** site, click "*Create Post*" then select "*Images & Video*" tab, to post your ***JPG*** image.  

https://github.com/user-attachments/assets/28553eaa-4162-43c5-b596-f6ab676c1b61

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Bluesky***, use the ***-b*** option with ***jdvin***.

For ***Bluesky***, you are required to use the ***Python*** script "*bsky_post.py*" (found in the repo ***src/bsky*** folder), to post the image.
It will not work if you post images via the ***Bluesky*** browser site or mobile app.

Bluesky script example:
```console
$ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka --image jrif_11050.jpg --alt-text "text to describe image..." "text to appear in main post...*"
```
You will need to create an app password from your ***Bluesky*** account. (*https://bsky.app/settings/app-passwords*)

https://github.com/user-attachments/assets/dcc7c31d-4bec-4741-81e5-3b70fd6c29f5

https://github.com/user-attachments/assets/e5d2e0f1-d110-4712-8334-b1394d59f3dd

With ***X-Twitter,*** ***Bluesky,*** & ***Tumblr***, the small size limits are measured by the ***data file size*** and not the combined image + data file size.
As the embedded data file is compressed with ***jdvin*** using ***zlib/deflate*** (*if not already a compressed file type*), you should be able to get significantly more than the default size limit, especially for text documents and other file types that compress well. You may wish to compress the data file yourself (***zip, rar, 7z***, etc) before embedding it with ***jdvin***, so as to know exactly what the compressed file size will be.

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
