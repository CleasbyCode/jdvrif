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

	// Read file-embedded jpg image into vector "ImageVec".
	std::vector<unsigned char> ImageVec((std::istreambuf_iterator<char>(readImage)), std::istreambuf_iterator<char>());

	int
		profileSigIndex = 6,
		jdvSigIndex = 25,
		nameLengthIndex = 32,
		nameIndex = 33,
		countIndex = 72,
		dataSizeIndex = 88,
		dataIndex = 152,  // Start index location of user's data file within iCC Profile.
		nameLength = ImageVec[nameLengthIndex],
		profileCount = ImageVec[countIndex] << 8 | ImageVec[countIndex + 1],  // Get ICC_PROFILE insert count value stored in main profile.
		profileDataSize = ImageVec[dataSizeIndex] << 24 | ImageVec[dataSizeIndex + 1] << 16 | ImageVec[dataSizeIndex + 2] << 8 | ImageVec[dataSizeIndex + 3]; // Get data file size stored in main profile.
		
	const std::string
		PROFILE_SIG = "ICC_PROFILE",
		JDV_SIG = "JDVRdT",
		PROFILE_CHECK{ ImageVec.begin() + profileSigIndex, ImageVec.begin() + profileSigIndex + PROFILE_SIG.length() }, // Get ICC signature from vector "ImageVec".
		JDV_CHECK{ ImageVec.begin() + jdvSigIndex, ImageVec.begin() + jdvSigIndex + JDV_SIG.length() };	// Get JDVRdT signature from vector "ImageVec".

	if (PROFILE_CHECK != PROFILE_SIG || JDV_CHECK != JDV_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: Image file \"" << IMAGE_FILE << "\" does not appear to be a JDVRdT file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::string encryptedName = { ImageVec.begin() + nameIndex, ImageVec.begin() + nameIndex + ImageVec[nameLengthIndex] };	// Get encrypted filename stored in vector "ImageVec".
	
	// From "ImageVec" vector index 0, erase bytes so that start of vector is now the beginning of the user's data file.
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + dataIndex);

	if (profileCount) {

		// Within "ImageVec", find and erase all occurrences of the contents of "ProfileBlockVec".
		// Stop the search once profileCount is 0.
		ptrdiff_t findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;

		while (profileCount--) {
			ImageVec.erase(ImageVec.begin() + findProfileSigIndex, ImageVec.begin() + findProfileSigIndex + 18);
			findProfileSigIndex = search(ImageVec.begin() + findProfileSigIndex, ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;
		}
	}

	// Remove jpg image from data file. Erase all bytes starting from end of "profileDataSize". Vector now contains just the user's data file.
    ImageVec.erase(ImageVec.begin() + profileDataSize, ImageVec.end());

	// We will store the decrypted embedded file in this vector.
	std::vector<unsigned char> ExtractedFileVec;

	std::string decryptedName;

	bool nameDecrypted = false;

	int
		keyStartPos = 0,
		keyLength = 5,
		keyPos = keyStartPos;

	// Decrypt the embedded filename and the embedded data file.
	for (int i = 0; ImageVec.size() > i; i++) {
		if (!nameDecrypted) {
			keyPos = keyPos > keyStartPos + keyLength ? keyStartPos : keyPos;
			decryptedName += encryptedName[i] ^ JDV_SIG[keyPos];
		}

		ExtractedFileVec.insert(ExtractedFileVec.begin() + i, ImageVec[i] ^ encryptedName[keyPos++]);

		if (i >= nameLength - 1) {
			nameDecrypted = true;
			keyPos = keyPos > keyStartPos + keyLength ? keyStartPos : keyPos;
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
	// Without this basic profile, some image display programs will show error messages when loading the image.
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
		// Read-in user jpg image file and store in vector "ImageVec".
		ImageVec((std::istreambuf_iterator<char>(readImage)), std::istreambuf_iterator<char>());

	const std::string
		TXT_NUM = std::to_string(sub - argc),
		EMBEDDED_IMAGE_FILE = "jdvimg" + TXT_NUM + ".jpg",
		JPG_SIG = "\xFF\xD8\xFF",
		JPG_CHECK{ ImageVec.begin(), ImageVec.begin() + JPG_SIG.length() };	// Get image header from vector. 

	// Make sure image has valid jpg header.
	if (JPG_CHECK != JPG_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	// Sig for Define Quantization Table(s) 
	std::vector<unsigned char> DQT_SIG{ 0xFF, 0xDB };

	// Find location in ImageVec of first DQT.
	ptrdiff_t dqtPos = search(ImageVec.begin(), ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - ImageVec.begin();

	// Erase the first n bytes before this DQT position.
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + dqtPos);

	const int MAX_LENGTH_FILENAME = 23;

	// We don't want "./" or ".\" characters at the start of the filename (user's data file), which we will store in the iCC Profile. 
	std::size_t firstSlashPos = DATA_FILE.find_first_of("\\/");
	std::string
		noSlashName = DATA_FILE.substr(firstSlashPos + 1, DATA_FILE.length()),
		encryptedName;

	int
		profileNameLengthIndex = 32,	// Index location inside the iCC Profile to store the length value of the embedded data's filename.
		profileNameIndex = 33,		// Start index inside the iCC Profile to store the filename for the embedded data file.
		xorKeyStartPos = 25,
		xorKeyLength = 5,
		xorKeyIndex = 25;

	size_t noSlashNameLength = noSlashName.length(); // Character length of filename for the embedded data file.

	// Make sure character length of filename (user's data file) does not exceed set maximum.
	if (noSlashNameLength > MAX_LENGTH_FILENAME) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(noSlashNameLength) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}

	// Insert the character length value of the filename (user's data file) into the iCC Profile,
	// We need to know how many characters to read when later retrieving the filename from the profile.
	insertValue(ProfileVec, profileNameLengthIndex, noSlashNameLength, 8);

	// Make space for this data by removing equivalent length of characters from iCC Profile.
	ProfileVec.erase(ProfileVec.begin() + profileNameIndex, ProfileVec.begin() + noSlashNameLength + profileNameIndex);

	// Encrypt filename.
	for (int i = 0; noSlashNameLength != 0; noSlashNameLength--) {
		xorKeyIndex = xorKeyIndex > xorKeyStartPos + xorKeyLength ? xorKeyStartPos : xorKeyIndex;  // Reset position.
		encryptedName += noSlashName[i++] ^ ProfileVec[xorKeyIndex++]; // xor each character of filename against each byte (8) of the crc values.
	}

	ProfileVec.insert(ProfileVec.begin() + profileNameIndex, encryptedName.begin(), encryptedName.end());

	char byte;

	xorKeyStartPos = 33;
	xorKeyIndex = xorKeyStartPos;

	for (int insertIndex = 152; DATA_SIZE + 152 > insertIndex;) {
		byte = readFile.get();
		ProfileVec.insert((ProfileVec.begin() + insertIndex++), byte ^ ProfileVec[xorKeyIndex++]);
		xorKeyIndex = xorKeyIndex > xorKeyStartPos + xorKeyLength ? xorKeyStartPos : xorKeyIndex; // Reset position.
	}

	// Insert user data file at end of ProfileVec.
	//ProfileVec.resize(DATA_SIZE + ProfileVec.size() / sizeof(unsigned char));
	//readFile.read((char*)&ProfileVec[152], DATA_SIZE);

	int bits = 16,
		blockSize = 65535,				// ICC profile max block size.
		tallySize = 2,
		profileCount = 0,
		profileMainBlockSizeIndex = 4,  // ProfileVec 2 byte length field vector index location.
		profileBlockSizeIndex = 2,		// ProfileBlockVec 2 byte length field vector index location.
		profileCountIndex = 72,			// Index location in main profile, where we store the total number of inserted ICC profile blocks.
		profileDataSizeIndex = 88;		// Index location in main profile, where we store file size of user's data file.

	// Get update size for ProfileVec after adding user data file.
	const size_t VECTOR_SIZE = ProfileVec.size();
	
	// Where we see +4 (-4) or +2, these are the number of bytes at the start of "ProfileVec" (4 bytes: 0xFF, 0xD8, 0xFF, 0xE2) 
	// and "ProfileBlockVec" (2 bytes: 0xFF, 0xE2), just before the default block size bytes 0xFF, 0xFF, where the block count starts from. 

	if (blockSize + 4 >= VECTOR_SIZE) {
		// Update profile block size of "ProfileVec", as it is smaller than the set default (FFFF).
		insertValue(ProfileVec, profileMainBlockSizeIndex, VECTOR_SIZE - 4, bits);
		bits = 32;
		insertValue(ProfileVec, profileDataSizeIndex, DATA_SIZE, bits);
	}
	
	// Insert "ProfileBlockVec" at every "blockSize", or whatever data size that's under "blockSize" remains, until end of file.
	if (VECTOR_SIZE > blockSize + 4) {

		bool isTrue = true;

		while (isTrue) {
			tallySize += blockSize + 2; // Tally of how much data we have traversed through when inserting "ProfileBlockVec" at every "blockSize" within "ProfileVec". 
			if (blockSize + 2 >= ProfileVec.size() - tallySize + 2) {
				// Data file size remaining is less than "blockSize" default (FFFF), so update profile block size & insert last "ProfileBlockVec" then exit the loop.
				profileCount++;
				insertValue(ProfileBlockVec, profileBlockSizeIndex, (ProfileVec.size() + ProfileBlockVec.size()) - (tallySize + 2), bits);
				insertValue(ProfileVec, profileCountIndex, profileCount, bits);
				bits = 32;
				insertValue(ProfileVec, profileDataSizeIndex, DATA_SIZE, bits);
				ProfileVec.insert(ProfileVec.begin() + tallySize, ProfileBlockVec.begin(), ProfileBlockVec.end());
				isTrue = false;
			}
			else {  // Keep going, we have not yet reached end of file (last blockSize).
				profileCount++;
				ProfileVec.insert(ProfileVec.begin() + tallySize, ProfileBlockVec.begin(), ProfileBlockVec.end());
			}
		}
	}

	// Insert contents of vector "ProfileVec" into vector "ImageVec", combining iCC Profile chunk + user's data file with jpg image.	
	ImageVec.insert(ImageVec.begin(), ProfileVec.begin(), ProfileVec.end());

	// Write out to file the jpg image with the embedded user's data file.
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
