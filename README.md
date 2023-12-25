# jdvrif

Use this command-line tool to embed or extract any file type via a **JPG** image.  
You can share your image on several *social media sites, which will retain the embedded data. 

**Image size limits vary across platforms:**
* *Flickr (200MB), \*ImgPile (100MB), ImgBB (32MB), PostImage (24MB), \*Reddit (20MB / Requires -r option)*,
* \**Imgur (20MB), Mastodon (16MB), \*Twitter (~10KB / Limit measured by data file size).*
  
**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/robo_clown.jpg)  
***{Image credit: [MΞV.ai / @aest_artificial](https://twitter.com/aest_artificial)}***

Demo Videos: [***Mastodon***](https://youtu.be/9jBhayXBEq0) / [***Reddit***](https://youtu.be/1q9pitqJXcY) / [***Twitter***](https://youtu.be/FvkLwYu8xFg)

The method jdvrif uses to store your data is not typical steganography, such as LSB.  
Your data file is encrypted & inserted within multiple 65KB ICC_Profile blocks in the JPG image.  

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

When ***posting*** file-embedded images on Reddit, always the select the "***Images & Video***" box.

When **saving** file-embedded images from **Reddit**, click the image in the post to expand it, then save it.  
You should see the filename with a *.jpeg* extension in the address bar of your browser.  

When ***saving*** file-embedded images from Twitter, always ***click the image first to fully expand it***, before saving.

Compile and run the program under Windows or **Linux**.

## Usage Demo

```console

user1@linuxbox:~/Desktop$ g++ jdvrif.cpp -O2 -s -o jdvrif
user1@linuxbox:~/Desktop$ ./jdvrif 

Usage: jdvrif -e [-r] <cover_image> <data_file>  
       jdvrif -x <file_embedded_image>  
       jdvrif --info

user1@linuxbox:~/Desktop$ ./jdvrif -e .\clown.jpg .\workflow.rar
  
Embed mode selected.

Reading files. Please wait...

Encrypting data file.

Embedding data file within the JPG image.

Writing file-embedded JPG image out to disk.

Created JPG image: jrif_28367.jpg 150476 Bytes.

Based on image/data size, you can post your JPG image on the following sites:

_Twitter
_Mastodon
_Imgur
_PostImage
_ImgBB
_ImgPile
_Flickr

Complete!

You can now post your file-embedded JPG image(s) on the relevant supported platforms.

user1@linuxbox:~/Desktop$ ./jdvrif -x jrif_28367.jpg

eXtract mode selected.

Reading JPG image file. Please wait...

Found jdvrif embedded data file.

Extracting encrypted data file from the JPG image.

Decrypting data file.

Writing data file out to disk.

Saved file: workflow.rar 4225 Bytes.

Complete! Please check your extracted file(s).

user1@linuxbox:~/Desktop$ 

``` 
**Issues:**
* **Reddit -** *Images not compatible with Reddit's mobile app. Desktop/browser only.*
* **Imgur -** *Retains embedded data, but reduces the dimension size of images over 5MB.*
* **ImgPile -** *You must sign in to an account before sharing your data-embedded JPG image on ImgPile*.  
*Sharing your image without logging in, your embedded data will not be preserved.*

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [jzp: CLI tool to embed small files (e.g. "workflow.json") within a tweetable JPG-ZIP polyglot image.](https://github.com/CleasbyCode/jzp) 
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

