# jdvrif

JPG Data Vehicle for Reddit, Imgur, Flickr & Other Compatible Social Media / Image Hosting Sites.

jdvrif partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

This program enables you to embed & extract any file type of up to \*200MB via a single JPG image.  

Post & share your "*file-embedded*" image on compatible social media & image hosting sites.

\*Size limit per JPG image is platform dependant:-  
* Flickr (200MB), ImgPile (100MB), ImgBB (32MB), ImageShack (25MB),
* PostImage (24MB), Reddit & \*Imgur (20MB), Mastodon (8MB).

**\*Imgur issue:** Data is still retained when the "*file-embedded*" JPG image is over 5MB, but Imgur reduces the dimension size of the image.

***jdvrif "file-embedded" images do not work with Twitter.  For Twitter, please use [pdvzip](https://github.com/CleasbyCode/pdvzip) (PNG only).***

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/img.jpg)  
Image Credit: [@Knightama_](https://twitter.com/Knightama_/status/1672320024680476672)  
{***Image contains encrypted FLAC music file (18MB)***}   

**Video Demos**  

[***Flickr Upload - MP3 Music File Embedded in JPG Image***](https://youtu.be/pc5h6AGCstI)  
[***Mastodon Upload -  ZIP File Embedded in JPG Image***](https://youtu.be/rYAMNy5uPh8)

This program can be used on Linux and Windows.

Your data file is inserted and preserved within multiple 65KB ICC Profile blocks in the JPG image file.

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

To maximise the amount of data you can embed in your image file, I recommend first compressing your 
data file(s) to zip/rar formats, etc.  

Using jdvrif, you can insert up to six files at a time (outputs one image per file).  

You can also extract files from up to six images at a time.

Compile and run the program under Windows or **Linux**.

## Usage (Linux - Insert & Extract file from JPG image)

```c

$ g++ jdvrif.cpp -s -o jdvrif
$
$ ./jdvrif 

Usage:  jdvrif -i <jpg-image>  <file(s)>  
	jdvrif -x <jpg-image(s)>  
	jdvrif --info

$ ./jdvrif -i image.jpg  document.pdf
  
Created output file: "jdv_img1.jpg 1243153 Bytes"  

Complete!  

You can now post your "file-embedded" JPG image(s) to the relevant supported platforms.
 
$ ./jdvrif

Usage:  jdvrif -i <jpg-image>  <file(s)>  
	jdvrif -x <jpg-image(s)>  
	jdvrif --info
        
$ ./jdvrif -x jdv_img1.jpg

Extracted file: "document.pdf 1242153 Bytes"

Complete! Please check your extracted file(s).

$ ./jdvrif -i toy.jpg  Clowns.part1.rar  Clowns.part2.rar  Clowns.part3.rar 

Created output file: "jdv_img1.jpg 10489760 Bytes"

Created output file: "jdv_img2.jpg 10489760 Bytes"

Created output file: "jdv_img3.jpg 10489760 Bytes"

Complete!

You can now post your "file-embedded" JPG image(s) to the relevant supported platforms.

$ ./jdvrif -x jdv_img1.jpg  jdv_img2.jpg  jdv_img3.jpg  

Extracted file: "Clowns.part1.rar 10485760 Bytes"

Extracted file: "Clowns.part2.rar 10485760 Bytes"

Extracted file: "Clowns.part3.rar 10485760 Bytes"

Complete! Please check your extracted file(s).

```

My other programs you may find useful:-  

* [pdvzip - PNG Data Vehicle for Twitter, ZIP Edition](https://github.com/CleasbyCode/pdvzip)
* [imgprmt - Embed image prompts as a basic HTML page within a JPG image file](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt - PNG Data Vehicle for Reddit](https://github.com/CleasbyCode/pdvrdt)  
* [pdvps - PNG Data Vehicle for Twitter, PowerShell Edition](https://github.com/CleasbyCode/pdvps)   

##

