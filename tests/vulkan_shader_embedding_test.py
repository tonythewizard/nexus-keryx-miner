"""Verify low-memory ELF embedding against ordinary C++ compilation, without a GPU."""
import ctypes
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SOURCE=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('embedding',SOURCE/'experiments/vulkan_inference/clang_dispatcher.py')
embedding=importlib.util.module_from_spec(spec);spec.loader.exec_module(embedding)


class Embedding(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']);self.root=Path(self.tmp.name)
        (self.root/'ggml-vulkan-shaders.hpp').write_text('#include <cstdint>\nextern const uint64_t a_len,b_len;\nextern const unsigned char a_data[],b_data[];\nextern const void * table_data[2];\nextern const uint64_t table_len[2];\n')
        self.source=self.root/'fixture.comp.cpp'
        self.source.write_text('#include "ggml-vulkan-shaders.hpp"\n\nconst uint64_t a_len = 4;\nconst unsigned char a_data[4] = {\n0x3,0x2,0x23,0x7,\n};\nconst uint64_t b_len = 5;\nconst unsigned char b_data[5] = {\n0xff,0x0,0x80,0x1,0x42,\n};\nconst void * table_data[2] = {a_data,b_data,};\nconst uint64_t table_len[2] = {a_len,b_len,};\n')
    def tearDown(self):self.tmp.cleanup()
    def test_bytes_lengths_tables_and_alignment_match_cpp(self):
        baseline=self.root/'baseline.so';candidate=self.root/'candidate.so';obj=self.root/'candidate.o';dep=self.root/'candidate.d'
        subprocess.run(['c++','-std=c++17','-shared','-fPIC',str(self.source),'-o',str(baseline)],check=True)
        embedding.embed_shader(self.source,['-o',str(obj),'-MF',str(dep)])
        subprocess.run(['c++','-shared',str(obj),'-o',str(candidate)],check=True)
        old,new=ctypes.CDLL(str(baseline)),ctypes.CDLL(str(candidate))
        for lib in (old,new):
            self.assertEqual(ctypes.c_uint64.in_dll(lib,'a_len').value,4)
            self.assertEqual(bytes((ctypes.c_ubyte*4).in_dll(lib,'a_data')),b'\x03\x02\x23\x07')
            self.assertEqual(bytes((ctypes.c_ubyte*5).in_dll(lib,'b_data')),b'\xff\x00\x80\x01\x42')
            lengths=(ctypes.c_uint64*2).in_dll(lib,'table_len');self.assertEqual(list(lengths),[4,5])
            pointers=(ctypes.c_void_p*2).in_dll(lib,'table_data')
            self.assertEqual(ctypes.string_at(pointers[0],4),b'\x03\x02\x23\x07')
            self.assertEqual(ctypes.string_at(pointers[1],5),b'\xff\x00\x80\x01\x42')
        self.assertEqual(ctypes.addressof((ctypes.c_ubyte*4).in_dll(new,'a_data'))%4,0)
        self.assertIn(str(self.source),dep.read_text())
    def test_incomplete_or_overflowing_generated_data_rejected(self):
        for text in ('const uint64_t a_len = 4;\n','const uint64_t a_len = 1;\nconst unsigned char a_data[1] = {\n0x1,0x2,\n};\n'):
            self.source.write_text(text)
            with self.assertRaises(ValueError):embedding.embed_shader(self.source,['-o',str(self.root/'bad.o')])


if __name__=='__main__':unittest.main(verbosity=2)
