# jdvrif

CLI tools ***jdvin*** & ***jdvout***, used to embed or extract any file type, up to **~2GB** (cover image + data file).  

****Compatible hosting sites, listed below, have their own much smaller image size limits:***
* ***Flickr*** (**200MB**), ***ImgPile*** (**100MB**), ***ImgBB*** (**32MB**), ***PostImage*** (**32MB**), ***Reddit*** (**20MB** / ***-r option***),
* Limit measured by data file size: ***Mastodon*** (**~6MB**), ***Tumblr*** (**~64KB**), ***Twitter*** (**~10KB**).
  
*There are many other image hosting sites on the web that may also be compatible.*  

***jdvrif*** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** demonstrated by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_12272.jpg)  
***Image credit:*** [***@DontSmileAI***](https://x.com/DontSmileAI)

Demo Videos: [***Mastodon***](https://youtu.be/rnLf3W60IKQ) / [***X/Twitter***](https://youtu.be/Ajn5F1BO0Zg) / [***Reddit***](https://youtu.be/xIUsa3F8ZQc) / [***Tumblr***](https://youtu.be/8lIyLbx7CO8) / [***Flickr***](https://youtu.be/kg_MJHQuzLY) / [***Web Tool***](https://youtu.be/WvZMRp7Z6W4)  

Your data file is split and stored into multiple **~64KB** ***ICC Profile segments*** within the ***JPG*** cover image.  

The data file is also ***compressed and encrypted***.

To ***share*** *"file-embedded"* ***JPG*** images on ***Reddit***, you must use the ***-r*** option with ***jdvin***.  
Select the "***Images & Video***" tab on ***Reddit*** to post your image.

To correctly download images from ***X/Twitter*** or ***Reddit***, click the image in the post to ***fully expand it***, before saving.  

To correctly download an image from ***Flickr***, click the download arrow near the bottom right-hand corner of the site and select ***Original*** for the size of image to download.

With ***X/Twitter & Tumblr***, the small size limits (**~10KB** / **~64KB**) are measured by the ***data file size*** and not the combined image size.
As the data file is compressed when embedded, you should be able to hide files larger than **10KB** or **64KB**.
For example, a **30KB** workflow.json file compressed down to under **10KB**, making it compatible with sharing on ***X/Twitter.***

Also with ***Mastodon***, the size limit is measured by the ***data file size*** and not the combined image size.  
For example, if your cover image is **1MB** you can still embed a data file up to the **~6MB** size limit.

You can try ***jdvrif*** from [***this site***](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download and compile the source code.

## Usage (Linux - jdvin / jdvout)

```console

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ g++ main.cpp -O2 -lz -s -o jdvin
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo cp jdvin /usr/bin

user1@linuxbox:~/Desktop$ jdvin 

Usage: jdvin [-r] <cover_image> <data_file>  
       jdvin --info

user1@linuxbox:~/Desktop$ jdvin clown.jpg workflow.rar
  
Saved "file-embedded" JPG image: jrif_28597.jpg 176345 Bytes.

Complete!

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ g++ main.cpp -O2 -lz -s -o jdvout
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ sudo cp jdvout /usr/bin

user1@linuxbox:~/Desktop$ jdvout

Usage: jdvout <file_embedded_image>
       jdvout --info
        
user1@linuxbox:~/Desktop$ jdvout jrif_28597.jpg

Extracted hidden file: workflow.rar 4225 Bytes.

Complete! Please check your file.

```
![Demo Image2](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/new_screen2.png) 
![Demo Image3](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/screen2.png)  

***Issues:***
* **ImgPile -** *You must sign in to an account before sharing your data-embedded JPG image on ImgPile*.  
*Sharing your image without logging in, your embedded data will not be preserved.*

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [jzp: CLI tool to embed small files (e.g. "workflow.json") within a tweetable JPG-ZIP polyglot image.](https://github.com/CleasbyCode/jzp) 
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

