//	JPG Data Vehicle (jdvrif v1.5) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ jdvrif.cpp -O2 -s -o jdvrif

// 	Run it:
// 	$ ./jdvrif

#include <algorithm>
#include <fstream>
#include <filesystem>	
#include <regex>
#include <iostream>
#include <string>
#include <cstdint>
#include <vector>

typedef unsigned char BYTE;

struct JDV_STRUCT {
	const uint_fast8_t PROFILE_HEADER_LENGTH = 18;
	const size_t MAX_FILE_SIZE = 209715200, MAX_FILE_SIZE_REDDIT = 20971520, MIN_FILE_SIZE = 134;
	std::vector<BYTE> Image_Vec, File_Vec, Profile_Vec, Image_Header_Vec, Encrypted_Vec, Decrypted_Vec;
	std::vector<size_t> Profile_Header_Offset_Vec;
	std::string image_name, file_name;
	size_t image_size{}, file_size{};
	bool embed_file_mode = false, extract_file_mode = false, reddit_opt = false;
};

void
	// Attempt to open image file, followed by some basic checks to make sure user's image file meets program requiremensts.
	Check_Image_File(JDV_STRUCT&),
	// Some basic checks to make sure user data file meets program requirements.
	Check_Data_File(JDV_STRUCT&),
	// Fill vector with our modified ICC Profile data.
	Load_Profile_Vec(JDV_STRUCT&),
	// Find all the inserted ICC_Profile headers in the "file-embedded" image and store their index locations within vector "Profile_Header_Offset_Vec".
	// We use these index locations to skip the profile headers when decrypting the file, so that they don't get included within the extracted data file.
	Find_Profile_Headers(JDV_STRUCT&),
	// Depending on mode, encrypt or decrypt user's data file and its filename.
	Encrypt_Decrypt(JDV_STRUCT&),
	// Function splits user's data file into 65KB (or smaller) blocks by inserting ICC_Profile headers throughout the data file.
	Insert_Profile_Headers(JDV_STRUCT&),
	// Depending on mode, write out to file the file-embedded image file or the extracted data file.
	Write_Out_File(JDV_STRUCT&),
	// Display program infomation.
	Display_Info(),
	// Update values, such as block lengths, CRC, file sizes and other values. Writes values into the relevant vector index locations.
	Value_Updater(std::vector<BYTE>&, size_t, const size_t&, uint_fast8_t),
	// Check args input for invalid data.
	Check_Arguments_Input(const std::string&);

int main(int argc, char** argv) {

	JDV_STRUCT jdv;

	if (argc == 2 && std::string(argv[1]) == "--info") {
		Display_Info();
	} else if (argc == 3 && std::string(argv[1]) == "-x") {

		// Extract file mode.

		jdv.extract_file_mode = true;
		jdv.image_name = argv[2];

		Check_Image_File(jdv);

	} else if (argc >= 4 && argc < 6) {

		// Embed file mode.

		if (argc == 4 && std::string(argv[2]) == "-r") {
			std::cerr << "\nFile Error: Missing argument.\n\n";
			std::exit(EXIT_FAILURE);
		}

		jdv.embed_file_mode = true;

		if (argc == 5 && std::string(argv[2]) == "-r") {
			jdv.reddit_opt = true;
		}

		jdv.image_name = jdv.reddit_opt ? argv[3] : argv[2];
		jdv.file_name = jdv.reddit_opt ? argv[4] : argv[3];

		Check_Image_File(jdv);

	} else {
		std::cout << "\nUsage: jdvrif -e [-r] <cover_image> <data_file>\n\t\bjdvrif -x <file_embedded_image>\n\t\bjdvrif --info\n\n";
	}
	return 0;
}

