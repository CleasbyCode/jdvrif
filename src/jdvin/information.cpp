void displayInfo() {
	std::cout << R"(

JPG Data Vehicle (jdvin v1.5). 
Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

jdvin is a steganography-like CLI tool for embedding & concealing any file type within a JPG image. 

Compile & run jdvin (Linux):
		
$ g++ main.cpp -O2 -lz -s -o jdvin
$ sudo cp jdvin /usr/bin
$ jdvin
		
Usage: jdvin [-r] <cover_image> <data_file>
       jdvin --info
		
Share your "file-embedded" JPG image on the following compatible sites. Image size limits (cover image + data file):

Flickr (200MB), ImgPile (100MB), ImgBB (32MB), PostImage (32MB), Reddit (20MB / requires -r option), 
*Mastodon (~6MB), *Tumblr(~64KB), *X/Twitter (~10KB). *Limit measured by data file size only.

Arguments / options:	
		
If you want to share a "file-embedded" JPG image on Reddit, you will need to use the -r option with jdvin.	
-r = Reddit option. $ jdvin -r cover_image.jpg data_file.doc
		
From the Reddit site, click "Create Post" then select "Images & Video" tab, to post your JPG image.
		
ImgPile - You must sign in to an account before sharing your data-embedded JPG image on this platform.
Sharing your image without logging in, your embedded data will not be preserved.

)";
}
