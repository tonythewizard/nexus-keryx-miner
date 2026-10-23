"""CPU-only validation of the exact embedded BDA modules and assembly sources."""
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys

source = Path(__file__).resolve().parents[1]
output = Path(sys.argv[1]).resolve()
if output == Path('/tmp') or Path('/tmp') in output.parents:
    raise ValueError('Persistent output directory required')
output.mkdir(parents=True, exist_ok=True)
manifest = json.loads((source / 'consensus/vulkan-bda/manifest.json').read_text())
header = (source / 'include/keryx/nexus/consensus/pom_v4_bda_spv.hpp').read_text()
for name, item in manifest['modules'].items():
    match = re.search(r'inline constexpr uint32_t ' + name + r'\[(\d+)\] = \{(.*?)\};', header, re.S)
    assert match, name
    words = [int(word, 16) for word in re.findall(r'0x([0-9a-fA-F]+)u', match[2])]
    assert len(words) == int(match[1]), name
    embedded = struct.pack('<' + 'I' * len(words), *words)
    assert hashlib.sha256(embedded).hexdigest() == item['sha256'], name
    binary = output / (name + '.spv')
    binary.write_bytes(embedded)
    subprocess.run(['spirv-val', '--target-env', manifest['target_env'], str(binary)], check=True)
    assembled = output / (name + '.assembled.spv')
    subprocess.run(['spirv-as', '--preserve-numeric-ids', '--target-env', 'spv1.5',
                    str(source / 'consensus/vulkan-bda' / item['source']), '-o', str(assembled)], check=True)
    # spirv-as stamps its own generator. All instructions and IDs must be exact.
    assert assembled.read_bytes()[20:] == embedded[20:], name
    assert bytes.fromhex(item['header_hex']) == embedded[:20], name
print('BDA_SPIRV=PASS modules=2 validation=PASS assembly_instruction_identity=PASS')
