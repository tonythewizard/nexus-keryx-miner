use keryx_consensus_core::{
    pom::{pom_block_seed_rewalk_era, pom_pow_value_h3, PomProof},
    pom_v4,
};
use std::{cmp::Ordering, fs, path::Path};
fn take<'a>(b: &'a [u8], p: &mut usize, n: usize) -> &'a [u8] {
    let e = p.checked_add(n).expect("offset overflow");
    assert!(e <= b.len(), "truncated real model artifact");
    let x = &b[*p..e];
    *p = e;
    x
}
fn u32le(b: &[u8], p: &mut usize) -> u32 {
    u32::from_le_bytes(take(b, p, 4).try_into().unwrap())
}
fn u64le(b: &[u8], p: &mut usize) -> u64 {
    u64::from_le_bytes(take(b, p, 8).try_into().unwrap())
}
pub fn run(ctx_path: &Path, trace_path: &Path, proof_path: &Path) {
    let c = fs::read(ctx_path).expect("read real proof context");
    let tr = fs::read(trace_path).expect("read C++ trace");
    let proof = fs::read(proof_path).expect("read proof");
    println!("{}", verify_bytes(&c, &tr, &proof));
}

pub fn verify_bytes(c: &[u8], tr: &[u8], proof_bytes: &[u8]) -> String {
    assert!(c.starts_with(b"NXRF"), "bad proof context magic");
    let mut p = 4;
    let seed = u64le(&c, &mut p);
    let chunks = u64le(&c, &mut p);
    let tier = take(&c, &mut p, 1)[0];
    let root: [u8; 32] = take(&c, &mut p, 32).try_into().unwrap();
    let target: [u8; 32] = take(&c, &mut p, 32).try_into().unwrap();
    let pph: [u8; 32] = take(&c, &mut p, 32).try_into().unwrap();
    let timestamp = u64le(&c, &mut p);
    let nonce = u64le(&c, &mut p);
    let daa = u64le(&c, &mut p);
    let generation = u64le(&c, &mut p);
    let _capture_digest: [u8; 32] = take(&c, &mut p, 32).try_into().unwrap();
    let claimed_final = u64le(&c, &mut p);
    let claimed_pow: [u8; 32] = take(&c, &mut p, 32).try_into().unwrap();
    let wire_len = u32le(&c, &mut p) as usize;
    let wire = take(&c, &mut p, wire_len);
    assert_eq!(p, c.len(), "trailing proof context bytes");
    assert_eq!(proof_bytes, wire, "proof/context mismatch");
    keryx_consensus_core::pom::init_pom_level_activation(43_450_000);
    keryx_consensus_core::pom::init_service_commit_activation(76_316_623);
    keryx_consensus_core::pom::init_pom_seed_h10_activation(87_360_000);
    keryx_consensus_core::pom::init_private_inference_activation(121_985_000);
    assert_eq!(
        pom_block_seed_rewalk_era(&pph, timestamp, nonce, daa),
        seed,
        "official header/seed/nonce binding mismatch"
    );
    assert_ne!(
        pom_block_seed_rewalk_era(&pph, timestamp, nonce.wrapping_add(1), daa),
        seed,
        "nonce mutation failed to change seed"
    );
    assert!(generation > 0);
    let proof = PomProof::from_wire_bytes(wire).expect("official Borsh decode");
    assert_eq!(proof.tier, tier);
    let v4 = proof.v4.as_ref().expect("missing v4 witness");
    assert_eq!(v4.tier, tier);
    assert_eq!(v4.tiles.len(), pom_v4::POM_V4_K);
    assert_eq!(v4.merkle.len(), pom_v4::POM_V4_K);
    assert!(tr.starts_with(b"NXST"), "bad C++ trace magic");
    let mut q = 4;
    assert_eq!(u64le(&tr, &mut q), seed);
    assert_eq!(u64le(&tr, &mut q), nonce);
    let nstates = u32le(&tr, &mut q) as usize;
    let noffsets = u32le(&tr, &mut q) as usize;
    assert_eq!(nstates, 257);
    assert_eq!(noffsets, 256);
    let states = take(&tr, &mut q, nstates * 1024);
    let offsets = take(&tr, &mut q, noffsets * 8);
    assert_eq!(q, tr.len(), "trailing C++ trace bytes");
    if daa >= keryx_consensus_core::config::params::H15_ACTIVATION_DAA {
        let derived = crate::v5::verify_native(
            seed, chunks, tier, &root, &target, &pph, &proof, states, offsets,
        );
        assert_eq!(derived, claimed_final);
        assert_eq!(proof.pow_value, claimed_pow);
        assert_eq!(proof.to_wire_bytes(), wire, "noncanonical v5 wire");
        assert!(PomProof::from_wire_bytes(&wire[..wire.len() - 1]).is_err());
        assert_ne!(
            pom_block_seed_rewalk_era(&pph, timestamp.wrapping_add(1), nonce, daa),
            seed
        );
        let target_met = claimed_pow.iter().rev().cmp(target.iter().rev()) != Ordering::Greater;
        return format!("OFFICIAL_RUST_REAL_MODEL=PASS era=H15 states=257 offsets=256 Borsh=BYTE_IDENTICAL proof_bytes={} generation={} seed_binding=PASS negative_tile_sibling_pad_root_tier_nonce_timestamp_borsh=PASS target_satisfied={} container={}",wire.len(),generation,target_met,if target_met {"PASS"} else {"TARGET_NOT_MET"});
    }
    let official_offsets = pom_v4::v4_offset_chain(seed, &v4.tiles, chunks / 32);
    for (i, &off) in official_offsets.iter().enumerate() {
        let cpp = u64::from_le_bytes(offsets[i * 8..i * 8 + 8].try_into().unwrap());
        assert_eq!(
            off, cpp,
            "offset mismatch nonce={nonce} iteration={i} rust={off} cpp={cpp}"
        );
    }
    let mut state = pom_v4::v4_initial_state(seed);
    assert_eq!(&states[..1024], state.as_slice(), "initial state mismatch");
    for step in 1..=pom_v4::POM_V4_K {
        state = pom_v4::v4_transition(&state, &v4.tiles[step - 1], step as u32);
        let expected = &states[step * 1024..(step + 1) * 1024];
        if state.as_slice() != expected {
            let b = state
                .iter()
                .zip(expected)
                .position(|(x, y)| x != y)
                .unwrap();
            panic!(
                "state mismatch nonce={nonce} iteration={step} byte={b} rust={} cpp={}",
                state[b], expected[b]
            );
        }
    }
    let derived = pom_v4::verify_pom_proof_v4(seed, v4, &root, chunks)
        .expect("official Rust PoM verifier rejected real GGUF proof");
    assert_eq!(derived, claimed_final);
    assert_eq!(proof.final_state, derived);
    assert_eq!(pom_pow_value_h3(derived, &pph), claimed_pow);
    assert_eq!(proof.pow_value, claimed_pow);
    assert_eq!(proof.to_wire_bytes(), wire, "Borsh bytes are not canonical");
    let target_met = claimed_pow.iter().rev().cmp(target.iter().rev()) != Ordering::Greater;
    let container =
        pom_v4::verify_pom_proof_v4_container(seed, &proof, chunks, &root, &target, |s| {
            pom_pow_value_h3(s, &pph)
        });
    if target_met {
        container.expect("official container rejected target-valid candidate");
    } else {
        assert_eq!(container, Err(pom_v4::PomV4VerifyError::TargetNotMet));
    }
    let mut bad = proof.clone();
    bad.v4.as_mut().unwrap().tiles[0][0] ^= 1;
    assert!(
        pom_v4::verify_pom_proof_v4(seed, bad.v4.as_ref().unwrap(), &root, chunks).is_err(),
        "mutated tile accepted"
    );
    let mut bad = proof.clone();
    bad.v4.as_mut().unwrap().merkle[0].path[0][0] ^= 1;
    assert!(
        pom_v4::verify_pom_proof_v4(seed, bad.v4.as_ref().unwrap(), &root, chunks).is_err(),
        "mutated sibling accepted"
    );
    let mut wrong_root = root;
    wrong_root[0] ^= 1;
    assert!(
        pom_v4::verify_pom_proof_v4(seed, v4, &wrong_root, chunks).is_err(),
        "wrong root accepted"
    );
    let mut wrong_tier = proof.clone();
    wrong_tier.tier ^= 1;
    assert_eq!(
        pom_v4::verify_pom_proof_v4_container(seed, &wrong_tier, chunks, &root, &[0xff; 32], |s| {
            pom_pow_value_h3(s, &pph)
        }),
        Err(pom_v4::PomV4VerifyError::TierMismatch)
    );
    assert!(
        PomProof::from_wire_bytes(&wire[..wire.len() - 1]).is_err(),
        "truncated Borsh accepted"
    );
    assert_ne!(
        pom_block_seed_rewalk_era(&pph, timestamp.wrapping_add(1), nonce, daa),
        seed,
        "timestamp mutation did not change seed"
    );
    format!("OFFICIAL_RUST_REAL_MODEL=PASS states=257 offsets=256 tile_paths=256 Borsh=BYTE_IDENTICAL proof_bytes={} generation={} seed_binding=PASS negative_tile_sibling_root_tier_nonce_timestamp_borsh=PASS target_satisfied={} container={}",wire.len(),generation,target_met,if target_met{"PASS"}else{"TARGET_NOT_MET"})
}
