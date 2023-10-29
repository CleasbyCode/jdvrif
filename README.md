# jdvrif

A simple command-line tool to embed and extract any file type via a JPG image.  

Share your *file-embedded* image on the following compatible sites.  

* ***Flickr (200MB), \*ImgPile (100MB), ImgBB (32MB), PostImage (24MB)***,
* ***\*Reddit (20MB / Browser only), \*Imgur (20MB), Mastodon (16MB)***.

**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/song.jpg)  
Image Credit: [Sophia Kramer Art](https://twitter.com/sophiakramerart/status/1688014531807584256)  
{***Image contains encrypted FLAC music file (18MB) / extract:  jdvrif  -x  rain_img.jpg***}   

[***Video Demo 1: Mastodon - JPG Image Embedded with Encrypted FLAC Music File (15MB)***](https://youtu.be/S7O6-93vS_o)  
[***Video Demo 2: Reddit - JPG Image Embedded with Encrypted FLAC Music File (19MB)***](https://youtu.be/s_ejm3bd2Qg)  

**\*ImgPile** - *You must sign in to an account before sharing your data-embedded JPG image on **ImgPile**.  
Sharing your image without logging in, your embedded data will not be preserved.*

**Twitter:** ***If your data file is under 10KB, you can also use Twitter to share your *file-embedded* JPG image.  
To share larger files on Twitter, *(up to 5MB)*, please use [pdvzip](https://github.com/CleasbyCode/pdvzip).***  

This program can be used on Linux and Windows.

Your data file is encrypted & inserted within multiple 65KB ICC Profile blocks in the JPG image file.

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

Using **jdvrif**, you can insert up to eight files at a time (outputs one image per file).  

You can also extract files from up to eight images at a time.

Compile and run the program under Windows or **Linux**.

## Usage Demo

```console

user1@linuxbox:~/Desktop$ g++ jdvrif.cpp -O2 -s -o jdvrif
user1@linuxbox:~/Desktop$
user1@linuxbox:~/Desktop$ ./jdvrif 

Usage:  jdvrif -i <jpg-image> <file(s)>  
	jdvrif -x <jpg-image(s)>  
	jdvrif --info

user1@linuxbox:~/Desktop$ ./jdvrif -i rabbit.jpg document.pdf
  
Insert mode selected.

Reading files. Please wait...

Encrypting data file.

Embedding data file within the ICC Profile of the JPG image.

Writing data-embedded JPG image out to disk.

Created data-embedded JPG image: "jdv_img1.jpg" Size: "1218285 Bytes".

Complete!

You can now post your data-embedded JPG image(s) to the relevant supported platforms.
 
user1@linuxbox:~/Desktop$ ./jdvrif

Usage:  jdvrif -i <jpg-image> <file(s)>  
	jdvrif -x <jpg-image(s)>  
	jdvrif --info
        
user1@linuxbox:~/Desktop$ ./jdvrif -x jdv_img1.jpg

Extract mode selected.

Reading embedded JPG image file. Please wait...

Found jdvrif embedded data file.

Extracting encrypted data file from the JPG image.

Decrypting extracted data file.

Writing decrypted data file out to disk.

Saved file: "document.pdf" Size: "1016540 Bytes"

Complete! Please check your extracted file(s).

user1@linuxbox:~/Desktop$ 

```
**Issues:**
* **Reddit -** ***Does not work with Reddit's mobile app. Desktop/browser only.*** 
* **Imgur -** ***Keeps embedded data, but reduces the dimension size of images over 5MB.***

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [xif: CLI tool to embed small files (e.g. "workflow.json") within a tweetable JPG-ZIP polyglot image.](https://github.com/CleasbyCode/xif) 
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

