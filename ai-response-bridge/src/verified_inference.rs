//! Official H15 wire/verification compatibility. No production trace producer or sender.
use keryx_inference::AiResponsePayload;
use keryx_verifiable_inference::{draw, opening::{Opening, SignedPayload}, verify::{self, ResponseCtx, TierCtx}};
use secp256k1::{schnorr::Signature, Message, Secp256k1, XOnlyPublicKey};

pub const NODE_COMMIT: &str = "15c1d4f358f018298c4ce69260b19540d44dbfbf";
pub const MINER_COMMIT: &str = "f2561aedeb649da7b2c46fc4e845e487c9a7d9e5";
// Official mainnet parameters have no H15 activation. Never infer activation from version/DAA.
pub const MAINNET_H15_ACTIVATION: Option<u64> = None;
// Official 565c2fe prefix gate: mainnet u64::MAX sentinel, testnet DAA 215000.
// None prevents treating the disabled mainnet sentinel as an activatable DAA.
pub const MAINNET_TRACE_PREFIX_ACTIVATION: Option<u64> = None;
pub const TESTNET_H15_ACTIVATION: u64 = 3_500;
pub const TESTNET_TRACE_PREFIX_ACTIVATION: u64 = TESTNET_H15_ACTIVATION;
pub const OPENING_SUBNETWORK: [u8; 20] = [6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0];
pub const TRACE_SEED_DELAY_DAA: u64 = 100;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Failure { BeforeActivation, Malformed, Binding, Signature, Point }

/// Exact node era policy, with the ledger horizon supplied from verified network parameters.
/// This is a guard, not an activation override; production uses the pinned None above.
pub fn check_era(subnetwork: &[u8;20], payload: &[u8], daa: u64,
                 activation: Option<u64>, ledger_horizon: u64) -> Result<(),Failure> {
    let active=activation.is_some_and(|at|daa>=at);
    let response=if subnetwork[0]==4 && subnetwork[1..].iter().all(|&v|v==0) {
        AiResponsePayload::deserialize(payload)
    }else{None};
    if !active {
        if response.as_ref().and_then(|r|r.private_body.as_ref()).is_some_and(|b|b.len()>keryx_inference::MAX_AI_RESPONSE_PRIVATE_BODY_LEN_H14) {
            return Err(Failure::Malformed);
        }
        if subnetwork==&OPENING_SUBNETWORK || response.as_ref().is_some_and(|r|r.trace.is_some()) {
            return Err(Failure::BeforeActivation);
        }
        return Ok(());
    }
    if subnetwork==&OPENING_SUBNETWORK {
        let signed=SignedPayload::split(payload).ok_or(Failure::Malformed)?;
        Opening::decode(signed.body).ok_or(Failure::Malformed)?;
    }
    if daa>=activation.unwrap().saturating_add(ledger_horizon) &&
        response.as_ref().is_some_and(|r|r.private_body.is_some() && r.trace.is_none()) {
        return Err(Failure::Malformed);
    }
    Ok(())
}

/// The official draw seed is chain-hash dependent, never based on presumed consecutive DAA.
pub fn draw_seed(chain_hash:&[u8;32], response_txid:&[u8;32])->[u8;32] {
    let mut h=blake2b_simd::Params::new().hash_length(32).to_state();
    h.update(b"KeryxTraceDrawV1");h.update(chain_hash);h.update(response_txid);
    h.finalize().as_bytes().try_into().unwrap()
}

/// Node-equivalent signature and both point checks. Caller must separately establish accepted
/// response, canonical draw block, assignment, activation, deadline and reorg coverage.
pub fn verify_opening(payload:&[u8], expected_response:&[u8;32], expected_signer:&[u8;32],
                      tier:&TierCtx, response:&ResponseCtx, seed:&[u8;32])->Result<(),Failure> {
    verify_opening_inner(payload, expected_response, expected_signer, tier, response, seed, None, None, None)
}

