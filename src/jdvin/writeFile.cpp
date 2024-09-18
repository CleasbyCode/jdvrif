bool writeFile(std::vector<uint_fast8_t>& Vec) {

	srand((unsigned)time(NULL));  

	const std::string 
		TIME_VALUE 		= std::to_string(rand()),
		EMBEDDED_IMAGE_FILENAME = "jrif_" + TIME_VALUE.substr(0, 5) + ".jpg";

	std::ofstream file_ofs(EMBEDDED_IMAGE_FILENAME, std::ios::binary);

	if (!file_ofs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		return false;
	}
	
	uint_fast32_t EMBEDDED_IMAGE_SIZE = static_cast<uint_fast32_t>(Vec.size());

	file_ofs.write((char*)&Vec[0], EMBEDDED_IMAGE_SIZE);

	std::cout << "\nSaved file-embedded JPG image: " + EMBEDDED_IMAGE_FILENAME + '\x20' + std::to_string(EMBEDDED_IMAGE_SIZE) + " Bytes.\n";
	return true;
}
