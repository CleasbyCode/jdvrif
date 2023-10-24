//	JPG Data Vehicle (jdvrif v1.2) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023
//
//	To compile program (Linux):
// 	$ g++ jdvrif.cpp -O2 -s -o jdvrif

// 	Run it:
// 	$ ./jdvrif

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

typedef unsigned char BYTE;

struct jdvStruct {
	std::vector<BYTE> ImageVec, FileVec, ProfileVec, EncryptedVec, DecryptedVec;
	std::vector<uint32_t> ProfileHeaderOffsetVec;
	std::string IMAGE_NAME, FILE_NAME;
	uint64_t IMAGE_SIZE{}, FILE_SIZE{};
	const uint8_t PROFILE_HEADER_LENGTH = 0x12;
	uint8_t Img_Value{}, Sub_Value{};
	bool Insert_File = false, Extract_File = false;
};

// Update values, such as block size, file sizes and other values and write them into the relevant vector index locations. Overwrites previous values.
class ValueUpdater {
public:
	void Value(std::vector<BYTE>& vect, uint32_t Value_Insert_Index, const uint32_t VALUE, uint8_t Bits) {
		while (Bits) vect[Value_Insert_Index++] = (VALUE >> (Bits -= 8)) & 0xff;
	}
} *update;

// Open user image & data file or embedded image file. Display error & exit program if any file fails to open.
void openFiles(char* [], jdvStruct& jdv);

// Finds all the inserted iCC Profile headers in the "file-embedded" image and stores their index locations within vector "ProfileHeaderOffsetVec".
// We will later use these index locations to skip the profile headers when decrypting the file, so that they don't get included within the extracted data file.
void findProfileHeaders(jdvStruct& jdv);

// Encrypt or decrypt user's data file and its filename.
void encryptDecrypt(jdvStruct& jdv);

// Function splits user's data file up into 65KB (or smaller) blocks by inserting iCC Profile headers throughout the data file.
void insertProfileHeaders(jdvStruct& jdv);

// Write out to file the embedded image file or the extracted data file.
void writeOutFile(jdvStruct& jdv);

// Display program infomation
void displayInfo();

int main(int argc, char** argv) {

	jdvStruct jdv;

	if (argc == 2 && std::string(argv[1]) == "--info") {
		argc = 0;
		displayInfo();
	}
	else if (argc >= 4 && argc < 12 && std::string(argv[1]) == "-i") { // Insert file mode.
		jdv.Insert_File = true, jdv.Sub_Value = argc - 1, jdv.IMAGE_NAME = argv[2];
		argc -= 2;
		while (argc != 1) {  // We can insert up to 8 files at a time (outputs one image for each file).
			jdv.Img_Value = argc, jdv.FILE_NAME = argv[3];
			openFiles(argv++, jdv);
			argc--;
		}
		argc = 1;
	}
	else if (argc >= 3 && argc < 11 && std::string(argv[1]) == "-x") { // Extract file mode.
		jdv.Extract_File = true;
		while (argc >= 3) {  // We can extract files from up to 8 embedded images at a time.
			jdv.IMAGE_NAME = argv[2];
			openFiles(argv++, jdv);
			argc--;
		}
	}
	else {
		std::cout << "\nUsage:\t\bjdvrif -i <jpg-image>  <file(s)>\n\t\bjdvrif -x <jpg-image(s)>\n\t\bjdvrif --info\n\n";
		argc = 0;
	}
	if (argc != 0) {
		if (argc == 2) {
			std::cout << "\nComplete! Please check your extracted file(s).\n\n";
		}
		else {
			std::cout << "\nComplete!\n\nYou can now share your \"file-embedded\" JPG image(s) on compatible platforms.\n\n";
		}
	}
	return 0;
}

