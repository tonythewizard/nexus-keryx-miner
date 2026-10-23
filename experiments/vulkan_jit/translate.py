#!/usr/bin/env python3
"""Experimental runtime CUDA subset -> GLSL -> validated Vulkan SPIR-V.

No fixed shader or kernel-name lookup. Not a CUDA compatibility runtime and
not connected to mining. Unsupported source is rejected; never falls back to
previous code after a failed translation. Flat integer buffer kernels and a
typed Clang AST subset for shared-memory integer walks are admitted.
"""
import argparse
import fcntl
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


class Unsupported(ValueError):
    pass


TYPES = {'std::uint8_t': 'uint8_t', 'std::uint32_t': 'uint',
         'std::int32_t': 'int', 'std::uint64_t': 'uint64_t',
         'std::size_t': 'uint64_t', 'unsigned': 'uint', 'int': 'int'}


def extract(source, entry):
    # Comments cannot participate in delimiter matching. String literals are
    # forbidden inside the admitted kernel (including inline assembly).
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    pattern = r'extern\s+"C"\s+__global__\s+void\s+' + re.escape(entry) + r'\s*\('
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1:
        raise Unsupported('Expected exactly one extern C global kernel: ' + entry)
    start = matches[0].end()
    end = source.find(')', start)
    if end < 0 or '(' in source[start:end]:
        raise Unsupported('Unsupported kernel parameter syntax')
    pos = end + 1
    while pos < len(source) and source[pos].isspace():
        pos += 1
    if pos == len(source) or source[pos] != '{':
        raise Unsupported('Missing kernel body')
    first = pos + 1
    depth = 1
    while depth and pos + 1 < len(source):
        pos += 1
        depth += (source[pos] == '{') - (source[pos] == '}')
    if depth:
        raise Unsupported('Unclosed kernel body')
    return source, source[start:end], source[first:pos]


