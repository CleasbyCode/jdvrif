# jdvrif

Use CLI tools ***jdvin*** & ***jdvout*** with a JPG image, to hide/extract any file type, up to ***2GB**.  

*Compatible hosting sites, ***listed below***, have their own ***much smaller*** embedded data size limits:  
* ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** / ***-r option***),
* ***Mastodon*** (**~6MB**), ***Bluesky*** (**~64KB** / ***-b option***), ***Tumblr*** (**~64KB**), ***Twitter*** (**~10KB**).
  
***jdvrif*** partly derives from the ***[technique demonstrated](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_70001.jpg)  
***Image credit:*** [***@unpaidactor1***](https://x.com/unpaidactor1) / ***PIN: 7505403832509957594***

For extra security, your data file is also compressed (*zlib*) and encrypted using the ***libsodium*** crypto library. 

*You can try the [***jdvrif Web App, here,***](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download and compile the CLI source code.*

## Usage (Linux - jdvin / jdvout)

```console

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo apt-get install libsodium-dev
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ g++ main.cpp -O2 -lz -lsodium -s -o jdvin
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo cp jdvin /usr/bin

user1@linuxbox:~/Desktop$ jdvin 

Usage: jdvin [-r] <cover_image> <secret_file>  
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

To correctly download an image from ***Flickr***, click the download arrow near the bottom right-hand corner of the page and select ***Original*** for the size of image to download.

With ***X/Twitter*** & ***Tumblr***, the small size limits (**~10KB** / **~64KB**) are measured by the ***data file size*** and not the combined image size.
As the data file is compressed when embedded, you should be able to hide files larger than **10KB** or **64KB**.
For example, a **50KB** workflow.json file compressed down to **6KB**, making it compatible with sharing on ***X/Twitter.*** 

Also with ***Mastodon***, the size limit is measured by the ***data file size*** and not the combined image size.  
For example, if your cover image is **1MB** you can still embed a data file up to the **~6MB** size limit.

https://github.com/user-attachments/assets/cbae0361-bbb7-433c-a9a6-851c74940cd9

https://github.com/user-attachments/assets/7e243233-ae8d-4220-9dfd-eff6d28b8f2b

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

