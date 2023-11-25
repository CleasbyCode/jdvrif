//	JPG Data Vehicle (jdvrif v1.3) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ jdvrif.cpp -O2 -s -o jdvrif

// 	Run it:
// 	$ ./jdvrif

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <cstdint>
#include <vector>

typedef unsigned char BYTE;

struct JDV_STRUCT {
	const uint_fast8_t PROFILE_HEADER_LENGTH = 18;
	std::vector<BYTE> Image_Vec, File_Vec, Profile_Vec, Image_Header_Vec, Encrypted_Vec, Decrypted_Vec;
	std::vector<size_t> Profile_Header_Offset_Vec;
	std::string image_name, file_name;
	size_t image_size{}, file_size{};
	uint_fast8_t file_count{}, sub_file_count{};
	bool insert_file = false, extract_file = false;
};

void
	// Update values, such as chunk lengths, crc, file sizes and other values. Writes them into the relevant vector index locations.
	Value_Updater(std::vector<BYTE>&, size_t, const size_t&, uint_fast8_t),

	// Open user image & data file or just embedded image file (depends on selected mode). Display error & exit program if any file fails to open.
	Open_Files(char* [], JDV_STRUCT& jdv),

	// Finds all the inserted iCC-Profile headers in the "file-embedded" image and stores their index locations within vector "Profile_Header_Offset_Vec".
	// We will later use these index locations to skip the profile headers when decrypting the file, so that they don't get included within the extracted data file.
	Find_Profile_Headers(JDV_STRUCT& jdv),

	// Depending on mode, encrypt or decrypt user's data file and its filename.
	Encrypt_Decrypt(JDV_STRUCT& jdv),

	// Function splits user's data file into 65KB (or smaller) blocks by inserting iCC-Profile headers throughout the data file.
	Insert_Profile_Headers(JDV_STRUCT& jdv),

	// Depending on more, write out to file the embedded image file or the extracted data file.
	Write_Out_File(JDV_STRUCT& jdv),

	// Display program infomation
	Display_Info();

int main(int argc, char** argv) {

	JDV_STRUCT jdv;

	if (argc == 2 && std::string(argv[1]) == "--info") {
		argc = 0;
		Display_Info();
	}
	else if (argc >= 4 && argc < 12 && std::string(argv[1]) == "-i") { // Insert file mode.
		jdv.insert_file = true, jdv.sub_file_count = argc - 1, jdv.image_name = argv[2];
		argc -= 2;
		while (argc != 1) {  // We can insert up to 8 files at a time (outputs one image for each file).
			ptrdiff_t
				stdin_astring_pos = 0,
				stdin_bstring_pos = 0;
			jdv.file_count = argc, jdv.file_name = argv[3];
			stdin_astring_pos = jdv.image_name.find("stdin", 0);
			stdin_bstring_pos = jdv.file_name.find("stdin", 0);
			if (stdin_astring_pos >= 0 || stdin_bstring_pos >=0) {
				std::cerr << "\nUnexpected Input Error: Invalid file name(s).\n\n";
				std::exit(EXIT_FAILURE);
			}

			Open_Files(argv++, jdv);
			argc--;
		}
		argc = 1;
	}
	else if (argc >= 3 && argc < 11 && std::string(argv[1]) == "-x") { // Extract file mode.
		jdv.extract_file = true;
		while (argc >= 3) {  // We can extract files from up to 8 embedded images at a time.
			ptrdiff_t stdin_string_pos = 0;
			jdv.image_name = argv[2];
			stdin_string_pos = jdv.image_name.find("stdin", 0);
		    if (stdin_string_pos >= 0) {
				std::cerr << "\nUnexpected Input Error: Invalid file name(s).\n\n";
				std::exit(EXIT_FAILURE);
			}

			 Open_Files(argv++, jdv);
			argc--;
		}
	}
	else {
		std::cout << "\nUsage: jdvrif -i <jpg_image> <file(s)>\n\t\bjdvrif -x <jpg_image(s)>\n\t\bjdvrif --info\n\n";
		argc = 0;
	}
	if (argc != 0) {
		if (argc == 2) {
			std::cout << "\nComplete! Please check your extracted file(s).\n\n";
		}
		else {
			std::cout << "\nComplete!\n\nYou can now post your data-embedded JPG image(s) on the relevant supported platforms.\n\n";
		}
	}
	return 0;
}

