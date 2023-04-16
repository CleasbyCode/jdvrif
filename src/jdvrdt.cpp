//	JPG Data Vehicle for Reddit, (JDVRDT v1.0). Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include <algorithm>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <zlib.h>

// Open user image & data file and check file size requirements. Display error & exit program if any file fails to open or exceeds size limits.
void processFiles(char* [], int, int, const std::string&);

// Open jdvrdt jpg image file, then proceed to inflate/uncompress and extract embedded data file from it. 
void processEmbeddedImage(char* []);

// Read in and store jpg image & data file into vectors. Data file is deflate/compressed (zlib).
void readFilesIntoVectors(std::ifstream&, std::ifstream&, const std::string&, const std::string&, const ptrdiff_t&, const ptrdiff_t&, int, int);

// Insert updated values, such as chunk size into relevant vector index locations.
void insertValue(std::vector<unsigned char>&, ptrdiff_t, const size_t&, int);

// Inflate or Deflate iCC Profile chunk, which include user's data file.
void inflateDeflate(std::vector<unsigned char>&, bool);

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

// Inflate and extract embedded data file from jpg image.
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

	int profileSigIndex = 6;

	// Get iCC Profile chunk signature from vector.
	const std::string
		PROFILE_SIG = "ICC_PROFILE",
		PROFILE_CHECK{ ImageVec.begin() + profileSigIndex, ImageVec.begin() + profileSigIndex + PROFILE_SIG.length()};

	if (PROFILE_CHECK != PROFILE_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nImage Error: Image file \"" << IMAGE_FILE << "\" does not appear to contain a valid iCC Profile.\n\n";
		std::exit(EXIT_FAILURE);
	}

	int deflateDataIndex = 20;  // Start index location of deflate data within iCC Profile chunk.

	// From "ImageVec" vector index 0, erase bytes so that start of vector is now the beginning of the deflate data (78,DA...).
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + deflateDataIndex);

	std::vector<unsigned char> DQT_SIG_A{ 0xFF, 0xDB, 0x00, 0x43 };
	std::vector<unsigned char> DQT_SIG_B{ 0xFF, 0xDB, 0x00, 0x84 };
	
	std::cout << "\nSearching for embedded data file. Please wait...\n";
	
	// Within "ImageVec", find and erase all occurrences of the contents of "ProfileChunkVec".
	// Stop the search once we get to location of dqtIndex.
	ptrdiff_t 
		findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4,
		dqtFirstPosA = search(ImageVec.begin(), ImageVec.end(), DQT_SIG_A.begin(), DQT_SIG_A.end()) - ImageVec.begin(),
		dqtFirstPosB = search(ImageVec.begin(), ImageVec.end(), DQT_SIG_B.begin(), DQT_SIG_B.end()) - ImageVec.begin(),
		dqtIndex = 0;
	
	dqtIndex = dqtFirstPosB > dqtFirstPosA ? dqtFirstPosA : dqtFirstPosB;
	
	while (dqtIndex > findProfileSigIndex) {
		ImageVec.erase(ImageVec.begin() + findProfileSigIndex, ImageVec.begin() + findProfileSigIndex + 18);
		findProfileSigIndex = search(ImageVec.begin(), ImageVec.end(), PROFILE_SIG.begin(), PROFILE_SIG.end()) - ImageVec.begin() - 4;
	}
	
	dqtIndex = dqtIndex == dqtFirstPosA ? search(ImageVec.begin(), ImageVec.end(), DQT_SIG_A.begin(), DQT_SIG_A.end()) - ImageVec.begin() 
		: search(ImageVec.begin(), ImageVec.end(), DQT_SIG_B.begin(), DQT_SIG_B.end()) - ImageVec.begin();

	// Erase bytes starting at first occurrence of dqt until end of "ImageVec". Vector now contains just the deflate data (Basic profile + user data).
	ImageVec.erase(ImageVec.begin() + dqtFirstPos, ImageVec.end());

	bool inflate = true;

	// Call function to inflate "ImageVec" content, which includes user's data file.
	inflateDeflate(ImageVec, inflate);

	const std::string
		JDV_SIG = "JDV",
		JDV_CHECK{ ImageVec.begin() + 5, ImageVec.begin() + 5 + JDV_SIG.length() };	// Get JDVRDT signature from vector "ImageVec".
		
	// Make sure this is a jdvrdt file-embedded image.
	if (JDV_CHECK != JDV_SIG) {
		// File requirements check failure, display relevant error message and exit program.
		std::cerr << "\nProfile Error: iCC Profile does not seem to be a JDV modified profile.\n\n";
		std::exit(EXIT_FAILURE);
	}

	std::string fileName = { ImageVec.begin() + 13, ImageVec.begin() + 13 + ImageVec[12] };	// Get embedded filename from vector "ImageVec".
	if (fileName.substr(0, 4) != "jdv_") {
		fileName = "jdv_" + fileName;
	}

	// Erase the 132 byte basic ICC Profile at the start of the "ImageVec" vector.
	ImageVec.erase(ImageVec.begin(), ImageVec.begin() + 132);

	// Write data from vector "ImageVec" out to file.
	std::ofstream writeFile(fileName, std::ios::binary);

	if (!writeFile) {
		std::cerr << "\nWrite Error: Unable to write to file.\n\n";
		std::exit(EXIT_FAILURE);
	}

	writeFile.write((char*)&ImageVec[0], ImageVec.size());

	std::cout << "\nExtracted file: \"" + fileName + " "  << ImageVec.size() << " " << "Bytes\"\n";
}

