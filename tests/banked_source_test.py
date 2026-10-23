import os
from pathlib import Path
import sys
import tempfile
import unittest

SOURCE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SOURCE / "experiments/vulkan_jit"))
from banked_source import generate, LOAD


class BankedSourceTest(unittest.TestCase):
    def test_padded_tile_preserves_bytes_and_distributes_column_banks(self):
        positions = [(i // 32) * 33 + i % 32 for i in range(1024)]
        self.assertEqual(len(set(positions)), 1024)
        self.assertLess(max(positions), 1056)
        self.assertEqual(positions[:32], list(range(32)))  # snippet_fold input
        for k in range(32):
            self.assertEqual(len({(col * 33 + k) % 32 for col in range(32)}), 32)
            self.assertEqual(len({(col * 32 + k) % 32 for col in range(32)}), 1)
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=os.environ["TMPDIR"])
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.original = (SOURCE / "consensus/cuda_portable_resident.cu").read_text()
        self.source = self.root / "canonical.cu"
        self.source.write_text(self.original)

    def test_shared_offset_is_reflected_without_narrowing_its_storage(self):
        from ast_lower import lower
        out, _ = generate(self.source, self.root / "cache")
        shader, _, reflection = lower(out.read_text(), "pom_v4_banked_resident_walk_kernel",
                                 256, True, self.root / "ast")
        self.assertEqual(reflection["shared_arrays"], {
            "state": 512, "tile": 288, "next_offset": 8})
        self.assertEqual(reflection["shared_memory_bytes"], 3232)
        self.assertEqual(shader.count("nexus_barrier();"), 3)

    def test_packing_requires_exact_entry_and_restores_byte_layout_on_change(self):
        from ast_lower import lower
        out, _ = generate(self.source, self.root / "cache")
        source = out.read_text()
        packed, _, reflection = lower(source, "pom_v4_banked_resident_walk_kernel",
                                     256, True, self.root / "ast_packed")
        self.assertIn("dotPacked4x8EXT", packed)
        self.assertEqual(reflection["shared_storage_layout"], "packed-s8x4-v1")
        self.assertTrue(reflection["offset_suppression"])
        # An otherwise safe extra signed read must not inherit changed array indices.
        source = source.replace("const unsigned tid = threadIdx.x;",
                                "const unsigned tid = threadIdx.x;\nconst int unused = s8_d(0);")
        ordinary, _, reflection = lower(source, "pom_v4_banked_resident_walk_kernel",
                                       256, True, self.root / "ast_ordinary")
        self.assertNotIn("dotPacked4x8EXT", ordinary)
        self.assertEqual(reflection["shared_storage_layout"], "uint-per-byte")
        self.assertEqual(reflection["shared_memory_bytes"], 12448)

    def test_storage_specialization_preserves_math_helpers(self):
        out, _ = generate(self.source, self.root / "cache")
        generated = out.read_text().splitlines(keepends=True)[2:]
        generated = "".join(generated)
        self.assertEqual(generated.split('extern "C"')[0], self.original.split('extern "C"')[0])
        self.assertIn("offset - split_tiles", generated)
        self.assertIn("std::uint64_t split_tiles", generated)
        self.assertEqual(self.source.read_text(), self.original)

    def test_source_math_change_reaches_derived_cuda_and_invalidates_cache(self):
        first, a = generate(self.source, self.root / "cache")
        self.source.write_text(self.original.replace("0x9E3779B9U", "0x9E3779B8U"))
        second, b = generate(self.source, self.root / "cache")
        self.assertNotEqual(a["canonical_sha256"], b["canonical_sha256"])
        self.assertNotEqual(first, second)
        self.assertIn("0x9E3779B8U", second.read_text())

    def test_unknown_tile_access_shape_is_rejected(self):
        self.source.write_text(self.original.replace(LOAD, "tile[tid] = tiles[tid];"))
        with self.assertRaises(ValueError):
            generate(self.source, self.root / "cache")

    def test_extra_tile_access_is_rejected(self):
        self.source.write_text(self.original + "\n// tiles[unaccounted]\n")
        with self.assertRaises(ValueError):
            generate(self.source, self.root / "cache")

    def test_cache_reuse_and_tampering(self):
        out, a = generate(self.source, self.root / "cache")
        same, b = generate(self.source, self.root / "cache")
        self.assertEqual((out, a), (same, b))
        out.write_text("corrupted source cache")
        with self.assertRaises(ValueError):
            generate(self.source, self.root / "cache")

    def test_non_multiple_thread_stride_bound_is_guarded(self):
        from ast_lower import lower
        src = """#include <cstdint>
extern "C" __global__ void test_kernel(std::uint8_t *out) {
  const unsigned tid = threadIdx.x;
  for (unsigned i = tid; i < 257U; i += blockDim.x)
    out[i] = static_cast<std::uint8_t>(i);
}
"""
        shader, _, _ = lower(src, "test_kernel", 256, True, self.root / "ast_guard")
        self.assertIn("if ((i < 257u))", shader)

    def test_s8_pre_sign_extension_requires_verified_ast_pattern(self):
        from ast_lower import lower
        out, _ = generate(self.source, self.root / "cache")
        canonical_shader, _, _ = lower(out.read_text(), "pom_v4_banked_resident_walk_kernel",
                                       256, True, self.root / "ast_ok")
        self.assertIn("uint nexus_u8(uint x) { return uint(int(int8_t(x))); }", canonical_shader)
        self.assertIn("int s8_d(int x) { return x; }", canonical_shader)
        # If s8_d body or shared usage changes, pre-sign-extension must not be blindly applied.
        src = """#include <cstdint>
namespace {
__device__ int s8_d(int x) { return (x ^ 64) - 64; }
}
extern "C" __global__ void custom_s8_kernel(std::uint8_t *out) {
  __shared__ std::uint8_t buf[256];
  const unsigned tid = threadIdx.x;
  buf[tid] = static_cast<std::uint8_t>(tid);
  __syncthreads();
  out[tid] = static_cast<std::uint8_t>(s8_d(static_cast<int>(buf[tid])));
}
"""
        fallback_shader, _, _ = lower(src, "custom_s8_kernel", 256, True, self.root / "ast_fallback")
        self.assertIn("uint nexus_u8(uint x) { return (x & 255u); }", fallback_shader)
        self.assertNotIn("int s8_d(int x) { return x; }", fallback_shader)
        self.assertIn("(((x ^ 64)) - 64)", fallback_shader)

        # Unsigned equality comparison `if (state[0] == 255U)` must disable pre-sign-extension.
        eq_src = """#include <cstdint>
namespace {
__device__ int s8_d(int x) { return (x ^ 128) - 128; }
}
extern "C" __global__ void eq_kernel(std::uint8_t *out) {
  __shared__ std::uint8_t state[256];
  const unsigned tid = threadIdx.x;
  state[tid] = static_cast<std::uint8_t>(tid);
  __syncthreads();
  if (state[0] == 255U)
    out[tid] = static_cast<std::uint8_t>(s8_d(static_cast<int>(state[tid])));
}
"""
        eq_shader, _, _ = lower(eq_src, "eq_kernel", 256, True, self.root / "ast_eq")
        self.assertIn("uint nexus_u8(uint x) { return (x & 255u); }", eq_shader)
        self.assertNotIn("int s8_d(int x) { return x; }", eq_shader)
        # Arbitrary unsigned function argument `some_unsigned_function(0U, state[0])` must disable pre-sign-extension.
        fn_src = """#include <cstdint>
namespace {
__device__ int s8_d(int x) { return (x ^ 128) - 128; }
__device__ std::uint32_t some_unsigned_function(std::uint32_t a, std::uint32_t b) { return a + b; }
}
extern "C" __global__ void fn_kernel(std::uint8_t *out) {
  __shared__ std::uint8_t state[256];
  const unsigned tid = threadIdx.x;
  state[tid] = static_cast<std::uint8_t>(tid);
  __syncthreads();
  const std::uint32_t u = some_unsigned_function(0U, state[0]);
  out[tid] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(s8_d(static_cast<int>(state[tid]))) + u);
}
"""
        fn_shader, _, _ = lower(fn_src, "fn_kernel", 256, True, self.root / "ast_fn")
        self.assertIn("uint nexus_u8(uint x) { return (x & 255u); }", fn_shader)
        self.assertNotIn("int s8_d(int x) { return x; }", fn_shader)


if __name__ == "__main__":
    unittest.main()
