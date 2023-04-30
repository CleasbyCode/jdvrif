//	JPG Data Vehicle for Reddit, (JDVRDT v1.0). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

void processFiles(char* [], int, int, const std::string&);
void processEmbeddedImage(char* []);
void readFilesIntoVectors(std::ifstream&, std::ifstream&, const std::string&, const std::string&, const ptrdiff_t&, const ptrdiff_t&, int, int);
void insertValue(std::vector<unsigned char>&, ptrdiff_t, const size_t&, int);
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

		const std::string ERR_MSG = !readImage ? READ_ERR_MSG + "\"" + IMAGE_FILE + "\"\n\n" : READ_ERR_MSG + "\"" + DATA_FILE + "\"\n\n";

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	const int
		MAX_JPG_SIZE_BYTES = 20971520,		
		MAX_DATAFILE_SIZE_BYTES = 20971520;	

	readImage.seekg(0, readImage.end),
	readFile.seekg(0, readFile.end);

	const ptrdiff_t
		IMAGE_SIZE = readImage.tellg(),
		DATA_SIZE = readFile.tellg();

	if ((IMAGE_SIZE + DATA_SIZE) > MAX_JPG_SIZE_BYTES
		|| DATA_SIZE > MAX_DATAFILE_SIZE_BYTES) {

		const std::string
			SIZE_ERR_JPG = "\nImage Size Error: JPG image (+including embedded file size) must not exceed 20MB.\n\n",
			SIZE_ERR_DATA = "\nFile Size Error: Your data file must not exceed 20MB.\n\n",

			ERR_MSG = IMAGE_SIZE + MAX_DATAFILE_SIZE_BYTES > MAX_JPG_SIZE_BYTES ? SIZE_ERR_JPG : SIZE_ERR_DATA;

		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}

	readFilesIntoVectors(readImage, readFile, IMAGE_FILE, DATA_FILE, IMAGE_SIZE, DATA_SIZE, argc, sub);
}

