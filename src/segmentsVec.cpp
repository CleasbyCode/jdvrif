
#include "segmentsVec.h"

// ICC color profile segment (FFE2). Default method for storing data file (in multiple segments, if required).
// Notes: 	Total segments value index = 0x2E0 (2 bytes)
//		Compressed data file size index = 0x2E2	(4 bytes)
//		Data filename length index = 0x2E6 (1 byte)
//		Data filename index = 0x2E7 (20 bytes)
//		Data filename XOR key index = 0x2FB (24 bytes)
//		Sodium key index = 0x313 (32 bytes)
//		Nonce key index = 0x333 (24 bytes)
//		jdvrif sig index = 0x34B (7 bytes)
//		Data file start index = 0x353 (see index 0x2E2 (4 bytes) for compressed data file size).
std::vector<uint8_t>segment_vec {
	0xFF, 0xD8, 0xFF, 0xE2, 0xFF, 0xFF, 0x49, 0x43, 0x43, 0x5F, 0x50, 0x52,
	0x4F, 0x46, 0x49, 0x4C, 0x45, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xEF,
	0x41, 0x44, 0x42, 0x45, 0x04, 0x20, 0x00, 0x00, 0x6D, 0x6E, 0x74, 0x72,
	0x52, 0x47, 0x42, 0x20, 0x58, 0x59, 0x5A, 0x20, 0x07, 0xE5, 0x00, 0x04,
	0x00, 0x1B, 0x00, 0x0A, 0x00, 0x1B, 0x00, 0x00, 0x61, 0x63, 0x73, 0x70,
	0x4D, 0x53, 0x46, 0x54, 0x00, 0x00, 0x00, 0x00, 0x73, 0x61, 0x77, 0x73,
	0x63, 0x74, 0x72, 0x6C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00,
	0x00, 0x00, 0xD3, 0x2D, 0x68, 0x61, 0x6E, 0x64, 0x40, 0x92, 0xFF, 0x1E,
	0x67, 0x34, 0xB5, 0x6D, 0x00, 0x1C, 0x4E, 0x36, 0x73, 0x3F, 0x4E, 0x71,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0x64, 0x65, 0x73, 0x63,
	0x00, 0x00, 0x00, 0xFC, 0x00, 0x00, 0x00, 0x24, 0x63, 0x70, 0x72, 0x74,
	0x00, 0x00, 0x01, 0x20, 0x00, 0x00, 0x00, 0x22, 0x77, 0x74, 0x70, 0x74,
	0x00, 0x00, 0x01, 0x44, 0x00, 0x00, 0x00, 0x14, 0x63, 0x68, 0x61, 0x64,
	0x00, 0x00, 0x01, 0x58, 0x00, 0x00, 0x00, 0x2C, 0x72, 0x58, 0x59, 0x5A,
	0x00, 0x00, 0x01, 0x84, 0x00, 0x00, 0x00, 0x14, 0x67, 0x58, 0x59, 0x5A,
	0x00, 0x00, 0x01, 0x98, 0x00, 0x00, 0x00, 0x14, 0x62, 0x58, 0x59, 0x5A,
	0x00, 0x00, 0x01, 0xAC, 0x00, 0x00, 0x00, 0x14, 0x72, 0x54, 0x52, 0x43,
	0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x20, 0x67, 0x54, 0x52, 0x43,
	0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x20, 0x62, 0x54, 0x52, 0x43,
	0x00, 0x00, 0x01, 0xC0, 0x00, 0x00, 0x00, 0x20, 0x6D, 0x6C, 0x75, 0x63,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C,
	0x65, 0x6E, 0x55, 0x53, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x1C,
	0x00, 0x41, 0x00, 0x39, 0x00, 0x38, 0x00, 0x43, 0x6D, 0x6C, 0x75, 0x63,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x0C,
	0x65, 0x6E, 0x55, 0x53, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x1C,
	0x00, 0x43, 0x00, 0x43, 0x00, 0x30, 0x00, 0x00, 0x58, 0x59, 0x5A, 0x20,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xD6, 0x00, 0x01, 0x00, 0x00,
	0x00, 0x00, 0xD3, 0x2D, 0x73, 0x66, 0x33, 0x32, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x0C, 0x42, 0x00, 0x00, 0x05, 0xDE, 0xFF, 0xFF, 0xF3, 0x25,
	0x00, 0x00, 0x07, 0x93, 0x00, 0x00, 0xFD, 0x90, 0xFF, 0xFF, 0xFB, 0xA1,
	0xFF, 0xFF, 0xFD, 0xA2, 0x00, 0x00, 0x03, 0xDC, 0x00, 0x00, 0xC0, 0x6E,
	0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x18,
	0x00, 0x00, 0x4F, 0xA5, 0x00, 0x00, 0x04, 0xFC, 0x58, 0x59, 0x5A, 0x20,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x8D, 0x00, 0x00, 0xA0, 0x2C,
	0x00, 0x00, 0x0F, 0x95, 0x58, 0x59, 0x5A, 0x20, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x26, 0x31, 0x00, 0x00, 0x10, 0x2F, 0x00, 0x00, 0xBE, 0x9C,
	0x70, 0x61, 0x72, 0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00,
	0x00, 0x02, 0x33, 0x33, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x0E, 0x41, 0xFF, 0xDB, 0x00, 0x43,
	0x00, 0x05, 0x03, 0x04, 0x04, 0x04, 0x03, 0x05, 0x04, 0x04, 0x04, 0x05,
	0x05, 0x05, 0x06, 0x07, 0x0C, 0x08, 0x07, 0x07, 0x07, 0x07, 0x0F, 0x0B,
	0x0B, 0x09, 0x0C, 0x11, 0x0F, 0x12, 0x12, 0x11, 0x0F, 0x11, 0x11, 0x13,
	0x16, 0x1C, 0x17, 0x13, 0x14, 0x1A, 0x15, 0x11, 0x11, 0x18, 0x21, 0x18,
	0x1A, 0x1D, 0x1D, 0x1F, 0x1F, 0x1F, 0x13, 0x17, 0x22, 0x24, 0x22, 0x1E,
	0x24, 0x1C, 0x1E, 0x1F, 0x1E, 0xFF, 0xDB, 0x00, 0x43, 0x01, 0x05, 0x05,
	0x05, 0x07, 0x06, 0x07, 0x0E, 0x08, 0x08, 0x0E, 0x1E, 0x14, 0x11, 0x14,
	0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
	0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
	0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
	0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E, 0x1E,
	0x1E, 0x1E, 0xFF, 0xC2, 0x00, 0x11, 0x08, 0x04, 0x00, 0x04, 0x00, 0x03,
	0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00,
	0x1C, 0x00, 0x00, 0x02, 0x03, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x01, 0x04, 0x05,
	0x06, 0x07, 0x08, 0xFF, 0xC4, 0x00, 0x1B, 0x01, 0x00, 0x03, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xFF, 0xDA, 0x00, 0x0C,
	0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10, 0x00, 0x00, 0x01, 0xF0, 0x43,
	0x6B, 0x20, 0x42, 0xC4, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xCA,
	0xFD, 0x64, 0xC2, 0x21, 0x63, 0x7B, 0x37, 0xE5, 0x4E, 0xEC, 0xC7, 0xDE,
	0x2B, 0x97, 0x48, 0x8C, 0x7A, 0x24, 0x65, 0x88, 0x94, 0x10, 0xB6, 0x44,
	0x11, 0x24, 0x7B, 0x80, 0x3D, 0x9F, 0xA8, 0xB0, 0x05, 0xE3, 0x30, 0xF8,
	0xFD, 0xEC, 0x50, 0xF9, 0x9B, 0xDD, 0x9E, 0xB6, 0xFE, 0xBC, 0x93, 0xAD,
	0xE1, 0xFD, 0x39, 0xB2, 0x7F, 0x6F, 0x74, 0xAB, 0x7E, 0x4B, 0x36, 0xFC,
	0xF2, 0xA8, 0x2B, 0xD0, 0x00, 0x04, 0x28, 0x43, 0xF3, 0x0A, 0xEF, 0x76,
	0xEE, 0xAC, 0x08, 0x71, 0xBB, 0xAA, 0x77, 0xB7, 0xB1, 0x50, 0x0F, 0xB1,
	0x9B, 0x34, 0xB3, 0x29, 0x12, 0x15, 0x51, 0x64, 0x85, 0xF7, 0x91, 0x9A,
	0xFD, 0xD5, 0x82, 0xB4, 0x6A, 0x3E, 0xEA, 0x5E, 0x9D, 0xF9, 0x90	
};