void openFiles(char* argv[], jdvStruct& jdv) {

	std::ifstream
		readImage(jdv.IMAGE_NAME, std::ios::binary),
		readFile(jdv.FILE_NAME, std::ios::binary);

	if (jdv.Insert_File && (!readImage || !readFile) || !jdv.Extract_File && !readImage) {

		// Open file failure, display relevant error message and exit program.
		const std::string
			READ_ERR_MSG = "\nRead Error: Unable to open/read file: ",
			ERR_MSG = !readImage ? READ_ERR_MSG + "\"" + jdv.IMAGE_NAME + "\"\n\n" : READ_ERR_MSG + "\"" + jdv.FILE_NAME + "\"\n\n";

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}
	
	// Read-in and store JPG image (or "file-embedded" image file) into vector "ImageVec".
	jdv.ImageVec.assign(std::istreambuf_iterator<char>(readImage), std::istreambuf_iterator<char>());

	// Get size of image file (or "file-embedded" image file), update variable.
	jdv.IMAGE_SIZE = jdv.ImageVec.size();

	if (jdv.Extract_File) { // Extract mode.

		const uint8_t
			JPG_HEADER_SIZE = 0x12,
			JDV_SIG_INDEX = 0x2A,		// Standard signature index location within vector "ImageVec"
			JDV_SIG_INDEX_IMGUR = 0x18;	// Shorter signature index location within vector "ImageVec". This is caused by Imgur, which strips part of the JPG header.

		if (jdv.ImageVec[JDV_SIG_INDEX] == 'J' && jdv.ImageVec[JDV_SIG_INDEX + 5] == 'F') {

			findProfileHeaders(jdv);
		}
		else if (jdv.ImageVec[JDV_SIG_INDEX_IMGUR] == 'J' && jdv.ImageVec[JDV_SIG_INDEX_IMGUR + 5] == 'F') {

			// If JPG header has been shortened (Imgur), we need to put back the standard length header.
			const BYTE JPG_HEADER_BLOCK[JPG_HEADER_SIZE] = { 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00 };

			// Insert 18 byte JPG header into vector "jdv.ImageVec". Data alignment restored. We should now be able to extract the embedded data. 
			jdv.ImageVec.insert(jdv.ImageVec.begin() + 2, &JPG_HEADER_BLOCK[0], &JPG_HEADER_BLOCK[JPG_HEADER_SIZE]);

			findProfileHeaders(jdv);
		}

		else {
			std::cerr << "\nImage Error: Image file \"" << jdv.IMAGE_NAME << "\" does not appear to be a jdvrif \"file-embedded\" image.\n\n";
			std::exit(EXIT_FAILURE);
		}
	}

	else {	// Insert mode.

		// Read-in and store user's data file into vector "FileVec".
		jdv.FileVec.assign(std::istreambuf_iterator<char>(readFile), std::istreambuf_iterator<char>());

		jdv.FILE_SIZE = jdv.FileVec.size();

		const std::string
			JPG_SIG = "\xFF\xD8\xFF",	// JPG image signature. 
			GET_JPG_SIG{ jdv.ImageVec.begin(), jdv.ImageVec.begin() + JPG_SIG.length() };	// Get image header from vector. 

		// Before removing header, make sure we are dealing with a valid JPG image file.
		if (GET_JPG_SIG != JPG_SIG) {
			// File requirements check failure, display relevant error message and exit program.
			std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
			std::exit(EXIT_FAILURE);
		}

		const uint32_t MAX_FILE_SIZE = 0xC800000; // 200MB file size limit for this program. 

		if (jdv.IMAGE_SIZE + jdv.FILE_SIZE + (jdv.FILE_SIZE / 65535 * jdv.PROFILE_HEADER_LENGTH + jdv.PROFILE_HEADER_LENGTH) > MAX_FILE_SIZE) {

			// File size check failure, display error message and exit program.
			std::cerr << "\nFile Size Error: Your data file size must not exceed 200MB (209715200 Bytes).\n\n" <<
				"The data file size includes the image file size and the total size of profile headers,\n(18 bytes for every 65KB of the data file).\n\n";

			std::exit(EXIT_FAILURE);
		}

		// The first 434 bytes (0x1B2) of this vector contains the main iCC Profile.
		jdv.ProfileVec.reserve(jdv.FILE_SIZE + 0x1B2 + 0xE290);

		// This vector will be used to store the users encrypted data file.
		jdv.EncryptedVec.reserve(jdv.FILE_SIZE),

		jdv.ProfileVec = {
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
			0x00, 0x00
		};

		const std::string
			EXIF_SIG = "Exif\x00\x00II",
			EXIF_END_SIG = "xpacket end",
			ICC_PROFILE_SIG = "ICC_PROFILE";

		// An embedded JPG thumbnail will cause problems with this program. Search and remove blocks like "Exif" that may contain a JPG thumbnail.

		// Check for an iCC Profile and delete all content before the beginning of the profile. This removes any embedded JPG thumbnail. Profile deleted later.
		const uint32_t ICC_PROFILE_POS = static_cast<uint32_t>(search(jdv.ImageVec.begin(), jdv.ImageVec.end(), ICC_PROFILE_SIG.begin(), ICC_PROFILE_SIG.end()) - jdv.ImageVec.begin());
		if (jdv.ImageVec.size() > ICC_PROFILE_POS) {
			jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + ICC_PROFILE_POS);
		}

		// If no profile found, search for "Exif2 block (look for end signature "xpacket end") and remove the block.
		const uint32_t EXIF_END_POS = static_cast<uint32_t>(search(jdv.ImageVec.begin(), jdv.ImageVec.end(), EXIF_END_SIG.begin(), EXIF_END_SIG.end()) - jdv.ImageVec.begin());
		if (jdv.ImageVec.size() > EXIF_END_POS) {
			jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + (EXIF_END_POS + 17));
		}

		// Remove "Exif" block that has no "xpacket end" signature.
		const uint32_t EXIF_START_POS = static_cast<uint32_t>(search(jdv.ImageVec.begin(), jdv.ImageVec.end(), EXIF_SIG.begin(), EXIF_SIG.end()) - jdv.ImageVec.begin());
		if (jdv.ImageVec.size() > EXIF_START_POS) {
			// get size of "Exif" block
			const uint32_t EXIF_BLOCK_SIZE = jdv.ImageVec[EXIF_START_POS - 2] << 8 | jdv.ImageVec[EXIF_START_POS - 1];
			jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + EXIF_BLOCK_SIZE - 2);
		}

		// ^ Any JPG embedded thumbnail should have now been removed.

		// Signature for Define Quantization Table(s) 
		const auto DQT_SIG = { 0xFF, 0xDB };

		// Find location in vector "ImageVec" of first DQT index location of the image file.
		const uint32_t DQT_POS = static_cast<uint32_t>(search(jdv.ImageVec.begin(), jdv.ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - jdv.ImageVec.begin());

		// Erase the first n bytes of the JPG header before the DQT position. We later replace the erased header with the contents of vector "ProfileVec".
		jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + DQT_POS);

		// Encrypt the user's data file and its filename.
		encryptDecrypt(jdv);
	}
}

