//! Bounded GGUF/sparse-Merkle reader shared by offline GPU trace oracles.
use nexus_ai_response_bridge::trace_producer::WeightIndex;
use keryx_verifiable_inference::merkle::Hash;
use std::{cell::RefCell, collections::HashMap, fs::File, io::{Read, Seek, SeekFrom}, os::unix::fs::FileExt, path::Path};
pub(crate) fn need(value: bool, message: &str) -> Result<(), String> {
    if value { Ok(()) } else { Err(message.into()) }
}
pub(crate) fn read(path: &Path, cap: usize) -> Result<Vec<u8>, String> {
    let f = File::open(path).map_err(|e| e.to_string())?;
    let mut data = Vec::new(); f.take(cap as u64 + 1).read_to_end(&mut data).map_err(|e| e.to_string())?;
    need(data.len() <= cap, "oversized capture file")?; Ok(data)
}
fn number(file: &mut File, bytes: usize) -> Result<u64, String> {
    let mut data = [0u8; 8]; file.read_exact(&mut data[..bytes]).map_err(|e| e.to_string())?;
    Ok(u64::from_le_bytes(data))
}
fn string(file: &mut File) -> Result<String, String> {
    let n = number(file, 8)?; need(n <= 1 << 20, "oversized GGUF string")?;
    let mut bytes = vec![0; n as usize]; file.read_exact(&mut bytes).map_err(|e| e.to_string())?;
    String::from_utf8(bytes).map_err(|e| e.to_string())
}
fn skip(file: &mut File, ty: u64, depth: usize) -> Result<(), String> {
    need(depth <= 2, "nested GGUF metadata exceeds bound")?;
    let bytes = match ty { 0 | 1 | 7 => 1, 2 | 3 => 2, 4..=6 => 4, 10..=12 => 8,
        8 => { let n = number(file, 8)?; need(n <= 1 << 26, "oversized metadata string")?;
            file.seek(SeekFrom::Current(n as i64)).map_err(|e| e.to_string())?; return Ok(()); },
        9 => { let inner = number(file, 4)?; let n = number(file, 8)?;
            need(n <= 1 << 22, "oversized metadata array")?;
            for _ in 0..n { skip(file, inner, depth + 1)?; } return Ok(()); },
        _ => return Err("unknown GGUF metadata type".into()) };
    file.seek(SeekFrom::Current(bytes)).map_err(|e| e.to_string())?; Ok(())
}

