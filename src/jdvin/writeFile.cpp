#include "writeFile.h"       
#include <string>          
#include <fstream>         
#include <iostream>        
#include <random>               
#include <cstdlib>   

bool writeFile(std::vector<uint8_t>& vec) {
	std::random_device rd;
    	std::mt19937 gen(rd());
    	std::uniform_int_distribution<> dist(10000, 99999);  

	const std::string IMAGE_FILENAME = "jrif_" + std::to_string(dist(gen)) + ".jpg";

	std::ofstream file_ofs(IMAGE_FILENAME, std::ios::binary);

	if (!file_ofs) {
		return false;
	}
	
	const uint32_t IMAGE_SIZE = static_cast<uint32_t>(vec.size());

	file_ofs.write(reinterpret_cast<const char*>(vec.data()), IMAGE_SIZE);
	
	std::vector<uint8_t>().swap(vec);
	
	std::cout << "\nSaved \"file-embedded\" JPG image: " << IMAGE_FILENAME << " (" << IMAGE_SIZE << " bytes).\n";
	return true;
}