void findProfileHeaders(jdvStruct& jdv) {

	std::cout << "\nOK, jdvrif \"file-embedded\" image found!\n";

	const uint8_t
		NAME_LENGTH_INDEX = 0x50,			// Index location within vector "ImageVec" for length value of filename for user's data file.
		NAME_LENGTH = jdv.ImageVec[NAME_LENGTH_INDEX],	// Get embedded value of filename length from "ImageVec", stored within the main profile.
		NAME_INDEX = 0x51,				// Start index location within vector "ImageVec" of filename for user's data file.
		PROFILE_COUNT_INDEX = 0x8A,			// Value index location within vector "ImageVec" for the total number of inserted iCC Profile headers.
		FILE_SIZE_INDEX = 0x90;				// Start index location within vector "ImageVec" for the file size value of the user's data file.

	const uint16_t FILE_INDEX = 0x1B2; 	// Start index location within vector "ImageVec" for the user's data file.

	// From the relevant index location, get size value of user's data file from "ImageVec", stored within the main profile.
	const uint32_t FILE_SIZE = jdv.ImageVec[FILE_SIZE_INDEX] << 24 | jdv.ImageVec[FILE_SIZE_INDEX + 1] << 16 | jdv.ImageVec[FILE_SIZE_INDEX + 2] << 8 | jdv.ImageVec[FILE_SIZE_INDEX + 3];

	// Signature string for the embedded profile headers we need to find within the user's data file.
	const std::string PROFILE_SIG = "ICC_PROFILE";

	// From vector "ImageVec", get the total number of embedded profile headers value, stored within the main profile.
	uint16_t Profile_Count = jdv.ImageVec[PROFILE_COUNT_INDEX] << 8 | jdv.ImageVec[PROFILE_COUNT_INDEX + 1];

	// Get the encrypted filename from vector "ImageVec", stored within the main profile.
	jdv.FILE_NAME = { jdv.ImageVec.begin() + NAME_INDEX, jdv.ImageVec.begin() + NAME_INDEX + jdv.ImageVec[NAME_LENGTH_INDEX] };

	// Erase the 434 byte (0x1B2) main profile from vector "ImageVec", so that the start of "ImageVec" is now the beginning of the user's encrypted data file.
	jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + FILE_INDEX);

	uint32_t Profile_Header_Index = 0; // Variable will store the index locations within vector "ImageVec" of each profile header found.

	if (Profile_Count) { // If one or more profiles.
		
		jdv.ProfileHeaderOffsetVec.reserve(0x200000); // 2MB.

		// Within "ImageVec" find all occurrences of the 18 byte profile headers & store their offset / index location within vector "ProfileHeaderOffsetVec".
		while (Profile_Count--) {
			jdv.ProfileHeaderOffsetVec.emplace_back(Profile_Header_Index = static_cast<uint32_t>(search(jdv.ImageVec.begin() + Profile_Header_Index + 5, jdv.ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - jdv.ImageVec.begin()) - 4);
		}
	}

	// Remove the JPG image from the user's data file. Erase all bytes starting from the end of the "FILE_SIZE" value. 
	// Vector "ImageVec" now contains just the user's encrypted data file.
	jdv.ImageVec.erase(jdv.ImageVec.begin() + FILE_SIZE, jdv.ImageVec.end());

	// This vector will be used to store the user's decrypted data file. 
	jdv.DecryptedVec.reserve(FILE_SIZE);

	std::cout << "\nDecrypting embedded file & removing profile headers...\n";

	// Decrypt the contents of vector "ImageVec".
	encryptDecrypt(jdv);
}

