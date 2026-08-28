#include "program_args.h"

#include <format>
#include <print>
#include <stdexcept>
#include <string_view>

namespace {
constexpr std::string_view INFO_TEXT =
    "\n\nJPG Data Vehicle (jdvrif v9.0)\nCreated by Nicholas Cleasby (@CleasbyCode) 10/04/2023\n\n"
    "jdvrif is a metadata \"steganography-like\" command-line tool used for concealing and extracting\n"
    "any file type within and from a JPG image.\n\n"
    "──────────────────────────\nCompile & run (Linux)\n──────────────────────────\n\n"
    "  $ sudo apt update\n"
    "  $ sudo apt install g++ cmake ninja-build util-linux libsodium-dev libturbojpeg0-dev libjpeg-dev zlib1g-dev libdeflate-dev\n\n"
    "  $ chmod +x compile_jdvrif.sh\n  $ ./compile_jdvrif.sh\n\n"
    "  $ sudo cp jdvrif /usr/bin\n  $ jdvrif\n\n"
    "──────────────────────────\nUsage\n──────────────────────────\n\n"
    "  jdvrif conceal [-b|-r|-x] <cover_image> <secret_file>\n  jdvrif recover <cover_image>\n  jdvrif capsize [-r|-x] <cover_image>\n  jdvrif --info\n\n"
    "──────────────────────────\nPlatform compatibility & size limits\n──────────────────────────\n\n"
    "Share your \"file-embedded\" JPG image on the following compatible sites.\n\n"
    "Platforms where size limit is measured by the combined size of cover image + compressed data file:\n\n"
    "\t• Flickr    (200 MB)\n\t• ImgPile   (100 MB)\n\t• ImgBB     (32 MB)\n\t• PostImage (32 MB)\n\t• Pixelfed  (15 MB)\n\n"
    "Limit measured by compressed data file size only:\n\n"
    "\t• Mastodon  (~6 MB)\n\t• Tumblr    (~64 KB)\n\t• X-Twitter (~10 KB)\n\n"
    "For example, on Mastodon, even if your cover image is 1 MB, you can still embed a data file\n"
    "up to the ~6 MB Mastodon size limit.\n\n"
    "Other:\n\n"
    "Bluesky - (use -b option). The finished \"file-embedded\" JPG must not exceed 2,000,000 bytes,\n"
    "so the cover image and the compressed data file share one combined budget:\n\n"
    "\t• Cover image + compressed data file: 2,000,000 bytes combined\n"
    "\t• Compressed data file on its own:    ~171 KB\n\n"
    "A cover image already at 2,000,000 bytes leaves no room for a payload, so keep the cover\n"
    "smaller than 2,000,000 bytes by at least the size of the compressed data file.\n\n"
    "Reddit - (use -r option). Cover and payload input files must each be no larger than 20 MiB.\n"
    "The cover is transcoded to baseline Q75/4:2:0, and the theoretical C3 payload limit is\n"
    "calculated from its luminance DCT blocks after transcoding.\n\n"
    "X-Twitter adaptive carrier - (use -x option). Cover and payload input files must each be no\n"
    "larger than 5 MiB and the cover must not exceed 4096x4096 pixels. The cover is transcoded\n"
    "to progressive 4:2:0 using its source-derived JPEG quality (capped at Q97), then embeds an\n"
    "encrypted payload using J-UNIWARD/STC. Without\n"
    "-x, the existing X-Twitter-compatible ICC path and its ~10 KB first-segment limit remain in use.\n\n"
    "In -r and -x modes, jdvrif skips zlib recompression for recognized already-compressed\n"
    "file types. For other inputs, you may want to compress the data yourself first (zip, rar,\n"
    "7z, etc.) so that you know its exact stored size.\n\n"
    "Platforms with small size limits, like X-Twitter (~10 KB), are best suited for data that\n"
    "compress especially well, such as text files.\n\n"
    "──────────────────────────\nModes\n──────────────────────────\n\n"
    "conceal - *Compresses when appropriate, encrypts and embeds your secret data file within a JPG cover image.\n"
    "recover - Decrypts, uncompresses when required and extracts the concealed data file from a JPG cover image\n"
    "          (recovery PIN required).\n"
    "capsize - Test-prepares a cover image and reports its C3 or J-UNIWARD/STC capacity.\n"
    "          This information applies only to conceal -r (Reddit) and conceal -x (X-Twitter);\n"
    "          no image is saved. Omitting the capsize option retains -x as the default.\n\n"
    "(*Compression: In -r and -x modes, recognized already-compressed file types (based on file\n"
    " extension, e.g. \".zip\") skip compression regardless of size. In other conceal modes,\n"
    " these file types skip compression only when greater than 10MB.)\n\n"
    "──────────────────────────\nPlatform options for conceal mode\n──────────────────────────\n\n"
    "-b (Bluesky) : Creates compatible \"file-embedded\" JPG images for posting on Bluesky.\n\n"
    "$ jdvrif conceal -b my_image.jpg hidden.doc\n\n"
    "These images are only compatible for posting on Bluesky.\n\n"
    "You must use the Python script \"bsky/create_bsky_post.py\" (in the repo's src folder) to post to Bluesky.\n"
    "Posting via the Bluesky website or mobile app will NOT work.\n\n"
    "You also need to create an app password for your Bluesky account: https://bsky.app/settings/app-passwords\n\n"
    "Set your credentials as environment variables. This keeps the app password out of the command line,\n"
    "where it would be visible to other local users via tools such as ps:\n\n"
    "  $ export ATP_AUTH_HANDLE='you.bsky.social'\n"
    "  $ read -rsp 'Bluesky app password: ' ATP_AUTH_PASSWORD && export ATP_AUTH_PASSWORD\n"
    "  $ printf '\\n'\n\n"
    "Here are some basic usage examples for the create_bsky_post.py Python script:\n\n"
    "Standard image post to your profile/account.\n\n"
    "$ python3 bsky/create_bsky_post.py \\\n"
    "    --image your_image.jpg \\\n"
    "    --alt-text \"alt-text here [optional]\" \\\n"
    "    \"standard post text here [required]\"\n\n"
    "If you want to post multiple images (Max. 4):\n\n"
    "$ python3 bsky/create_bsky_post.py \\\n"
    "    --image img1.jpg \\\n"
    "    --alt-text \"alt text for image 1\" \\\n"
    "    --image img2.jpg \\\n"
    "    --alt-text \"alt text for image 2\" \\\n"
    "    \"standard post text...\"\n\n"
    "If you want to post an image as a reply to another thread:\n\n"
    "$ python3 bsky/create_bsky_post.py \\\n"
    "    --image your_image.jpg \\\n"
    "    --alt-text \"alt_here\" \\\n"
    "    --reply-to https://bsky.app/profile/someone.bsky.social/post/8m2tgw6cgi23i \\\n"
    "    \"standard post text...\"\n\n"
    "After posting, remove the app password from the current shell with:\n\n"
    "  $ unset ATP_AUTH_PASSWORD\n\n"
    "Bluesky size limits: cover image + compressed data file must total no more than 2,000,000\n"
    "bytes, and the compressed data file alone must not exceed ~171 KB.\n\n"
    "-r (Reddit) : Creates a baseline Q75/4:2:0 JPG with an encrypted C3 DCT payload for Reddit.\n\n"
    "$ jdvrif conceal -r my_image.jpg hidden.doc\n\n"
    "These images are only compatible for posting on Reddit. The program displays the exact\n"
    "theoretical carrier limit for each transcoded cover image.\n\n"
    "Use \"jdvrif capsize -r my_image.jpg\" to inspect a Reddit cover before choosing a payload.\n\n"
    "-x (X-Twitter) : Creates a progressive 4:2:0 JPG using source-derived JPEG quality\n"
    "(capped at Q97), with an encrypted adaptive J-UNIWARD/STC payload for X-Twitter.\n\n"
    "$ jdvrif conceal -x my_image.jpg hidden.doc\n\n"
    "These images are only compatible for posting on X-Twitter. Cover and payload inputs must\n"
    "each be no larger than 5 MiB, and cover dimensions must not exceed 4096x4096 pixels.\n\n"
    "Use \"jdvrif capsize -x my_image.jpg\" to inspect an X-Twitter cover before choosing a payload.\n"
    "Its theoretical limit is total encrypted-envelope capacity, not a raw secret-file limit.\n"
    "Compression, encryption/recovery metadata and filename storage consume part of that capacity.\n"
    "Do not target the exact theoretical limit; when capacity permits, leave at least 1 KB of margin.\n\n"
    "To correctly download images from X-Twitter, click image within the post to fully expand it before saving.\n\n";

[[nodiscard]] std::string_view argAt(int argc, char** argv, int index) {
    if (index < 0 || index >= argc || argv[index] == nullptr) return {};
    return argv[index];
}

[[nodiscard]] std::string programName(int argc, char** argv) {
    if (argc <= 0 || argv[0] == nullptr) return "jdvrif";
    return fs::path(argv[0]).filename().string();
}

[[nodiscard]] std::string usageText(int argc, char** argv) {
    constexpr std::string_view PREFIX = "Usage: ";

    const std::string prog = programName(argc, argv);
    const std::string indent(PREFIX.size(), ' ');
    return std::format(
        "{0}{1} conceal [-b|-r|-x] <cover_image> <secret_file>\n"
        "{2}{1} recover <cover_image>\n"
        "{2}{1} capsize [-r|-x] <cover_image>\n"
        "{2}{1} --info",
        PREFIX,
        prog,
        indent);
}

[[nodiscard]] bool parseConcealOption(std::string_view arg, Option& option) {
    if (arg == "-b") {
        option = Option::Bluesky;
        return true;
    }
    if (arg == "-r") {
        option = Option::Reddit;
        return true;
    }
    if (arg == "-x") {
        option = Option::Twitter;
        return true;
    }
    return false;
}
} // namespace

