//! Offline verifier of a real Vulkan capture. No GPU initialization or network operations.
use nexus_ai_response_bridge::trace_producer::{self as producer, TierManifest, TraceRecord};
use nexus_ai_response_bridge::verified_inference;
use keryx_verifiable_inference::{manifest, merkle::Hash, prefix, q8, verify::{self, ResponseCtx, TierCtx}};
use secp256k1::{Keypair, Message, Secp256k1, SecretKey, XOnlyPublicKey};
use std::{collections::HashSet, path::Path};

#[path = "support/trace_weights.rs"]
mod trace_weights;
use trace_weights::{need, read, Index};

fn run(args: &[std::ffi::OsString]) -> Result<(), String> {
    need(args.len() == 4, "usage: vulkan_trace_oracle CAPTURE_DIRECTORY MODEL TREE")?;
    let root = Path::new(&args[1]);
    let text = String::from_utf8(read(&root.join("capture.tsv"), 8192)?).map_err(|e| e.to_string())?;
    let mut lines = text.lines(); need(lines.next() == Some("NEXUS_VULKAN_TRACE_FIXTURE_V1"), "invalid capture schema")?;
    let shape: Vec<usize> = lines.next().ok_or("missing shape")?.split('\t').map(|v| v.parse().map_err(|_| "invalid dimension")).collect::<Result<_, _>>()?;
    need(shape.len() == 5 && shape[0] > 0 && shape[0] <= shape[1] && shape[1] <= 64 && shape[4] <= 1, "invalid capture dimensions")?;
    let tokens: Vec<i32> = lines.map(|v| v.parse().map_err(|_| "invalid token")).collect::<Result<_, _>>()?;
    need(tokens.len() == shape[1] && tokens.iter().all(|t| *t >= 0), "invalid captured tokens")?;
    let answer = read(&root.join("answer.txt"), 32)?; need(answer.len() == shape[2], "answer size mismatch")?;
    let m = TierManifest::for_tier(0).ok_or("manifest unavailable")?;
    need(hex::encode(m.m_t) == "9b802ae5f4cafe073bd97b72222a77f44eb597491ab07235180c5ef8783f62bc", "official manifest pin mismatch")?;
    let index = Index::open(Path::new(&args[2]), Path::new(&args[3]))?;
    for (entry, _) in &m.entries {
        let t = index.tensors.iter().find(|t| t.name == entry.name).ok_or("manifest tensor absent from GGUF")?;
        need((t.ty, t.k, t.m, t.base, t.bytes) == (entry.wtype, entry.k, entry.m * entry.n_exp as u64, entry.chunk_base, entry.n_bytes), "GGUF/manifest identity mismatch")?;
    }
    let r_t: Hash = hex::decode("2c497164eaf20078add20e82ae4e1b0fdb27d3fdd5eaefc1c48f204111e14e88").unwrap().try_into().unwrap();
    let root_level = (0..64).find(|l| index.count(*l) == 1).ok_or("invalid model height")?;
    need(index.node(root_level, 0) == Some(r_t), "official model root mismatch")?;
    let tier = TierCtx { r_t, n_chunks: index.chunks, m_t: m.m_t, n_entries: m.entries.len() as u64, n_products: m.n_products as u64 };
    let mut leaves = Vec::new();
    for t in 0..shape[1] {
        for (e, _) in m.entries.iter().take(m.n_products as usize) {
            let file = |suffix| root.join(t.to_string()).join(format!("{}.{suffix}", m.entries.iter().position(|(v, _)| v.name == e.name).unwrap()));
            let x = read(&file('x'), 65536)?;
            need(x.len() == e.k.div_ceil(512) as usize * 512 / 32 * 36, "captured X size mismatch")?;
            let (values, sums) = q8::decode(&x, q8::MMVQ, e.k as usize).ok_or("invalid Q8 input")?;
            need(values.iter().all(|v| v.is_finite()) && sums.as_ref().is_none_or(|v| v.iter().all(|x| x.is_finite())), "non-finite Q8 input")?;
            leaves.push(*blake3::hash(&x).as_bytes());
            if e.glu_form == manifest::FORM_PAIR_UP { leaves.push([0; 32]); }
            else { let y = read(&file('y'), 8 << 20)?; need(y.len() == e.m as usize * 4, "captured Y size mismatch")?;
                need(y.chunks_exact(4).all(|v| f32::from_le_bytes(v.try_into().unwrap()).is_finite()), "non-finite captured output")?;
                leaves.push(*blake3::hash(&y).as_bytes()); }
        }
        let start = leaves.len() - 2 * m.n_products as usize;
        for (rank, (e, _)) in m.entries.iter().take(m.n_products as usize).enumerate() {
            if e.glu_form == manifest::FORM_PAIR_GATE {
                need(leaves[start + 2 * rank] == leaves[start + 2 * e.partner as usize], "GLU pair consumed different activation bytes")?;
            }
        }
        leaves.push([0; 32]);
    }
    let rec = TraceRecord::new_for_response(&m, shape[0] as u32, vec![1; shape[1]], leaves, shape[3] as u32, shape[2] as u32, 1)
        .ok_or("incomplete or invalid official response trace shape")?;
    let response = ResponseCtx { trace_root: rec.trace_root, n_prompt: rec.n_prompt, n_pos: rec.n_pos };
    let mut points = 0;
    // Cover every product in the official opening verifier, including pair-up draws.
    for rank in 0..m.n_products {
        let mut selected = None;
        for counter in 0u64..100000 {
            let seed = *blake3::hash(&counter.to_le_bytes()).as_bytes();
            let p = producer::draw_point(&m, rec.n_prompt, rec.n_pos, &seed, 0).ok_or("draw failed")?;
            if p.rank == rank as u64 { selected = Some((seed, p)); break; }
        }
        let (seed, p) = selected.ok_or("bounded product draw search exhausted")?;
        let at = root.join(p.t.to_string());
        let x = read(&at.join(format!("{}.x", p.g_rank)), 65536)?;
        let y = read(&at.join(format!("{}.y", p.g_rank)), 8 << 20)?;
        let point = producer::build_point(&m, &rec, &p, x, &y, &index).ok_or("opening construction failed")?;
        verify::verify_point(&tier, &response, &seed, 0, &point).map_err(|e| format!("official opening rejected rank={rank} t={} reason={e:?}", p.t))?;
        points += 1;
    }
    let mut prefixes = 0;
    let mut prefix_positions = HashSet::new();
    if shape[4] == 1 {
        let request = [0x41; 32]; let ctx = producer::prefix_ctx(0).ok_or("prefix context absent")?;
        let expected = producer::prefix_tokens(ctx, &request);
        need(tokens.get(ctx.offset as usize..ctx.offset as usize + 32) == Some(expected.as_slice()), "request-bound prefix token mismatch")?;
        for counter in 0u64..2048 {
            let seed = *blake3::hash(&counter.to_le_bytes()).as_bytes(); let t = producer::draw_prefix(ctx, &seed);
            let x = read(&root.join(t.to_string()).join("0.x"), 65536)?;
            let point = producer::build_prefix_point(&m, &rec, ctx, &request, t, x, &index).ok_or("prefix construction failed")?;
            prefix::verify_prefix_point(&tier, ctx, &response, &seed, &request, &point).map_err(|e| format!("official prefix rejected: {e:?}"))?;
            prefixes += 1; prefix_positions.insert(t);
            if prefixes >= 64 && prefix_positions.len() == 32 { break; }
        }
        need(prefix_positions.len() == 32, "bounded prefix draw coverage incomplete")?;
    }
    // Sign one complete real-model opening using a public, deterministic test-only key.
    // No configured escrow identity is read or changed, and nothing is submitted.
    let response_txid = [9u8; 32]; let request = [0x41; 32];
    let seed = producer::trace_seed(&[3u8; 32], &response_txid);
    let mut drawn = Vec::new();
    for i in 0..producer::POINTS {
        let p = producer::draw_point(&m, rec.n_prompt, rec.n_pos, &seed, i).ok_or("opening draw failed")?;
        let at = root.join(p.t.to_string());
        drawn.push(producer::build_point(&m, &rec, &p, read(&at.join(format!("{}.x", p.g_rank)), 65536)?,
            &read(&at.join(format!("{}.y", p.g_rank)), 8 << 20)?, &index).ok_or("signed opening construction failed")?);
    }
    let prefix_point = if shape[4] == 1 {
        let ctx = producer::prefix_ctx(0).unwrap(); let t = producer::draw_prefix(ctx, &seed);
        Some(producer::build_prefix_point(&m, &rec, ctx, &request, t,
            read(&root.join(t.to_string()).join("0.x"), 65536)?, &index).ok_or("signed prefix construction failed")?)
    } else { None };
    let body = producer::opening_body_with_prefix(response_txid, drawn, prefix_point).ok_or("opening exceeds official size cap")?;
    let secp = Secp256k1::new(); let key = Keypair::from_secret_key(&secp, &SecretKey::from_slice(&[7u8; 32]).unwrap());
    let (public, _) = XOnlyPublicKey::from_keypair(&key); let signer = public.serialize();
    let signature = secp.sign_schnorr_no_aux_rand(&Message::from_digest_slice(&producer::opening_signature_message(&body)).unwrap(), &key);
    let mut payload = body; payload.extend_from_slice(&signer); payload.extend_from_slice(signature.as_ref());
    let verify_signed = |payload: &[u8], txid: &[u8; 32], request: &[u8; 32]| {
        if shape[4] == 1 {
            verified_inference::verify_opening_for_request(payload, txid, &signer, &tier, &response, &seed, producer::prefix_ctx(0).unwrap(), request)
        } else { verified_inference::verify_opening(payload, txid, &signer, &tier, &response, &seed) }
    };
    verify_signed(&payload, &response_txid, &request).map_err(|e| format!("signed opening rejected: {e:?}"))?;
    let mut bad = payload.clone(); *bad.last_mut().unwrap() ^= 1;
    need(verify_signed(&bad, &response_txid, &request).is_err(), "altered signature accepted")?;
    need(verify_signed(&payload, &[8; 32], &request).is_err(), "wrong response binding accepted")?;
    if shape[4] == 1 { need(verify_signed(&payload, &response_txid, &[0x42; 32]).is_err(), "wrong request prefix accepted")?; }
    std::fs::write(root.join("opening.synthetic-fixture.bin"), &payload).map_err(|e| e.to_string())?;
    println!("OFFICIAL_TRACE_OPENINGS=PASS products={points} prefixes={prefixes} prefix_positions={} positions={} trace_root={} SIGNED_OPENING=PASS NEGATIVE_BINDING_SIGNATURE=PASS mainnet_transactions=0",
        prefix_positions.len(), rec.n_pos, hex::encode(rec.trace_root));
    Ok(())
}
fn main() {
    if let Err(error) = run(&std::env::args_os().collect::<Vec<_>>()) { eprintln!("OFFICIAL_TRACE_OPENINGS=FAIL {error}"); std::process::exit(1); }
}
