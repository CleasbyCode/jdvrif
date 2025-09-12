
#include "information.h"
#include <iostream>

void displayInfo() {
	std::cout << R"(
 JPG Data Vehicle (jdvrif v5.2)
 Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

 jdvrif is a "steganography-like" command-line tool used for concealing and extracting any file type within and from a JPG image. 

 Compile & run jdvrif (Linux):
		
  $ sudo apt-get install libsodium-dev
  $ sudo apt-get install libturbojpeg0-dev

  $ chmod +x compile_jdvrif.sh
  $ ./compile_jdvrif.sh

  $ Compilation successful. Executable 'jdvrif' created.

  $ sudo cp jdvrif /usr/bin
  $ jdvrif
		
  Usage: jdvrif conceal [-b|-r] <cover_image> <secret_file>
  	 jdvrif recover <cover_image> 
         jdvrif --info
		
 Share your "file-embedded" JPG image on the following compatible sites.
 
 Size limit for these platforms measured by the combined size of cover image & compressed data file.
 
  Flickr (200MB), ImgPile (100MB), ImgBB (32MB), PostImage (32MB), Reddit (20MB / -r option).
 
 Size limit for these platforms measured only by the compressed data file size:
 
  Mastodon (~6MB), Tumblr(~64KB), X-Twitter (~10KB). 
  
 For example, with Mastodon, if your cover image is 1MB you can still embed a data file up to the ~6MB Mastodon size limit.
 
 *Other: Size limit for the Bluesky platform has seperate size limits for cover image and the compressed data file:
 
  Bluesky (-b option) cover image size limit: 800KB. / Secret data file size limit (Compressed): ~171KB. 
 
 Even though jdvrif will compress your data file, you may wish to compress the file yourself (zip, rar, 7z, etc) 
 before embedding it with jdvrif, so as to know exactly what the compressed data file size will be. 
 
 For platforms such as X-Twitter & Tumblr, which have small size limits, you may want to focus on data files that 
 compress well, such as .txt documents, etc.

 jdvrif mode arguments:
 
  conceal - Compresses, encrypts and embeds your secret data file within a JPG cover image.
  recover - Decrypts, uncompresses and extracts the concealed data file from a JPG cover image (recovery PIN required!).
 
 jdvrif conceal mode platform options:
 
  -b (Blusesky). To share/post compatible "file-embedded" JPG images on Bluesky, you must use the -b option with conceal mode.
 
  $ jdvrif conceal -b my_image.jpg hidden.doc
 
  These images are only compatible for posting on the Bluesky platform. 
  Your embedded data file will be removed if posted on a different platform.
 
  You are required to use the Python script "bsky_post.py" (found in the repo src folder) to post the image to Bluesky.
  It will not work if you post images to Bluesky via the Bluesky browser site or mobile app.

  Script example:

   $ python3 bsky_post.py --handle exampleuser.bsky.social --password pxae-f17r-alp4-xqka --image jrif_11050.jpg
   --alt-text "alt-text to describe image..." "text to appear in main post..."

   You will also need to create an app password from your Bluesky account. (https://bsky.app/settings/app-passwords).

   Bluesky cover image size limit: 800KB. / Secret file size limit (Compressed): ~171KB. 

  -r (Reddit). To share/post compatible "file-embedded" JPG images on Reddit, you must use the -r option with conceal mode.
	
  $ jdvrif conceal -r my_image.jpg secret.mp3 

  From the Reddit site, click "Create Post" then select the "Images & Video" tab, to attach and post your JPG image.
  
  These images are only compatible for posting on the Reddit platform. 
  Your embedded data file will be removed if posted on a different platform.
  
 To correctly download images from X-Twitter or Reddit, click the image in the post to fully expand it, before saving.
		
 ImgPile - You must sign in to an account before sharing your "file-embedded" JPG image on this site.
 Sharing your image without logging in, your embedded data will not be preserved.

)";
}