void encryptDecrypt(jdvStruct& jdv) {

	if (jdv.Insert_File) { 

		// Before we encrypt user's data filename, check for and remove "./" or ".\" characters at the start of the filename. 
		const uint32_t Last_Slash_Pos = static_cast<uint32_t>(jdv.FILE_NAME.find_last_of("\\/"));

		if (Last_Slash_Pos <= jdv.FILE_NAME.length()) {
			const std::string_view No_Slash_Name(jdv.FILE_NAME.c_str() + (Last_Slash_Pos + 1), jdv.FILE_NAME.length() - (Last_Slash_Pos + 1));
			jdv.FILE_NAME = No_Slash_Name;
		}
	}

	const std::string XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF";	// String used to XOR encrypt/decrypt the filename of user's data file.

	uint32_t 
		FILE_SIZE = jdv.Insert_File ? static_cast<uint32_t>(jdv.FileVec.size()) : static_cast<uint32_t>(jdv.ImageVec.size()),	 // File size of user's data file.
		Index_Pos = 0;		// When encrypting/decrypting the filename, this variable stores the index character position of the filename,
					// When encrypting/decrypting the user's data file, this variable is used as the index position of where to 
					// insert each byte of the data file into the relevant "encrypted" or "decrypted" vectors.

	const uint8_t 
		MAX_FILENAME_LENGTH = 0x17, 
		NAME_LENGTH = static_cast<uint8_t>(jdv.FILE_NAME.length()), 	// Filename length of user's data file.
		XOR_KEY_LENGTH = static_cast<uint8_t>(XOR_KEY.length());

	uint16_t Offset_Index = 0;	// Index of offset location value within vector "ProfileHeaderOffsetVec".
	
	// Make sure character length of filename does not exceed set maximum.
	if (NAME_LENGTH > MAX_FILENAME_LENGTH) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(NAME_LENGTH) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}

	const std::string In_Name = jdv.FILE_NAME;

	std::string	Out_Name;

	const uint8_t
		Xor_Key_Start_Pos = 0,
		Name_Key_Start_Pos = 0;

	uint8_t
		Xor_Key_Pos = Xor_Key_Start_Pos,	// Character position variable for XOR_KEY string.
		Name_Key_Pos = Name_Key_Start_Pos;	// Character position variable for filename string (outName / inName).

	if (NAME_LENGTH > FILE_SIZE) {			// File size needs to be greater than filename length.
		std::cerr << "\nFile Size Error: File size is too small.\n\n";
		std::exit(EXIT_FAILURE);
	}

	// XOR encrypt/decrypt filename and user's data file.
	while (FILE_SIZE > Index_Pos) {

		if (Index_Pos >= NAME_LENGTH) {
			Name_Key_Pos = Name_Key_Pos > NAME_LENGTH ? Name_Key_Start_Pos : Name_Key_Pos;	 // Reset filename character position to the start if it has reached last character.
		}

		else {
			Xor_Key_Pos = Xor_Key_Pos > XOR_KEY_LENGTH ? Xor_Key_Start_Pos : Xor_Key_Pos;	// Reset XOR_KEY position to the start if it has reached last character.
			Out_Name += In_Name[Index_Pos] ^ XOR_KEY[Xor_Key_Pos++];			// XOR each character of filename against characters of XOR_KEY string. Store output characters in "outName".
																							// Depending on Mode, filename is either encrypted or decrypted.
		}

		if (jdv.Insert_File) {
			// Encrypt data file. XOR each byte of the data file within "jdv.FileVec" against each character of the encrypted filename, "outName". 
			// Store encrypted output in vector "jdv.EncryptedVec".
			jdv.EncryptedVec.emplace_back(jdv.FileVec[Index_Pos++] ^ Out_Name[Name_Key_Pos++]);
		}

		else {
			// Decrypt data file: XOR each byte of the data file within vector "jdv.ImageVec" against each character of the encrypted filename, "inName". 
			// Store decrypted output in vector "jdv.DecryptedVec".
		    jdv.DecryptedVec.emplace_back(jdv.ImageVec[Index_Pos++] ^ In_Name[Name_Key_Pos++]);

			// While decrypting, we need to check for and skip over any occurrence of the iCC Profile headers inserted throughout the encrypted file.
			if (jdv.ProfileHeaderOffsetVec.size() && Index_Pos == jdv.ProfileHeaderOffsetVec[Offset_Index]) {

				// We have found a location for a profile header. Skip over it so that we don't include it within the decrypted output file.
				// Skipping the profile headers is much quicker than removing them with the Vector erase command.
				Index_Pos += jdv.PROFILE_HEADER_LENGTH; // From the current location, skip the 18 byte profile header.
				FILE_SIZE += jdv.PROFILE_HEADER_LENGTH; // Increase FILE_SIZE variable by 18 bytes to keep things in alignment.
				Offset_Index++; 			// Move to the next profile header offset index location value within vector "ProfileHeaderOffsetVec".
			}
		}
	}

	if (jdv.Insert_File) { 

		const uint16_t
			PROFILE_NAME_LENGTH_INDEX = 0x50,	// Location index within the main profile "ProfileVec" to store the filename length value of the user's data file.
			PROFILE_NAME_INDEX = 0x51,		// Location index within the main profile "ProfileVec" to store the filename of the user's data file.
			PROFILE_VEC_SIZE = 0x1B2;		// Byte size of main profile within vector "ProfileVec". User's encrypted data file is stored at the end of the main profile.

		// Update the character length value of the filename for user's data file. Write this value into the main profile of vector "ProfileVec".
		jdv.ProfileVec[PROFILE_NAME_LENGTH_INDEX] = static_cast<int>(NAME_LENGTH);

		// Make space for the filename by removing equivalent length of characters from main profile within vector "ProfileVec".
		jdv.ProfileVec.erase(jdv.ProfileVec.begin() + PROFILE_NAME_INDEX, jdv.ProfileVec.begin() +	NAME_LENGTH + PROFILE_NAME_INDEX);

		// Insert the encrypted filename within the main profile of vector "ProfileVec".
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + PROFILE_NAME_INDEX, Out_Name.begin(), Out_Name.end());

		// Insert contents of vector "EncryptedVec" within vector "ProfileVec", combining then main profile with the user's encrypted data file.	
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + PROFILE_VEC_SIZE, jdv.EncryptedVec.begin(), jdv.EncryptedVec.end());

		jdv.FileVec.clear();

		// Clear vector. This is important if we are embedding more than one file.
		jdv.EncryptedVec.clear();

		// Call function to insert profile headers into the user's data file, so as to split the data into 65KB (or smaller) profile blocks.
		insertProfileHeaders(jdv);
	}
	else { // Extract 

		// Update string variable with the decrypted filename.
		jdv.FILE_NAME = Out_Name;

		// Clear vector. This is important if we are embedding more than one file.
		jdv.ProfileHeaderOffsetVec.clear();
	
		std::cout << "\nOK, embedded file decrypted & profile headers removed.\n";
						
		// Write the extracted (decrypted) content out to file.
		writeOutFile(jdv);
	}
}