def lower(source, entry, local_size, bounded_index32=False, frontend_directory=None):
    if not re.fullmatch(r'[A-Za-z_]\w*', entry) or not 1 <= local_size <= 1024:
        raise Unsupported('Invalid entry point or local size')
    clean, signature, body = extract(source, entry)
    includes = re.findall(r'^\s*#\s*include\s+([^\n]+)', clean, re.M)
    if any(include.strip() not in ('<cstddef>', '<cstdint>') for include in includes):
        raise Unsupported('External include dependencies require a preprocessing/AST frontend')
    if re.search(r'^\s*#\s*(?!include\b)\w+', clean, re.M):
        raise Unsupported('Preprocessor directives require a preprocessing/AST frontend')
    if '__shared__' in body:
        if frontend_directory is None:
            raise Unsupported('Shared-memory kernels require the persistent Clang AST frontend')
        spec = importlib.util.spec_from_file_location('nexus_jit_ast_frontend', Path(__file__).with_name('ast_lower.py'))
        if spec is None or spec.loader is None:
            raise Unsupported('Clang AST frontend is unavailable')
        frontend = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(frontend)
        try:
            return frontend.lower(source, entry, local_size, bounded_index32, frontend_directory)
        except frontend.AstUnsupported as error:
            raise Unsupported(str(error)) from error
    if 'std::size_t' in body and not bounded_index32:
        raise Unsupported('size_t indexing requires an explicit bounded-index32 host contract')
    types_map = dict(TYPES)
    if bounded_index32:
        types_map['std::size_t'] = 'uint'
    if re.search(r'asm|__shared__|__device__|__sync|__shfl|__ballot|__cvta|atomic|\b(?:float|double|half)\b|["#]', body):
        raise Unsupported('Kernel requires assembly, shared memory, barriers or intrinsics outside the admitted subset')
    declarations, arguments, scalar_names = [], [], []
    offset = 0
    types = '|'.join(re.escape(t) for t in sorted(TYPES, key=len, reverse=True))
    for arg in signature.split(','):
        match = re.fullmatch(r'\s*(const\s+)?(' + types + r')\s*(\*)?\s*(\w+)\s*', arg)
        if not match:
            raise Unsupported('Unsupported argument: ' + arg.strip())
        const, typ, pointer, name = match.groups()
        if name.startswith(('gl_', 'nexus_')) or any(a['name'] == name for a in arguments):
            raise Unsupported('Reserved or duplicate argument name')
        if pointer:
            binding = sum(a['kind'] == 'buffer' for a in arguments)
            declarations.append(f'layout(set=0,binding={binding},std430) '
                                f'{"readonly " if const else ""}buffer nexus_buffer_{binding}'
                                f' {{ {TYPES[typ]} {name}[]; }};')
            arguments.append({'name': name, 'kind': 'buffer', 'binding': binding,
                              'cuda_type': typ, 'readonly': bool(const)})
        else:
            if typ not in ('std::uint32_t', 'unsigned', 'std::int32_t', 'int'):
                raise Unsupported('Only 32-bit scalar push constants are admitted')
            scalar_names.append((name, TYPES[typ], offset))
            arguments.append({'name': name, 'kind': 'scalar', 'offset': offset, 'cuda_type': typ})
            offset += 4
    if offset > 128:
        raise Unsupported('Push constants exceed portable 128-byte limit')
    if scalar_names:
        fields = ' '.join(f'layout(offset={off}) {typ} {name};' for name, typ, off in scalar_names)
        declarations.append('layout(push_constant) uniform nexus_parameters { ' + fields + ' } nexus_args;')
    # Only plain integer constexpr definitions are imported, when referenced.
    # No eval, macros, includes, executable compiler hooks or shell expansion.
    constants = []
    for match in re.finditer(r'constexpr\s+(' + types + r')\s+(\w+)\s*=\s*(0[xX][0-9a-fA-F]+|[0-9]+)(?:ULL|UL|LL|U|L)?\s*;', clean):
        typ, name, value = match.groups()
        if re.search(r'\b' + re.escape(name) + r'\b', body):
            constants.append(f'const {types_map[typ]} {name} = {types_map[typ]}({value});')
    body = re.sub(r'static_cast<\s*(' + types + r')\s*>\s*\(', lambda m: types_map[m[1]] + '(', body)
    for original, replacement in sorted(types_map.items(), key=lambda pair: -len(pair[0])):
        body = re.sub(r'\b' + re.escape(original) + r'\b', replacement, body)
    for name, _, _ in scalar_names:
        body = re.sub(r'\b' + re.escape(name) + r'\b', 'nexus_args.' + name, body)
    for cuda, vulkan in [('threadIdx', 'gl_LocalInvocationID'), ('blockIdx', 'gl_WorkGroupID'),
                         ('blockDim', 'gl_WorkGroupSize'), ('gridDim', 'gl_NumWorkGroups')]:
        body = re.sub(r'\b' + cuda + r'\s*\.\s*([xyz])\b', vulkan + r'.\1', body)
    # Pointer aliasing/arithmetic, C++ casts and function calls not handled by
    # this lowering must not accidentally acquire different GLSL semantics.
    calls = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body))
    if calls - {'for', 'if', 'while', 'uint', 'int', 'uint8_t', 'uint64_t'}:
        raise Unsupported('Unsupported calls: ' + ', '.join(sorted(calls - {'for', 'if', 'while', 'uint', 'int', 'uint8_t', 'uint64_t'})))
    if re.search(r'\b(?:auto|static_cast|reinterpret_cast|std|threadIdx|blockIdx|blockDim|gridDim)\b|->|&|\*\s*[A-Za-z_]\w*\s*=', body):
        raise Unsupported('Unsupported pointer or C++ expression')
    shader = '\n'.join(['#version 450',
        '#extension GL_EXT_shader_8bit_storage : require',
        '#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require',
        '#extension GL_ARB_gpu_shader_int64 : require',
        f'layout(local_size_x={local_size},local_size_y=1,local_size_z=1) in;',
        *constants, *declarations, 'void main() {', body, '}']) + '\n'
    return shader, arguments


def digest(data):
    return hashlib.sha256(data).hexdigest()


