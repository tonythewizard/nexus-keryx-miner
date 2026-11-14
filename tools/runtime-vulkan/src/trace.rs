//! Traced inference at ABI 9 (`src/network/trace_capture.cpp`) over the vendored engine's traced
//! evaluations. The host configures a manifest and a sequence layout, generates, reads the
//! leaves back, and later replays the same tokens to open single leaves.
//!
//! The engine runs each traced evaluation in a fresh context and its leaves depend on that
//! context's size, so every evaluation here — generation, token traces and replays — uses the one
//! context size the runtime was loaded with. The host's `context_acquire` sizes are bookkeeping.

use crate::llm_engine::{LlamaEngine, PromptHeadMismatch, PromptPrefix, PromptTooLong, SeqAttn, SeqRecur, SeqSpec, SEQ_KIND_GDA};
use keryx_verifiable_inference::manifest::{self, Chain};

/// A failed trace call, with the ABI return code the host sees.
pub struct TraceError {
    pub code: i32,
    pub message: String,
}

impl TraceError {
    fn new(code: i32, message: impl Into<String>) -> Self {
        Self { code, message: message.into() }
    }

    /// An engine error, with the codes of the official wrapper: -2 for a prompt the context cannot
    /// hold, -8 for a prompt head of the wrong length, -5 for any other trace failure.
    fn engine(e: anyhow::Error) -> Self {
        let code = if e.is::<PromptTooLong>() {
            -2
        } else if e.is::<PromptHeadMismatch>() {
            -8
        } else {
            -5
        };
        Self::new(code, format!("{e:#}"))
    }
}

type Result<T> = std::result::Result<T, TraceError>;

/// The trace the host configured, and the last traced evaluation, which `trace_read` and
/// `seq_roots` read back.
#[derive(Default)]
pub struct TraceState {
    manifest: Vec<u8>,
    max_pos: u32,
    leaves: usize,
    seq: Option<SeqSpec>,
    last: Option<Evaluation>,
}

struct Evaluation {
    ubatch: Vec<u32>,
    leaves: Vec<[u8; 32]>,
    seq: Option<Vec<[u8; 32]>>,
}

/// One traced generation as the host reads it.
pub struct Generated {
    pub text: String,
    pub tokens: Vec<i32>,
    pub n_prompt: u32,
}

impl TraceState {
    pub fn leaves(&self) -> Option<usize> {
        (self.leaves > 0).then_some(self.leaves)
    }

    /// Configures the trace of `manifest` for evaluations of up to `max_pos` positions. The
    /// sequence layout survives unless the manifest changes, as on the official runtime.
    pub fn configure(&mut self, engine: &LlamaEngine, manifest: &[u8], max_pos: u32) -> Result<usize> {
        if self.manifest != manifest {
            self.seq = None;
        }
        self.leaves = 0;
        let leaves = engine.trace_configure(manifest, max_pos, self.seq.as_ref()).map_err(TraceError::engine)?;
        self.manifest = manifest.to_vec();
        self.max_pos = max_pos;
        self.leaves = leaves;
        Ok(leaves)
    }

    /// Enables the sequence leaves of `rows` (`(layer, kind, slot, heads, head_dim)`), or disables
    /// them when `rows` is empty.
    pub fn configure_sequence(&mut self, engine: &LlamaEngine, rows: &[[u32; 5]], n_attn: u32, n_recur: u32, heads: u32, interval: u32) -> Result<()> {
        // Disabling always succeeds, with or without a trace (the host releases scratch this way).
        if rows.is_empty() {
            if self.seq.take().is_some() && self.leaves > 0 {
                engine.trace_configure(&self.manifest, self.max_pos, None).map_err(TraceError::engine)?;
            }
            return Ok(());
        }
        if self.leaves == 0 {
            return Err(TraceError::new(-1, "sequence leaves need a configured trace"));
        }
        let seq = seq_spec(&self.manifest, rows, n_attn, n_recur, heads, interval).map_err(|e| TraceError::new(-5, e))?;
        engine.trace_configure(&self.manifest, self.max_pos, Some(&seq)).map_err(TraceError::engine)?;
        self.seq = Some(seq);
        Ok(())
    }

    #[allow(clippy::too_many_arguments)]
    pub fn generate(
        &mut self,
        engine: &LlamaEngine,
        n_ctx: u32,
        prompt: &str,
        prefix: Option<PromptPrefix<'_>>,
        max_tokens: usize,
        min_prompt: u32,
        answer_bytes_per_position: u32,
    ) -> Result<Generated> {
        self.require_configured()?;
        self.last = None;
        let g = engine
            .generate_traced(&self.manifest, prompt, prefix, max_tokens, min_prompt, answer_bytes_per_position, |_| n_ctx)
            .map_err(TraceError::engine)?;
        if self.seq.is_some() != g.seq.is_some() {
            return Err(TraceError::new(-5, "sequence leaves missing from the traced evaluation"));
        }
        self.last = Some(Evaluation { ubatch: g.ubatch, leaves: g.leaves, seq: g.seq });
        Ok(Generated { text: g.text, tokens: g.tokens, n_prompt: g.n_prompt })
    }

