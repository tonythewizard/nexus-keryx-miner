//! Diagnostic comparison against the pinned official verifier. Not a trace opening.
use keryx_verifiable_inference::{dot, q8, quant, verify};
use std::{fs, path::Path};
use std::collections::HashSet;

// Vulkan stores four ordinary Q8_1 blocks as four ds pairs followed by 128 qs.
// Reorder captured bytes only; never requantize the original F32 activation.
fn mmvq_bytes(raw: &[u8], k: usize) -> Result<Vec<u8>, String> {
    if k == 0 || k > 32768 || k % 32 != 0 || raw.len() != k.div_ceil(128) * 144 {
        return Err("invalid Vulkan Q8 row shape".into());
    }
    let padded = k.div_ceil(512) * 512; // official MATRIX_ROW_PADDING
    let mut out = vec![0u8; padded / 32 * 36];
    for (i, group) in raw.chunks_exact(144).enumerate() {
        for block in 0..4 {
            let offset = (4 * i + block) * 36;
            out[offset..offset + 4].copy_from_slice(&group[4 * block..4 * block + 4]);
            out[offset + 4..offset + 36].copy_from_slice(&group[16 + 32 * block..48 + 32 * block]);
        }
    }
    Ok(out)
}

fn check(root: &Path) -> Result<(), String> {
    let index = fs::read_to_string(root.join("cases.tsv")).map_err(|e| e.to_string())?;
    let mut checked = 0;
    let mut failures = 0;
    let mut unsupported = 0;
    let mut seen = HashSet::new();
    for line in index.lines() {
        let fields: Vec<_> = line.split('\t').collect();
        if fields.len() != 5 { return Err("malformed fixture index".into()); }
        let name = fields[0];
        if name.is_empty() || !name.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_') {
            return Err("invalid case name".into());
        }
        let wtype: u32 = fields[1].parse().map_err(|_| "invalid weight type")?;
        let expected = match name { "q4_0" => 2, "q5_0" => 6, "q8_0" => 8, "q4_K" => 12, "q5_K" => 13, "q6_K" => 14,
            _ => return Err("unknown fixture case".into()) };
        if wtype != expected || !seen.insert(wtype) { return Err("duplicate or mismatched fixture case".into()); }
        let k: usize = fields[2].parse().map_err(|_| "invalid K")?;
        let m: usize = fields[3].parse().map_err(|_| "invalid M")?;
        let rc: i32 = fields[4].parse().map_err(|_| "invalid capture status")?;
        if rc <= 0 { unsupported += 1; println!("OFFICIAL_Q8_DOT type={name} BLOCKED capture_code={rc}"); continue; }
        if m == 0 || m > 1024 { return Err("invalid M bound".into()); }
        let dir = root.join(name);
        let read = |file| fs::read(dir.join(file)).map_err(|e| e.to_string());
        let weights = read("weights.bin")?;
        let output = read("output.f32")?;
        let raw = read("input.q8x4")?;
        if raw.len() != rc as usize { return Err("capture length mismatch".into()); }
        let x = mmvq_bytes(&raw, k)?;
        let row_bytes = quant::row_bytes(wtype, k).ok_or("unsupported weight type")?;
        if weights.len() != row_bytes * m || output.len() != 4 * m { return Err("invalid tensor shape".into()); }
        let mut max_ratio: f64 = 0.0;
        let mut bad = 0;
        for row in 0..m {
            let y = f32::from_le_bytes(output[4 * row..4 * row + 4].try_into().unwrap());
            let r = dot::canonical_dot(wtype, &weights[row * row_bytes..(row + 1) * row_bytes], k, &x, q8::MMVQ)
                .ok_or("official canonical_dot rejected row")?;
            if !y.is_finite() || !r.y_hat.is_finite() || !r.scale.is_finite() { return Err("non-finite row".into()); }
            let error = (y as f64 - r.y_hat).abs();
            let tolerance = verify::EPS * r.scale;
            if error > tolerance { bad += 1; }
            let ratio = if tolerance > 0.0 { error / tolerance } else if error == 0.0 { 0.0 } else { f64::INFINITY };
            max_ratio = max_ratio.max(ratio);
            checked += 1;
        }
        failures += bad;
        println!("OFFICIAL_Q8_DOT type={name} rows={m} mismatches={bad} max_error_over_tolerance={max_ratio:.9}");
    }
    println!("ROWS_CHECKED={checked} MISMATCHES={failures} UNSUPPORTED_CASES={unsupported}");
    if seen.len() != 6 || checked == 0 || failures != 0 || unsupported != 0 { return Err("canonical Q8 compatibility not established".into()); }
    Ok(())
}

fn main() {
    let args: Vec<_> = std::env::args_os().collect();
    if args.len() != 2 { eprintln!("usage: nexus-vulkan-q8-oracle CAPTURE_DIRECTORY"); std::process::exit(2); }
    if let Err(e) = check(Path::new(&args[1])) { eprintln!("Q8_NUMERIC_PARITY=FAIL {e}"); std::process::exit(1); }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn captured_bytes_are_reordered_without_numeric_conversion() {
        let raw: Vec<_> = (0..144).map(|i| i as u8).collect();
        let row = mmvq_bytes(&raw, 128).unwrap();
        assert_eq!(row.len(), 512 / 32 * 36);
        for block in 0..4 {
            assert_eq!(&row[block * 36..block * 36 + 4], &raw[block * 4..block * 4 + 4]);
            assert_eq!(&row[block * 36 + 4..block * 36 + 36], &raw[16 + block * 32..48 + block * 32]);
        }
        assert!(row[144..].iter().all(|v| *v == 0));
    }
    #[test]
    fn malformed_rows_are_rejected() {
        for k in [0, 1, 31, 33, 32769] { assert!(mmvq_bytes(&[0; 144], k).is_err()); }
        assert!(mmvq_bytes(&[0; 143], 128).is_err());
        assert!(mmvq_bytes(&[0; 145], 128).is_err());
    }
}
