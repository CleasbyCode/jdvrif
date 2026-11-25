#include "compression.h"
#include "file_utils.h"
#include "signal_utils.h"
#include "worker_team.h"
#include "zlib_wipe_alloc.h"

#include <libdeflate.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
// Two tiers. The "maximum compression" tier that used to sit here is not worth
// paying for on either back end: measured on a 150 MB mixed-binary corpus,
// zlib level 9 took 19.4 s for 58,100,237 bytes against 6.3 s for 58,360,140 at
// level 6 -- 3.1x the time for 0.45% smaller output. libdeflate shows the same
// shape (see LIBDEFLATE_LEVEL). Very large inputs drop to fastest so a
// conceal stays bounded in time.
[[nodiscard]] int selectCompressionLevel(std::size_t input_size) {
    constexpr std::size_t THRESHOLD_BEST_SPEED = 500 * 1024 * 1024;

    return (input_size > THRESHOLD_BEST_SPEED) ? Z_BEST_SPEED : Z_DEFAULT_COMPRESSION;
}

inline constexpr std::size_t
    ZLIB_IN_CHUNK_SIZE  = 2 * 1024 * 1024,
    ZLIB_OUT_CHUNK_SIZE = 2 * 1024 * 1024,
    // OutputFile buffer for the deflated stream: whole chunks bypass it, short
    // tails pass through and are zeroed with it.
    COMPRESSED_OUTPUT_BUFFER_SIZE = 64 * 1024;

// Files at or below this size are compressed in a single whole-buffer
// libdeflate call. That call is also the one place a conceal cannot react to
// Ctrl+C, because libdeflate has no incremental API to check cancellation from,
// so the cap bounds two things at once: peak RSS (input + compress_bound, ~2x
// input while compressing) and the uninterruptible window (~2 s at 61 MB/s,
// measured). Larger inputs fall back to the zlib streaming deflate below, which
// holds only fixed chunks and checks for cancellation every chunk.
//
// Given enough cores (see parallelDeflatePays), inputs of
// PARALLEL_DEFLATE_MIN_SIZE and up never reach either path: they go to the
// sliced parallel deflate further down. What is left here is small inputs, and
// everything on a single core.
//
// The cost of the cap is real -- libdeflate ran 2.7x faster than streaming zlib
// at the same level on the same corpus -- so it is set where payloads stop
// being plausible for any supported platform (the largest, Flickr, is 200 MB
// total and every other site is 100 MB or less) rather than lower.
inline constexpr std::size_t LIBDEFLATE_WHOLE_BUFFER_LIMIT = 128 * 1024 * 1024;

// --------------------------- libdeflate fast path ---------------------------

// libdeflate level for the whole-buffer path. Measured (bench/compress_levels)
// on real corpora, libdeflate L9 costs ~2.6-2.9x the time of L6 for only ~1-2%
// smaller output; L10-12 are far worse (an "ultra" tier with marginal gains).
// L6 is the ratio/time sweet spot -- compression is the dominant cost in a
// conceal, and a 1-2% larger payload practically never changes platform fit.
// Inputs here are at most LIBDEFLATE_WHOLE_BUFFER_LIMIT, well under the size
// where selectCompressionLevel drops to fastest, so one level covers them all.
// Do NOT raise this back toward 9-12 without re-running the level sweep.
inline constexpr int LIBDEFLATE_LEVEL = 6;

// libdeflate's compressor state is built from the plaintext: its match-finder
// hash tables are indexed by hashes of the input's bytes, and its sequence and
// frequency buffers describe the input. libdeflate_free_compressor would
// release that unwiped through plain free(), so route its allocations through
// the same wiping blocks as zlib's (zlib_wipe_alloc.h). Its aligned allocator
// sits on top of these and needs nothing beyond malloc's alignment. C linkage:
// libdeflate.h declares the option members inside extern "C".
extern "C" inline void* jdvrifLibdeflateWipeAllocate(std::size_t bytes) noexcept {
    return zlib_wipe_alloc::allocateBlock(bytes);
}