void Open_Files(char* argv[], JDV_STRUCT& jdv) {

	std::ifstream
		read_image_fs(jdv.image_name, std::ios::binary),
		read_file_fs(jdv.file_name, std::ios::binary);
	
	if (jdv.insert_file && (!read_image_fs || read_image_fs.peek() == EOF || !read_file_fs || read_file_fs.peek() == EOF) || jdv.extract_file && !read_image_fs || jdv.extract_file && read_image_fs.peek() == EOF) {

		// Open file failure, display relevant error message and exit program.
		std::string
			ERR_MSG_COVER = "\nFile Error: Unable to open cover image or file is empty: ",
			ERR_MSG_DATA = "\nFile Error: Unable to open data file or file is empty: ",
			ERR_MSG_IMAGE = "\nFile Error: Unable to open embedded image or file is empty: ",
		
		ERR_MSG = jdv.insert_file && !read_image_fs || jdv.insert_file && read_image_fs.peek() == EOF ? ERR_MSG_COVER + "\"" + jdv.image_name + "\"\n\n" 
				: jdv.insert_file && !read_file_fs || jdv.insert_file && read_file_fs.peek() == EOF ? ERR_MSG_DATA + "\"" + jdv.file_name + "\"\n\n"
				: ERR_MSG_IMAGE + "\"" + jdv.image_name + "\"\n\n";

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	const std::string START_MSG = jdv.insert_file ? "\nInsert mode selected.\n\nReading files. Please wait...\n"
		: "\nExtract mode selected.\n\nReading embedded JPG image file. Please wait...\n";

	std::cout << START_MSG;

	// Read-in and store JPG image (or data-embedded image file) into vector "Image_Vec".
	jdv.Image_Vec.assign(std::istreambuf_iterator<char>(read_image_fs), std::istreambuf_iterator<char>());

	// Get size of image file (or "file-embedded" image file).
	jdv.image_size = jdv.Image_Vec.size();

	const std::string
		JPG_SIG = "\xFF\xD8\xFF",	// JPG image signature. 
		GET_JPG_SIG{ jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + JPG_SIG.length() };	// Get image header from vector. 

	// Make sure we are dealing with a valid JPG image file.
	if (GET_JPG_SIG != JPG_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	if (jdv.extract_file) { // Extract mode.

		const uint_fast8_t
			JPG_HEADER_SIZE = jdv.PROFILE_HEADER_LENGTH,
			JDV_SIG_INDEX = 42,		// Standard signature index location within vector "Image_Vec"
			JDV_SIG_INDEX_IMGUR = 24;	// Shorter signature index location within vector "Image_Vec". 
							// This is written by Imgur, which removes part of the JPG header when saving images from their site.

		if (jdv.Image_Vec[JDV_SIG_INDEX] != 'J' && jdv.Image_Vec[JDV_SIG_INDEX + 5] != 'F'
			&& jdv.Image_Vec[JDV_SIG_INDEX_IMGUR] != 'J' && jdv.Image_Vec[JDV_SIG_INDEX_IMGUR + 5] != 'F') {

			std::cerr << "\nImage Error: File is not a jdvrif data-embedded image.\n\n";
			std::exit(EXIT_FAILURE);
		}

		if (jdv.Image_Vec[JDV_SIG_INDEX_IMGUR] == 'J' && jdv.Image_Vec[JDV_SIG_INDEX_IMGUR + 5] == 'F') {

			// If the JPG header has been shortened by Imgur, we need to put back the standard length header.
			jdv.Image_Header_Vec = { 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 };

			// Insert the 18 byte JPG header into vector "jdv.Image_Vec". Data alignment restored. We should now be able to decrypt & extract the embedded data. 
			jdv.Image_Vec.insert(jdv.Image_Vec.begin() + 2, jdv.Image_Header_Vec.begin(), jdv.Image_Header_Vec.end());
		}

		std::cout << "\nFound jdvrif embedded data file.\n";

		Find_Profile_Headers(jdv);
	}

	else {	// Insert mode.

		// Read-in and store user's data file into vector "File_Vec".
		jdv.File_Vec.assign(std::istreambuf_iterator<char>(read_file_fs), std::istreambuf_iterator<char>());
		jdv.file_size = jdv.File_Vec.size();

			// 663 bytes of this vector contains the main iCC-Profile (434 bytes), 
			// with the remining 229 bytes being fake JPG image data (FFDB, FFC2, FFC4, FFDA, etc), 
			// in order to make it look (somewhat) normal.
			// The user's encrypted data file will be added to the end of this profile.
			jdv.Profile_Vec = {
				0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01,
				0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0xE2, 0xFF, 0xFF,
				0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00,
				0x01, 0x01, 0x00, 0x00, 0xFF, 0xEF, 0x4A, 0x44, 0x56, 0x52, 0x69, 0x46,
				0x00, 0x00, 0x6D, 0x6E, 0x74, 0x72, 0x52, 0x47, 0x42, 0x20, 0x58, 0x59,
				0x5A, 0x20, 0x07, 0xE0, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x61, 0x63, 0x73, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
				0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD3, 0x2D, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x09, 0x64, 0x65, 0x73, 0x63, 0x00, 0x00, 0x00, 0xF0, 0x00, 0x00,
				0x00, 0x24, 0x72, 0x58, 0x59, 0x5A, 0x00, 0x00, 0x01, 0x14, 0x00, 0x00,
				0x00, 0x14, 0x67, 0x58, 0x59, 0x5A, 0x00, 0x00, 0x01, 0x28, 0x00, 0x00,
				0x00, 0x14, 0x62, 0x58, 0x59, 0x5A, 0x00, 0x00, 0x01, 0x3C, 0x00, 0x00,
				0x00, 0x14, 0x77, 0x74, 0x70, 0x74, 0x00, 0x00, 0x01, 0x50, 0x00, 0x00,
				0x00, 0x14, 0x72, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0x64, 0x00, 0x00,
				0x00, 0x28, 0x67, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0x64, 0x00, 0x00,
				0x00, 0x28, 0x62, 0x54, 0x52, 0x43, 0x00, 0x00, 0x01, 0x64, 0x00, 0x00,
				0x00, 0x28, 0x63, 0x70, 0x72, 0x74, 0x00, 0x00, 0x01, 0x8C, 0x00, 0x00,
				0x00, 0x00, 0x6D, 0x6C, 0x75, 0x63, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x01, 0x00, 0x00, 0x00, 0x0C, 0x65, 0x6E, 0x55, 0x53, 0x00, 0x00,
				0x00, 0x08, 0x00, 0x00, 0x00, 0x1C, 0x00, 0x73, 0x00, 0x52, 0x00, 0x47,
				0x00, 0x42, 0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x6F, 0xA2, 0x00, 0x00, 0x38, 0xF5, 0x00, 0x00, 0x03, 0x90, 0x58, 0x59,
				0x5A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x62, 0x99, 0x00, 0x00,
				0xB7, 0x85, 0x00, 0x00, 0x18, 0xDA, 0x58, 0x59, 0x5A, 0x20, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x24, 0xA0, 0x00, 0x00, 0x0F, 0x84, 0x00, 0x00,
				0xB6, 0xCF, 0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xD3, 0x2D, 0x70, 0x61,
				0x72, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02,
				0x66, 0x66, 0x00, 0x00, 0xF2, 0xA7, 0x00, 0x00, 0x0D, 0x59, 0x00, 0x00,
				0x13, 0xD0, 0x00, 0x00, 0x0A, 0x5B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x04, 0x04, 0x04, 0x04, 0x04,
				0x04, 0x07, 0x04, 0x04, 0x07, 0x0A, 0x07, 0x07, 0x07, 0x0A, 0x0D, 0x0A,
				0x0A, 0x0A, 0x0A, 0x0D, 0x10, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x10, 0x14,
				0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
				0x14, 0x14, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1C, 0x1C, 0x1C, 0x1C,
				0x1C, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0xFF,
				0xDB, 0x00, 0x43, 0x01, 0x05, 0x05, 0x05, 0x08, 0x07, 0x08, 0x0E, 0x07,
				0x07, 0x0E, 0x20, 0x16, 0x12, 0x16, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
				0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
				0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
				0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
				0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0xFF, 0xC2, 0x00, 0x11,
				0x08, 0x04, 0x00, 0x04, 0x00, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01,
				0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x1C, 0x00, 0x00, 0x01, 0x05, 0x01,
				0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				0x01, 0x00, 0x02, 0x04, 0x05, 0x06, 0x03, 0x07, 0x08, 0xFF, 0xC4, 0x00,
				0x1A, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
				0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x04, 0x05,
				0x06, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10,
				0x00, 0x00, 0x01
		};

		const std::string
			EXIF_SIG = "Exif\x00\x00II",
			EXIF_END_SIG = "xpacket end",
			ICC_PROFILE_SIG = "ICC_PROFILE";

		// An embedded JPG thumbnail will cause problems with this program. Search and remove blocks like "Exif" that may contain a JPG thumbnail.

		// Check for an iCC Profile and delete all content before the beginning of the profile. This removes any embedded JPG thumbnail. Profile deleted later.
		const size_t ICC_PROFILE_POS = std::search(jdv.Image_Vec.begin(), jdv.Image_Vec.end(), ICC_PROFILE_SIG.begin(), ICC_PROFILE_SIG.end()) - jdv.Image_Vec.begin();
		if (jdv.Image_Vec.size() > ICC_PROFILE_POS) {
			jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + ICC_PROFILE_POS);
		}

		// If no profile found, search for "Exif2 block (look for end signature "xpacket end") and remove the block.
		const size_t EXIF_END_POS = std::search(jdv.Image_Vec.begin(), jdv.Image_Vec.end(), EXIF_END_SIG.begin(), EXIF_END_SIG.end()) - jdv.Image_Vec.begin();
		if (jdv.Image_Vec.size() > EXIF_END_POS) {
			jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + (EXIF_END_POS + 17));
		}

		// Remove "Exif" block that has no "xpacket end" signature.
		const size_t EXIF_START_POS = std::search(jdv.Image_Vec.begin(), jdv.Image_Vec.end(), EXIF_SIG.begin(), EXIF_SIG.end()) - jdv.Image_Vec.begin();
		if (jdv.Image_Vec.size() > EXIF_START_POS) {
			// Get size of "Exif" block
			const uint_fast16_t EXIF_BLOCK_SIZE = jdv.Image_Vec[EXIF_START_POS - 2] << 8 | jdv.Image_Vec[EXIF_START_POS - 1];
			// Remove it.
			jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + EXIF_BLOCK_SIZE - 2);
		}

		// ^ Any JPG embedded thumbnail should have now been removed.

		// Signature for Define Quantization Table(s) 
		const auto DQT_SIG = { 0xFF, 0xDB };

		const size_t 
			DQT_POS = std::search(jdv.Image_Vec.begin(), jdv.Image_Vec.end(), DQT_SIG.begin(), DQT_SIG.end()) - jdv.Image_Vec.begin(), // Find location in vector "Image_Vec" of first DQT index location of the image file.
			LAST_SLASH_POS = jdv.file_name.find_last_of("\\/"),
			MAX_FILE_SIZE = 209715200; // 200MB file size limit for this program. 

		// Erase the first n bytes of the JPG header before the DQT position. We later replace the erased header with the contents of vector "Profile_Vec".
		jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + DQT_POS);

		// Update image size
		jdv.image_size = jdv.Image_Vec.size();

		// Image size + File Size + Inserted Profile Header Size (18 bytes for every 65K of the data file).
		if (jdv.image_size + jdv.file_size + (jdv.file_size / 65535 * jdv.PROFILE_HEADER_LENGTH + jdv.PROFILE_HEADER_LENGTH) > MAX_FILE_SIZE) {	 // Division approx. Don't care about remainder.

			// File size check failure, display error message and exit program.
			std::cerr << "\nFile Size Error: Your data file size must not exceed 200MB (209715200 Bytes).\n\n" <<
				"The data file size includes the image file size and the total size of profile headers,\n(18 bytes for every 65KB of the data file).\n\n";

			std::exit(EXIT_FAILURE);
		}

		// Check for and remove "./" or ".\" characters at the start of the filename. 
		if (LAST_SLASH_POS <= jdv.file_name.length()) {
			const std::string_view NO_SLASH_NAME(jdv.file_name.c_str() + (LAST_SLASH_POS + 1), jdv.file_name.length() - (LAST_SLASH_POS + 1));
			jdv.file_name = NO_SLASH_NAME;
		}

		const uint_fast8_t MAX_FILENAME_LENGTH = 23;

		// Make sure character length of filename does not exceed set maximum.
		if (jdv.file_name.length() > MAX_FILENAME_LENGTH) {
			std::cerr << "\nFile Error: Filename length of your data file is too long.\n"
				"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
			std::exit(EXIT_FAILURE);
		}
		// File size needs to be greater than filename length.
		else if (jdv.file_name.length() > jdv.file_size) {
			std::cerr << "\nFile Size Error: File size is too small.\n"
				"\nFor compatibility requirements, data file size must be greater than filename length.\n\n";
			std::exit(EXIT_FAILURE);
		}
		else {
			std::cout << "\nEncrypting data file.\n";

			// Encrypt the user's data file and its filename.
			Encrypt_Decrypt(jdv);
		}
	}
}

