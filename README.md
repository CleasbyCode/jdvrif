# jdvrif

CLI tools to hide (*jdvin*) or extract (*jdvout*) any file type via a JPG image.  
You can post your image with hidden data on ***Mastodon*** and a few other social media sites.

\***Image size limits (cover image + data file):**
* *Flickr (200MB), ImgPile (100MB), ImgBB (32MB), PostImage (24MB), Reddit (20MB / -r option)*,
* *Mastodon (~6MB / Limit measured by data file size), Twitter (~10KB / Limit measured by data file size).*
  
**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_11865.jpg)  
*Image credit:* [***@altphotos_pl***](https://x.com/altphotos_pl)

Demo Videos: [***Mastodon (hidden jpg image)***](https://youtu.be/XUUbSXQuD1g) / [***X/Twitter (hidden workflow.json file)***](https://youtu.be/Ajn5F1BO0Zg) / [***Web Tool***](https://youtu.be/WvZMRp7Z6W4)  

To **post/share** file-embedded JPG images on **Reddit**, you must use the **-r** option.  
Always select the "***Images & Video***" tab on Reddit to post these images.

When **saving** images from **Reddit**, use the **new.reddit.com site**, click the image in the post to expand it, then save it.  
You should see the filename with a *.jpeg* extension in the address bar of your browser.  

To correctly download an image from **Twitter**, click the image in the post to fully expand it, before saving.

With **Twitter**, the size limit (~10KB) is measured by the **data file size** and not the combined image size.   
As the data file is compressed when embedded, you should be able to hide files larger than 10KB.   
For example, a 30KB workflow.json file compressed down to under 10KB.

Also with Mastodon, the size limit (~6MB) is measured by the **data file size** and not the combined image size.  
For example, if your cover image is 2MB you can still embeded a data file upto ~6MB.

*(You can try **jdvrif** from this [**site**](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download & compile the source code.)*

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

You can now post your file-embedded JPG image on the relevant supported platforms.

user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ g++ main.cpp -O2 -lz -s -o jdvout
user1@linuxbox:~/Downloads/jdvrif-main/src/jdvout$ sudo cp jdvout /usr/bin

user1@linuxbox:~/Desktop$ jdvout

Usage: jdvout <file_embedded_image>
       jdvout --info
        
user1@linuxbox:~/Desktop$ jdvout jrif_28597.jpg

Extracted hidden file: workflow.rar 4225 Bytes.

Complete! Please check your file.

```
![Demo Image2](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/new_screen.png) 
![Demo Image3](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/screen2.png)  

**Issues:**
* **Reddit -** *While you can upload JPG images via the mobile app, you need to use the Desktop/browser to download them (new.reddit.com).*
* **ImgPile -** *You must sign in to an account before sharing your data-embedded JPG image on ImgPile*.  
*Sharing your image without logging in, your embedded data will not be preserved.*

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [jzp: CLI tool to embed small files (e.g. "workflow.json") within a tweetable JPG-ZIP polyglot image.](https://github.com/CleasbyCode/jzp) 
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