extern "C" inline void jdvrifLibdeflateWipeRelease(void* address) noexcept {
    zlib_wipe_alloc::releaseBlock(address);
}

struct CompressorGuard {
    libdeflate_compressor* c{nullptr};
    explicit CompressorGuard(int level) {
        libdeflate_options options{};
        options.sizeof_options = sizeof(options);
        options.malloc_func = jdvrifLibdeflateWipeAllocate;
        options.free_func = jdvrifLibdeflateWipeRelease;
        c = libdeflate_alloc_compressor_ex(level, &options);
    }
    ~CompressorGuard() { if (c) libdeflate_free_compressor(c); }
    CompressorGuard(const CompressorGuard&) = delete;
    CompressorGuard& operator=(const CompressorGuard&) = delete;
};

// An initialized deflate stream, ended on every exit path. Its window and
// pending output hold plaintext, so its allocations are wiped on release.
// Pinned in place: zlib's state points back at the z_stream.
struct DeflateStream {
    z_stream strm{};

    DeflateStream(int level, int window_bits) {
        zlib_wipe_alloc::install(strm);
        throwIf(deflateInit2(&strm, level, Z_DEFLATED, window_bits, 8, Z_DEFAULT_STRATEGY) != Z_OK,
            "zlib: deflateInit failed");
    }
    ~DeflateStream() { deflateEnd(&strm); }
    DeflateStream(const DeflateStream&) = delete;
    DeflateStream& operator=(const DeflateStream&) = delete;
};

[[nodiscard]] InputFile openCompressionInput(const fs::path& path) {
    return InputFile(path, std::format("Failed to open file for compression: {}", displaySafePath(path)));
}

[[nodiscard]] vBytes readWholeFile(const fs::path& path, std::size_t expected_size) {
    throwIfSignalCancellationRequested();
    InputFile input = openCompressionInput(path);
    vBytes buffer(expected_size);
    // A throw between here and the return -- a short read, a file that changed
    // under us, Ctrl+C -- would otherwise free a buffer holding some or all of
    // the plaintext. The success path hands ownership to the caller (which
    // wipes it), so the guard cannot simply cover the whole scope.
    try {
        input.readExact(buffer, "Read Error: Failed while reading input file.");
        input.requireEnd("Read Error: Input file changed while compressing.");
        throwIfSignalCancellationRequested();
    } catch (...) {
        wipeContainerCapacity(buffer);
        throw;
    }
    return buffer;
}

[[nodiscard]] bool libdeflateCompressFileToPath(
    const fs::path& input_path,
    const StagingFile& output_stage,
    std::size_t expected_input_size,
    UnshrunkOutput unshrunk_output) {
    vBytes output;
    // Deflate is not encryption: this buffer holds the payload in a form that
    // is plaintext-equivalent, so it is zeroed rather than merely freed --
    // including on the throwing paths below. Same reasoning as the link-free
    // StagingFile the compressed bytes are written to (see conceal.cpp).
    WipeBytesGuard output_wipe{output};
    std::size_t produced = 0;

    {
        // Scoped so the plaintext input is released (and its pages returned)
        // before the output is written: the write can be slow on a slow disk,
        // and there is no reason to hold both buffers across it.
        vBytes input = readWholeFile(input_path, expected_input_size);
        WipeBytesGuard input_wipe{input};

        CompressorGuard compressor(LIBDEFLATE_LEVEL);
        throwIf(!compressor.c, "libdeflate: failed to allocate compressor");

        output.resize(libdeflate_zlib_compress_bound(compressor.c, input.size()));

        produced = libdeflate_zlib_compress(compressor.c, input.data(), input.size(), output.data(), output.size());
        throwIfSignalCancellationRequested();
        if (produced == 0) {
            // Only happens if the bound-sized buffer was somehow insufficient.
            throw std::runtime_error("libdeflate: zlib compression failed");
        }
    }
    if (unshrunk_output == UnshrunkOutput::discard && produced >= expected_input_size) {
        return false;
    }

    // output_stage is a StagingFile: link-free already, so a failure part way
    // through leaves nothing behind for a cleanup guard to remove.
    OutputFile out_file(output_stage, COMPRESSED_OUTPUT_BUFFER_SIZE);
    out_file.write(std::span<const Byte>(output.data(), produced), WRITE_COMPLETE_ERROR);
    out_file.close(WRITE_COMPLETE_ERROR);
    return true;
}