/// Verify a prefix-era opening against the original canonical AiRequest id.
/// This pure verifier does not authorize transactions or activate an era.
pub fn verify_opening_for_request(payload:&[u8], expected_response:&[u8;32], expected_signer:&[u8;32],
                      tier:&TierCtx, response:&ResponseCtx, seed:&[u8;32],
                      prefix:&keryx_verifiable_inference::prefix::PrefixCtx,
                      request_hash:&[u8;32])->Result<(),Failure> {
    verify_opening_inner(payload, expected_response, expected_signer, tier, response, seed,
                        Some((prefix, request_hash)), None, None)
}

/// Stage-2 verifier: signature, products, normalization links, routing/residual
/// data and request-bound prefix. This never activates a network era or sends.
#[allow(clippy::too_many_arguments)]
pub fn verify_chain_opening(payload:&[u8], expected_response:&[u8;32], expected_signer:&[u8;32],
    tier:&TierCtx, response:&ResponseCtx, seed:&[u8;32],
    prefix:&keryx_verifiable_inference::prefix::PrefixCtx, request_hash:&[u8;32],
    chain:&keryx_verifiable_inference::chain::ChainCtx) -> Result<(),Failure> {
    verify_opening_inner(payload,expected_response,expected_signer,tier,response,seed,Some((prefix,request_hash)),Some(chain),None)
}

/// Seed-bound v4 envelope. Old entry points reject seeded bodies rather than ignoring binding.
#[allow(clippy::too_many_arguments)]
pub fn verify_seeded_chain_opening(payload:&[u8],response_id:&[u8;32],signer:&[u8;32],
    tier:&TierCtx,response:&ResponseCtx,seed_block:&[u8;32],
    prefix:&keryx_verifiable_inference::prefix::PrefixCtx,request:&[u8;32],
    chain:&keryx_verifiable_inference::chain::ChainCtx)->Result<(),Failure> {
    let seed=draw_seed(seed_block,response_id);
    verify_opening_inner(payload,response_id,signer,tier,response,&seed,Some((prefix,request)),Some(chain),Some(seed_block))
}

