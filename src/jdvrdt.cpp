//	JPG Data Vehicle for Reddit, (JDVRDT v1.2). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

typedef unsigned char BYTE;

struct jdvStruct {
	std::vector<BYTE> ImageVec, FileVec, ProfileVec, ProfileBlockVec, EmbdImageVec, EncryptedVec, DecryptedVec;
	std::string IMAGE_NAME, FILE_NAME, MODE;
	size_t IMAGE_SIZE{},FILE_SIZE{};
	int imgVal{}, subVal{};
};

// Open user image & data file or Embedded image file. Display error & exit program if any file fails to open.
void openFiles(char* [], jdvStruct &jdv);

// Check file size requirements of user's data and image file. Display error & exit program if any file (or their combined size) exceeds size limits.
void checkFileSize(std::ifstream&, std::ifstream&, jdvStruct &jdv);

// Make sure we are dealing with a valid jdv embedded image file.
void checkEmbeddedImage(std::ifstream&, jdvStruct &jdv);

// Function finds and removes all the inserted ICC profile blocks.
void removeProfileHeaders(jdvStruct& jdv);

// Setup relevant vectors. Read-in and store files into vectors.
void configureVectors(std::ifstream&, std::ifstream&, jdvStruct& jdv);

// Make sure the image file is a valid JPG image, then remove its default file header.
void removeImageHeader(jdvStruct &jdv);

// Encrypt or decrypt user's data file and its filename.
void encryptDecrypt(jdvStruct& jdv);

// Function splits user's data file up into 65KB (or smaller) ICC profile blocks.
void insertProfileBlocks(jdvStruct &jdv);

// Write out to file the embedded image file or the extracted data file.
void writeOutFile(jdvStruct& jdv);

// Update values, such as block size, file sizes and other values, and write them into the relevant vector index locations. Overwrites previous values.
void updateValue(std::vector<BYTE>&, int, const size_t, int);

// Display program infomation
void displayInfo();

int main(int argc, char** argv) {

	jdvStruct jdv;
	
	if (argc == 2 && std::string(argv[1]) == "--info") {
		argc = 0;
		displayInfo();
	}
	else if (argc >= 4 && argc < 9 && std::string(argv[1]) == "-i") { // Insert file mode.
		jdv.MODE = argv[1], jdv.subVal = argc - 1, jdv.IMAGE_NAME = argv[2];
		argc -= 2;
		while (argc != 1) {  // We can insert upto five files at a time (outputs one image for each file).
			jdv.imgVal = argc, jdv.FILE_NAME = argv[3];
			openFiles(argv++, jdv);
			argc--;
		}
		argc = 1;
	}
	else if (argc >= 3 && argc < 8 && std::string(argv[1]) == "-x") { // Extract file mode.
		jdv.MODE = argv[1];
		while (argc >= 3) {  // We can extract files from upto five embedded images at a time.
			jdv.IMAGE_NAME = argv[2];
			openFiles(argv++, jdv);
			argc--;
		}
	}
	else {
		std::cerr << "\nUsage:\t\bjdvrdt -i <jpg-image>  <file(s)>\n\t\bjdvrdt -x <jpg-image(s)>\n\t\bjdvrdt --info\n\n";
		argc = 0;
	}
	if (argc != 0) {
		if (argc == 2) {
			std::cout << "\nComplete! Please check your extracted file(s).\n\n";
		}
		else {
			std::cout << "\nComplete!\n\nYou can now post your \"file-embedded\" JPG image(s) on reddit.\n\n";
		}
	}
	return 0;
}

