//! Vulkan `keryx_llama` runtime for `nexus-live-vulkan-worker`: the C symbols
//! `src/network/opoi.cpp` resolves, at ABI 9 (generation, traced generation with replays and
//! sequence leaves, and the PoM walk).
//!
//! Inference is llama.cpp's Vulkan backend in-process, and the PoM v4/v5 walk is the
//! keryx-miner-rdna3 engine (`vendor/keryx-miner-rdna3`). The walk reads the weights llama.cpp
//! already holds in VRAM where the device llama.cpp created allows it, and otherwise streams the
//! canonical layout into a device of its own. The worker re-walks and independently verifies
//! every winner before submitting it, so a GPU fault costs shares, never validity.
//!
//! The GPU is the first discrete Vulkan device, or raw Vulkan device `KERYX_INFER_GPU`.
//! `KERYX_POM_V4_KERNEL=dot` forces the v_dot4 walk; `KERYX_VK_LOG` sets the log level (info).

#[path = "../../../vendor/keryx-miner-rdna3/src/llm_engine.rs"]
#[allow(dead_code, unused_imports)]
mod llm_engine;

mod canonical;
mod device;
mod models;
mod trace;

use std::cell::RefCell;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::{Mutex, MutexGuard};

use canonical::CanonicalLayout;
use keryx_vulkan::pom_walk::{PomWalkGpu, PomWalkShared};
use keryx_vulkan::pom_walk_v4::V4Job;
use llm_engine::{LlamaEngine, PromptPrefix};
use trace::{TraceError, TraceState};

/// Generation, traced generation with replays and sequence leaves, and PoM.
const ABI: c_int = 9;
/// Largest leaf a replay captures (the engine's capture cap).
const REPLAY_SLOT_CAP: usize = 4 << 20;
/// Nonces per mining call: one TDR-bounded walk dispatch.
const POM_BATCH: u32 = 1 << 15;
/// Canonical chunks read through the shared walk's table and compared with the GGUF.
const SAMPLE_CHUNKS: u32 = 256;
/// Walk steps per nonce (`consensus::pom_v4` K).
const POM_STEPS: u32 = 256;
/// `cSHAKE256("ProofOfWorkHash")` sponge before anything is absorbed — the `initial` state of
/// `h10_impl` in consensus/pom_v4.cpp.
#[rustfmt::skip]
const POW_HASH_INITIAL_STATE: [u64; 25] = [
    1242148031264380989, 3008272977830772284, 2188519011337848018, 1992179434288343456, 8876506674959887717,
    5399642050693751366, 1745875063082670864, 8605242046444978844, 17936695144567157056, 3343109343542796272,
    1123092876221303306, 4963925045340115282, 17037383077651887893, 16629644495023626889, 12833675776649114147,
    3784524041015224902, 1082795874807940378, 13952716920571277634, 13411128033953605860, 15060696040649351053,
    9928834659948351306, 5237849264682708699, 12825353012139217522, 6706187291358897596, 196324915476054915,
];

enum Walk {
    /// Reads llama.cpp's resident weights in place.
    Shared(PomWalkShared),
    /// Holds its own canonical copy on a device it created.
    Dedicated(PomWalkGpu),
}

impl Walk {
    fn n_chunks(&self) -> u64 {
        match self {
            Walk::Shared(w) => w.n_chunks(),
            Walk::Dedicated(w) => w.n_chunks(),
        }
    }

    fn mine_v4(&self, job: &V4Job, start: u64, batch: u32) -> Option<u64> {
        match self {
            Walk::Shared(w) => w.mine_v4(job, start, batch),
            Walk::Dedicated(w) => w.mine_v4(job, start, batch),
        }
    }

    fn mine_v5(&self, job: &V4Job, start: u64, batch: u32, seed: &[u64; 4], tiles: u64) -> Result<Option<u64>, String> {
        match self {
            Walk::Shared(w) => w.mine_v5(job, start, batch, seed, tiles),
            Walk::Dedicated(w) => w.mine_v5(job, start, batch, seed, tiles),
        }
    }

    fn release_pad(&self) -> u64 {
        match self {
            Walk::Shared(w) => w.release_pad(),
            Walk::Dedicated(w) => w.release_pad(),
        }
    }
}

struct Runtime {
    // Fields drop in order: the walk goes before the engine whose buffers it may read.
    walk: Option<Walk>,
    trace: Mutex<TraceState>,
    /// Context size of every traced evaluation and replay.
    n_ctx: u32,
    engine: LlamaEngine,
}

impl Runtime {
    fn trace(&self) -> MutexGuard<'_, TraceState> {
        self.trace.lock().unwrap_or_else(|p| p.into_inner())
    }
}

