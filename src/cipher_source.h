#pragma once

#include "common.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

// Sequential reader over a recovery ciphertext of known length, read straight
// from wherever it already lives (the cover image, or a decoded carrier) so it
// is never copied to a staging file first. The decryptor pulls frames from it
// as it authenticates them, so a wrong PIN is rejected after the first frame.
class CipherSource {
public:
    CipherSource(const CipherSource&) = delete;
    CipherSource& operator=(const CipherSource&) = delete;
    virtual ~CipherSource() = default;

    [[nodiscard]] std::size_t remaining() const noexcept { return remaining_; }

    // Fills `out` and returns true, or returns false (reading nothing) when
    // fewer than out.size() bytes remain. I/O failures throw.
    [[nodiscard]] bool tryRead(std::span<Byte> out) {
        if (out.size() > remaining_) return false;
        readExact(out);
        remaining_ -= out.size();
        return true;
    }

protected:
    explicit CipherSource(std::size_t size) noexcept : remaining_(size) {}

    // Called only with out.size() <= remaining().
    virtual void readExact(std::span<Byte> out) = 0;

private:
    std::size_t remaining_{0};
};

class MemoryCipherSource final : public CipherSource {
public:
    // Reads a buffer the caller keeps alive...
    explicit MemoryCipherSource(std::span<const Byte> data) noexcept
        : CipherSource(data.size()), data_(data) {}
    // ...or one this source owns.
    explicit MemoryCipherSource(vBytes owned) noexcept
        : CipherSource(owned.size()), owned_(std::move(owned)), data_(owned_) {}

private:
    void readExact(std::span<Byte> out) override {
        std::ranges::copy(data_.subspan(position_, out.size()), out.begin());
        position_ += out.size();
    }

    vBytes owned_;
    std::span<const Byte> data_;
    std::size_t position_{0};
};
