#include "program_args.h"

#include <format>
#include <print>
#include <stdexcept>
#include <string_view>

namespace {
constexpr std::string_view INFO_TEXT =
    "\n\nJPG Data Vehicle (jdvrif v7.8)\nCreated by Nicholas Cleasby (@CleasbyCode) 10/04/2023\n\n"
    "jdvrif is a metadata \"steganography-like\" command-line tool used for concealing and extracting\n"
    "any file type within and from a JPG image.\n\n"
    "──────────────────────────\nCompile & run (Linux)\n──────────────────────────\n\n"
    "  Note: Compiler support for C++23 required.\n\n"
    "  $ sudo apt install libsodium-dev libturbojpeg0-dev\n\n"
    "  $ chmod +x compile_jdvrif.sh\n  $ ./compile_jdvrif.sh\n\n"
    "  Compilation successful. Executable 'jdvrif' created.\n\n"
    "  $ sudo cp jdvrif /usr/bin\n  $ jdvrif\n\n"
    "──────────────────────────\nUsage\n──────────────────────────\n\n"
    "  jdvrif conceal [-b|-r] <cover_image> <secret_file>\n  jdvrif recover <cover_image>\n  jdvrif --info\n\n"
    "──────────────────────────\nPlatform compatibility & size limits\n──────────────────────────\n\n"
    "Share your \"file-embedded\" JPG image on the following compatible sites.\n\n"
    "Platforms where size limit is measured by the combined size of cover image + compressed data file:\n\n"
    "\t• Flickr    (200 MB)\n\t• ImgPile   (100 MB)\n\t• ImgBB     (32 MB)\n\t• PostImage (32 MB)\n\t• Reddit    (20 MB) — (use -r option).\n\t• Pixelfed  (15 MB)\n\n"
    "Limit measured by compressed data file size only:\n\n"
    "\t• Mastodon  (~6 MB)\n\t• Tumblr    (~64 KB)\n\t• X-Twitter (~10 KB)\n\n"
    "For example, on Mastodon, even if your cover image is 1 MB, you can still embed a data file\n"
    "up to the ~6 MB Mastodon size limit.\n\n"
    "Other:\n\n"
    "Bluesky - Separate size limits for cover image and data file - (use -b option).\n"
    "  • Cover image: 800 KB\n  • Secret data file (compressed): ~171 KB\n\n"
    "Even though jdvrif compresses the data file, you may want to compress it yourself first\n"
    "(zip, rar, 7z, etc.) so that you know the exact compressed file size.\n\n"
    "Platforms with small size limits, like X-Twitter (~10 KB), are best suited for data that\n"
    "compress especially well, such as text files.\n\n"
    "──────────────────────────\nModes\n──────────────────────────\n\n"
    "conceal - *Compresses, encrypts and embeds your secret data file within a JPG cover image.\n"
    "recover - Decrypts, uncompresses and extracts the concealed data file from a JPG cover image\n"
    "          (recovery PIN required).\n\n"
    "(*Compression: If data file is already a compressed file type (based on file extension: e.g. \".zip\")\n"
    " and the file is greater than 10MB, skip compression).\n\n"
    "──────────────────────────\nPlatform options for conceal mode\n──────────────────────────\n\n"
    "-b (Bluesky) : Creates compatible \"file-embedded\" JPG images for posting on Bluesky.\n\n"
    "$ jdvrif conceal -b my_image.jpg hidden.doc\n\n"
    "These images are only compatible for posting on Bluesky.\n\n"
    "You must use the Python script \"bsky_post.py\" (in the repo's src folder) to post to Bluesky.\n"
    "Posting via the Bluesky website or mobile app will NOT work.\n\n"
    "You also need to create an app password for your Bluesky account: https://bsky.app/settings/app-passwords\n\n"
    "Here are some basic usage examples for the bsky_post.py Python script:\n\n"
    "Standard image post to your profile/account.\n\n"
    "$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx\n"
    "--image your_image.jpg --alt-text \"alt-text here [optional]\" \"standard post text here [required]\"\n\n"
    "If you want to post multiple images (Max. 4):\n\n"
    "$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx\n"
    "--image img1.jpg --image img2.jpg --alt-text \"alt_here\" \"standard post text...\"\n\n"
    "If you want to post an image as a reply to another thread:\n\n"
    "$ python3 bsky_post.py --handle you.bsky.social --password xxxx-xxxx-xxxx-xxxx\n"
    "--image your_image.jpg --alt-text \"alt_here\"\n"
    "--reply-to https://bsky.app/profile/someone.bsky.social/post/8m2tgw6cgi23i\n"
    "\"standard post text...\"\n\n"
    "Bluesky size limits: Cover 800 KB / Secret data file (compressed) ~171 KB\n\n"
    "-r (Reddit) : Creates compatible \"file-embedded\" JPG images for posting on Reddit.\n\n"
    "$ jdvrif conceal -r my_image.jpg secret.mp3\n\n"
    "From the Reddit site, click \"Create Post\", then select the \"Images & Video\" tab to attach the JPG image.\n"
    "These images are only compatible for posting on Reddit.\n\n"
    "To correctly download images from X-Twitter or Reddit, click image within the post to fully expand it before saving.\n\n";
}

void displayInfo() { std::print("{}", INFO_TEXT); }

std::optional<ProgramArgs> ProgramArgs::parse(int argc, char** argv) {
    const auto arg = [&](int i) -> std::string_view { return (i >= 0 && i < argc) ? std::string_view(argv[i]) : std::string_view{}; };

    constexpr std::string_view PREFIX = "Usage: ";
    const std::string PROG = fs::path(argv[0]).filename().string(), INDENT(PREFIX.size(), ' '), USAGE = std::format(
        "{0}{1} conceal [-b|-r] <cover_image> <secret_file>\n"
        "{2}{1} recover <cover_image>\n"
        "{2}{1} --info",
        PREFIX, PROG, INDENT);

    if (argc < 2) die(USAGE);

    if (argc == 2 && arg(1) == "--info") { displayInfo(); return std::nullopt; }

    ProgramArgs out;
    const std::string_view MODE = arg(1);

    if (MODE == "conceal") {
        int i = 2;

        if (arg(i) == "-b") { out.option = Option::Bluesky; ++i; }
        else if (arg(i) == "-r") { out.option = Option::Reddit; ++i; }

        if (argc != i + 2) die(USAGE);

        out.image_file_path = arg(i); out.data_file_path = arg(i + 1);
        return out;
    }

    if (MODE == "recover") {
        if (argc != 3) die(USAGE);
        out.mode = Mode::recover; out.image_file_path = arg(2);
        return out;
    }
    die(USAGE);
}

[[noreturn]] void ProgramArgs::die(const std::string& message) { throw std::runtime_error(message); }
