// JPG Data Vehicle (jdvrif v7.5) Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

#include "common.h"
#include "conceal.h"
#include "file_utils.h"
#include "program_args.h"
#include "recover.h"

#include <iostream>
#include <print>
#include <stdexcept>

namespace {
void initializeCrypto() {
    if (sodium_init() < 0) {
        throw std::runtime_error("Libsodium initialization failed!");
    }
}

void runConceal(const ProgramArgs& args) {
    vBytes jpg_vec = readFile(args.image_file_path, FileTypeCheck::cover_image);
    concealData(jpg_vec, args.option, args.data_file_path);
}

void runRecover(const ProgramArgs& args) {
    recoverData(args.image_file_path);
}

void runMode(const ProgramArgs& args) {
    switch (args.mode) {
        case Mode::conceal:
            runConceal(args);
            return;
        case Mode::recover:
            runRecover(args);
            return;
        default:
            throw std::runtime_error("Internal Error: Unsupported mode.");
    }
}

[[nodiscard]] int run(int argc, char** argv) {
    initializeCrypto();

    auto args_opt = ProgramArgs::parse(argc, argv);
    if (!args_opt) {
        return 0;
    }

    runMode(*args_opt);
    return 0;
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
