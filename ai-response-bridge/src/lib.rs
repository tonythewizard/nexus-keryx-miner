#![deny(unsafe_op_in_unsafe_fn)]

use blake2b_simd::Params as Blake2bParams;
use keryx_inference::{
    escrow_pubkey_of, open_request, open_response, seal_response, AiRequestPayload,
    AiResponder, AiResponsePayload,
};
use secp256k1::{schnorr::Signature, Keypair, Message, Secp256k1, SecretKey, XOnlyPublicKey};
use std::slice;
pub mod verified_inference;
pub mod trace_producer;
pub mod trace_store;
pub mod trace_recovery;
#[cfg(test)]
mod pom_v5_reference;
#[cfg(test)]
pub(crate) use pom_v5_reference::{pom,pom_v3,pom_v4};
// Expose the exact upstream synthetic stage-2 fixture only to CPU tests.
#[cfg(test)]
pub use keryx_verifiable_inference::chainkit as stock_stage2_fixture;
#[cfg(test)]
pub use trace_producer as trace;
#[cfg(test)]
#[path = "../../vendor/keryx-trace-abi6/src/trace_store.rs"]
pub mod stock_trace_store_fixture;

pub mod proto {
    include!(concat!(env!("OUT_DIR"), "/protowire.rs"));
}

#[path = "../../vendor/keryx-miner-v057/src/escrow.rs"]
pub mod stock_escrow;

use prost::Message as ProstMessage;
use stock_escrow::{EscrowWatcher, SubmitResponseOutcome};
use std::ffi::c_void;
use std::path::PathBuf;
use std::panic::{catch_unwind, AssertUnwindSafe};

struct EscrowBridge {
    watcher: EscrowWatcher,
    validation_hashes: Option<Vec<String>>,
    block_processed: bool,
    pending_block_wire: Vec<u8>,
    pending_claim_wire: Option<Vec<u8>>,
}

fn input<'a>(ptr: *const u8, len: usize) -> Result<&'a [u8], ()> {
    if len != 0 && ptr.is_null() { return Err(()); }
    Ok(if len == 0 { &[] } else { unsafe { slice::from_raw_parts(ptr, len) } })
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_watcher_new(
    secret_hex: *const u8, secret_len: usize,
    payout: *const u8, payout_len: usize,
    state_path: *const u8, state_path_len: usize,
) -> *mut c_void {
    catch_unwind(AssertUnwindSafe(|| {
        let secret = std::str::from_utf8(input(secret_hex, secret_len).ok()?).ok()?;
        let payout = std::str::from_utf8(input(payout, payout_len).ok()?).ok()?;
        let path = std::str::from_utf8(input(state_path, state_path_len).ok()?).ok()?;
        let full = PathBuf::from(path);
        if !full.is_absolute() || full.starts_with("/tmp") { return None; }
        let mut watcher = EscrowWatcher::new(secret, payout, full).ok()?;
        watcher.set_claims_enabled(false);
        Some(Box::into_raw(Box::new(EscrowBridge { watcher, validation_hashes: None, block_processed: false,
                                                   pending_block_wire: Vec::new(),
                                                   pending_claim_wire: None })) as *mut c_void)
    })).ok().flatten().unwrap_or(std::ptr::null_mut())
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_set_claims_enabled(handle: *mut c_void, enabled: i32) -> i32 {
    if handle.is_null() || (enabled != 0 && enabled != 1) { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        (*handle.cast::<EscrowBridge>()).watcher.set_claims_enabled(enabled != 0);
        0
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_watcher_free(handle: *mut c_void) {
    if !handle.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| unsafe { drop(Box::from_raw(handle.cast::<EscrowBridge>())) }));
    }
}

// Bounded protobuf transaction envelope carries pending outpoints; no signatures or secrets.
#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_claim_outpoints(handle: *mut c_void, daa: u64, out: *mut u8, cap: usize, len: *mut usize) -> i32 {
    if handle.is_null() || len.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let ops = (*handle.cast::<EscrowBridge>()).watcher.claim_outpoints(daa);
        if ops.len() > 10000 { return -4; }
        let tx = proto::RpcTransaction { inputs: ops.into_iter().map(|op| proto::RpcTransactionInput { previous_outpoint: Some(op), ..Default::default() }).collect(), ..Default::default() };
        let bytes = tx.encode_to_vec(); *len = bytes.len();
        if cap < bytes.len() || (!bytes.is_empty() && out.is_null()) { return 1; }
        if !bytes.is_empty() { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len()); } 0
    })).unwrap_or(-9)
}
#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_verified_claim(handle: *mut c_void, block: *const u8, block_len: usize, live: *const u8, live_len: usize, out: *mut u8, cap: usize, len: *mut usize) -> i32 {
    if handle.is_null() || len.is_null() || out.is_null() || cap < 65536 { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let b = match input(block, block_len).and_then(|v| proto::RpcBlock::decode(v).map_err(|_| ())) { Ok(v) => v, Err(_) => return -2 };
        let u = match input(live, live_len).and_then(|v| proto::GetUtxosByAddressesResponseMessage::decode(v).map_err(|_| ())) { Ok(v) => v, Err(_) => return -3 };
        let bytes = (*handle.cast::<EscrowBridge>()).watcher.verified_claim(&b, &u).map(|t| t.encode_to_vec()).unwrap_or_default();
        *len = bytes.len(); if bytes.len() > cap { return -4; }
        if !bytes.is_empty() { std::ptr::copy_nonoverlapping(bytes.as_ptr(), out, bytes.len()); } 0
    })).unwrap_or(-9)
}

