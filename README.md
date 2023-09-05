# jdvrif

A simple command-line tool to embed and extract any file type via a JPG image file.  
Share your "*file-embedded*" image on the following compatible sites.  

Image size limit is platform dependant:-  
* ***Flickr (200MB), ImgPile (100MB), ImgBB (32MB), ImageShack (25MB)***,
* ***PostImage (24MB), \*Reddit (Desktop only) & \*Imgur (20MB), Mastodon (16MB)***.

**Twitter:** ***If your data file under 10KB, you can also use Twitter to share your "*file-embedded*" JPG image.  
To share larger files on Twitter, *(up to 5MB)*, please use [pdvzip](https://github.com/CleasbyCode/pdvzip).***  

**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/song.jpg)  
Image Credit: [Sophia Kramer Art](https://twitter.com/sophiakramerart/status/1688014531807584256)  
{***Image contains encrypted FLAC music file (18MB) / extract:  jdvrif  -x  rain_img.jpg***}   

**Issues:**
* **Reddit -** ***Does not work with Reddit's mobile app. Desktop only.*** 
* **Imgur -** ***Keeps data, but reduces the dimension size of images over 5MB.***

**Video Demos**  

[***Mastodon - JPG Image Embedded With Encrypted FLAC Music File (15MB)***](https://youtu.be/S7O6-93vS_o)  
[***Reddit - JPG Image Embedded With Encrypted FLAC Music File (19MB)***](https://youtu.be/s_ejm3bd2Qg)

This program can be used on Linux and Windows.

Your data file is encrypted & inserted within multiple 65KB ICC Profile blocks in the JPG image file.

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

Using **jdvrif**, you can insert up to eight files at a time (outputs one image per file).  

You can also extract files from up to eight images at a time.

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

* [pdvzip - PNG Data Vehicle (ZIP Edition) for Compatible Social Media & Image Hosting Sites](https://github.com/CleasbyCode/pdvzip)
* [imgprmt - Embed image prompts as a basic HTML page within a JPG image file](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt - PNG Data Vehicle for Reddit](https://github.com/CleasbyCode/pdvrdt)  
* [pdvps - PNG Data Vehicle for Twitter, PowerShell Edition](https://github.com/CleasbyCode/pdvps)   

##