void Check_Image_File(JDV_STRUCT& jdv) {

	const std::string GET_JPG_EXTENSION = jdv.image_name.length() > 3 ? jdv.image_name.substr(jdv.image_name.length() - 4) : jdv.image_name;

	std::ifstream read_image_fs(jdv.image_name, std::ios::binary);

	// Make sure image file opened successfully and has correct extension.
	if (!read_image_fs || GET_JPG_EXTENSION != ".jpg" && GET_JPG_EXTENSION != "jpeg" && GET_JPG_EXTENSION != "jiff") {
		// Open file failure, display relevant error message and exit program.
		std::cerr << (!read_image_fs ? "\nRead File Error: Unable to open image file.\n\n" : "\nImage File Error: Image file does not contain a valid extension.\n\n");
		std::exit(EXIT_FAILURE);
	}

	// Check JPG image for valid file size requirements.

	jdv.image_size = std::filesystem::file_size(jdv.image_name);

	if (jdv.image_size > jdv.MAX_FILE_SIZE || jdv.reddit_opt && jdv.image_size > jdv.MAX_FILE_SIZE_REDDIT || jdv.MIN_FILE_SIZE > jdv.image_size) {
		// Image size is smaller or larger than the set size limits. Display relevant error message and exit program.
		std::cerr << "\nImage File Error: " << (jdv.MIN_FILE_SIZE > jdv.image_size ? "Size of image is too small to be a valid PNG image"
			: "Size of image exceeds the maximum limit of " + (jdv.reddit_opt ? std::to_string(jdv.MAX_FILE_SIZE_REDDIT) + " Bytes"
			: std::to_string(jdv.MAX_FILE_SIZE)) + " Bytes") << ".\n\n";
		
		std::exit(EXIT_FAILURE);
	}

	// Display start message. Different depending on mode and options selected.
	std::cout << (jdv.embed_file_mode && jdv.reddit_opt ? "\nEmbed mode selected with -r option.\n\nReading files"		
			: (jdv.embed_file_mode ? "\nEmbed mode selected.\n\nReading files"
			: "\neXtract mode selected.\n\nReading JPG image file")) << ". Please wait...\n";

	// Store JPG image (or file-embedded image) into vector "Image_Vec".
	jdv.Image_Vec.assign(std::istreambuf_iterator<char>(read_image_fs), std::istreambuf_iterator<char>());

	// Update image size variable with vector size of the image file.
	jdv.image_size = jdv.Image_Vec.size();

	// Now that the image is stored within a vector, we can continue with our checks on the image file.
	const std::string
		JPG_START_SIG = "\xFF\xD8\xFF",	// JPG image header signature. 
		JPG_END_SIG = "\xFF\xD9",	// JPG end of image file signature.	
		IMAGE_START_SIG{ jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + JPG_START_SIG.length() },	// Get image header signature from vector. 
		IMAGE_END_SIG{ jdv.Image_Vec.end() - JPG_END_SIG.length(), jdv.Image_Vec.end() };

	// Make sure we are dealing with a valid JPG image file.
	if (IMAGE_START_SIG != JPG_START_SIG || IMAGE_END_SIG != JPG_END_SIG) {
		// File requirements failure, display relevant error message and exit program.
		std::cerr << "\nImage File Error: This is not a valid JPG image.\n\n";
		
		std::exit(EXIT_FAILURE);
	}

	if (jdv.embed_file_mode) {

		// An embedded JPG thumbnail will cause problems with this program. Search and remove blocks like "Exif" that may contain a JPG thumbnail.
		const std::string
			EXIF_SIG = "Exif\x00\x00II",
			EXIF_END_SIG = "xpacket end",
			ICC_PROFILE_SIG = "ICC_PROFILE";

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
			const uint_fast16_t EXIF_BLOCK_SIZE = (static_cast<size_t>(jdv.Image_Vec[EXIF_START_POS - 2]) << 8)
							| (static_cast<size_t>(jdv.Image_Vec[EXIF_START_POS - 1]));
			// Remove it.
			jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + EXIF_BLOCK_SIZE - 2);
		}

		// ^ Any JPG embedded thumbnail should have now been removed.

		if (!jdv.reddit_opt) { // Skip this if Reddit option selected.

			// Signature for Define Quantization Table(s) 
			const auto DQT_SIG = { 0xFF, 0xDB };

			// Find location in vector "Image_Vec" of first DQT index location of the image file.
			const size_t DQT_POS = std::search(jdv.Image_Vec.begin(), jdv.Image_Vec.end(), DQT_SIG.begin(), DQT_SIG.end()) - jdv.Image_Vec.begin();

			// Erase the first n bytes of the JPG header before the DQT position. We later replace the deleted header with the contents of vector "Profile_Vec".
			jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + DQT_POS);

			// Update image size
			jdv.image_size = jdv.Image_Vec.size();
		}
		Check_Data_File(jdv);

	} else { // Extract Mode.

		// Check to make sure we have a valid jdvrif embedded image.

		const std::string JDV_SIG = "JDVRiF";
		
		// Search image file for above signature.
		const size_t JDV_SIG_INDEX = std::search(jdv.Image_Vec.begin(), jdv.Image_Vec.end(), JDV_SIG.begin(), JDV_SIG.end()) - jdv.Image_Vec.begin();

		if (JDV_SIG_INDEX == jdv.Image_Vec.size()) { 
			// No signature found. Display relevant error message and exit program.
			std::cerr << "\nImage File Error: This is not a valid jdvrif file-embedded image.\n\n";
			
			std::exit(EXIT_FAILURE);

		} else { // Signature found.
			
			// Remove all data from the vector "Image_Vec" before the JDV_SIG_INDEX value.
			jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + JDV_SIG_INDEX );

			Find_Profile_Headers(jdv);
		}		
	}
}

