#include "file_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <ranges>
#include <stdexcept>

bool hasValidFilename(const fs::path& p) {
    if (p.empty()) {
        return false;
    }
    std::string filename = p.filename().string();
    if (filename.empty()) {
        return false;
    }
    auto validChar = [](unsigned char c) {
        return std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == '@' || c == '%';
    };
    return std::ranges::all_of(filename, validChar);
}

bool hasFileExtension(const fs::path& p, std::initializer_list<std::string_view> exts) {
    auto e = p.extension().string();
    std::ranges::transform(e, e.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return std::ranges::any_of(exts, [&e](std::string_view ext) {
        return e == ext;
    });
}

vBytes readFile(const fs::path& path, FileTypeCheck FileType) {
    if (!hasValidFilename(path)) {
        throw std::runtime_error("Invalid Input Error: Unsupported characters in filename arguments.");
    }

    if (!fs::exists(path) || !fs::is_regular_file(path)) {
        throw std::runtime_error(std::format("Error: File \"{}\" not found or not a regular file.", path.string()));
    }

    std::size_t file_size = fs::file_size(path);

    if (!file_size) {
        throw std::runtime_error("Error: File is empty.");
    }

    if (FileType == FileTypeCheck::cover_image || FileType == FileTypeCheck::embedded_image) {
        if (!hasFileExtension(path, {".png", ".jpg", ".jpeg", ".jfif"})) {
            throw std::runtime_error("File Type Error: Invalid image extension. Only expecting \".jpg\", \".jpeg\", \".jfif\" or \".png\".");
        }

        if (FileType == FileTypeCheck::cover_image) {
            constexpr std::size_t MINIMUM_IMAGE_SIZE = 134;

            if (MINIMUM_IMAGE_SIZE > file_size) {
                throw std::runtime_error("File Error: Invalid image file size.");
            }

            constexpr std::size_t MAX_IMAGE_SIZE = 8 * 1024 * 1024;

            if (file_size > MAX_IMAGE_SIZE) {
                throw std::runtime_error("Image File Error: Cover image file exceeds maximum size limit.");
            }
        }
    }

    constexpr std::size_t MAX_FILE_SIZE = 3ULL * 1024 * 1024 * 1024;

    if (file_size > MAX_FILE_SIZE) {
        throw std::runtime_error("Error: File exceeds program size limit.");
    }

    std::ifstream file(path, std::ios::binary);

    if (!file) {
        throw std::runtime_error(std::format("Failed to open file: {}", path.string()));
    }

    vBytes vec(file_size);
    file.read(reinterpret_cast<char*>(vec.data()), static_cast<std::streamsize>(file_size));

    if (file.gcount() != static_cast<std::streamsize>(file_size)) {
        throw std::runtime_error("Failed to read full file: partial read");
    }
    return vec;
}
