# jdvrif

Use this command-line tool to embed or extract any file type via a **JPG** image.  
You can share your image on several *social media sites, which will retain the embedded data. 

**Image size limits vary across platforms:*
* ***Flickr (200MB), \*ImgPile (100MB), ImgBB (32MB), PostImage (24MB)***,
* ***\*Reddit (20MB), \*Imgur (20MB), Mastodon (16MB), Twitter (10KB).***
  
**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/water.jpg)  
***{Image credit: [PixelArte.IA / @IAEsp4n4](https://twitter.com/IAEsp4n4/status/1733170639853252795)}***

Demo Videos: [***Mastodon***](https://youtu.be/9jBhayXBEq0) / [***Reddit***](https://youtu.be/1q9pitqJXcY) / [***Twitter***](https://youtu.be/FvkLwYu8xFg)

The method jdvrif uses to store your data is not typical steganography, such as LSB.  
Your data file is encrypted & inserted within multiple 65KB ICC_Profile blocks in the JPG image.  

There are several benefits of using this method, such as, it's easier to implement,  
no image distortion, we can embed more data & images are sharable over various  
social media and image hosting sites, with the embedded-data being retained.

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

When ***saving*** file-embedded images from Twitter, always ***click the image first to fully expand it***, before saving.

When ***posting*** file-embedded images on Reddit, always the select the "***Images & Video***" box.

When **saving** file-embedded images from **Reddit**, click the image in the post to expand it, then save it.  
You should see the filename with a *.jpg* extension in the address bar of your browser.  

Compile and run the program under Windows or **Linux**.

## Usage Demo

```console

user1@linuxbox:~/Desktop$ g++ jdvrif.cpp -O2 -s -o jdvrif
user1@linuxbox:~/Desktop$ ./jdvrif 

Usage: jdvrif -i <cover_image> <data_file>  
       jdvrif -x <embedded_image>  
       jdvrif --info

user1@linuxbox:~/Desktop$ ./jdvrif -i rabbit.jpg document.pdf
  
Insert mode selected.

Reading files. Please wait...

Encrypting data file.

Embedding data file within the ICC Profile of the JPG image.

Writing data-embedded JPG image out to disk.

Created data-embedded JPG image: "jdv_img1.jpg" Size: "1218285 Bytes".

Complete!

You can now post your data-embedded JPG image(s) on the relevant supported platforms.

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
Using **jdvrif**, you can insert up to eight files at a time (outputs one image per file).  
*(jdvrif -i image.jpg file1.mp3 file2.doc file3.zip, etc.)*  

You can also extract files from up to eight images at a time.  
*(jdvrif -x jdv_img1.jpg jdv_img2.jpg jdv_img3.jpg, etc.)*  

**Issues:**
* **Reddit -** *Does not work with Reddit's mobile app. Desktop/browser only.*
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

