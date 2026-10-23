//! Local bounded transport; consensus verification is shared with verify-real.
use std::io::{self, Read, Write};

fn cpu_ns() -> u64 {
    let mut usage: libc::rusage = unsafe { std::mem::zeroed() };
    assert_eq!(unsafe { libc::getrusage(libc::RUSAGE_SELF, &mut usage) }, 0);
    ((usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) as u64) * 1_000_000_000
        + ((usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) as u64) * 1_000
}

pub fn run() {
    let mut input = io::stdin().lock();
    let mut output = io::stdout().lock();
    let mut last_id = 0;
    let mut context = Vec::with_capacity(225 + 2 * 1024 * 1024);
    let mut trace = vec![0u8; 28 + 257 * 1024 + 256 * 8];
    let mut proof = Vec::with_capacity(2 * 1024 * 1024);
    let mut response = Vec::with_capacity(2112);
    loop {
        let mut header = [0u8; 28];
        let n = input.read(&mut header[..1]).expect("read verification frame");
        if n == 0 { return; }
        input.read_exact(&mut header[1..]).expect("truncated verification frame");
        assert_eq!(&header[..4], b"NXVR");
        assert_eq!(u32::from_le_bytes(header[4..8].try_into().unwrap()), 1);
        let id = u64::from_le_bytes(header[8..16].try_into().unwrap());
        assert!(id > last_id, "replayed verification frame");
        last_id = id;
        let context_len = u32::from_le_bytes(header[16..20].try_into().unwrap()) as usize;
        let trace_len = u32::from_le_bytes(header[20..24].try_into().unwrap()) as usize;
        let proof_len = u32::from_le_bytes(header[24..28].try_into().unwrap()) as usize;
        assert_eq!(context_len, 225);
        assert_eq!(trace_len, 28 + 257 * 1024 + 256 * 8);
        assert!(proof_len > 0 && proof_len <= 2 * 1024 * 1024);
        context.resize(context_len, 0);
        input.read_exact(&mut context[..context_len]).expect("truncated context");
        input.read_exact(&mut trace).expect("truncated trace");
        proof.resize(proof_len, 0);
        input.read_exact(&mut proof[..proof_len]).expect("truncated proof");
        assert_eq!(u32::from_le_bytes(context[221..225].try_into().unwrap()) as usize, proof_len);
        context.extend_from_slice(&proof);
        let start = cpu_ns();
        let summary = crate::real_verify::verify_bytes(&context, &trace, &proof);
        let used = cpu_ns().checked_sub(start).expect("invalid CPU interval");
        assert!(summary.len() <= 2048);
        response.clear();
        response.extend_from_slice(b"NXVA");
        response.extend_from_slice(&1u32.to_le_bytes());
        response.extend_from_slice(&id.to_le_bytes());
        response.extend_from_slice(&0u32.to_le_bytes());
        response.extend_from_slice(&used.to_le_bytes());
        response.extend_from_slice(blake3::hash(&proof).as_bytes());
        response.extend_from_slice(&(summary.len() as u32).to_le_bytes());
        response.extend_from_slice(summary.as_bytes());
        output.write_all(&response).expect("verification response write failed");
        output.flush().expect("verification response flush failed");
    }
}