void Check_Data_File(JDV_STRUCT& jdv) {

	// Checks the data file.

	std::ifstream read_file_fs(jdv.file_name, std::ios::binary);

	// Make sure data file opened successfully.
	if (!read_file_fs) {
		// Open file failure, display relevant error message and exit program.
		std::cerr << "\nRead File Error: Unable to open data file.\n\n";
		
		std::exit(EXIT_FAILURE);
	}

	jdv.file_size = std::filesystem::file_size(jdv.file_name);

	const size_t LAST_SLASH_POS = jdv.file_name.find_last_of("\\/");

	// Check for and remove "./" or ".\" characters at the start of the filename. 
	if (LAST_SLASH_POS <= jdv.file_name.length()) {
		const std::string_view NO_SLASH_NAME(jdv.file_name.c_str() + (LAST_SLASH_POS + 1), jdv.file_name.length() - (LAST_SLASH_POS + 1));
		jdv.file_name = NO_SLASH_NAME;
	}

	const uint_fast8_t MAX_FILENAME_LENGTH = 23;
	const size_t FILE_NAME_LENGTH = jdv.file_name.length();

	if (jdv.file_size > jdv.MAX_FILE_SIZE
		|| jdv.reddit_opt && jdv.file_size > jdv.MAX_FILE_SIZE_REDDIT
		|| FILE_NAME_LENGTH > jdv.file_size
		|| FILE_NAME_LENGTH > MAX_FILENAME_LENGTH) {
		std::cerr << "\nData File Error: " << (FILE_NAME_LENGTH > MAX_FILENAME_LENGTH ? "Length of file name is too long.\n\nFor compatibility requirements, length of file name must be under 24 characters"
			: (FILE_NAME_LENGTH > jdv.file_size ? "Size of file is too small.\n\nFor compatibility requirements, file size must be greater than the length of the file name"
			: "Size of file exceeds the maximum limit of " + (jdv.reddit_opt ? std::to_string(jdv.MAX_FILE_SIZE_REDDIT) + " Bytes"
			: std::to_string(jdv.MAX_FILE_SIZE)) + " Bytes")) << ".\n\n";
		
		std::exit(EXIT_FAILURE);
	}

	// Read-in and store user's data file into vector "File_Vec".
	jdv.File_Vec.assign(std::istreambuf_iterator<char>(read_file_fs), std::istreambuf_iterator<char>());
	jdv.file_size = jdv.File_Vec.size();

	// Combined file size check.
	// Image size + File Size + Inserted Profile Header Size (18 bytes for every 65K of the data file).
	if (jdv.image_size + jdv.file_size + (jdv.file_size / 65535 * jdv.PROFILE_HEADER_LENGTH + jdv.PROFILE_HEADER_LENGTH) > (jdv.reddit_opt ? jdv.MAX_FILE_SIZE_REDDIT : jdv.MAX_FILE_SIZE)) {	 // Division approx. Don't care about remainder.
		// File size check failure, display error message and exit program.
		std::cerr << "\nImage File Error:\n\nThe combined size of the image file + data file exceeds the maximum limit for this program.\n\n";
		
		std::exit(EXIT_FAILURE);
	}

	Load_Profile_Vec(jdv);
}

