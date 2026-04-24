#pragma once

#include "common.h"

#include <optional>
#include <string>

void displayInfo();

struct ProgramArgs {
    Mode mode{Mode::conceal}; Option option{Option::None}; fs::path image_file_path, data_file_path;
    static std::optional<ProgramArgs> parse(int argc, char** argv);
private:
    [[noreturn]] static void die(const std::string& message);
};