// ------------------------- zlib streaming fallback --------------------------

// write_chunk returns false to abandon the stream; the function then returns
// false at once, without reading the rest of the input.
template<typename WriteChunkFn>
[[nodiscard]] bool deflateFromInputFile(InputFile& input, std::size_t expected_input_size, WriteChunkFn&& write_chunk) {
    // Plaintext and plaintext-equivalent windows: zeroed on every exit path,
    // for the same reason as the whole-buffer path above.
    vBytes in_chunk(ZLIB_IN_CHUNK_SIZE);
    WipeBytesGuard in_chunk_wipe{in_chunk};
    vBytes out_chunk(ZLIB_OUT_CHUNK_SIZE);
    WipeBytesGuard out_chunk_wipe{out_chunk};

    DeflateStream deflater(selectCompressionLevel(expected_input_size), MAX_WBITS);
    z_stream& strm = deflater.strm;

    std::size_t input_left = expected_input_size;
    bool finished = false;
    while (!finished) {
        throwIfSignalCancellationRequested();
        const std::size_t to_read = std::min(input_left, in_chunk.size());
        const std::size_t read_count = input.readSome(
            std::span<Byte>(in_chunk).first(to_read),
            "Read Error: Failed while reading input file.");
        throwIf(read_count != to_read, "Read Error: Input file changed while compressing.");
        input_left -= to_read;

        strm.next_in  = in_chunk.data();
        strm.avail_in = static_cast<uInt>(read_count);

        const int flush = (input_left == 0) ? Z_FINISH : Z_NO_FLUSH;
        do {
            strm.next_out  = out_chunk.data();
            strm.avail_out = static_cast<uInt>(out_chunk.size());

            const int ret = deflate(&strm, flush);
            throwIfSignalCancellationRequested();
            if (ret != Z_OK && ret != Z_STREAM_END) {
                throw std::runtime_error(std::format(
                    "zlib deflate error: {}",
                    strm.msg ? strm.msg : std::to_string(ret)));
            }

            const std::size_t produced = out_chunk.size() - strm.avail_out;
            if (produced > 0 && !write_chunk(std::span<const Byte>(out_chunk.data(), produced))) {
                return false;
            }

            if (ret == Z_STREAM_END) {
                finished = true;
                break;
            }
        } while (strm.avail_out == 0);
    }

    input.requireEnd("Read Error: Input file changed while compressing.");
    return true;
}

[[nodiscard]] bool zlibStreamCompressFileToPath(
    const fs::path& input_path,
    const StagingFile& output_stage,
    std::size_t expected_input_size,
    UnshrunkOutput unshrunk_output) {
    InputFile input = openCompressionInput(input_path);
    OutputFile output(output_stage, COMPRESSED_OUTPUT_BUFFER_SIZE);
    // The stream only grows, so once it has reached the input's size the
    // result is already decided and the rest of the file need not be deflated.
    std::size_t written = 0;
    const bool completed = deflateFromInputFile(input, expected_input_size, [&](std::span<const Byte> chunk) {
        written += chunk.size();
        if (unshrunk_output == UnshrunkOutput::discard && written >= expected_input_size) return false;
        output.write(chunk, WRITE_COMPLETE_ERROR);
        return true;
    });
    if (!completed) return false;
    output.close(WRITE_COMPLETE_ERROR);
    return true;
}

