#pragma once

#include "common.h"

class StagingFile;

// Whether a deflate stream that is no smaller than its input is still wanted.
enum class UnshrunkOutput { keep, discard };

// Writes the zlib stream of input_path to output_stage and returns true. With
// UnshrunkOutput::discard it returns false instead, leaving output_stage without
// a usable stream, as soon as the stream is known to be no smaller than the
// input: the caller then stores the input as it is, so writing the stream out
// (or, when streaming, deflating the rest of the file) would be wasted work.
[[nodiscard]] bool zlibCompressFileToPath(
    const fs::path& input_path,
    const StagingFile& output_stage,
    std::size_t expected_input_size,
    UnshrunkOutput unshrunk_output);

// True when deflating windows sampled evenly across the file saves under 1%,
// i.e. the file is already compressed or encrypted whatever its header says.
// Lets a large payload skip a full deflate pass that would be thrown away.
// A guess: compressible data lying wholly between the samples is missed.
[[nodiscard]] bool sampledDataLooksIncompressible(const fs::path& input_path, std::size_t input_size);
