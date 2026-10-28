//! Native replay-to-proof path. GPU ownership belongs to the caller; every
//! returned piece has passed the same point checks used by the pinned node.
use crate::{trace_audit_store::wire::ResponseTrace, trace_producer as p, trace_sequence as s};
use keryx_verifiable_inference::{attn_blocks, chain, merkle::Hash, opening};

pub trait Replay {
    fn captures(
        &mut self,
        trace: &ResponseTrace,
        points: &[(u32, u32)],
        slot: u32,
    ) -> Result<p::Captures, String>;
    fn spans(
        &mut self,
        trace: &ResponseTrace,
        plans: &[s::SpanPlan],
        max_chunks: u32,
    ) -> Result<Vec<s::SeqCapture>, String>;
}
fn captures(
    r: &mut impl Replay,
    m: &p::TierManifest,
    t: &ResponseTrace,
    points: &[(u32, u32)],
) -> Result<p::Captures, String> {
    let slot = points
        .iter()
        .map(|&(_, leaf)| p::leaf_cap(m, leaf))
        .max()
        .ok_or("empty capture plan")?;
    r.captures(
        t,
        points,
        u32::try_from(slot).map_err(|_| "capture slot overflow")?,
    )
}
pub fn chain_pieces(
    tier: u8,
    t: &ResponseTrace,
    seed: &Hash,
    request: &Hash,
    response: Hash,
    index: &impl p::WeightIndex,
    r: &mut impl Replay,
) -> Result<(Vec<u8>, Vec<u8>), String> {
    let m = p::TierManifest::for_attn(tier).ok_or("unsupported tier")?;
    let ctx = m.chain.ok_or("missing chain context")?;
    let pctx = p::chain_ctx(tier).ok_or("no prefix context")?.1;
    let d = p::draw_chain(&m, &pctx, &t.record, seed).ok_or("draw failed")?;
    let caps = captures(r, &m, t, &d.captures(m.n_products as u64))?;
    let (mut o, mut residual) = p::chain_opening(
        &m, &pctx, &t.record, &t.tokens, seed, request, response, &d, &caps, index,
    )
    .ok_or("chain/residual proof assembly failed")?;
    let seq = t.seq.as_ref().ok_or("missing sequence commitment")?;
    o.roots = Some((t.record.trace_root, seq.root));
    residual.roots = o.roots;
    let tier = m.tier_ctx(index);
    let resp = t.record.response_ctx();
    if o.points.len() != p::POINTS as usize
        || o.links.len() != o.points.len()
        || o.experts.len() != o.points.len()
    {
        return Err("chain shape mismatch".into());
    }
    for (i, (point, (tok, link))) in o.points.iter().zip(&o.links).enumerate() {
        chain::verify_chain_point(
            &tier,
            &ctx,
            &resp,
            seed,
            i as u32,
            point,
            *tok,
            link,
            o.experts[i].as_ref(),
        )
        .map_err(|e| format!("chain point {e:?}"))?;
    }
    chain::verify_chain_prefix_point(
        &tier,
        &ctx,
        &pctx,
        &resp,
        seed,
        request,
        o.prefix.as_ref().ok_or("missing prefix")?,
    )
    .map_err(|e| format!("prefix {e:?}"))?;
    chain::verify_resid_opening(&tier, &ctx, &resp, seed, &residual)
        .map_err(|e| format!("residual feed {e:?}"))?;
    let body = o.encode();
    let residual = residual.encode();
    if !p::opening_fits_seeded(body.len()) || !p::opening_fits_seeded(residual.len()) {
        return Err("chain/residual envelope too large".into());
    }
    Ok((body, residual))
}

pub fn attention_piece(
    tier: u8,
    t: &ResponseTrace,
    seed: &Hash,
    response: Hash,
    index: &impl p::WeightIndex,
    r: &mut impl Replay,
    path: &[u64],
    prev: Option<&[u8]>,
) -> Result<Vec<u8>, String> {
    let m = p::TierManifest::for_attn(tier).ok_or("unsupported tier")?;
    let rec = &t.record;
    let seq = t.seq.as_ref().ok_or("no sequence trace")?;
    let d = s::draw_attn(&m, rec, seed).ok_or("no attention draw")?;
    let caps = captures(r, &m, t, &d.captures(path).ok_or("bad phase path")?)?;
    let plans = d
        .spans(&m, rec.n_pos as u64, path)
        .ok_or("no sequence plan")?;
    let spans = r.spans(t, &plans, s::max_chunks(&plans))?;
    let full = s::attn_opening(&m, rec, seq, index, &d, &caps, &spans, response, path, prev)?;
    let ctx = m.chain.ok_or("missing chain")?;
    let tier = m.tier_ctx(index);
    let resp = s::seq_response_ctx(rec, seq);
    let parent = prev.and_then(|b| attn_blocks::partials_root(b, d.dv));
    let verify = |op: &opening::AttnOpening| {
        chain::verify_attn_phase(&tier, &ctx, &resp, seed, path, parent.as_ref(), op)
    };
    let op = if d.leaf(path) {
        full
    } else {
        let (q, scale) = chain::attn_query(&tier, &ctx, &resp, seed, &full)
            .map_err(|e| format!("attention query {e:?}"))?;
        let (keys, values) = d.rows(&full, path).ok_or("missing cache rows")?;
        let start = d.tree.node(path).ok_or("missing tree node")?.0;
        let partials =
            attn_blocks::pick_partials(&q, scale, &keys, &values, start, &d.tree, path, |parts| {
                verify(&d.phase_fields(&full, path, attn_blocks::encode_partials(parts))).is_ok()
            })
            .ok_or("no verifying partials")?;
        d.phase_fields(&full, path, partials)
    };
    verify(&op).map_err(|e| format!("attention phase {e:?}"))?;
    s::encode_attention_opening(&op).ok_or("attention envelope too large".into())
}
pub fn recurrent_piece(
    tier: u8,
    t: &ResponseTrace,
    seed: &Hash,
    response: Hash,
    index: &impl p::WeightIndex,
    r: &mut impl Replay,
) -> Result<Option<Vec<u8>>, String> {
    let m = p::TierManifest::for_attn(tier).ok_or("unsupported tier")?;
    if m.chain.ok_or("missing chain")?.n_recur == 0 {
        return Ok(None);
    }
    let d = s::draw_recur(&m, &t.record, seed).ok_or("no recurrent draw")?;
    let caps = captures(r, &m, t, &d.captures())?;
    let plans = d
        .spans(&m, t.record.n_pos as u64)
        .ok_or("no recurrent spans")?;
    let spans = r.spans(t, &plans, s::max_chunks(&plans))?;
    let seq = t.seq.as_ref().ok_or("missing sequence commitment")?;
    let op = s::recur_opening(&m, &t.record, seq, index, &d, &caps, &spans, response)?;
    keryx_verifiable_inference::recur::verify_recur_point(
        &m.tier_ctx(index),
        &m.chain.unwrap(),
        &s::seq_response_ctx(&t.record, seq),
        seed,
        &op,
    )
    .map_err(|e| format!("recurrent {e:?}"))?;
    Ok(Some(
        s::encode_recurrent_opening(&op).ok_or("recurrent envelope too large")?,
    ))
}