void Find_Profile_Headers(JDV_STRUCT& jdv) {

	const uint_fast8_t
		NAME_LENGTH_INDEX = 80,		// Index location within vector "Image_Vec" for length value of filename for user's data file.
		NAME_LENGTH = jdv.Image_Vec[NAME_LENGTH_INDEX],	// Get embedded value of filename length from "Image_Vec", stored within the main profile.
		NAME_INDEX = 81,		// Start index location within vector "Image_Vec" of filename for user's data file.
		PROFILE_COUNT_INDEX = 138,	// Value index location within vector "Image_Vec" for the total number of inserted iCC-Profile headers.
		FILE_SIZE_INDEX = 144;		// Start index location within vector "Image_Vec" for the file size value of the user's data file.

	const uint_fast16_t FILE_INDEX = 663; 	// Start index location within vector "Image_Vec" for the user's data file.

	// From the relevant index location, get size value of user's data file from "Image_Vec", stored within the main profile.
	const uint_fast32_t EMBEDDED_FILE_SIZE = jdv.Image_Vec[FILE_SIZE_INDEX] << 24 | jdv.Image_Vec[FILE_SIZE_INDEX + 1] << 16 | jdv.Image_Vec[FILE_SIZE_INDEX + 2] << 8 | jdv.Image_Vec[FILE_SIZE_INDEX + 3];

	// Signature string for the embedded profile headers we need to find within the user's data file.
	const std::string PROFILE_SIG = "ICC_PROFILE";

	// From vector "Image_Vec", get the total number of embedded profile headers value, stored within the main profile.
	uint_fast16_t profile_count = jdv.Image_Vec[PROFILE_COUNT_INDEX] << 8 | jdv.Image_Vec[PROFILE_COUNT_INDEX + 1];

	// Get the encrypted filename from vector "Image_Vec", stored within the main profile.
	jdv.file_name = { jdv.Image_Vec.begin() + NAME_INDEX, jdv.Image_Vec.begin() + NAME_INDEX + jdv.Image_Vec[NAME_LENGTH_INDEX] };

	std::cout << "\nExtracting encrypted data file from the JPG image.\n";

	// Erase the 663 byte main profile from vector "Image_Vec", so that the start of "Image_Vec" is now the beginning of the user's encrypted data file.
	jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + FILE_INDEX);

	size_t profile_header_index = 0; // Variable will store the index locations within vector "Image_Vec" of each profile header found.

	if (profile_count) { // If one or more profiles.

		// Within "Image_Vec" find all occurrences of the 18 byte profile headers & store their offset index location within vector "Profile_Header_Offset_Vec".
		while (profile_count--) {
			jdv.Profile_Header_Offset_Vec.emplace_back(profile_header_index = std::search(jdv.Image_Vec.begin() + profile_header_index + 5, jdv.Image_Vec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - jdv.Image_Vec.begin() - 4);
		}
	}

	// Remove the JPG image from the user's data file. Erase all bytes starting from the end of the "EXTRACTED_FILE_SIZE" value. 
	// Vector "Image_Vec" now contains just the user's encrypted data file.
	jdv.Image_Vec.erase(jdv.Image_Vec.begin() + EMBEDDED_FILE_SIZE, jdv.Image_Vec.end());

	std::cout << "\nDecrypting extracted data file.\n";

	// Decrypt the contents of vector "Image_Vec".
	Encrypt_Decrypt(jdv);
}

