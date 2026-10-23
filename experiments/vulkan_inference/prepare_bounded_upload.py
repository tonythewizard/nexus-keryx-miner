#!/usr/bin/env python3
"""Patch a separate copy of the pinned loader's synchronous GPU upload fallback."""
import hashlib
from pathlib import Path
import sys

PIN = 'c647b037af76291989082ad739d881a70a48ab2caf4cdc35bdc8e96223335cb5'
OLD = '''                    read_buf.resize(n_size);
                    file->seek(weight->offs, SEEK_SET);
                    file->read_raw(read_buf.data(), n_size);
                    ggml_backend_tensor_set(cur, read_buf.data(), 0, n_size);
                    if (check_tensors && !ggml_validate_row_data(cur->type, read_buf.data(), n_size)) {
                        throw std::runtime_error(format("tensor '%s' has invalid data", ggml_get_name(cur)));
                    }'''
NEW = '''                    file->seek(weight->offs, SEEK_SET);
                    const bool complete = nexus_trace_fixture::bounded_upload(n_size, ggml_row_size(cur->type, cur->ne[0]),
                        [&](void * dst, size_t count) { file->read_raw(dst, count); },
                        [&](const void * data, size_t offset, size_t count) { ggml_backend_tensor_set(cur, data, offset, count); },
                        [&](const void * data, size_t count) { return !check_tensors || ggml_validate_row_data(cur->type, data, count); },
                        [&](size_t offset) { return !progress_callback || progress_callback((float) (size_done + offset) / size_data, progress_callback_user_data); });
                    if (!complete) return false;'''


def prepare(source):
    if hashlib.sha256(source.encode()).hexdigest() != PIN or source.count(OLD) != 1:
        raise ValueError('Unexpected pinned model loader')
    header = Path(__file__).with_name('bounded_upload.hpp').resolve()
    return '#include "' + str(header) + '"\n' + source.replace(OLD, NEW, 1)


def write_overlay(source, output, replace=False):
    if source.resolve() == output.resolve() or output.is_symlink():
        raise ValueError('Separate regular overlay output required')
    if output.exists() and not replace:
        raise ValueError('New separate output required')
    prepared = prepare(source.read_text())
    output.write_text(prepared)


if __name__ == '__main__':
    if len(sys.argv) not in (3, 4) or (len(sys.argv) == 4 and sys.argv[3] != '--replace-output'):
        raise SystemExit('usage: prepare_bounded_upload.py PINNED_LOADER OUTPUT [--replace-output]')
    source, output = map(Path, sys.argv[1:3])
    write_overlay(source, output, replace=len(sys.argv) == 4)