// Output is `count` contiguous 32-byte decoded block hashes. A too-small buffer
// returns 1 and preserves the pending list so the caller can allocate and retry.
#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_start_validation(
    handle: *mut c_void, out: *mut u8, capacity: usize, count: *mut usize,
) -> i32 {
    if handle.is_null() || count.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let bridge = &mut *handle.cast::<EscrowBridge>();
        if bridge.validation_hashes.is_none() {
            bridge.validation_hashes = Some(bridge.watcher.start_state_validation());
        }
        let hashes = bridge.validation_hashes.as_ref().unwrap();
        *count = hashes.len();
        let required_bytes = hashes.len().saturating_mul(32);
        if capacity < required_bytes || (required_bytes != 0 && out.is_null()) { return 1; }
        let target = if hashes.is_empty() { &mut [][..] } else {
            slice::from_raw_parts_mut(out, hashes.len().saturating_mul(32))
        };
        for (i, hash) in hashes.iter().enumerate() {
            let decoded = match hex::decode(hash) { Ok(v) if v.len() == 32 => v, _ => return -2 };
            target[i * 32..(i + 1) * 32].copy_from_slice(&decoded);
        }
        bridge.validation_hashes = None;
        0
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_validation_result(
    handle: *mut c_void, hash: *const u8, is_chain_block: i32,
) -> i32 {
    if handle.is_null() || hash.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let encoded = hex::encode(slice::from_raw_parts(hash, 32));
        (*handle.cast::<EscrowBridge>()).watcher.consume_validation_ok(&encoded, is_chain_block != 0) as i32
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_validation_error(
    handle: *mut c_void, message: *const u8, message_len: usize,
) -> i32 {
    if handle.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let Ok(message) = std::str::from_utf8(input(message, message_len).unwrap_or(&[])) else { return -2; };
        (*handle.cast::<EscrowBridge>()).watcher.on_block_validation_error(message) as i32
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_chain_block_removed(
    handle: *mut c_void, hash: *const u8,
) -> i32 {
    if handle.is_null() || hash.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let encoded = hex::encode(slice::from_raw_parts(hash, 32));
        (*handle.cast::<EscrowBridge>()).watcher.on_chain_blocks_removed(&[encoded]); 0
    })).unwrap_or(-9)
}