void insertProfileHeaders(jdvStruct& jdv) {

	const uint32_t PROFILE_VECTOR_SIZE = static_cast<uint32_t>(jdv.ProfileVec.size());	// Get updated size for vector "ProfileVec" after adding user's data file.

	const uint16_t BLOCK_SIZE = 0xFFFF;	// Profile default block size 65KB (0xFFFF).

	uint32_t Tally_Size = 0x14;		// A value used in conjunction with the user's data file size. We keep incrementing this value by BLOCK_SIZE until
						// we reach near end of the file, which will be a value less than BLOCK_SIZE, the last iCC Profile block.
	
	uint16_t Profile_Count = 0;		// Keep count of how many profile headers that have been inserted into user's data file. We use this value when removing the headers.

	const uint8_t		
		Profile_Header_Start_Index = 0x14,	// Start index location within "ProfileVec" of the profile header (18 bytes).
		Profile_Header_Size_Index = 0x16,	// "ProfileVec" start index location for the 2 byte jpg profile header size field.
		Profile_Size_Index = 0x28,		// "ProfileVec" start index location for the 4 byte main profile size field.
		Profile_Count_Index = 0x8A,		// Start index location within the main profile, where we store the value of the total number of inserted profile headers.
		Profile_Data_Size_Index = 0x90;		// Start index location within the main profile, where we store the file size value of the user's data file.

	uint8_t Bits = 16;	// Variable used with the "updateValue" function. 2 bytes.

	// Get the 18 byte iCC Profile Header from vector "ProfileVec" and store it as a string.
	const std::string ICC_PROFILE_HEADER = { jdv.ProfileVec.begin() + Profile_Header_Start_Index, jdv.ProfileVec.begin() + Profile_Header_Start_Index + jdv.PROFILE_HEADER_LENGTH };

	// Where we see +4 (-4) or +2, these values are the number of bytes at the start of vector "ProfileVec" (4 bytes: 0xFF, 0xD8, 0xFF, 0xE2) 
	// and "ICC_PROFILE_HEADER" (2 bytes: 0xFF, 0xE2), just before the default "BLOCK_SIZE" size bytes: 0xFF, 0xFF, where the block count starts from. 
	// We need to count or subtract these bytes where relevant.

	if (static_cast<uint32_t>(BLOCK_SIZE + jdv.PROFILE_HEADER_LENGTH + 4) >= PROFILE_VECTOR_SIZE) {

		// Looks like we are dealing with a small data file. All data content of "ProfileVec" fits within the first, main 65KB profile block of the image file. 
		// Finish up and write the "embedded" image out to file, exit program.

		// Get the updated size for the 2 byte JPG profile header size.
		// Get the updated size for the 4 byte main profile size. (only 2 bytes used, value is always 16 bytes less than the JPG profile header size). 
		const uint32_t 
			PROFILE_HEADER_BLOCK_SIZE = PROFILE_VECTOR_SIZE - (jdv.PROFILE_HEADER_LENGTH + 4),
			PROFILE_BLOCK_SIZE = PROFILE_HEADER_BLOCK_SIZE - 0x10;
		
		// Insert the updated JPG profile header size for vector "ProfileVec", as it is probably smaller than the set default value (0xFFFF).
		update->Value(jdv.ProfileVec, Profile_Header_Size_Index, PROFILE_HEADER_BLOCK_SIZE, Bits);

		// Insert the updated main profile size for vector "ProfileVec". Size is always 16 bytes less than the JPG profile header size above.
		update->Value(jdv.ProfileVec, Profile_Size_Index, PROFILE_BLOCK_SIZE, Bits);

		jdv.FileVec.swap(jdv.ProfileVec);
	}

	// User's data file is greater than a single 65KB profile block. 
	// Use this section to split up the data content into 65KB profile blocks,
	// by inserting profile headers at the relevant index locations, until the final, 
	// remaining block of data.

	else {
		
		uint32_t Byte_Index = 0;

		Tally_Size += BLOCK_SIZE + 2;
		
		jdv.FileVec.reserve(PROFILE_VECTOR_SIZE + Tally_Size);
		
		while (PROFILE_VECTOR_SIZE > Byte_Index) {
			
			// Store byte at "byteIndex" location of vector "ProfileVec" within vector "FinalVec".	
			jdv.FileVec.emplace_back(jdv.ProfileVec[Byte_Index++]);

			// Does the current "byteIndex" value match the the current "tallySize" value?
			// If match is found, we will insert a profile header into the data at the current location.
			if (Byte_Index == Tally_Size) {

				// Insert a profile header at this location within the file.
				jdv.FileVec.insert(jdv.FileVec.begin() + Tally_Size, ICC_PROFILE_HEADER.begin(), ICC_PROFILE_HEADER.end());

				// Update profile count value after inserting the above profile header. 
				Profile_Count++;

				// Increment tallySize by another BLOCK_SIZE
				Tally_Size += BLOCK_SIZE + 2;

			}
		}
		// Almost all the profile headers have been inserted into the data from the above while-loop.
		// We now have to deal with the last profile header.  Depending on the remaining data size, we may
		// have to insert one last profile header or we just need to update the last profile header size field,
		// to give it the correct size for the last block of data.
		
		// Most files should be delt with in this "if" branch. Other "edge cases" will be delt with in the "else" branch.
		if (Tally_Size > PROFILE_VECTOR_SIZE + (Profile_Count * jdv.PROFILE_HEADER_LENGTH) + 2) {

			// The while loop leaves us with an extra "tallySize += BLOCK_SIZE =2", which is one too many for this section, so we correct it here.
			Tally_Size -= BLOCK_SIZE + 2;
			
			// Update the 2 byte size field of the final profile header (last profile header has already been inserted from the above "while-loop").
			update->Value(jdv.FileVec, Tally_Size + 2, PROFILE_VECTOR_SIZE - Tally_Size + (Profile_Count * jdv.PROFILE_HEADER_LENGTH) - 2, Bits);
		}
		else 
		{  // For this branch we keep the extra "tallySize += BLOCK_SIZE +2", as we need to insert one more profile header into the file.

			// Insert last profile header, required for the data file.
			jdv.FileVec.insert(jdv.FileVec.begin() + Tally_Size, ICC_PROFILE_HEADER.begin(), ICC_PROFILE_HEADER.end());

			// Update the profile count value.
			Profile_Count++;

			// Update the 2 byte size field of the final profile header.
			update->Value(jdv.FileVec, Tally_Size + 2, PROFILE_VECTOR_SIZE - Tally_Size + (Profile_Count * jdv.PROFILE_HEADER_LENGTH) - 2, Bits);
		}

		// Store the total profileCount value into vector "FinalVec", within the main profile. This value is required for when extracting the data file.
		update->Value(jdv.FileVec, Profile_Count_Index, Profile_Count, Bits);
	}

	Bits = 32; // 4 bytes.

	// Store file size value of the user's data file into vector "FinalVec", within the main profile. This value is required for when extracting the data file.
	update->Value(jdv.FileVec, Profile_Data_Size_Index, static_cast<uint32_t>(jdv.FILE_SIZE), Bits);

	// Insert contents of vector "FinalVec" into vector "ImageVec", combining the jpg image with user's data file (now split within 65KB iCC Profile header blocks).	
	jdv.ImageVec.insert(jdv.ImageVec.begin(), jdv.FileVec.begin(), jdv.FileVec.end());

	// Clear vectors. This is important if we are embedding more than one file. (e.g. jdvrif -i image.jpg file1.zip file2.zip file3.zip).
	jdv.ProfileHeaderOffsetVec.clear();
	jdv.ProfileVec.clear();
	jdv.FileVec.clear();

	// If we embed multiple data files (max 8), each outputted image will be differentiated by a number in the filename, 
	// e.g. jdv_img1.jpg, jdv_img2.jpg, jdv_img3.jpg.
	const std::string Diff_Value = std::to_string(jdv.Sub_Value - jdv.Img_Value);	

	jdv.FILE_NAME = "jdv_img" + Diff_Value + ".jpg";

	writeOutFile(jdv);
}

