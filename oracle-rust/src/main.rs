use keryx_consensus_core::{
    hashing::header::{hash, hash_override_nonce_time},
    header::{CompressedParents, Header},
    pom::{pom_block_seed_h10, pom_block_seed_h14, pom_pow_value_h3, PomProof},
    pom_v4, Hash,
};
use keryx_math::Uint256;
use std::{env, fs, path::Path};
mod cuda_batch;
mod gguf_index;
mod real_root;
mod real_verify;
mod verify_stream;
fn mix64(mut x: u64) -> u64 {
    x ^= x >> 30;
    x = x.wrapping_mul(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x = x.wrapping_mul(0x94d049bb133111eb);
    x ^= x >> 31;
    x
}
fn blob64() -> Vec<u8> {
    let mut v = vec![0u8; 64 * 1024 + 3 * 32];
    let mut h = 0xDEADBEEFu64;
    for b in &mut v {
        h = mix64(h);
        *b = h as u8;
    }
    v
}
fn root(leaves: &[[u8; 32]]) -> [u8; 32] {
    let mut a = leaves.to_vec();
    while a.len() > 1 {
        let mut b = Vec::with_capacity((a.len() + 1) / 2);
        for p in a.chunks(2) {
            let right = if p.len() == 2 { p[1] } else { p[0] };
            let mut x = [0u8; 64];
            x[..32].copy_from_slice(&p[0]);
            x[32..].copy_from_slice(&right);
            b.push(*blake3::hash(&x).as_bytes());
        }
        a = b;
    }
    a[0]
}
fn u32le(o: &mut Vec<u8>, x: u32) {
    o.extend_from_slice(&x.to_le_bytes())
}
fn u64le(o: &mut Vec<u8>, x: u64) {
    o.extend_from_slice(&x.to_le_bytes())
}
fn header_fixture(path: &Path) {
    let mut parents = CompressedParents::default();
    parents.push(vec![
        Hash::from_bytes([0x11; 32]),
        Hash::from_bytes([0x22; 32]),
    ]);
    parents.push(vec![Hash::from_bytes([0x33; 32])]);
    parents.push(vec![Hash::from_bytes([0x33; 32])]);
    let mut header = Header::new_finalized(
        1,
        parents,
        Hash::from_bytes([0x44; 32]),
        Hash::from_bytes([0x55; 32]),
        Hash::from_bytes([0x66; 32]),
        1_700_000_123,
        0x1d00ffff,
        0x0102030405060708,
        121_211_211,
        0x0102030405060708u64.into(),
        0x1122334455667788,
        Hash::from_bytes([0x77; 32]),
        0x8877665544332211,
        Hash::from_bytes([0x88; 32]),
        3,
    );
    let override_hash =
        hash_override_nonce_time(&header, 0x0f1e2d3c4b5a6978, 1_700_000_321).as_bytes();
    let pre = hash_override_nonce_time(&header, 0, 0).as_bytes();
    let mut out = Vec::new();
    out.extend_from_slice(b"NXHD");
    out.extend_from_slice(&override_hash);
    out.extend_from_slice(&pre);
    u64le(
        &mut out,
        pom_block_seed_h10(&pre, 1_700_000_321, 0x0f1e2d3c4b5a6978),
    );
    u64le(
        &mut out,
        pom_block_seed_h14(&pre, 1_700_000_321, 0x0f1e2d3c4b5a6978),
    );
    u64le(
        &mut out,
        keryx_consensus_core::pom::pom_block_seed_v4(&pre, 1_700_000_321, 0x0f1e2d3c4b5a6978),
    );
    keryx_consensus_core::pom::init_pom_level_activation(43_450_000);
    keryx_consensus_core::pom::init_service_commit_activation(76_316_623);
    header.finalize();
    out.extend_from_slice(&hash(&header).as_bytes());
    let bits = [
        0x1d00ffff, 0x03009234, 0x02008000, 0x1d80ffff, 0x20000001, 0x21000001, 0x22000001,
        0x23000001, 0xff000001,
    ];
    u32le(&mut out, bits.len() as u32);
    for b in bits {
        u32le(&mut out, b);
        out.extend_from_slice(&Uint256::from_compact_target_bits(b).to_le_bytes());
    }
    for tiers in [
        keryx_consensus_core::config::params::POM_TIERS_H6,
        keryx_consensus_core::config::params::POM_TIERS_H14,
    ] {
        out.push(tiers.len() as u8);
        for tier in tiers {
            out.extend_from_slice(&tier.root);
            u64le(&mut out, tier.chunks);
        }
    }
    fs::write(path, out).expect("write header oracle fixture");
    println!("OFFICIAL_HEADER_ORACLE=PASS pre_pow={:02x?}", pre);
}

fn take<'a>(b: &'a [u8], p: &mut usize, n: usize) -> &'a [u8] {
    let end = p.checked_add(n).expect("offset overflow");
    assert!(end <= b.len(), "truncated live header vector");
    let x = &b[*p..end];
    *p = end;
    x
}
fn get_u16(b: &[u8], p: &mut usize) -> u16 {
    u16::from_le_bytes(take(b, p, 2).try_into().unwrap())
}
fn get_u32(b: &[u8], p: &mut usize) -> u32 {
    u32::from_le_bytes(take(b, p, 4).try_into().unwrap())
}
fn get_u64(b: &[u8], p: &mut usize) -> u64 {
    u64::from_le_bytes(take(b, p, 8).try_into().unwrap())
}
fn live_header_fixture(path: &Path) {
    let data = fs::read(path).expect("read live-header vector");
    assert!(data.starts_with(b"NXRH"), "bad live-header magic");
    let mut p = 4;
    let count = get_u32(&data, &mut p);
    keryx_consensus_core::pom::init_pom_level_activation(43_450_000);
    keryx_consensus_core::pom::init_service_commit_activation(76_316_623);
    keryx_consensus_core::pom::init_pom_seed_h10_activation(87_360_000);
    keryx_consensus_core::pom::init_private_inference_activation(121_985_000);
    for index in 0..count {
        let size = get_u32(&data, &mut p) as usize;
        let row = take(&data, &mut p, size);
        let mut q = 0;
        let version = get_u16(row, &mut q);
        let levels = get_u32(row, &mut q);
        let mut parents = CompressedParents::default();
        for _ in 0..levels {
            let n = get_u32(row, &mut q);
            let mut v = Vec::new();
            for _ in 0..n {
                v.push(Hash::from_bytes(take(row, &mut q, 32).try_into().unwrap()));
            }
            parents.push(v);
        }
        let merkle = Hash::from_bytes(take(row, &mut q, 32).try_into().unwrap());
        let accepted = Hash::from_bytes(take(row, &mut q, 32).try_into().unwrap());
        let utxo = Hash::from_bytes(take(row, &mut q, 32).try_into().unwrap());
        let timestamp = get_u64(row, &mut q);
        let bits = get_u32(row, &mut q);
        let nonce = get_u64(row, &mut q);
        let daa = get_u64(row, &mut q);
        let blue_len = get_u32(row, &mut q) as usize;
        let blue = keryx_math::Uint192::from_be_bytes_var(take(row, &mut q, blue_len))
            .expect("blue work overflow");
        let blue_score = get_u64(row, &mut q);
        let pruning = Hash::from_bytes(take(row, &mut q, 32).try_into().unwrap());
        let pom_state = get_u64(row, &mut q);
        let service = Hash::from_bytes(take(row, &mut q, 32).try_into().unwrap());
        let tier = take(row, &mut q, 1)[0];
        let claimed_hash: [u8; 32] = take(row, &mut q, 32).try_into().unwrap();
        let claimed_seed = get_u64(row, &mut q);
        let claimed_target: [u8; 32] = take(row, &mut q, 32).try_into().unwrap();
        assert_eq!(q, row.len(), "trailing live-header row bytes");
        let header = Header::new_finalized(
            version, parents, merkle, accepted, utxo, timestamp, bits, nonce, daa, blue,
            blue_score, pruning, pom_state, service, tier,
        );
        assert_eq!(
            header.hash.as_bytes(),
            claimed_hash,
            "official node header hash mismatch on live record {index}"
        );
        assert_eq!(
            Uint256::from_compact_target_bits(bits).to_le_bytes(),
            claimed_target,
            "official compact target mismatch on live record {index}"
        );
        let pph = keryx_consensus_core::hashing::header::hash_override_nonce_time(&header, 0, 0)
            .as_bytes();
        assert_eq!(
            keryx_consensus_core::pom::pom_block_seed_rewalk_era(&pph, timestamp, nonce, daa),
            claimed_seed,
            "official seed mismatch on live record {index}"
        );
        println!(
            "OFFICIAL_RUST_LIVE_HEADER=PASS index={} daa={} tier={} bits={:#010x}",
            index, daa, tier, bits
        );
    }
    assert_eq!(p, data.len(), "trailing bytes in live vector");
    println!(
        "OFFICIAL_RUST_LIVE_HEADER_SUMMARY records={} submit=DISABLED",
        count
    );
}

