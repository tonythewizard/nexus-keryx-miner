#pragma once
// Descriptor-local, byte-preserving fallback upload. No tensor math changes.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
namespace nexus_trace_fixture {
template<class Read, class Upload, class Validate, class Progress>
bool bounded_upload(size_t total, size_t row, Read read, Upload upload, Validate validate, Progress progress) {
    constexpr size_t limit = 4u << 20;
    if (!row || row > limit || total % row) throw std::runtime_error("Unsupported bounded tensor upload shape");
    const size_t capacity = std::min(total, limit / row * row);
    std::vector<uint8_t> buffer(capacity);
    for (size_t offset = 0; offset < total;) {
        if (!progress(offset)) return false;
        const size_t count = std::min(capacity, total - offset);
        read(buffer.data(), count);
        if (!validate(buffer.data(), count)) throw std::runtime_error("Invalid quantized tensor upload data");
        upload(buffer.data(), offset, count);
        offset += count;
    }
    return true;
}
}
