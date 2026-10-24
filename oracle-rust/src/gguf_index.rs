use std::{
    fs::File,
    io::{Read, Seek, SeekFrom},
    path::Path,
};

type Result<T> = std::result::Result<T, String>;
struct Reader {
    f: File,
    size: u64,
}
impl Reader {
    fn new(path: &Path) -> Result<Self> {
        let f = File::open(path).map_err(|e| format!("open GGUF: {e}"))?;
        let size = f.metadata().map_err(|e| format!("stat GGUF: {e}"))?.len();
        Ok(Self { f, size })
    }
    fn bytes(&mut self, n: usize) -> Result<Vec<u8>> {
        let mut b = vec![0; n];
        self.f
            .read_exact(&mut b)
            .map_err(|e| format!("truncated GGUF: {e}"))?;
        Ok(b)
    }
    fn u32(&mut self) -> Result<u32> {
        Ok(u32::from_le_bytes(self.bytes(4)?.try_into().unwrap()))
    }
    fn u64(&mut self) -> Result<u64> {
        Ok(u64::from_le_bytes(self.bytes(8)?.try_into().unwrap()))
    }
    fn string(&mut self) -> Result<String> {
        let n = self.u64()?;
        if n > 64 * 1024 * 1024 {
            return Err("unreasonable GGUF string length".into());
        }
        String::from_utf8(self.bytes(n as usize)?)
            .map_err(|e| format!("GGUF string is not UTF-8: {e}"))
    }
    fn skip(&mut self, n: u64) -> Result<()> {
        let p = self.f.stream_position().map_err(|e| e.to_string())?;
        let end = p.checked_add(n).ok_or("GGUF seek overflow")?;
        if end > self.size {
            return Err("GGUF metadata exceeds file".into());
        }
        self.f
            .seek(SeekFrom::Start(end))
            .map_err(|e| e.to_string())?;
        Ok(())
    }
    fn position(&mut self) -> Result<u64> {
        self.f.stream_position().map_err(|e| e.to_string())
    }
}
fn scalar_size(t: u32) -> Option<u64> {
    match t {
        0 | 1 | 7 => Some(1),
        2 | 3 => Some(2),
        4 | 5 | 6 => Some(4),
        10 | 11 | 12 => Some(8),
        _ => None,
    }
}
fn skip_value(r: &mut Reader, t: u32) -> Result<()> {
    match t {
        0 | 1 | 7 => r.skip(1),
        2 | 3 => r.skip(2),
        4 | 5 | 6 => r.skip(4),
        8 => {
            let _ = r.string()?;
            Ok(())
        }
        10 | 11 | 12 => r.skip(8),
        9 => {
            let et = r.u32()?;
            let n = r.u64()?;
            if let Some(sz) = scalar_size(et) {
                r.skip(n.checked_mul(sz).ok_or("GGUF array size overflow")?)
            } else if et == 8 {
                if n > 1_000_000 {
                    return Err("unreasonable GGUF string array".into());
                }
                for _ in 0..n {
                    let _ = r.string()?;
                }
                Ok(())
            } else {
                return Err(format!("unsupported GGUF array element type {et}"));
            }
        }
        _ => Err(format!("unsupported GGUF metadata type {t}")),
    }
}
fn layout(t: u32) -> Result<(u64, u64)> {
    match t {
        0 => Ok((1, 4)),
        1 => Ok((1, 2)),
        2 => Ok((32, 18)),
        3 => Ok((32, 20)),
        6 => Ok((32, 22)),
        7 => Ok((32, 24)),
        8 => Ok((32, 34)),
        9 => Ok((32, 36)),
        10 => Ok((256, 84)),
        11 => Ok((256, 110)),
        12 => Ok((256, 144)),
        13 => Ok((256, 176)),
        14 => Ok((256, 210)),
        15 => Ok((256, 292)),
        20 => Ok((32, 18)),
        23 => Ok((256, 136)),
        24 => Ok((1, 1)),
        25 => Ok((1, 2)),
        26 => Ok((1, 4)),
        27 | 28 => Ok((1, 8)),
        30 => Ok((1, 2)),
        _ => Err(format!("unsupported GGML dtype {t}")),
    }
}
#[derive(Debug, PartialEq, Eq)]
pub struct TensorRange {
    pub name: String,
    pub offset: u64,
    pub bytes: u64,
}
#[derive(Debug)]
pub struct CanonicalIndex {
    pub size: u64,
    pub chunks: u64,
    pub tensors: Vec<TensorRange>,
}
fn independent_parse(path: &Path) -> Result<CanonicalIndex> {
    let mut r = Reader::new(path)?;
    if r.u32()? != 0x46554747 {
        return Err("GGUF magic mismatch".into());
    }
    let version = r.u32()?;
    if !(2..=3).contains(&version) {
        return Err(format!("unsupported GGUF version {version}"));
    }
    let nt = r.u64()?;
    let nkv = r.u64()?;
    if nt > 1_000_000 || nkv > 1_000_000 {
        return Err("unreasonable GGUF header counts".into());
    }
    let mut align = 32u64;
    for _ in 0..nkv {
        let key = r.string()?;
        let ty = r.u32()?;
        if key == "general.alignment" && (ty == 4 || ty == 10) {
            align = if ty == 4 { r.u32()? as u64 } else { r.u64()? };
        } else {
            skip_value(&mut r, ty)?
        }
    }
    if align == 0 || align > 1 << 20 {
        return Err("invalid GGUF alignment".into());
    }
    let mut tensors = Vec::with_capacity(nt as usize);
    for _ in 0..nt {
        let name = r.string()?;
        let dims = r.u32()?;
        if dims == 0 || dims > 8 {
            return Err("invalid GGUF tensor dimensions".into());
        }
        let mut elements = 1u64;
        let mut first = 1u64;
        for i in 0..dims {
            let d = r.u64()?;
            if i == 0 {
                first = d
            }
            elements = elements.checked_mul(d).ok_or("tensor element overflow")?;
        }
        let ty = r.u32()?;
        let relative = r.u64()?;
        let (block, bytes_per_block) = layout(ty)?;
        if first % block != 0 || elements % block != 0 {
            return Err(format!("tensor dimensions not block aligned: {name}"));
        }
        let bytes = (elements / block)
            .checked_mul(bytes_per_block)
            .ok_or("tensor byte length overflow")?;
        tensors.push(TensorRange {
            name,
            offset: relative,
            bytes,
        });
    }
    let pos = r.position()?;
    let data_start = pos
        .checked_add(align - 1)
        .ok_or("GGUF alignment overflow")?
        / align
        * align;
    if data_start > r.size {
        return Err("GGUF data section exceeds file".into());
    }
    tensors.sort_by(|a, b| a.name.as_bytes().cmp(b.name.as_bytes()));
    for pair in tensors.windows(2) {
        if pair[0].name == pair[1].name {
            return Err(format!("duplicate tensor name {}", pair[0].name));
        }
    }
    let mut physical = Vec::with_capacity(tensors.len());
    let mut chunks = 0u64;
    for t in &tensors {
        if t.offset % align != 0 {
            return Err(format!("unaligned tensor offset {}", t.name));
        }
        let start = data_start
            .checked_add(t.offset)
            .ok_or("tensor offset overflow")?;
        let end = start.checked_add(t.bytes).ok_or("tensor end overflow")?;
        if end > r.size {
            return Err(format!("tensor range exceeds file {}", t.name));
        }
        physical.push((start, end));
        chunks = chunks
            .checked_add(t.bytes / 32)
            .ok_or("chunk count overflow")?;
    }
    physical.sort_unstable();
    for w in physical.windows(2) {
        if w[1].0 < w[0].1 {
            return Err("overlapping GGUF tensor ranges".into());
        }
    }
    Ok(CanonicalIndex {
        size: r.size,
        chunks,
        tensors: tensors
            .into_iter()
            .map(|mut t| {
                t.offset += data_start;
                t
            })
            .collect(),
    })
}
fn get_u32(b: &[u8], p: &mut usize) -> Result<u32> {
    let e = p.checked_add(4).ok_or("index offset overflow")?;
    let x = b.get(*p..e).ok_or("truncated C++ index")?;
    *p = e;
    Ok(u32::from_le_bytes(x.try_into().unwrap()))
}
fn get_u64(b: &[u8], p: &mut usize) -> Result<u64> {
    let e = p.checked_add(8).ok_or("index offset overflow")?;
    let x = b.get(*p..e).ok_or("truncated C++ index")?;
    *p = e;
    Ok(u64::from_le_bytes(x.try_into().unwrap()))
}
pub fn verify_index(model: &Path, index: &Path) -> CanonicalIndex {
    let parsed =
        independent_parse(model).unwrap_or_else(|e| panic!("independent GGUF parser: {e}"));
    let data = std::fs::read(index).expect("read C++ canonical index");
    assert!(data.starts_with(b"NXIX"), "bad C++ index magic");
    let mut p = 4;
    let claimed_size = get_u64(&data, &mut p).unwrap();
    let n = get_u32(&data, &mut p).unwrap() as usize;
    assert_eq!(
        claimed_size, parsed.size,
        "C++ index file size differs from independently parsed GGUF"
    );
    assert_eq!(n, parsed.tensors.len(), "C++ index tensor count differs");
    for (i, t) in parsed.tensors.iter().enumerate() {
        let len = get_u32(&data, &mut p).unwrap() as usize;
        let end = p.checked_add(len).expect("tensor name overflow");
        let name = std::str::from_utf8(data.get(p..end).expect("truncated C++ tensor name"))
            .expect("C++ name UTF-8");
        p = end;
        let off = get_u64(&data, &mut p).unwrap();
        let bytes = get_u64(&data, &mut p).unwrap();
        assert_eq!(name, t.name, "canonical name/order differs at tensor {i}");
        assert_eq!(
            off, t.offset,
            "absolute data offset differs at tensor {name}"
        );
        assert_eq!(
            bytes, t.bytes,
            "tensor byte length differs at tensor {name}"
        );
    }
    assert_eq!(p, data.len(), "trailing C++ index bytes");
    parsed
}
pub fn verify(model: &Path, index: &Path) {
    let parsed = verify_index(model, index);
    println!(
        "INDEPENDENT_RUST_GGUF_INDEX=PASS tensors={} chunks={} size={} version-parser=independent",
        parsed.tensors.len(),
        parsed.chunks,
        parsed.size
    );
}

