//! Bounded offline v5 attestation against the pinned, unmodified node crate.
//! Fixture mode permits a small synthetic pad; real mode enforces official tier anchors.
use crate::{get_u32, get_u64, take};
use keryx_consensus_core::{
    config::params,
    pom::{pom_block_seed_h14, pom_pow_value_h3, PomProof},
    pom_v3, pom_v4, pom_v5,
};
use std::{fs, path::Path};

pub fn verify(path: &Path, real: bool) {
    assert!(
        fs::metadata(path).expect("v5 artifact metadata").len() <= 2 * 1024 * 1024,
        "oversized v5 artifact"
    );
    let raw = fs::read(path).expect("read v5 artifact");
    let mut p = 0;
    assert_eq!(take(&raw, &mut p, 4), b"NXV5");
    let tier = take(&raw, &mut p, 1)[0];
    assert!(tier < 5);
    let model: [u8; 32] = take(&raw, &mut p, 32).try_into().unwrap();
    let root: [u8; 32] = take(&raw, &mut p, 32).try_into().unwrap();
    let chunks = get_u64(&raw, &mut p);
    let pads = get_u64(&raw, &mut p);
    assert!(
        chunks >= 32 && chunks < 1 << 48 && pads > 0 && pads < 1 << 40,
        "invalid v5 geometry"
    );
    let seed = get_u64(&raw, &mut p);
    let pph: [u8; 32] = take(&raw, &mut p, 32).try_into().unwrap();
    let target: [u8; 32] = take(&raw, &mut p, 32).try_into().unwrap();
    let timestamp = get_u64(&raw, &mut p);
    let nonce = get_u64(&raw, &mut p);
    let daa = get_u64(&raw, &mut p);
    assert!(
        daa >= params::H15_ACTIVATION_DAA,
        "v5 fixture is outside mainnet v5 era"
    );
    assert_eq!(
        seed,
        pom_block_seed_h14(&pph, timestamp, nonce),
        "v5 header seed/nonce binding"
    );
    if real {
        let anchor = params::POM_TIERS_H14[tier as usize];
        assert_eq!(model, anchor.model_id, "v5 official model ID");
        assert_eq!(root, anchor.root, "v5 official model root");
        assert_eq!(chunks, anchor.chunks, "v5 official model chunk count");
        assert_eq!(
            pads,
            params::POM_V5_PAD_TILES_H14[tier as usize],
            "v5 official pad geometry"
        );
    }
    let pad_seed = pom_v5::v5_pad_seed(tier, &model, &root);
    assert_eq!(get_u32(&raw, &mut p), 257);
    assert_eq!(get_u32(&raw, &mut p), 256);
    let states = take(&raw, &mut p, 257 * 1024);
    let offsets = take(&raw, &mut p, 256 * 8);
    let wire_len = get_u32(&raw, &mut p) as usize;
    assert!(wire_len <= 1024 * 1024);
    let wire = take(&raw, &mut p, wire_len);
    assert_eq!(p, raw.len(), "trailing v5 artifact bytes");
    let proof = PomProof::from_wire_bytes(wire).expect("v5 canonical proof decode");
    assert_eq!(proof.to_wire_bytes(), wire, "noncanonical v5 wire");
    assert_eq!(proof.tier, tier);
    assert!(proof.task.is_none());
    let witness = proof.v4.as_ref().expect("v5 needs v4-shaped witness");
    assert_eq!(witness.tier, tier);
    assert_eq!(witness.tiles.len(), 256);
    assert_eq!(witness.merkle.len(), 256);
    let pad_positions = verify_walk(seed, chunks, pads, &pad_seed, witness, states, offsets);
    let derived = pom_v5::verify_pom_proof_v5(seed, witness, &root, chunks, &pad_seed, pads)
        .expect("official v5 proof rejected");
    assert_eq!(proof.final_state, derived);
    assert_eq!(proof.pow_value, pom_pow_value_h3(derived, &pph));
    pom_v5::verify_pom_proof_v5_container(
        seed,
        &proof,
        chunks,
        &root,
        &pad_seed,
        pads,
        &target,
        |s| pom_pow_value_h3(s, &pph),
    )
    .expect("official v5 container rejected");
    println!("OFFICIAL_V5_VERIFY=PASS real_anchors={real} states=257 offsets=256 pad_positions={pad_positions} canonical_borsh=true target=true submission=false");
}

