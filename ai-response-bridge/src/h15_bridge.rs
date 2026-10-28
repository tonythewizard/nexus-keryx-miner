use crate::{
    h15_producer as prod, trace_audit_store::wire::ResponseTrace, trace_producer as p,
    trace_sequence as s,
};
use borsh::{BorshDeserialize, BorshSerialize};
use keryx_verifiable_inference::{merkle::Hash, shape};
use std::{
    ffi::c_void,
    panic::{catch_unwind, AssertUnwindSafe},
    slice,
};
const CAP: usize = 64 << 20;
const CAPTURE_CAP: usize = 512 << 20;
#[repr(C)]
pub struct NativeCapture {
    pub tokens: *const i32,
    pub ubatch: *const u32,
    pub leaves: *const u8,
    pub sequence: *const u8,
    pub positions: usize,
    pub leaves_per_position: usize,
    pub sequence_count: usize,
    pub prompt_positions: u32,
}
#[repr(C)]
pub struct Buffer {
    pub data: *mut u8,
    pub length: usize,
}
#[repr(C)]
pub struct Api {
    pub user: *mut c_void,
    pub weight: unsafe extern "C" fn(*mut c_void, u64, *mut u8, *mut u8, *mut usize) -> i32,
    pub replay: unsafe extern "C" fn(
        *mut c_void,
        *const NativeCapture,
        u32,
        *const u64,
        usize,
        u32,
        u32,
        *mut u8,
        usize,
        *mut usize,
    ) -> i32,
    pub model_root: Hash,
    pub chunks: u64,
}
#[derive(BorshSerialize, BorshDeserialize)]
pub struct Capture {
    pub tier: u8,
    pub request: Hash,
    pub max_tokens: u32,
    pub gen_secs: f64,
    pub trace: ResponseTrace,
    pub plaintext: Vec<u8>,
}
pub(crate) unsafe fn view<'a, T>(ptr: *const T, n: usize, cap: usize) -> Result<&'a [T], String> {
    if n > cap
        || n > isize::MAX as usize / std::mem::size_of::<T>().max(1)
        || (n > 0 && (ptr.is_null() || (ptr as usize) % std::mem::align_of::<T>() != 0))
    {
        return Err("invalid native span".into());
    }
    if n == 0 {
        Ok(&[])
    } else {
        Ok(unsafe { slice::from_raw_parts(ptr, n) })
    }
}
thread_local! {static ERROR:std::cell::RefCell<std::ffi::CString>=std::cell::RefCell::new(std::ffi::CString::new("no error").unwrap());}
#[no_mangle]
pub extern "C" fn nexus_h15_error() -> *const std::ffi::c_char {
    ERROR.with(|e| e.borrow().as_ptr())
}
pub(crate) fn guard(f: impl FnOnce() -> Result<(), String>) -> i32 {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => 0,
        result => {
            let error = match result {
                Ok(Err(e)) => e,
                _ => "native audit panic contained".into(),
            };
            ERROR.with(|e| {
                *e.borrow_mut() = std::ffi::CString::new(error.replace('\0', " ")).unwrap()
            });
            -1
        }
    }
}
pub(crate) unsafe fn output(bytes: Vec<u8>, out: *mut Buffer) -> Result<(), String> {
    if out.is_null() || bytes.len() > CAP {
        return Err("invalid output budget".into());
    }
    let b = bytes.into_boxed_slice();
    let length = b.len();
    let data = Box::into_raw(b) as *mut u8;
    unsafe { *out = Buffer { data, length } };
    Ok(())
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// The pointer AND length must be the unchanged pair returned by this bridge. Transfer that
/// allocation exactly once; no other allocator or caller may free it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_buffer_free(b: Buffer) {
    if !b.data.is_null() {
        unsafe {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                b.data, b.length,
            )))
        }
    }
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// A non-null pointer must own an unreleased capture returned by this bridge, with no concurrent
/// calls or outstanding borrows.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_capture_free(p: *mut c_void) {
    if !p.is_null() {
        unsafe { drop(Box::from_raw(p as *mut Capture)) }
    }
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `request` spans 32 readable bytes. `n` must initially contain the writable i32-element
/// capacity of `out`; `n` and `head` must be writable and aligned.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_prefix(
    tier: u8,
    request: *const u8,
    out: *mut i32,
    n: *mut usize,
    head: *mut u32,
) -> i32 {
    guard(|| {
        if n.is_null() || head.is_null() {
            return Err("missing prefix dimensions".into());
        }
        let request = unsafe { view(request, 32, 32) }?.try_into().unwrap();
        let ctx = p::chain_ctx(tier).ok_or("unsupported tier")?.1;
        let tokens = p::prefix_tokens(&ctx, request);
        if unsafe { *n } < tokens.len() || out.is_null() {
            return Err("prefix capacity".into());
        }
        unsafe {
            std::ptr::copy_nonoverlapping(tokens.as_ptr(), out, tokens.len());
            *n = tokens.len();
            *head = ctx.offset
        };
        Ok(())
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `raw` and its nested arrays must remain readable with their declared element counts. `request`
/// spans 32 bytes; `commit` spans 73 writable bytes and `out` is a writable handle slot.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_capture(
    raw: *const NativeCapture,
    tier: u8,
    request: *const u8,
    max_tokens: u32,
    prompt_len: u32,
    text: *const u8,
    text_len: usize,
    gen_secs: f64,
    out: *mut *mut c_void,
    commit: *mut u8,
) -> i32 {
    guard(|| {
        if raw.is_null()
            || out.is_null()
            || commit.is_null()
            || !gen_secs.is_finite()
            || gen_secs <= 0.0
        {
            return Err("invalid capture".into());
        }
        let raw = unsafe { &*raw };
        let m = p::TierManifest::for_attn(tier).ok_or("unsupported tier")?;
        if raw.positions > 32768
            || raw.leaves_per_position != m.n_leaves()
            || raw
                .positions
                .checked_mul(m.n_leaves())
                .is_none_or(|n| n > CAPTURE_CAP / 32)
        {
            return Err("capture memory budget".into());
        }
        let tokens = unsafe { view(raw.tokens, raw.positions, 32768) }?.to_vec();
        let ubatch = unsafe { view(raw.ubatch, raw.positions, 32768) }?.to_vec();
        let leaves = unsafe {
            view(
                raw.leaves.cast::<Hash>(),
                raw.positions * m.n_leaves(),
                CAPTURE_CAP / 32,
            )
        }?
        .to_vec();
        let request: Hash = unsafe { view(request, 32, 32) }?.try_into().unwrap();
        let text = std::str::from_utf8(unsafe { view(text, text_len, 64 << 10) }?)
            .map_err(|_| "invalid UTF8")?;
        if !shape::shape_ok(
            prompt_len,
            text_len as u32,
            max_tokens,
            raw.prompt_positions,
            raw.positions as u32,
        ) {
            return Err("response shape".into());
        }
        let ctx = p::chain_ctx(tier).ok_or("missing prefix context")?.1;
        let expected = p::prefix_tokens(&ctx, &request);
        if tokens.get(ctx.offset as usize..ctx.offset as usize + expected.len())
            != Some(expected.as_slice())
        {
            return Err("request prefix mismatch".into());
        }
        let rec = p::TraceRecord::new_chain(&m, raw.prompt_positions, ubatch, leaves, &tokens)
            .ok_or("incomplete positional trace")?;
        let c = m.chain.unwrap();
        let expected_seq = 2 * c.seq_heads as usize * (c.n_attn + c.n_recur) as usize
            + c.seq_heads as usize * c.n_attn as usize;
        if raw.sequence_count != expected_seq {
            return Err("incomplete sequence trace".into());
        }
        let seq = p::SeqTrace::new(
            unsafe { view(raw.sequence.cast::<Hash>(), raw.sequence_count, 8192) }?.to_vec(),
        )
        .ok_or("sequence roots")?;
        let plain = p::ResponseTrace {
            record: rec,
            tokens,
        };
        let commitment = plain
            .commitment_with_sequence(&seq)
            .ok_or("commitment failure")?;
        let plaintext = plain
            .sealed_plaintext(text)
            .ok_or("sealed token mismatch")?;
        let capture = Capture {
            tier,
            request,
            max_tokens,
            gen_secs,
            trace: ResponseTrace {
                record: plain.record,
                tokens: plain.tokens,
                seq: Some(seq),
            },
            plaintext,
        };
        let wire = commitment.to_bytes();
        if wire.len() != 73 {
            return Err("commitment shape".into());
        }
        unsafe {
            std::ptr::copy_nonoverlapping(wire.as_ptr(), commit, 73);
            *out = Box::into_raw(Box::new(capture)).cast()
        };
        Ok(())
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `c` must be a live capture; `out` must be a writable Buffer slot. Free any previously owned
/// output before replacing it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_plaintext(c: *mut c_void, out: *mut Buffer) -> i32 {
    guard(|| {
        if c.is_null() {
            return Err("missing capture".into());
        }
        unsafe { output((&*(c as *const Capture)).plaintext.clone(), out) }
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `c` must be a live capture; `out` must be a writable Buffer slot. Free any previously owned
/// output before replacing it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_encode_capture(c: *mut c_void, out: *mut Buffer) -> i32 {
    guard(|| {
        if c.is_null() {
            return Err("missing capture".into());
        }
        unsafe {
            output(
                borsh::to_vec(&*(c as *const Capture)).map_err(|_| "capture serialization")?,
                out,
            )
        }
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `data` must span `len` readable bytes and `out` must be a writable handle slot. A successful
/// result transfers a new capture to the caller.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_decode_capture(
    data: *const u8,
    len: usize,
    out: *mut *mut c_void,
) -> i32 {
    guard(|| {
        if out.is_null() {
            return Err("missing output".into());
        }
        let c = Capture::try_from_slice(unsafe { view(data, len, CAP) }?)
            .map_err(|_| "corrupt capture")?;
        if !c.trace.record.storage_shape_valid()
            || c.trace.seq.is_none()
            || c.tier > 4
            || c.max_tokens == 0
            || !c.gen_secs.is_finite()
            || c.gen_secs <= 0.0
        {
            return Err("invalid stored capture".into());
        }
        unsafe { *out = Box::into_raw(Box::new(c)).cast() };
        Ok(())
    })
}
struct Native<'a> {
    api: &'a Api,
}
impl p::WeightIndex for Native<'_> {
    fn root(&self) -> Hash {
        self.api.model_root
    }
    fn n_chunks(&self) -> u64 {
        self.api.chunks
    }
    fn read_chunk_bytes(&self, i: u64) -> Option<Hash> {
        let mut b = [0; 32];
        let mut n = 0;
        let rc = unsafe {
            (self.api.weight)(
                self.api.user,
                i,
                b.as_mut_ptr(),
                std::ptr::null_mut(),
                &mut n,
            )
        };
        (rc == 0).then_some(b)
    }
    fn merkle_path(&self, i: u64) -> Option<Vec<Hash>> {
        let mut b = [0; 32];
        let mut path = vec![[0; 32]; 64];
        let mut n = 64;
        let rc = unsafe {
            (self.api.weight)(
                self.api.user,
                i,
                b.as_mut_ptr(),
                path.as_mut_ptr().cast(),
                &mut n,
            )
        };
        if rc != 0 || n > 64 {
            None
        } else {
            path.truncate(n);
            Some(path)
        }
    }
}
fn native_view(t: &ResponseTrace) -> NativeCapture {
    let seq = t.seq.as_ref().unwrap();
    NativeCapture {
        tokens: t.tokens.as_ptr(),
        ubatch: t.record.ubatch.as_ptr(),
        leaves: t.record.leaf_hashes().as_ptr().cast(),
        sequence: seq.leaves.as_ptr().cast(),
        positions: t.tokens.len(),
        leaves_per_position: t.record.leaf_hashes().len() / t.tokens.len(),
        sequence_count: seq.leaves.len(),
        prompt_positions: t.record.n_prompt,
    }
}
impl Native<'_> {
    fn replay(
        &self,
        t: &ResponseTrace,
        kind: u32,
        plan: &[u64],
        count: usize,
        slot: u32,
        chunks: u32,
    ) -> Result<Vec<u8>, String> {
        let mut data = vec![0u8; CAP];
        let mut n = 0;
        let rc = unsafe {
            (self.api.replay)(
                self.api.user,
                &native_view(t),
                kind,
                plan.as_ptr(),
                count,
                slot,
                chunks,
                data.as_mut_ptr(),
                data.len(),
                &mut n,
            )
        };
        if rc != 0 || n > CAP {
            return Err("native replay failed".into());
        }
        data.truncate(n);
        Ok(data)
    }
}
fn take<'a>(b: &mut &'a [u8], n: usize) -> Result<&'a [u8], String> {
    if n > b.len() {
        return Err("truncated replay".into());
    }
    let (x, rest) = b.split_at(n);
    *b = rest;
    Ok(x)
}
impl prod::Replay for Native<'_> {
    fn captures(
        &mut self,
        t: &ResponseTrace,
        points: &[(u32, u32)],
        slot: u32,
    ) -> Result<p::Captures, String> {
        let plan: Vec<u64> = points
            .iter()
            .flat_map(|&(a, b)| [a as u64, b as u64])
            .collect();
        let bytes = self.replay(t, 0, &plan, points.len(), slot, 0)?;
        let mut b = bytes.as_slice();
        let mut caps = p::Captures::new();
        for &(pos, leaf) in points {
            let n = u32::from_le_bytes(take(&mut b, 4)?.try_into().unwrap()) as usize;
            if n == 0 || n > slot as usize {
                return Err("capture slot overflow".into());
            }
            caps.insert((pos, leaf), take(&mut b, n)?.to_vec());
        }
        if !b.is_empty() {
            return Err("trailing replay bytes".into());
        }
        Ok(caps)
    }
    fn spans(
        &mut self,
        t: &ResponseTrace,
        plans: &[s::SpanPlan],
        chunks: u32,
    ) -> Result<Vec<s::SeqCapture>, String> {
        let plan: Vec<u64> = plans
            .iter()
            .flat_map(|p| [p.leaf as u64, p.lo, p.hi])
            .collect();
        let bytes = self.replay(t, 1, &plan, plans.len(), 0, chunks)?;
        let mut b = bytes.as_slice();
        let mut out = Vec::new();
        for p in plans {
            let n = u64::from_le_bytes(take(&mut b, 8)?.try_into().unwrap());
            if n != p.hi - p.lo {
                return Err("span size mismatch".into());
            }
            let bytes = take(&mut b, n as usize)?.to_vec();
            let n = u32::from_le_bytes(take(&mut b, 4)?.try_into().unwrap()) as usize;
            if n > chunks as usize {
                return Err("span CV overflow".into());
            }
            let cvs = take(&mut b, n * 32)?
                .chunks_exact(32)
                .map(|c| c.try_into().unwrap())
                .collect();
            out.push(s::SeqCapture { bytes, cvs });
        }
        if !b.is_empty() {
            return Err("trailing sequence bytes".into());
        }
        Ok(out)
    }
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `c` must be a live capture. The API and callbacks must remain valid, cannot unwind, and must
/// hold the authorized exclusive GPU ownership for replay. Block/response hashes span 32 bytes;
/// path and prior-proof spans obey their supplied counts.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_open(
    c: *mut c_void,
    api: *const Api,
    block: *const u8,
    response: *const u8,
    path: *const u64,
    npath: usize,
    prev: *const u8,
    nprev: usize,
    out: *mut Buffer,
) -> i32 {
    guard(|| {
        if c.is_null() || api.is_null() {
            return Err("missing replay context".into());
        }
        let c = unsafe { &*(c as *const Capture) };
        let api = unsafe { &*api };
        let block: Hash = unsafe { view(block, 32, 32) }?.try_into().unwrap();
        let response: Hash = unsafe { view(response, 32, 32) }?.try_into().unwrap();
        let path = unsafe { view(path, npath, 16) }?;
        let prev = unsafe { view(prev, nprev, 112 << 10) }?;
        let bodies = build(c, api, block, response, path, prev)?;
        unsafe { output(s::bundle(&bodies), out) }
    })
}

pub(crate) fn build(
    c: &Capture,
    api: &Api,
    block: Hash,
    response: Hash,
    path: &[u64],
    prev: &[u8],
) -> Result<Vec<Vec<u8>>, String> {
    let seed = p::trace_seed(&block, &response);
    let index = Native { api };
    let mut runtime = Native { api };
    let bodies = if prev.is_empty() {
        let (chain, residual) = prod::chain_pieces(
            c.tier,
            &c.trace,
            &seed,
            &c.request,
            response,
            &index,
            &mut runtime,
        )?;
        let mut v = vec![
            chain,
            prod::attention_piece(
                c.tier,
                &c.trace,
                &seed,
                response,
                &index,
                &mut runtime,
                &[],
                None,
            )?,
        ];
        if let Some(b) =
            prod::recurrent_piece(c.tier, &c.trace, &seed, response, &index, &mut runtime)?
        {
            v.push(b);
        }
        v.push(residual);
        v
    } else {
        vec![prod::attention_piece(
            c.tier,
            &c.trace,
            &seed,
            response,
            &index,
            &mut runtime,
            path,
            Some(prev),
        )?]
    };
    Ok(bodies)
}

/// Offline parity oracle: the native callback must be exclusively admitted by
/// the caller. This API never constructs a transaction or touches a journal.
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live capture. The API callbacks must remain valid, cannot unwind, and require
/// exclusive GPU ownership. `stats` must cover TWO writable u32 values.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_selftest(
    ptr: *mut c_void,
    api: *const Api,
    seeds: u32,
    stats: *mut u32,
) -> i32 {
    guard(|| {
        if ptr.is_null() || api.is_null() || stats.is_null() || seeds == 0 || seeds > 16 {
            return Err("self-test bounds".into());
        }
        let c = unsafe { &*(ptr as *const Capture) };
        let api = unsafe { &*api };
        let response = [3u8; 32];
        let mut pieces = 0;
        let mut phases = 0;
        for counter in 0..seeds {
            let block = *blake3::hash(&counter.to_le_bytes()).as_bytes();
            let bodies = build(c, api, block, response, &[], &[])?;
            pieces += bodies.len() as u32;
            let mut op = keryx_verifiable_inference::opening::AttnOpening::decode(&bodies[1])
                .ok_or("self-test attention body")?;
            let seed = p::trace_seed(&block, &response);
            let m = p::TierManifest::for_attn(c.tier).unwrap();
            let d = s::draw_attn(&m, &c.trace.record, &seed).ok_or("self-test draw")?;
            for depth in 0..8 {
                if op.partials.is_empty() {
                    break;
                }
                if depth == 7 {
                    return Err("self-test attention phase bound".into());
                }
                let parts =
                    keryx_verifiable_inference::attn_blocks::decode_partials(&op.partials, d.dv)
                        .ok_or("self-test partials")?;
                let weights = keryx_verifiable_inference::attn_blocks::child_weights(&parts)
                    .ok_or("self-test weights")?;
                let next =
                    p::draw_attention_child(&block, &response, op.path.len() as u8, &weights)
                        .ok_or("self-test child")?;
                let mut path = op.path.clone();
                path.push(next);
                let bodies = build(c, api, block, response, &path, &op.partials)?;
                pieces += 1;
                phases += 1;
                op = keryx_verifiable_inference::opening::AttnOpening::decode(&bodies[0])
                    .ok_or("self-test phase")?;
            }
        }
        unsafe {
            *stats = pieces;
            *stats.add(1) = phases
        };
        Ok(())
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[cfg(target_pointer_width = "64")]
    fn native_abi_layout() {
        assert_eq!(std::mem::size_of::<NativeCapture>(), 64);
        assert_eq!(std::mem::offset_of!(NativeCapture, prompt_positions), 56);
        assert_eq!(std::mem::size_of::<Api>(), 64);
        assert_eq!(std::mem::size_of::<Buffer>(), 16);
    }
    #[test]
    fn prefix_capacity_failure_preserves_output() {
        let request = [2u8; 32];
        let mut tokens = [-17i32; 256];
        let mut n = 0;
        let mut head = 99;
        assert_eq!(
            unsafe {
                nexus_h15_prefix(0, request.as_ptr(), tokens.as_mut_ptr(), &mut n, &mut head)
            },
            -1
        );
        assert_eq!(tokens, [-17; 256]);
        assert_eq!(head, 99);
        n = 256;
        assert_eq!(
            unsafe {
                nexus_h15_prefix(0, request.as_ptr(), tokens.as_mut_ptr(), &mut n, &mut head)
            },
            0
        );
        assert!(n > 0 && n < 256);
        assert!(tokens[n..].iter().all(|&t| t == -17));
        assert_eq!(
            unsafe {
                nexus_h15_prefix(0, std::ptr::null(), tokens.as_mut_ptr(), &mut n, &mut head)
            },
            -1
        );
    }
    #[test]
    fn capture_overflow_is_rejected_before_borrowing() {
        let raw = NativeCapture {
            tokens: std::ptr::null(),
            ubatch: std::ptr::null(),
            leaves: std::ptr::null(),
            sequence: std::ptr::null(),
            positions: usize::MAX,
            leaves_per_position: 1,
            sequence_count: 0,
            prompt_positions: 0,
        };
        let mut out = std::ptr::null_mut();
        let mut commit = [0x77; 73];
        assert_eq!(
            unsafe {
                nexus_h15_capture(
                    &raw,
                    0,
                    [1u8; 32].as_ptr(),
                    32,
                    1,
                    std::ptr::null(),
                    0,
                    1.0,
                    &mut out,
                    commit.as_mut_ptr(),
                )
            },
            -1
        );
        assert!(out.is_null());
        assert_eq!(commit, [0x77; 73]);
        assert!(unsafe { view::<u64>(std::ptr::null(), 1, 1) }.is_err());
        assert!(unsafe { view::<u64>(std::ptr::null(), usize::MAX, usize::MAX) }.is_err());
    }
}
