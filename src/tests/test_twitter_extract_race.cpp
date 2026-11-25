// extractTwitterPayload's last cancellation check: a signal that arrives
// right after the check has loaded "no signal" must not split the decision
// to wipe from the decision to throw. The payload is either returned (the
// cancellation is then seen at the next check) or wiped before the throw;
// it is never freed with its bytes in it.
//
// pendingSignalCancellation is wrapped (-Wl,--wrap) so that, on the last
// call extraction makes, SIGINT is raised just after the real load: the
// window between a load and whatever acts on it. Run with one carrier
// thread so the call count is the same in both runs.
//
// Also: coefficientParityBits wipes its partial result when an out-of-range
// offset stops it (an internal-API misuse twitter_steg.cpp never makes).
#include "encryption_internal.h"
#include "signal_utils.h"
#include "twitter_juniward.h"
#include "twitter_steg.h"

#include <sodium.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <malloc.h>
#include <new>
#include <stdexcept>
#include <vector>

extern "C" int __real__Z25pendingSignalCancellationv() noexcept;

namespace {
int wrapped_calls = 0;
int race_at = 0;  // 0: off
const Byte* watched = nullptr;
std::size_t watched_size = 0;
bool freed_unwiped = false;

vBytes readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return vBytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}
} // namespace

extern "C" int __wrap__Z25pendingSignalCancellationv() noexcept {
    const int value = __real__Z25pendingSignalCancellationv();
    if (++wrapped_calls == race_at) std::raise(SIGINT);
    return value;
}

void operator delete(void* block) noexcept {
    if (block != nullptr && watched != nullptr &&
        memmem(block, malloc_usable_size(block), watched, watched_size) != nullptr) {
        freed_unwiped = true;
    }
    std::free(block);
}
void operator delete(void* block, std::size_t) noexcept { operator delete(block); }

int main(int argc, char** argv) {
    if (argc != 3 || sodium_init() < 0) return 2;
    installProcessSignalHandlers();

    // Every coefficient odd, so the partial parity prefix is a run of ones.
    {
        const std::vector<std::int16_t> coefficients(64, 1);
        std::vector<std::uint32_t> offsets(64);
        for (std::uint32_t index = 0; index < 64; ++index) offsets[index] = index;
        offsets.push_back(1000);
        const vBytes ones(32, 1);
        watched = ones.data();
        watched_size = ones.size();
        bool refused = false;
        try {
            (void)twitter_steg_internal::coefficientParityBits(coefficients, offsets);
        } catch (const std::runtime_error&) {
            refused = true;
        }
        watched = nullptr;
        if (!refused || freed_unwiped) {
            std::fputs(refused ? "FAIL: an out-of-range offset freed the parity prefix unwiped\n"
                               : "FAIL: an out-of-range offset was accepted\n", stderr);
            return 1;
        }
    }
    const vBytes cover_bytes = readFile(argv[1]);
    const vBytes data = readFile(argv[2]);
    const vBytes kdf_metadata(56, 0x5A);
    const vBytes envelope = makeTwitterEnvelope(kdf_metadata, data, true);
    constexpr CarrierKey128 key{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const vBytes embedded = embedTwitterPayload(prepareTwitterCover(cover_bytes), key, envelope);

    // Count the calls one extraction makes.
    wrapped_calls = 0;
    {
        auto payload = extractTwitterPayload(embedded, key);
        if (!payload || *payload != envelope) {
            std::fputs("FAIL: extraction round trip\n", stderr);
            return 1;
        }
        sodium_memzero(payload->data(), payload->size());
    }
    const int calls = wrapped_calls;

    // Again, with the signal landing just after the last of them.
    wrapped_calls = 0;
    race_at = calls;
    watched = envelope.data() + envelope.size() / 2 - 16;
    watched_size = 32;
    bool cancelled = false;
    try {
        auto payload = extractTwitterPayload(embedded, key);
        if (payload) sodium_memzero(payload->data(), payload->size());
    } catch (const SignalCancellation&) {
        cancelled = true;
    }
    watched = nullptr;
    if (wrapped_calls != calls || pendingSignalCancellation() != SIGINT) {
        std::fputs("FAIL: the race was not reproduced\n", stderr);
        return 1;
    }
    if (freed_unwiped) {
        std::fputs("FAIL: a cancellation freed the extracted payload unwiped\n", stderr);
        return 1;
    }
    std::printf("X-Twitter payload survives a cancellation racing its last check (%s).\n",
                cancelled ? "wiped, then cancelled" : "returned, cancellation pending");
    return 0;
}
