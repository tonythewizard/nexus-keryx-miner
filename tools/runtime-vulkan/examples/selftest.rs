//! Hardware check of the runtime on one GPU, without a pool or node: loads a model, answers a
//! prompt, then grinds synthetic PoM v5 jobs. It prints the walk rate and the lowest winners of an
//! easy target, so a run with `KERYX_POM_V4_KERNEL=dot` can be compared with the default kernel.
//! Nothing is mined or submitted.
//!
//!     cargo run --release --example selftest -- <model.gguf> [rate batches]

use std::ffi::{CStr, CString};
use std::time::Instant;

use keryx_llama_vk::*;

/// Tier-0 context pad from the H16 gate (`consensus/pom_v5.cpp`).
const PAD_TILES: u64 = 434_176;
const PROMPT: &str = "<|im_start|>user\nName the largest planet in one word.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n";

fn main() {
    let mut args = std::env::args().skip(1);
    let model = args.next().expect("usage: selftest <model.gguf> [rate batches]");
    let rate_batches: u32 = args.next().map_or(32, |n| n.parse().expect("rate batches"));

    let path = CString::new(model).unwrap();
    let t0 = Instant::now();
    let handle = unsafe { keryx_llama_load(path.as_ptr(), 0, 512) };
    if handle.is_null() {
        panic!("load failed: {}", last_error());
    }
    println!("load: {:.1} s", t0.elapsed().as_secs_f64());
    assert!(unsafe { keryx_llama_pom_ready(handle) }, "no PoM walk: {}", last_error());
    println!("chunks: {}", unsafe { keryx_llama_pom_n_chunks(handle) });

    let prompt = CString::new(PROMPT).unwrap();
    let mut out = vec![0u8; 4096];
    let n = unsafe { keryx_llama_generate(handle, prompt.as_ptr(), 16, out.as_mut_ptr().cast(), out.len() as i32) };
    assert!(n > 0, "generate failed: {}", last_error());
    println!("answer: {:?}", String::from_utf8_lossy(&out[..n as usize]));

    let batch = keryx_llama_pom_v5_batch(handle);
    let (p, s, seed) = ([0x1111u64, 0x2222, 0x3333, 0x4444], [0x5555u64, 0x6666, 0x7777, 0x8888], [9u64, 10, 11, 12]);
    let ts = 1_790_000_000_000u64;
    let mine = |t: &[u64; 4], start: u64| unsafe {
        keryx_llama_pom_mine_v5(handle, p.as_ptr(), s.as_ptr(), t.as_ptr(), ts, start, batch, seed.as_ptr(), PAD_TILES)
    };

    // A target no nonce meets: every call walks the whole batch.
    let hard = [0u64; 4];
    let t0 = Instant::now();
    assert_eq!(mine(&hard, 0), -1, "{}", last_error());
    println!("pad fill + first batch: {:.2} s", t0.elapsed().as_secs_f64());
    let t0 = Instant::now();
    for i in 1..=rate_batches {
        assert_eq!(mine(&hard, i as u64 * batch as u64), -1, "{}", last_error());
    }
    let secs = t0.elapsed().as_secs_f64();
    let nonces = rate_batches as f64 * batch as f64;
    println!("rate: {:.0} nonces/s ({:.2} M walk steps/s)", nonces / secs, nonces * 256.0 / secs / 1e6);

    // About one nonce in 256 meets this target.
    let easy = [u64::MAX, u64::MAX, u64::MAX, 0x00FF_FFFF_FFFF_FFFF];
    let winners: Vec<i64> = (0..8u64).map(|k| mine(&easy, k * 1_000_003)).collect();
    println!("easy-target winners: {winners:?}");

    let t0 = Instant::now();
    unsafe { keryx_llama_pom_release_pad(handle) };
    assert_eq!(mine(&hard, 0), -1, "{}", last_error());
    println!("pad release + refill + batch: {:.2} s", t0.elapsed().as_secs_f64());

    unsafe { keryx_llama_free(handle) };
}

fn last_error() -> String {
    unsafe { CStr::from_ptr(keryx_llama_last_error()) }.to_string_lossy().into_owned()
}