void Encrypt_Decrypt(JDV_STRUCT& jdv) {

	const std::string
		XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF",	// String used to XOR encrypt/decrypt the filename of user's data file.
		INPUT_NAME = jdv.file_name;

	std::string output_name;

	size_t
		file_size = jdv.insert_file ? jdv.File_Vec.size() : jdv.Image_Vec.size(),	 // File size of user's data file.
		index_pos = 0;	// When encrypting/decrypting the filename, this variable stores the index character position of the filename,
				// When encrypting/decrypting the user's data file, this variable is used as the index position of where to 
				// insert each byte of the data file into the relevant "encrypted" or "decrypted" vectors.

	uint_fast16_t offset_index = 0;	// Index of offset location value within vector "Profile_Header_Offset_Vec".

	uint_fast8_t
		xor_key_pos = 0,	// Character position variable for XOR_KEY string.
		name_key_pos = 0;	// Character position variable for filename string (output_name / INPUT_NAME).

	// XOR encrypt/decrypt filename and user's data file.
	while (file_size > index_pos) {

		if (index_pos >= INPUT_NAME.length()) {
			name_key_pos = name_key_pos > INPUT_NAME.length() ? 0 : name_key_pos;	 // Reset filename character position to the start if it has reached last character.
		}

		else {
			xor_key_pos = xor_key_pos > XOR_KEY.length() ? 0 : xor_key_pos;	// Reset XOR_KEY position to the start if it has reached last character.
			output_name += INPUT_NAME[index_pos] ^ XOR_KEY[xor_key_pos++];	// XOR each character of filename against characters of XOR_KEY string. Store output characters in "output_name".
											// Depending on mode, filename is either encrypted or decrypted.
		}

		if (jdv.insert_file) {
			// Encrypt data file. XOR each byte of the data file within "jdv.File_Vec" against each character of the encrypted filename, "output_name". 
			// Store encrypted output in vector "jdv.Encrypted_Vec".
			jdv.Encrypted_Vec.emplace_back(jdv.File_Vec[index_pos++] ^ output_name[name_key_pos++]);
		}

		else {
			// Decrypt data file: XOR each byte of the data file within vector "jdv.Image_Vec" against each character of the encrypted filename, "INPUT_NAME". 
			// Store decrypted output in vector "jdv.Decrypted_Vec".
			jdv.Decrypted_Vec.emplace_back(jdv.Image_Vec[index_pos++] ^ INPUT_NAME[name_key_pos++]);

			// While decrypting, we need to check for and skip over any occurrence of the iCC-Profile headers inserted throughout the encrypted file.
			if (jdv.Profile_Header_Offset_Vec.size() && index_pos == jdv.Profile_Header_Offset_Vec[offset_index]) {

				// We have found a location for a profile header. Skip over it so that we don't include it within the decrypted output file.
				// Skipping the profile headers is much quicker than removing them with the Vector erase command.
				index_pos += jdv.PROFILE_HEADER_LENGTH; // From the current location, skip the 18 byte profile header.
				file_size += jdv.PROFILE_HEADER_LENGTH; // Increase file_size variable by 18 bytes to keep things in alignment.
				offset_index++;	// Move to the next profile header offset index location value within vector "Profile_Header_Offset_Vec".
			}
		}
	}

	if (jdv.insert_file) {

		const uint_fast16_t PROFILE_VEC_SIZE = 663;	// Byte size of main profile within vector "Profile_Vec". User's encrypted data file is stored at the end of the main profile.

		const uint_fast8_t 
			PROFILE_NAME_LENGTH_INDEX = 80,	// Location index within the main profile "Profile_Vec" to store the filename length value of the user's data file.
			PROFILE_NAME_INDEX = 81;	// Location index within the main profile "Profile_Vec" to store the filename of the user's data file.

		// Update the character length value of the filename for user's data file. Write this value into the main profile of vector "Profile_Vec".
		jdv.Profile_Vec[PROFILE_NAME_LENGTH_INDEX] = static_cast<BYTE>(INPUT_NAME.length());

		// Make space for the filename by removing equivalent length of characters from main profile within vector "Profile_Vec".
		jdv.Profile_Vec.erase(jdv.Profile_Vec.begin() + PROFILE_NAME_INDEX, jdv.Profile_Vec.begin() + INPUT_NAME.length() + PROFILE_NAME_INDEX);

		// Insert the encrypted filename within the main profile of vector "Profile_Vec".
		jdv.Profile_Vec.insert(jdv.Profile_Vec.begin() + PROFILE_NAME_INDEX, output_name.begin(), output_name.end());

		// Insert contents of vector "Encrypted_Vec" within vector "Profile_Vec", combining the main profile with the user's encrypted data file.	
		jdv.Profile_Vec.insert(jdv.Profile_Vec.begin() + PROFILE_VEC_SIZE, jdv.Encrypted_Vec.begin(), jdv.Encrypted_Vec.end());

		jdv.File_Vec.clear();

		// Clear vector. This is important when embedding more than one file.
		jdv.Encrypted_Vec.clear();

		// Call function to insert profile headers into the user's data file, so as to split the data into 65KB (or smaller) profile blocks.
		Insert_Profile_Headers(jdv);
	}
	else { // Extract 

		// Update string variable with the decrypted filename.
		jdv.file_name = output_name;

		// Clear vector. This is important when embedding more than one file.
		jdv.Profile_Header_Offset_Vec.clear();

		// Write the extracted (decrypted) content out to file.
		Write_Out_File(jdv);
	}
}