// EXIF (FFE1) segment. This is the way we store the data file when user selects the -b option switch for Bluesky platform. 
// Notes: 	Total segments value index = N/A
//		Compressed data file size index = 0x1CD	(4 bytes)
//		Data filename length index = 0x160 (1 byte)
//		Data filename index = 0x161 (20 bytes)
//		Data filename XOR key index = 0x175 (24 bytes)
//		Sodium key index = 0x18D (32 bytes)
//		Nonce key index = 0x1AD (24 bytes)
//		jdvrif sig index = 0x1C5 (7 bytes)
//		Data file start index = 0x1D1 (see index 0x1CD (4 bytes) for compressed data file size).
std::vector<uint8_t>bluesky_exif_vec {
	0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x00, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x4D, 0x4D, 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x06, 0x01, 0x12,
	0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x1A, 0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x1B,
	0x00, 0x05, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x28, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x01, 0x3B,
	0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x87, 0x69, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0xFF, 0xDB, 0x00, 0x43, 0x00, 0x03, 0x02, 0x02, 0x03, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x03, 0x03, 0x04, 0x05, 0x08, 0x05,
	0x05, 0x04, 0x04, 0x05, 0x0A, 0x07, 0x07, 0x06, 0x08, 0x0C, 0x0A, 0x0C, 0x0C, 0x0B, 0x0A, 0x0B, 0x0B, 0x0D, 0x0E, 0x12, 0x10, 0x0D, 0x0E, 0x11,
	0x0E, 0x0B, 0x0B, 0x10, 0x16, 0x10, 0x11, 0x13, 0x14, 0x15, 0x15, 0x15, 0x0C, 0x0F, 0x17, 0x18, 0x16, 0x14, 0x18, 0x12, 0x14, 0x15, 0x14, 0xFF,
	0xDB, 0x00, 0x43, 0x01, 0x03, 0x04, 0x04, 0x05, 0x04, 0x05, 0x09, 0x05, 0x05, 0x09, 0x14, 0x0D, 0x0B, 0x0D, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
	0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14,
	0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0x14, 0xFF, 0xC2, 0x00, 0x11,
	0x08, 0x04, 0x00, 0x04, 0x00, 0x03, 0x01, 0x22, 0x00, 0x02, 0x11, 0x01, 0x03, 0x11, 0x01, 0xFF, 0xC4, 0x00, 0x1C, 0x00, 0x00, 0x02, 0x03, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x04, 0x01, 0x02, 0x05, 0x00, 0x06, 0x07, 0x08, 0xFF, 0xC4, 0x00,
	0x1B, 0x01, 0x00, 0x02, 0x03, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x00, 0x04, 0x05, 0x01,
	0x06, 0x07, 0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x10, 0x03, 0x10, 0x00, 0x00, 0x01, 0xF8, 0x29, 0xC6, 0x7A, 0x1F, 0x49, 0x1A, 0x8E,
	0xAB, 0x38, 0xA0, 0x8C, 0x26, 0x63, 0x8E, 0x97, 0x83, 0xA9, 0xD3, 0xD7, 0x12, 0xEB, 0x75, 0xF8, 0x00, 0x6E, 0x96, 0xF9, 0xB0, 0x46, 0xDE, 0x8B,
	0x7A, 0x82, 0x60, 0x43, 0xAD, 0xA0, 0x62, 0x68, 0xCC, 0xD0, 0xA8, 0xA1, 0x0D, 0x01, 0x59, 0x85, 0xE1, 0xA3, 0x52, 0x57, 0x3A, 0x92, 0x41, 0x84,
	0x5C, 0x3C, 0xF4, 0xFF, 0x82, 0xCA, 0x30, 0x79, 0x0E, 0x97, 0xA0, 0x36, 0xB0, 0x7D, 0x72, 0x92, 0x6A, 0x31, 0xD0, 0x09, 0x0A, 0x77, 0x90, 0xA7,
	0x36, 0x66, 0xA0, 0x26, 0xAB, 0xBC, 0xDF, 0x61, 0xFE, 0xEE, 0xD9, 0x46, 0x70, 0xD7, 0xB0, 0x79, 0xF5, 0xA5, 0x29, 0xD8, 0xAB, 0x1F, 0x58, 0xE2,
	0xAF, 0xA8, 0x1F, 0x00, 0xDB, 0x32, 0x1F, 0xC2, 0xFD, 0x55, 0x21, 0x27, 0x3A, 0x6A, 0xBE, 0x1D, 0xB8, 0x5F, 0x60, 0x38, 0x99, 0xB4, 0x6A, 0x3E,
	0xEA, 0x5E, 0x9D, 0xF9, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x48, 0x00, 0x00, 0x00,
	0x01, 0x00, 0x02, 0xA0, 0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x06, 0x00, 0xA0, 0x03, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00,
	0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00
};

