"""Ordered, overlapping runtime patch verification; no model or GPU execution."""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'tools/runtime/apply_runtime_patches.cmake'


def digest(data):
    return hashlib.sha256(data).hexdigest()


class RuntimeChain(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(dir=ROOT / '.local/tmp')
        self.addCleanup(temporary.cleanup)
        self.checkout = Path(temporary.name) / 'checkout'
        self.checkout.mkdir()
        subprocess.run(['git', 'init', '--quiet', str(self.checkout)], check=True)
        self.source = self.checkout / 'build/dependency'
        self.source.mkdir(parents=True)
        self.manifest = self.checkout / 'manifest.json'
        (self.checkout / 'patches').mkdir()
        wrapper = b'int fixture_abi() { return 9; }\n'
        (self.checkout / 'wrapper.cpp').write_bytes(wrapper)
        self.a = 'ggml/src/ggml-cuda/q8canon.cu'
        self.b = 'ggml/src/ggml-cuda/q8seq.cu'
        patches = [
            self.add(self.a, 'canon'),
            self.change(self.a, 'canon', 'sequence') + self.add(self.b, 'seq'),
            self.change(self.b, 'seq', 'f32'),
            self.change(self.b, 'f32', 'f64'),
            self.change(self.b, 'f64', 'lse'),
        ]
        states = [{self.a: None, self.b: None}, {self.a: b'canon\n', self.b: None},
                  {self.a: b'sequence\n', self.b: b'seq\n'},
                  {self.a: b'sequence\n', self.b: b'f32\n'},
                  {self.a: b'sequence\n', self.b: b'f64\n'},
                  {self.a: b'sequence\n', self.b: b'lse\n'}]
        self.data = {'schema_version': 1, 'application_abi': 9,
                     'wrapper': {'path': 'wrapper.cpp', 'sha256': digest(wrapper)}, 'patches': [],
                     'stages': [{'name': str(i), 'files': {k: digest(v) if v is not None else None for k, v in s.items()}}
                                for i, s in enumerate(states)]}
        for i, patch in enumerate(patches):
            path = f'patches/{i+1}.patch'
            (self.checkout / path).write_bytes(patch.encode())
            self.data['patches'].append({'path': path, 'sha256': digest(patch.encode())})
        self.save()

    @staticmethod
    def add(name, value):
        return (f'diff --git a/{name} b/{name}\nnew file mode 100644\n'
                f'--- /dev/null\n+++ b/{name}\n@@ -0,0 +1 @@\n+{value}\n')

    @staticmethod
    def change(name, old, new):
        return (f'diff --git a/{name} b/{name}\n--- a/{name}\n+++ b/{name}\n'
                f'@@ -1 +1 @@\n-{old}\n+{new}\n')

    def save(self):
        self.manifest.write_text(json.dumps(self.data))

    def run_chain(self, verify=False):
        env = dict(os.environ, GIT_DIR=str(self.checkout / '.git'), GIT_WORK_TREE=str(self.checkout),
                   GIT_INDEX_FILE=str(self.checkout / '.git/index'))
        return subprocess.run(['cmake', f'-DNEXUS_LLAMA_SOURCE={self.source}',
                               f'-DNEXUS_RUNTIME_MANIFEST={self.manifest}',
                               f'-DNEXUS_PATCH_ROOT={self.checkout}',
                               f'-DNEXUS_VERIFY_ONLY={"ON" if verify else "OFF"}', '-P', str(SCRIPT)],
                              env=env, text=True, capture_output=True)

    def good(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def prefix(self):
        full = json.loads(json.dumps(self.data))
        self.data['patches'] = self.data['patches'][:1]
        self.data['stages'] = self.data['stages'][:2]
        self.save()
        self.good(self.run_chain())
        self.data = full
        self.save()

    def test_overlapping_chain_is_complete_idempotent_and_parent_git_isolated(self):
        self.good(self.run_chain())
        self.assertEqual((self.source / self.a).read_bytes(), b'sequence\n')
        self.assertEqual((self.source / self.b).read_bytes(), b'lse\n')
        self.good(self.run_chain(verify=True))
        self.good(self.run_chain())

    def test_known_abi8_prefix_upgrades_but_is_not_mistaken_for_abi9(self):
        self.prefix()
        self.assertNotEqual(self.run_chain(verify=True).returncode, 0)
        self.good(self.run_chain())
        self.good(self.run_chain(verify=True))

    def test_previous_abi9_four_patch_prefix_requires_the_mass_patch(self):
        full = json.loads(json.dumps(self.data))
        self.data['patches'] = self.data['patches'][:4]
        self.data['stages'] = self.data['stages'][:5]
        self.save()
        self.good(self.run_chain())
        self.assertEqual((self.source / self.b).read_bytes(), b'f64\n')
        self.data = full
        self.save()
        self.assertNotEqual(self.run_chain(verify=True).returncode, 0)
        self.good(self.run_chain())
        self.assertEqual((self.source / self.b).read_bytes(), b'lse\n')

    def test_revised_fifth_patch_does_not_accept_or_mutate_the_previous_revision(self):
        self.good(self.run_chain())
        patch=self.checkout / self.data['patches'][4]['path']
        data=self.change(self.b,'f64','complete-mass').encode()
        patch.write_bytes(data)
        self.data['patches'][4]['sha256']=digest(data)
        self.data['stages'][5]['files'][self.b]=digest(b'complete-mass\n')
        self.save()
        self.assertNotEqual(self.run_chain(verify=True).returncode,0)
        self.assertNotEqual(self.run_chain().returncode,0)
        self.assertEqual((self.source/self.b).read_bytes(),b'lse\n')

    def test_unknown_partial_state_is_not_modified(self):
        self.prefix()
        (self.source / self.a).write_bytes(b'unknown edit\n')
        self.assertNotEqual(self.run_chain().returncode, 0)
        self.assertEqual((self.source / self.a).read_bytes(), b'unknown edit\n')
        self.assertFalse((self.source / self.b).exists())

    def test_corruption_after_final_patch_is_rejected(self):
        self.good(self.run_chain())
        (self.source / self.b).write_bytes(b'modified\n')
        self.assertNotEqual(self.run_chain(verify=True).returncode, 0)
        self.assertNotEqual(self.run_chain().returncode, 0)

    def test_all_patch_inputs_are_checked_before_source_mutation(self):
        (self.checkout / self.data['patches'][-1]['path']).unlink()
        self.assertNotEqual(self.run_chain().returncode, 0)
        self.assertFalse((self.source / self.a).exists())

    def test_patch_and_wrapper_hashes_are_checked_even_on_complete_sources(self):
        self.good(self.run_chain())
        patch = self.checkout / self.data['patches'][0]['path']
        original = patch.read_bytes()
        patch.write_bytes(original + b'\n')
        self.assertNotEqual(self.run_chain(verify=True).returncode, 0)
        patch.write_bytes(original)
        (self.checkout / 'wrapper.cpp').write_bytes(b'wrong ABI\n')
        self.assertNotEqual(self.run_chain(verify=True).returncode, 0)

    def test_reordered_patches_cannot_match_the_pinned_stages(self):
        self.data['patches'][0], self.data['patches'][1] = self.data['patches'][1], self.data['patches'][0]
        self.save()
        self.assertNotEqual(self.run_chain().returncode, 0)
        self.assertFalse((self.source / self.a).exists())

    def test_manifest_source_path_cannot_escape_dependency(self):
        self.data['stages'][0]['files']['../escape'] = None
        self.save()
        self.assertNotEqual(self.run_chain().returncode, 0)

    def test_windows_checkout_preserves_all_abi9_input_hashes(self):
        manifest = json.loads((ROOT / 'tools/runtime/abi9-patches.json').read_text())
        names = ['.gitattributes', 'tools/runtime/abi9-patches.json', manifest['wrapper']['path']]
        names += [p['path'] for p in manifest['patches']]
        original = {}
        for name in names:
            data = (ROOT / name).read_bytes()
            original[name] = data
            target = self.checkout / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        git = ['git', '-C', str(self.checkout), '-c', 'core.autocrlf=true']
        subprocess.run([*git, 'add', '--', *names], check=True, capture_output=True)
        for name in names:
            (self.checkout / name).unlink()
        subprocess.run([*git, 'checkout-index', '--force', '--', *names], check=True)
        for name, expected in original.items():
            actual = (self.checkout / name).read_bytes()
            if name == '.gitattributes':
                self.assertEqual(actual.decode().splitlines(), expected.decode().splitlines(), name)
            else:
                self.assertEqual(actual, expected, name)


if __name__ == '__main__':
    unittest.main()
