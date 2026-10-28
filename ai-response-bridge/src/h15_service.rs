//! Durable native audit obligations. Node acceptance and selected-chain draws
//! are distinct from RPC acknowledgement. All effects follow fsynced intent.
use crate::{
    h15_bridge::Capture, trace_audit_store as store, trace_producer as p,
    trace_recovery as recovery, trace_sequence as seq,
};
use borsh::{BorshDeserialize, BorshSerialize};
use keryx_verifiable_inference::{attn_blocks, merkle::Hash, opening};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs,
    io::Write,
    path::{Path, PathBuf},
    sync::Arc,
};
const MAX_RECORD: usize = 64 << 20;
#[derive(Clone, Debug, BorshSerialize, BorshDeserialize, PartialEq, Eq)]
pub struct ChainBlock {
    pub hash: Hash,
    pub daa: u64,
    pub accepted: Vec<Hash>,
}
#[derive(Clone, Debug, BorshSerialize, BorshDeserialize, PartialEq, Eq)]
pub struct Piece {
    pub payload: Vec<u8>,
    pub id: Hash,
    pub path: Vec<u64>,
    pub kind: u8,
    pub draw_block: Option<Hash>,
    pub accepted: Option<(Hash, u64)>,
    pub deadline: u64,
    pub attempts: u32,
    pub last_ms: u64,
}
#[derive(Clone, BorshSerialize, BorshDeserialize)]
struct StateV2 {
    version: u32,
    binding: Binding,
    response: Hash,
    request: Hash,
    response_payload: Vec<u8>,
    submitted: u64,
    anchor: Hash,
    accepted: Option<(Hash, u64)>,
    seed: Option<(Hash, u64)>,
    pieces: Vec<Piece>,
    cached: Vec<Piece>,
    failures: u32,
    retry_after: u64,
    retired: bool,
    response_deadline: u64,
    response_attempts: u32,
    response_last_ms: u64,
    response_rejected: bool,
}
#[derive(Clone, Debug, BorshSerialize, BorshDeserialize, PartialEq, Eq)]
pub struct AwaitedOpening {
    pub response: Hash,
    pub signer: Hash,
    pub seed: Hash,
    pub deadline: u64,
    pub pieces: u32,
    pub path: Vec<u64>,
    pub accepted_daa: u64,
}
#[derive(BorshSerialize, BorshDeserialize)]
pub struct AwaitedFrame {
    pub daa: u64,
    pub openings: Vec<AwaitedOpening>,
}
#[derive(Clone, Default, BorshSerialize, BorshDeserialize)]
struct AuditPolicyV3 {
    own_deadline: Option<u64>,
    deadline: Option<u64>,
    unaudited: bool,
    replay_retry_ms: u64,
    request_accepted: Option<Hash>,
    early_after_ms: Option<u64>,
    early_sends: u32,
    last_response_was_early: bool,
}
#[derive(Clone, BorshSerialize, BorshDeserialize)]
struct StateV3 {
    version: u32,
    binding: Binding,
    response: Hash,
    request: Hash,
    response_payload: Vec<u8>,
    submitted: u64,
    anchor: Hash,
    accepted: Option<(Hash, u64)>,
    seed: Option<(Hash, u64)>,
    pieces: Vec<Piece>,
    cached: Vec<Piece>,
    failures: u32,
    retry_after: u64,
    retired: bool,
    response_deadline: u64,
    response_attempts: u32,
    response_last_ms: u64,
    response_rejected: bool,
    policy: AuditPolicyV3,
}
#[derive(Clone, Default, BorshSerialize, BorshDeserialize)]
struct AuditPolicy {
    own_deadline: Option<u64>,
    deadline: Option<u64>,
    unaudited: bool,
    replay_retry_ms: u64,
    request_accepted: Option<Hash>,
    early_after_ms: Option<u64>,
    early_sends: u32,
    last_response_was_early: bool,
    node: Option<AwaitedOpening>,
}
#[derive(Clone, BorshSerialize, BorshDeserialize)]
struct State {
    version: u32,
    binding: Binding,
    response: Hash,
    request: Hash,
    response_payload: Vec<u8>,
    submitted: u64,
    anchor: Hash,
    accepted: Option<(Hash, u64)>,
    seed: Option<(Hash, u64)>,
    pieces: Vec<Piece>,
    cached: Vec<Piece>,
    failures: u32,
    retry_after: u64,
    retired: bool,
    response_deadline: u64,
    response_attempts: u32,
    response_last_ms: u64,
    response_rejected: bool,
    policy: AuditPolicy,
}
impl From<StateV2> for State {
    fn from(s: StateV2) -> Self {
        Self {
            version: s.version,
            binding: s.binding,
            response: s.response,
            request: s.request,
            response_payload: s.response_payload,
            submitted: s.submitted,
            anchor: s.anchor,
            accepted: s.accepted,
            seed: s.seed,
            pieces: s.pieces,
            cached: s.cached,
            failures: s.failures,
            retry_after: s.retry_after,
            retired: s.retired,
            response_deadline: s.response_deadline,
            response_attempts: s.response_attempts,
            response_last_ms: s.response_last_ms,
            response_rejected: s.response_rejected,
            policy: AuditPolicy::default(),
        }
    }
}
impl From<StateV3> for State {
    fn from(s: StateV3) -> Self {
        let p = s.policy;
        Self {
            version: s.version,
            binding: s.binding,
            response: s.response,
            request: s.request,
            response_payload: s.response_payload,
            submitted: s.submitted,
            anchor: s.anchor,
            accepted: s.accepted,
            seed: s.seed,
            pieces: s.pieces,
            cached: s.cached,
            failures: s.failures,
            retry_after: s.retry_after,
            retired: s.retired,
            response_deadline: s.response_deadline,
            response_attempts: s.response_attempts,
            response_last_ms: s.response_last_ms,
            response_rejected: s.response_rejected,
            policy: AuditPolicy {
                own_deadline: p.own_deadline,
                deadline: p.deadline,
                unaudited: p.unaudited,
                replay_retry_ms: p.replay_retry_ms,
                request_accepted: p.request_accepted,
                early_after_ms: p.early_after_ms,
                early_sends: p.early_sends,
                last_response_was_early: p.last_response_was_early,
                node: None,
            },
        }
    }
}
fn wall_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .min(u64::MAX as u128) as u64
}
#[derive(Clone, BorshSerialize, BorshDeserialize, PartialEq, Eq)]
pub struct Binding {
    pub model: Hash,
    pub root: Hash,
    pub chunks: u64,
}
pub struct Pending {
    state: State,
    pub capture: Capture,
}
pub struct Service {
    dir: PathBuf,
    store: store::TraceStore,
    pending: BTreeMap<Hash, Pending>,
    pub chain: Vec<ChainBlock>,
    restore: BTreeMap<Hash, BTreeSet<Hash>>,
    secret: Hash,
    pub tier: u8,
    binding: Binding,
    next_span: u64,
    // Session observations, deliberately absent from the durable schema.
    node_supported: bool,
    awaited_polled: Vec<Hash>,
    node_awaits: BTreeMap<Hash, bool>,
}
fn unfinished(pieces: &[Piece]) -> bool {
    if pieces.is_empty() || pieces.iter().any(|p| p.accepted.is_none()) {
        return true;
    }
    let Some(last) = pieces
        .iter()
        .filter(|p| p.kind == 1)
        .max_by_key(|p| p.path.len())
    else {
        return true;
    };
    opening::SignedPayload::split(&last.payload)
        .and_then(|p| opening::inner_body(p.body))
        .and_then(|b| opening::AttnOpening::decode(&b))
        .is_none_or(|o| !o.partials.is_empty())
}
fn id(bytes: &[u8], subnet: u8) -> Hash {
    let mut b = Vec::new();
    b.extend_from_slice(&0u16.to_le_bytes());
    b.extend_from_slice(&[0; 24]);
    let mut sn = [0u8; 20];
    sn[0] = subnet;
    b.extend_from_slice(&sn);
    b.extend_from_slice(&0u64.to_le_bytes());
    b.extend_from_slice(&(bytes.len() as u64).to_le_bytes());
    b.extend_from_slice(bytes);
    blake2b_simd::Params::new()
        .hash_length(32)
        .key(b"TransactionID")
        .hash(&b)
        .as_bytes()
        .try_into()
        .unwrap()
}
pub fn audit_txid(bytes: &[u8]) -> Hash {
    id(bytes, 6)
}
fn signing(secret: &Hash, body: &[u8]) -> Result<Vec<u8>, String> {
    let digest = p::opening_signature_message(body);
    let key = secp256k1::SecretKey::from_slice(secret).map_err(|_| "invalid signer")?;
    let secp = secp256k1::Secp256k1::new();
    let keypair = secp256k1::Keypair::from_secret_key(&secp, &key);
    let signer = keypair.x_only_public_key().0.serialize();
    let msg = secp256k1::Message::from_digest_slice(&digest).map_err(|_| "signing digest")?;
    let sig = secp.sign_schnorr_no_aux_rand(&msg, &keypair);
    let mut out = body.to_vec();
    out.extend_from_slice(&signer);
    out.extend_from_slice(sig.as_ref());
    Ok(out)
}
fn persist(dir: &Path, id: &Hash, state: &State) -> Result<(), String> {
    let mut bytes = Vec::with_capacity(4096);
    bytes.extend_from_slice(b"NXH15ST1");
    bytes.resize(40, 0); // Digest follows the unchanged eight-byte magic.
    state
        .serialize(&mut bytes)
        .map_err(|_| "audit state encoding")?;
    // The first Borsh field is the u32 schema. Normalize restored v2/v3 state
    // without cloning its signed responses and proofs or changing the wire.
    bytes[40..44].copy_from_slice(&4u32.to_le_bytes());
    let digest = blake3::hash(&bytes[40..]);
    bytes[8..40].copy_from_slice(digest.as_bytes());
    if bytes.len() > 4 << 20 {
        return Err("audit metadata bound".into());
    }
    let mut tmp = tempfile::NamedTempFile::new_in(dir).map_err(|e| e.to_string())?;
    tmp.write_all(&bytes)
        .and_then(|_| tmp.as_file().sync_all())
        .map_err(|e| e.to_string())?;
    tmp.persist(dir.join(format!("{}.audit", hex::encode(id))))
        .map_err(|e| e.to_string())?;
    #[cfg(unix)]
    fs::File::open(dir)
        .and_then(|f| f.sync_all())
        .map_err(|e| e.to_string())?;
    Ok(())
}
fn read_state(path: &Path) -> Result<State, String> {
    let meta = fs::symlink_metadata(path).map_err(|e| e.to_string())?;
    if !meta.is_file() || meta.len() > 4 << 20 {
        return Err("audit state bounds".into());
    }
    let bytes = fs::read(path).map_err(|e| e.to_string())?;
    if bytes.len() < 40
        || &bytes[..8] != b"NXH15ST1"
        || blake3::hash(&bytes[40..]).as_bytes() != &bytes[8..40]
    {
        return Err("audit metadata checksum mismatch; original preserved".into());
    }
    let version = u32::from_le_bytes(
        bytes
            .get(40..44)
            .ok_or("truncated audit metadata version")?
            .try_into()
            .unwrap(),
    );
    match version {
        2 => StateV2::try_from_slice(&bytes[40..])
            .map(State::from)
            .map_err(|_| "corrupt legacy audit metadata".into()),
        3 => StateV3::try_from_slice(&bytes[40..])
            .map(State::from)
            .map_err(|_| "corrupt legacy audit metadata".into()),
        4 => State::try_from_slice(&bytes[40..]).map_err(|_| "corrupt audit metadata".into()),
        _ => Err("unsupported audit metadata version; original preserved".into()),
    }
}
fn trace_commit(c: &Capture) -> Result<keryx_inference::TraceCommitment, String> {
    let r = &c.trace.record;
    let s = c.trace.seq.as_ref().ok_or("missing sequence trace")?;
    let m = p::TierManifest::for_attn(c.tier).ok_or("unsupported trace tier")?;
    if r.n_pos > 32768
        || r.leaf_hashes().len() > 512 * 1024 * 1024 / 32
        || !r.storage_shape_valid()
        || r.n_pos as usize != c.trace.tokens.len()
        || !c.gen_secs.is_finite()
        || c.gen_secs <= 0.0
        || c.max_tokens == 0
    {
        return Err("invalid persisted trace dimensions".into());
    }
    let prefix = p::chain_ctx(c.tier).ok_or("missing prefix")?.1;
    let expected = p::prefix_tokens(&prefix, &c.request);
    if c.trace
        .tokens
        .get(prefix.offset as usize..prefix.offset as usize + expected.len())
        != Some(expected.as_slice())
    {
        return Err("persisted request prefix mismatch".into());
    }
    let rebuilt = p::TraceRecord::new_chain(
        &m,
        r.n_prompt,
        r.ubatch.clone(),
        r.leaf_hashes().to_vec(),
        &c.trace.tokens,
    )
    .ok_or("corrupt persisted trace")?;
    let layout = m.chain.unwrap();
    let n = 2 * layout.seq_heads as usize * (layout.n_attn + layout.n_recur) as usize
        + layout.seq_heads as usize * layout.n_attn as usize;
    if rebuilt.trace_root != r.trace_root
        || rebuilt.tokens_root != r.tokens_root
        || s.leaves.len() != n
        || keryx_verifiable_inference::merkle::b3_root(&s.leaves) != s.root
    {
        return Err("corrupt persisted trace roots".into());
    }
    Ok(keryx_inference::TraceCommitment {
        trace_root: seq::seq_response_ctx(r, s).trace_root,
        n_prompt: r.n_prompt,
        n_pos: r.n_pos,
        stage: p::STAGE_CHAIN,
        tokens_root: r.tokens_root,
    })
}
fn validate_response(secret: &Hash, payload: &[u8], c: &Capture) -> Result<(), String> {
    let r = keryx_inference::AiResponsePayload::deserialize(payload).ok_or("invalid response")?;
    if r.request_hash != c.request || r.trace != Some(trace_commit(c)?) {
        return Err("response/trace binding mismatch".into());
    }
    let responder = r.responder.as_ref().ok_or("unsigned response")?;
    let (expected, _) = crate::sign_digest(secret, b"audit identity").map_err(|_| "signer")?;
    if responder.escrow_pubkey != expected {
        return Err("audit signer changed".into());
    }
    let pk = secp256k1::XOnlyPublicKey::from_slice(&expected).map_err(|_| "signer")?;
    let sig =
        secp256k1::schnorr::Signature::from_slice(&responder.signature).map_err(|_| "signature")?;
    let mut h = blake2b_simd::Params::new().hash_length(32).to_state();
    h.update(b"KeryxServiceResponderV1");
    h.update(&r.signed_bytes());
    let digest = h.finalize();
    let message =
        secp256k1::Message::from_digest_slice(digest.as_bytes()).map_err(|_| "signature digest")?;
    secp256k1::Secp256k1::verification_only()
        .verify_schnorr(&sig, &message, &pk)
        .map_err(|_| "response signature".into())
}
impl Service {
    pub fn open(
        state_path: &Path,
        secret: Hash,
        tier: u8,
        binding: Binding,
    ) -> Result<Self, String> {
        if tier > 4 {
            return Err("unsupported audit tier".into());
        }
        let store = store::TraceStore::new(state_path).map_err(|e| e.to_string())?;
        let dir = state_path.with_extension("audit-traces");
        let mut pending = BTreeMap::new();
        let mut restore = BTreeMap::<Hash, BTreeSet<Hash>>::new();
        let marker = dir.join("native-owner");
        if !marker.exists() {
            if fs::read_dir(&dir).map_err(|e| e.to_string())?.any(|e| {
                e.ok().is_some_and(|e| {
                    e.path()
                        .extension()
                        .is_some_and(|x| x == "trace" || x == "audit")
                })
            }) {
                return Err("unowned audit namespace; migration/reconciliation required".into());
            }
            let mut marker_file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&marker)
                .map_err(|e| e.to_string())?;
            marker_file
                .write_all(b"NXH15AUDIT1")
                .and_then(|_| marker_file.sync_all())
                .map_err(|e| e.to_string())?;
            #[cfg(unix)]
            fs::File::open(&dir)
                .and_then(|f| f.sync_all())
                .map_err(|e| e.to_string())?;
        } else {
            let m = fs::symlink_metadata(&marker).map_err(|e| e.to_string())?;
            if !m.is_file()
                || m.len() != 11
                || fs::read(&marker).map_err(|e| e.to_string())? != b"NXH15AUDIT1"
            {
                return Err("invalid audit namespace marker".into());
            }
        }
        let mut trace_ids = BTreeSet::new();
        for t in store
            .load_filtered(|id| {
                let path = dir.join(format!("{id}.audit"));
                if !path.exists() {
                    return Ok(false);
                }
                let state = read_state(&path).map_err(std::io::Error::other)?;
                Ok(!state.retired)
            })
            .map_err(|e| e.to_string())?
        {
            let response: Hash = hex::decode(&t.response_txid)
                .map_err(|_| "bad response id")?
                .try_into()
                .map_err(|_| "bad response id")?;
            trace_ids.insert(t.response_txid.clone());
            let path = dir.join(format!("{}.audit", t.response_txid));
            let meta = match fs::symlink_metadata(&path) {
                Ok(m) => m,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => continue,
                Err(e) => return Err(e.to_string()),
            };
            if !meta.is_file() || meta.len() > 4 << 20 || pending.len() >= 1024 {
                return Err("audit metadata bounds".into());
            }
            let mut state = read_state(&path)?;
            let capture = Capture {
                tier: t.tier,
                request: t.request_hash,
                max_tokens: t.max_tokens,
                gen_secs: t.gen_secs.unwrap_or(0.0),
                trace: t.trace,
                plaintext: Vec::new(),
            };
            if !matches!(state.version, 2..=4)
                || state.response != response
                || state.request != capture.request
                || capture.tier != tier
                || state.binding != binding
                || t.model_id != binding.model
            {
                return Err("audit metadata binding".into());
            }
            if state.policy.unaudited && state.policy.node.is_none() {
                state.policy.unaudited = false;
                state.policy.deadline = None;
            }
            state.response_last_ms = 0;
            validate_response(&secret, &state.response_payload, &capture)?;
            verify_pieces(&secret, &binding, &state, &capture)?;
            for piece in &mut state.pieces {
                if audit_txid(&piece.payload) != piece.id
                    || piece.payload.len() > opening::MAX_PAYLOAD_LEN
                {
                    return Err("corrupt stored opening".into());
                }
                piece.last_ms = 0;
            }
            if !state.retired {
                let anchor = state
                    .pieces
                    .iter()
                    .rev()
                    .find_map(|p| p.accepted.map(|a| a.0))
                    .or(state.accepted.map(|a| a.0))
                    .unwrap_or(state.anchor);
                restore.entry(anchor).or_default().insert(response);
            }
            if !state.pieces.is_empty() {
                let _ = writeln!(
                    std::io::stderr(),
                    "H15_AUDIT event=RESTORED_OPENING response={} pieces={} submission={} canonical_acceptance=SEPARATE",
                    hex::encode(response),
                    state.pieces.len(),
                    if state.pieces.iter().any(|piece| piece.attempts > 0) {
                        "PREVIOUS_INTENT"
                    } else {
                        "NOT_ATTEMPTED"
                    }
                );
            }
            pending.insert(response, Pending { state, capture });
        }
        for e in fs::read_dir(&dir).map_err(|e| e.to_string())? {
            let p = e.map_err(|e| e.to_string())?.path();
            if p.extension().is_some_and(|x| x == "audit") {
                let state = read_state(&p)?;
                let stem = p
                    .file_stem()
                    .and_then(|x| x.to_str())
                    .ok_or("invalid audit name")?;
                if state.binding != binding || stem != hex::encode(state.response) {
                    return Err("audit metadata profile binding".into());
                }
                if !dir.join(format!("{stem}.trace")).is_file() {
                    return Err("audit metadata lacks capture; preserve and reconcile".into());
                }
            }
        }
        Ok(Self {
            dir,
            store,
            next_span: 0,
            node_supported: false,
            awaited_polled: Vec::new(),
            node_awaits: BTreeMap::new(),
            pending,
            chain: Vec::new(),
            restore,
            secret,
            tier,
            binding,
        })
    }
    pub fn commit(
        &mut self,
        c: Capture,
        payload: Vec<u8>,
        submitted: u64,
        anchor: Hash,
        response_deadline: u64,
    ) -> Result<Hash, String> {
        validate_response(&self.secret, &payload, &c)?;
        if response_deadline < submitted {
            return Err("response deadline precedes signed response".into());
        }
        if self.response_for_request(c.request).is_some() {
            return Err("request already has a durable signed response".into());
        }
        if self.pending.len() >= 1024
            || self
                .pending
                .values()
                .map(|p| p.capture.trace.record.leaf_hashes().len() * 32)
                .sum::<usize>()
                .saturating_add(c.trace.record.leaf_hashes().len() * 32)
                > 1024 * 1024 * 1024
            || c.tier != self.tier
        {
            return Err("audit capacity or tier".into());
        }
        crate::trace_speeds::note_generation(
            &self.binding.model,
            c.trace.record.n_prompt,
            c.trace.record.n_pos.saturating_sub(c.trace.record.n_prompt),
            c.gen_secs,
        );
        let response = id(&payload, 4);
        if self.pending.contains_key(&response) {
            return Err("duplicate audit capture".into());
        }
        let state = State {
            version: 4,
            binding: self.binding.clone(),
            response,
            request: c.request,
            response_payload: payload,
            submitted,
            anchor,
            accepted: None,
            seed: None,
            pieces: Vec::new(),
            cached: Vec::new(),
            failures: 0,
            retry_after: 0,
            retired: false,
            response_deadline,
            response_attempts: 0,
            response_last_ms: 0,
            response_rejected: false,
            policy: AuditPolicy::default(),
        };
        // Persist capture before an AiResponse SUBMITTING intent can reach the node.
        let trace = Arc::new(c.trace);
        self.store
            .save_queued(store::StoredTraceOwned {
                response_txid: hex::encode(response),
                model_id: self.binding.model,
                tier: c.tier,
                request_hash: c.request,
                max_tokens: c.max_tokens,
                submitted_daa: submitted,
                accepted_hash: None,
                opening: None,
                trace: trace.clone(),
                attn: None,
                anchor_hash: Some(hex::encode(anchor)),
                gen_secs: Some(c.gen_secs),
            })
            .map_err(|e| e.to_string())?;
        self.store.flush().map_err(|e| e.to_string())?;
        persist(&self.dir, &response, &state)?;
        let trace =
            Arc::try_unwrap(trace).map_err(|_| "audit writer retained trace after flush")?;
        self.pending.insert(
            response,
            Pending {
                state,
                capture: Capture { trace, ..c },
            },
        );
        self.restore.entry(anchor).or_default().insert(response);
        Ok(response)
    }
    pub fn anchors(&self) -> Vec<Hash> {
        if !self.has_work() {
            return Vec::new();
        }
        if !self.restore.is_empty() {
            return self.restore.keys().copied().collect();
        }
        if let Some(last) = self.chain.last() {
            return vec![last.hash];
        }
        self.pending
            .values()
            .filter(|p| !p.state.retired)
            .map(|p| p.state.anchor)
            .collect()
    }
    pub fn note_anchor(&mut self, b: ChainBlock) -> Result<(), String> {
        if b.daa == 0 {
            return Err("invalid anchor DAA".into());
        }
        if let Some(old) = self.chain.iter().find(|a| a.hash == b.hash) {
            if old.daa != b.daa {
                return Err("conflicting anchor DAA".into());
            }
        } else {
            self.chain.push(b);
        }
        Ok(())
    }
    pub fn has_work(&self) -> bool {
        self.pending.values().any(|p| !p.state.retired)
    }
    pub fn chain_page(
        &mut self,
        start: Hash,
        removed: &[Hash],
        added: Vec<ChainBlock>,
        last_page: bool,
    ) -> Result<(), String> {
        if added.len() > 10000 || removed.len() > 72000 {
            return Err("chain page bounds".into());
        }
        let unique: BTreeSet<_> = added.iter().map(|b| b.hash).collect();
        if unique.len() != added.len()
            || unique.contains(&start)
            || added.iter().any(|b| b.daa == 0 || b.accepted.len() > 50000)
        {
            return Err("invalid canonical chain page".into());
        }
        if added.windows(2).any(|w| w[0].daa > w[1].daa) {
            return Err("chain DAA order".into());
        }
        let removed: BTreeSet<_> = removed.iter().copied().collect();
        self.chain
            .retain(|b| !removed.contains(&b.hash) && !unique.contains(&b.hash));
        let at = self
            .chain
            .iter()
            .position(|b| b.hash == start)
            .map_or(self.chain.len(), |i| i + 1);
        self.chain.splice(at..at, added.clone());
        let origins = self.restore.remove(&start).unwrap_or_else(|| {
            self.pending
                .iter()
                .filter(|(_, p)| !p.state.retired)
                .map(|(id, _)| *id)
                .collect()
        });
        if !last_page {
            if let Some(b) = added.last() {
                self.restore.entry(b.hash).or_default().extend(origins);
            }
        }
        for (id, p) in &mut self.pending {
            if p.state.retired {
                continue;
            }
            if p.state.accepted.is_some_and(|(h, _)| removed.contains(&h))
                || p.state.seed.is_some_and(|(h, _)| removed.contains(&h))
            {
                if p.state.accepted.is_some_and(|(h, _)| removed.contains(&h)) {
                    p.state.accepted = None;
                }
                self.node_awaits.remove(id);
                p.state.seed = None;
                p.state.policy.node = None;
                p.state.policy.own_deadline = None;
                p.state.policy.deadline = None;
                p.state.policy.unaudited = false;
                p.state.policy.replay_retry_ms = 0;
                p.state.cached.append(&mut p.state.pieces);
                if p.state.cached.len() > 12 {
                    let n = p.state.cached.len() - 12;
                    p.state.cached.drain(..n);
                }
                p.state.failures = 0;
            }
            let affected = p
                .state
                .pieces
                .iter()
                .filter_map(|x| {
                    if x.draw_block.is_some_and(|h| removed.contains(&h)) {
                        Some((x.path.len(), true))
                    } else if x.accepted.is_some_and(|(h, _)| removed.contains(&h)) {
                        Some((x.path.len(), false))
                    } else {
                        None
                    }
                })
                .min_by_key(|(n, drop)| (*n, !*drop));
            if let Some((depth, drop_this)) = affected {
                self.node_awaits.remove(id);
                let (keep, drop): (Vec<_>, Vec<_>) = p.state.pieces.drain(..).partition(|x| {
                    x.kind != 1
                        || if drop_this {
                            x.path.len() < depth
                        } else {
                            x.path.len() <= depth
                        }
                });
                p.state.pieces = keep;
                p.state.cached.extend(drop);
                if p.state.cached.len() > 12 {
                    let n = p.state.cached.len() - 12;
                    p.state.cached.drain(..n);
                }
                for x in &mut p.state.pieces {
                    if x.accepted.is_some_and(|(h, _)| removed.contains(&h)) {
                        x.accepted = None;
                        x.last_ms = 0;
                    }
                }
            }
            if p.state
                .policy
                .request_accepted
                .is_some_and(|h| removed.contains(&h))
            {
                p.state.policy.request_accepted = None;
                p.state.policy.early_after_ms = None;
                p.state.policy.early_sends = 0;
            }
            for b in &added {
                if b.accepted.contains(&p.state.request)
                    && p.state.accepted.is_some_and(|(h, _)| h != b.hash)
                    && p.state.policy.request_accepted != Some(b.hash)
                {
                    p.state.policy.request_accepted = Some(b.hash);
                    p.state.policy.early_after_ms = Some(wall_ms().saturating_add(125000));
                    p.state.policy.early_sends = 0;
                }
                if b.accepted.contains(id) {
                    p.state.accepted = Some((b.hash, b.daa));
                }
                for piece in &mut p.state.pieces {
                    if b.accepted.contains(&piece.id) {
                        piece.accepted = Some((b.hash, b.daa));
                    }
                }
            }
            persist(&self.dir, id, &p.state)?;
        }
        if self.chain.len() > 72000 {
            return Err("canonical audit chain exceeds bounded horizon".into());
        }
        Ok(())
    }
    pub fn awaited_ids(&mut self, now: u64) -> Vec<Hash> {
        self.node_supported = true;
        let mut due: Vec<_> = self
            .pending
            .iter()
            .filter(|(_, p)| {
                !p.state.retired
                    && p.state.accepted.is_some()
                    && p.state.policy.deadline.is_none_or(|d| {
                        now <= d.saturating_add(p::OPENING_BACKLOG_CAP_DAA)
                            || unfinished(&p.state.pieces)
                    })
            })
            .map(|(id, p)| (p.state.policy.deadline.unwrap_or(0), *id))
            .collect();
        due.sort();
        self.awaited_polled = due.into_iter().take(1024).map(|(_, id)| id).collect();
        self.awaited_polled.clone()
    }
    pub fn local_estimates(&mut self) -> Result<(), String> {
        self.node_supported = false;
        self.awaited_polled.clear();
        self.node_awaits.clear();
        for (id, p) in &mut self.pending {
            if p.state.policy.node.take().is_some() {
                p.state.policy.deadline = None;
                p.state.policy.unaudited = false;
                persist(&self.dir, id, &p.state)?;
            }
        }
        Ok(())
    }
    pub fn apply_awaited(&mut self, frame: AwaitedFrame) -> Result<(), String> {
        if frame.openings.len() > 1024 || frame.daa == 0 {
            return Err("node ledger frame bounds".into());
        }
        let signer = secp256k1::Keypair::from_secret_key(
            &secp256k1::Secp256k1::new(),
            &secp256k1::SecretKey::from_slice(&self.secret).map_err(|_| "invalid signer")?,
        )
        .x_only_public_key()
        .0
        .serialize();
        let mut seen = BTreeSet::new();
        for o in &frame.openings {
            if !seen.insert(o.response)
                || o.path.len() > 16
                || o.path
                    .iter()
                    .any(|&c| c >= keryx_verifiable_inference::shape::MAX_TRACE_POSITIONS as u64)
                || o.pieces > 15
                || o.deadline == 0
                || o.accepted_daa == 0
                || o.accepted_daa > frame.daa
            {
                return Err("node ledger row bounds".into());
            }
            if self.pending.contains_key(&o.response) && o.signer != signer {
                return Err("node ledger signer mismatch".into());
            }
        }
        // Only a completely validated successful response can release queried duties.
        // An omitted ID outside this exact request conveys no information.
        self.node_supported = true;
        for id in std::mem::take(&mut self.awaited_polled) {
            self.node_awaits.insert(id, seen.contains(&id));
        }
        for o in frame.openings {
            self.node_awaits.insert(o.response, true);
            let Some(p) = self.pending.get_mut(&o.response) else {
                continue;
            };
            if p.state.retired || p.state.accepted.is_none() {
                continue;
            }
            let same = p
                .state
                .seed
                .filter(|(h, _)| p::trace_seed(h, &o.response) == o.seed);
            let seed = same.or_else(|| {
                self.chain
                    .iter()
                    .find(|b| {
                        b.daa >= o.accepted_daa && p::trace_seed(&b.hash, &o.response) == o.seed
                    })
                    .map(|b| (b.hash, b.daa))
            });
            let Some(seed) = seed else { continue }; // canonical coverage must supply the seed block
            let had_draw = p.state.seed.is_some();
            let changed_seed = p.state.seed != Some(seed);
            if changed_seed {
                p.state.cached.append(&mut p.state.pieces);
                if p.state.cached.len() > 12 {
                    p.state.cached.drain(..p.state.cached.len() - 12);
                }
                p.state.seed = Some(seed);
                p.state.failures = 0;
                p.state.policy.replay_retry_ms = 0;
            }
            p.state.policy.unaudited = false;
            p.state.accepted = p.state.accepted.map(|(h, _)| (h, o.accepted_daa));
            p.state.policy.deadline = Some(o.deadline);
            for piece in &mut p.state.pieces {
                if o.pieces & (1u32 << piece.kind) != 0 && (piece.kind != 1 || piece.path == o.path)
                {
                    piece.deadline = o.deadline;
                    if piece.accepted.take().is_some() {
                        piece.last_ms = 0;
                    }
                }
            }
            p.state.policy.node = Some(o);
            persist(&self.dir, &p.state.response, &p.state)?;
            if changed_seed {
                let _ = writeln!(
                    std::io::stderr(),
                    "H15_AUDIT event={} response={} seed_block={} deadline_daa={}",
                    if had_draw {
                        "NODE_SEED_MISMATCH"
                    } else {
                        "NODE_SEEDED"
                    },
                    hex::encode(p.state.response),
                    hex::encode(seed.0),
                    p.state.policy.deadline.unwrap()
                );
            }
        }
        Ok(())
    }
    fn replay_work(&self, p: &Pending, phase: bool) -> u64 {
        let r = &p.capture.trace.record;
        let generated = r.n_pos.saturating_sub(r.n_prompt);
        crate::trace_speeds::replay_daa(
            &self.binding.model,
            phase,
            Some(p.capture.gen_secs),
            generated,
        )
        .unwrap_or_else(|| {
            p::work_daa(
                p.capture.tier,
                r.n_prompt,
                generated,
                if phase { 2 } else { 3 },
            )
        })
    }
    fn resolve_deadlines(&mut self) -> Result<(), String> {
        let mut fresh = Vec::new();
        for (id, p) in &mut self.pending {
            if p.state.seed.is_none() {
                if let Some((h, daa)) = p.state.accepted {
                    p.state.seed = Self::draw_after(&self.chain, h, daa);
                }
            }
            if let Some((_, daa)) = p.state.seed {
                if p.state.policy.deadline.is_none() && !p.state.policy.unaudited {
                    fresh.push((daa, *id));
                }
            }
        }
        fresh.sort();
        let mut open: Vec<_> =
            self.pending
                .values()
                .filter(|p| !p.state.retired && unfinished(&p.state.pieces))
                .filter_map(|p| {
                    p.state.policy.deadline.map(|d| {
                        d.max(p.state.pieces.iter().map(|x| x.deadline).max().unwrap_or(d))
                    })
                })
                .collect();
        for (daa, id) in fresh {
            let p = self.pending.get_mut(&id).unwrap();
            let r = &p.capture.trace.record;
            let own = daa
                .saturating_add(p::opening_window_daa(p.capture.tier, p.capture.max_tokens))
                .saturating_add(p::attn_first_prompt_daa(r.n_prompt));
            let work = p::service_work_daa(
                p.capture.tier,
                r.n_prompt,
                r.n_pos.saturating_sub(r.n_prompt),
            );
            p.state.policy.own_deadline = Some(own);
            p.state.policy.deadline = Some(
                p::chained_deadline(own, &open, daa, work)
                    .ok_or("audit deadline estimate overflow")?,
            );
            p.state.policy.unaudited = false;
            if let Some(d) = p.state.policy.deadline {
                open.push(d);
            }
            persist(&self.dir, &id, &p.state)?;
        }
        Ok(())
    }
    fn draw_after(chain: &[ChainBlock], hash: Hash, daa: u64) -> Option<(Hash, u64)> {
        let at = chain.iter().position(|b| b.hash == hash)?;
        chain
            .iter()
            .skip(at + 1)
            .find(|b| b.daa >= daa.saturating_add(p::TRACE_SEED_DELAY_DAA))
            .map(|b| (b.hash, b.daa))
    }
    /// Produce at most one replay task; requests only yield when its deadline is urgent.
    /// `build` owns GPU exclusion. Its result cannot authorize effects without persistence.
    pub fn advance(
        &mut self,
        now: u64,
        next_work: Option<u64>,
        mut build: impl FnMut(
            &Capture,
            Hash,
            Hash,
            &[u64],
            Option<&[u8]>,
        ) -> Result<Vec<Vec<u8>>, String>,
    ) -> Result<(), String> {
        self.prune(now)?;
        if !self.restore.is_empty() {
            return Ok(());
        }
        let ms = wall_ms();
        self.resolve_deadlines()?;
        let mut tasks = Vec::new();
        for (id, pending) in &mut self.pending {
            let s = &mut pending.state;
            if s.retired || s.policy.unaudited || ms < s.policy.replay_retry_ms {
                continue;
            }
            if s.seed.is_none() {
                if let Some((h, daa)) = s.accepted {
                    s.seed = Self::draw_after(&self.chain, h, daa);
                }
            }
            let Some((seed, seed_daa)) = s.seed else {
                if !recovery::retain_undrawn(now, s.submitted, s.accepted.is_some(), 72000) {
                    s.retired = true;
                    persist(&self.dir, id, s)?;
                }
                continue;
            };
            let first_deadline = s.policy.deadline.unwrap_or_else(|| {
                seed_daa
                    .saturating_add(p::opening_window_daa(
                        pending.capture.tier,
                        pending.capture.max_tokens,
                    ))
                    .saturating_add(p::attn_first_prompt_daa(
                        pending.capture.trace.record.n_prompt,
                    ))
            });
            if s.pieces.is_empty() {
                if self.node_supported
                    && first_deadline <= now
                    && self.node_awaits.get(id) != Some(&true)
                {
                    continue;
                }
                tasks.push((first_deadline, *id, seed, Vec::new(), None, None));
                continue;
            }
            let last = s
                .pieces
                .iter()
                .filter(|p| p.kind == 1)
                .max_by_key(|p| p.path.len())
                .ok_or("missing attention obligation")?;
            let signed =
                opening::SignedPayload::split(&last.payload).ok_or("stored signature shape")?;
            let body = opening::inner_body(signed.body).ok_or("stored seeded body")?;
            let op = opening::AttnOpening::decode(&body).ok_or("stored attention body")?;
            if op.partials.is_empty() {
                continue;
            }
            let Some((h, daa)) = last.accepted else {
                continue;
            };
            let Some((child, child_daa)) = Self::draw_after(&self.chain, h, daa) else {
                continue;
            };
            let d = seq::draw_attn(
                &p::TierManifest::for_attn(pending.capture.tier).unwrap(),
                &pending.capture.trace.record,
                &p::trace_seed(&seed, id),
            )
            .ok_or("attention draw")?;
            let parts =
                attn_blocks::decode_partials(&op.partials, d.dv).ok_or("stored partials")?;
            let weights = attn_blocks::child_weights(&parts).ok_or("stored partial weights")?;
            let mut path = last.path.clone();
            path.push(
                p::draw_attention_child(&child, id, last.path.len() as u8, &weights)
                    .ok_or("child draw")?,
            );
            if s.policy
                .node
                .as_ref()
                .is_some_and(|o| !o.path.is_empty() && o.path != path)
            {
                continue;
            }
            let deadline = child_daa.saturating_add(p::attn_phase_window_daa(
                pending.capture.tier,
                pending.capture.max_tokens,
                pending.capture.trace.record.n_prompt,
                pending.capture.trace.record.n_pos,
            ));
            let deadline = s
                .policy
                .node
                .as_ref()
                .filter(|o| o.path == path)
                .map_or(deadline, |o| o.deadline);
            if self.node_supported && deadline <= now && self.node_awaits.get(id) != Some(&true) {
                continue;
            }
            tasks.push((deadline, *id, seed, path, Some(op.partials), Some(child)));
        }
        tasks.sort_by_key(|t| (t.0 <= now, t.0, t.1));
        if let Some(next) = next_work {
            let mut jobs: Vec<_> = tasks
                .iter()
                .filter(|t| t.0 > now)
                .map(|t| {
                    let p = &self.pending[&t.1];
                    (t.0, self.replay_work(p, !t.3.is_empty()), true)
                })
                .collect();
            for p in self.pending.values().filter(|p| {
                p.state.accepted.is_some() && p.state.seed.is_none() && !p.state.retired
            }) {
                let due = p
                    .state
                    .submitted
                    .saturating_add(p::TRACE_SEED_DELAY_DAA)
                    .saturating_add(p::opening_window_daa(p.capture.tier, p.capture.max_tokens));
                if due > now {
                    jobs.push((due, self.replay_work(p, false), false));
                }
            }
            let chain_end = self
                .pending
                .values()
                .filter(|p| !p.state.retired && unfinished(&p.state.pieces))
                .filter_map(|p| p.state.policy.deadline)
                .max();
            let span_urgent = jobs.iter().any(|j| j.2)
                && chain_end.is_some_and(|end| {
                    end.saturating_add(self.next_span)
                        > now.saturating_add(next).saturating_add(270000)
                });
            if !span_urgent && !recovery::replays_urgent(now, next, &jobs) {
                return Ok(());
            }
        }
        if let Some((deadline, id, seed, path, prev, draw_block)) =
            tasks.into_iter().find(|t| next_work.is_none() || t.0 > now)
        {
            let p = self.pending.get_mut(&id).unwrap();
            if now > deadline.saturating_add(p::OPENING_BACKLOG_CAP_DAA) {
                return Ok(());
            }
            let kinds: Vec<u8> = if prev.is_some() {
                vec![1]
            } else if p::TierManifest::for_attn(p.capture.tier)
                .unwrap()
                .chain
                .unwrap()
                .n_recur
                > 0
            {
                vec![0, 1, 2, 3]
            } else {
                vec![0, 1, 3]
            };
            let cached: Option<Vec<Vec<u8>>> = kinds
                .iter()
                .map(|kind| {
                    p.state
                        .cached
                        .iter()
                        .find(|old| {
                            old.kind == *kind
                                && old.path == path
                                && opening::SignedPayload::split(&old.payload)
                                    .and_then(|s| opening::seed_block(s.body))
                                    == Some(seed)
                        })
                        .and_then(|old| {
                            opening::SignedPayload::split(&old.payload)
                                .and_then(|s| opening::inner_body(s.body))
                        })
                })
                .collect();
            let bodies = match cached.map(Ok).unwrap_or_else(|| {
                let start = std::time::Instant::now();
                let result = build(&p.capture, seed, id, &path, prev.as_deref());
                if result.is_ok() {
                    crate::trace_speeds::note_replay(
                        &self.binding.model,
                        !path.is_empty(),
                        start.elapsed().as_secs_f64() / p.capture.gen_secs,
                    );
                }
                result
            }) {
                Ok(b) => b,
                Err(e) => {
                    p.state.failures = p.state.failures.saturating_add(1);
                    p.state.policy.replay_retry_ms =
                        ms.saturating_add(recovery::replay_backoff_ms(p.state.failures));
                    persist(&self.dir, &id, &p.state)?;
                    log::warn!(
                        "H15 replay failed; immutable duty retained for bounded-backoff retry: {}",
                        e
                    );
                    return Ok(());
                }
            };
            if bodies.len() != kinds.len() {
                return Err("invalid native opening bundle".into());
            }
            for (i, body) in bodies.into_iter().enumerate() {
                if !p::opening_fits_seeded(body.len()) || body.get(1..33) != Some(id.as_slice()) {
                    return Err("opening binding/cap".into());
                }
                let kind = kinds[i];
                let seeded = opening::seeded(&body, &seed).ok_or("seed envelope")?;
                let payload = signing(&self.secret, &seeded)?;
                let tx = audit_txid(&payload);
                p.state.pieces.push(Piece {
                    payload,
                    id: tx,
                    path: path.clone(),
                    kind,
                    draw_block,
                    accepted: self
                        .chain
                        .iter()
                        .find(|b| b.accepted.contains(&tx))
                        .map(|b| (b.hash, b.daa)),
                    deadline,
                    attempts: 0,
                    last_ms: 0,
                });
            }
            verify_pieces(&self.secret, &self.binding, &p.state, &p.capture)?;
            p.state.failures = 0;
            p.state.retry_after = 0;
            p.state.policy.replay_retry_ms = 0;
            persist(&self.dir, &id, &p.state)?;
        }
        Ok(())
    }
    /// Durable attempt intent; returns only immutable signed bytes eligible for
    /// reconciliation/submission. RPC ACK never advances an attention phase.
    pub fn due(&mut self, now_daa: u64, now_ms: u64) -> Result<Vec<Vec<u8>>, String> {
        if !self.restore.is_empty() {
            return Ok(Vec::new());
        }
        let mut out = Vec::new();
        for (id, p) in &mut self.pending {
            if p.state.retired {
                continue;
            }
            let mut changed = false;
            for x in &mut p.state.pieces {
                let node_allows = p.state.policy.node.as_ref().is_none_or(|o| {
                    o.pieces & (1u32 << x.kind) != 0
                        && (x.kind != 1 || x.path == o.path)
                        && opening::SignedPayload::split(&x.payload)
                            .and_then(|s| opening::seed_block(s.body))
                            .is_some_and(|seed| p::trace_seed(&seed, id) == o.seed)
                });
                if node_allows
                    && out.len() < 32
                    && x.accepted.is_none()
                    && now_daa <= x.deadline.saturating_add(p::OPENING_BACKLOG_CAP_DAA)
                    && (x.last_ms == 0
                        || now_ms.saturating_sub(x.last_ms)
                            >= if x.attempts < 10 { 3000 } else { 60000 })
                {
                    changed = true;
                    x.attempts = x.attempts.saturating_add(1);
                    x.last_ms = now_ms;
                    out.push(x.payload.clone());
                }
            }
            if changed {
                persist(&self.dir, id, &p.state)?;
            }
        }
        Ok(out)
    }

    pub fn response_intent(&mut self, response: Hash, ms: u64) -> Result<bool, String> {
        let Some(p) = self.pending.get_mut(&response) else {
            return Ok(false);
        };
        if p.state.response_attempts >= 100 {
            return Err("response retry budget exhausted; signed bytes retained".into());
        }
        p.state.policy.last_response_was_early = false;
        p.state.response_attempts += 1;
        p.state.response_last_ms = ms;
        p.state.response_rejected = false;
        persist(&self.dir, &response, &p.state)?;
        Ok(true)
    }
    pub fn response_result(&mut self, response: Hash, rejected: bool) -> Result<(), String> {
        if let Some(p) = self.pending.get_mut(&response) {
            p.state.response_rejected = rejected;
            persist(&self.dir, &response, &p.state)?;
        }
        Ok(())
    }
    pub fn response_due(&mut self, now: u64, ms: u64) -> Result<Vec<Vec<u8>>, String> {
        if !self.restore.is_empty() {
            return Ok(Vec::new());
        }
        let ids: Vec<_> = self
            .pending
            .iter()
            .filter(|(_, p)| {
                let st = &p.state;
                !st.retired
                    && !st.response_rejected
                    && if st.accepted.is_some() {
                        st.policy.early_sends < 10
                            && st.policy.early_after_ms.is_some_and(|at| ms >= at)
                    } else {
                        st.response_attempts < 100
                            && now <= st.response_deadline
                            && (st.response_last_ms == 0
                                || ms.saturating_sub(st.response_last_ms) >= 3000)
                    }
            })
            .map(|(id, _)| *id)
            .take(32)
            .collect();
        let mut out = Vec::new();
        for id in ids {
            if self.pending[&id].state.accepted.is_some() {
                let st = &mut self.pending.get_mut(&id).unwrap().state;
                st.policy.early_sends += 1;
                st.policy.last_response_was_early = true;
                st.policy.early_after_ms = Some(ms.saturating_add(60000));
                persist(&self.dir, &id, st)?;
            } else {
                self.response_intent(id, ms)?;
            }
            out.push(self.pending[&id].state.response_payload.clone());
        }
        Ok(out)
    }
    pub fn response_for_request(&self, request: Hash) -> Option<Hash> {
        self.pending
            .iter()
            .find(|(_, p)| p.state.request == request && !p.state.retired)
            .map(|(id, _)| *id)
    }
    pub fn refund_unsent(&mut self, ids: &[Hash]) -> Result<(), String> {
        let ids: BTreeSet<_> = ids.iter().copied().collect();
        if ids.len() > 32 {
            return Err("unsent bound".into());
        }
        let mut found = BTreeSet::new();
        for (id, p) in &mut self.pending {
            if ids.contains(id) {
                if p.state.policy.last_response_was_early {
                    p.state.policy.early_sends = p.state.policy.early_sends.saturating_sub(1);
                } else {
                    p.state.response_attempts = p.state.response_attempts.saturating_sub(1);
                }
                found.insert(*id);
            }
            for piece in &mut p.state.pieces {
                if ids.contains(&piece.id) {
                    piece.attempts = piece.attempts.saturating_sub(1);
                    found.insert(piece.id);
                }
            }
            persist(&self.dir, id, &p.state)?;
        }
        if found != ids {
            return Err("unknown unsent piece".into());
        }
        Ok(())
    }
    pub fn prune(&mut self, now: u64) -> Result<(), String> {
        for (id, pending) in &mut self.pending {
            let state = &mut pending.state;
            if state.retired {
                continue;
            }
            let limit = if state.policy.unaudited {
                state
                    .seed
                    .map_or(state.submitted, |s| s.1)
                    .saturating_add(600)
            } else if state.seed.is_none() {
                state.submitted.saturating_add(72000)
            } else {
                let initial = state
                    .seed
                    .unwrap()
                    .1
                    .saturating_add(p::opening_window_daa(
                        pending.capture.tier,
                        pending.capture.max_tokens,
                    ))
                    .saturating_add(p::attn_first_prompt_daa(
                        pending.capture.trace.record.n_prompt,
                    ));
                state
                    .pieces
                    .iter()
                    .map(|x| x.deadline)
                    .max()
                    .unwrap_or(state.policy.deadline.unwrap_or(initial))
                    .saturating_add(if unfinished(&state.pieces) {
                        p::OPENING_BACKLOG_CAP_DAA
                    } else {
                        200
                    })
            };
            let deadline = state
                .policy
                .deadline
                .into_iter()
                .chain(state.pieces.iter().map(|p| p.deadline))
                .max();
            let awaiting_child = state
                .pieces
                .iter()
                .filter(|p| p.kind == 1)
                .max_by_key(|p| p.path.len())
                .is_some_and(|p| {
                    p.accepted.is_some_and(|(h, daa)| {
                        unfinished(std::slice::from_ref(p))
                            && Self::draw_after(&self.chain, h, daa).is_none()
                    })
                });
            let released = self.node_awaits.get(id) == Some(&false)
                && deadline.is_some_and(|d| now > d)
                && !awaiting_child;
            if released || now > limit {
                state.retired = true;
                persist(&self.dir, id, state)?;
                log::warn!("H15 audit expired; durable record retained");
            }
        }
        self.pending.retain(|_, p| !p.state.retired);
        self.node_awaits
            .retain(|id, _| self.pending.contains_key(id));
        if self.pending.is_empty() {
            self.chain.clear();
        } else {
            self.chain.retain(|b| b.daa >= now.saturating_sub(72000));
        }
        let active: BTreeSet<_> = self.pending.keys().copied().collect();
        self.restore.retain(|_, ids| {
            ids.retain(|id| active.contains(id));
            !ids.is_empty()
        });
        Ok(())
    }
    pub fn flush(&self) -> Result<(), String> {
        self.store.flush().map_err(|e| e.to_string())
    }
}

