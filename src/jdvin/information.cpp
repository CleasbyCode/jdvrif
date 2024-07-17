void displayInfo() {

	std::cout << R"(

JPG Data Vehicle (jdvin v1.0.8). 
Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

A steganography-like CLI tool to embed & hide any file type within a JPG image.  

Compile & run jdvin (Linux):
		
$ g++ main.cpp -O2 -lz -s -o jdvin
$ ./jdvin
		
Usage: jdvin [-r] <cover_image> <data_file>
       jdvin --info
		
Post your file-embedded image on the following compatible sites.
*Image size limits(cover image + data file):

Flickr (200MB), *ImgPile (100MB), ImgBB (32MB), PostImage (24MB), *Reddit (20MB / requires -r option), 
Mastodon (16MB), *X/Twitter (~10KB / *Limit measured by data file size).

Arguments / options:	
		
To post/share file-embedded PNG images on Reddit, you need to use the -r option.	
		
-r = Reddit option, (jdvin -r cover_image.jpg data_file.doc).
		
*Reddit: Post/download images via the new.reddit.com desktop/browser site only. 
Upload images to Reddit using the "Images & Video" tab/box.
		
*ImgPile - You must sign in to an account before sharing your data-embedded JPG image on this platform.
Sharing your image without logging in, your embedded data will not be preserved.

)";
}