fn verify_opening_inner(payload:&[u8], expected_response:&[u8;32], expected_signer:&[u8;32],
                      tier:&TierCtx, response:&ResponseCtx, seed:&[u8;32],
                      prefix:Option<(&keryx_verifiable_inference::prefix::PrefixCtx, &[u8;32])>,
                      chain:Option<&keryx_verifiable_inference::chain::ChainCtx>,expected_seed_block:Option<&[u8;32]>)
                      ->Result<(),Failure> {
    let signed=SignedPayload::split(payload).ok_or(Failure::Malformed)?;
    if &signed.escrow_pubkey!=expected_signer {return Err(Failure::Binding);}
    match (keryx_verifiable_inference::opening::seed_block(signed.body),expected_seed_block) {
        (Some(named),Some(expected)) if &named==expected && draw_seed(expected,expected_response)==*seed => {},
        (None,None) if signed.body.first()!=Some(&keryx_verifiable_inference::opening::VERSION_SEEDED) => {},
        _ => return Err(Failure::Binding),
    }
    let opening=Opening::decode(signed.body).ok_or(Failure::Malformed)?;
    if &opening.response_txid!=expected_response || opening.points.len()!=draw::POINTS as usize {
        return Err(Failure::Binding);
    }
    let mut h=blake2b_simd::Params::new().hash_length(32).to_state();
    h.update(b"KeryxTraceOpeningV1");h.update(signed.body);
    let msg=Message::from_digest_slice(h.finalize().as_bytes()).map_err(|_|Failure::Signature)?;
    let pk=XOnlyPublicKey::from_slice(expected_signer).map_err(|_|Failure::Signature)?;
    let sig=Signature::from_slice(&signed.signature).map_err(|_|Failure::Signature)?;
    Secp256k1::verification_only().verify_schnorr(&sig,&msg,&pk).map_err(|_|Failure::Signature)?;
    if let Some(chain) = chain {
        use keryx_verifiable_inference::chain::{verify_chain_point,verify_chain_prefix_point,verify_resid_point};
        let Some((prefix, request)) = prefix else { return Err(Failure::Binding); };
        let Some(prefix_point) = &opening.prefix else { return Err(Failure::Binding); };
        let Some(resid) = &opening.resid else { return Err(Failure::Binding); };
        if opening.links.len()!=opening.points.len() || opening.experts.len()!=opening.points.len() {
            return Err(Failure::Binding);
        }
        for (i,(point,(token,link))) in opening.points.iter().zip(&opening.links).enumerate() {
            verify_chain_point(tier,chain,response,seed,i as u32,point,*token,link,opening.experts[i].as_ref())
                .map_err(|_|Failure::Point)?;
        }
        verify_chain_prefix_point(tier,chain,prefix,response,seed,request,prefix_point).map_err(|_|Failure::Point)?;
        verify_resid_point(tier,chain,response,seed,resid).map_err(|_|Failure::Point)?;
    } else {
        // A stage-1 entry point must never ignore stage-2 obligations.
        if !opening.links.is_empty() || !opening.experts.is_empty() || opening.resid.is_some() {
            return Err(Failure::Binding);
        }
        for (i,point) in opening.points.iter().enumerate() {
            verify::verify_point(tier,response,seed,i as u32,point).map_err(|_|Failure::Point)?;
        }
        match (prefix, opening.prefix.as_ref()) {
            (Some((ctx, request)), Some(point)) => {
                keryx_verifiable_inference::prefix::verify_prefix_point(tier,ctx,response,seed,request,point)
                    .map_err(|_|Failure::Point)?;
            }
            (None, None) => {},
            _ => return Err(Failure::Binding),
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
 use super::*;
 use keryx_inference::{AiResponder,TraceCommitment};
 use keryx_verifiable_inference::testkit::Model;
 use secp256k1::{Keypair,SecretKey};
 fn sign(body:&[u8])->([u8;32],Vec<u8>) {
  let secp=Secp256k1::new();let key=Keypair::from_secret_key(&secp,&SecretKey::from_slice(&[7;32]).unwrap());
  let (pk,_)=XOnlyPublicKey::from_keypair(&key);let mut h=blake2b_simd::Params::new().hash_length(32).to_state();h.update(b"KeryxTraceOpeningV1");h.update(body);
  let sig=secp.sign_schnorr_no_aux_rand(&Message::from_digest_slice(h.finalize().as_bytes()).unwrap(),&key);
  let mut payload=body.to_vec();payload.extend_from_slice(&pk.serialize());payload.extend_from_slice(sig.as_ref());(pk.serialize(),payload)
 }
 #[test]fn mainnet_gate_never_emits_new_transactions() {
  let mut subnet=[0;20];subnet[0]=4;
  let legacy=AiResponsePayload::new_v2([1;32],10,[0;34],1,AiResponder{escrow_pubkey:[2;32],signature:[3;64]}).with_private_body(vec![4;40]);
  let traced=legacy.clone().with_trace(TraceCommitment{trace_root:[5;32],n_prompt:3,n_pos:9,stage:1,tokens_root:None});
  assert_eq!(check_era(&subnet,&legacy.serialize(),u64::MAX,MAINNET_H15_ACTIVATION,100),Ok(()));
  assert_eq!(check_era(&subnet,&traced.serialize(),u64::MAX,MAINNET_H15_ACTIVATION,100),Err(Failure::BeforeActivation));
  assert_eq!(check_era(&OPENING_SUBNETWORK,&[],u64::MAX,MAINNET_H15_ACTIVATION,100),Err(Failure::BeforeActivation));
  assert_eq!(check_era(&subnet,&legacy.serialize(),99,Some(100),200),Ok(()));
  assert_eq!(check_era(&subnet,&legacy.serialize(),299,Some(100),200),Ok(()));
  assert_eq!(check_era(&subnet,&legacy.serialize(),300,Some(100),200),Err(Failure::Malformed));
  assert_eq!(check_era(&subnet,&traced.serialize(),300,Some(100),200),Ok(()));
 }
 #[test]fn official_opening_signature_binding_and_point_verification() {
  let model=Model::new();let resp=model.response(3,9,true);let seed=draw_seed(&[3;32],&[9;32]);
  let opening=model.open(&resp,[9;32],&seed);let (pk,payload)=sign(&opening.encode());
  let ctx=ResponseCtx{trace_root:resp.trace_root,n_prompt:resp.n_prompt,n_pos:resp.n_pos};
  assert_eq!(verify_opening(&payload,&[9;32],&pk,&model.tier(),&ctx,&seed),Ok(()));
  let tier=model.tier();
  let native=OpeningContext{model_root:tier.r_t,manifest_root:tier.m_t,trace_root:resp.trace_root,response_txid:[9;32],escrow_pubkey:pk,draw_seed:seed,n_chunks:tier.n_chunks,n_entries:tier.n_entries,n_products:tier.n_products,n_prompt:3,n_pos:9};
  assert_eq!(std::mem::size_of::<OpeningContext>(),224);
  assert_eq!(unsafe{nexus_ai_verify_trace_opening(payload.as_ptr(),payload.len(),&native)},0);
  assert_eq!(verify_opening(&payload,&[8;32],&pk,&model.tier(),&ctx,&seed),Err(Failure::Binding));
  let mut tampered=payload.clone();tampered[40]^=1;assert!(verify_opening(&tampered,&[9;32],&pk,&model.tier(),&ctx,&seed).is_err());
  assert_ne!(unsafe{nexus_ai_verify_trace_opening(tampered.as_ptr(),tampered.len(),&native)},0);
  let dishonest=model.response(3,9,false);let bad=model.open(&dishonest,[9;32],&seed);let (_,badpayload)=sign(&bad.encode());
  let badctx=ResponseCtx{trace_root:dishonest.trace_root,n_prompt:3,n_pos:9};
  assert_eq!(verify_opening(&badpayload,&[9;32],&pk,&model.tier(),&badctx,&seed),Err(Failure::Point));
 }
 #[test]fn prefix_openings_require_the_original_request_and_explicit_verifier() {
  let model=Model::new();let request=[0x41;32];let seed=draw_seed(&[3;32],&[9;32]);
  let resp=model.response_with_prefix(40,48,true,Some((&request,6)));
  let ctx=ResponseCtx{trace_root:resp.trace_root,n_prompt:resp.n_prompt,n_pos:resp.n_pos};
  let prefix=model.prefix_ctx(6);
  let opening=model.open_with_prefix(&resp,[9;32],&seed,&request,6);
  let (pk,payload)=sign(&opening.encode());
  assert_eq!(verify_opening_for_request(&payload,&[9;32],&pk,&model.tier(),&ctx,&seed,&prefix,&request),Ok(()));
  let tier=model.tier();
  let native=OpeningContext{model_root:tier.r_t,manifest_root:tier.m_t,trace_root:resp.trace_root,
   response_txid:[9;32],escrow_pubkey:pk,draw_seed:seed,n_chunks:tier.n_chunks,n_entries:tier.n_entries,
   n_products:tier.n_products,n_prompt:resp.n_prompt,n_pos:resp.n_pos};
  let mut native_prefix=PrefixOpeningContext{request_hash:request,tokens:prefix.table,offset:prefix.offset,
   embedding_entry:prefix.embd_entry,norm_entry:prefix.norm_entry,epsilon:prefix.eps,embedding_scale:prefix.embd_scale};
  assert_eq!(std::mem::size_of::<PrefixOpeningContext>(),1088);
  assert_eq!(unsafe{nexus_ai_verify_trace_opening_prefix(payload.as_ptr(),payload.len(),&native,&native_prefix)},0);
  native_prefix.request_hash=[0x42;32];
  assert_ne!(unsafe{nexus_ai_verify_trace_opening_prefix(payload.as_ptr(),payload.len(),&native,&native_prefix)},0);
  assert_eq!(unsafe{nexus_ai_verify_trace_opening_prefix(payload.as_ptr(),payload.len(),&native,std::ptr::null())},-1);
  assert_eq!(verify_opening(&payload,&[9;32],&pk,&model.tier(),&ctx,&seed),Err(Failure::Binding));
  assert!(verify_opening_for_request(&payload,&[9;32],&pk,&model.tier(),&ctx,&seed,&prefix,&[0x42;32]).is_err());
  let (_,legacy)=sign(&model.open(&resp,[9;32],&seed).encode());
  assert_eq!(verify_opening_for_request(&legacy,&[9;32],&pk,&model.tier(),&ctx,&seed,&prefix,&request),Err(Failure::Binding));
  assert!(!crate::trace_producer::prefix_active(u64::MAX));
  assert_eq!(TESTNET_TRACE_PREFIX_ACTIVATION,3_500);
 }
 #[test]
 fn stage2_signed_opening_checks_chain_prefix_residuals_and_rejects_downgrade() {
  use keryx_verifiable_inference::{opening::Opening,verify::ResponseCtx};
  let model=crate::stock_stage2_fixture::Tier::new();
  let request=[9u8;32];let tokens=request.iter().map(|v|*v as u32).collect::<Vec<_>>();
  let trace=model.trace(&tokens,32);let seed=[7u8;32];let prefix=model.prefix_ctx(0);
  let ctx=ResponseCtx{trace_root:trace.trace_root,n_prompt:32,n_pos:32};
  let mut opening=Opening{response_txid:[9;32],points:Vec::new(),links:Vec::new(),
    experts:vec![None;draw::POINTS as usize],prefix:Some(model.open_prefix(&trace,&seed,0)),
    resid:Some(model.open_resid(&trace,&seed))};
  for i in 0..draw::POINTS {let (p,t,l)=model.open_point(&trace,&seed,i);opening.points.push(p);opening.links.push((t,l));}
  let (pk,payload)=sign(&opening.encode());let tier=model.tier();let chain=model.chain_ctx();
  assert_eq!(verify_chain_opening(&payload,&[9;32],&pk,&tier,&ctx,&seed,&prefix,&request,&chain),Ok(()));
  let native=OpeningContext{model_root:tier.r_t,manifest_root:tier.m_t,trace_root:ctx.trace_root,
    response_txid:[9;32],escrow_pubkey:pk,draw_seed:seed,n_chunks:tier.n_chunks,n_entries:tier.n_entries,
    n_products:tier.n_products,n_prompt:ctx.n_prompt,n_pos:ctx.n_pos};
  let native_prefix=PrefixOpeningContext{request_hash:request,tokens:prefix.table,offset:prefix.offset,
    embedding_entry:prefix.embd_entry,norm_entry:prefix.norm_entry,epsilon:prefix.eps,embedding_scale:prefix.embd_scale};
  let mut native_chain=ChainOpeningContext{n_layers:chain.n_layers,layer_base:chain.layer_base,
    embedding_entry:chain.embd_entry,n_used:chain.n_used,epsilon:chain.eps,embedding_scale:chain.embd_scale,
    trim_last:u8::from(chain.trim_last),reserved:[0;7]};
  assert_eq!(std::mem::size_of::<ChainOpeningContext>(),40);
  assert_eq!(unsafe{nexus_ai_verify_trace_opening_chain(payload.as_ptr(),payload.len(),&native,&native_prefix,&native_chain)},0);
  native_chain.trim_last=2;
  assert_eq!(unsafe{nexus_ai_verify_trace_opening_chain(payload.as_ptr(),payload.len(),&native,&native_prefix,&native_chain)},-2);
  assert_eq!(unsafe{nexus_ai_verify_trace_opening_chain(payload.as_ptr(),payload.len(),&native,&native_prefix,std::ptr::null())},-1);
  assert_eq!(verify_opening_for_request(&payload,&[9;32],&pk,&tier,&ctx,&seed,&prefix,&request),Err(Failure::Binding));
  assert!(verify_chain_opening(&payload,&[9;32],&pk,&tier,&ctx,&seed,&prefix,&[3;32],&chain).is_err());
  opening.links[0].0^=1;let (_,bad)=sign(&opening.encode());
  assert!(verify_chain_opening(&bad,&[9;32],&pk,&tier,&ctx,&seed,&prefix,&request,&chain).is_err());
 }

}