/// The walk for the model `engine` serves from `gguf`.
fn build_walk(engine: &LlamaEngine, gguf: &str) -> Result<Walk, String> {
    let layout = CanonicalLayout::open(gguf)?;
    let shared = device::shared_device()?;
    log::info!(
        "PoM: {} (vendor {:#06x}, {:?}); llama.cpp enabled matrix cores: {}, integer dot: {}",
        shared.name,
        shared.vendor_id,
        shared.arch,
        shared.matrix_cores,
        shared.integer_dot
    );
    if !shared.matrix_cores && std::env::var_os("KERYX_POM_V4_KERNEL").is_none() {
        // Where llama.cpp leaves cooperative matrices off, AMD drivers only emulate them.
        std::env::set_var("KERYX_POM_V4_KERNEL", "dot");
    }
    if shared.integer_dot {
        let checked = engine
            .build_shared_walk()
            .map_err(|e| format!("{e:#}"))
            .and_then(|(walk, _)| check_shared(&walk, &layout).map(|()| walk));
        match checked {
            Ok(walk) => {
                log::info!(
                    "PoM: walking llama.cpp's resident weights ({} chunks, {} sampled against the GGUF), v4 walk on {}",
                    walk.n_chunks(),
                    SAMPLE_CHUNKS,
                    walk.v4_kernel()
                );
                return Ok(Walk::Shared(walk));
            }
            Err(e) => log::warn!("PoM: walk over llama.cpp's weights refused ({e}); loading a dedicated copy"),
        }
    } else {
        log::warn!("PoM: llama.cpp's device has no accelerated integer dot product; loading a dedicated copy");
    }
    let mut source = |first: u64, out: &mut [u8]| layout.read_chunks(first, out);
    let walk = PomWalkGpu::new_streamed(Some(keryx_vulkan::inference_device_index()), layout.n_chunks(), &mut source)?;
    log::info!("PoM: dedicated copy of {} chunks on {}, v4 walk on {}", walk.n_chunks(), walk.device_name(), walk.v4_kernel());
    Ok(Walk::Dedicated(walk))
}

/// The shared walk's table must address exactly the GGUF's canonical chunks.
fn check_shared(walk: &PomWalkShared, layout: &CanonicalLayout) -> Result<(), String> {
    if walk.n_chunks() != layout.n_chunks() {
        return Err(format!("{} resident chunks, the GGUF has {}", walk.n_chunks(), layout.n_chunks()));
    }
    let mut x = 0x9E37_79B9_7F4A_7C15u64 ^ layout.n_chunks();
    let mut want = [0u8; 32];
    for _ in 0..SAMPLE_CHUNKS {
        x = x.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = (x ^ (x >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        let chunk = (z ^ (z >> 31)) % layout.n_chunks();
        layout.read_chunks(chunk, &mut want)?;
        if walk.read_chunk(chunk) != want {
            return Err(format!("chunk {chunk} differs from the GGUF"));
        }
    }
    Ok(())
}

/// The walk inputs of one job. `s` is the H14-tagged pre-PoW hash, so the H10 sponge the shader
/// absorbs each nonce into is the initial state with `s` and the timestamp absorbed.
unsafe fn job(p: *const u64, s: *const u64, t: *const u64, timestamp: u64) -> Result<V4Job, String> {
    let (p, s, t) = (words(p)?, words(s)?, words(t)?);
    let mut h10 = POW_HASH_INITIAL_STATE;
    for (lane, w) in h10.iter_mut().zip(s) {
        *lane ^= w;
    }
    h10[4] ^= timestamp;
    let mut target_le = [0u8; 32];
    for (i, w) in t.iter().enumerate() {
        target_le[i * 8..i * 8 + 8].copy_from_slice(&w.to_le_bytes());
    }
    Ok(V4Job { pow_words: p, seed_words: s, timestamp, h10_state: Some(h10), target_le })
}

/// A walk result as the worker reads it: the winner's offset in the batch, or -1.
fn batch_offset(found: Option<u64>, start: u64, batch: u32) -> Result<i64, String> {
    match found {
        None => Ok(-1),
        Some(nonce) if nonce.wrapping_sub(start) < batch as u64 => Ok(nonce.wrapping_sub(start) as i64),
        Some(nonce) => Err(format!("walk returned nonce {nonce} outside [{start}, +{batch})")),
    }
}

unsafe fn words(p: *const u64) -> Result<[u64; 4], String> {
    if p.is_null() {
        return Err("null word array".into());
    }
    Ok(std::ptr::read_unaligned(p as *const [u64; 4]))
}

unsafe fn text<'a>(s: *const c_char) -> Result<&'a str, String> {
    if s.is_null() {
        return Err("null string".into());
    }
    CStr::from_ptr(s).to_str().map_err(|_| "string is not UTF-8".to_string())
}

unsafe fn runtime<'a>(model: *mut c_void) -> Result<&'a Runtime, String> {
    (model as *const Runtime).as_ref().ok_or_else(|| "null model handle".to_string())
}

unsafe fn walk<'a>(model: *mut c_void) -> Result<&'a Walk, String> {
    runtime(model)?.walk.as_ref().ok_or_else(|| "no PoM walk on this device".to_string())
}

fn complete(rt: &Runtime, prompt: &str, max_tokens: c_int, out: *mut c_char, cap: c_int) -> Result<c_int, String> {
    if max_tokens <= 0 || cap <= 0 || out.is_null() {
        return Err("invalid generation arguments".into());
    }
    let answer = rt.engine.generate(prompt, max_tokens as usize, &[]).map_err(|e| format!("{e:#}"))?;
    let mut n = answer.len().min(cap as usize);
    while !answer.is_char_boundary(n) {
        n -= 1;
    }
    if n == 0 {
        return Err("empty completion".into());
    }
    // SAFETY: the caller's buffer holds `cap` >= n bytes.
    unsafe { std::ptr::copy_nonoverlapping(answer.as_ptr(), out as *mut u8, n) };
    Ok(n as c_int)
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_error(message: String) {
    log::error!("{message}");
    let message = CString::new(message.replace('\0', " ")).unwrap_or_default();
    LAST_ERROR.with(|e| *e.borrow_mut() = message);
}

fn panic_message(panic: &(dyn std::any::Any + Send)) -> String {
    let what = panic
        .downcast_ref::<&str>()
        .map(|s| s.to_string())
        .or_else(|| panic.downcast_ref::<String>().cloned())
        .unwrap_or_default();
    format!("panic: {what}")
}

/// Runs an export body: an error or a panic sets the last error and yields `fallback`.
fn guard<T>(fallback: T, body: impl FnOnce() -> Result<T, String>) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(value)) => value,
        Ok(Err(message)) => {
            set_error(message);
            fallback
        }
        Err(panic) => {
            set_error(panic_message(&*panic));
            fallback
        }
    }
}

