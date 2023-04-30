//	JPG Data Vehicle for Reddit, (JDVRDT v1.0). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Open user image & data file and check file size requirements. Display error & exit program if any file fails to open or exceeds size limits.
void processFiles(char* [], int, int, const std::string&);

// Open jdvrdt jpg image file, then proceed to extract embedded data file. 
void processEmbeddedImage(char* []);

// Read in and store jpg image & data file into vectors..
void readFilesIntoVectors(std::ifstream&, std::ifstream&, const std::string&, const std::string&, const ptrdiff_t&, const ptrdiff_t&, int, int);

// Insert updated values, such as block size and other values into relevant vector index locations.
void insertValue(std::vector<unsigned char>&, ptrdiff_t, const size_t&, int);

// Display program infomation
void displayInfo();

const std::string READ_ERR_MSG = "\nRead Error: Unable to open/read file: ";

int main(int argc, char** argv) {

	if (argc == 2 && std::string(argv[1]) == "--info") {
		argc = 0;
		displayInfo();
	}
	else if (argc >= 4 && argc < 9 && std::string(argv[1]) == "-i") {
		int sub = argc - 1;
		argc -= 2;
		const std::string IMAGE_FILE = argv[2];
		
		while (argc != 1) {
			processFiles(argv++, argc, sub, IMAGE_FILE);
			argc--;
		}
		
		argc = 1;
	}
	else if (argc >= 3 && argc < 8 && std::string(argv[1]) == "-x") {
		
		while (argc >= 3) {
			processEmbeddedImage(argv++);
			argc--;
		}
	}
	else {
		std::cerr << "\nUsage:\t\bjdvrdt -i <jpg-image>  <file(s)>\n\t\bjdvrdt -x <jpg-image(s)>\n\t\bjdvrdt --info\n\n";
		argc = 0;
	}
	if (argc != 0) {
		if (argc == 2) {
			std::cout << "\nComplete!\n\n";
		}
		else {
			std::cout << "\nComplete!\n\nYou can now post your file-embedded jpg image(s) on reddit.\n\n";
		}
	}
	return 0;
}