void readFilesIntoVectors(std::ifstream& readImage, std::ifstream& readFile, const std::string& IMAGE_FILE, const std::string& DATA_FILE, const ptrdiff_t& IMAGE_SIZE, const ptrdiff_t& DATA_SIZE, int argc, int sub) {

	// Reset position of files. 
	readImage.seekg(0, readImage.beg),
	readFile.seekg(0, readFile.beg);

	// The first 132 bytes of this vector contains the basic profile.
	// Without this basic profile, some image display programs will show error messages when loading the image.
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

	ptrdiff_t
		profileNameLengthIndex = 12,	// Index location inside the iCC Profile to store the length value of the embedded data's filename.
		profileNameIndex = 13,		// Start index inside the iCC Profile to store the filename for the embedded data file.
		noSlashNameLength = noSlashName.length(); // Character length of filename for the embedded data file.

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

	ProfileVec.insert(ProfileVec.begin() + profileNameIndex, noSlashName.begin(), noSlashName.end());

	// Insert user data file at end of ProfileVec.
	ProfileVec.resize(DATA_SIZE + ProfileVec.size() / sizeof(unsigned char));
	readFile.read((char*)&ProfileVec[132], DATA_SIZE);

	bool inflate = false;  // Set to deflate

	// Call function to deflate/compress the contents of vector "ProfileVec" (Basic profile with user's data file).
	inflateDeflate(ProfileVec, inflate);

	const size_t DEFLATE_VECTOR_SIZE = ProfileVec.size() + ProfileFirstChunkVec.size(); // including 20 bytes of the ProfileFirstChunkVec.

	int bits = 16,
		profileChunkSize = 65535,
		profileFirstChunkSizeIndex = 4,
		profileChunkSizeIndex = 2,
		profileFirstByteCount = 4,	// 0xFF, 0xD8, 0xFF, 0xE2
		profileChunkByteCount = 2;	// 0xFF, 0xE2,

	if (profileChunkSize > DEFLATE_VECTOR_SIZE) {
		// Update profile chunk size, as it is smaller than the set default.
		insertValue(ProfileFirstChunkVec, profileFirstChunkSizeIndex, DEFLATE_VECTOR_SIZE - profileFirstByteCount, bits);
	}

	// Insert the content of "ProfileFirstChunkVec" into "ProfileVec".
	ProfileVec.insert(ProfileVec.begin(), ProfileFirstChunkVec.begin(), ProfileFirstChunkVec.end());

	// Insert "ProfileChunkVec" at every "profileChunkSize" (approx), or whatever data size under "profileChunkSize" remains, until end of file.
	if (DEFLATE_VECTOR_SIZE > profileChunkSize) {

		ProfileVec.insert(ProfileVec.begin() + profileChunkSize + profileFirstByteCount, ProfileChunkVec.begin(), ProfileChunkVec.end());

		int totalChunkSize = profileChunkSize + profileFirstByteCount + profileChunkByteCount;  // 65,535 + 4 + 2

		bool isTrue = true;

		while (isTrue) {
			totalChunkSize += profileChunkSize + profileChunkByteCount; // Tally...
			if (profileChunkSize > ProfileVec.size() - (totalChunkSize + profileChunkByteCount)) {
				// Data file size remaining is less than "profileChunkSize", so update profile chunk size, insert last "ProfileChunkVec" and exit the loop.
				insertValue(ProfileChunkVec, profileChunkSizeIndex, (ProfileVec.size() + ProfileChunkVec.size()) - (totalChunkSize + profileChunkByteCount), bits);
				ProfileVec.insert(ProfileVec.begin() + totalChunkSize, ProfileChunkVec.begin(), ProfileChunkVec.end());
				isTrue = false;
			}
			else {
				ProfileVec.insert(ProfileVec.begin() + totalChunkSize, ProfileChunkVec.begin(), ProfileChunkVec.end());
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

void inflateDeflate(std::vector<unsigned char>& Vec, bool inflateData) {

	// zlib function, see https://zlib.net/

	std::vector <unsigned char> Buffer;

	size_t BUFSIZE;

	if (!inflateData) {
		BUFSIZE = 1032 * 1024;  // Required for deflate. This BUFSIZE covers us to our max file size of 20MB. A lower BUFSIZE results in lost data after 1MB.
	}
	else {
		BUFSIZE = 256 * 1024;  // Fine for inflate.
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
		deflateInit(&strm, 9); // Compression level 6 (78, DA...)
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