void Insert_Profile_Headers(JDV_STRUCT& jdv) {

	const size_t PROFILE_VECTOR_SIZE = jdv.Profile_Vec.size();	// Get updated size for vector "Profile_Vec" after adding user's data file.

	const uint_fast16_t BLOCK_SIZE = 65535;	// Profile default block size 65KB (0xFFFF).

	size_t tally_size = 20;	// A value used in conjunction with the user's data file size. We keep incrementing this value by BLOCK_SIZE until
	// we reach near end of the file, which will be a value less than BLOCK_SIZE, the last iCC-Profile block.

	uint_fast16_t profile_count = 0;	// Keep count of how many profile headers that have been inserted into user's data file. We use this value when removing the headers.

	const uint_fast8_t
		PROFILE_HEADER_INDEX = 20,	// Start index location within "Profile_Vec" of the profile header (18 bytes).
		PROFILE_HEADER_SIZE_INDEX = 22,	// "Profile_Vec" start index location for the 2 byte jpg profile header size field.
		PROFILE_SIZE_INDEX = 40,	// "Profile_Vec" start index location for the 4 byte main profile size field.
		PROFILE_COUNT_INDEX = 138,	// Start index location within the main profile, where we store the value of the total number of inserted profile headers.
		PROFILE_DATA_SIZE_INDEX = 144;	// Start index location within the main profile, where we store the file size value of the user's data file.

	uint_fast8_t bits = 16;	// Variable used with the "Value_Updater" function. 2 bytes.

	// Get the 18 byte iCC-Profile Header from vector "Profile_Vec" and store it as a string.
	const std::string ICC_PROFILE_HEADER = { jdv.Profile_Vec.begin() + PROFILE_HEADER_INDEX, jdv.Profile_Vec.begin() + PROFILE_HEADER_INDEX + jdv.PROFILE_HEADER_LENGTH };

	std::cout << "\nEmbedding data file within the ICC Profile of the JPG image.\n";

	// Where we see +4 (-4) or +2, these values are the number of bytes at the start of vector "Profile_Vec" (4 bytes: 0xFF, 0xD8, 0xFF, 0xE2) 
	// and "ICC_PROFILE_HEADER" (2 bytes: 0xFF, 0xE2), just before the default "BLOCK_SIZE" size bytes: 0xFF, 0xFF, where the block count starts from. 
	// We need to count or subtract these bytes where relevant.

	if (BLOCK_SIZE + jdv.PROFILE_HEADER_LENGTH + 4 >= PROFILE_VECTOR_SIZE) {

		// Looks like we are dealing with a small data file. All data content of "Profile_Vec" fits within the first, main 65KB profile block of the image file. 
		// Finish up and write the "embedded" image out to file, exit program.

		// Get the updated size for the 2 byte JPG profile header size.
		// Get the updated size for the 4 byte main profile size. (only 2 bytes used, value is always 16 bytes less than the JPG profile header size). 
		const size_t
			PROFILE_HEADER_BLOCK_SIZE = PROFILE_VECTOR_SIZE - (jdv.PROFILE_HEADER_LENGTH + 4),
			PROFILE_BLOCK_SIZE = PROFILE_HEADER_BLOCK_SIZE - 16;

		// Insert the updated JPG profile header size for vector "Profile_Vec", as it is probably smaller than the set default value (0xFFFF).
		Value_Updater(jdv.Profile_Vec, PROFILE_HEADER_SIZE_INDEX, PROFILE_HEADER_BLOCK_SIZE, bits); 
		
		// Insert the updated main profile size for vector "Profile_Vec". Size is always 16 bytes less than the JPG profile header size above.
		Value_Updater(jdv.Profile_Vec, PROFILE_SIZE_INDEX, PROFILE_BLOCK_SIZE, bits);

		jdv.File_Vec.swap(jdv.Profile_Vec);
	}

	// User's data file is greater than a single 65KB profile block. 
	// Use this section to split up the data content into 65KB profile blocks,
	// by inserting profile headers at the relevant index locations, until the final, 
	// remaining block of data.

	else {

		size_t byte_index = 0;

		tally_size += BLOCK_SIZE + 2;

		while (PROFILE_VECTOR_SIZE > byte_index) {

			// Store byte at "byte_index" location of vector "Profile_Vec" within vector "File_Vec".	
			jdv.File_Vec.emplace_back(jdv.Profile_Vec[byte_index++]);

			// Does the current "byte_index" value match the the current "tally_size" value?
			// If match is found, we will insert a profile header into the data at the current location.
			if (byte_index == tally_size) {

				// Insert a profile header at this location within the file.
				jdv.File_Vec.insert(jdv.File_Vec.begin() + tally_size, ICC_PROFILE_HEADER.begin(), ICC_PROFILE_HEADER.end());

				// Update profile_count value after inserting the above profile header. 
				profile_count++;

				// Increment tally_size by another BLOCK_SIZE
				tally_size += BLOCK_SIZE + 2;
			}
		}

		// Almost all the profile headers have been inserted into the data file from the above while-loop.
		// We now have to deal with the last profile header.  Depending on the remaining data size, we may
		// have to insert one last profile header or we just need to update the last profile header size field,
		// to give it the correct size for the last block of data.

		// Most files should be delt with in this "if" branch. Other "edge cases" will be delt with in the "else" branch.
		if (tally_size > PROFILE_VECTOR_SIZE + (profile_count * jdv.PROFILE_HEADER_LENGTH) + 2) {

			// The while-loop leaves us with an extra "tally_size += BLOCK_SIZE + 2", which is one too many for this section, so we correct it here.
			tally_size -= BLOCK_SIZE + 2;

			// Update the 2 byte size field of the final profile header (last profile header has already been inserted from the above "while-loop").
			Value_Updater(jdv.File_Vec, tally_size + 2, PROFILE_VECTOR_SIZE - tally_size + (profile_count * jdv.PROFILE_HEADER_LENGTH) - 2, bits);
		}
		else
		{  // For this branch we keep the extra "tally_size += BLOCK_SIZE +2", as we need to insert one more profile header into the file.

			// Insert last profile header, required for the data file.
			jdv.File_Vec.insert(jdv.File_Vec.begin() + tally_size, ICC_PROFILE_HEADER.begin(), ICC_PROFILE_HEADER.end());

			// Update the profile count value.
			profile_count++;

			// Update the 2 byte size field of the final profile header.
			Value_Updater(jdv.File_Vec, tally_size + 2, PROFILE_VECTOR_SIZE - tally_size + (profile_count * jdv.PROFILE_HEADER_LENGTH) - 2, bits);
		}

		// Store the total profile_count value into vector "File_Vec", within the main profile. This value is required when extracting the data file.
		Value_Updater(jdv.File_Vec, PROFILE_COUNT_INDEX, profile_count, bits);
	}

	bits = 32; // 4 bytes.

	// Store file size value of the user's data file into vector "File_Vec", within the main profile. This value is required when extracting the data file.
	Value_Updater(jdv.File_Vec, PROFILE_DATA_SIZE_INDEX, jdv.file_size, bits);

	// Insert contents of vector "File_Vec" into vector "Image_Vec", combining the jpg image with user's data file (now split within 65KB iCC-Profile header blocks).	
	jdv.Image_Vec.insert(jdv.Image_Vec.begin(), jdv.File_Vec.begin(), jdv.File_Vec.end());

	// Clear vectors. This is important when embedding more than one file. (e.g. jdvrif -i image.jpg file1.zip file2.zip file3.zip).
	jdv.Profile_Header_Offset_Vec.clear();
	jdv.Profile_Vec.clear();
	jdv.File_Vec.clear();

	// If we embed multiple data files (max 8), each outputted image will be differentiated by a number in the filename, 
	// e.g. jdv_img1.jpg, jdv_img2.jpg, jdv_img3.jpg.
	const std::string DIFF_VALUE = std::to_string(jdv.sub_file_count - jdv.file_count);

	jdv.file_name = "jdv_img" + DIFF_VALUE + ".jpg";

	std::cout << "\nWriting data-embedded JPG image out to disk.\n";

	Write_Out_File(jdv);
}

