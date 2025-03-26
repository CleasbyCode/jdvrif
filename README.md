# jdvrif

Use CLI tools ***jdvin*** & ***jdvout*** with a JPG image, to hide/extract any file type, up to ***2GB**.  

*Compatible hosting sites, ***listed below***, have their own ***much smaller*** image + data size ***limits*** and other requirements.  

*Limit measured by the combined size of the cover image + compressed data file:*  
***__Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** | ***-r option***).  

*Limit measured by just the compressed data file size:*  
***__Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***Twitter*** (**~10KB**)  
*Other:*  
***__Bluesky*** (***Image:*** **800KB** | ***Compressed data file:*** **~106KB** | ***-b option***). ***Use the *bsky_post.py* script (found within the src folder of this repo) to upload & post image to Bluesky.***
  
***jdvrif*** partly derives from the ***[technique implemented](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_70001.jpg)  
***Image credit:*** [***@unpaidactor1***](https://x.com/unpaidactor1) / ***PIN: 7505403832509957594***

For increased storage capacity and better security, your embedded data file is compressed with ***zlib/deflate*** (*if not already a compressed file type*) and encrypted using the ***libsodium*** crypto library. 

*You can try the [***jdvrif Web App, here,***](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download and compile the CLI source code.*

## Usage (Linux - jdvin / jdvout)

```console

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo apt-get install libsodium-dev
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ g++ main.cpp -O2 -lz -lsodium -s -o jdvin
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

https://github.com/user-attachments/assets/b54dd925-2c0b-4fbf-890e-4ea5cc197292

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Reddit***, use the ***-r*** option with ***jdvin***.  
From the ***Reddit*** site, click "*Create Post*" then select "*Images & Video*" tab, to post your ***JPG*** image.  

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Bluesky***, use the ***-b*** option with ***jdvin***.

For ***Bluesky***, you are required to use the ***Python*** script "*bsky_post.py*" (found in the repo ***src*** folder), to post the image.
It will not work if you post images via the ***Bluesky*** browser site or mobile app.

Bluesky script example:

$ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka --image jrif_11050.jpg --alt-text "*text to describe image, here...*" "*standard text to appear in main post, here...*"

You will need to create an app password from your ***Bluesky*** account. (*https://bsky.app/settings/app-passwords*)

https://github.com/user-attachments/assets/dcc7c31d-4bec-4741-81e5-3b70fd6c29f5

https://github.com/user-attachments/assets/41ee5e7f-7ae0-4f55-93ba-ba2dc9a027ab

With ***X/Twitter,*** ***Bluesky,*** & ***Tumblr***, the small size limits are measured by the ***data file size*** and not the combined image + data file size.
As the embedded data file is compressed with ***jdvin*** using ***zlib/deflate*** (*if not already a compressed file type*), you should be able to get significantly more than the default size limit, especially for text documents and other file types that compress well. You may wish to compress the data file yourself (***zip, rar, 7z***, etc) before embedding it with ***jdvin***, so as to know exactly what the compressed file size will be.

Also with ***Mastodon***, the size limit is measured by the ***data file size*** and not the combined image + data file size.  
For example, if your cover image is **1MB** you can still embed a data file up to the **~6MB** ***Mastodon*** size limit.

https://github.com/user-attachments/assets/cbae0361-bbb7-433c-a9a6-851c74940cd9

https://github.com/user-attachments/assets/7e243233-ae8d-4220-9dfd-eff6d28b8f2b  

To correctly download an image from ***Flickr***, click the download arrow near the bottom right-hand corner of the page and select ***Original*** for the size of image to download.

https://github.com/user-attachments/assets/e284979c-9c73-487e-bd2a-57504e897257

## Third-Party Libraries

This project makes use of the following third-party libraries:

- [**libsodium**](https://libsodium.org/) for cryptographic functions.
  - [**LICENSE**](https://github.com/jedisct1/libsodium/blob/master/LICENSE)
  - Copyright (c) 2013-2025 Frank Denis (github@pureftpd.org)
- **zlib**: General-purpose compression library
  - License: zlib/libpng license (see [***LICENSE***](https://github.com/madler/zlib/blob/develop/LICENSE) file)
  - Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
    

##

