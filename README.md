# jdvrdt

JPG Data Vehicle for Reddit. 

jdvrdt partly derives from the ***[technique](https://www.vice.com/en/article/bj4wxm/tiny-picture-twitter-complete-works-of-shakespeare-steganography)*** discovered by security researcher ***[David Buchanan](https://www.da.vidbuchanan.co.uk/).*** 

This program enables you to embed & extract arbitrary data of up to ~20MB within a single JPG image.  
Post & share your "*file-embedded*" image on ***[reddit](https://www.reddit.com/)*** or Imgur. 

![Demo Image](https://github.com/CleasbyCode/jdvrdt/blob/main/demo_image/demo.jpg)  
{***Image demo: MP3 embedded within this JPG***} 

**Imgur issue:** When the embedded image size is over 5MB, the data is still retained, but Imgur will reduce the dimension size of your image.

***jdvrdt file-embedded images do not work with Twitter.  For Twitter, please use [pdvzip](https://github.com/CleasbyCode/pdvzip) (PNG only).***

This program can be used on Linux and Windows.

The file data is inserted and preserved within multiple 65KB ICC Profile chunks in the JPG image.

To maximise the amount of data you can embed in your image file, I recommend first compressing your 
data file(s) to zip/rar formats, etc.  

Using jdvrdt, you can insert up to five files at a time (outputs one image per file).  
You can also extract files from up to five images at a time.

Compile and run the program under Windows or **Linux**.

## Usage (Linux - Insert file into JPG image / Extract file from image)

```c

$ g++ jdvrdt.cpp -lz -o pdvin
$
$ ./jdvrdt 

Usage:  jdvrdt -i <jpg-image>  <file(s)>  
	      jdvrdt -x <jpg-image(s)>  
	      jdvrdt --info

$ ./jdvrdt -i image.jpg  document.pdf
  
Created output file: "jdvimg1.jpg"  

Complete!  

You can now post your file-embedded JPG image(s) on reddit.  

$ ./jdvrdt

Usage:  jdvrdt -i <jpg-image>  <file(s)>  
	      jdvrdt -x <jpg-image(s)>  
	      jdvrdt --info
        
$ ./jdvrdt -x jdvimg1.jpg

Searching for embedded data file. Please wait...

Extracted file: "jdv_document.pdf 1242153 Bytes"

Complete!  

$ ./jdvrdt -i toy.jpg  Clowns.part1.rar Clowns.part2.rar Clowns.part3.rar 

Created output file: "jdvimg1.jpg"

Created output file: "jdvimg2.jpg"

Created output file: "jdvimg3.jpg"

Complete!

You can now post your file-embedded JPG image(s) on reddit.  

$ ./jdvrdt -x jdvimg1.jpg jdvimg2.jpg jdvimg3.jpg  

Searching for embedded data file. Please wait...

Extracted file: "jdv_Clowns.part1.rar 10485760 Bytes"

Searching for embedded data file. Please wait...

Extracted file: "jdv_Clowns.part2.rar 10485760 Bytes"

Searching for embedded data file. Please wait...

Extracted file: "jdv_Clowns.part3.rar 10485760 Bytes"

Complete!

```

My other program you may find useful:-  

* [pdvzip - PNG Data Vehicle for Twitter, ZIP Edition](https://github.com/CleasbyCode/pdvzip)  
* [pdvps - PNG Data Vehicle for Twitter, PowerShell Edition](https://github.com/CleasbyCode/pdvps)   

##

