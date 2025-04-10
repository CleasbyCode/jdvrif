void displayInfo() {
	std::cout << R"(

JPG Data Vehicle (jdvout v4.0). 
Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

jdvout is a CLI tool for extracting hidden data from a jdvin "file-embedded" JPG image. 

Compile & run jdvout (Linux):
		
$ sudo apt-get install libsodium-dev
$ g++ main.cpp -O2 -lz -lsodium -s -o jdvout
$ sudo cp jdvout /usr/bin
$ jdvout

Usage: jdvout <file-embedded-image>
       jdvout --info

)";
}
