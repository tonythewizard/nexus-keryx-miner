#pragma once
// Offline capture of actual single-row Vulkan matmul inputs and graph outputs.
// This is deliberately separate from the production inference ABI/activation gate.
#include "ggml.h"
#include "ggml-backend.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

extern "C" int nexus_vk_q8_probe_arm_for(const char *);
extern "C" int nexus_vk_q8_probe_read(uint8_t *, size_t);

namespace nexus_trace_fixture {
struct Product {
    std::string name;
    uint32_t rank{}, type{}, partner{}, bias{};
    uint64_t k{}, m{};
    uint8_t role{}, form{}, glu{};
};
inline void require(bool value, const std::string & message) {
    if (!value) throw std::runtime_error(message);
}
inline uint64_t le(const std::vector<uint8_t> & data, size_t at, size_t bytes) {
    require(bytes <= 8 && at <= data.size() && bytes <= data.size() - at, "Truncated trace manifest");
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value |= uint64_t(data[at + i]) << (8 * i);
    return value;
}
inline std::vector<Product> manifest(const std::vector<uint8_t> & data) {
    require(data.size() >= 16 && !memcmp(data.data(), "KXMANIF1", 8), "Invalid trace manifest magic");
    const auto count = le(data, 8, 4), products = le(data, 12, 4);
    require(count > 0 && count <= 4096 && products > 0 && products <= count, "Invalid manifest count");
    size_t at = 16; std::vector<Product> result; std::set<std::string> names;
    for (uint32_t i = 0; i < count; ++i) {
        const size_t length = le(data, at, 2); at += 2;
        require(length && length < GGML_MAX_NAME && at <= data.size() && length + 55 <= data.size() - at, "Invalid manifest entry");
        std::string name(reinterpret_cast<const char *>(data.data() + at), length); at += length;
        require(name.find('\0') == std::string::npos && names.insert(name).second, "Duplicate/invalid product name");
        if (i < products) {
            Product p{name, i, uint32_t(le(data, at, 4)), uint32_t(le(data, at + 47, 4)),
                uint32_t(le(data, at + 51, 4)), le(data, at + 4, 8), le(data, at + 12, 8),
                data[at + 44], data[at + 46], data[at + 45]};
            require(le(data, at + 20, 4) == 1 && p.k > 0 && p.k <= 32768 && p.k % 32 == 0 && p.m > 0 && p.m <= 1048576,
                "Unsupported product dimensions");
            require((p.type == 12 || p.type == 13 || p.type == 14 || p.type == 2 || p.type == 6 || p.type == 8) &&
                (p.role == 0 || p.role == 2) && p.bias == UINT32_MAX &&
                (p.form == 0 || p.form == 2 || p.form == 3), "Unsupported trace product semantics");
            result.push_back(p);
        }
        at += 55;
    }
    require(at == data.size(), "Trailing manifest bytes");
    for (const auto & p : result) if (p.form != 0) {
        require(p.partner < result.size(), "Invalid GLU partner");
        const auto & other = result[p.partner];
        require(other.partner == p.rank && other.form == (p.form == 2 ? 3 : 2) &&
            other.k == p.k && other.m == p.m && other.glu == p.glu && p.glu == 1,
            "Inconsistent GLU product pair");
    }
    return result;
}

inline std::vector<uint8_t> mmvq(const std::vector<uint8_t> & raw, uint64_t k) {
    require(k > 0 && k <= 32768 && k % 32 == 0 && raw.size() == (k + 127) / 128 * 144, "Invalid captured Q8 shape");
    std::vector<uint8_t> bytes(((k + 511) / 512) * 512 / 32 * 36, 0);
    for (size_t i = 0; i < raw.size() / 144; ++i) for (size_t b = 0; b < 4; ++b) {
        const auto target = (4 * i + b) * 36;
        for (size_t half = 0; half < 2; ++half) {
            const auto at = 144 * i + 4 * b + 2 * half;
            const uint16_t value = uint16_t(raw[at]) | (uint16_t(raw[at + 1]) << 8);
            require((value & 0x7c00) != 0x7c00, "Non-finite captured Q8 scale/sum");
        }
        memcpy(bytes.data() + target, raw.data() + 144 * i + 4 * b, 4);
        memcpy(bytes.data() + target + 4, raw.data() + 144 * i + 16 + 32 * b, 32);
    }
    return bytes;
}

struct Capture {
    std::vector<Product> products;
    std::map<std::string, uint32_t> ranks;
    std::filesystem::path root;
    uint32_t position{};
    uint64_t written{};
    std::set<uint32_t> inputs, outputs;
    std::string failure;
    explicit Capture(std::vector<Product> p, std::filesystem::path directory): products(std::move(p)), root(std::move(directory)) {
        for (auto & e : products) ranks.emplace(e.name, e.rank);
    }
    const Product * product(const ggml_tensor * t) const {
        if (!t || t->op != GGML_OP_MUL_MAT || !t->src[0]) return nullptr;
        auto found = ranks.find(t->src[0]->name);
        return found == ranks.end() ? nullptr : &products[found->second];
    }
    void put(const Product & p, char kind, const std::vector<uint8_t> & bytes) {
        require(bytes.size() <= (8u << 20) && written + bytes.size() <= (512u << 20), "Bounded trace capture exhausted");
        auto & seen = kind == 'x' ? inputs : outputs;
        require(seen.insert(p.rank).second, "Duplicate product capture");
        const auto path = root / std::to_string(position) / (std::to_string(p.rank) + '.' + kind);
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        out.close(); require(bool(out), "Cannot persist trace bytes"); written += bytes.size();
    }
    void output(const Product & p, ggml_tensor * t) {
        require(t->type == GGML_TYPE_F32 && ggml_is_contiguous(t) && ggml_nrows(t) == 1 && uint64_t(t->ne[0]) == p.m,
            "Unsupported captured output layout: " + p.name);
        std::vector<uint8_t> bytes(ggml_nbytes(t));
        ggml_backend_tensor_get(t, bytes.data(), 0, bytes.size());
        for (size_t at = 0; at < bytes.size(); at += 4) {
            float value; memcpy(&value, bytes.data() + at, 4);
            require(std::isfinite(value), "Non-finite captured product output");
        }
        put(p, 'y', bytes);
    }
    void begin(uint32_t pos) {
        require(pos < 64 && failure.empty(), "Invalid trace position");
        position = pos; inputs.clear(); outputs.clear();
        require(std::filesystem::create_directory(root / std::to_string(pos)), "Existing trace position");
    }
    void finish() const {
        require(failure.empty(), failure);
        for (auto & p : products) require(inputs.count(p.rank) && (p.form == 3 || outputs.count(p.rank)),
            "Incomplete trace at position " + std::to_string(position) + ": " + p.name);
    }
    static bool callback(ggml_tensor * t, bool ask, void * userdata) noexcept {
        auto & c = *static_cast<Capture *>(userdata);
        try {
            if (!c.failure.empty()) return false;
            if (const auto * p = c.product(t)) {
                require(t->src[1] && t->src[1]->type == GGML_TYPE_F32 && uint64_t(t->src[1]->ne[0]) == p->k &&
                    ggml_nrows(t->src[1]) == 1 && ggml_is_contiguous(t->src[1]) && t->src[0]->ne[2] == 1 && t->src[0]->ne[3] == 1 &&
                    t->src[0]->type == p->type && uint64_t(t->src[0]->ne[0]) == p->k && uint64_t(t->src[0]->ne[1]) == p->m,
                    "Manifest/graph binding mismatch: " + p->name);
                if (ask) {
                    require(nexus_vk_q8_probe_arm_for(p->name.c_str()) == 0, "Cannot arm Q8 capture");
                } else {
                    std::vector<uint8_t> raw((p->k + 127) / 128 * 144);
                    const auto size = nexus_vk_q8_probe_read(raw.data(), raw.size());
                    require(size > 0 && size_t(size) == raw.size(), "Actual Vulkan Q8 capture unavailable: " + p->name + " code=" + std::to_string(size));
                    c.put(*p, 'x', mmvq(raw, p->k));
                    if (p->form == 0) c.output(*p, t);
                }
                return true;
            }
            if (t->op == GGML_OP_GLU) {
                const auto * p = c.product(t->src[0]);
                if (p && p->form == 2) {
                    const auto * up = c.product(t->src[1]);
                    require(up && up->rank == p->partner && ggml_get_glu_op(t) == GGML_GLU_OP_SWIGLU,
                        "GLU graph/manifest mismatch");
                    if (!ask) c.output(*p, t);
                    return true;
                }
            }
            return false;
        } catch (const std::exception & e) { c.failure = e.what(); return false; }
          catch (...) { c.failure = "Unexpected trace callback failure"; return false; }
    }
};
}