/// Runs a trace export: a failure sets the last error and returns its ABI code (-5 for a panic).
fn traced(body: impl FnOnce() -> Result<c_int, TraceError>) -> c_int {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(Ok(value)) => value,
        Ok(Err(e)) => {
            set_error(e.message);
            e.code
        }
        Err(panic) => {
            set_error(panic_message(&*panic));
            -5
        }
    }
}

fn bad(message: &str) -> TraceError {
    TraceError { code: -1, message: message.into() }
}

unsafe fn trace_runtime<'a>(model: *mut c_void) -> Result<&'a Runtime, TraceError> {
    runtime(model).map_err(|e| bad(&e))
}

unsafe fn slice<'a, T>(p: *const T, n: usize) -> Result<&'a [T], TraceError> {
    if p.is_null() && n > 0 {
        return Err(bad("null array"));
    }
    Ok(if n == 0 { &[] } else { std::slice::from_raw_parts(p, n) })
}

/// Copies `src` into the caller's buffer of `cap` elements at `dst`.
unsafe fn copy_out<T: Copy>(src: &[T], dst: *mut T, cap: usize) -> Result<(), TraceError> {
    if src.is_empty() {
        return Ok(());
    }
    if src.len() > cap || dst.is_null() {
        return Err(TraceError { code: -6, message: format!("{} elements for a buffer of {cap}", src.len()) });
    }
    std::ptr::copy_nonoverlapping(src.as_ptr(), dst, src.len());
    Ok(())
}

fn prompt_text<'a>(bytes: &'a [u8]) -> Result<&'a str, TraceError> {
    std::str::from_utf8(bytes).map_err(|_| bad("prompt is not UTF-8"))
}

/// The traced generation behind both exports: writes the answer (NUL-terminated) and every fed
/// token, as `keryx_llama_generate_traced` does.
#[allow(clippy::too_many_arguments)]
unsafe fn generate_traced(
    model: *mut c_void,
    prompt: &[u8],
    prefix: *const i32,
    n_prefix: c_int,
    head_tokens: c_int,
    rest: Option<&[u8]>,
    max_tokens: c_int,
    out: *mut c_char,
    cap: c_int,
    tokens: *mut i32,
    tokens_cap: c_int,
    n_prompt_out: *mut c_int,
    n_fed_out: *mut c_int,
    min_prompt: c_int,
    answer_bytes_per_position: c_int,
) -> Result<c_int, TraceError> {
    let rt = trace_runtime(model)?;
    if out.is_null() || cap < 2 || tokens.is_null() || tokens_cap < 1 || n_prompt_out.is_null() || n_fed_out.is_null() || max_tokens < 0 {
        return Err(bad("invalid traced generation arguments"));
    }
    let prompt = prompt_text(prompt)?;
    let prefix = if n_prefix > 0 {
        let rest = prompt_text(rest.ok_or_else(|| bad("prefix without rest"))?)?;
        Some(PromptPrefix { tokens: slice(prefix, n_prefix as usize)?, head_tokens: head_tokens.max(0) as u32, rest })
    } else {
        None
    };
    let g = rt.trace().generate(
        &rt.engine,
        rt.n_ctx,
        prompt,
        prefix,
        max_tokens as usize,
        min_prompt.max(0) as u32,
        answer_bytes_per_position.max(0) as u32,
    )?;
    let mut text = g.text.into_bytes();
    if text.len() >= cap as usize {
        return Err(TraceError { code: -6, message: format!("an answer of {} bytes for a buffer of {cap}", text.len()) });
    }
    copy_out(&g.tokens, tokens, tokens_cap as usize)?;
    text.push(0);
    copy_out(&text, out as *mut u8, cap as usize)?;
    *n_prompt_out = g.n_prompt as c_int;
    *n_fed_out = g.tokens.len() as c_int;
    Ok((text.len() - 1) as c_int)
}