// The RPC block is protobuf-wire compatible with the exact stock proto source.
// The stock watcher only creates spendable coinbase outpoints when verboseData
// says isChainBlock; it still consumes red-set and reorg information from blocks.
#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_handle_block(
    handle: *mut c_void, block_wire: *const u8, block_len: usize,
    out: *mut u8, capacity: usize, out_len: *mut usize,
    pending_count: *mut u64, pending_amount: *mut u64,
) -> i32 {
    if handle.is_null() || out_len.is_null() || pending_count.is_null() || pending_amount.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let wire = match input(block_wire, block_len) { Ok(v) => v, Err(_) => return -2 };
        let bridge = &mut *handle.cast::<EscrowBridge>();
        if bridge.block_processed {
            if bridge.pending_block_wire.as_slice() != wire { return -4; }
        } else {
            let block = match proto::RpcBlock::decode(wire) { Ok(v) => v, Err(_) => return -3 };
            bridge.pending_claim_wire = bridge.watcher.handle_block(&block).map(|tx| tx.encode_to_vec());
            bridge.pending_block_wire.clear();
            bridge.pending_block_wire.extend_from_slice(wire);
            bridge.block_processed = true;
        }
        let (count, amount) = bridge.watcher.pending_escrow();
        *pending_count = count; *pending_amount = amount;
        let claim = bridge.pending_claim_wire.as_deref().unwrap_or(&[]);
        *out_len = claim.len();
        if capacity < claim.len() || (!claim.is_empty() && out.is_null()) { return 1; }
        if !claim.is_empty() { std::ptr::copy_nonoverlapping(claim.as_ptr(), out, claim.len()); }
        bridge.pending_claim_wire = None;
        bridge.block_processed = false;
        bridge.pending_block_wire.clear();
        0
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_track_inference_outpoint(
    handle: *mut c_void, txid: *const u8, txid_len: usize, daa: u64, amount: u64,
) -> i32 {
    if handle.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let Ok(txid) = std::str::from_utf8(input(txid, txid_len).unwrap_or(&[])) else { return -2; };
        if txid.len() != 64 || !txid.bytes().all(|b| b.is_ascii_hexdigit()) { return -3; }
        (*handle.cast::<EscrowBridge>()).watcher.track_inference_escrow(txid.to_ascii_lowercase(), daa, amount); 0
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_submit_result(
    handle: *mut c_void, txid: *const u8, txid_len: usize,
    error: *const u8, error_len: usize,
) -> i32 {
    if handle.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe {
        let Ok(txid) = std::str::from_utf8(input(txid, txid_len).unwrap_or(&[])) else { return -2; };
        let error_bytes = input(error, error_len).unwrap_or(&[]);
        let error = if error_bytes.is_empty() { None } else { match std::str::from_utf8(error_bytes) { Ok(v) => Some(v), Err(_) => return -3 } };
        match (*handle.cast::<EscrowBridge>()).watcher.on_submit_response(txid, error) {
            SubmitResponseOutcome::NotOurs => 0,
            SubmitResponseOutcome::Handled => 1,
            SubmitResponseOutcome::Accepted { .. } => 2,
        }
    })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_validation_in_progress(handle: *mut c_void) -> i32 {
    if handle.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe { (*handle.cast::<EscrowBridge>()).watcher.validation_in_progress() as i32 })).unwrap_or(-9)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_escrow_flush(handle: *mut c_void) -> i32 {
    if handle.is_null() { return -1; }
    catch_unwind(AssertUnwindSafe(|| unsafe { (*handle.cast::<EscrowBridge>()).watcher.flush_state().map_or(-2, |_| 0) })).unwrap_or(-9)
}

const RESPONDER_DOMAIN: &[u8] = b"KeryxServiceResponderV1";
const ESCROW_DELEGATION_DOMAIN: &[u8] = b"KeryxEscrowDelegationV1";

fn sign_digest(secret: &[u8; 32], bytes: &[u8]) -> Result<([u8; 32], [u8; 64]), ()> {
    let sk = SecretKey::from_slice(secret).map_err(|_| ())?;
    let secp = Secp256k1::new();
    let keypair = Keypair::from_secret_key(&secp, &sk);
    let (xonly, _) = keypair.x_only_public_key();
    let pubkey = xonly.serialize();
    let mut h = Blake2bParams::new().hash_length(32).to_state();
    h.update(RESPONDER_DOMAIN);
    h.update(bytes);
    let digest = h.finalize();
    let msg = Message::from_digest_slice(digest.as_bytes()).map_err(|_| ())?;
    let sig = secp.sign_schnorr_no_aux_rand(&msg, &keypair);
    let mut signature = [0u8; 64];
    signature.copy_from_slice(sig.as_ref());
    Ok((pubkey, signature))
}

fn stock_payout_xonly(address: &[u8]) -> Result<[u8; 32], ()> {
    let text = std::str::from_utf8(address).map_err(|_| ())?;
    let (_, encoded) = text.split_once(':').ok_or(())?;
    if !text.starts_with("keryx:") || encoded.len() < 9 { return Err(()); }
    // Match the immutable stock v0.5.7 decoder: the last eight bech32 characters are
    // checksum and the preceding 5-bit groups encode version || 32-byte x-only key.
    let data = &encoded[..encoded.len() - 8];
    const CHARSET: &[u8] = b"qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    let mut bytes = Vec::with_capacity(data.len() * 5 / 8);
    let (mut buffer, mut bits) = (0u32, 0u32);
    for c in data.bytes() {
        let value = CHARSET.iter().position(|&x| x == c).ok_or(())? as u32;
        buffer = (buffer << 5) | value;
        bits += 5;
        if bits >= 8 { bits -= 8; bytes.push((buffer >> bits) as u8); }
    }
    if bytes.len() < 33 || bytes[0] != 0 { return Err(()); }
    let mut key = [0u8; 32]; key.copy_from_slice(&bytes[1..33]);
    Ok(key)
}

#[no_mangle]
pub unsafe extern "C" fn nexus_ai_verify_escrow_delegation(
    payout: *const u8, payout_len: usize, escrow_pubkey: *const u8, cert: *const u8,
) -> i32 {
    if escrow_pubkey.is_null() || cert.is_null() { return -1; }
    let Ok(address) = (unsafe { bytes(payout, payout_len) }) else { return -1; };
    let Ok(payout_key) = stock_payout_xonly(address) else { return -2; };
    let escrow: &[u8; 32] = unsafe { &*(escrow_pubkey.cast::<[u8; 32]>()) };
    let signature: &[u8; 64] = unsafe { &*(cert.cast::<[u8; 64]>()) };
    let Ok(pubkey) = XOnlyPublicKey::from_slice(&payout_key) else { return -3; };
    let Ok(sig) = Signature::from_slice(signature) else { return -3; };
    let mut h = Blake2bParams::new().hash_length(32).to_state();
    h.update(ESCROW_DELEGATION_DOMAIN);
    h.update(escrow);
    let Ok(msg) = Message::from_digest_slice(h.finalize().as_bytes()) else { return -3; };
    if Secp256k1::verification_only().verify_schnorr(&sig, &msg, &pubkey).is_ok() { 0 } else { -4 }
}

/// Derive the official x-only escrow public key from an existing stock secret.
/// This is read-only: it never generates or persists key material.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_escrow_pubkey_from_secret(
    secret: *const u8, out: *mut u8,
) -> i32 {
    if secret.is_null() || out.is_null() { return -1; }
    let secret: &[u8; 32] = unsafe { &*(secret.cast::<[u8; 32]>()) };
    let Ok(sk) = SecretKey::from_slice(secret) else { return -2; };
    let pair = Keypair::from_secret_key(&Secp256k1::new(), &sk);
    let (pubkey, _) = pair.x_only_public_key();
    unsafe { std::ptr::copy_nonoverlapping(pubkey.serialize().as_ptr(), out, 32) };
    0
}