// XMP (FFE1) segment.
// Notes: 	Data file index = 0x139 (remainder part of data file that is stored in the EXIF segment above. Data file content stored here as BASE64).
std::vector<uint8_t>bluesky_xmp_vec {
	0xFF, 0xE1, 0x01, 0x93, 0x68, 0x74, 0x74, 0x70, 0x3A, 0x2F, 0x2F, 0x6E,
	0x73, 0x2E, 0x61, 0x64, 0x6F, 0x62, 0x65, 0x2E, 0x63, 0x6F, 0x6D, 0x2F,
	0x78, 0x61, 0x70, 0x2F, 0x31, 0x2E, 0x30, 0x2F, 0x00, 0x3C, 0x3F, 0x78,
	0x70, 0x61, 0x63, 0x6B, 0x65, 0x74, 0x20, 0x62, 0x65, 0x67, 0x69, 0x6E,
	0x3D, 0x22, 0x22, 0x20, 0x69, 0x64, 0x3D, 0x22, 0x57, 0x35, 0x4D, 0x30,
	0x4D, 0x70, 0x43, 0x65, 0x68, 0x69, 0x48, 0x7A, 0x72, 0x65, 0x53, 0x7A,
	0x4E, 0x54, 0x63, 0x7A, 0x6B, 0x63, 0x39, 0x64, 0x22, 0x3F, 0x3E, 0x0A,
	0x3C, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61, 0x20, 0x78,
	0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x78, 0x3D, 0x22, 0x61, 0x64, 0x6F, 0x62,
	0x65, 0x3A, 0x6E, 0x73, 0x3A, 0x6D, 0x65, 0x74, 0x61, 0x2F, 0x22, 0x20,
	0x78, 0x3A, 0x78, 0x6D, 0x70, 0x74, 0x6B, 0x3D, 0x22, 0x47, 0x6F, 0x20,
	0x58, 0x4D, 0x50, 0x20, 0x53, 0x44, 0x4B, 0x20, 0x31, 0x2E, 0x30, 0x22,
	0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46, 0x20, 0x78, 0x6D,
	0x6C, 0x6E, 0x73, 0x3A, 0x72, 0x64, 0x66, 0x3D, 0x22, 0x68, 0x74, 0x74,
	0x70, 0x3A, 0x2F, 0x2F, 0x77, 0x77, 0x77, 0x2E, 0x77, 0x33, 0x2E, 0x6F,
	0x72, 0x67, 0x2F, 0x31, 0x39, 0x39, 0x39, 0x2F, 0x30, 0x32, 0x2F, 0x32,
	0x32, 0x2D, 0x72, 0x64, 0x66, 0x2D, 0x73, 0x79, 0x6E, 0x74, 0x61, 0x78,
	0x2D, 0x6E, 0x73, 0x23, 0x22, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x44,
	0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69, 0x6F, 0x6E, 0x20, 0x78,
	0x6D, 0x6C, 0x6E, 0x73, 0x3A, 0x64, 0x63, 0x3D, 0x22, 0x68, 0x74, 0x74,
	0x70, 0x3A, 0x2F, 0x2F, 0x70, 0x75, 0x72, 0x6C, 0x2E, 0x6F, 0x72, 0x67,
	0x2F, 0x64, 0x63, 0x2F, 0x65, 0x6C, 0x65, 0x6D, 0x65, 0x6E, 0x74, 0x73,
	0x2F, 0x31, 0x2E, 0x31, 0x2F, 0x22, 0x20, 0x72, 0x64, 0x66, 0x3A, 0x61,
	0x62, 0x6F, 0x75, 0x74, 0x3D, 0x22, 0x22, 0x3E, 0x3C, 0x64, 0x63, 0x3A,
	0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 0x3C, 0x72, 0x64, 0x66,
	0x3A, 0x53, 0x65, 0x71, 0x3E, 0x3C, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69,
	0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x6C, 0x69, 0x3E, 0x3C, 0x2F,
	0x72, 0x64, 0x66, 0x3A, 0x53, 0x65, 0x71, 0x3E, 0x3C, 0x2F, 0x64, 0x63,
	0x3A, 0x63, 0x72, 0x65, 0x61, 0x74, 0x6F, 0x72, 0x3E, 0x3C, 0x2F, 0x72,
	0x64, 0x66, 0x3A, 0x44, 0x65, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x69,
	0x6F, 0x6E, 0x3E, 0x3C, 0x2F, 0x72, 0x64, 0x66, 0x3A, 0x52, 0x44, 0x46,
	0x3E, 0x3C, 0x2F, 0x78, 0x3A, 0x78, 0x6D, 0x70, 0x6D, 0x65, 0x74, 0x61,
	0x3E, 0x0A, 0x3C, 0x3F, 0x78, 0x70, 0x61, 0x63, 0x6B, 0x65, 0x74, 0x20,
	0x65, 0x6E, 0x64, 0x3D, 0x22, 0x77, 0x22, 0x3F, 0x3E
};

std::vector<std::string> platforms_vec { 
	"X-Twitter", 
	"Tumblr", 
	"Bluesky. (Only share this \"file-embedded\" JPG image on Bluesky).\n\n You must use the Python script \"bsky_post.py\" (found in the repo src folder)\n to post the image to Bluesky.", 
	"Mastodon", 
	"Reddit. (Only share this \"file-embedded\" JPG image on Reddit).",
	"PostImage", 
	"ImgBB",
	"ImgPile", 
	"Flickr", 
	};

