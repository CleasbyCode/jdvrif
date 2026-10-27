// The plaintext readers -- raw-mode encryption, libdeflate whole-file and
// zlib streaming compression -- must not leave a copy of the input file in
// freed heap memory. A std::ifstream would: its internal buffer is filled
// from the file and freed unwiped. libstdc++ allocates that buffer with
// operator new[], so replacing the global operators observes it even though
// the allocation happens inside the shared library.
#include "compression.cpp"        // found via -I<source tree>
#include "encryption_stream.cpp"

#include <cstdio>
#include <cstring>
#include <new>
#include <string_view>

namespace {
bool watching = false;
bool leaked = false;
std::size_t observed = 0;

struct Record {
    void* pointer;
    std::size_t size;
};
Record records[256]{};

constexpr std::string_view MARKER = "jdvrif-input-wipe-marker";

void* allocate(std::size_t size) {
    void* pointer = std::malloc(size == 0 ? 1 : size);
    if (pointer == nullptr) throw std::bad_alloc();
    if (watching) {
        for (Record& record : records) {
            if (record.pointer == nullptr) {
                record = {pointer, size};
                break;
            }
        }
    }
    return pointer;
}

void release(void* pointer) noexcept {
    if (pointer == nullptr) return;
    for (Record& record : records) {
        if (record.pointer != pointer) continue;
        ++observed;
        const std::string_view block(static_cast<const char*>(pointer), record.size);
        if (block.find(MARKER) != std::string_view::npos) leaked = true;
        record = {};
        break;
    }
    std::free(pointer);
}
} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { release(pointer); }

namespace {
int failures = 0;

template<typename Fn>
void check(const char* name, Fn&& fn) {
    for (Record& record : records) record = {};
    leaked = false;
    observed = 0;
    watching = true;
    fn();
    watching = false;
    if (observed == 0) {
        std::fprintf(stderr, "%s: observed no heap blocks\n", name);
        ++failures;
    } else if (leaked) {
        std::fprintf(stderr, "%s: freed a heap block holding input-file plaintext\n", name);
        ++failures;
    }
}
} // namespace

int main() {
    if (sodium_init() < 0) return 1;
    const fs::path path = "plaintext_input.bin";
    std::string plaintext;
    while (plaintext.size() < 100) plaintext += MARKER;
    plaintext.resize(100);
    {
        std::FILE* file = std::fopen(path.c_str(), "wb");
        if (file == nullptr || std::fwrite(plaintext.data(), 1, plaintext.size(), file) != plaintext.size()) return 1;
        std::fclose(file);
    }

    check("raw-mode encryption reader", [&] {
        std::size_t seen = 0;
        forEachPrefixedPlainChunkFromFile(path, plaintext.size(), {}, [&](std::span<const Byte> chunk, bool) {
            seen += chunk.size();
        });
        if (seen != plaintext.size()) ++failures;
    });
    check("libdeflate whole-file reader", [&] {
        vBytes input = readWholeFile(path, plaintext.size());
        wipeContainerCapacity(input);
    });
    check("zlib streaming reader", [&] {
        StagingFile out({}, "z");
        zlibStreamCompressFileToPath(path, out.path(), plaintext.size());
    });
    std::remove(path.c_str());

    if (failures != 0) return 1;
    std::puts("plaintext input readers leave no unwiped heap copy");
    return 0;
}
