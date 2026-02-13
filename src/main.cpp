// JPG Data Vehicle (jdvrif v7.5) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include "common.h"
#include "conceal.h"
#include "file_utils.h"
#include "program_args.h"
#include "recover.h"

#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    try {
        if (sodium_init() < 0) {
            throw std::runtime_error("Libsodium initialization failed!");
        }

        #ifdef _WIN32
            SetConsoleOutputCP(CP_UTF8);
        #endif

        auto args_opt = ProgramArgs::parse(argc, argv);
        if (!args_opt) return 0;

        auto& args = *args_opt;

        vBytes jpg_vec = readFile(args.image_file_path, args.mode == Mode::conceal ? FileTypeCheck::cover_image : FileTypeCheck::embedded_image);

        if (args.mode == Mode::conceal) {
            concealData(jpg_vec, args.mode, args.option, args.data_file_path);
        } else {
            recoverData(jpg_vec, args.mode, args.image_file_path);
        }
    }
    catch (const std::exception& e) {
        std::println(std::cerr, "\n{}\n", e.what());
        return 1;
	}
    return 0;
}