// ------------------------- sliced parallel deflate --------------------------
//
// One deflate stream runs on one core (about 44 MB/s through libdeflate, 32
// MB/s through streaming zlib), and compression is 95% of a default-mode
// conceal. So an input large enough to be worth the threads is cut into fixed
// 1 MiB slices that are deflated in parallel, pigz-style:
// each slice is primed with the 32 KiB before it as its dictionary, so matches
// still reach back across the seam, and ends in a sync flush (an empty stored
// block), so the slices concatenate into one valid deflate stream; their
// Adler-32s are combined. Recovery inflates it like any other zlib stream.
//
// Same level and, to within a few hundredths of a percent, the same size as
// the streaming path. Against the whole-buffer libdeflate call it replaces up
// to LIBDEFLATE_WHOLE_BUFFER_LIMIT it is 3-4x faster on 16 threads for output
// 0.2-2.4% larger on real binaries (measured; zlib level 6 packs a little
// less tightly than libdeflate level 6). The bytes depend only on the slice
// size, never on the thread count. Memory is bounded
// by the slice ring (two slots per thread, each 1 MiB of input plus its
// output), and every buffer holding payload bytes -- the slot inputs and
// outputs, the dictionary tail, zlib's own state -- is zeroed before it is
// freed.
inline constexpr std::size_t
    PARALLEL_SLICE_SIZE     = 1024 * 1024,
    DEFLATE_DICTIONARY_SIZE = 32 * 1024,
    // Below this the whole-buffer libdeflate call takes a third of a second at
    // most, and its slightly smaller output is worth more than the time: the
    // capacity-bound modes (Bluesky, Reddit, X-Twitter) carry payloads that
    // nearly always sit below it, so their output is unchanged.
    PARALLEL_DEFLATE_MIN_SIZE = 16 * PARALLEL_SLICE_SIZE;
inline constexpr unsigned MAX_DEFLATE_THREADS = 32;

[[nodiscard]] unsigned deflateThreadCount() noexcept {
    return std::min(worker_team::availableCpus(), MAX_DEFLATE_THREADS);
}

// Whether slicing beats the single-stream path this input would otherwise
// take. Against streaming zlib, which the slices match byte for byte in cost,
// two threads already halve the time. Against the whole-buffer libdeflate
// call, which is about 1.7x faster per byte than zlib, two threads only break
// even (measured), so it takes three.
[[nodiscard]] bool parallelDeflatePays(std::size_t input_size, unsigned threads) noexcept {
    if (input_size < PARALLEL_DEFLATE_MIN_SIZE) return false;
    return threads >= (input_size <= LIBDEFLATE_WHOLE_BUFFER_LIMIT ? 3U : 2U);
}

