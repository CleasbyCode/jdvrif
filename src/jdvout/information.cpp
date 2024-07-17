void displayInfo() {

	std::cout << R"(

JPG Data Vehicle (jdvout v1.0.8). 
Created by Nicholas Cleasby (@CleasbyCode) 24/01/2023.

A steganography-like CLI tool to extract hidden data from a (jdvin) JPG image. 

Compile & run jdvout (Linux):
		
$ g++ main.cpp -O2 -lz -s -o jdvout
$ ./jdvout

Usage: jdvout <file-embedded-image>
       jdvout --info

)";
}
