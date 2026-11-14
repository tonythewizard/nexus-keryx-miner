//! Canonical PoM chunk layout of a GGUF file, read from its metadata: tensors in byte-wise name
//! order, each contributing floor(bytes / 32) raw 32-byte chunks of its data
//! (`src/model/gguf_reader.cpp`). It is the reference the shared walk is sampled against, and the
//! source a dedicated walk streams its weight copy from.

use std::ffi::{CStr, CString};
use std::fs::File;

pub struct CanonicalLayout {
    file: File,
    /// Absolute file offset and first canonical chunk of every tensor holding at least one chunk.
    segments: Vec<(u64, u64)>,
    n_chunks: u64,
}

impl CanonicalLayout {
    pub fn open(path: &str) -> Result<Self, String> {
        let mut tensors = tensor_extents(path)?;
        tensors.sort_by(|a, b| a.0.cmp(&b.0));
        let mut segments = Vec::with_capacity(tensors.len());
        let mut n_chunks = 0u64;
        for (_, offset, bytes) in tensors {
            let chunks = bytes / 32;
            if chunks > 0 {
                segments.push((offset, n_chunks));
                n_chunks += chunks;
            }
        }
        if n_chunks == 0 {
            return Err(format!("{path}: no canonical chunks"));
        }
        let file = File::open(path).map_err(|e| format!("{path}: {e}"))?;
        Ok(Self { file, segments, n_chunks })
    }

    pub fn n_chunks(&self) -> u64 {
        self.n_chunks
    }

    /// Fill `out` (whole chunks) with canonical chunks `first..`, which may span tensors.
    pub fn read_chunks(&self, first: u64, out: &mut [u8]) -> Result<(), String> {
        if out.len() % 32 != 0 || first + (out.len() / 32) as u64 > self.n_chunks {
            return Err(format!("chunk range {first}+{} outside the canonical layout", out.len() / 32));
        }
        let mut chunk = first;
        let mut filled = 0usize;
        while filled < out.len() {
            let s = self.segments.partition_point(|&(_, start)| start <= chunk) - 1;
            let end = self.segments.get(s + 1).map_or(self.n_chunks, |&(_, start)| start);
            let n = ((end - chunk) as usize * 32).min(out.len() - filled);
            let (offset, start) = self.segments[s];
            read_exact_at(&self.file, &mut out[filled..filled + n], offset + (chunk - start) * 32)?;
            filled += n;
            chunk += (n / 32) as u64;
        }
        Ok(())
    }
}

/// (name, absolute data offset, data bytes) of every tensor in the GGUF header.
fn tensor_extents(path: &str) -> Result<Vec<(Vec<u8>, u64, u64)>, String> {
    let c_path = CString::new(path).map_err(|_| "model path contains NUL".to_string())?;
    // SAFETY: a metadata-only read (no tensor data, no ggml context); the names are copied out
    // before the context is freed.
    unsafe {
        let params = llama_cpp_sys_2::gguf_init_params { no_alloc: true, ctx: std::ptr::null_mut() };
        let g = llama_cpp_sys_2::gguf_init_from_file(c_path.as_ptr(), params);
        if g.is_null() {
            return Err(format!("{path}: not a readable GGUF file"));
        }
        let base = llama_cpp_sys_2::gguf_get_data_offset(g) as u64;
        let n = llama_cpp_sys_2::gguf_get_n_tensors(g);
        let mut out = Vec::with_capacity(n.max(0) as usize);
        for i in 0..n {
            let name = CStr::from_ptr(llama_cpp_sys_2::gguf_get_tensor_name(g, i)).to_bytes().to_vec();
            let offset = base + llama_cpp_sys_2::gguf_get_tensor_offset(g, i) as u64;
            let bytes = llama_cpp_sys_2::gguf_get_tensor_size(g, i) as u64;
            out.push((name, offset, bytes));
        }
        llama_cpp_sys_2::gguf_free(g);
        Ok(out)
    }
}

#[cfg(windows)]
fn read_exact_at(file: &File, mut buf: &mut [u8], mut offset: u64) -> Result<(), String> {
    use std::os::windows::fs::FileExt;
    while !buf.is_empty() {
        match file.seek_read(buf, offset) {
            Ok(0) => return Err(format!("unexpected end of file at {offset}")),
            Ok(n) => {
                buf = &mut buf[n..];
                offset += n as u64;
            }
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e.to_string()),
        }
    }
    Ok(())
}

#[cfg(unix)]
fn read_exact_at(file: &File, buf: &mut [u8], offset: u64) -> Result<(), String> {
    use std::os::unix::fs::FileExt;
    file.read_exact_at(buf, offset).map_err(|e| e.to_string())
}