/// Parse the immutable stock AiResponse wire payload and return its request hash.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_response_request_hash(
    payload: *const u8, payload_len: usize, out: *mut u8,
) -> i32 {
    if out.is_null() { return -1; }
    let Ok(raw) = (unsafe { bytes(payload, payload_len) }) else { return -1; };
    let Some(response) = AiResponsePayload::deserialize(raw) else { return -2; };
    unsafe { std::ptr::copy_nonoverlapping(response.request_hash.as_ptr(), out, 32) };
    0
}

unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> Result<&'a [u8], ()> {
    if len != 0 && ptr.is_null() { return Err(()); }
    Ok(if len == 0 { &[] } else { unsafe { slice::from_raw_parts(ptr, len) } })
}

/// Opens the official AiRequest payload. Returns 0 on success, 1 when the escrow key is not a
/// recipient, and a negative value for malformed payload, authentication failure, or bad ABI.
/// `prompt_len` is in/out: set it to the output capacity; on return it is the required length.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_open_request(
    payload: *const u8, payload_len: usize, escrow_secret: *const u8,
    prompt_out: *mut u8, prompt_len: *mut usize, root_key_out: *mut u8,
    private_out: *mut u8,
) -> i32 {
    if prompt_len.is_null() || root_key_out.is_null() || private_out.is_null() || escrow_secret.is_null() { return -1; }
    let Ok(raw) = (unsafe { bytes(payload, payload_len) }) else { return -1; };
    let secret_slice = unsafe { slice::from_raw_parts(escrow_secret, 32) };
    let Ok(secret) = <&[u8; 32]>::try_from(secret_slice) else { return -1; };
    let Some(request) = AiRequestPayload::deserialize(raw) else { return -2; };
    let is_private = request.is_private();
    let (prompt, root) = if is_private {
        match open_request(&request, secret) {
            Ok(opened) => (opened.prompt.clone(), Some(opened.root_key)),
            Err(keryx_inference::PrivateError::NotARecipient) => return 1,
            Err(_) => return -3,
        }
    } else {
        (request.prompt.clone(), None)
    };
    let capacity = unsafe { *prompt_len };
    unsafe { *prompt_len = prompt.len(); *private_out = u8::from(is_private); }
    if prompt.len() > capacity || (prompt.len() != 0 && prompt_out.is_null()) { return 2; }
    if !prompt.is_empty() { unsafe { std::ptr::copy_nonoverlapping(prompt.as_ptr(), prompt_out, prompt.len()) }; }
    unsafe {
        if let Some(root) = root { std::ptr::copy_nonoverlapping(root.as_ptr(), root_key_out, 32); }
        else { std::ptr::write_bytes(root_key_out, 0, 32); }
    }
    0
}

/// Seals a private AiResponse body using the official v0.5.7 primitive.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_seal_private_response(
    root_key: *const u8, request_hash: *const u8, escrow_secret: *const u8,
    answer: *const u8, answer_len: usize, out: *mut u8, out_len: *mut usize,
) -> i32 {
    if root_key.is_null() || request_hash.is_null() || escrow_secret.is_null() || out_len.is_null() { return -1; }
    let Ok(answer) = (unsafe { bytes(answer, answer_len) }) else { return -1; };
    let root: &[u8; 32] = unsafe { &*(root_key.cast::<[u8; 32]>()) };
    let req: &[u8; 32] = unsafe { &*(request_hash.cast::<[u8; 32]>()) };
    let secret: &[u8; 32] = unsafe { &*(escrow_secret.cast::<[u8; 32]>()) };
    let Ok(pubkey) = escrow_pubkey_of(secret) else { return -2; };
    let body = seal_response(root, req, &pubkey, answer);
    let capacity = unsafe { *out_len };
    unsafe { *out_len = body.len(); }
    if body.len() > capacity || (body.len() != 0 && out.is_null()) { return 2; }
    if !body.is_empty() { unsafe { std::ptr::copy_nonoverlapping(body.as_ptr(), out, body.len()) }; }
    0
}

