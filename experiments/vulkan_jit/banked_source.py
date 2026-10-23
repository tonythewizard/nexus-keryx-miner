"""Specialize storage layout at runtime, retaining one CUDA arithmetic source.

This deliberately bounded adapter fails if the canonical tile-load shape changes.
The existing typed Clang frontend subsequently validates the derived CUDA source.
It does not maintain a GLSL or independent mathematical implementation.
"""
import hashlib
from pathlib import Path

SIGNATURE = "const std::uint8_t *tiles,\n                                   std::uint64_t ntiles,"
LOAD = """    const std::size_t tile_base =
        static_cast<std::size_t>(offset) * tile_bytes_cuda;
    for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
      tile[(i / 32) * tile_row_stride_cuda + i % 32] = tiles[tile_base + i];"""
BANKED_LOAD = """    // Host proves each bank fits a 32-bit descriptor range.
    // Subtract the bank base before narrowing the canonical 64-bit offset.
    if (offset < split_tiles) {
      const std::uint32_t tile_base = static_cast<std::uint32_t>(offset) * 1024U;
      for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
        tile[(i / 32) * tile_row_stride_cuda + i % 32] = tiles0[tile_base + i];
    } else {
      const std::uint32_t tile_base = static_cast<std::uint32_t>(offset - split_tiles) * 1024U;
      for (unsigned i = tid; i < tile_bytes_cuda; i += blockDim.x)
        tile[(i / 32) * tile_row_stride_cuda + i % 32] = tiles1[tile_base + i];
    }"""


def generate(source: Path, cache: Path):
    raw = source.read_bytes()
    text = raw.decode("utf-8")
    if (text.count(SIGNATURE) != 1 or text.count(LOAD) != 1
            or text.count("tiles[") != 1
            or text.count("pom_v4_resident_walk_kernel") != 1):
        raise ValueError("Canonical CUDA storage shape changed; bank specialization refused")
    source_sha = hashlib.sha256(raw).hexdigest()
    adapter_sha = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    derived = text.replace("pom_v4_resident_walk_kernel", "pom_v4_banked_resident_walk_kernel")
    derived = derived.replace(SIGNATURE, "const std::uint8_t *tiles0,\n"
                              "                                   const std::uint8_t *tiles1,\n"
                              "                                   std::uint64_t ntiles,\n"
                              "                                   std::uint64_t split_tiles,")
    derived = derived.replace(LOAD, BANKED_LOAD)
    derived = (f"// Derived at runtime: canonical_sha256={source_sha}\n"
               f"// storage_adapter_sha256={adapter_sha}\n" + derived)
    directory = cache / "banked-sources" / source_sha / adapter_sha
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / "banked.cu"
    if path.exists():
        if path.read_text() != derived:
            raise ValueError("Derived CUDA cache contents differ from source identity")
    else:
        # Publish a complete source once; concurrent launches may safely reuse it.
        import os
        temporary = directory / f"banked-{os.getpid()}.new"
        try:
            temporary.write_text(derived)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
    return path, {"canonical_sha256": source_sha, "adapter_sha256": adapter_sha}
