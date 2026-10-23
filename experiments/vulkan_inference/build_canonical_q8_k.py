#!/usr/bin/env python3
"""Build six isolated MMVQ shader variants matching official CUDA/Rust K-quant math.

This does not modify a llama checkout, the existing archive, or a release binary.
The caller separately embeds these arrays into its diagnostic archive.
"""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import json

PIN = '3fd3fb25cfdb3214eeb8659bbeaf0bc0f6d095d9314ca7536933d7cafd52bcf6'
OLD = '    return FLOAT_TYPE(float(cache_b_ds.x) * float(dm_scale.x) * float(q_sum) - float(dm_scale.y) * float(cache_b_ds.y / 2));'
NEW = '''    // Match official CUDA MMVQ / Rust canonical_dot: derive the minimum
    // correction from actual integer activations, not the rounded Q8 half sum.
    const int32_t sum_q = dotPacked4x8EXT(int32_t(0x01010101), cache_b_qs[0]) +
                         dotPacked4x8EXT(int32_t(0x01010101), cache_b_qs[1]) +
                         dotPacked4x8EXT(int32_t(0x01010101), cache_b_qs[2]) +
                         dotPacked4x8EXT(int32_t(0x01010101), cache_b_qs[3]);
    return FLOAT_TYPE(float(cache_b_ds.x) * (float(dm_scale.x) * float(q_sum) - float(dm_scale.y) * float(sum_q)));'''


def correct(source):
    if hashlib.sha256(source.encode()).hexdigest() != PIN or source.count(OLD) != 1:
        raise ValueError('Unexpected pinned K-quant shader source')
    return source.replace(OLD, NEW, 1)


def replace_arrays(original, output, shaders):
    replaced = set()
    skipping = False
    with original.open() as lines, output.open('w') as out:
        for line in lines:
            if skipping:
                if line.strip() == '};': skipping = False
                continue
            match = re.fullmatch(r'const uint64_t (\w+)_len = \d+;\n', line)
            if match and match[1] in shaders:
                name = match[1]; data = shaders[name].read_bytes()
                if len(data) % 4 or data[:4] != b'\x03\x02\x23\x07':
                    raise ValueError('Malformed generated SPIR-V')
                following = next(lines)
                if not following.startswith('const unsigned char ' + name + '_data['):
                    raise ValueError('Unpaired shader array')
                out.write(f'const uint64_t {name}_len = {len(data)};\n')
                out.write(f'const unsigned char {name}_data[{len(data)}] = {{\n')
                for offset in range(0, len(data), 12):
                    out.write(''.join(f'0x{v:x},' for v in data[offset:offset + 12]) + '\n')
                out.write('};\n'); skipping = True; replaced.add(name)
            else:
                out.write(line)
    if skipping or replaced != set(shaders):
        raise ValueError('Not every selected shader array was replaced')


def build(source, generated, output):
    if output.exists(): raise ValueError('New isolated output directory required')
    output.mkdir(parents=True)
    for name in ('types.glsl', 'mul_mat_vecq.comp', 'mul_mat_vec_base.glsl', 'mul_mat_vec_iface.glsl'):
        shutil.copyfile(source / name, output / name)
    (output / 'mul_mat_vecq_funcs.glsl').write_text(correct((source / 'mul_mat_vecq_funcs.glsl').read_text()))
    shaders = {}; commands = []
    for kind in ('q4_k', 'q5_k'):
        for suffix, flag in (('', None), ('_subgroup', 'USE_SUBGROUP_ADD'), ('_subgroup_no_shmem', 'USE_SUBGROUP_ADD_NO_SHMEM')):
            name = 'mul_mat_vec_' + kind + '_q8_1_f32' + suffix
            path = output / (name + '.spv')
            defines = ['DATA_A_' + kind.upper() + '=1', 'D_TYPE=float', 'FLOAT_TYPE=float', 'FLOAT_TYPEV2=vec2', 'ACC_TYPE=float']
            if flag: defines.append(flag + '=1')
            command = ['glslc', '--target-env=vulkan1.2', '-O', '-I' + str(output), *['-D' + d for d in defines],
                       str(output / 'mul_mat_vecq.comp'), '-o', str(path)]
            commands.append(command)
            subprocess.run(command, check=True)
            shaders[name] = path
    shutil.copyfile(generated.parent / 'ggml-vulkan-shaders.hpp', output / 'ggml-vulkan-shaders.hpp')
    replace_arrays(generated, output / generated.name, shaders)
    manifest = {'source_sha256': PIN, 'correction': 'integer activation sum for Q4_K/Q5_K MMVQ minimum term',
                'shaders': {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in shaders.items()},
                'commands': commands, 'production_runtime_modified': False}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')


if __name__ == '__main__':
    if len(sys.argv) != 4:
        raise SystemExit('usage: build_canonical_q8_k.py PINNED_SHADER_DIR ORIGINAL_GENERATED_CPP NEW_OUTPUT_DIRECTORY')
    build(*map(Path, sys.argv[1:]))