#[cfg(test)]
mod abi_tests {
 use super::*;
 use keryx_inference::{AiResponder,TraceCommitment};
 #[test]fn sealed_trace_response_signature_and_abi_roundtrip() {
  let request=[9u8;32];let secret=[7u8;32];let root=[11u8;32];
  let pk=keryx_inference::escrow_pubkey_of(&secret).unwrap();
  let answer=b"verified private response";
  let body=keryx_inference::seal_response(&root,&request,&pk,answer);
  let trace=TraceCommitment{trace_root:[5;32],n_prompt:3,n_pos:9,stage:1,tokens_root:None};
  let unsigned=AiResponsePayload::new(request,100,[0;34],3).with_private_body(body.clone()).with_trace(trace);
  let (_,signature)=crate::sign_digest(&secret,&unsigned.signed_bytes()).unwrap();
  let response=AiResponsePayload::new_v2(request,100,[0;34],3,AiResponder{escrow_pubkey:pk,signature}).with_private_body(body).with_trace(trace);
  let mut payload=response.serialize();let mut out=[0u8;41];
  assert_eq!(unsafe{crate::nexus_ai_response_trace(payload.as_ptr(),payload.len(),out.as_mut_ptr())},0);
  assert_eq!(out.as_slice(),trace.to_bytes().as_slice());
  let mut recovered=vec![0u8;64];let mut len=recovered.len();
  assert_eq!(unsafe{crate::nexus_ai_verify_response(payload.as_ptr(),payload.len(),request.as_ptr(),pk.as_ptr(),root.as_ptr(),recovered.as_mut_ptr(),&mut len)},0);
  assert_eq!(&recovered[..len],answer);
  // Root is signed: a relay cannot replace the commitment under a valid signature.
  let offset=payload.len()-41;payload[offset]^=1;
  assert_ne!(unsafe{crate::nexus_ai_verify_response(payload.as_ptr(),payload.len(),request.as_ptr(),pk.as_ptr(),root.as_ptr(),recovered.as_mut_ptr(),&mut len)},0);
  assert_eq!(unsafe{crate::nexus_ai_response_trace(payload.as_ptr(),10,out.as_mut_ptr())},-2);
 }
}