void writeOutFile(jdvStruct& jdv) {

	std::ofstream writeFile(jdv.FILE_NAME, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	if (jdv.Insert_File) {

		// Write out to disk image file embedded with the encrypted data file.
		writeFile.write((char*)&jdv.ImageVec[0], jdv.ImageVec.size());

		std::cout << "\nCreated output file: \"" + jdv.FILE_NAME + " " << jdv.ImageVec.size() << " " << "Bytes\"\n";
		std::string Msg_Size_Warning =
			"\n**Warning**\n\nDue to the file size of your \"file-embedded\" JPG image,\nyou will only be able to share " + jdv.FILE_NAME + " on the following platforms: \n\n"
			"Flickr, ImgPile, ImgBB, ImageShack, PostImage, Imgur & *Reddit (*Desktop only, Reddit mobile app not supported)";

		const uint8_t
			MSG_LEN = static_cast<uint8_t>(Msg_Size_Warning.length());

		const uint32_t
			IMG_SIZE = static_cast<uint32_t>(jdv.ImageVec.size()),
			// Twitter 9.5KB. Not really supported because of the tiny size requirement, but if your data file is this size 
			// (9.5KB, 9800 bytes) or lower, then you should be able to use Twitter to share/tweet the "file-embedded" image.
			MASTODON_MAX_SIZE =	0x1000000,	// 16MB
			IMGUR_REDDIT_MAX_SIZE = 0x1400000,	// 20MB
			POST_IMG_MAX_SIZE =	0x1800000,	// 24MB
			IMG_SHACK_MAX_SIZE =	0x1900000,	// 25MB
			IMG_BB_MAX_SIZE =	0x2000000,	// 32MB
			IMG_PILE_MAX_SIZE =	0x6400000;	// 100MB
			// Flickr is 200MB, this programs max size, no need to to make a variable for it.

		Msg_Size_Warning = (IMG_SIZE > IMGUR_REDDIT_MAX_SIZE && IMG_SIZE <= POST_IMG_MAX_SIZE ? Msg_Size_Warning.substr(0, MSG_LEN - 0x42)
					: (IMG_SIZE > POST_IMG_MAX_SIZE && IMG_SIZE <= IMG_SHACK_MAX_SIZE ? Msg_Size_Warning.substr(0, MSG_LEN - 0x4D)
					: (IMG_SIZE > IMG_SHACK_MAX_SIZE && IMG_SIZE <= IMG_BB_MAX_SIZE ? Msg_Size_Warning.substr(0, MSG_LEN - 0x59)
					: (IMG_SIZE > IMG_BB_MAX_SIZE && IMG_SIZE <= IMG_PILE_MAX_SIZE ? Msg_Size_Warning.substr(0, MSG_LEN - 0x60)
					: (IMG_SIZE > IMG_PILE_MAX_SIZE ? Msg_Size_Warning.substr(0, MSG_LEN - 0x69) : Msg_Size_Warning)))));

		if (IMG_SIZE > MASTODON_MAX_SIZE) {
			std::cerr << Msg_Size_Warning << ".\n";
		}

		jdv.ImageVec.clear();

	}
	else {
		// Write out to disk the extracted (decrypted) data file.
		writeFile.write((char*)&jdv.DecryptedVec[0], jdv.DecryptedVec.size());
		std::cout << "\nExtracted file: \"" + jdv.FILE_NAME + " " << jdv.DecryptedVec.size() << " " << "Bytes\"\n";

		// Clear vectors. This is important when extracting/decrypting more than one image.
		jdv.DecryptedVec.clear();
		jdv.ImageVec.clear();
	}
}

void displayInfo() {

	std::cout << R"(
JPG Data Vehicle (jdvrif v1.2). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

jdvrif enables you to embed & extract arbitrary data of up to *200MB within a single JPG image.

You can upload and share your data-embedded JPG image file on compatible social media & image hosting sites.

*Size limit per JPG image is platform dependant:-

  Flickr (200MB), ImgPile (100MB), ImgBB (32MB), ImageShack (25MB),
  PostImage (24MB), *Reddit (Desktop only) & *Imgur (20MB), Mastodon (16MB).

*Imgur issue: Data is still retained when the file-embedded JPG image is over 5MB, but Imgur reduces the dimension size of the image.
 
*Reddit issue: Desktop only. Does not work with Reddit mobile app. Mobile app converts images to Webp format.

*Twitter: If your data file is only 9KB or lower, you can also use Twitter to share your "file-embedded" JPG image.
To share larger files on Twitter, (up to 5MB), please use pdvzip (PNG only).

This program works on Linux and Windows.

The file data is encrypted and inserted within multiple 65KB ICC Profile blocks in the image file.
 
Using jdvrif, you can insert up to six files at a time (outputs one image per file).

You can also extract files from up to six images at a time.

)";
}
