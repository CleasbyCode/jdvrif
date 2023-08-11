# jdvrif

Command-line tool to embed & extract any file type (up to *200MB) via a JPG image.  
Share the "*file-embedded*" image on compatible social media & image hosting sites.

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/song.jpg)  
Image Credit: [Sophia Kramer Art](https://twitter.com/sophiakramerart/status/1688014531807584256)  
{***Image contains encrypted FLAC music file (18MB) / extract:  jdvrif  -x  rain_img.jpg***}   

**jdvrif** partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

\*Size limit per JPG image is platform dependant:-  
* ***Flickr (200MB), ImgPile (100MB), ImgBB (32MB), ImageShack (25MB)***,
* ***PostImage (24MB), \*Reddit (Desktop only) & \*Imgur (20MB), Mastodon (16MB)***.
  
**\*Reddit issue:** Desktop only. Does not work with mobile app. 

**\*Imgur issue:** Data is still retained when the "*file-embedded*" *JPG* image is over *5MB*, but Imgur reduces the dimension size of the image.

**\*Twitter:** If your data file is only *9KB or smaller*, you can also use Twitter to share your "*file-embedded*" JPG image.  
To share larger files on Twitter, *(up to 5MB)*, please use [pdvzip](https://github.com/CleasbyCode/pdvzip) (*PNG* only).

**Video Demos**  

[***Flickr Upload - Encrypted MP3 Music File Embedded in JPG Image***](https://youtu.be/pc5h6AGCstI)  
[***Mastodon Upload - Encrypted ZIP File Embedded in JPG Image***](https://youtu.be/rYAMNy5uPh8)  
[***Reddit Upload - Encrypted PNG Image Embedded in JPG Image***](https://youtu.be/6-BDwFJG8Cw)

This program can be used on Linux and Windows.

Your data file is encrypted & inserted within multiple 65KB ICC Profile blocks in the JPG image file.

![ICC](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/icc.png)  

Using **jdvrif**, you can insert up to six files at a time (outputs one image per file).  

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

* [pdvzip - PNG Data Vehicle (ZIP Edition) for Compatible Social Media & Image Hosting Sites](https://github.com/CleasbyCode/pdvzip)
* [imgprmt - Embed image prompts as a basic HTML page within a JPG image file](https://github.com/CleasbyCode/imgprmt)
* [pdvrdt - PNG Data Vehicle for Reddit](https://github.com/CleasbyCode/pdvrdt)  
* [pdvps - PNG Data Vehicle for Twitter, PowerShell Edition](https://github.com/CleasbyCode/pdvps)   

##

