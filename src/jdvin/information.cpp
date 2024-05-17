void displayInfo() {

	std::cout << R"(
JPG Data Vehicle (jdvrif v1.7). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

A simple command-line tool used to embed and extract any file type via a JPG image file.
Share your data-embedded image on the following compatible sites.

Image size limit is platform dependant:-

  Flickr (200MB), *ImgPile (100MB), ImgBB (32MB), PostImage (24MB), *Reddit (20MB / requires -r option), 
  Mastodon (16MB), *Twitter (~10KB / *Limit measured by data file size).

Arguments / options:	

  -e = Embed File Mode, 
  -x = eXtract File Mode,
  -r = Reddit option, used with -e (jdvrif -e -r cover_image.jpg data_file.doc).

*ImgPile - You must sign in to an account before sharing your data-embedded PNG image on this platform.
Sharing your image without logging in, your embedded data will not be preserved.
 
*Reddit: Post images via new.reddit.com desktop/browser site only. Use the "Images & Video" tab/box.

This program works on Linux and Windows.
)";
}