/// Builds the stock V2 signed AiResponse payload. `private_body` is either absent (public
/// response) or the already sealed H14 inline envelope. CID is the official raw sha2-256
/// multihash (IPFS CID for public responses; response-body multihash for inline private ones).
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_build_signed_response(
    request_hash: *const u8, escrow_secret: *const u8, cid: *const u8,
    response_daa: u64, answer: *const u8, answer_len: usize,
    private_body: *const u8, private_body_len: usize,
    out: *mut u8, out_len: *mut usize,
) -> i32 { unsafe {build_signed_response_inner(request_hash,escrow_secret,cid,response_daa,answer,answer_len,private_body,private_body_len,out,out_len,None,verified_inference::MAINNET_H15_ACTIVATION)} }

/// Explicit verified testnet-era builder. It constructs bytes only, never submits.
/// Network must come from a verified RPC identity, not a DAA/version inference.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_build_signed_traced_response(
    request_hash: *const u8, escrow_secret: *const u8, cid: *const u8,
    response_daa: u64, answer: *const u8, answer_len: usize,
    private_body: *const u8, private_body_len: usize,
    out: *mut u8, out_len: *mut usize,
    trace_bytes:*const u8,trace_len:usize,network:u32,
)->i32 {
    if network!=1 || response_daa<verified_inference::TESTNET_H15_ACTIVATION {return -10;}
    if private_body_len==0 || trace_len!=73 {return -2;}
    let Ok(raw)=(unsafe {bytes(trace_bytes,trace_len)}) else {return -1;};
    let Some(trace)=keryx_inference::TraceCommitment::from_bytes(raw) else {return -2;};
    if trace.stage!=trace_producer::STAGE_CHAIN || trace.tokens_root.is_none() ||
       trace.n_prompt==0 || trace.n_prompt>trace.n_pos || trace.n_pos-trace.n_prompt>4096 {return -2;}
    unsafe {build_signed_response_inner(request_hash,escrow_secret,cid,response_daa,answer,answer_len,private_body,private_body_len,out,out_len,Some(trace),Some(verified_inference::TESTNET_H15_ACTIVATION))}
}
unsafe fn build_signed_response_inner(
    request_hash: *const u8, escrow_secret: *const u8, cid: *const u8,
    response_daa: u64, answer: *const u8, answer_len: usize,
    private_body: *const u8, private_body_len: usize,
    out: *mut u8, out_len: *mut usize,
    trace:Option<keryx_inference::TraceCommitment>,activation:Option<u64>,
)->i32 {
    if request_hash.is_null() || escrow_secret.is_null() || cid.is_null() || out_len.is_null() { return -1; }
    let Ok(answer) = (unsafe { bytes(answer, answer_len) }) else { return -1; };
    let Ok(body) = (unsafe { bytes(private_body, private_body_len) }) else { return -1; };
    let request: &[u8; 32] = unsafe { &*(request_hash.cast::<[u8; 32]>()) };
    let secret: &[u8; 32] = unsafe { &*(escrow_secret.cast::<[u8; 32]>()) };
    let cid: &[u8; 34] = unsafe { &*(cid.cast::<[u8; 34]>()) };
    let mut words = 0u32;
    let mut in_word = false;
    for ch in String::from_utf8_lossy(answer).chars() {
        if ch.is_whitespace() { in_word = false; }
        else if !in_word { words = words.saturating_add(1); in_word = true; }
    }
    let mut response = AiResponsePayload::new(*request, response_daa.saturating_add(1000), *cid, words);
    if private_body_len != 0 { response = response.with_private_body(body.to_vec()); }
    if let Some(t)=trace {response=response.with_trace(t);}
    let Ok((pubkey, signature)) = sign_digest(secret, &response.signed_bytes()) else { return -2; };
    response = AiResponsePayload::new_v2(*request, response_daa.saturating_add(1000), *cid, words,
        AiResponder { escrow_pubkey: pubkey, signature });
    if private_body_len != 0 { response = response.with_private_body(body.to_vec()); }
    if let Some(t)=trace {response=response.with_trace(t);}
    let serialized = response.serialize();
    let mut subnet=[0u8;20];subnet[0]=4;
    if verified_inference::check_era(&subnet,&serialized,response_daa,
        activation,0).is_err() { return -10; }
    let capacity = unsafe { *out_len };
    unsafe { *out_len = serialized.len(); }
    if serialized.len() > capacity || (serialized.len() != 0 && out.is_null()) { return 2; }
    if !serialized.is_empty() { unsafe { std::ptr::copy_nonoverlapping(serialized.as_ptr(), out, serialized.len()) }; }
    0
}