void processFiles(char* argv[], int argc, int sub, const std::string& IMAGE_FILE) {

	const std::string DATA_FILE = argv[3];

	std::ifstream
		readImage(IMAGE_FILE, std::ios::binary),
		readFile(DATA_FILE, std::ios::binary);

	if (!readImage || !readFile) {

		// Open file failure, display relevant error message and exit program.
		const std::string ERR_MSG = !readImage ? READ_ERR_MSG + "\"" + IMAGE_FILE + "\"\n\n" : READ_ERR_MSG + "\"" + DATA_FILE + "\"\n\n";

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	// Open file success, now check file size requirements.
	const int
		MAX_JPG_SIZE_BYTES = 20971520,		// 20MB reddit/imgur jpg image size limit.
		MAX_DATAFILE_SIZE_BYTES = 20971520;	// 20MB data file size limit for reddit / imgur.

	// Get size of files.
	readImage.seekg(0, readImage.end),
	readFile.seekg(0, readFile.end);

	const ptrdiff_t
		IMAGE_SIZE = readImage.tellg(),
		DATA_SIZE = readFile.tellg();

	if ((IMAGE_SIZE + DATA_SIZE) > MAX_JPG_SIZE_BYTES
		|| DATA_SIZE > MAX_DATAFILE_SIZE_BYTES) {

		// File size check failure, display relevant error message and exit program.
		const std::string
			SIZE_ERR_JPG = "\nImage Size Error: JPG image (+including embedded file size) must not exceed 20MB.\n\n",
			SIZE_ERR_DATA = "\nFile Size Error: Your data file must not exceed 20MB.\n\n",

			ERR_MSG = IMAGE_SIZE + MAX_DATAFILE_SIZE_BYTES > MAX_JPG_SIZE_BYTES ? SIZE_ERR_JPG : SIZE_ERR_DATA;

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	// File size check success, now read-in and store files into vectors.
	readFilesIntoVectors(readImage, readFile, IMAGE_FILE, DATA_FILE, IMAGE_SIZE, DATA_SIZE, argc, sub);
}

// Extract embedded data file from jpg image.
void processEmbeddedImage(char* argv[]) {

	const std::string IMAGE_FILE = argv[2];

	if (IMAGE_FILE == "." || IMAGE_FILE == "/") std::exit(EXIT_FAILURE);

	std::ifstream readImage(IMAGE_FILE, std::ios::binary);

	if (!readImage) {
		std::cerr << "\nRead Error: Unable to open/read file: \"" + IMAGE_FILE + "\"\n\n";
		std::exit(EXIT_FAILURE);
	}

	// Read "file-embedded" JPG image into vector "ImageVec".
	std::vector<unsigned char> ImageVec((std::istreambuf_iterator<char>(readImage)), std::istreambuf_iterator<char>());

	const int			// Most values here relate to the contents of the vector "ImageVec".
		XOR_KEY_START_POS = 0,  // Default start index location for xor key.
		XOR_KEY_LENGTH = 5,	// Character length value (length starts from 0) of the key string used to xor decrypt the embedded filename.
		PROFILE_SIG_INDEX = 6,	// ICC Profile signature start index location.
		PROFILE_LENGTH = 18,	// Length value of the inserted ICC "ProfileBlockVec". We need to remove all of these from the data file.
		JDV_SIG_INDEX = 25,	// This programs signature start index location. 
		NAME_LENGTH_INDEX = 32,	// Index location for length value of filename for user's data file.
		NAME_INDEX = 33,	// Start index location of filename for user's data file.
		ICC_COUNT_INDEX = 72,	// Value index location for the total number of inserted ICC Profile blocks ("ProfileBlockVec").
		DATA_SIZE_INDEX = 88,	// Start index location for the size value of the user's data file.
		DATA_INDEX = 152,  	// Start index location of user's data file within the main ICC Profile.
		
		// Get embedded filename length value from "ImageVec" stored within the main Profile.
		ENCRYPTED_NAME_LENGTH = ImageVec[NAME_LENGTH_INDEX], 
		// Get data file size stored in the main Profile.
		PROFILE_DATA_SIZE = ImageVec[DATA_SIZE_INDEX] << 24 | ImageVec[DATA_SIZE_INDEX + 1] << 16 | ImageVec[DATA_SIZE_INDEX + 2] << 8 | ImageVec[DATA_SIZE_INDEX + 3]; 

	int 
		profileCount = ImageVec[ICC_COUNT_INDEX] << 8 | ImageVec[ICC_COUNT_INDEX + 1],  // Get ICC Profile insert count value from "ImageVec" stored in main Profile.
		firstKeyPos = XOR_KEY_START_POS,  // 0, First key position variable used for xor decrypting filename of user's data file.
		secondKeyPos = XOR_KEY_START_POS; // 0, Second key position variable used for xor decrypting user's data file.

	const std::string
		PROFILE_SIG = "ICC_PROFILE",		// Signature string for JPG Profile.
		JDV_SIG = "JDVRdT",			// Signature string for this program.
		XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF",	// String used to xor decrypt embedded filename.
		PROFILE_CHECK{ ImageVec.begin() + PROFILE_SIG_INDEX, ImageVec.begin() + PROFILE_SIG_INDEX + PROFILE_SIG.length() }, // Get ICC signature from vector "ImageVec".
		JDV_CHECK{ ImageVec.begin() + JDV_SIG_INDEX, ImageVec.begin() + JDV_SIG_INDEX + JDV_SIG.length() },			// Get JDV signature from vector "ImageVec".
		ENCRYPTED_NAME = { ImageVec.begin() + NAME_INDEX, ImageVec.begin() + NAME_INDEX + ImageVec[NAME_LENGTH_INDEX] };	// Get encrypted filename stored in vector "ImageVec".

	if (PROFILE_CHECK != PROFILE_SIG || JDV_CHECK != JDV_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: Image file \"" << IMAGE_FILE << "\" does not appear to be a JDVRdT file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	// From "ImageVec" vector index 0, erase bytes so that start of vector is now the beginning of the user's data file.
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + DATA_INDEX);

	if (profileCount) { // Apart from the first main Profile (0), are there anymore Profile inserts within the data file? If so, we need to find & remove them all.

		// Within "ImageVec" find and erase all occurrences of the contents of "ProfileBlockVec".
		ptrdiff_t findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;

		// Stop the search once profileCount is 0.
		while (profileCount--) {
			ImageVec.erase(ImageVec.begin() + findProfileSigIndex, ImageVec.begin() + findProfileSigIndex + PROFILE_LENGTH);
			findProfileSigIndex = search(ImageVec.begin() + findProfileSigIndex, ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;
		}
	}

	// Remove JPG image from data file. Erase all bytes starting from end of "PROFILE_DATA_SIZE" value. Vector now contains just the user's data file.
	ImageVec.erase(ImageVec.begin() + PROFILE_DATA_SIZE, ImageVec.end());

	// We will store the decrypted embedded file in this vector.
	std::vector<unsigned char> ExtractedFileVec;

	std::string decryptedName;

	bool isNameDecrypted = false;

	// Decrypt the embedded user's data filename and data file. Store decrypted data file in vector "ExtractedFileVec".
	
	// Each character of the embedded filename was xor encrypted against the characters of the XOR_KEY string, 
	// Each byte of the user's data file was xor encrypted against each character of the encrypted filename
	// First start xor decrypting the characters of the filename (store in "decryptedName" string) against the characters of XOR_KEY, 
	// then xor decrypt each byte of the data file against the encrypted characters of the filename.
	for (int i = 0; ImageVec.size() > i; i++) {

		if (!isNameDecrypted) {
			firstKeyPos = firstKeyPos > XOR_KEY_LENGTH ? XOR_KEY_START_POS : firstKeyPos;
			secondKeyPos = secondKeyPos > ENCRYPTED_NAME_LENGTH ? XOR_KEY_START_POS : secondKeyPos;
			decryptedName += ENCRYPTED_NAME[i] ^ XOR_KEY[firstKeyPos++];
		}

		ExtractedFileVec.insert(ExtractedFileVec.begin() + i, ImageVec[i] ^ ENCRYPTED_NAME[secondKeyPos++]);

		if (i >= ENCRYPTED_NAME_LENGTH - 1) {
			isNameDecrypted = true;
			secondKeyPos = secondKeyPos > ENCRYPTED_NAME_LENGTH - 1 ? XOR_KEY_START_POS : secondKeyPos;
		}
	}

	if (decryptedName.substr(0, 4) != "jdv_") {
		decryptedName = "jdv_" + decryptedName;
	}

	// Write data from vector "ImageVec" out to file.
	std::ofstream writeFile(decryptedName, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	writeFile.write((char*)&ExtractedFileVec[0], ImageVec.size());

	std::cout << "\nExtracted file: \"" + decryptedName + " " << ImageVec.size() << " " << "Bytes\"\n";
}

void readFilesIntoVectors(std::ifstream& readImage, std::ifstream& readFile, const std::string& IMAGE_FILE, const std::string& DATA_FILE, const ptrdiff_t& IMAGE_SIZE, const ptrdiff_t& DATA_SIZE, int argc, int sub) {

	// Reset position of files. 
	readImage.seekg(0, readImage.beg),
	readFile.seekg(0, readFile.beg);

	// The first 152 bytes of this vector contains the basic profile.
	std::vector<unsigned char>
		ProfileVec{
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
	},
		ProfileBlockVec
	{
			0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C,
			0x45, 0x00, 0x01, 0x01
	},
		// Read-in user JPG image file and store in vector "ImageVec".
		ImageVec((std::istreambuf_iterator<char>(readImage)), std::istreambuf_iterator<char>());

	const std::string
		TXT_NUM = std::to_string(sub - argc),			// If we embed multiple files (max 5), each outputted image will be differentiated, 
		EMBEDDED_IMAGE_FILE = "jdvimg" + TXT_NUM + ".jpg",  	// by a number in the name, jdvimg1.jpg, jdvimg2.jpg, jdvimg3.jpg, etc.
		JPG_SIG = "\xFF\xD8\xFF",				// JPG image signature. 
		JPG_CHECK{ ImageVec.begin(), ImageVec.begin() + JPG_SIG.length() };	// Get image header from vector. 

	// Make sure image file has valid JPG header.
	if (JPG_CHECK != JPG_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	// Signature for Define Quantization Table(s) 
	const std::vector<unsigned char> DQT_SIG{ 0xFF, 0xDB };

	// Find location in ImageVec of first DQT.
	const ptrdiff_t DQT_POS = search(ImageVec.begin(), ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - ImageVec.begin();

	// Erase the first n bytes before this DQT position.
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + DQT_POS);

	// We don't want "./" or ".\" characters at the start of the filename (user's data file), which we will store in the iCC Profile. 
	const size_t FIRST_SLASH_POS = DATA_FILE.find_first_of("\\/");

	const std::string NO_SLASH_NAME = DATA_FILE.substr(FIRST_SLASH_POS + 1, DATA_FILE.length());
	
	const size_t NO_SLASH_NAME_LENGTH = NO_SLASH_NAME.length(); // Character length of filename for the embedded data file.

	const int MAX_LENGTH_FILENAME = 23;

	// Make sure character length of filename (user's data file) does not exceed set maximum.
	if (NO_SLASH_NAME_LENGTH > MAX_LENGTH_FILENAME) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(NO_SLASH_NAME_LENGTH) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::string encryptedName;

	const int
		PROFILE_NAME_LENGTH_INDEX = 32, // Index location inside the iCC Profile to store the length value of the embedded data's filename.
		PROFILE_NAME_INDEX = 33,	// Start index inside the iCC Profile to store the filename for the embedded data file.
		PROFILE_VEC_SIZE = 152,		// Byte size of main Profile. User's data file is stored after the main Profile content.
		XOR_KEY_START_POS = 0,		// Start index location of xor key characters (the first 6 bytes of "ProfileVec").
		XOR_KEY_LENGTH = 5;		// Length of xor key (Length value starts from 0).

	int
		firstKeyPos = XOR_KEY_START_POS,	// 0, First key position variable used for xor encrypting filename of user's data file.
		secondKeyPos = XOR_KEY_START_POS;	// 0, Second key position variable used for xor encrypting user's data file.

	char byte;

	bool isNameEncrypted = false;

	// Xor encrypt user's data filename and data file. Store in "ProfileVec".
	// The filename is xor encrypted against the first 6 characters within "ProfileVec".
	// Each byte of the user's data file is xor encrypted against each character of the encrypted filename.
	for (int i = 0, insertIndex = PROFILE_VEC_SIZE; DATA_SIZE + PROFILE_VEC_SIZE > insertIndex; i++) {
		
		byte = readFile.get();

		if (!isNameEncrypted) {
			firstKeyPos = firstKeyPos > XOR_KEY_LENGTH ? XOR_KEY_START_POS : firstKeyPos;
			secondKeyPos = secondKeyPos > NO_SLASH_NAME_LENGTH ? XOR_KEY_START_POS : secondKeyPos;
			encryptedName += NO_SLASH_NAME[i] ^ ProfileVec[firstKeyPos++]; // xor each character of filename against first six characters of "ProfileVec".
		}

		ProfileVec.insert(ProfileVec.begin() + insertIndex++, byte ^ encryptedName[secondKeyPos++]);

		if (i >= NO_SLASH_NAME_LENGTH - 1) {
			isNameEncrypted = true;
			secondKeyPos = secondKeyPos > NO_SLASH_NAME_LENGTH - 1 ? XOR_KEY_START_POS : secondKeyPos;
		}
	}

	// Insert the character length value of the filename of user's data file into the main Profile,
	insertValue(ProfileVec, PROFILE_NAME_LENGTH_INDEX, NO_SLASH_NAME_LENGTH, 8);

	// Make space for filename by removing equivalent length of characters from main Profile.
	ProfileVec.erase(ProfileVec.begin() + PROFILE_NAME_INDEX, ProfileVec.begin() + encryptedName.length() + PROFILE_NAME_INDEX);

	// Insert encrypted filename into the main profile "ProfileVec".
	ProfileVec.insert(ProfileVec.begin() + PROFILE_NAME_INDEX, encryptedName.begin(), encryptedName.end());

	const int BLOCK_SIZE = 65535;		// ICC Profile default block size (0xFFFF).

	int 
		bits = 16,			// Variable used with the "insertValue" function.
		tallySize = 2,			// Keep count of how much data we have traversed while inserting "ProfileBlockVec" at every "BLOCK_SIZE" within "ProfileVec". 
		profileCount = 0,		// Keep count of how many ICC Profile blocks ("ProfileBlockVec") we insert into the data file.
		profileMainBlockSizeIndex = 4,  // "ProfileVec" start index location for it's 2 byte block size field.
		profileBlockSizeIndex = 2,	// "ProfileBlockVec" start index location for it's 2 block size field.
		profileCountIndex = 72,		// Start index location in main Profile, where we store the value of the total number of inserted ICC Profile blocks (2 bytes max).
		profileDataSizeIndex = 88;	// Start index location in main Profile, where we store the file size value of user's data file. ( 4 bytes max).

	// Get updated size for "ProfileVec" after adding data file.
	const size_t VECTOR_SIZE = ProfileVec.size();

	// Where we see +4 (-4) or +2, these are the number of bytes at the start of "ProfileVec" (4 bytes: 0xFF, 0xD8, 0xFF, 0xE2) 
	// and "ProfileBlockVec" (2 bytes: 0xFF, 0xE2), just before the default "BLOCK_SIZE" bytes: 0xFF, 0xFF, where the block count starts from. 

	if (BLOCK_SIZE + 4 >= VECTOR_SIZE) {

		// Seems we are dealing with a small data file, all content fits within the main Profile block. 
		// No need to insert any "ProfileBlockVec's" for this file. Finish up, skip the "While loop" and write the "embedded" image output file, exit program.
		// Update Profile block size of "ProfileVec", as it is smaller than the set default (0xFFFF).
		
		insertValue(ProfileVec, profileMainBlockSizeIndex, VECTOR_SIZE - 4, bits);
		bits = 32;
		// Insert file size value of user's data file into "ProfileVec".
		insertValue(ProfileVec, profileDataSizeIndex, DATA_SIZE, bits);
	}

	// Insert "ProfileBlockVec" contents in data file, at every "BLOCK_SIZE", or whatever remaining data size that's under "BLOCK_SIZE", until end of file.
	if (VECTOR_SIZE > BLOCK_SIZE + 4) {

		bool isMoreData = true;

		while (isMoreData) {

			tallySize += BLOCK_SIZE + 2;

			if (BLOCK_SIZE + 2 >= ProfileVec.size() - tallySize + 2) {

				// Data file size remaining is less than the "BLOCK_SIZE" default value (0xFFFF), 
				// so we are near end of file. Update last profile block size & insert last "ProfileBlockVec" content, then exit the loop.
				insertValue(ProfileBlockVec, profileBlockSizeIndex, (ProfileVec.size() + ProfileBlockVec.size()) - (tallySize + 2), bits);
				profileCount++;
				// Insert final profileCount value into "ProfileVec".
				insertValue(ProfileVec, profileCountIndex, profileCount, bits);
				bits = 32;
				// Insert file size value of user's data file into "ProfileVec".
				insertValue(ProfileVec, profileDataSizeIndex, DATA_SIZE, bits);
				ProfileVec.insert(ProfileVec.begin() + tallySize, ProfileBlockVec.begin(), ProfileBlockVec.end()); // Insert final "ProfileBlockVec".
				isMoreData = false; // No more data, exit loop.
			}

			else {  // Keep going, we have not yet reached end of file (last block size).
				profileCount++;
				ProfileVec.insert(ProfileVec.begin() + tallySize, ProfileBlockVec.begin(), ProfileBlockVec.end());
			}
		}
	}

	// Insert contents of vector "ProfileVec" into vector "ImageVec", combining Profile blocks + user's data file with JPG image.	
	ImageVec.insert(ImageVec.begin(), ProfileVec.begin(), ProfileVec.end());

	// Write out to file the JPG image with the embedded user's data file.
	std::ofstream writeFile(EMBEDDED_IMAGE_FILE, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	writeFile.write((char*)&ImageVec[0], ImageVec.size());

	std::cout << "\nCreated output file: \"" + EMBEDDED_IMAGE_FILE + "\"\n";
}

void insertValue(std::vector<unsigned char>& vec, ptrdiff_t valueInsertIndex, const size_t& VALUE, int bits) {

	while (bits) vec[valueInsertIndex++] = (VALUE >> (bits -= 8)) & 0xff;
}

void displayInfo() {

	std::cout << R"(
JPG Data Vehicle for Reddit, (jdvrdt v1.0). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

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