/// Public context pinned to an accepted response and its canonical draw block by the caller.
#[repr(C)]
pub struct OpeningContext {
 pub model_root:[u8;32], pub manifest_root:[u8;32], pub trace_root:[u8;32],
 pub response_txid:[u8;32], pub escrow_pubkey:[u8;32], pub draw_seed:[u8;32],
 pub n_chunks:u64, pub n_entries:u64, pub n_products:u64,
 pub n_prompt:u32, pub n_pos:u32,
}

fn opening_contexts(c: &OpeningContext) -> Result<(TierCtx, ResponseCtx), i32> {
    if c.n_chunks == 0 || c.n_entries == 0 || c.n_products == 0 || c.n_products > c.n_entries {
        return Err(-2);
    }
    Ok((TierCtx { r_t: c.model_root, n_chunks: c.n_chunks, m_t: c.manifest_root,
                  n_entries: c.n_entries, n_products: c.n_products },
        ResponseCtx { trace_root: c.trace_root, n_prompt: c.n_prompt, n_pos: c.n_pos }))
}

fn opening_result(result: Result<(), Failure>) -> i32 {
    match result {
        Ok(()) => 0,
        Err(Failure::Malformed) => -3,
        Err(Failure::Binding) => -4,
        Err(Failure::Signature) => -5,
        Err(_) => -6,
    }
}