fn verify_walk(
    seed: u64,
    chunks: u64,
    pads: u64,
    pad_seed: &[u8; 32],
    witness: &keryx_consensus_core::pom_v4::PomProofV4,
    states: &[u8],
    offsets: &[u8],
) -> u64 {
    let weights = chunks / 32;
    let pad_steps = ((256 * pads + (weights + pads) / 2) / (weights + pads)).clamp(1, 255);
    let mut state = pom_v4::v4_initial_state(seed);
    assert_eq!(state.as_slice(), &states[..1024]);
    let mut off = pom_v4::v4_first_offset(seed, weights);
    let mut pad_positions = 0;
    for step in 1..=256 {
        let actual = u64::from_le_bytes(offsets[(step - 1) * 8..step * 8].try_into().unwrap());
        assert_eq!(actual, off, "v5 offset at step {step}");
        let tile = if off < weights {
            assert_eq!(witness.tiles[step - 1].len(), 1024);
            witness.tiles[step - 1].clone()
        } else {
            pad_positions += 1;
            assert!(witness.tiles[step - 1].is_empty() && witness.merkle[step - 1].path.is_empty());
            pom_v5::v5_pad_tile(pad_seed, off - weights)
        };
        if step < 256 {
            let hs = (0..32).fold(0, |h, r| {
                h ^ crate::mix64(
                    u64::from_le_bytes(state[r * 32..r * 32 + 8].try_into().unwrap()) ^ r as u64,
                )
            });
            let snippet: [u8; 32] = tile[..32].try_into().unwrap();
            let raw = crate::mix64(
                seed ^ ((step + 1) as u64).wrapping_mul(pom_v4::POM_V4_OFFSET_STEP_SALT)
                    ^ pom_v3::snippet_fold(&snippet)
                    ^ hs,
            );
            let is_pad = ((step as u64 + 1) * pad_steps) / 256 != (step as u64 * pad_steps) / 256;
            off = if is_pad {
                weights + raw % pads
            } else {
                raw % weights
            };
        }
        state = pom_v4::v4_transition(&state, &tile, step as u32);
        assert_eq!(
            state.as_slice(),
            &states[step * 1024..(step + 1) * 1024],
            "v5 state at step {step}"
        );
    }
    assert_eq!(pad_positions, pad_steps);
    pad_positions
}

