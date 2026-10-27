#pragma once

#include "common.h"

// Which word the prompt uses. Default and Bluesky images say up front whether
// they were made with a 128-bit recovery KEY (jdvrif v10+) or a numeric PIN
// (v9.x). Reddit / X-Twitter carriers cannot be read until after entry -- the
// key locates them -- so they get a prompt that names both.
// Errors raised while reading the entry use the same word.
enum class PinPrompt { key, pin, key_or_pin };

[[nodiscard]] constexpr const char* pinPromptWord(PinPrompt prompt) noexcept {
    switch (prompt) {
        case PinPrompt::key: return "KEY";
        case PinPrompt::pin: return "PIN";
        case PinPrompt::key_or_pin: break;
    }
    return "KEY/PIN";
}

[[nodiscard]] SecurePin getPin(PinPrompt prompt);