/// Additive native ABI. Verification is separate from assignment/era/deadline/submission.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_verify_trace_opening(payload:*const u8,len:usize,context:*const OpeningContext)->i32 {
 if context.is_null() || payload.is_null() || len>keryx_verifiable_inference::opening::MAX_PAYLOAD_LEN {return -1;}
 std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
  let c=unsafe{&*context};let payload=unsafe{std::slice::from_raw_parts(payload,len)};
  let (tier, response) = match opening_contexts(c) { Ok(value) => value, Err(code) => return code };
  opening_result(verify_opening(payload,&c.response_txid,&c.escrow_pubkey,&tier,&response,&c.draw_seed))
 })).unwrap_or(-9)
}

/// Prefix parameters from the verified network/tier manifest; not an activation override.
#[repr(C)]
pub struct PrefixOpeningContext {
 pub request_hash:[u8;32], pub tokens:[u32;256],
 pub offset:u32, pub embedding_entry:u32, pub norm_entry:u32,
 pub epsilon:f64, pub embedding_scale:f64,
}

/// Additive native request-bound verifier. Caller still proves era, assignment,
/// accepted response, canonical draw block, deadline and reorg coverage.
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_verify_trace_opening_prefix(payload:*const u8,len:usize,
    context:*const OpeningContext,prefix:*const PrefixOpeningContext)->i32 {
 if context.is_null() || prefix.is_null() || payload.is_null() ||
    len>keryx_verifiable_inference::opening::MAX_PAYLOAD_LEN {return -1;}
 std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
  let c=unsafe{&*context};let p=unsafe{&*prefix};let payload=unsafe{std::slice::from_raw_parts(payload,len)};
  let (tier, response) = match opening_contexts(c) { Ok(value) => value, Err(code) => return code };
  if p.embedding_entry as u64>=c.n_entries || p.norm_entry as u64>=c.n_entries ||
     !p.epsilon.is_finite() || p.epsilon<0.0 || !p.embedding_scale.is_finite() {return -2;}
  let pref=keryx_verifiable_inference::prefix::PrefixCtx{table:p.tokens,offset:p.offset,
   embd_entry:p.embedding_entry,norm_entry:p.norm_entry,eps:p.epsilon,embd_scale:p.embedding_scale};
  opening_result(verify_opening_for_request(payload,&c.response_txid,&c.escrow_pubkey,&tier,&response,&c.draw_seed,&pref,&p.request_hash))
 })).unwrap_or(-9)
}