pub fn verify_native(
    seed: u64,
    chunks: u64,
    tier: u8,
    root: &[u8; 32],
    target: &[u8; 32],
    pph: &[u8; 32],
    proof: &PomProof,
    states: &[u8],
    offsets: &[u8],
) -> u64 {
    assert!(tier < 5, "invalid v5 tier");
    let anchor = params::POM_TIERS_H14[tier as usize];
    assert_eq!(*root, anchor.root, "v5 official model root");
    assert_eq!(chunks, anchor.chunks, "v5 official chunk count");
    assert!(proof.task.is_none(), "unexpected v5 task");
    assert_eq!(proof.tier, tier);
    let witness = proof.v4.as_ref().expect("missing v5 witness");
    assert_eq!(witness.tier, tier);
    assert_eq!(witness.tiles.len(), 256);
    assert_eq!(witness.merkle.len(), 256);
    let pads = params::POM_V5_PAD_TILES_H14[tier as usize];
    let pad_seed = pom_v5::v5_pad_seed(tier, &anchor.model_id, root);
    verify_walk(seed, chunks, pads, &pad_seed, witness, states, offsets);
    let derived = pom_v5::verify_pom_proof_v5(seed, witness, root, chunks, &pad_seed, pads)
        .expect("official v5 witness rejected");
    assert_eq!(proof.final_state, derived);
    assert_eq!(proof.pow_value, pom_pow_value_h3(derived, pph));
    let container = pom_v5::verify_pom_proof_v5_container(
        seed,
        proof,
        chunks,
        root,
        &pad_seed,
        pads,
        target,
        |s| pom_pow_value_h3(s, pph),
    );
    if proof.pow_value.iter().rev().cmp(target.iter().rev()) != std::cmp::Ordering::Greater {
        container.expect("official v5 container rejected target-valid candidate");
    } else {
        assert_eq!(container, Err(pom_v4::PomV4VerifyError::TargetNotMet));
    }
    let mut bad = proof.clone();
    let weight = witness
        .tiles
        .iter()
        .position(|tile| !tile.is_empty())
        .expect("missing v5 weight sample");
    bad.v4.as_mut().unwrap().tiles[weight][0] ^= 1;
    assert!(
        pom_v5::verify_pom_proof_v5(
            seed,
            bad.v4.as_ref().unwrap(),
            root,
            chunks,
            &pad_seed,
            pads
        )
        .is_err(),
        "mutated v5 weight accepted"
    );
    let mut bad = proof.clone();
    bad.v4.as_mut().unwrap().merkle[weight].path[0][0] ^= 1;
    assert!(
        pom_v5::verify_pom_proof_v5(
            seed,
            bad.v4.as_ref().unwrap(),
            root,
            chunks,
            &pad_seed,
            pads
        )
        .is_err(),
        "mutated v5 path accepted"
    );
    let pad = witness
        .tiles
        .iter()
        .position(Vec::is_empty)
        .expect("missing v5 pad sample");
    let mut bad = proof.clone();
    bad.v4.as_mut().unwrap().tiles[pad] = vec![0; 1024];
    assert!(
        pom_v5::verify_pom_proof_v5(
            seed,
            bad.v4.as_ref().unwrap(),
            root,
            chunks,
            &pad_seed,
            pads
        )
        .is_err(),
        "noncanonical v5 pad accepted"
    );
    let mut wrong_root = *root;
    wrong_root[0] ^= 1;
    assert!(
        pom_v5::verify_pom_proof_v5(seed, witness, &wrong_root, chunks, &pad_seed, pads).is_err(),
        "wrong v5 root accepted"
    );
    let mut bad = proof.clone();
    bad.tier ^= 1;
    assert!(
        pom_v5::verify_pom_proof_v5_container(
            seed,
            &bad,
            chunks,
            root,
            &pad_seed,
            pads,
            &[0xff; 32],
            |s| pom_pow_value_h3(s, pph)
        )
        .is_err(),
        "wrong v5 tier accepted"
    );
    derived
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn official_attention_phase_window_vectors() {
        use keryx_consensus_core::collateral::attn_phase_window_daa;
        assert_eq!(attn_phase_window_daa(0, 0, 0, 0), 3_000);
        assert_eq!(attn_phase_window_daa(2, 1_000, 30_000, 30_900), 7_800);
        assert_eq!(attn_phase_window_daa(4, 8_000, 0, 5_000), 19_384);
        assert_eq!(attn_phase_window_daa(3, 100, 2_000, 9_000), 3_500);
    }
    #[test]
    fn pinned_official_geometry_and_vectors() {
        assert_eq!(
            params::POM_V5_PAD_TILES_H14,
            [638976, 3213312, 4804608, 6471680, 2450432]
        );
        assert_eq!(params::H15_ACTIVATION_DAA, 140115000);
        let seed = pom_v5::v5_pad_seed(1, &[7; 32], &[9; 32]);
        let entry = pom_v5::v5_pad_entry(&seed, 5);
        assert_eq!(
            entry,
            [
                6, 1, 50, 127, 89, 116, 202, 97, 233, 167, 140, 212, 71, 175, 221, 164, 205, 100,
                5, 11, 172, 109, 223, 85, 44, 225, 161, 101, 240, 71, 39, 224
            ]
        );
    }
}
