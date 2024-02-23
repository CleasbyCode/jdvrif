# jdvrif

Use this command-line tool to embed or extract any file type via a **JPG** image.  
You can share your image on several *social media sites, which will preserve the inserted data. 

\***Image size limits vary across platforms:**
* *Flickr (200MB), \*ImgPile (100MB), ImgBB (32MB), PostImage (24MB), \*Reddit (20MB / -r option)*,
* *Mastodon (16MB), \*Twitter (~10KB / Limit measured by data file size).*
  
**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/jrif_32028.jpg)  
***{Image credit: [@_AtelierSG_](https://twitter.com/_AtelierSG_)}***

Demo Videos: [***Mastodon***](https://youtu.be/OKFTfWf-8oc) / [***Reddit***](https://youtu.be/lWVT8Oi5-cg) / [***Twitter***](https://youtu.be/h_PHmYe4M1E)

The method jdvrif uses to store your data is not typical steganography, such as LSB.  
Your data file is encrypted and embedded within multiple 65KB ICC_Profile blocks of the JPG image.  

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

To **post/share** file-embedded JPG images on **Reddit**, you must use the **-r** option with *jdvrif*.  
Always select the "***Images & Video***" tab to post these images on Reddit

When **saving** images from **Reddit**, click the image in the post to expand it, then save it.  
You should see the filename with a *.jpeg* extension in the address bar of your browser.  

With **Twitter**, the size limit is measured by the **data file size** and not the image size. As it is only 10KB,  
it is recommended to compress (*ZIP/RAR*) your data file to maximise the amount of embeddable data.  

To correctly download an image from **Twitter**, click the image in the post to fully expand it, before saving.

Compile and run the program under Windows or **Linux**.  

*(Test **jdvrif** from this [**Web Page**](https://cleasbycode.co.uk/jdvrif/index/) if you don't want to download & compile the source code.)*

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

Saved JPG image: jrif_28367.jpg 150476 Bytes.

Based on image/data size, you can post your JPG image on the following sites:

_Twitter
_Mastodon
_PostImage
_ImgBB
_ImgPile
_Flickr

Complete!

You can now post your file-embedded JPG image on the relevant supported platforms.

user1@linuxbox:~/Desktop$ ./jdvrif -x jrif_28367.jpg

eXtract mode selected.

Reading JPG image file. Please wait...

Found jdvrif embedded data file.

Extracting encrypted data file from the JPG image.

Decrypting data file.

Writing data file out to disk.

Saved file: workflow.rar 4225 Bytes.

Complete! Please check your extracted file.

user1@linuxbox:~/Desktop$ 

```

**Issues:**
* **Reddit -** *Images not compatible with Reddit's mobile app. Desktop/browser only (new.reddit.com).*
* **ImgPile -** *You must sign in to an account before sharing your data-embedded JPG image on ImgPile*.  
*Sharing your image without logging in, your embedded data will not be preserved.*

My other programs you may find useful:-  

* [pdvzip: CLI tool to embed a ZIP file within a tweetable and "executable" PNG-ZIP polyglot image.](https://github.com/CleasbyCode/pdvzip)
* [imgprmt: CLI tool to embed an image prompt (e.g. "Midjourney") within a tweetable JPG-HTML polyglot image.](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt: CLI tool to encrypt, compress & embed any file type within a PNG image.](https://github.com/CleasbyCode/pdvrdt)
* [jzp: CLI tool to embed small files (e.g. "workflow.json") within a tweetable JPG-ZIP polyglot image.](https://github.com/CleasbyCode/jzp) 
* [pdvps: PowerShell / C++ CLI tool to encrypt & embed any file type within a tweetable and "executable" PNG image](https://github.com/CleasbyCode/pdvps)   

##