mod logger {
    struct Stderr;

    impl log::Log for Stderr {
        fn enabled(&self, metadata: &log::Metadata) -> bool {
            metadata.level() <= log::max_level()
        }

        fn log(&self, record: &log::Record) {
            if self.enabled(record.metadata()) {
                eprintln!("keryx-llama-vk {} {}: {}", record.level(), record.target(), record.args());
            }
        }

        fn flush(&self) {}
    }

    pub fn init() {
        static LOGGER: Stderr = Stderr;
        if log::set_logger(&LOGGER).is_ok() {
            let level = std::env::var("KERYX_VK_LOG").ok().and_then(|v| v.parse().ok());
            log::set_max_level(level.unwrap_or(log::LevelFilter::Info));
        }
    }
}

#[no_mangle]
pub extern "C" fn keryx_llama_abi() -> c_int {
    ABI
}

#[no_mangle]
pub extern "C" fn keryx_llama_vk_abi() -> c_int {
    ABI
}

/// Binds the AMD device by physical identity before load. Returns the canonical
/// UUID (including its NUL) to the host for its interprocess lease.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_vk_select_device(identity: *const c_char, uuid: *mut c_char, cap: usize) -> c_int {
    guard(-1, || {
        if uuid.is_null() || cap < 41 { return Err("device UUID buffer too small".into()); }
        let selected = device::select(text(identity)?)?;
        let selected = CString::new(selected).map_err(|e| e.to_string())?;
        let bytes = selected.as_bytes_with_nul();
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), uuid.cast(), bytes.len());
        Ok(0)
    })
}

/// Loads `gguf` on the Vulkan GPU and prepares its PoM walk. `gpu` is ignored: the device is the
/// one described in the crate docs. `n_ctx` caps each request's context.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_load(gguf: *const c_char, _gpu: c_int, n_ctx: c_int) -> *mut c_void {
    logger::init();
    guard(std::ptr::null_mut(), || {
        let path = text(gguf)?;
        if n_ctx >= 256 {
            std::env::set_var("KERYX_LLAMA_CTX", n_ctx.to_string());
        }
        let engine = LlamaEngine::launch(path).map_err(|e| format!("{e:#}"))?;
        let walk = build_walk(&engine, path).map_err(|e| log::error!("PoM: no walk on this device: {e}")).ok();
        let runtime = Runtime { walk, trace: Mutex::default(), n_ctx: engine.ctx_cap(), engine };
        Ok(Box::into_raw(Box::new(runtime)) as *mut c_void)
    })
}

#[no_mangle]
pub unsafe extern "C" fn keryx_llama_free(model: *mut c_void) {
    if !model.is_null() {
        guard((), || {
            drop(Box::from_raw(model as *mut Runtime));
            Ok(())
        })
    }
}

#[no_mangle]
pub extern "C" fn keryx_llama_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}

/// Greedy completion of an already templated prompt; returns the bytes written to `out`.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_generate(
    model: *mut c_void,
    prompt: *const c_char,
    max_tokens: c_int,
    out: *mut c_char,
    cap: c_int,
) -> c_int {
    guard(-1, || complete(runtime(model)?, text(prompt)?, max_tokens, out, cap))
}

/// [`keryx_llama_generate`] for a prompt of `len` bytes that may contain NUL.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_generate_n(
    model: *mut c_void,
    prompt: *const c_char,
    len: usize,
    max_tokens: c_int,
    out: *mut c_char,
    cap: c_int,
) -> c_int {
    guard(-1, || {
        if prompt.is_null() {
            return Err("null prompt".into());
        }
        let bytes = std::slice::from_raw_parts(prompt as *const u8, len);
        let prompt = std::str::from_utf8(bytes).map_err(|_| "prompt is not UTF-8".to_string())?;
        complete(runtime(model)?, prompt, max_tokens, out, cap)
    })
}

#[no_mangle]
pub unsafe extern "C" fn keryx_llama_pom_ready(model: *mut c_void) -> bool {
    guard(false, || Ok(runtime(model)?.walk.is_some()))
}

