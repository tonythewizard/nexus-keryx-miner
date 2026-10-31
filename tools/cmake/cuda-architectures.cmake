# Shared by the PoM images and the separately built inference runtime.
# Native images cover GeForce RTX 30 (Ampere), 40 (Ada), 50 (Blackwell).
# Keep all three present: runtime admission uses the same closed set.
set(CMAKE_CUDA_ARCHITECTURES 86 89 120)
set(NEXUS_CUDA_IMAGE_FLAGS
  --generate-code=arch=compute_86,code=sm_86
  --generate-code=arch=compute_89,code=sm_89
  --generate-code=arch=compute_120,code=sm_120)
