#pragma once

#include "common.h"
#include "reddit_steg.h"
#include "twitter_steg.h"

#include <cstddef>

void recoverFromIccPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t icc_profile_sig_index);

void recoverFromBlueskyPath(
    const fs::path& image_file_path,
    std::size_t image_file_size,
    std::size_t jdvrif_sig_index);

void recoverFromRedditPath(RedditEncryptedEnvelope envelope, SecurePin recovery_pin);
void recoverFromTwitterPath(TwitterEncryptedEnvelope envelope, SecurePin recovery_pin);