/// Additive native stage-2 context. Never an activation or submission permission.
#[repr(C)]
pub struct ChainOpeningContext {
 pub n_layers:u32, pub layer_base:u32, pub embedding_entry:u32, pub n_used:u32,
 pub epsilon:f64, pub embedding_scale:f64, pub trim_last:u8, pub reserved:[u8;7],
}
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_verify_trace_opening_chain(payload:*const u8,len:usize,
 context:*const OpeningContext,prefix:*const PrefixOpeningContext,chain:*const ChainOpeningContext)->i32 {
 unsafe {verify_chain_native(payload,len,context,prefix,chain,None)}
}
#[no_mangle]
pub unsafe extern "C" fn nexus_ai_verify_trace_opening_chain_seeded(payload:*const u8,len:usize,
 context:*const OpeningContext,prefix:*const PrefixOpeningContext,chain:*const ChainOpeningContext,seed_block:*const u8)->i32 {
 if seed_block.is_null() {return -1;}
 unsafe {verify_chain_native(payload,len,context,prefix,chain,Some(&*seed_block.cast::<[u8;32]>()))}
}
unsafe fn verify_chain_native(payload:*const u8,len:usize,context:*const OpeningContext,
 prefix:*const PrefixOpeningContext,chain:*const ChainOpeningContext,seed_block:Option<&[u8;32]>)->i32 {
 if payload.is_null() || context.is_null() || prefix.is_null() || chain.is_null() ||
    len>keryx_verifiable_inference::opening::MAX_PAYLOAD_LEN {return -1;}
 std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
  let (c,p,k)=unsafe{(&*context,&*prefix,&*chain)};
  let (tier,response)=match opening_contexts(c){Ok(v)=>v,Err(e)=>return e};
  if k.n_layers==0 || k.n_layers>512 || k.n_used>256 || k.trim_last>1 || k.reserved!=[0;7] ||
     k.layer_base as u64+k.n_layers as u64>c.n_entries ||
     k.embedding_entry!=p.embedding_entry || p.embedding_entry as u64>=c.n_entries ||
     p.norm_entry as u64>=c.n_entries || !k.epsilon.is_finite() || k.epsilon<=0.0 ||
     !k.embedding_scale.is_finite() || k.embedding_scale<=0.0 ||
     k.epsilon!=p.epsilon || k.embedding_scale!=p.embedding_scale {return -2;}
  let pref=keryx_verifiable_inference::prefix::PrefixCtx{table:p.tokens,offset:p.offset,
    embd_entry:p.embedding_entry,norm_entry:p.norm_entry,eps:p.epsilon,embd_scale:p.embedding_scale};
  let chain=keryx_verifiable_inference::chain::ChainCtx{n_layers:k.n_layers,layer_base:k.layer_base,
    embd_entry:k.embedding_entry,eps:k.epsilon,embd_scale:k.embedding_scale,trim_last:k.trim_last!=0,n_used:k.n_used};
  let payload=unsafe{std::slice::from_raw_parts(payload,len)};
  if let Some(block)=seed_block {
      if draw_seed(block,&c.response_txid)!=c.draw_seed {return -3;}
      opening_result(verify_seeded_chain_opening(payload,&c.response_txid,&c.escrow_pubkey,&tier,&response,block,&pref,&p.request_hash,&chain))
  } else { opening_result(verify_chain_opening(payload,&c.response_txid,&c.escrow_pubkey,&tier,&response,&c.draw_seed,&pref,&p.request_hash,&chain)) }
 })).unwrap_or(-9)
}