void Load_Profile_Vec(JDV_STRUCT& jdv) {

	// 663 bytes of this vector contains the JPG image header + iCC_Profile (434 bytes), 
	// with the remining 229 bytes being fake JPG image data (FFDB, FFC2, FFC4, FFDA, etc), 
	// just to make it appear a (somewhat) normal image file.
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
		0x07, 0x0E, 0x20, 0x16, 0x12, 0x16, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
		0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xFF, 0xC2, 0x00, 0x11,
		0x08, 0x04, 0x00, 0x04, 0x00, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01,
		0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x1C, 0x00, 0x00, 0x01, 0x05, 0x01,
		0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x02, 0x04, 0x05, 0x06, 0x03, 0x07, 0x08, 0xFF, 0xC4, 0x00,
		0x1A, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x02, 0x04, 0x05,
		0x06, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10,
		0x00, 0x00, 0x01
	};

	std::cout << "\nEncrypting data file.\n";

	// Encrypt the user's data file and its file name.
	Encrypt_Decrypt(jdv);
}

void Find_Profile_Headers(JDV_STRUCT& jdv) {

	std::cout << "\nFound jdvrif embedded data file.\n";

	const uint_fast8_t
		NAME_LENGTH_INDEX = 38,		// Index location within vector "Image_Vec" for length value of filename for user's data file.
		NAME_LENGTH = jdv.Image_Vec[NAME_LENGTH_INDEX],	// Get embedded value of filename length from "Image_Vec", stored within the main profile.
		NAME_INDEX = 39,		// Start index location within vector "Image_Vec" of filename for user's data file.
		PROFILE_COUNT_INDEX = 96,	// Value index location within vector "Image_Vec" for the total number of inserted iCC-Profile headers.
		FILE_SIZE_INDEX = 102;		// Start index location within vector "Image_Vec" for the file size value of the user's data file.

	const uint_fast16_t FILE_INDEX = 621; 	// Start index location within vector "Image_Vec" for the user's data file.

	// From the relevant index location, get size value of user's data file from "Image_Vec", stored within the main profile.
	const size_t EMBEDDED_FILE_SIZE = ((static_cast<size_t>(jdv.Image_Vec[FILE_SIZE_INDEX]) << 24)
				| (static_cast<size_t>(jdv.Image_Vec[FILE_SIZE_INDEX + 1]) << 16)
				| (static_cast<size_t>(jdv.Image_Vec[FILE_SIZE_INDEX + 2]) << 8)
				| (static_cast<size_t>(jdv.Image_Vec[FILE_SIZE_INDEX + 3])));

	// Signature string for the embedded profile headers we need to find within the user's data file.
	const std::string PROFILE_SIG = "ICC_PROFILE";

	// From vector "Image_Vec", get the total number of embedded profile headers value, stored within the main profile.
	uint_fast16_t profile_count = (static_cast<size_t>(jdv.Image_Vec[PROFILE_COUNT_INDEX]) << 8)
				| (static_cast<size_t>(jdv.Image_Vec[PROFILE_COUNT_INDEX + 1]));

	// Get the encrypted filename from vector "Image_Vec", stored within the main profile.
	jdv.file_name = { jdv.Image_Vec.begin() + NAME_INDEX, jdv.Image_Vec.begin() + NAME_INDEX + jdv.Image_Vec[NAME_LENGTH_INDEX] };

	std::cout << "\nExtracting encrypted data file from the JPG image.\n";

	// Erase the 621 byte main profile from vector "Image_Vec", so that the start of vector "Image_Vec" is now the beginning of the user's encrypted data file.
	jdv.Image_Vec.erase(jdv.Image_Vec.begin(), jdv.Image_Vec.begin() + FILE_INDEX);

	size_t profile_header_index = 0; // Variable will store the index locations within vector "Image_Vec" of each profile header found.

	if (profile_count) { // If one or more profiles.

		// Within "Image_Vec" find all occurrences of the 18 byte profile headers & store their offset index location within vector "Profile_Header_Offset_Vec".
		while (profile_count--) {
			jdv.Profile_Header_Offset_Vec.emplace_back(profile_header_index = std::search(jdv.Image_Vec.begin() + profile_header_index + 5, jdv.Image_Vec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - jdv.Image_Vec.begin() - 4);
		}
	}

	// Remove any data after user's data file so that vector "Image_Vec" will contain just the user's encrypted data file.
	if (jdv.reddit_opt) {
		jdv.Image_Vec.erase(jdv.Image_Vec.end() - 2, jdv.Image_Vec.end());
	} else {
		jdv.Image_Vec.erase(jdv.Image_Vec.begin() + EMBEDDED_FILE_SIZE, jdv.Image_Vec.end());
	}

	std::cout << "\nDecrypting data file.\n";

	// Decrypt the contents of vector "Image_Vec".
	Encrypt_Decrypt(jdv);
}

void Encrypt_Decrypt(JDV_STRUCT& jdv) {

	const std::string
		XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF",	// String used to XOR encrypt/decrypt the filename of user's data file.
		INPUT_NAME = jdv.file_name;

	std::string output_name;

	size_t
		file_size = jdv.embed_file_mode ? jdv.File_Vec.size() : jdv.Image_Vec.size(),	 // File size of user's data file.
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
		} else {
			xor_key_pos = xor_key_pos > XOR_KEY.length() ? 0 : xor_key_pos;	// Reset XOR_KEY position to the start if it has reached last character.
			output_name += INPUT_NAME[index_pos] ^ XOR_KEY[xor_key_pos++];	// XOR each character of filename against characters of XOR_KEY string. 
											// Store output characters in "output_name".
											// Depending on mode, filename is either encrypted or decrypted.
		}

		if (jdv.embed_file_mode) {
			// Encrypt data file. XOR each byte of the data file within "jdv.File_Vec" against each character of the encrypted filename, "output_name". 
			// Store encrypted output in vector "jdv.Encrypted_Vec".
			jdv.Encrypted_Vec.emplace_back(jdv.File_Vec[index_pos++] ^ output_name[name_key_pos++]);
		} else {
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

	if (jdv.embed_file_mode) {

		const uint_fast16_t PROFILE_VEC_SIZE = 663;	// Byte size of main profile within vector "Profile_Vec". User's encrypted data file is stored at the end of the main profile.

		const uint_fast8_t
			PROFILE_NAME_LENGTH_INDEX = 80,	// Location index within the main profile "Profile_Vec" to store the filename length value of the user's data file.
			PROFILE_NAME_INDEX = 81;	// Location index within the main profile "Profile_Vec" to store the filename of the user's data file.

		// Update the character length value of the filename for user's data file. Write this value into the main profile of vector "Profile_Vec".
		jdv.Profile_Vec[PROFILE_NAME_LENGTH_INDEX] = static_cast<BYTE>(INPUT_NAME.length());

		// Write the encrypted filename within the main profile of vector "Profile_Vec".
		std::copy(output_name.begin(), output_name.end(), jdv.Profile_Vec.begin() + PROFILE_NAME_INDEX);

		// Insert contents of vector "Encrypted_Vec" within vector "Profile_Vec", combining the main profile with the user's encrypted data file.	
		jdv.Profile_Vec.insert(jdv.Profile_Vec.begin() + PROFILE_VEC_SIZE, jdv.Encrypted_Vec.begin(), jdv.Encrypted_Vec.end());

		jdv.File_Vec.clear();

		// Call function to insert profile headers into the user's data file, so as to split the data into 65KB (or smaller) profile blocks.
		Insert_Profile_Headers(jdv);

	} else { // Extract
		
		// Update string variable with the decrypted filename.
		jdv.file_name = output_name;

		// Write the extracted (decrypted) content out to file.
		Write_Out_File(jdv);
	}
}

void Insert_Profile_Headers(JDV_STRUCT& jdv) {

	const size_t PROFILE_VECTOR_SIZE = jdv.Profile_Vec.size();	// Get updated size for vector "Profile_Vec" after adding user's data file.

	const uint_fast16_t BLOCK_SIZE = 65535;	// Profile default block size 65KB (0xFFFF).

	size_t tally_size = 20;			// A value used in conjunction with the user's data file size. We keep incrementing this value by BLOCK_SIZE until
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

	std::cout << "\nEmbedding data file within the JPG image.\n";

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

	} else {
		
		// User's data file is greater than a single 65KB profile block. 
		// Use this section to split up the data content into 65KB profile blocks,
		// by inserting profile headers at the relevant index locations, until the final, remaining block of data.

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

		} else {  // For this branch we keep the extra "tally_size += BLOCK_SIZE +2", as we need to insert one more profile header into the file.
			
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
	if (jdv.reddit_opt) {
		jdv.Image_Vec.insert(jdv.Image_Vec.end() - 2, jdv.File_Vec.begin() + 18, jdv.File_Vec.end());
	} else {
		jdv.Image_Vec.insert(jdv.Image_Vec.begin(), jdv.File_Vec.begin(), jdv.File_Vec.end());
	}
	
	// Create a unique filename using time value for the complete output file. 

	srand((unsigned)time(NULL));  // For output filename.

	const std::string TIME_VALUE = std::to_string(rand());

	jdv.file_name = "jrif_" + TIME_VALUE.substr(0, 5) + ".jpg";

	std::cout << "\nWriting file-embedded JPG image out to disk.\n";

	Write_Out_File(jdv);
}

void Write_Out_File(JDV_STRUCT& jdv) {

	std::ofstream write_file_fs(jdv.file_name, std::ios::binary);

	if (!write_file_fs) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	if (jdv.embed_file_mode) {

		// Write out to disk image file embedded with the encrypted data file.
		write_file_fs.write((char*)&jdv.Image_Vec[0], jdv.Image_Vec.size());

		std::vector<std::string> Sites_Vec{ "_Flickr", "_ImgPile", "_ImgBB", "_PostImage", "_Imgur", "_Mastodon", "_Twitter" };

		const size_t IMG_SIZE = jdv.Image_Vec.size();

		const uint_fast32_t
			TWITTER_SIZE = 9557, 		// Bytes. (Twitter limit measured by data file size. The rest below are measured by image size with embedded data file).
			MASTODON_SIZE = 16777216,	// 16MB
			IMGUR_SIZE = 20971520,		// 20MB 
			REDDIT_SIZE = 20971520,		// 20MB (Requires -r option)
			POST_IMG_SIZE = 25165824,	// 24MB
			IMGBB_SIZE = 33554432,		// 32MB
			IMG_PILE_SIZE = 104857600;	// 100MB
							// Flickr is 200MB, this programs max size, no need to to make a variable for it.
		
		int_fast8_t compat_num = (jdv.File_Vec.size() <= TWITTER_SIZE ? 6 : (IMG_SIZE <= MASTODON_SIZE ? 5
						: (IMG_SIZE <= IMGUR_SIZE ? 4 : (IMG_SIZE <= POST_IMG_SIZE ? 3
						: (IMG_SIZE <= IMGBB_SIZE ? 2 : (IMG_SIZE <= IMG_PILE_SIZE ? 1
						: 0))))));

		std::cout << "\nCreated JPG image: " + jdv.file_name + '\x20' + std::to_string(IMG_SIZE) + " Bytes.\n";

		if (jdv.reddit_opt && REDDIT_SIZE >= IMG_SIZE) {
			std::cout << "\n**Warning**\n\nDue to your option selection, for compatibility reasons\nyou should only post this file-embedded JPG image on Reddit.\n";
		} else {
			std::cout << "\nBased on image/data size, you can post your JPG image on the following sites:\n\n";
			while (compat_num >= 0) {
				std::cout << Sites_Vec[compat_num--] << '\n';
			}
		}

		std::cout << "\nComplete!\n\nYou can now post your file-embedded JPG image on the relevant supported platforms.\n\n";

	} else {
		
		std::cout << "\nWriting data file out to disk.\n";

		// Write out to disk the extracted (decrypted) data file.
		write_file_fs.write((char*)&jdv.Decrypted_Vec[0], jdv.Decrypted_Vec.size());

		std::cout << "\nSaved file: " + jdv.file_name + '\x20' + std::to_string(jdv.Decrypted_Vec.size()) + " Bytes.\n";
		std::cout << "\nComplete! Please check your extracted file.\n\n";
	}
}

void Value_Updater(std::vector<BYTE>& vec, size_t value_insert_index, const size_t& VALUE, uint_fast8_t bits) {
	while (bits) {
		static_cast<size_t>(vec[value_insert_index++] = (VALUE >> (bits -= 8)) & 0xff);
	}
}

void Check_Arguments_Input(const std::string& FILE_NAME_INPUT) {

	const std::regex REG_EXP("(\\.[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+)?[a-zA-Z_0-9\\.\\\\\\s\\-\\/]+?(\\.[a-zA-Z0-9]+)?");

	if (!regex_match(FILE_NAME_INPUT, REG_EXP)) {
		std::cerr << "\nInvalid Input Error: Your file name: \"" + FILE_NAME_INPUT + "\" contains characters not supported by this program.\n\n";
		std::exit(EXIT_FAILURE);
	}
}

void Display_Info() {

	std::cout << R"(
JPG Data Vehicle (jdvrif v1.5). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

A simple command-line tool used to embed and extract any file type via a JPG image file.
Share your data-embedded image on the following compatible sites.

Image size limit is platform dependant:-

  Flickr (200MB), *ImgPile (100MB), ImgBB (32MB), PostImage (24MB), *Reddit (20MB / requires -r option), 
  *Imgur (20MB), Mastodon (16MB), *Twitter (~10KB / *Limit measured by data file size).

Arguments / options:	

  -e = Embed File Mode, 
  -x = eXtract File Mode,
  -r = Reddit option, used with -e (jdvrif -e -r cover_image.jpg data_file.doc).

*ImgPile - You must sign in to an account before sharing your data-embedded PNG image on this platform.
Sharing your image without logging in, your embedded data will not be preserved.

*Imgur issue: Data is still retained when the file-embedded JPG image is over 5MB, but Imgur reduces the dimension size of the image.
 
*Reddit issue: Requires -r option. Desktop/Browser only. Does not work with Reddit mobile app. Mobile app converts images to Webp format.

This program works on Linux and Windows.
)";
}