pub(crate) struct Tensor { pub name: String, pub ty: u32, pub k: u64, pub m: u64, pub offset: u64, pub bytes: u64, pub base: u64 }
pub(crate) struct Index {
    model: File, tree: File, pub tensors: Vec<Tensor>, pub chunks: u64,
    checkpoints: HashMap<u32, (u64, u64)>, cache: RefCell<HashMap<(u32, u64), Hash>>,
}
impl Index {
    pub(crate) fn open(model: &Path, tree: &Path) -> Result<Self, String> {
        let mut file = File::open(model).map_err(|e| e.to_string())?;
        need(number(&mut file, 4)? == 0x46554747 && number(&mut file, 4)? == 3, "GGUF v3 required")?;
        let nt = number(&mut file, 8)?; let nk = number(&mut file, 8)?;
        need(nt > 0 && nt <= 100000 && nk <= 100000, "invalid GGUF counts")?;
        let mut alignment = 32;
        for _ in 0..nk {
            let key = string(&mut file)?; let ty = number(&mut file, 4)?;
            if key == "general.alignment" { need(ty == 4, "invalid alignment type")?; alignment = number(&mut file, 4)?; }
            else { skip(&mut file, ty, 0)?; }
        }
        need(alignment > 0 && alignment <= 65536 && alignment.is_power_of_two(), "invalid GGUF alignment")?;
        let mut tensors = Vec::new();
        for _ in 0..nt {
            let name = string(&mut file)?; let nd = number(&mut file, 4)?;
            need(nd > 0 && nd <= 4 && name.is_ascii(), "invalid tensor descriptor")?;
            let mut dims = Vec::new(); for _ in 0..nd { dims.push(number(&mut file, 8)?); }
            let ty = number(&mut file, 4)? as u32; let offset = number(&mut file, 8)?;
            let k = dims[0]; let m = dims[1..].iter().try_fold(1u64, |a, b| a.checked_mul(*b)).ok_or("tensor dimension overflow")?;
            let (bb, block) = match ty { 0 => (4, 1), 1 => (2, 1), 2 => (18, 32), 3 => (20, 32),
                6 => (22, 32), 7 => (24, 32), 8 => (34, 32), 12 => (144, 256), 13 => (176, 256), 14 => (210, 256),
                _ => return Err(format!("unsupported GGUF tensor type {ty}")) };
            need(k > 0 && m > 0 && k % block == 0, "invalid tensor shape")?;
            let bytes = (k / block).checked_mul(m).and_then(|v| v.checked_mul(bb)).ok_or("tensor size overflow")?;
            tensors.push(Tensor { name, ty, k, m, offset, bytes, base: 0 });
        }
        let header_end = file.stream_position().map_err(|e| e.to_string())?;
        let data_start = header_end.div_ceil(alignment) * alignment;
        let size = file.metadata().map_err(|e| e.to_string())?.len();
        tensors.sort_by(|a, b| a.name.cmp(&b.name));
        let mut chunks = 0;
        for t in &mut tensors {
            t.offset = data_start.checked_add(t.offset).ok_or("GGUF offset overflow")?;
            need(t.offset.checked_add(t.bytes).is_some_and(|end| end <= size), "tensor outside GGUF")?;
            t.base = chunks; chunks += t.bytes / 32;
        }
        need(chunks > 0, "empty GGUF canonical chunk stream")?;
        let tree = File::open(tree).map_err(|e| e.to_string())?;
        let (mut count, mut level, mut offset) = (chunks, 0, 0);
        let mut checkpoints = HashMap::new();
        loop {
            if level > 0 && level % 6 == 0 || count == 1 { checkpoints.insert(level, (offset, count)); offset += count * 32; }
            if count == 1 { break; } count = count.div_ceil(2); level += 1;
        }
        need(tree.metadata().map_err(|e| e.to_string())?.len() == offset, "invalid sparse tree size")?;
        Ok(Self { model: file, tree, tensors, chunks, checkpoints, cache: RefCell::new(HashMap::new()) })
    }
    pub(crate) fn count(&self, level: u32) -> u64 { self.chunks.div_ceil(1u64 << level) }
    pub(crate) fn node(&self, level: u32, index: u64) -> Option<Hash> {
        if index >= self.count(level) { return None; }
        if let Some(v) = self.cache.borrow().get(&(level, index)) { return Some(*v); }
        let h = if let Some((offset, _)) = self.checkpoints.get(&level) {
            let mut h = [0u8; 32]; self.tree.read_exact_at(&mut h, offset + index * 32).ok()?; h
        } else if level == 0 { *blake3::hash(&self.read_chunk_bytes(index)?).as_bytes() }
        else {
            let left = self.node(level - 1, index * 2)?;
            let right = if index * 2 + 1 < self.count(level - 1) { self.node(level - 1, index * 2 + 1)? } else { left };
            let mut pair = [0u8; 64]; pair[..32].copy_from_slice(&left); pair[32..].copy_from_slice(&right);
            *blake3::hash(&pair).as_bytes()
        };
        let mut cache = self.cache.borrow_mut(); if cache.len() >= 16384 { cache.clear(); } cache.insert((level, index), h);
        Some(h)
    }
}
impl WeightIndex for Index {
    fn root(&self) -> Hash {
        // Index::open rejects empty trees and validates the sparse checkpoint size.
        let level = u64::BITS - (self.chunks - 1).leading_zeros();
        self.node(level, 0).expect("cannot read the weight tree root checkpoint")
    }
    fn n_chunks(&self) -> u64 { self.chunks }
    fn read_chunk_bytes(&self, chunk: u64) -> Option<Hash> {
        let t = self.tensors.iter().find(|t| chunk >= t.base && chunk - t.base < t.bytes / 32)?;
        let mut bytes = [0u8; 32]; self.model.read_exact_at(&mut bytes, t.offset + (chunk - t.base) * 32).ok()?; Some(bytes)
    }
    fn merkle_path(&self, mut chunk: u64) -> Option<Vec<Hash>> {
        if chunk >= self.chunks { return None; } let mut path = Vec::new(); let mut level = 0;
        while self.count(level) > 1 {
            let sibling = if (chunk ^ 1) < self.count(level) { chunk ^ 1 } else { chunk };
            path.push(self.node(level, sibling)?); chunk /= 2; level += 1;
        }
        Some(path)
    }
}