#[derive(BorshSerialize, BorshDeserialize)]
pub struct ChainFrame {
    pub start: ChainBlock,
    pub removed: Vec<Hash>,
    pub added: Vec<ChainBlock>,
    pub last_page: bool,
}
use crate::h15_bridge::{guard, output, view, Api, Buffer};
use std::ffi::c_void;
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// The path spans `len` bytes; secret/model span 32 bytes each. `api` is a valid callback table
/// and `out` a writable handle slot. The returned service has one owner.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_open(
    path: *const u8,
    len: usize,
    secret: *const u8,
    tier: u8,
    api: *const Api,
    model: *const u8,
    out: *mut *mut c_void,
) -> i32 {
    guard(|| {
        if out.is_null() || api.is_null() {
            return Err("missing service output".into());
        }
        let path = std::str::from_utf8(unsafe { view(path, len, 8192) }?)
            .map_err(|_| "state path UTF8")?;
        let secret: Hash = unsafe { view(secret, 32, 32) }?.try_into().unwrap();
        let api = unsafe { &*api };
        let binding = Binding {
            model: unsafe { view(model, 32, 32) }?.try_into().unwrap(),
            root: api.model_root,
            chunks: api.chunks,
        };
        let service = Service::open(Path::new(path), secret, tier, binding)?;
        unsafe { *out = Box::into_raw(Box::new(service)).cast() };
        Ok(())
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// A non-null pointer must own an unreleased service created by `nexus_h15_service_open`; no
/// concurrent call or outstanding borrow may remain.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_free(ptr: *mut c_void) {
    if !ptr.is_null() {
        let s = unsafe { Box::from_raw(ptr as *mut Service) };
        let _ = s.flush();
        drop(s);
    }
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `c` must be a live capture for the call; payload spans `len` bytes and anchor spans 32
/// readable bytes. Neither may alias the mutably borrowed service.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_commit(
    ptr: *mut c_void,
    c: *mut c_void,
    payload: *const u8,
    len: usize,
    daa: u64,
    anchor: *const u8,
    deadline: u64,
) -> i32 {
    guard(|| {
        if ptr.is_null() || c.is_null() {
            return Err("missing native service".into());
        }
        let service = unsafe { &mut *(ptr as *mut Service) };
        let original = unsafe { &*(c as *const Capture) };
        let capture =
            Capture::try_from_slice(&borsh::to_vec(original).map_err(|_| "capture clone")?)
                .map_err(|_| "capture clone")?;
        service.commit(
            capture,
            unsafe { view(payload, len, opening::MAX_PAYLOAD_LEN) }?.to_vec(),
            daa,
            unsafe { view(anchor, 32, 32) }?.try_into().unwrap(),
            deadline,
        )?;
        Ok(())
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_local_estimates(ptr: *mut c_void) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        unsafe { &mut *(ptr as *mut Service) }.local_estimates()
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `out` must be a writable Buffer slot; release its previous allocation before overwriting it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_awaited_ids(
    ptr: *mut c_void,
    now: u64,
    out: *mut Buffer,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        unsafe { output(s.awaited_ids(now).into_iter().flatten().collect(), out) }
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_apply_awaited(
    ptr: *mut c_void,
    bytes: *const u8,
    len: usize,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let frame = AwaitedFrame::try_from_slice(unsafe { view(bytes, len, 1 << 20) }?)
            .map_err(|_| "node ledger frame encoding")?;
        unsafe { &mut *(ptr as *mut Service) }.apply_awaited(frame)
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_next_span(
    ptr: *mut c_void,
    prompt: u32,
    tokens: u32,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        let phase = p::TRACE_SEED_DELAY_DAA
            .saturating_add(p::attn_phase_window_daa(
                s.tier,
                tokens,
                prompt,
                prompt.saturating_add(tokens),
            ))
            .saturating_add(p::OPENING_BACKLOG_CAP_DAA);
        s.next_span = p::OPEN_AUDIT_WORK_PASSES
            .saturating_mul(p::service_work_daa(s.tier, prompt, tokens))
            .saturating_add(p::OPENING_BACKLOG_CAP_DAA)
            .saturating_add(3u64.saturating_mul(phase));
        Ok(())
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `out` must be a writable Buffer slot; release its previous allocation before overwriting it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_anchors(ptr: *mut c_void, out: *mut Buffer) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let s = unsafe { &*(ptr as *const Service) };
        unsafe { output(s.anchors().into_iter().flatten().collect(), out) }
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_chain(
    ptr: *mut c_void,
    data: *const u8,
    len: usize,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        let frame = ChainFrame::try_from_slice(unsafe { view(data, len, MAX_RECORD) }?)
            .map_err(|_| "invalid chain frame")?;
        let start = frame.start.hash;
        s.note_anchor(frame.start)?;
        s.chain_page(start, &frame.removed, frame.added, frame.last_page)
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// The API and callbacks must remain valid, cannot unwind or reenter the service, and must hold
/// authorized exclusive GPU ownership during replay.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_advance(
    ptr: *mut c_void,
    api: *const Api,
    now: u64,
    next_work: u64,
) -> i32 {
    guard(|| {
        if ptr.is_null() || api.is_null() {
            return Err("missing native service".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        let api = unsafe { &*api };
        s.advance(
            now,
            (next_work != u64::MAX).then_some(next_work),
            |c, seed, id, path, prev| {
                crate::h15_bridge::build(c, api, seed, id, path, prev.unwrap_or(&[]))
            },
        )
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `out` must be a writable Buffer slot; release its previous allocation before overwriting it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_due(
    ptr: *mut c_void,
    daa: u64,
    ms: u64,
    out: *mut Buffer,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        let due = s.due(daa, ms)?;
        unsafe { output(seq::bundle(&due), out) }
    })
}

/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_restoring(ptr: *mut c_void) -> i32 {
    if ptr.is_null() {
        return -1;
    }
    let s = unsafe { &*(ptr as *const Service) };
    i32::from(!s.restore.is_empty())
}
fn verify_pieces(
    secret: &Hash,
    binding: &Binding,
    state: &State,
    capture: &Capture,
) -> Result<(), String> {
    let m = p::TierManifest::for_attn(capture.tier).ok_or("unsupported manifest")?;
    let ctx = m.chain.ok_or("no chain")?;
    let tier = keryx_verifiable_inference::verify::TierCtx {
        r_t: binding.root,
        n_chunks: binding.chunks,
        m_t: m.m_t,
        n_entries: m.entries.len() as u64,
        n_products: m.n_products as u64,
    };
    let resp = seq::seq_response_ctx(
        &capture.trace.record,
        capture.trace.seq.as_ref().ok_or("no sequence")?,
    );
    let signer = crate::sign_digest(secret, b"audit identity")
        .map_err(|_| "signer")?
        .0;
    for piece in &state.pieces {
        let seed = state.seed.ok_or("opening lacks seed")?.0;
        let result = match piece.kind {
            0 => crate::verified_inference::verify_seeded_trace_opening(
                &piece.payload,
                &state.response,
                &signer,
                &tier,
                &resp,
                &seed,
                &p::chain_ctx(capture.tier).unwrap().1,
                &capture.request,
                &ctx,
            ),
            1 => {
                let parent = if piece.path.is_empty() {
                    None
                } else {
                    let prior = state
                        .pieces
                        .iter()
                        .find(|p| p.kind == 1 && p.path == piece.path[..piece.path.len() - 1])
                        .ok_or("phase parent missing")?;
                    let signed =
                        opening::SignedPayload::split(&prior.payload).ok_or("parent signature")?;
                    let b = opening::inner_body(signed.body).ok_or("parent body")?;
                    let op = opening::AttnOpening::decode(&b).ok_or("parent phase")?;
                    let d = seq::draw_attn(
                        &m,
                        &capture.trace.record,
                        &p::trace_seed(&seed, &state.response),
                    )
                    .ok_or("attention draw")?;
                    Some(attn_blocks::partials_root(&op.partials, d.dv).ok_or("parent partials")?)
                };
                crate::verified_inference::verify_seeded_attention_phase(
                    &piece.payload,
                    &state.response,
                    &signer,
                    &tier,
                    &resp,
                    &seed,
                    &ctx,
                    &piece.path,
                    parent.as_ref(),
                )
                .map(|_| ())
            }
            2 => crate::verified_inference::verify_seeded_recurrent_opening(
                &piece.payload,
                &state.response,
                &signer,
                &tier,
                &resp,
                &seed,
                &ctx,
            ),
            3 => crate::verified_inference::verify_seeded_residual_opening(
                &piece.payload,
                &state.response,
                &signer,
                &tier,
                &resp,
                &seed,
                &ctx,
            ),
            _ => return Err("invalid piece kind".into()),
        };
        result.map_err(|e| format!("persisted opening verification {e:?}"))?;
    }
    Ok(())
}

/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `ids` must cover `count` consecutive 32-byte hashes, not merely `count` bytes.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_refund(
    ptr: *mut c_void,
    ids: *const u8,
    count: usize,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing audit service".into());
        }
        let ids = unsafe { view(ids.cast::<Hash>(), count, 32) }?;
        unsafe { &mut *(ptr as *mut Service) }.refund_unsent(ids)
    })
}

#[cfg(test)]
mod tests;

/// Dedicated offline lifecycle probe. It uses the already isolated test service
/// and real GPU proofs, but never calls any transport or production journal.
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// The service must be an empty, isolated diagnostic service; `capture` must be live. Callbacks
/// require exclusive GPU ownership and may not unwind or reenter. `stats` must cover FOUR
/// writable u32 values.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_selftest(
    ptr: *mut c_void,
    capture: *mut c_void,
    api: *const Api,
    stats: *mut u32,
) -> i32 {
    guard(|| {
        if ptr.is_null() || capture.is_null() || api.is_null() || stats.is_null() {
            return Err("lifecycle probe arguments".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        let api = unsafe { &*api };
        if !s.pending.is_empty() {
            return Err("lifecycle probe requires an empty isolated service".into());
        }
        let original = unsafe { &*(capture as *const Capture) };
        let capture =
            Capture::try_from_slice(&borsh::to_vec(original).map_err(|_| "probe capture")?)
                .map_err(|_| "probe capture")?;
        let commitment = trace_commit(&capture)?;
        let mut response =
            keryx_inference::AiResponsePayload::new(capture.request, 140116000, [0x12; 34], 1)
                .with_private_body(vec![9])
                .with_trace(commitment);
        let (pk, sig) = crate::sign_digest(&s.secret, &response.signed_bytes())
            .map_err(|_| "probe responder signature")?;
        response.responder = Some(keryx_inference::AiResponder {
            escrow_pubkey: pk,
            signature: sig,
        });
        let base = 140115000;
        let id = s.commit(capture, response.serialize(), base, [0xa1; 32], base + 6000)?;
        let block = |hash, daa, accepted| ChainBlock {
            hash: [hash; 32],
            daa,
            accepted,
        };
        s.note_anchor(block(0xa1, base, vec![]))?;
        s.chain_page(
            [0xa1; 32],
            &[],
            vec![
                block(0xa2, base + 1, vec![id]),
                block(0xa3, base + 101, vec![]),
            ],
            true,
        )?;
        let mut replay_calls = 0;
        let mut build = |c: &Capture, seed, id, path: &[u64], prev: Option<&[u8]>| {
            replay_calls += 1;
            crate::h15_bridge::build(c, api, seed, id, path, prev.unwrap_or(&[]))
        };
        s.advance(base + 102, None, &mut build)?;
        let initial = s.due(base + 102, 1000)?;
        if initial.len() != 4 {
            return Err("initial trace/attention/recurrent/residual obligations missing".into());
        }
        let initial_ids = s.pending[&id]
            .state
            .pieces
            .iter()
            .map(|p| p.id)
            .collect::<Vec<_>>();
        s.refund_unsent(&initial_ids[1..])?;
        if s.pending[&id]
            .state
            .pieces
            .iter()
            .map(|p| p.attempts)
            .collect::<Vec<_>>()
            != [1, 0, 0, 0]
        {
            return Err("unsent attempts were consumed".into());
        }
        // Reconnect/restoration uses identical signed pieces; no generation or replay.
        let path = s.dir.with_extension("state"); // with_extension changes only the storage suffix
        let secret = s.secret;
        let binding = s.binding.clone();
        let tier = s.tier;
        s.flush()?;
        let restored = Service::open(&path, secret, tier, binding)?;
        *s = restored;
        s.note_anchor(block(0xa1, base, vec![]))?;
        s.chain_page(
            [0xa1; 32],
            &[],
            vec![
                block(0xa2, base + 1, vec![id]),
                block(0xa3, base + 101, vec![]),
            ],
            true,
        )?;
        // The record's accepted anchor also needs a complete page after restart.
        s.note_anchor(block(0xa2, base + 1, vec![]))?;
        s.chain_page([0xa2; 32], &[], vec![block(0xa3, base + 101, vec![])], true)?;
        let repeated = s.due(base + 103, 5000)?;
        if repeated != initial {
            return Err("reconnect changed signed audit transactions".into());
        }
        s.advance(base + 103, None, &mut build)?;
        // A draw reorg builds the new seed, then reinclusion reuses the original bytes.
        s.chain_page(
            [0xa2; 32],
            &[[0xa3; 32]],
            vec![block(0xa4, base + 102, vec![])],
            true,
        )?;
        s.advance(base + 103, None, &mut build)?;
        s.chain_page(
            [0xa2; 32],
            &[[0xa4; 32]],
            vec![block(0xa3, base + 101, vec![])],
            true,
        )?;
        s.advance(base + 104, None, &mut build)?;
        if s.pending[&id]
            .state
            .pieces
            .iter()
            .map(|p| p.payload.clone())
            .collect::<Vec<_>>()
            != initial
        {
            return Err("seed reinclusion changed cached bytes".into());
        }
        let mut tail = [0xa3; 32];
        let mut now = base + 105;
        let mut phases = 0;
        for step in 0..8u8 {
            let ids = s.pending[&id]
                .state
                .pieces
                .iter()
                .filter(|p| p.accepted.is_none())
                .map(|p| p.id)
                .collect();
            let accepting = 0xb0 + step * 2;
            let draw = accepting + 1;
            s.chain_page(
                tail,
                &[],
                vec![block(accepting, now, ids), block(draw, now + 100, vec![])],
                true,
            )?;
            tail = [draw; 32];
            now += 101;
            let before = s.pending[&id].state.pieces.len();
            s.advance(now, None, &mut build)?;
            let after = s.pending[&id].state.pieces.len();
            if after == before {
                if !s.pending[&id]
                    .state
                    .pieces
                    .iter()
                    .all(|p| p.accepted.is_some())
                {
                    return Err("audit stopped before canonical acceptance".into());
                }
                break;
            }
            phases += 1;
            if step == 7 {
                return Err("lifecycle phase bound".into());
            }
        }
        // The complete record is retained through the reorg window, never finalized on an ACK.
        if !s.pending.contains_key(&id) {
            return Err("completed audit was discarded before its reorg window".into());
        }
        unsafe {
            *stats = s.pending[&id].state.pieces.len() as u32;
            *stats.add(1) = phases;
            *stats.add(2) = replay_calls;
            *stats.add(3) = 1;
        }
        Ok(())
    })
}

/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_estimate(
    ptr: *const c_void,
    prompt: u32,
    tokens: u32,
) -> u64 {
    if ptr.is_null() {
        return u64::MAX;
    }
    let service = unsafe { &*(ptr as *const Service) };
    crate::trace_speeds::generation_daa(&service.binding.model, tokens)
        .unwrap_or_else(|| p::work_daa(service.tier, 0, tokens, 0))
        .saturating_add(p::work_daa(service.tier, prompt, 0, 1))
        .saturating_add(3000)
}

/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `out` must be a writable Buffer slot; release its previous allocation before overwriting it.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_response_due(
    ptr: *mut c_void,
    daa: u64,
    ms: u64,
    out: *mut Buffer,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let s = unsafe { &mut *(ptr as *mut Service) };
        unsafe { output(seq::bundle(&s.response_due(daa, ms)?), out) }
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `id` must span 32 readable bytes.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_response_intent(
    ptr: *mut c_void,
    id: *const u8,
    ms: u64,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let id = unsafe { view(id, 32, 32) }?.try_into().unwrap();
        unsafe { &mut *(ptr as *mut Service) }.response_intent(id, ms)?;
        Ok(())
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `id` must span 32 readable bytes.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_response_result(
    ptr: *mut c_void,
    id: *const u8,
    rejected: u8,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let id = unsafe { view(id, 32, 32) }?.try_into().unwrap();
        unsafe { &mut *(ptr as *mut Service) }.response_result(id, rejected != 0)
    })
}
/// # Safety
/// Pointer validity, alignment, span lengths and non-aliasing must satisfy
/// the contract in `docs/native-ffi.md`; bounds checks cannot validate allocations.
/// `ptr` must be a live service; mutable access must be exclusive for the entire call.
/// `request` must span 32 readable bytes; `out` must be a writable Buffer slot.
#[no_mangle]
pub unsafe extern "C" fn nexus_h15_service_response_for_request(
    ptr: *const c_void,
    request: *const u8,
    out: *mut Buffer,
) -> i32 {
    guard(|| {
        if ptr.is_null() {
            return Err("missing service".into());
        }
        let request = unsafe { view(request, 32, 32) }?.try_into().unwrap();
        let bytes = unsafe { &*(ptr as *const Service) }
            .response_for_request(request)
            .map_or(Vec::new(), |h| h.to_vec());
        unsafe { output(bytes, out) }
    })
}