def compile_kernel(source_path, entry, cache, local_size=128, bounded_index32=False):
    started = time.monotonic_ns()
    # Cache/temp paths must be explicit persistent paths, never system temp.
    cache = Path(cache).expanduser().resolve()
    if any(cache == p or p in cache.parents for p in (Path('/tmp'), Path('/var/tmp'), Path('/dev/shm'))):
        raise ValueError('Cache must use persistent storage outside temporary directories')
    cache.mkdir(parents=True, exist_ok=True)
    if Path(source_path).stat().st_size > 1024 * 1024:
        raise Unsupported('Source exceeds experimental 1 MiB bound')
    source = Path(source_path).read_bytes()
    if not re.fullmatch(r'[A-Za-z_]\w*', entry) or not 1 <= local_size <= 1024:
        raise Unsupported('Invalid entry point or local size')
    source_text = source.decode('utf-8')
    compiler = Path(shutil.which('glslangValidator') or '').resolve()
    optimizer = Path(shutil.which('spirv-opt') or '').resolve()
    validator = Path(shutil.which('spirv-val') or '').resolve()
    if not compiler.is_file() or not optimizer.is_file() or not validator.is_file():
        raise RuntimeError('glslangValidator, spirv-opt and spirv-val are required')
    clang = shutil.which('clang++')
    identity = {'schema': 3, 'source_sha256': digest(source), 'entry': entry,
                'local_size': local_size, 'bounded_index32': bounded_index32,
                'translator_sha256': digest(Path(__file__).read_bytes()),
                'ast_lower_sha256': digest(Path(__file__).with_name('ast_lower.py').read_bytes()),
                'clang_sha256': digest(Path(clang).read_bytes()) if clang and '__shared__' in source_text else None,
                'compiler_sha256': digest(compiler.read_bytes()),
                'optimizer_sha256': digest(optimizer.read_bytes()),
                'validator_sha256': digest(validator.read_bytes()), 'target': 'vulkan1.2'}
    key = digest(json.dumps(identity, sort_keys=True).encode())
    target = cache / key
    target.mkdir(exist_ok=True)
    env = dict(os.environ, TMPDIR=str(target), TMP=str(target), TEMP=str(target))
    with (target / 'lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        manifest_path = target / 'manifest.json'
        binary = target / 'kernel.spv'
        hit = False
        if manifest_path.exists() and binary.exists():
            manifest = json.loads(manifest_path.read_text())
            hit = manifest.get('identity') == identity and manifest.get('spirv_sha256') == digest(binary.read_bytes())
        if not hit:
            manifest_path.unlink(missing_ok=True)
            frontend = cache / ('frontend-' + digest(source + entry.encode() + str(local_size).encode()))
            with (cache / 'frontend.lock').open('a') as frontend_lock:
                fcntl.flock(frontend_lock, fcntl.LOCK_EX)
                lowered = lower(source_text, entry, local_size, bounded_index32, frontend)
                shader, arguments = lowered[:2]
                reflection = lowered[2] if len(lowered) > 2 else {'frontend': 'flat-subset'}
            shader_path = target / 'kernel.comp'
            shader_path.write_text(shader)
            command = [str(compiler), '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-o', str(target / 'raw.spv'), str(shader_path)]
            result = subprocess.run(command, env=env, capture_output=True, text=True, timeout=30)
            (target / 'compiler.log').write_text(result.stdout + result.stderr)
            if result.returncode:
                raise Unsupported('GLSL lowering rejected; see ' + str(target / 'compiler.log'))
            subprocess.run([str(optimizer), '-O', '--target-env=vulkan1.2', str(target / 'raw.spv'), '-o', str(target / 'pending.spv')], env=env, check=True, capture_output=True, timeout=20)
            (target / 'raw.spv').unlink(missing_ok=True)
            subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(target / 'pending.spv')], env=env, check=True, capture_output=True, timeout=10)
            (target / 'pending.spv').replace(binary)
            manifest = {'identity': identity, 'arguments': arguments, 'spirv_sha256': digest(binary.read_bytes()),
                        'reflection': reflection,
                        'required_features': ['shaderInt64', 'shaderInt8', 'storageBuffer8BitAccess'] +
                            (['shaderIntegerDotProduct'] if 'GL_EXT_integer_dot_product : require' in shader else []),
                        'status': 'EXPERIMENTAL_NOT_MINING_ATTESTED'}
            pending = target / 'manifest.pending'
            pending.write_text(json.dumps(manifest, indent=2) + '\n')
            pending.replace(manifest_path)
        subprocess.run([str(validator), '--target-env', 'vulkan1.2', str(binary)], env=env, check=True, capture_output=True, timeout=10)
    return {'cache_hit': hit, 'elapsed_ms': (time.monotonic_ns() - started) / 1e6,
            'cache_key': key, 'spirv': str(binary), 'manifest': str(manifest_path)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--entry', required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--local-size', type=int, default=128)
    parser.add_argument('--bounded-index32', action='store_true',
                        help='Caller guarantees every size_t calculation fits uint32 (experimental host contract)')
    args = parser.parse_args()
    try:
        print(json.dumps(compile_kernel(args.source, args.entry, args.cache, args.local_size, args.bounded_index32), indent=2))
    except (Unsupported, ValueError, OSError, subprocess.SubprocessError) as error:
        parser.exit(2, 'BLOCKED: ' + str(error) + '\n')