void processEmbeddedImage(char* argv[]) {

	const std::string IMAGE_FILE = argv[2];

	if (IMAGE_FILE == "." || IMAGE_FILE == "/") std::exit(EXIT_FAILURE);

	std::ifstream readImage(IMAGE_FILE, std::ios::binary);

	if (!readImage) {
		std::cerr << "\nRead Error: Unable to open/read file: \"" + IMAGE_FILE + "\"\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::vector<unsigned char> ImageVec((std::istreambuf_iterator<char>(readImage)), std::istreambuf_iterator<char>());

	const int			
		XOR_KEY_START_POS = 0,  
		XOR_KEY_LENGTH = 5,	
		PROFILE_SIG_INDEX = 6,	
		PROFILE_LENGTH = 18,	
		JDV_SIG_INDEX = 25,	
		NAME_LENGTH_INDEX = 32,	
		NAME_INDEX = 33,	
		ICC_COUNT_INDEX = 72,	
		DATA_SIZE_INDEX = 88,	
		DATA_INDEX = 152,  	
		ENCRYPTED_NAME_LENGTH = ImageVec[NAME_LENGTH_INDEX], 
		PROFILE_DATA_SIZE = ImageVec[DATA_SIZE_INDEX] << 24 | ImageVec[DATA_SIZE_INDEX + 1] << 16 | ImageVec[DATA_SIZE_INDEX + 2] << 8 | ImageVec[DATA_SIZE_INDEX + 3]; 

	int 
		profileCount = ImageVec[ICC_COUNT_INDEX] << 8 | ImageVec[ICC_COUNT_INDEX + 1], 
		firstKeyPos = XOR_KEY_START_POS,  
		secondKeyPos = XOR_KEY_START_POS; 

	const std::string
		PROFILE_SIG = "ICC_PROFILE",		
		JDV_SIG = "JDVRdT",			
		XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF",	
		PROFILE_CHECK{ ImageVec.begin() + PROFILE_SIG_INDEX, ImageVec.begin() + PROFILE_SIG_INDEX + PROFILE_SIG.length() }, 
		JDV_CHECK{ ImageVec.begin() + JDV_SIG_INDEX, ImageVec.begin() + JDV_SIG_INDEX + JDV_SIG.length() },			
		ENCRYPTED_NAME = { ImageVec.begin() + NAME_INDEX, ImageVec.begin() + NAME_INDEX + ImageVec[NAME_LENGTH_INDEX] };	

	if (PROFILE_CHECK != PROFILE_SIG || JDV_CHECK != JDV_SIG) {
		std::cerr << "\nImage Error: Image file \"" << IMAGE_FILE << "\" does not appear to be a JDVRdT file-embedded image.\n\n";
		std::exit(EXIT_FAILURE);
	}

	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + DATA_INDEX);

	if (profileCount) { 
		
		ptrdiff_t findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;

		while (profileCount--) {
			ImageVec.erase(ImageVec.begin() + findProfileSigIndex, ImageVec.begin() + findProfileSigIndex + PROFILE_LENGTH);
			findProfileSigIndex = search(ImageVec.begin() + findProfileSigIndex, ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;
		}
	}

	ImageVec.erase(ImageVec.begin() + PROFILE_DATA_SIZE, ImageVec.end());

	std::vector<unsigned char> ExtractedFileVec;

	std::string decryptedName;

	bool isNameDecrypted = false;
	
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

	std::ofstream writeFile(decryptedName, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	writeFile.write((char*)&ExtractedFileVec[0], ImageVec.size());

	std::cout << "\nExtracted file: \"" + decryptedName + " " << ImageVec.size() << " " << "Bytes\"\n";
}

void readFilesIntoVectors(std::ifstream& readImage, std::ifstream& readFile, const std::string& IMAGE_FILE, const std::string& DATA_FILE, const ptrdiff_t& IMAGE_SIZE, const ptrdiff_t& DATA_SIZE, int argc, int sub) {
	
	readImage.seekg(0, readImage.beg),
	readFile.seekg(0, readFile.beg);
	
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
		ImageVec((std::istreambuf_iterator<char>(readImage)), std::istreambuf_iterator<char>());

	const std::string
		TXT_NUM = std::to_string(sub - argc),			
		EMBEDDED_IMAGE_FILE = "jdvimg" + TXT_NUM + ".jpg",  	
		JPG_SIG = "\xFF\xD8\xFF",				
		JPG_CHECK{ ImageVec.begin(), ImageVec.begin() + JPG_SIG.length() };	

	if (JPG_CHECK != JPG_SIG) {
		std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
		std::exit(EXIT_FAILURE);
	}
	
	const std::vector<unsigned char> DQT_SIG{ 0xFF, 0xDB };

	const ptrdiff_t DQT_POS = search(ImageVec.begin(), ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - ImageVec.begin();

	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + DQT_POS);

	const size_t FIRST_SLASH_POS = DATA_FILE.find_first_of("\\/");

	const std::string NO_SLASH_NAME = DATA_FILE.substr(FIRST_SLASH_POS + 1, DATA_FILE.length());
	
	const size_t NO_SLASH_NAME_LENGTH = NO_SLASH_NAME.length(); 

	const int MAX_LENGTH_FILENAME = 23;

	if (NO_SLASH_NAME_LENGTH > MAX_LENGTH_FILENAME) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(NO_SLASH_NAME_LENGTH) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::string encryptedName;

	const int
		PROFILE_NAME_LENGTH_INDEX = 32, 
		PROFILE_NAME_INDEX = 33,	
		PROFILE_VEC_SIZE = 152,		
		XOR_KEY_START_POS = 0,		
		XOR_KEY_LENGTH = 5;		

	int
		firstKeyPos = XOR_KEY_START_POS,	
		secondKeyPos = XOR_KEY_START_POS;	

	char byte;

	bool isNameEncrypted = false;
	
	for (int i = 0, insertIndex = PROFILE_VEC_SIZE; DATA_SIZE + PROFILE_VEC_SIZE > insertIndex; i++) {
		
		byte = readFile.get();

		if (!isNameEncrypted) {
			firstKeyPos = firstKeyPos > XOR_KEY_LENGTH ? XOR_KEY_START_POS : firstKeyPos;
			secondKeyPos = secondKeyPos > NO_SLASH_NAME_LENGTH ? XOR_KEY_START_POS : secondKeyPos;
			encryptedName += NO_SLASH_NAME[i] ^ ProfileVec[firstKeyPos++]; 
		}

		ProfileVec.insert(ProfileVec.begin() + insertIndex++, byte ^ encryptedName[secondKeyPos++]);

		if (i >= NO_SLASH_NAME_LENGTH - 1) {
			isNameEncrypted = true;
			secondKeyPos = secondKeyPos > NO_SLASH_NAME_LENGTH - 1 ? XOR_KEY_START_POS : secondKeyPos;
		}
	}
	
	insertValue(ProfileVec, PROFILE_NAME_LENGTH_INDEX, NO_SLASH_NAME_LENGTH, 8);
	
	ProfileVec.erase(ProfileVec.begin() + PROFILE_NAME_INDEX, ProfileVec.begin() + encryptedName.length() + PROFILE_NAME_INDEX);
	
	ProfileVec.insert(ProfileVec.begin() + PROFILE_NAME_INDEX, encryptedName.begin(), encryptedName.end());

	const int BLOCK_SIZE = 65535;		

	int 
		bits = 16,			
		tallySize = 2,			
		profileCount = 0,		
		profileMainBlockSizeIndex = 4,  
		profileBlockSizeIndex = 2,	
		profileCountIndex = 72,		
		profileDataSizeIndex = 88;	

	const size_t VECTOR_SIZE = ProfileVec.size();

	if (BLOCK_SIZE + 4 >= VECTOR_SIZE) {
		insertValue(ProfileVec, profileMainBlockSizeIndex, VECTOR_SIZE - 4, bits);
		bits = 32;
		insertValue(ProfileVec, profileDataSizeIndex, DATA_SIZE, bits);
	}

	if (VECTOR_SIZE > BLOCK_SIZE + 4) {

		bool isMoreData = true;

		while (isMoreData) {

			tallySize += BLOCK_SIZE + 2;

			if (BLOCK_SIZE + 2 >= ProfileVec.size() - tallySize + 2) {
				
				insertValue(ProfileBlockVec, profileBlockSizeIndex, (ProfileVec.size() + ProfileBlockVec.size()) - (tallySize + 2), bits);
				profileCount++;
				insertValue(ProfileVec, profileCountIndex, profileCount, bits);
				bits = 32;
				insertValue(ProfileVec, profileDataSizeIndex, DATA_SIZE, bits);
				ProfileVec.insert(ProfileVec.begin() + tallySize, ProfileBlockVec.begin(), ProfileBlockVec.end()); 
				isMoreData = false; 
			}

			else {  
				profileCount++;
				ProfileVec.insert(ProfileVec.begin() + tallySize, ProfileBlockVec.begin(), ProfileBlockVec.end());
			}
		}
	}

	ImageVec.insert(ImageVec.begin(), ProfileVec.begin(), ProfileVec.end());
	
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