/// Verifies the official encoding and responder signature. Optionally opens an inline private
/// body with the requester's root key; callers can compare the returned answer bytes.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_verify_response(
    payload: *const u8, payload_len: usize, expected_request_hash: *const u8,
    expected_escrow_pubkey: *const u8, requester_root_key: *const u8,
    answer_out: *mut u8, answer_len: *mut usize,
) -> i32 {
    if expected_request_hash.is_null() || expected_escrow_pubkey.is_null() || answer_len.is_null() { return -1; }
    let Ok(raw) = (unsafe { bytes(payload, payload_len) }) else { return -1; };
    let Some(response) = AiResponsePayload::deserialize(raw) else { return -2; };
    let request: &[u8; 32] = unsafe { &*(expected_request_hash.cast::<[u8; 32]>()) };
    if &response.request_hash != request { return -3; }
    let expected: &[u8; 32] = unsafe { &*(expected_escrow_pubkey.cast::<[u8; 32]>()) };
    let Some(responder) = response.responder else { return -4; };
    if &responder.escrow_pubkey != expected { return -5; }
    let mut h = Blake2bParams::new().hash_length(32).to_state();
    h.update(RESPONDER_DOMAIN);
    h.update(&response.signed_bytes());
    let Ok(msg) = Message::from_digest_slice(h.finalize().as_bytes()) else { return -6; };
    let Ok(pubkey) = XOnlyPublicKey::from_slice(expected) else { return -6; };
    let Ok(sig) = Signature::from_slice(&responder.signature) else { return -6; };
    if Secp256k1::verification_only().verify_schnorr(&sig, &msg, &pubkey).is_err() { return -7; }
    if let Some(body) = response.private_body {
        if requester_root_key.is_null() { return -8; }
        let root: &[u8; 32] = unsafe { &*(requester_root_key.cast::<[u8; 32]>()) };
        let Ok(answer) = open_response(root, request, expected, &body) else { return -9; };
        let capacity = unsafe { *answer_len };
        unsafe { *answer_len = answer.len(); }
        if answer.len() > capacity || (!answer.is_empty() && answer_out.is_null()) { return 2; }
        if !answer.is_empty() { unsafe { std::ptr::copy_nonoverlapping(answer.as_ptr(), answer_out, answer.len()) }; }
    } else {
        unsafe { *answer_len = 0; }
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use keryx_inference::{seal_request, AiRequestPayload};

    #[test]
    fn official_private_open_seal_signed_response_round_trip() {
        let requester = [7u8; 32];
        let escrow = [9u8; 32];
        let responder = escrow_pubkey_of(&escrow).unwrap();
        let (request, requester_secret) = seal_request([3u8; 32], 64, 100, 30_000_000,
            b"test prompt", &[responder]).unwrap();
        let opened = open_request(&request, &escrow).unwrap();
        assert_eq!(opened.prompt, b"test prompt");
        let body = seal_response(&opened.root_key, &[4u8; 32], &responder, b"cuda answer");
        let cid = [0x12u8; 34];
        let mut response = AiResponsePayload::new([4u8; 32], 1100, cid, 2).with_private_body(body.clone());
        let (pubkey, signature) = sign_digest(&escrow, &response.signed_bytes()).unwrap();
        response = AiResponsePayload::new_v2([4u8; 32], 1100, cid, 2,
            AiResponder { escrow_pubkey: pubkey, signature }).with_private_body(body);
        let bytes = response.serialize();
        let parsed = AiResponsePayload::deserialize(&bytes).unwrap();
        assert_eq!(parsed, response);
        assert_eq!(open_response(&requester_secret.root_key, &[4u8; 32], &responder,
            parsed.private_body.as_ref().unwrap()).unwrap(), b"cuda answer");
        assert_eq!(request.model_id, [3u8; 32]);
        let wrong = [8u8; 32];
        assert!(open_request(&request, &wrong).is_err());
        assert_eq!(escrow_pubkey_of(&escrow).unwrap(), responder);
        assert_ne!(requester, escrow);
    }

    #[test]
    fn public_request_format_uses_official_payload() {
        let request = AiRequestPayload::new([5u8; 32], 12, 99, 30_000_000, b"public prompt".to_vec());
        let bytes = request.serialize();
        assert_eq!(AiRequestPayload::deserialize(&bytes), Some(request));
    }

    #[test]
    fn stock_escrow_delegation_vector_matches_payout_and_escrow() {
        let payout = b"keryx:qp8n2k7uklxq4aegau7vawtptkgxsja4kt99lpv6krctwpq8tpc65uyeddvzr";
        let escrow = [0x22u8; 32];
        let mut derived = [0u8; 32];
        assert_eq!(unsafe { nexus_ai_escrow_pubkey_from_secret(escrow.as_ptr(), derived.as_mut_ptr()) }, 0);
        assert_eq!(derived, escrow_pubkey_of(&escrow).unwrap());
        let cert_hex = "f3cf9bfc6a29ba5608ebe777ff9e1f87d5bc3f80ab58e3040995631bfc16ab218b388c9b2d44140fc9448e639dc81c51ba475a780d1eaecc443c613e6d787c9e";
        let cert: [u8; 64] = (0..64).map(|i| u8::from_str_radix(&cert_hex[i*2..i*2+2], 16).unwrap()).collect::<Vec<_>>().try_into().unwrap();
        assert_eq!(unsafe { nexus_ai_verify_escrow_delegation(payout.as_ptr(), payout.len(), escrow.as_ptr(), cert.as_ptr()) }, 0);
        let wrong = [0x33u8; 32];
        assert_eq!(unsafe { nexus_ai_verify_escrow_delegation(payout.as_ptr(), payout.len(), wrong.as_ptr(), cert.as_ptr()) }, -4);
    }
}

// Consensus OPoI coinbase tag: delegate to the pinned official fixed-point model.
#[no_mangle]
pub unsafe extern "C" fn nexus_opoi_coinbase_tag(nonce: u64, output: *mut u8) -> i32 {
    if output.is_null() { return 0; }
    catch_unwind(|| {
        let tag = keryx_inference::tag_fixed(nonce);
        unsafe { std::ptr::copy_nonoverlapping(tag.as_ptr(), output, 16); }
        1
    }).unwrap_or(0)
}

#[cfg(test)]
#[test]
fn coinbase_tag_ffi_matches_official_consensus_parser() {
    for n in [0,42,u64::MAX] {
        let mut tag=[0u8;16];
        assert_eq!(unsafe { nexus_opoi_coinbase_tag(n,tag.as_mut_ptr()) },1);
        let wire=format!("/{:016x}/ai:v1:{}",n,std::str::from_utf8(&tag).unwrap());
        let mut payload=vec![0u8;32]; payload.extend_from_slice(wire.as_bytes());
        let (parsed,claimed)=keryx_inference::parse_opoi(&payload).unwrap();
        assert_eq!(parsed,n);
        assert!(keryx_inference::verify_tag_fixed(parsed,&claimed));
    }
    assert_eq!(unsafe { nexus_opoi_coinbase_tag(0,std::ptr::null_mut()) },0);
}

/// Reads the official, signed trace extension. Return 1 when absent, -2 on malformed payload.
/// This does not assert inference correctness; use the official opening verifier for that.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_response_trace(payload:*const u8, payload_len:usize, out:*mut u8)->i32 {
    if out.is_null() {return -1;}
    let Ok(raw)=(unsafe {bytes(payload,payload_len)}) else {return -1;};
    let Some(response)=AiResponsePayload::deserialize(raw) else {return -2;};
    let Some(trace)=response.trace else {return 1;};
    unsafe {if trace.tokens_root.is_some() { return -3; }
    std::ptr::copy_nonoverlapping(trace.to_bytes().as_ptr(),out,41);}
    0
}