[[nodiscard]] bool parallelDeflateFileToPath(
    const fs::path& input_path,
    const StagingFile& output_stage,
    std::size_t expected_input_size,
    UnshrunkOutput unshrunk_output,
    unsigned threads) {
    const int level = selectCompressionLevel(expected_input_size);
    const std::size_t total_slices =
        (expected_input_size + PARALLEL_SLICE_SIZE - 1U) / PARALLEL_SLICE_SIZE;

    // One team for the whole file, pipelined through a ring of slice slots.
    // A worker claims the next slice and reads it in (in file order, under
    // read_mutex), deflates it with the z_stream it set up once, then writes
    // out every finished slice at the head of the ring (in stream order,
    // under write_mutex). Reads and writes overlap other workers' deflating,
    // and a slow slice only holds up the slots behind it, not a whole window.
    // Two slots per thread: a worker waits for a free slot only when the
    // slice at the head is still being deflated.
    //
    // Each slot holds its slice behind the 32 KiB of input before it, the
    // slice's dictionary (none for the first slice; every later slice follows
    // a whole 1 MiB one).
    struct Slot {
        vBytes input;
        vBytes output;
        std::size_t length = 0;
        std::size_t primed = 0;
        std::size_t produced = 0;
        uLong adler = 0;
        bool done = false;
    };
    const std::size_t slot_count =
        std::min<std::size_t>(total_slices, static_cast<std::size_t>(threads) * 2U);
    std::vector<Slot> slots(slot_count);
    struct SlotsWipe {
        std::vector<Slot>& slots;
        ~SlotsWipe() {
            for (Slot& slot : slots) {
                wipeContainerCapacity(slot.input);
                wipeContainerCapacity(slot.output);
            }
        }
    } slots_wipe{slots};
    for (Slot& slot : slots) slot.input.resize(DEFLATE_DICTIONARY_SIZE + PARALLEL_SLICE_SIZE);
    std::array<Byte, DEFLATE_DICTIONARY_SIZE> tail{}; // the last 32 KiB read
    WipePodGuard tail_wipe{tail};

    InputFile input = openCompressionInput(input_path);
    OutputFile output(output_stage, COMPRESSED_OUTPUT_BUFFER_SIZE);
    // RFC 1950 header: CMF 0x78 (deflate, 32 KiB window), then FLG carrying
    // the level hint and no preset dictionary.
    const std::array<Byte, 2> header{0x78, static_cast<Byte>(level == Z_BEST_SPEED ? 0x01 : 0x9C)};
    output.write(header, WRITE_COMPLETE_ERROR);
    std::size_t written = header.size();
    uLong adler = adler32(0L, Z_NULL, 0);

    std::mutex read_mutex;      // input, tail, next_read
    std::mutex write_mutex;     // output, written, adler, the drain
    std::mutex state_mutex;     // next_written, slot `done` flags
    std::condition_variable slot_freed;
    std::size_t next_read = 0;
    std::size_t next_written = 0;
    std::atomic<bool> failed{false};
    std::atomic<bool> discarded{false};
    const auto stopping = [&] {
        return failed.load(std::memory_order_relaxed) || discarded.load(std::memory_order_relaxed) ||
            pendingSignalCancellation() != 0;
    };

    // Raises a stop flag under state_mutex and wakes every worker waiting for
    // a slot. claimSlice checks the flags under the same lock before each
    // wait, so none can miss the wakeup and sleep out its timeout.
    const auto stopAndWake = [&](std::atomic<bool>& flag) {
        {
            const std::lock_guard state_lock(state_mutex);
            flag = true;
        }
        slot_freed.notify_all();
    };

    // Claims and reads the next slice; nullopt once there is none or the run
    // is stopping.
    const auto claimSlice = [&]() -> std::optional<std::size_t> {
        const std::lock_guard read_lock(read_mutex);
        if (next_read == total_slices || stopping()) return std::nullopt;
        const std::size_t index = next_read;
        {
            // The slot is free once the slice slot_count before has been written.
            std::unique_lock state_lock(state_mutex);
            while (index >= next_written + slot_count) {
                if (stopping()) return std::nullopt;
                // Woken by the drain, a discard or a failure; the timeout
                // notices Ctrl+C, which a signal handler cannot notify.
                slot_freed.wait_for(state_lock, std::chrono::milliseconds(20));
                if (stopping()) return std::nullopt;
            }
        }
        Slot& slot = slots[index % slot_count];
        const std::size_t start = index * PARALLEL_SLICE_SIZE;
        slot.length = std::min(PARALLEL_SLICE_SIZE, expected_input_size - start);
        slot.primed = index == 0 ? 0 : DEFLATE_DICTIONARY_SIZE;
        std::copy(tail.begin(), tail.end(), slot.input.begin());
        Byte* const data = slot.input.data() + DEFLATE_DICTIONARY_SIZE;
        const std::size_t read_count = input.readSome(
            std::span<Byte>(data, slot.length), "Read Error: Failed while reading input file.");
        throwIf(read_count != slot.length, "Read Error: Input file changed while compressing.");
        // Only a short final slice leaves the tail stale, and nothing follows it.
        if (slot.length >= DEFLATE_DICTIONARY_SIZE) {
            std::copy(data + slot.length - DEFLATE_DICTIONARY_SIZE, data + slot.length, tail.begin());
        }
        ++next_read;
        return index;
    };

    // Writes every finished slice at the head of the ring, in order. Not
    // after a failure or a discard: the output is abandoned, so no later
    // slice is written.
    const auto drain = [&] {
        const std::lock_guard write_lock(write_mutex);
        while (!failed.load(std::memory_order_relaxed) && !discarded.load(std::memory_order_relaxed)) {
            std::size_t index = 0;
            {
                const std::lock_guard state_lock(state_mutex);
                if (next_written == total_slices || !slots[next_written % slot_count].done) return;
                index = next_written;
            }
            Slot& slot = slots[index % slot_count];
            adler = adler32_combine(adler, slot.adler, static_cast<z_off_t>(slot.length));
            written += slot.produced;
            // The stream only grows, so once it has reached the input's size
            // the result is already decided.
            if (unshrunk_output == UnshrunkOutput::discard && written >= expected_input_size) {
                stopAndWake(discarded);
                return;
            }
            output.write(std::span<const Byte>(slot.output.data(), slot.produced), WRITE_COMPLETE_ERROR);
            {
                const std::lock_guard state_lock(state_mutex);
                slot.done = false;
                ++next_written;
            }
            slot_freed.notify_all();
        }
    };

    const auto deflateSlices = [&] {
        DeflateStream deflater(level, -MAX_WBITS);  // raw deflate: the caller frames it
        z_stream& strm = deflater.strm;

        // Anything here may throw -- SignalCancellation included, from the
        // read in claimSlice or the write in drain; work() handles it.
        while (const std::optional<std::size_t> claimed = claimSlice()) {
            const std::size_t index = *claimed;
            Slot& slot = slots[index % slot_count];
            Byte* const data = slot.input.data() + DEFLATE_DICTIONARY_SIZE;
            const bool final_slice = index + 1U == total_slices;
            throwIf(deflateReset(&strm) != Z_OK, "zlib: deflateReset failed");
            throwIf(
                slot.primed != 0 &&
                    deflateSetDictionary(&strm, data - slot.primed, static_cast<uInt>(slot.primed)) != Z_OK,
                "zlib: deflateSetDictionary failed");
            // deflateBound plus the sync flush's empty stored block.
            const std::size_t bound = deflateBound(&strm, static_cast<uLong>(slot.length)) + 16U;
            if (slot.output.size() < bound) {
                wipeContainerCapacity(slot.output);
                slot.output.resize(bound);
            }
            strm.next_in = data;
            strm.avail_in = static_cast<uInt>(slot.length);
            strm.next_out = slot.output.data();
            strm.avail_out = static_cast<uInt>(slot.output.size());
            const int ret = deflate(&strm, final_slice ? Z_FINISH : Z_SYNC_FLUSH);
            if ((final_slice ? ret != Z_STREAM_END : ret != Z_OK) ||
                strm.avail_in != 0 || strm.avail_out == 0) {
                throw std::runtime_error(std::format(
                    "zlib deflate error: {}", strm.msg ? strm.msg : std::to_string(ret)));
            }
            slot.produced = slot.output.size() - strm.avail_out;
            slot.adler = adler32(adler32(0L, Z_NULL, 0), data, static_cast<uInt>(slot.length));
            {
                const std::lock_guard state_lock(state_mutex);
                slot.done = true;
            }
            drain();
        }
    };
    // A failure stops the others at once -- including any waiting for a slot
    // behind a slice that will now never be written -- rather than when
    // worker_team records it.
    const auto work = [&] {
        try {
            deflateSlices();
        } catch (...) {
            stopAndWake(failed);
            throw;
        }
    };
    worker_team::run(std::min<std::size_t>(total_slices, threads), failed, work);
    throwIfSignalCancellationRequested();
    if (discarded) return false;
    throwIf(next_written != total_slices, "zlib: parallel deflate stopped early");
    input.requireEnd("Read Error: Input file changed while compressing.");

    const std::array<Byte, 4> checksum{
        static_cast<Byte>(adler >> 24), static_cast<Byte>(adler >> 16),
        static_cast<Byte>(adler >> 8), static_cast<Byte>(adler)};
    written += checksum.size();
    if (unshrunk_output == UnshrunkOutput::discard && written >= expected_input_size) return false;
    output.write(checksum, WRITE_COMPLETE_ERROR);
    output.close(WRITE_COMPLETE_ERROR);
    return true;
}

} // namespace

