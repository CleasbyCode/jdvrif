# jdvrif

JPG Data Vehicle for Reddit, Imgur & Flickr. 

jdvrif partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

This program enables you to embed & extract any file type of up to 200MB* via a single JPG image.  
Post & share your "*file-embedded*" image on ***[reddit](https://www.reddit.com/), [imgur](https://imgur.com/) & [flickr](https://www.flickr.com)*** 

\*With Flickr, you can embed & post up to 200MB of data in a single JPG image.  
With Imgur* and Reddit you are limited to a maximum of 20MB for each JPG image.

**\*Imgur issue:** Data is still retained when the "file-embedded" JPG image is over 5MB, but Imgur reduces the dimension size of the image.

***jdvrif "file-embedded" images do not work with Twitter.  For Twitter, please use [pdvzip](https://github.com/CleasbyCode/pdvzip) (PNG only).***

![Demo Image](https://github.com/CleasbyCode/jdvrif/blob/main/demo_image/Image.jpg)  
{***Image contains encrypted MP4 movie trailer (17MB)***}   

[**Video_Demo (YouTube) - Embed 200MB Zip File Inside JPG Image & Post on Flickr**](https://www.youtube.com/watch_popup?v=YHT-V6otO2M)  

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
* [pdvrdt - PNG Data Vehicle for Reddit](https://github.com/CleasbyCode/pdvrdt)  
* [pdvps - PNG Data Vehicle for Twitter, PowerShell Edition](https://github.com/CleasbyCode/pdvps)   

##