#[cfg(test)]
mod seeded_tests {
    use super::*;
    #[test]
    fn seed_block_is_authenticated_and_never_ignored_by_old_entrypoint() {
        use keryx_verifiable_inference::opening;
        let model=crate::stock_stage2_fixture::Tier::new();let request=[9;32];let id=[8;32];let block=[6;32];
        let tokens=request.iter().map(|v|*v as u32).collect::<Vec<_>>();let trace=model.trace(&tokens,32);
        let seed=draw_seed(&block,&id);let prefix=model.prefix_ctx(0);let tier=model.tier();let chain=model.chain_ctx();
        let response=ResponseCtx {trace_root:trace.trace_root,n_prompt:32,n_pos:32};
        let mut body=Opening {response_txid:id,points:vec![],links:vec![],experts:vec![None;draw::POINTS as usize],
            prefix:Some(model.open_prefix(&trace,&seed,0)),resid:Some(model.open_resid(&trace,&seed))};
        for i in 0..draw::POINTS {let (p,t,l)=model.open_point(&trace,&seed,i);body.points.push(p);body.links.push((t,l));}
        let wrapped=opening::seeded(&body.encode(),&block).unwrap();
        use secp256k1::{Keypair,SecretKey};
        let secp=Secp256k1::new();let keys=Keypair::from_secret_key(&secp,&SecretKey::from_slice(&[7;32]).unwrap());
        let (key,_)=XOnlyPublicKey::from_keypair(&keys);let pk=key.serialize();
        let mut h=blake2b_simd::Params::new().hash_length(32).to_state();h.update(b"KeryxTraceOpeningV1");h.update(&wrapped);
        let sig=secp.sign_schnorr_no_aux_rand(&Message::from_digest_slice(h.finalize().as_bytes()).unwrap(),&keys);
        let mut signed=wrapped;signed.extend(pk);signed.extend(sig.as_ref());
        assert_eq!(verify_seeded_chain_opening(&signed,&id,&pk,&tier,&response,&block,&prefix,&request,&chain),Ok(()));
        assert_eq!(verify_seeded_chain_opening(&signed,&id,&pk,&tier,&response,&[4;32],&prefix,&request,&chain),Err(Failure::Binding));
        assert_eq!(verify_chain_opening(&signed,&id,&pk,&tier,&response,&seed,&prefix,&request,&chain),Err(Failure::Binding));
    }
}
