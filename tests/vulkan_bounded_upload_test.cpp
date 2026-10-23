#include "../experiments/vulkan_inference/bounded_upload.hpp"
#include <cstring>
#include <iostream>
#include <functional>
static void check(bool v) { if (!v) throw std::runtime_error("bounded upload contract failed"); }
int main() {
    try {
        for (size_t row : {size_t(32), size_t(3360), size_t(16384)}) {
            const size_t total = row * ((9u << 20) / row + 1);
            std::vector<uint8_t> source(total), actual(total);
            for (size_t i = 0; i < total; ++i) source[i] = (i * 31 + i / 127) % 251;
            size_t at = 0, max = 0, validated = 0, uploaded = 0;
            bool complete = nexus_trace_fixture::bounded_upload(total, row,
                [&](void * dst, size_t n) { check(n <= (4u << 20) && n % row == 0); max = std::max(max, n); memcpy(dst, source.data() + at, n); at += n; },
                [&](const void * data, size_t offset, size_t n) { check(offset == uploaded && validated == uploaded + n); memcpy(actual.data() + offset, data, n); uploaded += n; },
                [&](const void *, size_t n) { validated += n; return true; },
                [&](size_t offset) { return offset == uploaded; });
            check(complete && source == actual && at == total && uploaded == total && max <= (4u << 20));
        }
        bool read = false, upload = false;
        auto r = [&](void *, size_t) { read = true; };
        auto u = [&](const void *, size_t, size_t) { upload = true; };
        auto good = [](const void *, size_t) { return true; };
        check(!nexus_trace_fixture::bounded_upload(32, 32, r, u, good, [](size_t) { return false; }) && !read && !upload);
        bool rejected = false;
        try { nexus_trace_fixture::bounded_upload(32, 32, r, u, [](const void *, size_t) { return false; }, [](size_t) { return true; }); }
        catch (...) { rejected = true; } check(rejected && !upload);
        for (size_t row : {size_t(0), size_t(33), size_t(5u << 20)}) {
            rejected = false; try { nexus_trace_fixture::bounded_upload(32, row, r, u, good, [](size_t) { return true; }); }
            catch (...) { rejected = true; } check(rejected);
        }
        std::cout << "BOUNDED_UPLOAD_BYTE_PARITY=PASS limit=4194304 validation=preserved cancellation=checked\n";
        return 0;
    } catch (const std::exception & e) { std::cerr << e.what() << '\n'; return 1; }
}
