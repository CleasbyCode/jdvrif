//	JPG Data Vehicle for Reddit, (JDVRDT v1.0). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

void processFiles(char* [], int, int, const std::string&);
void processEmbeddedImage(char* []);
void readFilesIntoVectors(std::ifstream&, std::ifstream&, const std::string&, const std::string&, const ptrdiff_t&, const ptrdiff_t&, int, int);
void insertValue(std::vector<unsigned char>&, ptrdiff_t, const size_t&, int);
void inflateDeflate(std::vector<unsigned char>&, bool);
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

	int profileSigIndex = 6;

	const std::string
		PROFILE_SIG = "ICC_PROFILE",
		PROFILE_CHECK{ ImageVec.begin() + profileSigIndex, ImageVec.begin() + profileSigIndex + PROFILE_SIG.length()};

	if (PROFILE_CHECK != PROFILE_SIG) {
		std::cerr << "\nImage Error: Image file \"" << IMAGE_FILE << "\" does not appear to contain a valid iCC Profile.\n\n";
		std::exit(EXIT_FAILURE);
	}

	int deflateDataIndex = 20;  
  
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + deflateDataIndex);

	std::vector<unsigned char> DQT_SIG{ 0xFF, 0xDB, 0x00, 0x43 };
	
	ptrdiff_t 
		findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4,
		dqtFirstPos = search(ImageVec.begin(), ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - ImageVec.begin();
	
	std::cout << "\nSearching for embedded data file. Please wait...\n";

	while (dqtFirstPos >= findProfileSigIndex) {
		ImageVec.erase(ImageVec.begin() + findProfileSigIndex, ImageVec.begin() + findProfileSigIndex + 18);
		findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;
	}
	
	dqtFirstPos = search(ImageVec.begin(), ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - ImageVec.begin();

	ImageVec.erase(ImageVec.begin() + dqtFirstPos, ImageVec.end());

	bool inflate = true;

	inflateDeflate(ImageVec, inflate);

	const std::string
		JDV_SIG = "JDV",
		JDV_CHECK{ ImageVec.begin() + 5, ImageVec.begin() + 5 + JDV_SIG.length() };	
		
	if (JDV_CHECK != JDV_SIG) {
		std::cerr << "\nProfile Error: iCC Profile does not seem to be a JDV modified profile.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::string fileName = { ImageVec.begin() + 13, ImageVec.begin() + 13 + ImageVec[12] };	
	if (fileName.substr(0, 4) != "jdv_") {
		fileName = "jdv_" + fileName;
	}

	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + 132);

	std::ofstream writeFile(fileName, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	writeFile.write((char*)&ImageVec[0], ImageVec.size());

	std::cout << "\nExtracted file: \"" + fileName + " "  << ImageVec.size() << " " << "Bytes\"\n";
}

void readFilesIntoVectors(std::ifstream& readImage, std::ifstream& readFile, const std::string& IMAGE_FILE, const std::string& DATA_FILE, const ptrdiff_t& IMAGE_SIZE, const ptrdiff_t& DATA_SIZE, int argc, int sub) {

	readImage.seekg(0, readImage.beg),
	readFile.seekg(0, readFile.beg);
  
	std::vector<unsigned char>
		ProfileVec{
			0x00, 0x00, 0x00, 0x84, 0x20, 0x4A, 0x44, 0x56, 0x00, 0x00, 0x00, 0x20,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x61, 0x63, 0x73, 0x70, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	},

		ProfileFirstChunkVec
	{
			0xFF, 0xD8, 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46,
			0x49, 0x4C, 0x45, 0x00, 0x00, 0x02
	},

		ProfileChunkVec
	{
			0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52, 0x4F, 0x46,
			0x49, 0x4C, 0x45, 0x00, 0x00, 0x02
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

	std::vector<unsigned char> DQT_SIG{ 0xFF, 0xDB };
  
	ptrdiff_t dqtPos = search(ImageVec.begin(), ImageVec.end(), DQT_SIG.begin(), DQT_SIG.end()) - ImageVec.begin();

	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + dqtPos);

	const int MAX_LENGTH_FILENAME = 23;
  
	std::size_t firstSlashPos = DATA_FILE.find_first_of("\\/");
	std::string
		noSlashName = DATA_FILE.substr(firstSlashPos + 1, DATA_FILE.length()),
		encryptedName;

	ptrdiff_t
		profileNameLengthIndex = 12,	
		profileNameIndex = 13,		
		noSlashNameLength = noSlashName.length(); 

	if (noSlashNameLength > MAX_LENGTH_FILENAME) {
		std::cerr << "\nFile Error: Filename length of your data file (" + std::to_string(noSlashNameLength) + " characters) is too long.\n"
			"\nFor compatibility requirements, your filename must be under 24 characters.\nPlease try again with a shorter filename.\n\n";
		std::exit(EXIT_FAILURE);
	}

	insertValue(ProfileVec, profileNameLengthIndex, noSlashNameLength, 8);
  
	ProfileVec.erase(ProfileVec.begin() + profileNameIndex, ProfileVec.begin() + noSlashNameLength + profileNameIndex);

	ProfileVec.insert(ProfileVec.begin() + profileNameIndex, noSlashName.begin(), noSlashName.end());

	ProfileVec.resize(DATA_SIZE + ProfileVec.size() / sizeof(unsigned char));
	readFile.read((char*)&ProfileVec[132], DATA_SIZE);

	bool inflate = false;  

	inflateDeflate(ProfileVec, inflate);

	const size_t DEFLATE_VECTOR_SIZE = ProfileVec.size() + ProfileFirstChunkVec.size(); 

	int bits = 16,
		profileChunkSize = 65535,
		profileFirstChunkSizeIndex = 4,
		profileChunkSizeIndex = 2,
		profileFirstByteCount = 4,	
		profileChunkByteCount = 2;	

	if (profileChunkSize > DEFLATE_VECTOR_SIZE) {
		insertValue(ProfileFirstChunkVec, profileFirstChunkSizeIndex, DEFLATE_VECTOR_SIZE - profileFirstByteCount, bits);
	}
  
	ProfileVec.insert(ProfileVec.begin(), ProfileFirstChunkVec.begin(), ProfileFirstChunkVec.end());

	if (DEFLATE_VECTOR_SIZE > profileChunkSize) {

		ProfileVec.insert(ProfileVec.begin() + profileChunkSize + profileFirstByteCount, ProfileChunkVec.begin(), ProfileChunkVec.end());

		int totalChunkSize = profileChunkSize + profileFirstByteCount + profileChunkByteCount;  

		bool isTrue = true;

		while (isTrue) {
			totalChunkSize += profileChunkSize + profileChunkByteCount; 
			if (profileChunkSize > ProfileVec.size() - (totalChunkSize + profileChunkByteCount)) {
				insertValue(ProfileChunkVec, profileChunkSizeIndex, (ProfileVec.size() + ProfileChunkVec.size()) - (totalChunkSize + profileChunkByteCount), bits);
				ProfileVec.insert(ProfileVec.begin() + totalChunkSize, ProfileChunkVec.begin(), ProfileChunkVec.end());
				isTrue = false;
			}
			else {
				ProfileVec.insert(ProfileVec.begin() + totalChunkSize, ProfileChunkVec.begin(), ProfileChunkVec.end());
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

void inflateDeflate(std::vector<unsigned char>& Vec, bool inflateData) {
  
	std::vector <unsigned char> Buffer;

	size_t BUFSIZE;

	if (!inflateData) {
		BUFSIZE = 1032 * 1024;  
	}
	else {
		BUFSIZE = 256 * 1024;  
	}

	unsigned char* temp_buffer{ new unsigned char[BUFSIZE] };

	z_stream strm;
	strm.zalloc = 0;
	strm.zfree = 0;
	strm.next_in = Vec.data();
	strm.avail_in = Vec.size();
	strm.next_out = temp_buffer;
	strm.avail_out = BUFSIZE;

	if (inflateData) {
		inflateInit(&strm);
	}
	else {
		deflateInit(&strm, 6); 
	}

	while (strm.avail_in)
	{
		if (inflateData) {
			inflate(&strm, Z_NO_FLUSH);
		}
		else {
			deflate(&strm, Z_NO_FLUSH);
		}

		if (!strm.avail_out)
		{
			Buffer.insert(Buffer.end(), temp_buffer, temp_buffer + BUFSIZE);
			strm.next_out = temp_buffer;
			strm.avail_out = BUFSIZE;
		}
		else
			break;
	}
	if (inflateData) {
		inflate(&strm, Z_FINISH);
		Buffer.insert(Buffer.end(), temp_buffer, temp_buffer + BUFSIZE - strm.avail_out);
		inflateEnd(&strm);
	}
	else {
		deflate(&strm, Z_FINISH);
		Buffer.insert(Buffer.end(), temp_buffer, temp_buffer + BUFSIZE - strm.avail_out);
		deflateEnd(&strm);
	}

	Vec.swap(Buffer);
	delete[] temp_buffer;
}

void insertValue(std::vector<unsigned char>& vec, ptrdiff_t valueInsertIndex, const size_t& VALUE, int bits) {

	while (bits) vec[valueInsertIndex++] = (VALUE >> (bits -= 8)) & 0xff;
}

void displayInfo() {

	std::cout << R"(
JPG Data Vehicle for Reddit, (jdvrdt v1.0). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023.

jdvrdt enables you to embed & extract arbitrary data of upto ~20MB within a single JPG image.

You can upload and share your data embedded JPG image file on Reddit or *Imgur.

*Imgur issue: When the embedded image size is over 5MB, the data is still retained, but Imgur will reduce the dimension size of your image.
 
jdvrdt data embedded images will not work with Twitter. For Twitter, please use pdvzip (PNG only).

This program works on Linux and Windows.

The file data is inserted and preserved within multiple 65KB ICC Profile chunks in the image file.
 
To maximise the amount of data you can embed in your image file. I recommend compressing your 
data file(s) to zip/rar formats, etc.

Using jdvrdt, You can insert up to five files at a time (outputs one image per file).

You can also extract files from up to five images at a time.
)";
}
