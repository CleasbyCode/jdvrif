# jdvrif

Command-line tools to hide (*jdvin*) or extract (*jdvout*) any file type via a JPG image.  
You can post your image with hidden data on ***Mastodon*** and a few other social media sites.

**With ***jdvin***, you can embed a single file of any type up to a maximum size of ~1GB.**  
**Compatible hosting sites, listed below, have their own, much smaller, size limits (cover image + data file):**
* *Flickr (200MB), ImgPile (100MB), ImgBB (32MB), PostImage (24MB), Reddit (20MB / -r option)*,
* Limit measured by data file size: *Mastodon (~6MB), Tumblr (~64KB), Twitter (~10KB).*
  
**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** demonstrated by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_12271.jpg)  
*Image credit:* [***@DontSmileAI***](https://x.com/DontSmileAI)

Demo Videos: [***Mastodon***](https://youtu.be/rnLf3W60IKQ) / [***X/Twitter***](https://youtu.be/Ajn5F1BO0Zg) / [***Reddit***](https://youtu.be/xIUsa3F8ZQc) / [***Tumblr***](https://youtu.be/8lIyLbx7CO8) / [***Flickr***](https://youtu.be/kg_MJHQuzLY) / [***Web Tool***](https://youtu.be/WvZMRp7Z6W4)  

Your data file that is embedded within the JPG cover image is *compressed and encrypted. (*Compression is disabled for files over 200MB).

To **share** *file-embedded* JPG images on **Reddit**, you must use the **-r** option with jdvin.  
Select the "***Images & Video***" tab on Reddit to post your image.

To correctly download an image from ***X/Twitter*** or ***Reddit***, click the image in the post to **fully expand it**, before saving.

With **X/Twitter**, the small size limit (~10KB) is measured by the **data file size** and not the combined image size.   
As the data file is compressed when embedded, you should be able to hide files larger than 10KB.   
For example, a 30KB workflow.json file compressed down to under 10KB.

Also with Mastodon, the size limit (~6MB) is measured by the **data file size** and not the combined image size.  
For example, if your cover image is 2MB you can still embeded a data file upto ~6MB.

You can try **jdvrif** from [**this site**](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download and compile the source code.

## Usage (Linux - jdvin / jdvout)

```console

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ g++ main.cpp -O2 -lz -s -o jdvin
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvin$ sudo cp jdvin /usr/bin

user1@linuxbox:~/Desktop$ jdvin 

Usage: jdvin [-r] <cover_image> <data_file>  
       jdvin --info

user1@linuxbox:~/Desktop$ jdvin clown.jpg workflow.rar
  
Saved file-embedded JPG image: jrif_28597.jpg 176345 Bytes.

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

**Issues:**
* **ImgPile -** *You must sign in to an account before sharing your data-embedded JPG image on ImgPile*.  
*Sharing your image without logging in, your embedded data will not be preserved.*

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [jzp: CLI tool to embed small files (e.g. "workflow.json") within a tweetable JPG-ZIP polyglot image.](https://github.com/CleasbyCode/jzp) 
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

