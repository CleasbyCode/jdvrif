// This software is based in part on the work of the Independent JPEG Group.
// Copyright (C) 2009-2024 D. R. Commander. All Rights Reserved.
// Copyright (C) 2015 Viktor Szathmáry. All Rights Reserved.
// https://github.com/libjpeg-turbo/libjpeg-turbo

#include "transcodeImage.h"
#include <turbojpeg.h>
#include <stdexcept>
#include <string>

void transcodeImage(std::vector<uint8_t>& image_vec, bool hasBlueskyOption) {
    tjhandle decompressor = tjInitDecompress();
    if (!decompressor) {
        throw std::runtime_error("tjInitDecompress() failed.");
    }

    int width = 0, height = 0, jpegSubsamp = 0, jpegColorspace = 0;
    if (tjDecompressHeader3(decompressor, image_vec.data(), static_cast<unsigned long>(image_vec.size()), 
                            &width, &height, &jpegSubsamp, &jpegColorspace) != 0) {
        tjDestroy(decompressor);
        throw std::runtime_error(std::string("tjDecompressHeader3: ") + tjGetErrorStr());
    }

    std::vector<uint8_t> decoded_image_vec(width * height * 3); 
    if (tjDecompress2(decompressor, image_vec.data(),static_cast<unsigned long>(image_vec.size()), 
                      decoded_image_vec.data(), width, 0, height, TJPF_RGB, 0) != 0) {
        tjDestroy(decompressor);
        throw std::runtime_error(std::string("tjDecompress2: ") + tjGetErrorStr());
    }
    tjDestroy(decompressor);

    tjhandle compressor = tjInitCompress();
    if (!compressor) {
        throw std::runtime_error("tjInitCompress() failed.");
    }

    const uint8_t JPG_QUALITY_VAL = hasBlueskyOption ? 85 : 97;

    unsigned char* jpegBuf = nullptr;
    unsigned long jpegSize = 0;

    int flags = hasBlueskyOption ? TJFLAG_ACCURATEDCT : TJFLAG_PROGRESSIVE | TJFLAG_ACCURATEDCT;

    if (tjCompress2(compressor, decoded_image_vec.data(), width, 0, height, TJPF_RGB, 
                    &jpegBuf, &jpegSize, TJSAMP_444, JPG_QUALITY_VAL, flags) != 0) {
        tjDestroy(compressor);
        throw std::runtime_error(std::string("tjCompress2: ") + tjGetErrorStr());
    }
    tjDestroy(compressor);

    std::vector<uint8_t> output_image_vec(jpegBuf, jpegBuf + jpegSize);
    tjFree(jpegBuf);
    image_vec.swap(output_image_vec);
}
