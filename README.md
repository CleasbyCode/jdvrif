# jdvrif

Use CLI tools ***jdvin*** & ***jdvout*** with a JPG image, to embed or extract any file, up to **2GB** (cover image + data file).  

Compatible hosting sites, ***listed below***, have their own ***much smaller*** image size limits:
* ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** / ***-r option***),
* Limit measured by data file size: ***Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***Twitter*** (**~10KB**).
  
*There are many other image hosting sites on the web that may also be compatible.*  

***jdvrif*** partly derives from the ***[technique demonstrated](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_79866.jpg)  
***Image credit:*** [***@rdc768***](https://x.com/rdc768) / ***PIN: 3611019770***

Your embedded data file is ***compressed*** (depending on file size & type) and ***encrypted*** with ***PIN*** protection.  
The file, if required, is split into multiple [***64KB APP2 Segments (ICC Color Profile)***](https://youtu.be/1213w-k9X9M) within the ***JPG*** cover image.  

(*You can try the [***jdvrif Web App, here,***](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download and compile the CLI source code.*)

## Usage (Linux - jdvin / jdvout)

```console

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ g++ main.cpp -O2 -lz -s -o jdvin
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo cp jdvin /usr/bin

user1@linuxbox:~/Desktop$ jdvin 

Usage: jdvin [-r] <cover_image> <data_file>  
       jdvin --info

user1@linuxbox:~/Desktop$ jdvin Cover_Image.jpg Hidden_File.zip
  
Saved "file-embedded" JPG image: jrif_12462.jpg (143029 bytes).

Recovery PIN: [***2166776980***]

Important: Please remember to keep your PIN safe, so that you can extract the hidden file.

Complete!

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ g++ main.cpp -O2 -lz -s -o jdvout
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ sudo cp jdvout /usr/bin

user1@linuxbox:~/Desktop$ jdvout

Usage: jdvout <file_embedded_image>
       jdvout --info
        
user1@linuxbox:~/Desktop$ jdvout jrif_12462.jpg

PIN: **********

Extracted hidden file: Hidden_File.zip (6165 bytes).

Complete! Please check your file.

```
To correctly download images from ***X/Twitter*** or ***Reddit***, click the image in the post to ***fully expand it***, before saving.  

https://github.com/user-attachments/assets/de958ecb-3281-43b7-a290-f3a831e7bced

To create "*file-embedded*" ***JPG*** images compatible for posting on ***Reddit***, use the ***-r*** option with ***jdvin***.  
From the ***Reddit*** site, click "*Create Post*" then select "*Images & Video*" tab, to post your ***JPG*** image.

To correctly download an image from ***Flickr***, click the download arrow near the bottom right-hand corner of the page and select ***Original*** for the size of image to download.

With ***X/Twitter*** & ***Tumblr***, the small size limits (**~10KB** / **~64KB**) are measured by the ***data file size*** and not the combined image size.
As the data file is compressed when embedded, you should be able to hide files larger than **10KB** or **64KB**.
For example, a **50KB** workflow.json file compressed down to **6KB**, making it compatible with sharing on ***X/Twitter.***

Also with ***Mastodon***, the size limit is measured by the ***data file size*** and not the combined image size.  
For example, if your cover image is **1MB** you can still embed a data file up to the **~6MB** size limit.

https://github.com/user-attachments/assets/f51b2c89-23cb-423b-8d35-aa2300003f2e

* **ImgPile -** *You must sign in to an account before sharing your data-embedded JPG image on ImgPile*.  
*Sharing your image without logging in, your embedded data will not be preserved.*

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