#[no_mangle]
pub unsafe extern "C" fn keryx_llama_pom_n_chunks(model: *mut c_void) -> u64 {
    guard(0, || Ok(walk(model)?.n_chunks()))
}

#[no_mangle]
pub extern "C" fn keryx_llama_pom_v5_batch(_model: *mut c_void) -> u32 {
    POM_BATCH
}

/// PoM v4 search of nonces `[start, start + batch)` with the H10 seed; returns the lowest winner's
/// offset, -1 for none, or a value below -1 on failure.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_pom_mine(
    model: *mut c_void,
    p: *const u64,
    s: *const u64,
    t: *const u64,
    timestamp: u64,
    start: u64,
    batch: u32,
    steps: u32,
    h10: u32,
) -> i64 {
    guard(-3, || {
        if steps != POM_STEPS {
            return Err(format!("unsupported walk length {steps}"));
        }
        if h10 == 0 {
            return Err("the pre-H10 seed fold needs v4-salted seed words, which this ABI does not carry".into());
        }
        let walk = walk(model)?;
        if batch == 0 {
            return Ok(-1);
        }
        batch_offset(walk.mine_v4(&job(p, s, t, timestamp)?, start, batch), start, batch)
    })
}

/// PoM v5 search of nonces `[start, start + batch)` over the weights and the context pad of
/// `pad_seed` (filled on first use and after a release); results as [`keryx_llama_pom_mine`].
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_pom_mine_v5(
    model: *mut c_void,
    p: *const u64,
    s: *const u64,
    t: *const u64,
    timestamp: u64,
    start: u64,
    batch: u32,
    pad_seed: *const u64,
    pad_tiles: u64,
) -> i64 {
    guard(-3, || {
        let walk = walk(model)?;
        if batch == 0 {
            return Ok(-1);
        }
        if pad_tiles == 0 {
            return Err("PoM v5 needs a context pad".into());
        }
        let found = walk.mine_v5(&job(p, s, t, timestamp)?, start, batch, &words(pad_seed)?, pad_tiles)?;
        batch_offset(found, start, batch)
    })
}

/// Frees the context pad's VRAM before inference; the next v5 search refills it.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_pom_release_pad(model: *mut c_void) {
    guard((), || {
        if let Some(walk) = runtime(model)?.walk.as_ref() {
            let bytes = walk.release_pad();
            if bytes > 0 {
                log::debug!("PoM: context pad released ({} MiB)", bytes >> 20);
            }
        }
        Ok(())
    })
}

/// Configures the canonical trace of `manifest` for `max_pos` positions (the context size when 0).
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_trace_configure(model: *mut c_void, manifest: *const c_void, len: usize, max_pos: c_int) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if manifest.is_null() || len == 0 {
            return Err(bad("empty trace manifest"));
        }
        let max_pos = if max_pos > 0 { max_pos as u32 } else { rt.n_ctx };
        rt.trace().configure(&rt.engine, slice(manifest as *const u8, len)?, max_pos)?;
        Ok(0)
    })
}

/// Leaves per traced position, or -1 when the trace is not configured.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_trace_leaf_count(model: *mut c_void) -> c_int {
    traced(|| trace_runtime(model)?.trace().leaves().map(|n| n as c_int).ok_or_else(|| bad("the trace is not configured")))
}

/// Traced generation: the head `prompt`, the `prefix` token ids and `rest`, padded to `min_prompt`
/// positions, then the answer and its padding. Returns the answer's bytes or a negative code.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_generate_traced(
    model: *mut c_void,
    prompt: *const c_char,
    prefix: *const i32,
    n_prefix: c_int,
    head_tokens: c_int,
    rest: *const c_char,
    max_tokens: c_int,
    out: *mut c_char,
    cap: c_int,
    tokens: *mut i32,
    tokens_cap: c_int,
    n_prompt: *mut c_int,
    n_fed: *mut c_int,
    min_prompt: c_int,
    answer_bytes_per_position: c_int,
) -> c_int {
    traced(|| {
        if prompt.is_null() {
            return Err(bad("null prompt"));
        }
        let rest = (!rest.is_null()).then(|| CStr::from_ptr(rest).to_bytes());
        generate_traced(
            model,
            CStr::from_ptr(prompt).to_bytes(),
            prefix,
            n_prefix,
            head_tokens,
            rest,
            max_tokens,
            out,
            cap,
            tokens,
            tokens_cap,
            n_prompt,
            n_fed,
            min_prompt,
            answer_bytes_per_position,
        )
    })
}

