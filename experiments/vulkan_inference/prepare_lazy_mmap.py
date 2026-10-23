"""Disable eager whole-model prefaulting in a separate pinned Vulkan overlay."""
import hashlib
from pathlib import Path
import sys

PIN = '42d1d9c382dc7f287db7b5121fbf9ca8da339b2cd10726082a682d2d826548d4'
OLD = 'ml.init_mappings(true, use_mlock ? &pimpl->mlock_mmaps : nullptr);'
NEW = 'ml.init_mappings(false, use_mlock ? &pimpl->mlock_mmaps : nullptr);'


def prepare(source):
    if hashlib.sha256(source.encode()).hexdigest() != PIN or source.count(OLD) != 1:
        raise ValueError('Unexpected pinned model source')
    return source.replace(OLD, NEW, 1)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: prepare_lazy_mmap.py PINNED_MODEL SEPARATE_OUTPUT')
    source, output = map(Path, sys.argv[1:])
    if source.resolve() == output.resolve() or output.is_symlink():
        raise SystemExit('Separate regular overlay required')
    output.write_text(prepare(source.read_text()))