fn main() {
    if std::env::args().nth(1).as_deref() == Some("verify-real-stream") {
        assert_eq!(std::env::args().count(), 2, "unexpected verifier stream argument");
        verify_stream::run();
        return;
    }
    let args: Vec<String> = env::args().collect();
    if args.get(1).map(String::as_str) == Some("verify-index") {
        if args.len() != 4 {
            eprintln!("usage: oracle verify-index MODEL.gguf CANONICAL_INDEX");
            std::process::exit(2)
        }
        gguf_index::verify(Path::new(&args[2]), Path::new(&args[3]));
        return;
    }
    if args.get(1).map(String::as_str) == Some("verify-cuda-batch") {
        if args.len() != 5 {
            eprintln!("usage: oracle verify-cuda-batch MODEL.gguf CANONICAL_INDEX BATCH.bin");
            std::process::exit(2)
        }
        cuda_batch::verify(
            Path::new(&args[2]),
            Path::new(&args[3]),
            Path::new(&args[4]),
        );
        return;
    }
    if args.get(1).map(String::as_str) == Some("verify-real") {
        if args.len() != 5 {
            eprintln!("usage: oracle verify-real CONTEXT TRACE PROOF");
            std::process::exit(2)
        }
        real_verify::run(
            Path::new(&args[2]),
            Path::new(&args[3]),
            Path::new(&args[4]),
        );
        return;
    }
    if args.get(1).map(String::as_str) == Some("verify-root") {
        if args.len() != 4 {
            eprintln!("usage: oracle verify-root MODEL.gguf CANONICAL_INDEX");
            std::process::exit(2)
        }
        real_root::verify(Path::new(&args[2]), Path::new(&args[3]));
        return;
    }
    if args.len() != 3 {
        eprintln!("usage: oracle generate|verify|reject-tile|reject-target|reject-tier|reject-borsh|header|header-live PATH | verify-real CONTEXT TRACE PROOF | verify-root MODEL.gguf CANONICAL_INDEX");
        std::process::exit(2)
    }
    let mode = &args[1];
    let path = Path::new(&args[2]);
    if mode == "header" {
        header_fixture(path);
        return;
    }
    if mode == "header-live" {
        live_header_fixture(path);
        return;
    }
    let seed = 0x0fed_cba9_8765_4321u64;
    let tier = 2u8;
    let blob = blob64();
    let leaves: Vec<[u8; 32]> = blob
        .chunks(32)
        .map(|c| *blake3::hash(c).as_bytes())
        .collect();
    let rt = root(&leaves);
    let walk = pom_v4::v4_walk(seed, &blob).expect("official v4_walk");
    let v4 = pom_v4::v4_prove(seed, tier, &blob, &leaves).expect("official v4_prove");
    let fs_state = pom_v4::verify_pom_proof_v4(seed, &v4, &rt, leaves.len() as u64)
        .expect("official v4 verify");
    assert_eq!(walk.states.len(), 257);
    assert_eq!(walk.offsets.len(), 256);
    let pph = [0x42u8; 32];
    let pow = pom_pow_value_h3(fs_state, &pph);
    let proof = PomProof {
        tier,
        trace_root: [0; 32],
        pow_value: pow,
        final_state: fs_state,
        initial_trace_path: vec![],
        final_trace_path: vec![],
        openings: vec![],
        steps_v2: None,
        v3: None,
        v4: Some(v4),
    };
    let target = [0xffu8; 32];
    pom_v4::verify_pom_proof_v4_container(seed, &proof, leaves.len() as u64, &rt, &target, |s| {
        pom_pow_value_h3(s, &pph)
    })
    .expect("official container verify");
    let wire = proof.to_wire_bytes();
    if mode == "generate" {
        let mut out = Vec::new();
        out.extend_from_slice(b"NXV4");
        u64le(&mut out, seed);
        u64le(&mut out, leaves.len() as u64);
        out.push(tier);
        out.extend_from_slice(&rt);
        u64le(&mut out, fs_state);
        for x in &walk.offsets {
            u64le(&mut out, *x)
        }
        for s in &walk.states {
            out.extend_from_slice(s)
        }
        u32le(&mut out, wire.len() as u32);
        out.extend_from_slice(&wire);
        fs::write(path, out).expect("write oracle vector");
        println!(
            "OFFICIAL_ORACLE=PASS states=257 offsets=256 proof_bytes={} root={:02x?}",
            wire.len(),
            rt
        );
    } else if mode == "verify" || mode.starts_with("reject-") {
        let candidate = fs::read(path).expect("read proof candidate");
        if mode == "reject-borsh" {
            assert!(
                PomProof::from_wire_bytes(&candidate).is_err(),
                "truncated/corrupt Borsh unexpectedly decoded"
            );
            println!("OFFICIAL_NEGATIVE_BORSH=PASS");
            return;
        }
        let mut parsed = PomProof::from_wire_bytes(&candidate).expect("official Borsh decode");
        if mode == "reject-tier" {
            parsed.tier ^= 1;
        }
        let actual_target = if mode == "reject-target" {
            [0u8; 32]
        } else {
            target
        };
        let result = pom_v4::verify_pom_proof_v4_container(
            seed,
            &parsed,
            leaves.len() as u64,
            &rt,
            &actual_target,
            |s| pom_pow_value_h3(s, &pph),
        );
        if mode == "verify" {
            result.expect("official verify candidate");
            assert_eq!(
                candidate, wire,
                "candidate differs from official canonical Borsh"
            );
            println!(
                "OFFICIAL_RUST_VERIFY=PASS BORSCH=BYTE_IDENTICAL bytes={}",
                candidate.len()
            );
        } else {
            assert!(result.is_err(), "negative proof unexpectedly accepted");
            println!(
                "OFFICIAL_NEGATIVE_{}=PASS error={:?}",
                mode.to_uppercase(),
                result.unwrap_err()
            );
        }
    } else {
        panic!("unknown mode")
    }
}
