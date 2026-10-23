use std::{
    fs,
    io::{Read, Seek, SeekFrom},
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::Path,
};
type H = [u8; 32];
fn take<'a>(b: &'a [u8], p: &mut usize, n: usize) -> &'a [u8] {
    let e = p.checked_add(n).expect("index overflow");
    assert!(e <= b.len(), "truncated canonical index");
    let s = &b[*p..e];
    *p = e;
    s
}
fn u32le(b: &[u8], p: &mut usize) -> u32 {
    u32::from_le_bytes(take(b, p, 4).try_into().unwrap())
}
fn u64le(b: &[u8], p: &mut usize) -> u64 {
    u64::from_le_bytes(take(b, p, 8).try_into().unwrap())
}
fn pair(a: &H, b: &H) -> H {
    let mut x = [0u8; 64];
    x[..32].copy_from_slice(a);
    x[32..].copy_from_slice(b);
    *blake3::hash(&x).as_bytes()
}
fn available_kib() -> u64 {
    let s = fs::read_to_string("/proc/meminfo").expect("read meminfo");
    for l in s.lines() {
        if let Some(v) = l.strip_prefix("MemAvailable:") {
            return v.split_whitespace().next().unwrap().parse().unwrap();
        }
    }
    panic!("MemAvailable unavailable")
}
fn full_avg10() -> f64 {
    let s = fs::read_to_string("/proc/pressure/memory").expect("read memory PSI");
    let l = s
        .lines()
        .find(|l| l.starts_with("full "))
        .expect("memory full PSI missing");
    l.split_whitespace()
        .find_map(|x| x.strip_prefix("avg10="))
        .unwrap()
        .parse()
        .unwrap()
}
fn proc_start(exe: &str, daemon: bool) -> (u32, String) {
    let mut found = Vec::new();
    for e in fs::read_dir("/proc").unwrap() {
        let e = e.unwrap();
        let n = e.file_name().to_string_lossy().into_owned();
        if !n.bytes().all(|b| b.is_ascii_digit()) {
            continue;
        }
        let pid = n.parse::<u32>().unwrap();
        let actual = fs::read_link(e.path().join("exe"))
            .ok()
            .map(|p| p.canonicalize().unwrap_or(p));
        if actual
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned())
            .as_deref()
            != Some(exe)
        {
            continue;
        }
        if daemon {
            let args = fs::read(e.path().join("cmdline")).unwrap_or_default();
            if !args.split(|b| *b == 0).any(|a| a == b"daemon") {
                continue;
            }
        }
        let st = fs::read_to_string(e.path().join("stat")).unwrap();
        let tail = &st[st.rfind(')').unwrap() + 2..];
        let start = tail.split_whitespace().nth(19).unwrap().to_string();
        found.push((pid, start));
    }
    assert_eq!(found.len(), 1, "service process identity ambiguous: {exe}");
    found.remove(0)
}
fn same(id: &(u32, String), exe: &str) -> bool {
    let p = format!("/proc/{}/exe", id.0);
    let actual = fs::read_link(&p)
        .ok()
        .map(|x| x.canonicalize().unwrap_or(x));
    if actual
        .as_ref()
        .map(|x| x.to_string_lossy().into_owned())
        .as_deref()
        != Some(exe)
    {
        return false;
    }
    let st = fs::read_to_string(format!("/proc/{}/stat", id.0)).ok();
    st.map(|x| {
        x[x.rfind(')').unwrap() + 2..]
            .split_whitespace()
            .nth(19)
            .unwrap_or("")
            == id.1
    })
    .unwrap_or(false)
}
pub fn verify(model_path: &Path, index_path: &Path) {
    let idx = fs::read(index_path).expect("read C++ canonical index");
    assert!(idx.starts_with(b"NXIX"));
    let mut p = 4;
    let expected_size = u64le(&idx, &mut p);
    let n = u32le(&idx, &mut p);
    assert_eq!(n, 427);
    let metadata = fs::metadata(model_path).expect("stat GGUF");
    assert_eq!(metadata.len(), expected_size);
    let mut ranges = Vec::with_capacity(n as usize);
    let mut physical = Vec::with_capacity(n as usize);
    let mut previous = String::new();
    let mut chunks = 0u64;
    for _ in 0..n {
        let len = u32le(&idx, &mut p) as usize;
        let name = String::from_utf8(take(&idx, &mut p, len).to_vec()).expect("UTF-8 tensor name");
        assert!(
            previous.is_empty() || previous < name,
            "tensor index not strictly sorted"
        );
        previous = name;
        let off = u64le(&idx, &mut p);
        let bytes = u64le(&idx, &mut p);
        assert!(
            off <= expected_size && bytes <= expected_size - off,
            "tensor range outside GGUF"
        );
        ranges.push((off, bytes));
        physical.push((off, off + bytes));
        chunks += bytes / 32;
    }
    physical.sort_unstable();
    for pair in physical.windows(2) {
        assert!(pair[1].0 >= pair[0].1, "overlapping tensor byte ranges");
    }
    assert_eq!(p, idx.len());
    assert_eq!(chunks, 203_469_888);
    let avail = available_kib();
    let psi = full_avg10();
    let swap_free = fs::read_to_string("/proc/meminfo")
        .unwrap()
        .lines()
        .find_map(|l| l.strip_prefix("SwapFree:"))
        .unwrap()
        .split_whitespace()
        .next()
        .unwrap()
        .parse::<u64>()
        .unwrap();
    assert!(
        avail >= 2 * 1024 * 1024 && swap_free >= 2 * 1024 * 1024 && psi <= 0.10,
        "Rust root preflight blocked: MemAvailable={avail} KiB SwapFree={swap_free} KiB PSI={psi}"
    );
    let node = proc_start(
        &std::env::var("NEXUS_NODE_EXECUTABLE").map_err(|_| "NEXUS_NODE_EXECUTABLE required")?,
        false,
    );
    let ipfs = proc_start("/usr/bin/ipfs", true);
    let mut opts = fs::OpenOptions::new();
    opts.read(true)
        .custom_flags(libc::O_CLOEXEC | libc::O_NOATIME);
    let mut f = opts
        .open(model_path)
        .expect("open GGUF read-only with O_NOATIME");
    let mut stack: [Option<H>; 64] = [None; 64];
    let mut slab = vec![0u8; 2 * 1024 * 1024];
    let mut processed = 0u64;
    let started = std::time::Instant::now();
    for (off, bytes) in ranges {
        let count = bytes / 32;
        let mut done = 0u64;
        while done < count {
            let take_chunks = ((slab.len() / 32) as u64).min(count - done);
            let amount = (take_chunks * 32) as usize;
            let fileoff = off + done * 32;
            f.seek(SeekFrom::Start(fileoff)).expect("seek GGUF");
            f.read_exact(&mut slab[..amount]).expect("read GGUF slab");
            for c in slab[..amount].chunks_exact(32) {
                let mut h = *blake3::hash(c).as_bytes();
                let mut level = 0;
                while let Some(left) = stack[level].take() {
                    h = pair(&left, &h);
                    level += 1;
                }
                stack[level] = Some(h);
                processed += 1;
            }
            let rc = unsafe {
                libc::posix_fadvise(
                    f.as_raw_fd(),
                    fileoff as libc::off_t,
                    amount as libc::off_t,
                    libc::POSIX_FADV_DONTNEED,
                )
            };
            assert_eq!(rc, 0, "page-cache release advice failed");
            done += take_chunks;
            if processed % 1_048_576 == 0 {
                let avail = available_kib();
                let psi = full_avg10();
                assert!(
                    avail >= 1_048_576 && psi <= 0.50,
                    "resource guard aborted Rust root scan: MemAvailable={avail} KiB PSI={psi}"
                );
                assert!(
                    same(
                        &node,
                        &std::env::var("NEXUS_NODE_EXECUTABLE").map_err(|_| "NEXUS_NODE_EXECUTABLE required")?
                    ),
                    "keryxd identity changed"
                );
                assert!(same(&ipfs, "/usr/bin/ipfs"), "IPFS identity changed");
                eprintln!("RUST_ROOT_PROGRESS chunks={processed}/{chunks} mem_available_kib={avail} psi_full_avg10={psi}");
            }
        }
    }
    let mut level = stack
        .iter()
        .position(Option::is_some)
        .expect("nonempty tree");
    let mut h = stack[level].take().unwrap();
    for next in level + 1..stack.len() {
        if let Some(left) = stack[next].take() {
            while level < next {
                h = pair(&h, &h);
                level += 1;
            }
            h = pair(&left, &h);
            level = next + 1;
        }
    }
    let expected: [u8; 32] = [
        0x2c, 0x49, 0x71, 0x64, 0xea, 0xf2, 0x00, 0x78, 0xad, 0xd2, 0x0e, 0x82, 0xae, 0x4e, 0x1b,
        0x0f, 0xdb, 0x27, 0xd3, 0xfd, 0xd5, 0xea, 0xef, 0xc1, 0xc4, 0x8f, 0x20, 0x41, 0x11, 0xe1,
        0x4e, 0x88,
    ];
    assert_eq!(h, expected, "independent Rust GGUF root mismatch");
    assert!(same(
        &node,
        &std::env::var("NEXUS_NODE_EXECUTABLE").map_err(|_| "NEXUS_NODE_EXECUTABLE required")?
    ));
    assert!(same(&ipfs, "/usr/bin/ipfs"));
    println!("OFFICIAL_RUST_GGUF_ROOT=PASS chunks={processed} tensors={n} root=H6-tier0 elapsed_ms={} streaming_buffer_bytes={}",started.elapsed().as_millis(),slab.len());
}
