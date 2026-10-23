use keryx_consensus_core::{
    pom::{pom_block_seed_rewalk_era, pom_pow_value_h3},
    pom_v4,
};
use std::{
    fs::File,
    io::{BufReader, Read},
    path::Path,
};
const TILE: usize = 1024;
const K: usize = 256;
const STATES_BYTES: usize = 257 * TILE;
const TILES_BYTES: usize = K * TILE;
const OFFSETS_BYTES: usize = K * 8;
const HEADER_BYTES: u64 = 108;
const RECORD_BYTES: u64 = (16 + TILES_BYTES + OFFSETS_BYTES + STATES_BYTES) as u64;

fn expected_artifact_size(count: usize) -> Option<u64> {
    if !(1..=256).contains(&count) {
        return None;
    }
    HEADER_BYTES.checked_add(RECORD_BYTES.checked_mul(count as u64)?)
}

fn read_exact<const N: usize>(r: &mut impl Read) -> [u8; N] {
    let mut out = [0u8; N];
    r.read_exact(&mut out)
        .expect("truncated CUDA parity artifact");
    out
}
fn read_u32(r: &mut impl Read) -> u32 {
    u32::from_le_bytes(read_exact::<4>(r))
}
fn read_u64(r: &mut impl Read) -> u64 {
    u64::from_le_bytes(read_exact::<8>(r))
}
pub fn verify(model_path: &Path, index_path: &Path, path: &Path) {
    let file = File::open(path).expect("open CUDA parity artifact");
    let size = file.metadata().expect("stat CUDA parity artifact").len();
    let mut r = BufReader::with_capacity(64 * 1024, file);
    assert_eq!(&read_exact::<4>(&mut r), b"NXCB", "bad CUDA batch magic");
    assert_eq!(
        read_u32(&mut r),
        2,
        "unsupported CUDA parity artifact version"
    );
    let count = read_u32(&mut r) as usize;
    assert!((1..=256).contains(&count), "unexpected CUDA segment size");
    let expected_size = expected_artifact_size(count).expect("unsupported logical batch size");
    assert_eq!(
        size, expected_size,
        "CUDA batch artifact length does not match logical nonce count"
    );
    let ntiles = read_u64(&mut r);
    assert!(ntiles > 0);
    let nonce_base = read_u64(&mut r);
    let timestamp = read_u64(&mut r);
    let daa = read_u64(&mut r);
    let pph = read_exact::<32>(&mut r);
    let target = read_exact::<32>(&mut r);
    keryx_consensus_core::pom::init_pom_level_activation(43_450_000);
    keryx_consensus_core::pom::init_service_commit_activation(76_316_623);
    keryx_consensus_core::pom::init_pom_seed_h10_activation(87_360_000);
    keryx_consensus_core::pom::init_private_inference_activation(121_985_000);
    let canonical = crate::gguf_index::verify_index(model_path, index_path);
    assert_eq!(
        ntiles,
        canonical.chunks / 32,
        "CUDA batch tile count differs from independently parsed model"
    );

    let mut target_candidates = 0usize;
    let mut tiles = vec![0u8; TILES_BYTES];
    let mut claimed_offsets = vec![0u8; OFFSETS_BYTES];
    let mut claimed_states = vec![0u8; STATES_BYTES];
    for nonce_index in 0..count {
        let seed = read_u64(&mut r);
        let nonce = read_u64(&mut r);
        assert_eq!(
            nonce,
            nonce_base
                .checked_add(nonce_index as u64)
                .expect("nonce overflow")
        );
        assert_eq!(
            seed,
            pom_block_seed_rewalk_era(&pph, timestamp, nonce, daa),
            "official Rust seed mismatch nonce={nonce} daa={daa}"
        );
        r.read_exact(&mut tiles)
            .expect("truncated real model tile stream");
        r.read_exact(&mut claimed_offsets)
            .expect("truncated offset stream");
        r.read_exact(&mut claimed_states)
            .expect("truncated state stream");

        let tile_vec: Vec<Vec<u8>> = tiles.chunks_exact(TILE).map(|x| x.to_vec()).collect();
        let expected_offsets = pom_v4::v4_offset_chain(seed, &tile_vec, ntiles);
        for (i, off) in expected_offsets.iter().enumerate() {
            let actual = u64::from_le_bytes(claimed_offsets[i * 8..i * 8 + 8].try_into().unwrap());
            assert_eq!(actual, *off,
                       "Rust/CUDA offset mismatch nonce_index={nonce_index} iteration={i} cuda={actual} rust={off}");
        }
        drop(tile_vec);

        let actual_tiles = canonical.read_tiles(model_path, &expected_offsets);
        for (step, (actual, claimed)) in actual_tiles
            .iter()
            .zip(tiles.chunks_exact(TILE))
            .enumerate()
        {
            if actual.as_slice() != claimed {
                let byte = actual
                    .iter()
                    .zip(claimed)
                    .position(|(a, b)| a != b)
                    .unwrap();
                panic!("model tile mismatch nonce_index={nonce_index} iteration={} tile_offset={} byte={byte}",
                       step + 1, expected_offsets[step]);
            }
        }
        drop(actual_tiles);

        let mut state = pom_v4::v4_initial_state(seed);
        assert_eq!(
            &claimed_states[..TILE],
            state.as_slice(),
            "initial state mismatch nonce_index={nonce_index}"
        );
        for step in 1..=K {
            let tile = &tiles[(step - 1) * TILE..step * TILE];
            state = pom_v4::v4_transition(&state, tile, step as u32);
            let expected = &claimed_states[step * TILE..(step + 1) * TILE];
            if state != expected {
                let byte = state
                    .iter()
                    .zip(expected)
                    .position(|(a, b)| a != b)
                    .unwrap();
                panic!("Rust/CUDA state mismatch nonce_index={nonce_index} iteration={step} byte={byte} rust={} cuda={}",
                       state[byte], expected[byte]);
            }
        }
        let final_root = pom_v4::v4_state_root(&state);
        let final_state = u64::from_le_bytes(final_root[..8].try_into().unwrap());
        let pow = pom_pow_value_h3(final_state, &pph);
        if pow.iter().rev().cmp(target.iter().rev()) != std::cmp::Ordering::Greater {
            target_candidates += 1;
        }
    }
    let mut trailing = [0u8; 1];
    assert_eq!(
        r.read(&mut trailing).expect("read batch EOF"),
        0,
        "trailing bytes in CUDA batch artifact"
    );
    println!("OFFICIAL_RUST_CUDA_SEGMENT=PASS segment_nonces={count} states_per_nonce=257 offsets_per_nonce=256 real_model_tiles=verified official_seeds=verified target_candidates={target_candidates} streaming_memory_bound_bytes={} submit=DISABLED",
             TILES_BYTES + OFFSETS_BYTES + STATES_BYTES + 64 * 1024);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn v2_logical_file_sizes_include_every_nonce_record() {
        assert_eq!(expected_artifact_size(1), Some(527_484));
        assert_eq!(expected_artifact_size(8), Some(4_219_116));
        assert_eq!(expected_artifact_size(64), Some(33_752_172));
        assert_eq!(expected_artifact_size(256), Some(135_008_364));
        assert_eq!(expected_artifact_size(4), Some(2_109_612));
        assert_eq!(expected_artifact_size(3), Some(1_582_236));
        assert_eq!(expected_artifact_size(0), None);
        assert_eq!(expected_artifact_size(257), None);
    }
}