impl CanonicalIndex {
    pub fn read_tiles(&self, model: &Path, offsets: &[u64]) -> Vec<[u8; 1024]> {
        let mut f = crate::platform::open_model(model)
            .expect("open model read-only for Rust tile audit");
        offsets
            .iter()
            .map(|&tile| {
                assert!(
                    tile < self.chunks / 32,
                    "tile offset outside canonical model"
                );
                let first = tile.checked_mul(32).expect("tile chunk index overflow");
                let mut base = 0u64;
                let mut remain = 32u64;
                let mut dst = 0usize;
                let mut out = [0u8; 1024];
                for t in &self.tensors {
                    let count = t.bytes / 32;
                    if first >= base + count {
                        base += count;
                        continue;
                    }
                    let local = first.saturating_sub(base);
                    let take = remain.min(count - local);
                    let fileoff = t
                        .offset
                        .checked_add(local * 32)
                        .expect("tile file offset overflow");
                    f.seek(SeekFrom::Start(fileoff)).expect("seek model tile");
                    let n = (take * 32) as usize;
                    f.read_exact(&mut out[dst..dst + n])
                        .expect("read model tile");
                    dst += n;
                    remain -= take;
                    base += count;
                    if remain == 0 {
                        break;
                    }
                }
                assert_eq!(
                    remain, 0,
                    "canonical tile did not resolve to complete model bytes"
                );
                out
            })
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ggml_wire_layouts_match_supported_reference_ids() {
        assert_eq!(layout(13).unwrap(), (256, 176)); // Q5_K
        assert_eq!(layout(14).unwrap(), (256, 210)); // Q6_K
        assert_eq!(layout(2).unwrap(), (32, 18)); // Q4_0
        assert_eq!(layout(30).unwrap(), (1, 2)); // BF16
        assert!(layout(31).is_err());
    }
    #[test]
    fn metadata_scalar_sizes_are_exact_and_unknown_values_reject() {
        assert_eq!(scalar_size(4), Some(4));
        assert_eq!(scalar_size(12), Some(8));
        assert_eq!(scalar_size(8), None);
        assert_eq!(scalar_size(999), None);
    }
}