void openFiles(char* argv[], jdvStruct &jdv) {

	const std::string READ_ERR_MSG = "\nRead Error: Unable to open/read file: ";

	std::ifstream
		readImage(jdv.IMAGE_NAME, std::ios::binary),
		readFile(jdv.FILE_NAME, std::ios::binary);

	if (jdv.MODE == "-i" && (!readImage || !readFile) || jdv.MODE == "-x" && !readImage) {

		// Open file failure, display relevant error message and exit program.
		const std::string ERR_MSG = !readImage ? READ_ERR_MSG + "\"" + jdv.IMAGE_NAME + "\"\n\n" : READ_ERR_MSG + "\"" + jdv.FILE_NAME + "\"\n\n";

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	// Get size of files.
	readImage.seekg(0, readImage.end),
	readFile.seekg(0, readFile.end);

	// Update file size variables.
	jdv.IMAGE_SIZE = readImage.tellg();
	jdv.FILE_SIZE = readFile.tellg();

	// Reset read position of files. 
	readImage.seekg(0, readImage.beg),
	readFile.seekg(0, readFile.beg);

	if (jdv.MODE == "-i") { // Insert mode. Check file size requirements of JPG image and user's data file.
		checkFileSize(readImage, readFile, jdv);
	}
	else { // Extract mode. Make sure embedded image file is valid format.
		checkEmbeddedImage(readImage, jdv);
	}
}

void checkFileSize(std::ifstream& readImage, std::ifstream& readFile, jdvStruct &jdv) {
	
	const int
		MAX_JPG_SIZE_BYTES = 20971520,		// 20MB reddit & imgur JPG image size limit.
		MAX_DATAFILE_SIZE_BYTES = 20971520;	// 20MB data file size limit.

	if ((jdv.IMAGE_SIZE + jdv.FILE_SIZE) > MAX_JPG_SIZE_BYTES
		|| jdv.FILE_SIZE > MAX_DATAFILE_SIZE_BYTES) {

		// File size check failure, display relevant error message and exit program.
		const std::string
			SIZE_ERR_IMAGE = "\nImage Size Error: JPG image (+including embedded file size) must not exceed 20MB.\n\n",
			SIZE_ERR_FILE = "\nFile Size Error: Your data file must not exceed 20MB.\n\n",

			ERR_MSG = jdv.IMAGE_SIZE + MAX_DATAFILE_SIZE_BYTES > MAX_JPG_SIZE_BYTES ? SIZE_ERR_IMAGE : SIZE_ERR_FILE;

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	// File size check success. Setup relevant vectors. Read-in and store files into vectors. 
	configureVectors(readImage, readFile, jdv);
}

void checkEmbeddedImage(std::ifstream& readImage, jdvStruct &jdv) {

	// Read-in and store embedded image file into vector "EmbdImageVec".
	jdv.EmbdImageVec.resize(jdv.IMAGE_SIZE / sizeof(unsigned char));
	readImage.read((char*)&jdv.EmbdImageVec[0], jdv.IMAGE_SIZE);

	const int JDV_SIG_INDEX = 25;	// Signature index location within vector "EmbdImageVec". 
	
	const std::string
		JDV_SIG = "JDVRdT",	// Signature string for our file embedded images.
					// Get signature from vector "EmbdImageVec".
		JDV_CHECK{ jdv.EmbdImageVec.begin() + JDV_SIG_INDEX, jdv.EmbdImageVec.begin() + JDV_SIG_INDEX + JDV_SIG.length() };	
	
	if (JDV_CHECK != JDV_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: Image file \"" << jdv.IMAGE_NAME << "\" does not appear to be a JDVRdT file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}
	removeProfileHeaders(jdv);
}

void removeProfileHeaders(jdvStruct &jdv) {
	
	const int
		PROFILE_HEADER_LENGTH = 18,	// Byte length value of the embedded profile header (see "ProfileBlockVec"). 
		NAME_LENGTH_INDEX = 32,		// Index location for length value of filename for user's data file.
		NAME_INDEX = 33,		// Start index location of filename for user's data file.
		PROFILE_COUNT_INDEX = 72,	// Value index location for the total number of inserted ICC profile headers (see "ProfileBlockVec").
		FILE_SIZE_INDEX = 88,		// Start index location for the file size value of the user's data file.
		FILE_INDEX = 152,  		// Start index location of user's data file within vector "EmbdImageVec".
		ENCRYPTED_NAME_LENGTH = jdv.EmbdImageVec[NAME_LENGTH_INDEX], // Get embedded filename length value from "EmbdImageVec", stored within the main profile.
		// From the relevant index location, get size value of user's data file from "EmbdImageVec", stored within the mail profile.
		FILE_SIZE = jdv.EmbdImageVec[FILE_SIZE_INDEX] << 24 | jdv.EmbdImageVec[FILE_SIZE_INDEX + 1] << 16 | jdv.EmbdImageVec[FILE_SIZE_INDEX + 2] << 8 | jdv.EmbdImageVec[FILE_SIZE_INDEX + 3];

	const std::string PROFILE_SIG = "ICC_PROFILE";	// Signature string for the embedded ICC profile headers we need to find & remove from the user's data file.
		
	// From vector "EmbdImageVec", get the value of the total number of embedded profile headers, stored within the mail profile.
	int profileCount = jdv.EmbdImageVec[PROFILE_COUNT_INDEX] << 8 | jdv.EmbdImageVec[PROFILE_COUNT_INDEX + 1];

	// Get the encrypted filename from vector "EmbdImageVec", stored within the mail profile.
	jdv.FILE_NAME = { jdv.EmbdImageVec.begin() + NAME_INDEX, jdv.EmbdImageVec.begin() + NAME_INDEX + jdv.EmbdImageVec[NAME_LENGTH_INDEX] };

	// Erase the 152 byte main profile from vector "EmbdImageVec", so that the start of "EmbdImageVec" is now the beginning of the user's encrypted data file.
	jdv.EmbdImageVec.erase(jdv.EmbdImageVec.begin(), jdv.EmbdImageVec.begin() + FILE_INDEX);

	ptrdiff_t headerIndex = 0; // Variable will store the index location within "EmbdImageVec" of each ICC profile header we find within the vector.

	// Within "EmbdImageVec" find and erase all occurrences of the 18 byte ICC profile header, (see "ProfileBlockVec").
	while (profileCount--) {
		headerIndex = search(jdv.EmbdImageVec.begin() + headerIndex, jdv.EmbdImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - jdv.EmbdImageVec.begin() - 4;
		jdv.EmbdImageVec.erase(jdv.EmbdImageVec.begin() + headerIndex, jdv.EmbdImageVec.begin() + headerIndex + PROFILE_HEADER_LENGTH);
	}

	// Remove the JPG image from the user's data file. 
	// Erase all bytes starting from the end of "FILE_SIZE" value. Vector "EmbdImageVec" now contains just the user's encrypted data file.
	jdv.EmbdImageVec.erase(jdv.EmbdImageVec.begin() + FILE_SIZE, jdv.EmbdImageVec.end());

	// This vector will be used to store the decrypted user's data file. 
	jdv.DecryptedVec.reserve(FILE_SIZE);
	
	// The encrypted data file is now stored in the vector "FileVec".
	jdv.EmbdImageVec.swap(jdv.FileVec);

	// Decrypt the contents of "FileVec".
	encryptDecrypt(jdv);
}

void configureVectors(std::ifstream& readImage, std::ifstream& readFile, jdvStruct &jdv) {
	
	// The first 152 bytes of this vector contains the main (basic) ICC profile.
	// We also store in the main profile: 
	// 1.Length value of filename (user's data file).
	// 2.Encrypted filename of user's data file.
	// 3.File size value of user's data file.
	// 4.Value for the total number of ICC profile headers that are inserted when we split the data file into 65KB (or smaller) blocks. See "ProfileBlockVec".
	// 5.The encrypted data file (inserted at the end of profile).
	jdv.ProfileVec = {
		0xFF, 0xD8, 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46,
		0x49, 0x4C, 0x45, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x84, 0x20, 0x4A, 0x44, 0x56,
		0x52, 0x64, 0x54, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x61, 0x63, 0x73, 0x70, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	// When we split the data file into 65KB (or smaller) blocks, this is the ICC profile header we use for the start of each block.
	jdv.ProfileBlockVec = {
			0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C,
			0x45, 0x00, 0x01, 0x01
	};
	
	// Read-in and store JPG image file into vector "ImageVec".
	jdv.ImageVec.resize(jdv.IMAGE_SIZE / sizeof(unsigned char));
	readImage.read((char*)&jdv.ImageVec[0], jdv.IMAGE_SIZE);

	// Read-in and store user's data file into vector "FileVec".
	jdv.FileVec.resize(jdv.FILE_SIZE / sizeof(unsigned char));
	readFile.read((char*)&jdv.FileVec[0], jdv.FILE_SIZE);

	// This vector will be used to store the users encrypted data file.
	jdv.EncryptedVec.reserve(jdv.FILE_SIZE);
	
	// Remove the default header from the JPG image. It will be replaced with the header in "ProfileVec".
	removeImageHeader(jdv);
}

void removeImageHeader(jdvStruct &jdv) {

	const std::string
		JPG_SIG = "\xFF\xD8\xFF",	// JPG image signature. 
		JPG_CHECK{ jdv.ImageVec.begin(), jdv.ImageVec.begin() + JPG_SIG.length() };	// Get image header from vector. 

	// Before removing header, make sure we are dealing with a valid JPG image file.
	if (JPG_CHECK != JPG_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	// Signature for Define Quantization Table(s) 
	const auto DQT_SIG = { 0xFF, 0xDB };

	// Find location in vector "ImageVec" of first DQT index location of the image file.
	const ptrdiff_t DQT_POS = search(jdv.ImageVec.begin(), jdv.ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - jdv.ImageVec.begin();

	// Erase the first n bytes of the JPG header before this DQT position. We later replace the erased header with the contents of vector "ProfileVec".
	jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + DQT_POS);

	// Encrypt the user's data file and its filename.
	encryptDecrypt(jdv);
}

void encryptDecrypt(jdvStruct& jdv) {

	const std::string XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF";	// String used to xor encrypt/decrypt the filename of user's data file.

	if (jdv.MODE == "-i") { // Insert mode.

		// Before we encrypt user's data filename, check for and remove "./" or ".\" characters at the start of the filename. 
		const size_t FIRST_SLASH_POS = jdv.FILE_NAME.find_first_of("\\/");

		// Update the string variable with the modified filename.
		jdv.FILE_NAME = jdv.FILE_NAME.substr(FIRST_SLASH_POS + 1, jdv.FILE_NAME.length());
	}

	const int MAX_LENGTH_FILENAME = 23;

	const size_t
		NAME_LENGTH = jdv.FILE_NAME.length(),	// Filename length of user's data file.
		XOR_KEY_LENGTH = XOR_KEY.length(),
		FILE_SIZE = jdv.FileVec.size();		// File size of user's data file.

	// Make sure character length of filename does not exceed set maximum.
	if (NAME_LENGTH > MAX_LENGTH_FILENAME) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(NAME_LENGTH) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::string
		inName = jdv.FILE_NAME,
		outName;

	int
		xorKeyPos = 0,		// Character position variable for XOR_KEY string.
		nameKeyPos = 0,		// Character position variable for filename string (outName / inName).
		indexPos = 0,   	// When encrypting/decrypting the filename, this variable stores the index read position of the filename,
					// When encrypting/decrypting the user's data file, this variable is used as the index position of where to 
					// insert each byte of the data file into the relevant "encrypted" or "decrypted" vectors.
		bits = 8;		// Value used in "updateValue" function.

	// xor encrypt or decrypt filename and data file.
	while (FILE_SIZE > indexPos) {

		if (indexPos >= NAME_LENGTH) {
			nameKeyPos = nameKeyPos > NAME_LENGTH ? 0 : nameKeyPos;	 // Reset filename character position to the start if it has reached last character.
		}
		else {
			xorKeyPos = xorKeyPos > XOR_KEY_LENGTH ? 0 : xorKeyPos;	// Reset XOR_KEY position to the start if it has reached last character.
			outName += inName[indexPos] ^ XOR_KEY[xorKeyPos++];	// xor each character of filename against characters of XOR_KEY string. Store output characters in "outName".
										// Depending on Mode, filename is either encrypted or decrypted.
		}

		if (jdv.MODE == "-i") {
			// Encrypt data file. xor each byte of the data file within "jdv.FileVec" against each character of the encrypted filename, "outName". 
			// Store encrypted output in vector "jdv.EncryptedVec".
			jdv.EncryptedVec.emplace_back(jdv.FileVec[indexPos++] ^ outName[nameKeyPos++]);
		}
		else {
			// Decrypt data file: xor each byte of the data file within vector "jdv.FileVec" against each character of the encrypted filename, "inName". 
			// Store decrypted output in vector "jdv.DecryptedVec".
			jdv.DecryptedVec.emplace_back(jdv.FileVec[indexPos++] ^ inName[nameKeyPos++]);
		}
	}

	if (jdv.MODE == "-i") { // Insert mode.

		const int
			PROFILE_NAME_LENGTH_INDEX = 32, // Location index inside the main profile "ProfileVec" to store the filename length value of the user's data file.
			PROFILE_NAME_INDEX = 33,	// Location index inside the main profile "ProfileVec" to store the filename of the user's data file.
			PROFILE_VEC_SIZE = 152;		// Byte size of main profile within "ProfileVec". User's encrypted data file is stored at the end of the main profile.

		// Update the character length value of the filename for user's data file. Write value into the main profile of vector "ProfileVec".
		updateValue(jdv.ProfileVec, PROFILE_NAME_LENGTH_INDEX, NAME_LENGTH, bits);

		// Make space for the filename by removing equivalent length of characters from main profile within vector "ProfileVec".
		jdv.ProfileVec.erase(jdv.ProfileVec.begin() + PROFILE_NAME_INDEX, jdv.ProfileVec.begin() + outName.length() + PROFILE_NAME_INDEX);

		// Insert the encrypted filename into the main profile of vector "ProfileVec".
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + PROFILE_NAME_INDEX, outName.begin(), outName.end());

		// Insert contents of vector "EncryptedVec" into vector "ProfileVec", combining then main profile with the user's encrypted data file.	
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + PROFILE_VEC_SIZE, jdv.EncryptedVec.begin(), jdv.EncryptedVec.end());

		// Call function to insert ICC profile headers into the users data file, so as to split it into 65KB (or smaller) profile blocks.
		insertProfileBlocks(jdv);
	}
	else { // Extract Mode.
		// Update string variable with the decrypted filename.
		jdv.FILE_NAME = outName;

		// Write the extracted (decrypted) data file out to disk.
		writeOutFile(jdv);
	}
}

void insertProfileBlocks(jdvStruct &jdv) {

	const size_t
		VECTOR_SIZE = jdv.ProfileVec.size(),	// Get updated size for vector "ProfileVec" after adding user's data file.
		BLOCK_SIZE = 65535;			// ICC profile default block size (0xFFFF).

	int
		bits = 16,			// Variable used with the "updateValue" function.
		tallySize = 2,			// Keep count of how much data we have traversed while inserting "ProfileBlockVec" headers at every "BLOCK_SIZE" within "ProfileVec". 
		profileCount = 0,		// Keep count of how many ICC profile blocks ("ProfileBlockVec") we insert into the user's data file.
		profileMainBlockSizeIndex = 4,  // "ProfileVec" start index location for the 2 byte block size field.
		profileBlockSizeIndex = 2,	// "ProfileBlockVec" start index location for the 2 byte block size field.
		profileCountIndex = 72,		// Start index location in main profile, where we store the value of the total number of inserted ICC profile headers.
		profileDataSizeIndex = 88;	// Start index location in main profile, where we store the file size value of the user's data file.

	// Where we see +4 (-4) or +2, these values are the number of bytes at the start of vector "ProfileVec" (4 bytes: 0xFF, 0xD8, 0xFF, 0xE2) 
	// and "ProfileBlockVec" (2 bytes: 0xFF, 0xE2), just before the default "BLOCK_SIZE" bytes: 0xFF, 0xFF, where the block count starts from. 
	// We need to count or subtract these bytes where relevant.

	if (BLOCK_SIZE + 4 >= VECTOR_SIZE) {

		// Seems we are dealing with a small data file. All data content fits within the main 65KB profile block. 
		// Finish up, skip the "While loop" and write the "embedded" image out to file, exit program.

		// Update profile block size of vector "ProfileVec", as it is smaller than the set default value (0xFFFF).
		updateValue(jdv.ProfileVec, profileMainBlockSizeIndex, VECTOR_SIZE - 4, bits);

		// Even though no more profile header blocks are required, so as to prevent the app GIMP from displaying an error message regarding an invalid ICC profile, 
		// we will insert a single "ProfileBlockVec" with a minimum size of 16 bytes at the end of the data file, just before start of JPG image.
		updateValue(jdv.ProfileBlockVec, profileBlockSizeIndex, 16, bits);
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + VECTOR_SIZE, jdv.ProfileBlockVec.begin(), jdv.ProfileBlockVec.end()); // Insert final "ProfileBlockVec" header.

		profileCount = 1; // One will always be the minimum number of ICC profile headers. 

		// Insert final profileCount value into vector "Profile". // 1
		updateValue(jdv.ProfileVec, profileCountIndex, profileCount, bits);

		bits = 32;

		// Insert file size value of user's data file into vector "ProfileVec".
		updateValue(jdv.ProfileVec, profileDataSizeIndex, jdv.FILE_SIZE, bits);
	}

	// Insert "ProfileBlockVec" vector header contents into the data file at every "BLOCK_SIZE", or whatever data size remains, that is below "BLOCK_SIZE", until end of file.
	if (VECTOR_SIZE > BLOCK_SIZE + 4) {

		bool isMoreData = true;

		while (isMoreData) {

			tallySize += BLOCK_SIZE + 2;

			if (BLOCK_SIZE + 2 >= jdv.ProfileVec.size() - tallySize + 2) {

				// Data file size remaining is less than the "BLOCK_SIZE" default value (0xFFFF), 
				// so we are near end of file. Update last profile block size & insert last "ProfileBlock" vector header content, then exit the loop.
				updateValue(jdv.ProfileBlockVec, profileBlockSizeIndex, (jdv.ProfileVec.size() + jdv.ProfileBlockVec.size()) - (tallySize + 2), bits);

				profileCount++;

				// Update final profileCount value into vector "ProfileVec".
				updateValue(jdv.ProfileVec, profileCountIndex, profileCount, bits);

				bits = 32;

				// Insert file size value of user's data file into vector "ProfileVec".
				updateValue(jdv.ProfileVec, profileDataSizeIndex, jdv.FILE_SIZE, bits);

				// Insert final "ProfileBlockVec" header contents.
				jdv.ProfileVec.insert(jdv.ProfileVec.begin() + tallySize, jdv.ProfileBlockVec.begin(), jdv.ProfileBlockVec.end()); 

				isMoreData = false; // No more data, exit loop.
			}

			else {  // Keep going, we have not yet reached end of file (last block size).
				profileCount++;
				// Insert another ICC profile header ("ProfileBlockVec").
				jdv.ProfileVec.insert(jdv.ProfileVec.begin() + tallySize, jdv.ProfileBlockVec.begin(), jdv.ProfileBlockVec.end());
			}
		}
	}

	// Insert contents of vector "ProfileVec" into vector "ImageVec", combining user's data file and ICC profile header blocks, with the JPG image.	
	jdv.ImageVec.insert(jdv.ImageVec.begin(), jdv.ProfileVec.begin(), jdv.ProfileVec.end());

	std::string diffVal = std::to_string(jdv.subVal - jdv.imgVal);	// If we embed multiple data files (max 5), each outputted image will be differentiated by a number in the name, 
									// jdv_img1.jpg, jdv_img2.jpg, jdv_img3.jpg, etc.
	jdv.FILE_NAME = "jdv_img" + diffVal + ".jpg";					

	writeOutFile(jdv);
}

void writeOutFile(jdvStruct &jdv) {

	if (jdv.FILE_NAME.substr(0, 4) != "jdv_") {
		jdv.FILE_NAME = "jdv_" + jdv.FILE_NAME;
	}

	std::ofstream writeFile(jdv.FILE_NAME, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	if (jdv.MODE == "-i") {
		// Write out to disk image file embedded with the encrypted data file.
		writeFile.write((char*)&jdv.ImageVec[0], jdv.ImageVec.size());
		std::cout << "\nCreated output file: \"" + jdv.FILE_NAME + " " << jdv.ImageVec.size() << " " << "Bytes\"\n";
		jdv.EncryptedVec.clear();
	}
	else {
		// Write out to disk the extracted (decrypted) data file.
		writeFile.write((char*)&jdv.DecryptedVec[0], jdv.DecryptedVec.size());
		std::cout << "\nExtracted file: \"" + jdv.FILE_NAME + " " << jdv.DecryptedVec.size() << " " << "Bytes\"\n";
		jdv.DecryptedVec.clear();
	}
}

void updateValue(std::vector<unsigned char>& vect, int valueInsertIndex, const size_t VALUE, int bits) {

	while (bits) vect[valueInsertIndex++] = (VALUE >> (bits -= 8)) & 0xff;
}

void displayInfo() {

	std::cout << R"(
JPG Data Vehicle for Reddit, (jdvrdt v1.2). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

jdvrdt enables you to embed & extract arbitrary data of upto ~20MB within a single JPG image.

You can upload and share your data embedded JPG image file on Reddit or *Imgur.

*Imgur issue: Data is still retained when the file-embedded JPG image is over 5MB, but Imgur reduces the dimension size of the image.
 
jdvrdt data embedded images will not work with Twitter. For Twitter, please use pdvzip (PNG only).

This program works on Linux and Windows.

The file data is inserted and preserved within multiple 65KB ICC Profile blocks in the image file.
 
To maximise the amount of data you can embed in your image file. I recommend compressing your 
data file(s) to zip/rar formats, etc.

Using jdvrdt, You can insert up to five files at a time (outputs one image per file).

You can also extract files from up to five images at a time.

)";
}