void displayInfo() {
    std::print("{}", INFO_TEXT);
}

std::optional<ProgramArgs> ProgramArgs::parse(int argc, char** argv) {
    const std::string usage = usageText(argc, argv);

    if (argc < 2) die(usage);

    if (argc == 2 && argAt(argc, argv, 1) == "--info") {
        displayInfo();
        return std::nullopt;
    }

    ProgramArgs out;
    const std::string_view mode = argAt(argc, argv, 1);

    if (mode == "conceal") {
        int image_index = 2;

        if (parseConcealOption(argAt(argc, argv, image_index), out.option)) {
            ++image_index;
        }

        if (argc != image_index + 2) die(usage);

        out.image_file_path = argAt(argc, argv, image_index);
        out.data_file_path = argAt(argc, argv, image_index + 1);
        return out;
    }

    if (mode == "recover") {
        if (argc != 3) die(usage);
        out.mode = Mode::recover;
        out.image_file_path = argAt(argc, argv, 2);
        return out;
    }

    if (mode == "capsize") {
        int image_index = 2;
        out.mode = Mode::capsize;
        // Preserve the original capsize behavior when no platform option is
        // supplied, while allowing an explicit choice for both carrier types.
        out.option = Option::Twitter;
        const std::string_view capsize_option = argAt(argc, argv, image_index);
        if (capsize_option == "-r") {
            out.option = Option::Reddit;
            ++image_index;
        } else if (capsize_option == "-x") {
            ++image_index;
        }

        if (argc != image_index + 1) die(usage);
        out.image_file_path = argAt(argc, argv, image_index);
        return out;
    }

    die(usage);
}

[[noreturn]] void ProgramArgs::die(const std::string& message) {
    throw std::runtime_error(message);
}