/// Official request answer-byte bound. Pure CPU; does not produce or activate a trace.
#[no_mangle]
pub extern "C" fn nexus_ai_max_answer_len(max_tokens: u32) -> u64 {
    keryx_verifiable_inference::shape::max_answer_len(max_tokens)
}

#[cfg(test)]
#[test]
fn official_task_root_protobuf_field_19_is_preserved() {
    use prost::Message;
    let mut header=proto::RpcBlockHeader::default();
    header.task_root="55".repeat(32);
    let wire=header.encode_to_vec();
    assert_eq!(proto::RpcBlockHeader::decode(wire.as_slice()).unwrap().task_root,header.task_root);
    assert!(wire.windows(2).any(|v|v==[0x9a,0x01])); // field 19, length-delimited
}

/// Capacity-aware trace extension: 41-byte legacy or 73-byte token-bound commitment.
/// Reports required length without touching an undersized destination.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_response_trace_v2(payload:*const u8,payload_len:usize,
    out:*mut u8,capacity:usize,out_len:*mut usize)->i32 {
    if out_len.is_null() {return -1;}
    unsafe {*out_len=0;}
    let Ok(raw)=(unsafe {bytes(payload,payload_len)}) else {return -1;};
    let Some(response)=AiResponsePayload::deserialize(raw) else {return -2;};
    let Some(trace)=response.trace else {return 1;};
    let encoded=trace.to_bytes();
    unsafe {*out_len=encoded.len();}
    if capacity<encoded.len() {return -3;}
    if out.is_null() {return -1;}
    unsafe {std::ptr::copy_nonoverlapping(encoded.as_ptr(),out,encoded.len());}
    0
}