/// [`keryx_llama_generate_traced`] with explicit lengths, so `prompt` and `rest` may contain NUL.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_generate_traced_n(
    model: *mut c_void,
    prompt: *const c_char,
    prompt_len: usize,
    prefix: *const i32,
    n_prefix: c_int,
    head_tokens: c_int,
    rest: *const c_char,
    rest_len: usize,
    max_tokens: c_int,
    out: *mut c_char,
    cap: c_int,
    tokens: *mut i32,
    tokens_cap: c_int,
    n_prompt: *mut c_int,
    n_fed: *mut c_int,
    min_prompt: c_int,
    answer_bytes_per_position: c_int,
) -> c_int {
    traced(|| {
        if prompt.is_null() {
            return Err(bad("null prompt"));
        }
        let rest = if rest.is_null() { None } else { Some(slice(rest as *const u8, rest_len)?) };
        generate_traced(
            model,
            slice(prompt as *const u8, prompt_len)?,
            prefix,
            n_prefix,
            head_tokens,
            rest,
            max_tokens,
            out,
            cap,
            tokens,
            tokens_cap,
            n_prompt,
            n_fed,
            min_prompt,
            answer_bytes_per_position,
        )
    })
}

/// Traced evaluation of `tokens` as a prompt with nothing generated.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_trace_tokens(model: *mut c_void, tokens: *const i32, n_tokens: c_int) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if n_tokens <= 0 {
            return Err(bad("no tokens"));
        }
        rt.trace().trace_tokens(&rt.engine, rt.n_ctx, slice(tokens, n_tokens as usize)?)?;
        Ok(0)
    })
}

/// Leaves (`n_pos × leaf_count × 32` bytes) and batch sizes of the last traced evaluation;
/// `n_pos` is set even when the buffers are null, to size them.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_trace_read(
    model: *mut c_void,
    leaves: *mut u8,
    leaves_cap: usize,
    ubatch: *mut u32,
    ubatch_cap: usize,
    n_pos: *mut u32,
) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if n_pos.is_null() {
            return Err(bad("null position count"));
        }
        let state = rt.trace();
        let (n, captured, batches) = state.last().ok_or_else(|| bad("no traced evaluation"))?;
        *n_pos = n as u32;
        if leaves.is_null() || ubatch.is_null() {
            return Ok(0);
        }
        copy_out(captured.as_flattened(), leaves, leaves_cap)?;
        copy_out(batches, ubatch, ubatch_cap)?;
        Ok(0)
    })
}

/// Replays `tokens` to position `pos` and copies leaves `leaf_x` and `leaf_y` there.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_replay(
    model: *mut c_void,
    tokens: *const i32,
    n_tokens: c_int,
    n_prompt: c_int,
    pos: u32,
    leaf_x: u32,
    leaf_y: u32,
    x: *mut u8,
    x_cap: usize,
    y: *mut u8,
    y_cap: usize,
    x_len: *mut u32,
    y_len: *mut u32,
) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if n_prompt <= 0 || n_tokens < n_prompt || x_len.is_null() || y_len.is_null() {
            return Err(bad("invalid replay arguments"));
        }
        let slot = x_cap.max(y_cap).min(REPLAY_SLOT_CAP) & !3;
        let toks = slice(tokens, n_tokens as usize)?;
        let got = rt.trace().replay(&rt.engine, rt.n_ctx, toks, n_prompt as u32, &[(pos, leaf_x), (pos, leaf_y)], slot)?;
        if got.iter().any(Vec::is_empty) {
            return Err(TraceError { code: -7, message: "nothing captured".into() });
        }
        copy_out(&got[0], x, x_cap)?;
        copy_out(&got[1], y, y_cap)?;
        (*x_len, *y_len) = (got[0].len() as u32, got[1].len() as u32);
        Ok(0)
    })
}

/// Replays `tokens` to the last of `n` positions and copies leaf `leaf[k]` at `pos[k]` into
/// `out + k * slot_bytes`, its length into `lens[k]`.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_replay_multi(
    model: *mut c_void,
    tokens: *const i32,
    n_tokens: c_int,
    n_prompt: c_int,
    pos: *const u32,
    leaf: *const u32,
    n: u32,
    slot_bytes: u32,
    out: *mut u8,
    out_cap: usize,
    lens: *mut u32,
) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if n_prompt <= 0 || n_tokens < n_prompt || n == 0 || out.is_null() || lens.is_null() {
            return Err(bad("invalid replay arguments"));
        }
        let slot = slot_bytes as usize;
        if out_cap < n as usize * slot {
            return Err(TraceError { code: -6, message: "replay buffer too small".into() });
        }
        let caps: Vec<(u32, u32)> = slice(pos, n as usize)?.iter().copied().zip(slice(leaf, n as usize)?.iter().copied()).collect();
        let got = rt.trace().replay(&rt.engine, rt.n_ctx, slice(tokens, n_tokens as usize)?, n_prompt as u32, &caps, slot)?;
        for (k, bytes) in got.iter().enumerate() {
            copy_out(bytes, out.add(k * slot), slot)?;
            *lens.add(k) = bytes.len() as u32;
        }
        Ok(0)
    })
}

