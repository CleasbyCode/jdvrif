#include "common.h"
#include "conceal.h"
#include "file_utils.h"
#include "program_args.h"
#include "recover.h"

#include <iostream>
#include <print>
#include <stdexcept>

namespace {
[[nodiscard]] int run(int argc, char** argv) {
    if (sodium_init() < 0) throw std::runtime_error("Libsodium initialization failed!");

    auto args_opt = ProgramArgs::parse(argc, argv);
    if (!args_opt) return 0;

    const auto& args = *args_opt;
    switch (args.mode) {
        case Mode::conceal: {
            vBytes jpg_vec = readFile(args.image_file_path, FileTypeCheck::cover_image);
            concealData(jpg_vec, args.option, args.data_file_path);
            return 0;
        }
        case Mode::recover:
            recoverData(args.image_file_path);
            return 0;
        default:
            throw std::runtime_error("Internal Error: Unsupported mode.");
    }
}
}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::println(std::cerr, "\n{}\n", e.what());
        return 1;
    } catch (...) {
        std::println(std::cerr, "\nUnknown fatal error.\n");
        return 1;
    }
}
