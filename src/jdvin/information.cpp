#include "information.h"
#include <iostream>

void displayInfo() {
	std::cout << R"(

JPG Data Vehicle (jdvin v4.4). 
Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

jdvin is a steganography-like CLI tool for embedding & concealing any file type within a JPG image. 

Compile & run jdvin (Linux):
		
$ sudo apt-get install libsodium-dev
$ sudo apt-get install libturbojpeg0-dev

$ chmod +x compile_jdvin.sh
$ ./compile_jdvin.sh

$ Compilation successful. Executable 'jdvin' created.

$ sudo cp jdvin /usr/bin
$ jdvin
		
Usage: jdvin [-b|-r] <cover_image> <secret_file> 
       jdvin --info
		
Share your "file-embedded" JPG image on the following compatible sites. Image size limits (cover image + data file):

Flickr (200MB), ImgPile (100MB), ImgBB (32MB), PostImage (32MB), Reddit (20MB / -r option), 
Bluesky (*see below / -b option), *Mastodon (~6MB), *Tumblr(~64KB), *X-Twitter (~10KB). *Limit measured by data file size only.

Argument options:	

To create "file-embedded" JPG images compatible for the Bluesky platform, use the -b option with jdvin.
$ jdvin -b cover_image.jpg data_file.txt

You are required to use the Python script "bsky_post.py" (found in the repo src folder), to post the image to Bluesky.
It will not work if you post images to Bluesky via the Bluesky browser site or mobile app.

Script example:

$ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka --image jrif_11050.jpg --alt-text "text to describe image, here..." "standard text to appear in main post, here..."

You will need to create an app password from your Bluesky account. (https://bsky.app/settings/app-passwords)

*Bluesky cover image size limit: 800KB.
Secret file size limit (Compressed): ~106KB. 

With X-Twitter, Bluesky, & Tumblr, the small size limits are measured by the compressed data file size and not the combined image + data file size. 
As the embedded data file is compressed with jdvin using zlib/deflate (if not already a compressed file type), you should be able to get
significantly more than the default size limit, especially for text documents and other file types that compress well. 
You may wish to compress the data file yourself (zip, rar, 7z, etc) before embedding it with jdvin, so as to know exactly what
the compressed data file size will be.

Also with Mastodon, the size limit is measured by the compressed data file size and not the combined image + data file size.
For example, if your cover image is 1MB you can still embed a data file up to the ~6MB Mastodon size limit.
		
To create "file-embedded" JPG images compatible for posting on Reddit, use the -r option with jdvin.	
$ jdvin -r cover_image.jpg data_file.doc
		
From the Reddit site, click "Create Post" then select "Images & Video" tab, to post your JPG image.
		
To correctly download images from X-Twitter or Reddit, click the image in the post to fully expand it, before saving.
		
ImgPile - You must sign in to an account before sharing your data-embedded JPG image on this site.
Sharing your image without logging in, your embedded data will not be preserved.

)";
}