/// The context size traced evaluations run in. Every traced evaluation and replay uses the one
/// size the runtime was loaded with, so a smaller request is granted without reallocating.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_context_acquire(model: *mut c_void, n_ctx: c_int) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        Ok(if n_ctx > 0 { (n_ctx as u32).min(rt.n_ctx) } else { rt.n_ctx } as c_int)
    })
}

/// Enables the sequence leaves of `n` rows of `(layer, kind, slot, heads, head_dim)`; `n` = 0
/// disables them.
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_seq_configure(
    model: *mut c_void,
    layers: *const u32,
    n: u32,
    n_attn: u32,
    n_recur: u32,
    heads: u32,
    interval: u32,
) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        let rows: Vec<[u32; 5]> = slice(layers, n as usize * 5)?.chunks_exact(5).map(|r| [r[0], r[1], r[2], r[3], r[4]]).collect();
        rt.trace().configure_sequence(&rt.engine, &rows, n_attn, n_recur, heads, interval)?;
        Ok(0)
    })
}

/// Sequence leaf hashes of the last traced evaluation (`n_leaves × 32` bytes).
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_seq_roots(model: *mut c_void, out: *mut u8, out_cap: usize, n_leaves: *mut u32) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if out.is_null() || n_leaves.is_null() {
            return Err(bad("null sequence root buffer"));
        }
        let state = rt.trace();
        let roots = state.sequence_roots().ok_or_else(|| bad("the last evaluation committed no sequence leaves"))?;
        copy_out(roots.as_flattened(), out, out_cap)?;
        *n_leaves = roots.len() as u32;
        Ok(0)
    })
}

