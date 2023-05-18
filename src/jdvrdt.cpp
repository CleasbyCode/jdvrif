//	JPG Data Vehicle for Reddit, (JDVRDT v1.2). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

typedef unsigned char BYTE;
typedef unsigned short SBYTE;

struct jdvStruct {
	std::vector<BYTE> ImageVec, FileVec, ProfileVec, EmbdImageVec, EncryptedVec, DecryptedVec;
	std::string IMAGE_NAME, FILE_NAME, MODE;
	size_t IMAGE_SIZE{}, FILE_SIZE{};
	SBYTE imgVal{}, subVal{};
};

class ValueUpdater {
public:
	void Value(std::vector<BYTE>& vect, BYTE(&ary)[], int valueInsertIndex, const size_t VALUE, SBYTE bits, bool isArray) {
		if (isArray) {
			while (bits) {
				ary[valueInsertIndex++] = (VALUE >> (bits -= 8)) & 0xff;
			}
		}
		else {
			while (bits) {
				vect[valueInsertIndex++] = (VALUE >> (bits -= 8)) & 0xff;
			}
		}
	}
} *update;

class VectorFill {
public:
	void Vector(std::vector<BYTE>& vect, std::ifstream& rFile, const size_t FSIZE) {
		vect.resize(FSIZE / sizeof(BYTE));
		rFile.read((char*)vect.data(), FSIZE);
	}
} *fill;

void openFiles(char* [], jdvStruct& jdv);
void removeProfileHeaders(jdvStruct& jdv);
void encryptDecrypt(jdvStruct& jdv);
void insertProfileBlocks(jdvStruct& jdv);
void writeOutFile(jdvStruct& jdv);
void displayInfo();

