#pragma once

#include "common.h"

#include <format>
#include <optional>
#include <print>
#include <stdexcept>
#include <string>

inline void displayInfo() {
    std::print(R"(

JPG Data Vehicle (jdvrif v7.5)
Created by Nicholas Cleasby (@CleasbyCode) 10/04/2023

jdvrif is a metadata "steganography-like" command-line tool used for concealing and extracting
any file type within and from a JPG image.

──────────────────────────
Compile & run (Linux)
──────────────────────────

  Note: Compiler support for C++23 required.

  $ sudo apt install libsodium-dev libturbojpeg0-dev

  $ chmod +x compile_jdvrif.sh
  $ ./compile_jdvrif.sh

  Compilation successful. Executable 'jdvrif' created.

  $ sudo cp jdvrif /usr/bin
  $ jdvrif

──────────────────────────
Usage
──────────────────────────

  jdvrif conceal [-b|-r] <cover_image> <secret_file>
  jdvrif recover <cover_image>
  jdvrif --info

──────────────────────────
Platform compatibility & size limits
──────────────────────────

Share your "file-embedded" JPG image on the following compatible sites.

Platforms where size limit is measured by the combined size of cover image + compressed data file:

	• Flickr    (200 MB)
	• ImgPile   (100 MB)
	• ImgBB     (32 MB)
	• PostImage (32 MB)
	• Reddit    (20 MB) — (use -r option).
	• Pixelfed  (15 MB)

Limit measured by compressed data file size only:

	• Mastodon  (~6 MB)
	• Tumblr    (~64 KB)
	• X-Twitter (~10 KB)

For example, on Mastodon, even if your cover image is 1 MB, you can still embed a data file
up to the ~6 MB Mastodon size limit.

Other:

Bluesky - Separate size limits for cover image and data file - (use -b option).
  • Cover image: 800 KB
  • Secret data file (compressed): ~171 KB

Even though jdvrif compresses the data file, you may want to compress it yourself first
(zip, rar, 7z, etc.) so that you know the exact compressed file size.

Platforms with small size limits, like X-Twitter (~10 KB), are best suited for data that
compress especially well, such as text files.

──────────────────────────
Modes
──────────────────────────

conceal - *Compresses, encrypts and embeds your secret data file within a JPG cover image.
recover - Decrypts, uncompresses and extracts the concealed data file from a JPG cover image
          (recovery PIN required).

(*Compression: If data file is already a compressed file type (based on file extension: e.g. ".zip")
 and the file is greater than 10MB, skip compression).

──────────────────────────
Platform options for conceal mode
──────────────────────────

-b (Bluesky) : Creates compatible "file-embedded" JPG images for posting on Bluesky.

$ jdvrif conceal -b my_image.jpg hidden.doc

These images are only compatible for posting on Bluesky.

You must use the Python script "bsky_post.py" (in the repo's src folder) to post to Bluesky.
Posting via the Bluesky website or mobile app will NOT work.

You also need to create an app password for your Bluesky account: https://bsky.app/settings/app-passwords

Here are some basic usage examples for the bsky_post.py Python script:

Standard image post to your profile/account.

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx
--image your_image.jpg --alt-text "alt-text here [optional]" "standard post text here [required]"

If you want to post multiple images (Max. 4):

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx
--image img1.jpg --image img2.jpg --alt-text "alt_here" "standard post text..."

If you want to post an image as a reply to another thread:

$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx
--image your_image.jpg --alt-text "alt_here"
--reply-to https://bsky.app/profile/someone.bsky.social/post/8m2tgw6cgi23i
"standard post text..."

Bluesky size limits: Cover 800 KB / Secret data file (compressed) ~171 KB

-r (Reddit) : Creates compatible "file-embedded" JPG images for posting on Reddit.

$ jdvrif conceal -r my_image.jpg secret.mp3

From the Reddit site, click "Create Post", then select the "Images & Video" tab to attach the JPG image.
These images are only compatible for posting on Reddit.

To correctly download images from X-Twitter or Reddit, click image within the post to fully expand it before saving.

    )");
}

struct ProgramArgs {
    Mode mode{Mode::conceal};
    Option option{Option::None};
    fs::path image_file_path;
    fs::path data_file_path;

    static std::optional<ProgramArgs> parse(int argc, char** argv) {
        const auto arg = [&](int i) -> std::string_view {
            return (i >= 0 && i < argc) ? std::string_view(argv[i]) : std::string_view{};
        };

        constexpr std::string_view PREFIX = "Usage: ";
        const std::string
            PROG = fs::path(argv[0]).filename().string(),
            INDENT(PREFIX.size(), ' '),
            USAGE = std::format(
                "{0}{1} conceal [-b|-r] <cover_image> <secret_file>\n"
                "{2}{1} recover <cover_image>\n"
                "{2}{1} --info",
                PREFIX, PROG, INDENT
            );

        if (argc < 2)
            die(USAGE);

        if (argc == 2 && arg(1) == "--info") {
            displayInfo();
            return std::nullopt;
        }

        ProgramArgs out;
        const std::string_view MODE = arg(1);

        if (MODE == "conceal") {
            int i = 2;

            if (arg(i) == "-b") {
                out.option = Option::Bluesky;
                ++i;
            } else if (arg(i) == "-r") {
                out.option = Option::Reddit;
                ++i;
            }

            if (argc != i + 2)
                die(USAGE);

            out.image_file_path = arg(i);
            out.data_file_path  = arg(i + 1);
            return out;
        }

        if (MODE == "recover") {
            if (argc != 3)
                die(USAGE);

            out.mode = Mode::recover;
            out.image_file_path = arg(2);
            return out;
        }
        die(USAGE);
    }

private:
    [[noreturn]] static void die(const std::string& message) {
        throw std::runtime_error(message);
    }
};