/// Replays `tokens` to the end and copies bytes `[lo[k], hi[k])` of sequence leaf `stream[k]` end
/// to end into `bytes` (`lens[k]` each), with that leaf's chunk chaining values into `cvs`
/// (`n_cvs[k]` each).
#[no_mangle]
pub unsafe extern "C" fn keryx_llama_seq_replay(
    model: *mut c_void,
    tokens: *const i32,
    n_tokens: c_int,
    n_prompt: c_int,
    stream: *const u32,
    lo: *const u64,
    hi: *const u64,
    n: u32,
    max_chunks: u32,
    bytes: *mut u8,
    bytes_cap: usize,
    lens: *mut u64,
    cvs: *mut u8,
    cvs_cap: usize,
    n_cvs: *mut u32,
) -> c_int {
    traced(|| {
        let rt = trace_runtime(model)?;
        if n_prompt <= 0 || n_tokens < n_prompt || n == 0 || bytes.is_null() || cvs.is_null() || lens.is_null() || n_cvs.is_null() {
            return Err(bad("invalid sequence replay arguments"));
        }
        let n = n as usize;
        let (stream, lo, hi) = (slice(stream, n)?, slice(lo, n)?, slice(hi, n)?);
        let spans: Vec<(u32, u64, u64)> = (0..n).map(|k| (stream[k], lo[k], hi[k])).collect();
        let got = rt.trace().replay_sequence(&rt.engine, rt.n_ctx, slice(tokens, n_tokens as usize)?, n_prompt as u32, &spans)?;
        let (mut b, mut c) = (0usize, 0usize);
        for (k, (span_bytes, chunk_cvs)) in got.iter().enumerate() {
            if chunk_cvs.len() > max_chunks as usize {
                return Err(TraceError { code: -6, message: format!("a leaf of {} chunks for {max_chunks}", chunk_cvs.len()) });
            }
            copy_out(span_bytes, bytes.add(b), bytes_cap.saturating_sub(b))?;
            copy_out(chunk_cvs.as_flattened(), cvs.add(c), cvs_cap.saturating_sub(c))?;
            *lens.add(k) = span_bytes.len() as u64;
            *n_cvs.add(k) = chunk_cvs.len() as u32;
            b += span_bytes.len();
            c += chunk_cvs.len() * 32;
        }
        Ok(0)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_selection_rejects_implicit_identity_without_opening_vulkan() {
        assert!(device::select("0").is_err());
        assert!(device::select("AMD Radeon").is_err());
        let id = CString::new("0").unwrap();
        let mut uuid = [0i8; 64];
        unsafe {
            assert_eq!(keryx_llama_vk_select_device(std::ptr::null(), uuid.as_mut_ptr(), 64), -1);
            assert_eq!(keryx_llama_vk_select_device(id.as_ptr(), std::ptr::null_mut(), 64), -1);
            assert_eq!(keryx_llama_vk_select_device(id.as_ptr(), uuid.as_mut_ptr(), 1), -1);
        }
    }

    #[test]
    fn offsets_stay_inside_the_batch() {
        assert_eq!(batch_offset(None, 10, 4), Ok(-1));
        assert_eq!(batch_offset(Some(10), 10, 4), Ok(0));
        assert_eq!(batch_offset(Some(13), 10, 4), Ok(3));
        assert!(batch_offset(Some(14), 10, 4).is_err());
        assert!(batch_offset(Some(9), 10, 4).is_err());
        assert_eq!(batch_offset(Some(1), u64::MAX, 4), Ok(2));
    }

    #[test]
    fn job_absorbs_seed_words_and_timestamp_into_the_h10_sponge() {
        let (p, s, t) = ([1u64, 2, 3, 4], [5u64, 6, 7, 8], [0x11u64, 0x22, 0x33, u64::MAX]);
        let job = unsafe { job(p.as_ptr(), s.as_ptr(), t.as_ptr(), 99) }.unwrap();
        let h10 = job.h10_state.unwrap();
        for i in 0..4 {
            assert_eq!(h10[i], POW_HASH_INITIAL_STATE[i] ^ s[i]);
        }
        assert_eq!(h10[4], POW_HASH_INITIAL_STATE[4] ^ 99);
        assert_eq!(h10[5..], POW_HASH_INITIAL_STATE[5..]);
        assert_eq!(job.pow_words, p);
        assert_eq!(job.target_le[0], 0x11);
        assert_eq!(job.target_le[24..], [0xFF; 8]);
    }

    #[test]
    fn errors_and_panics_become_the_fallback() {
        assert_eq!(guard(-3i64, || Err("boom".into())), -3);
        let message = unsafe { CStr::from_ptr(keryx_llama_last_error()) };
        assert_eq!(message.to_str(), Ok("boom"));
        assert_eq!(guard(-3i64, || panic!("walk")), -3);
        let message = unsafe { CStr::from_ptr(keryx_llama_last_error()) };
        assert_eq!(message.to_str(), Ok("panic: walk"));
    }

    #[test]
    fn null_handles_are_refused() {
        let w = [0u64; 4];
        let got = unsafe {
            keryx_llama_pom_mine_v5(std::ptr::null_mut(), w.as_ptr(), w.as_ptr(), w.as_ptr(), 0, 0, 8, w.as_ptr(), 1)
        };
        assert_eq!(got, -3);
        assert!(!unsafe { keryx_llama_pom_ready(std::ptr::null_mut()) });
        assert_eq!(unsafe { keryx_llama_pom_n_chunks(std::ptr::null_mut()) }, 0);
    }

    #[test]
    fn trace_exports_refuse_null_handles_with_the_abi_code() {
        let null = std::ptr::null_mut();
        let manifest = [0u8; 4];
        let mut n = 0u32;
        unsafe {
            assert_eq!((keryx_llama_abi(), keryx_llama_vk_abi()), (9, 9));
            assert_eq!(keryx_llama_trace_configure(null, manifest.as_ptr().cast(), manifest.len(), 0), -1);
            assert_eq!(keryx_llama_trace_leaf_count(null), -1);
            assert_eq!(keryx_llama_context_acquire(null, 512), -1);
            assert_eq!(keryx_llama_trace_tokens(null, [1i32].as_ptr(), 1), -1);
            assert_eq!(keryx_llama_trace_read(null, std::ptr::null_mut(), 0, std::ptr::null_mut(), 0, &mut n), -1);
            assert_eq!(keryx_llama_seq_configure(null, std::ptr::null(), 0, 0, 0, 0, 0), -1);
            assert_eq!(keryx_llama_seq_roots(null, std::ptr::null_mut(), 0, &mut n), -1);
        }
        let message = unsafe { CStr::from_ptr(keryx_llama_last_error()) };
        assert_eq!(message.to_str(), Ok("null model handle"));
    }

    #[test]
    fn trace_failures_and_panics_map_to_abi_codes() {
        assert_eq!(traced(|| Err(TraceError { code: -8, message: "head".into() })), -8);
        assert_eq!(traced(|| panic!("replay")), -5);
        let message = unsafe { CStr::from_ptr(keryx_llama_last_error()) };
        assert_eq!(message.to_str(), Ok("panic: replay"));
    }

    #[test]
    fn copies_out_only_into_a_buffer_that_fits() {
        let mut dst = [0u8; 4];
        unsafe {
            assert!(copy_out(&[1u8, 2, 3], dst.as_mut_ptr(), dst.len()).is_ok());
            assert_eq!(copy_out(&[1u8; 5], dst.as_mut_ptr(), dst.len()).err().map(|e| e.code), Some(-6));
            assert_eq!(copy_out(&[1u8], std::ptr::null_mut(), 8).err().map(|e| e.code), Some(-6));
            assert!(copy_out::<u8>(&[], std::ptr::null_mut(), 0).is_ok());
            assert!(slice::<u32>(std::ptr::null(), 0).is_ok());
            assert_eq!(slice::<u32>(std::ptr::null(), 1).err().map(|e| e.code), Some(-1));
        }
        assert_eq!(dst, [1, 2, 3, 0]);
    }
}