void Write_Out_File(JDV_STRUCT& jdv) {

	std::ofstream write_file_fs(jdv.file_name, std::ios::binary);

	if (!write_file_fs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	if (jdv.insert_file) {

		// Write out to disk image file embedded with the encrypted data file.
		write_file_fs.write((char*)&jdv.Image_Vec[0], jdv.Image_Vec.size());

		std::string size_warning =
			"\n**Warning**\n\nDue to the file size of your data-embedded JPG image,\nyou will only be able to share this image on the following platforms: \n\n"
			"Flickr, ImgPile, ImgBB, PostImage, Imgur & *Reddit (Desktop/Browser only)";

		const size_t 
			IMG_SIZE = jdv.Image_Vec.size(),
			MSG_LEN = size_warning.length();

		const uint_fast32_t
			// Twitter 9KB. Not really supported because of the tiny size requirement, but if your data file is this size 
			// (9KB, 9216 bytes) or lower, then you should be able to use Twitter to share/tweet the "file-embedded" image.
			MASTODON_SIZE = 16777216,	// 16MB
			IMGUR_REDDIT_SIZE = 20971520,	// 20MB
			POST_IMG_SIZE = 25165824,	// 24MB
			IMGBB_SIZE = 33554432,		// 32MB
			IMG_PILE_SIZE = 104857600;	// 100MB
			// Flickr is 200MB, this programs max size, no need to to make a variable for it.

		size_warning = (IMG_SIZE > IMGUR_REDDIT_SIZE && IMG_SIZE <= POST_IMG_SIZE ? size_warning.substr(0, MSG_LEN - 40)
				: (IMG_SIZE > POST_IMG_SIZE && IMG_SIZE <= IMGBB_SIZE ? size_warning.substr(0, MSG_LEN - 51)
				: (IMG_SIZE > IMGBB_SIZE && IMG_SIZE <= IMG_PILE_SIZE ? size_warning.substr(0, MSG_LEN - 58)
				: (IMG_SIZE > IMG_PILE_SIZE ? size_warning.substr(0, MSG_LEN - 67) : size_warning))));

		if (IMG_SIZE > MASTODON_SIZE) {
			std::cerr << size_warning << ".\n";
		}

		std::cout << "\nCreated data-embedded JPG image: \"" + jdv.file_name + "\" Size: \"" << jdv.Image_Vec.size() << " Bytes\"\n";

		jdv.Image_Vec.clear();
	}
	else {

		std::cout << "\nWriting decrypted data file out to disk.\n";

		// Write out to disk the extracted (decrypted) data file.
		write_file_fs.write((char*)&jdv.Decrypted_Vec[0], jdv.Decrypted_Vec.size());

		std::cout << "\nSaved file: \"" + jdv.file_name + "\" Size: \"" << jdv.Decrypted_Vec.size() << " Bytes\"\n";

		// Clear vectors. This is important when extracting/decrypting more than one image.
		jdv.Decrypted_Vec.clear();
		jdv.Image_Vec.clear();
	}
}

void Value_Updater(std::vector<BYTE>& vec, size_t value_insert_index, const size_t& VALUE, uint_fast8_t bits) {
	while (bits) {
		vec[value_insert_index++] = (VALUE >> (bits -= 8)) & 0xff;
	}
}

void Display_Info() {

	std::cout << R"(
JPG Data Vehicle (jdvrif v1.3). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

A simple command-line tool used to embed and extract any file type via a JPG image file.
Share your data-embedded image on the following compatible sites.

Image size limit is platform dependant:-

  Flickr (200MB), *ImgPile (100MB), ImgBB (32MB), PostImage (24MB), 
  *Reddit (20MB), *Imgur (20MB), Mastodon (16MB).

Options: [-i] File insert mode.
	 [-x] File extract mode.

*ImgPile - You must sign in to an account before sharing your data-embedded PNG image on this platform.
Sharing your image without logging in, your embedded data will not be preserved.

*Imgur issue: Data is still retained when the file-embedded JPG image is over 5MB, but Imgur reduces the dimension size of the image.
 
*Reddit issue: Desktop/Browser only. Does not work with Reddit mobile app. Mobile app converts images to Webp format.

*Twitter: If your data file is only 9KB or lower, you can also use Twitter to share your data-embedded JPG image.
To share larger files on Twitter, (up to 5MB), please use pdvzip (PNG only).

This program works on Linux and Windows.

The file data is encrypted and inserted within multiple 65KB ICC Profile blocks in the JPG image file.
 
Using jdvrif, you can insert up to eight files at a time (outputs one image per file).

You can also extract files from up to eight images at a time.

)";
}