#[cfg(test)]
mod trace_v2_ffi_tests {
    use super::*;
    #[test]
    fn extended_commitment_never_overflows_legacy_or_short_buffers() {
        let trace=keryx_inference::TraceCommitment {trace_root:[5;32],n_prompt:3,n_pos:9,stage:2,tokens_root:Some([7;32])};
        let response=AiResponsePayload::new_v2([1;32],100,[0;34],3,AiResponder {escrow_pubkey:[2;32],signature:[3;64]})
            .with_private_body(vec![9]).with_trace(trace).serialize();
        let mut out=[0xcc;90];let mut len=0;
        assert_eq!(unsafe {nexus_ai_response_trace(response.as_ptr(),response.len(),out.as_mut_ptr())},-3);
        assert_eq!(out,[0xcc;90]);
        assert_eq!(unsafe {nexus_ai_response_trace_v2(response.as_ptr(),response.len(),out.as_mut_ptr(),41,&mut len)},-3);
        assert_eq!(len,73);assert_eq!(out,[0xcc;90]);
        assert_eq!(unsafe {nexus_ai_response_trace_v2(response.as_ptr(),response.len(),out.as_mut_ptr(),73,&mut len)},0);
        assert_eq!(&out[..73],trace.to_bytes());assert!(out[73..].iter().all(|b|*b==0xcc));
    }
}

#[cfg(test)]
mod traced_builder_tests {
 use super::*;
 #[test]
 fn stage2_tokens_are_sealed_signed_and_mainnet_cannot_activate_the_builder() {
  let request=[9;32];let root=[11;32];let secret=[7;32];let pk=escrow_pubkey_of(&secret).unwrap();
  let ids=[12u32,34];let plain=keryx_verifiable_inference::answer::encode(b"answer",&ids);
  let token_bytes=ids.iter().flat_map(|t|t.to_le_bytes()).collect::<Vec<_>>();
  let trace=keryx_inference::TraceCommitment {trace_root:[5;32],n_prompt:3,n_pos:5,stage:2,tokens_root:Some(*blake3::hash(&token_bytes).as_bytes())};
  let encoded=trace.to_bytes();let body=seal_response(&root,&request,&pk,&plain);let cid=[0;34];
  let mut out=vec![0u8;8192];let mut len=out.len();
  let call=|network,daa,out:&mut Vec<u8>,len:&mut usize|unsafe{nexus_ai_build_signed_traced_response(request.as_ptr(),secret.as_ptr(),cid.as_ptr(),daa,b"answer".as_ptr(),6,body.as_ptr(),body.len(),out.as_mut_ptr(),len,encoded.as_ptr(),encoded.len(),network)};
  assert_eq!(call(0,u64::MAX,&mut out,&mut len),-10);assert_eq!(call(1,3499,&mut out,&mut len),-10);
  assert_eq!(call(1,3500,&mut out,&mut len),0);out.truncate(len);
  let response=AiResponsePayload::deserialize(&out).unwrap();assert_eq!(response.trace,Some(trace));
  let mut answer=vec![0u8;8192];let mut n=answer.len();
  assert_eq!(unsafe{nexus_ai_verify_response(out.as_ptr(),out.len(),request.as_ptr(),pk.as_ptr(),root.as_ptr(),answer.as_mut_ptr(),&mut n)},0);
  let (text,got)=keryx_verifiable_inference::answer::decode(&answer[..n]).unwrap();assert_eq!(text,b"answer");assert_eq!(got,ids);
 }
}

#[cfg(test)]
mod chain_restore_wire_tests {
    use super::proto::*;
    use prost::Message;
    #[test]
    fn selected_chain_acceptance_fields_use_official_wire_numbers() {
        let request=GetVirtualSelectedParentChainFromBlockRequestMessage {start_hash:"a".into(),include_accepted_transaction_ids:true};
        assert_eq!(request.encode_to_vec(),vec![10,1,97,16,1]);
        let accepted=RpcAcceptedTransactionIds {accepting_block_hash:"b".into(),accepted_transaction_ids:vec!["c".into()]};
        let response=GetVirtualSelectedParentChainFromBlockResponseMessage {removed_chain_block_hashes:vec![],added_chain_block_hashes:vec!["b".into()],accepted_transaction_ids:vec![accepted],error:None};
        assert_eq!(response.encode_to_vec(),vec![18,6,10,1,98,18,1,99,26,1,98]);
        assert_eq!(GetVirtualSelectedParentChainFromBlockResponseMessage::decode(response.encode_to_vec().as_slice()).unwrap(),response);
    }
}
