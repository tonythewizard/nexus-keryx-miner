//! The keryx-miner-rdna3 model lineup, reduced to what the vendored `llm_engine.rs` reads from
//! it: a per-model context cap. Nexus serves one model with the cap it passes to
//! `keryx_llama_load` (exported as `KERYX_LLAMA_CTX`), so no lineup entry applies.

pub struct ModelSpec {
    pub ctx_cap: u32,
}

pub fn spec_for_gguf(_gguf_path: &str) -> Option<&'static ModelSpec> {
    None
}