int main(int argc, char** argv) {
	
	jdvStruct jdv;
	
	if (argc == 2 && std::string(argv[1]) == "--info") {
		argc = 0;
		displayInfo();
	}
	else if (argc >= 4 && argc < 10 && std::string(argv[1]) == "-i") { 
		jdv.MODE = argv[1], jdv.subVal = argc - 1, jdv.IMAGE_NAME = argv[2];
		argc -= 2;
		
		while (argc != 1) {  
			jdv.imgVal = argc, jdv.FILE_NAME = argv[3];
			openFiles(argv++, jdv);
			argc--;
		}
		
		argc = 1;
	}
	else if (argc >= 3 && argc < 9 && std::string(argv[1]) == "-x") { 
		jdv.MODE = argv[1];
		
		while (argc >= 3) { 
			jdv.IMAGE_NAME = argv[2];
			openFiles(argv++, jdv);
			argc--;
		}
	}
	else {
		std::cout << "\nUsage:\t\bjdvrdt -i <jpg-image>  <file(s)>\n\t\bjdvrdt -x <jpg-image(s)>\n\t\bjdvrdt --info\n\n";
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

void openFiles(char* argv[], jdvStruct& jdv) {
	
	const std::string READ_ERR_MSG = "\nRead Error: Unable to open/read file: ";
	
	std::ifstream
		readImage(jdv.IMAGE_NAME, std::ios::binary),
		readFile(jdv.FILE_NAME, std::ios::binary);
	
	if (jdv.MODE == "-i" && (!readImage || !readFile) || jdv.MODE == "-x" && !readImage) {
		const std::string ERR_MSG = !readImage ? READ_ERR_MSG + "\"" + jdv.IMAGE_NAME + "\"\n\n" : READ_ERR_MSG + "\"" + jdv.FILE_NAME + "\"\n\n";
		std::cerr << ERR_MSG;
		std::exit(EXIT_FAILURE);
	}
	
	readImage.seekg(0, readImage.end), 
	readFile.seekg(0, readFile.end);
	
	jdv.IMAGE_SIZE = readImage.tellg(); 
	jdv.FILE_SIZE = readFile.tellg();
	
	readImage.seekg(0, readImage.beg), 
	readFile.seekg(0, readFile.beg);
	
	if (jdv.MODE == "-x") { 
		fill->Vector(jdv.EmbdImageVec, readImage, jdv.IMAGE_SIZE);
		const SBYTE JDV_SIG_INDEX = 25;	
		
		if (jdv.EmbdImageVec[JDV_SIG_INDEX] == 'J' && jdv.EmbdImageVec[JDV_SIG_INDEX + 4] == 'd') {
			removeProfileHeaders(jdv);
		}
		else {
			std::cerr << "\nImage Error: Image file \"" << jdv.IMAGE_NAME << "\" does not appear to be a JDVRdT file-embedded image.\n\n";
			std::exit(EXIT_FAILURE);
		}
	}
	else { 
		const int MAX_FILE_SIZE_BYTES = 20971520;
		
		if (jdv.IMAGE_SIZE + jdv.FILE_SIZE > MAX_FILE_SIZE_BYTES) {
			std::cerr << "\nFile Size Error: Your image file (including size of data file) must not exceed 20MB.\n\n";
			std::exit(EXIT_FAILURE);
		}
		
		jdv.ProfileVec.reserve(jdv.FILE_SIZE);
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
		
		fill->Vector(jdv.ImageVec, readImage, jdv.IMAGE_SIZE);
		fill->Vector(jdv.FileVec, readFile, jdv.FILE_SIZE);
		
		jdv.EncryptedVec.reserve(jdv.FILE_SIZE);
		
		const std::string
			JPG_SIG = "\xFF\xD8\xFF",
			JPG_CHECK{ jdv.ImageVec.begin(), jdv.ImageVec.begin() + JPG_SIG.length() };
		
		if (JPG_CHECK != JPG_SIG) {
			std::cerr << "\nImage Error: File does not appear to be a valid JPG image.\n\n";
			std::exit(EXIT_FAILURE);
		}
		
		const auto DQT_SIG = { 0xFF, 0xDB };
		const size_t DQT_POS = search(jdv.ImageVec.begin(), jdv.ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - jdv.ImageVec.begin();
		
		jdv.ImageVec.erase(jdv.ImageVec.begin(), jdv.ImageVec.begin() + DQT_POS);
		
		encryptDecrypt(jdv);
	}
}

void removeProfileHeaders(jdvStruct& jdv) {	
	
	const SBYTE
		PROFILE_HEADER_LENGTH = 18,	
		NAME_LENGTH_INDEX = 32,		
		NAME_LENGTH = jdv.EmbdImageVec[NAME_LENGTH_INDEX], 
		NAME_INDEX = 33,		
		PROFILE_COUNT_INDEX = 72,	
		FILE_SIZE_INDEX = 88,		
		FILE_INDEX = 152; 	
	
	const size_t FILE_SIZE = jdv.EmbdImageVec[FILE_SIZE_INDEX] << 24 | jdv.EmbdImageVec[FILE_SIZE_INDEX + 1] << 16 |
				jdv.EmbdImageVec[FILE_SIZE_INDEX + 2] << 8 | jdv.EmbdImageVec[FILE_SIZE_INDEX + 3];
	const std::string PROFILE_SIG = "ICC_PROFILE";	
	
	SBYTE profileCount = jdv.EmbdImageVec[PROFILE_COUNT_INDEX] << 8 | jdv.EmbdImageVec[PROFILE_COUNT_INDEX + 1];
	
	jdv.FILE_NAME = { jdv.EmbdImageVec.begin() + NAME_INDEX, jdv.EmbdImageVec.begin() + NAME_INDEX + jdv.EmbdImageVec[NAME_LENGTH_INDEX] };
	jdv.EmbdImageVec.erase(jdv.EmbdImageVec.begin(), jdv.EmbdImageVec.begin() + FILE_INDEX);
	
	size_t headerIndex = 0; 
	
	while (profileCount--) {
		headerIndex = search(jdv.EmbdImageVec.begin() + headerIndex, jdv.EmbdImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - jdv.EmbdImageVec.begin() - 4;
		jdv.EmbdImageVec.erase(jdv.EmbdImageVec.begin() + headerIndex, jdv.EmbdImageVec.begin() + headerIndex + PROFILE_HEADER_LENGTH);
	}
	
	jdv.EmbdImageVec.erase(jdv.EmbdImageVec.begin() + FILE_SIZE, jdv.EmbdImageVec.end());
	jdv.DecryptedVec.reserve(FILE_SIZE);
	jdv.EmbdImageVec.swap(jdv.FileVec);
	
	encryptDecrypt(jdv);
}
void encryptDecrypt(jdvStruct& jdv) {
	
	if (jdv.MODE == "-i") { 	
		size_t lastSlashPos = jdv.FILE_NAME.find_last_of("\\/");
		
		if (lastSlashPos <= jdv.FILE_NAME.length()) {
			std::string_view new_name(jdv.FILE_NAME.c_str() + (lastSlashPos + 1), jdv.FILE_NAME.length() - (lastSlashPos + 1));
			jdv.FILE_NAME = new_name;
		}
	}
	
	const std::string XOR_KEY = "\xFF\xD8\xFF\xE2\xFF\xFF";	
	const SBYTE MAX_LENGTH_FILENAME = 23;
	const size_t
		NAME_LENGTH = jdv.FILE_NAME.length(),	
		XOR_KEY_LENGTH = XOR_KEY.length(),
		FILE_SIZE = jdv.FileVec.size();		
	
	if (NAME_LENGTH > MAX_LENGTH_FILENAME) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(NAME_LENGTH) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}
	
	std::string
		inName = jdv.FILE_NAME,
		outName;
	
	size_t indexPos = 0; 
	
	SBYTE
		xorKeyStartPos = 0,
		xorKeyPos = xorKeyStartPos,	
		nameKeyStartPos = 0,
		nameKeyPos = nameKeyStartPos,	
		bits = 8;	
	
	while (FILE_SIZE > indexPos) {
		
		if (indexPos >= NAME_LENGTH) {
			nameKeyPos = nameKeyPos > NAME_LENGTH ? nameKeyStartPos : nameKeyPos;	 
		}
		else {
			xorKeyPos = xorKeyPos > XOR_KEY_LENGTH ? xorKeyStartPos : xorKeyPos;	
			outName += inName[indexPos] ^ XOR_KEY[xorKeyPos++];										
		}
		
		if (jdv.MODE == "-i") {
			jdv.EncryptedVec.emplace_back(jdv.FileVec[indexPos++] ^ outName[nameKeyPos++]);
		}
		else {
			jdv.DecryptedVec.emplace_back(jdv.FileVec[indexPos++] ^ inName[nameKeyPos++]);
		}
	}
	
	if (jdv.MODE == "-i") { 
		const SBYTE
			PROFILE_NAME_LENGTH_INDEX = 32, 
			PROFILE_NAME_INDEX = 33,	
			PROFILE_VEC_SIZE = 152;	
		
		jdv.ProfileVec[PROFILE_NAME_LENGTH_INDEX] = static_cast<int>(NAME_LENGTH);
		jdv.ProfileVec.erase(jdv.ProfileVec.begin() + PROFILE_NAME_INDEX, jdv.ProfileVec.begin() + outName.length() + PROFILE_NAME_INDEX);
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + PROFILE_NAME_INDEX, outName.begin(), outName.end());
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + PROFILE_VEC_SIZE, jdv.EncryptedVec.begin(), jdv.EncryptedVec.end());
		
		insertProfileBlocks(jdv);
	}
	else { 
		jdv.FILE_NAME = outName;
		writeOutFile(jdv);
	}
}

void insertProfileBlocks(jdvStruct& jdv) {
	
	const SBYTE PROFILE_HEADER_SIZE = 18;
	const size_t
		VECTOR_SIZE = jdv.ProfileVec.size(),	
		BLOCK_SIZE = 65535;	
	
	size_t tallySize = 2;	
	
	BYTE ProfileHeaderBlock[PROFILE_HEADER_SIZE] = { 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01 };
	
	SBYTE
		bits = 16,			
		profileCount = 0,		
		profileMainBlockSizeIndex = 4,  
		profileBlockSizeIndex = 2,	
		profileCountIndex = 72,		
		profileDataSizeIndex = 88;
	
	if (BLOCK_SIZE + 4 >= VECTOR_SIZE) {
		update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileMainBlockSizeIndex, VECTOR_SIZE - 4, bits, false);
		update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileBlockSizeIndex, (PROFILE_HEADER_SIZE - 2), bits, true);
		jdv.ProfileVec.insert(jdv.ProfileVec.begin() + VECTOR_SIZE, &ProfileHeaderBlock[0], &ProfileHeaderBlock[PROFILE_HEADER_SIZE]); 
		profileCount = 1; 
		update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileCountIndex, profileCount, bits, false);
		bits = 32;
		update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileDataSizeIndex, jdv.FILE_SIZE, bits, false);
	}
	
	if (VECTOR_SIZE > BLOCK_SIZE + 4) {
		bool isMoreData = true;
		
		while (isMoreData) {
			tallySize += BLOCK_SIZE + 2;
			
			if (BLOCK_SIZE + 2 >= jdv.ProfileVec.size() - tallySize + 2) {
				update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileBlockSizeIndex, (jdv.ProfileVec.size() + PROFILE_HEADER_SIZE) - (tallySize + 2), bits, true);
				profileCount++;
				update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileCountIndex, profileCount, bits, false);
				bits = 32;
				update->Value(jdv.ProfileVec, ProfileHeaderBlock, profileDataSizeIndex, jdv.FILE_SIZE, bits, false);
				jdv.ProfileVec.insert(jdv.ProfileVec.begin() + tallySize, &ProfileHeaderBlock[0], &ProfileHeaderBlock[PROFILE_HEADER_SIZE]);
				isMoreData = false;
			}
			else {  
				profileCount++;
				jdv.ProfileVec.insert(jdv.ProfileVec.begin() + tallySize, &ProfileHeaderBlock[0], &ProfileHeaderBlock[PROFILE_HEADER_SIZE]);
			}
		}
	}
	
	jdv.ImageVec.insert(jdv.ImageVec.begin(), jdv.ProfileVec.begin(), jdv.ProfileVec.end());
	std::string diffVal = std::to_string(jdv.subVal - jdv.imgVal);																	
	jdv.FILE_NAME = "jdv_img" + diffVal + ".jpg";
	writeOutFile(jdv);
}

void writeOutFile(jdvStruct& jdv) {
	
	std::ofstream writeFile(jdv.FILE_NAME, std::ios::binary);
	
	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}
	
	if (jdv.MODE == "-i") {
		writeFile.write((char*)&jdv.ImageVec[0], jdv.ImageVec.size());
		std::cout << "\nCreated output file: \"" + jdv.FILE_NAME + " " << jdv.ImageVec.size() << " " << "Bytes\"\n";
		jdv.EncryptedVec.clear();
	}
	
	else {
		writeFile.write((char*)&jdv.DecryptedVec[0], jdv.DecryptedVec.size());
		std::cout << "\nExtracted file: \"" + jdv.FILE_NAME + " " << jdv.DecryptedVec.size() << " " << "Bytes\"\n";
		jdv.DecryptedVec.clear();
	}
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

Using jdvrdt, You can insert up to six files at a time (outputs one image per file).

You can also extract files from up to six images at a time.

)";
}