bool zlibCompressFileToPath(
    const fs::path& input_path,
    const StagingFile& output_stage,
    std::size_t expected_input_size,
    UnshrunkOutput unshrunk_output) {
    // Every path emits a standard RFC 1950 zlib stream, so the recover-side
    // zlib inflate decodes any of them. Anything big enough to be worth the
    // threads is deflated in parallel slices. Below that, and on a single
    // core, libdeflate handles the common (smaller) case in one call and the
    // streaming zlib path keeps memory bounded for very large inputs.
    if (const unsigned threads = deflateThreadCount(); parallelDeflatePays(expected_input_size, threads)) {
        return parallelDeflateFileToPath(input_path, output_stage, expected_input_size, unshrunk_output, threads);
    }
    if (expected_input_size <= LIBDEFLATE_WHOLE_BUFFER_LIMIT) {
        return libdeflateCompressFileToPath(input_path, output_stage, expected_input_size, unshrunk_output);
    }
    return zlibStreamCompressFileToPath(input_path, output_stage, expected_input_size, unshrunk_output);
}

bool sampledDataLooksIncompressible(const fs::path& input_path, std::size_t input_size) {
    // 32 windows of 32 KiB: 1 MiB deflated (about 25 ms) however large the
    // file, at the level the full pass would use. A window is deflate's whole
    // history size, so each sample compresses about as well as it would in
    // place.
    constexpr std::size_t WINDOW_SIZE = 32 * 1024;
    constexpr std::size_t WINDOW_COUNT = 32;
    constexpr std::size_t SAMPLED_SIZE = WINDOW_SIZE * WINDOW_COUNT;
    // Sampling a file not much bigger than the samples saves nothing.
    if (input_size < SAMPLED_SIZE * 4) return false;

    InputFile input = openCompressionInput(input_path);
    // Plaintext, and deflate output that is plaintext-equivalent: zeroed.
    vBytes window(WINDOW_SIZE);
    WipeBytesGuard window_wipe{window};
    CompressorGuard compressor(LIBDEFLATE_LEVEL);
    throwIf(!compressor.c, "libdeflate: failed to allocate compressor");
    vBytes deflated(libdeflate_deflate_compress_bound(compressor.c, WINDOW_SIZE));
    WipeBytesGuard deflated_wipe{deflated};

    const std::size_t stride = (input_size - WINDOW_SIZE) / (WINDOW_COUNT - 1);
    std::size_t deflated_size = 0;
    for (std::size_t index = 0; index < WINDOW_COUNT; ++index) {
        throwIfSignalCancellationRequested();
        preadExactFromFd(input.fd(), window, index * stride, "Read Error: Failed while reading input file.");
        const std::size_t produced = libdeflate_deflate_compress(
            compressor.c, window.data(), window.size(), deflated.data(), deflated.size());
        throwIf(produced == 0, "libdeflate: deflate compression failed");
        deflated_size += produced;
    }
    return deflated_size >= SAMPLED_SIZE - SAMPLED_SIZE / 100;
}
