//! Verify real CUDA ABI6 captures/replays offline with the official Rust verifier.
use nexus_ai_response_bridge::trace_producer::{self as producer, TierManifest, TraceRecord};
use keryx_verifiable_inference::{merkle::Hash, verify::{self, ResponseCtx, TierCtx}};
use std::{path::Path, fs};
#[path = "support/trace_weights.rs"]
mod trace_weights;
use trace_weights::{need, read, Index};

fn number(data: &[u8], at: &mut usize) -> Result<u32, String> {
    let bytes = data.get(*at..*at+4).ok_or("truncated capture")?;
    *at += 4; Ok(u32::from_le_bytes(bytes.try_into().unwrap()))
}
fn record(root: &Path, m: &TierManifest) -> Result<TraceRecord, String> {
    let data = read(&root.join("capture.bin"), 64 << 20)?;
    need(data.get(..8) == Some(b"NXTRACE1"), "invalid native capture schema")?;
    let mut at = 8;
    let prompt = number(&data, &mut at)?;
    let positions = number(&data, &mut at)? as usize;
    let leaves = number(&data, &mut at)? as usize;
    let text = number(&data, &mut at)? as usize;
    need(positions > 0 && positions <= 512 && leaves == m.n_leaves() && text > 0 && text < 65536, "invalid native dimensions")?;
    need(prompt > 0 && prompt as usize <= positions && data.len() == 24 + text + positions * 8 + positions * leaves * 32, "incomplete native capture")?;
    std::str::from_utf8(&data[at..at+text]).map_err(|_| "invalid generated UTF8")?; at += text;
    for _ in 0..positions { need(number(&data, &mut at)? < i32::MAX as u32, "invalid token")?; }
    let mut ubatch = Vec::new(); for _ in 0..positions { ubatch.push(number(&data, &mut at)?); }
    let hashes = data[at..].chunks_exact(32).map(|v| v.try_into().unwrap()).collect();
    TraceRecord::new(m, prompt, ubatch, hashes).ok_or("official trace commitment rejected incomplete capture".into())
}
fn run() -> Result<(), String> {
    let args: Vec<_> = std::env::args_os().collect();
    need(args.len() == 3 || args.len() == 5, "usage: abi6_trace_oracle draw DIR | verify DIR MODEL TREE")?;
    let root = Path::new(&args[2]); let m = TierManifest::for_tier(0).ok_or("missing manifest")?;
    let rec = record(root, &m)?;
    // Deterministic offline seed, never a fabricated mainnet response/acceptance.
    let seed: Hash = *blake3::hash(b"NEXUS_ABI6_OFFLINE_CAPTURE_REPLAY_V1").as_bytes();
    let points = (0..4).map(|i| producer::draw_point(&m, rec.n_prompt, rec.n_pos, &seed, i)
        .ok_or("official draw failed")).collect::<Result<Vec<_>,_>>()?;
    if args[1] == "draw" && args.len() == 3 {
        let text = points.iter().map(|p| format!("{} {} {}\n", p.t, p.leaf_x(), p.leaf_y())).collect::<String>();
        fs::write(root.join("draws.txt"), text).map_err(|e| e.to_string())?;
        println!("ABI6_RUST_COMMITMENT=PASS positions={} trace_root={}", rec.n_pos, hex::encode(rec.trace_root));
        return Ok(());
    }
    need(args[1] == "verify" && args.len() == 5, "invalid oracle action")?;
    let index = Index::open(Path::new(&args[3]), Path::new(&args[4]))?;
    let root_hash: Hash = hex::decode("2c497164eaf20078add20e82ae4e1b0fdb27d3fdd5eaefc1c48f204111e14e88").unwrap().try_into().unwrap();
    let level = (0..64).find(|l| index.count(*l) == 1).ok_or("invalid model height")?;
    need(index.node(level, 0) == Some(root_hash), "official model root mismatch")?;
    for (e, _) in &m.entries {
        let t = index.tensors.iter().find(|t| t.name == e.name).ok_or("missing manifest tensor")?;
        need((t.ty,t.k,t.m,t.base,t.bytes)==(e.wtype,e.k,e.m*e.n_exp as u64,e.chunk_base,e.n_bytes), "GGUF manifest mismatch")?;
    }
    let tier = TierCtx { r_t:root_hash, n_chunks:index.chunks, m_t:m.m_t, n_entries:m.entries.len() as u64, n_products:m.n_products as u64 };
    let response = ResponseCtx { trace_root:rec.trace_root, n_prompt:rec.n_prompt, n_pos:rec.n_pos };
    for (i, p) in points.iter().enumerate() {
        let x = read(&root.join(format!("{i}.x")), 4 << 20)?;
        let y = read(&root.join(format!("{i}.y")), 4 << 20)?;
        let point = producer::build_point(&m, &rec, p, x, &y, &index).ok_or("replay bytes do not match captured commitment")?;
        verify::verify_point(&tier, &response, &seed, i as u32, &point).map_err(|e| format!("official verifier rejected real CUDA replay: {e:?}"))?;
    }
    println!("ABI6_REAL_CUDA_RUST_POINTS=PASS draws=4 gpu_source=REAL submissions=NONE");
    Ok(())
}
fn main() { if let Err(e) = run() { eprintln!("ABI6_RUST_ORACLE=FAIL {e}"); std::process::exit(1); } }

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Vec<u8> {
        let m = TierManifest::for_tier(0).unwrap();
        let mut data = b"NXTRACE1".to_vec();
        for n in [1u32, 2, m.n_leaves() as u32, 2] { data.extend_from_slice(&n.to_le_bytes()); }
        data.extend_from_slice(b"OK");
        for n in [1u32, 2, 1, 1] { data.extend_from_slice(&n.to_le_bytes()); }
        data.extend(vec![1u8; 2 * m.n_leaves() * 32]);
        data
    }
    #[test]
    fn capture_reader_preserves_shape_and_rejects_truncation_or_trailing_bytes() {
        let dir = tempfile::tempdir().unwrap(); let file = dir.path().join("capture.bin");
        let manifest = TierManifest::for_tier(0).unwrap(); let mut bytes = fixture();
        fs::write(&file, &bytes).unwrap();
        let trace = record(dir.path(), &manifest).unwrap();
        assert_eq!((trace.n_prompt, trace.n_pos), (1,2));
        bytes.push(0); fs::write(&file,&bytes).unwrap(); assert!(record(dir.path(),&manifest).is_err());
        bytes.truncate(24); fs::write(&file,&bytes).unwrap(); assert!(record(dir.path(),&manifest).is_err());
    }
    #[test]
    fn capture_reader_rejects_missing_leaves_and_invalid_tokens() {
        let dir = tempfile::tempdir().unwrap(); let file = dir.path().join("capture.bin");
        let manifest = TierManifest::for_tier(0).unwrap(); let mut bytes = fixture();
        bytes[26..30].copy_from_slice(&u32::MAX.to_le_bytes());
        fs::write(&file,&bytes).unwrap(); assert!(record(dir.path(),&manifest).is_err());
        let mut bytes=fixture(); let len=bytes.len(); bytes[len-32..].fill(0);
        // The last leaf is protocol-derived metadata, so corrupt a real X leaf instead.
        bytes[42..74].fill(0);
        fs::write(&file,&bytes).unwrap(); assert!(record(dir.path(),&manifest).is_err());
    }
}