    /// Traces `tokens` as a prompt with nothing generated. It commits no sequence leaves, so a
    /// sequence root read after it fails.
    pub fn trace_tokens(&mut self, engine: &LlamaEngine, n_ctx: u32, tokens: &[i32]) -> Result<()> {
        self.require_configured()?;
        self.last = None;
        let t = engine.trace_tokens(&self.manifest, tokens, n_ctx).map_err(TraceError::engine)?;
        self.last = Some(Evaluation { ubatch: t.ubatch, leaves: t.leaves, seq: None });
        Ok(())
    }

    /// Positions of the last evaluation, its leaves (`positions × leaves`) and batch sizes.
    pub fn last(&self) -> Option<(usize, &[[u8; 32]], &[u32])> {
        self.last.as_ref().map(|e| (e.ubatch.len(), e.leaves.as_slice(), e.ubatch.as_slice()))
    }

    pub fn sequence_roots(&self) -> Option<&[[u8; 32]]> {
        self.last.as_ref()?.seq.as_deref()
    }

    /// Bytes of leaf `leaf` at position `pos` for every `(pos, leaf)` of `caps`.
    pub fn replay(&self, engine: &LlamaEngine, n_ctx: u32, tokens: &[i32], n_prompt: u32, caps: &[(u32, u32)], slot_bytes: usize) -> Result<Vec<Vec<u8>>> {
        self.require_configured()?;
        engine.replay_multi(&self.manifest, tokens, n_prompt, caps, slot_bytes, n_ctx).map_err(TraceError::engine)
    }

    /// Bytes `[lo, hi)` of each `(leaf, lo, hi)` sequence span, with the chunk chaining values of
    /// that leaf.
    pub fn replay_sequence(&self, engine: &LlamaEngine, n_ctx: u32, tokens: &[i32], n_prompt: u32, spans: &[(u32, u64, u64)]) -> Result<Vec<(Vec<u8>, Vec<[u8; 32]>)>> {
        self.require_configured()?;
        if self.seq.is_none() {
            return Err(TraceError::new(-1, "no sequence leaves configured"));
        }
        let leaves: Vec<u32> = spans.iter().map(|s| s.0).collect();
        let whole = engine.replay_seq(&self.manifest, tokens, n_prompt, n_ctx, &leaves).map_err(TraceError::engine)?;
        spans
            .iter()
            .zip(&whole)
            .map(|(&(_, lo, hi), leaf)| {
                crate::llm_engine::seq_capture(leaf, lo, hi).map(|c| (c.bytes, c.cvs)).map_err(|e| TraceError::new(-6, format!("{e:#}")))
            })
            .collect()
    }

    fn require_configured(&self) -> Result<()> {
        if self.leaves == 0 {
            Err(TraceError::new(-1, "the trace is not configured"))
        } else {
            Ok(())
        }
    }
}

/// The engine's sequence layout for `manifest`: its attention and gated delta-rule layers with the
/// query-head counts the host's rows do not carry. The rows must name exactly those layers.
fn seq_spec(manifest_bytes: &[u8], rows: &[[u32; 5]], n_attn: u32, n_recur: u32, heads: u32, interval: u32) -> std::result::Result<SeqSpec, String> {
    let (entries, _) = manifest::read_file(manifest_bytes).ok_or("unreadable trace manifest")?;
    let (mut attn, mut recur) = (Vec::new(), Vec::new());
    for (e, _) in &entries {
        let Chain::Layer(ll) = &e.chain else { continue };
        if let Some(a) = &ll.attn {
            if a.mla_rank > 0 || a.swa_window > 0 || a.v_norm > 0 || a.softcap != 0 {
                return Err(format!("attention layer {} has a latent cache, a window, a value norm or a softcap", ll.layer));
            }
            attn.push(SeqAttn { layer: ll.layer, slot: a.slot, n_head: a.n_head, n_head_kv: a.n_head_kv, head_dim: a.head_dim });
        }
        if let Some(r) = &ll.recur {
            if r.kind != manifest::RECUR_GDA || r.interval != interval {
                return Err(format!("recurrent layer {} decays per key dimension or checkpoints every {} positions", ll.layer, r.interval));
            }
            recur.push(SeqRecur { layer: ll.layer, slot: r.slot, kind: SEQ_KIND_GDA, n_v_heads: r.n_v_heads, head_dim: r.head_dim });
        }
    }
    attn.sort_by_key(|a| a.slot);
    recur.sort_by_key(|r| r.slot);
    let mut expected: Vec<[u32; 5]> = attn
        .iter()
        .map(|a| [a.layer, 1, a.slot, a.n_head_kv, a.head_dim])
        .chain(recur.iter().map(|r| [r.layer, r.kind, r.slot, r.n_v_heads, r.head_dim]))
        .collect();
    let mut given = rows.to_vec();
    expected.sort_unstable();
    given.sort_unstable();
    if expected != given {
        return Err("the sequence rows differ from the manifest's layers".into());
    }
    let spec = SeqSpec { n_attn, n_recur, heads, attn, recur, interval: if n_recur == 0 { 0 } else { interval } };
    spec.check().map_err(|e| e.to_string())?;
    Ok(spec)
}
