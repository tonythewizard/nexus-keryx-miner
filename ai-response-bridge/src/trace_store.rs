//! Stock-compatible restart records. Corrupt/unknown records remain on disk and
//! block recovery instead of being silently discarded. Loading never submits.
use crate::trace_producer::ResponseTrace;
use borsh::{BorshDeserialize, BorshSerialize};
use std::{
    fs, io,
    io::{Read, Write},
    path::{Path, PathBuf},
};

const VERSION: u32 = 1;
const MAX_FILE: u64 = 64 * 1024 * 1024;
const MAX_RECORDS: usize = 1024;
pub const CHAIN_RESTORE_INFLIGHT: usize = 8;

#[derive(Clone, Debug, BorshSerialize, BorshDeserialize)]
pub struct StoredOpening {
    pub seed_hash: String,
    pub deadline_daa: u64,
    pub payload: String,
}

#[derive(BorshSerialize, BorshDeserialize)]
pub struct StoredTrace {
    pub version: u32,
    pub response_txid: String,
    pub model_id: [u8; 32],
    pub tier: u8,
    pub request_hash: [u8; 32],
    pub max_tokens: u32,
    pub submitted_daa: u64,
    pub accepted_hash: Option<String>,
    pub opening: Option<StoredOpening>,
    pub trace: ResponseTrace,
}

#[derive(BorshSerialize,BorshDeserialize)]
struct LegacyRecord {
    n_prompt:u32,n_pos:u32,ubatch:Vec<u32>,n_leaves:usize,
    leaves:Vec<[u8;32]>,pos_roots:Vec<[u8;32]>,trace_root:[u8;32],
}
#[derive(BorshSerialize,BorshDeserialize)]
struct LegacyResponse {record:LegacyRecord,tokens:Vec<i32>}
#[derive(BorshSerialize,BorshDeserialize)]
struct LegacyStored {
    version:u32,response_txid:String,model_id:[u8;32],tier:u8,request_hash:[u8;32],
    max_tokens:u32,submitted_daa:u64,accepted_hash:Option<String>,opening:Option<StoredOpening>,trace:LegacyResponse,
}
fn decode_record(bytes:&[u8])->io::Result<StoredTrace> {
    if let Ok(current)=StoredTrace::try_from_slice(bytes) {
        if current.validate().is_ok() {return Ok(current);}
    }
    let old=LegacyStored::try_from_slice(bytes).map_err(|_|invalid())?;
    let r=old.trace.record;
    let current=StoredTrace {version:old.version,response_txid:old.response_txid,model_id:old.model_id,
        tier:old.tier,request_hash:old.request_hash,max_tokens:old.max_tokens,submitted_daa:old.submitted_daa,
        accepted_hash:old.accepted_hash,opening:old.opening,trace:ResponseTrace {
            record:crate::trace_producer::TraceRecord::from_legacy(r.n_prompt,r.n_pos,r.ubatch,r.n_leaves,r.leaves,r.pos_roots,r.trace_root),tokens:old.trace.tokens}};
    current.validate()?;Ok(current)
}

fn invalid() -> io::Error {
    io::Error::new(
        io::ErrorKind::InvalidData,
        "Trace recovery is indeterminate; original record preserved",
    )
}
fn hash(value: &str) -> bool {
    value.len() == 64
        && value
            .bytes()
            .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
impl StoredTrace {
    fn validate(&self) -> io::Result<()> {
        let r = &self.trace.record;
        if self.version != VERSION
            || !hash(&self.response_txid)
            || self.tier > 4
            || self.max_tokens == 0
            || self.accepted_hash.as_deref().is_some_and(|h| !hash(h))
            || !r.storage_shape_valid()
            || self.trace.tokens.len() < r.n_pos as usize
        {
            return Err(invalid());
        }
        if let Some(o) = &self.opening {
            if self.accepted_hash.is_none()
                || !hash(&o.seed_hash)
                || o.deadline_daa <= self.submitted_daa
                || o.payload.is_empty()
                || o.payload.len() % 2 != 0
                || o.payload.len() > 2 * keryx_verifiable_inference::opening::MAX_PAYLOAD_LEN
                || !o.payload.bytes().all(|b| b.is_ascii_hexdigit())
            {
                return Err(invalid());
            }
        }
        Ok(())
    }
}

pub struct TraceStore {
    dir: PathBuf,
}
impl TraceStore {
    /// Same `<escrow state>.traces/` location and Borsh field order as stock.
    pub fn new(escrow_state_file: &Path) -> Self {
        Self {
            dir: escrow_state_file.with_extension("traces"),
        }
    }
    pub fn dir(&self) -> &Path {
        &self.dir
    }
    pub fn is_present(&self) -> io::Result<bool> {
        match fs::symlink_metadata(&self.dir) {
            Ok(info) if info.is_dir() => Ok(true),
            Ok(_) => Err(invalid()),
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(false),
            Err(e) => Err(e),
        }
    }
    pub fn save(&self, record: &StoredTrace) -> io::Result<()> {
        record.validate()?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::{DirBuilderExt, PermissionsExt};
            fs::DirBuilder::new()
                .recursive(true)
                .mode(0o700)
                .create(&self.dir)?;
            let metadata = fs::symlink_metadata(&self.dir)?;
            if !metadata.is_dir() || metadata.permissions().mode() & 0o077 != 0 {
                return Err(invalid());
            }
        }
        #[cfg(not(unix))]
        fs::create_dir_all(&self.dir)?;
        let bytes = borsh::to_vec(record)?;
        if bytes.len() as u64 > MAX_FILE {
            return Err(invalid());
        }
        let destination = self.dir.join(format!("{}.trace", record.response_txid));
        match fs::symlink_metadata(&destination) {
            Ok(info) => {
                if !info.is_file() || info.len() > MAX_FILE {
                    return Err(invalid());
                }
                let mut previous = Vec::new();
                fs::File::open(&destination)?
                    .take(MAX_FILE + 1)
                    .read_to_end(&mut previous)?;
                if previous.len() as u64 > MAX_FILE {
                    return Err(invalid());
                }
                let previous = decode_record(&previous)?;
                previous.validate()?;
                if previous.response_txid != record.response_txid
                    || previous.request_hash != record.request_hash
                    || previous.model_id != record.model_id
                    || previous.tier != record.tier
                    || previous.max_tokens != record.max_tokens
                    || previous.submitted_daa != record.submitted_daa
                    || previous.trace.record.trace_root != record.trace.record.trace_root
                    || previous.trace.record.n_prompt != record.trace.record.n_prompt
                    || previous.trace.record.n_pos != record.trace.record.n_pos
                {
                    return Err(invalid());
                }
                if previous.accepted_hash != record.accepted_hash && record.opening.is_some() {
                    return Err(invalid()); // Reorg reconciliation clears the old opening first.
                }
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e),
        }
        let mut temporary = tempfile::NamedTempFile::new_in(&self.dir)?;
        temporary.write_all(&bytes)?;
        temporary.as_file().sync_all()?;
        temporary.persist(destination).map_err(|e| e.error)?;
        #[cfg(unix)]
        fs::File::open(&self.dir)?.sync_all()?;
        Ok(())
    }
    pub fn load_all(&self) -> io::Result<Vec<StoredTrace>> {
        let entries = match fs::read_dir(&self.dir) {
            Ok(entries) => entries,
            Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
            Err(e) => return Err(e),
        };
        if !fs::symlink_metadata(&self.dir)?.is_dir() {
            return Err(invalid());
        }
        let mut paths = Vec::new();
        for entry in entries {
            let path = entry?.path();
            if path.extension().and_then(|s| s.to_str()) == Some("trace") {
                if paths.len() >= MAX_RECORDS {
                    return Err(invalid());
                }
                paths.push(path);
            }
        }
        paths.sort();
        let mut out = Vec::new();
        for path in paths {
            let metadata = fs::symlink_metadata(&path)?;
            if !metadata.is_file() || metadata.len() > MAX_FILE {
                return Err(invalid());
            }
            let mut bytes = Vec::new();
            fs::File::open(&path)?
                .take(MAX_FILE + 1)
                .read_to_end(&mut bytes)?;
            if bytes.len() as u64 > MAX_FILE {
                return Err(invalid());
            }
            let record = decode_record(&bytes)?;
            record.validate()?;
            if path.file_stem().and_then(|s| s.to_str()) != Some(record.response_txid.as_str()) {
                return Err(invalid());
            }
            out.push(record);
        }
        Ok(out)
    }
}

/// Restoration asks for acceptance blocks first. Stored RPC ACK/opening bytes
/// are not chain confirmation or permission to retry before reconciliation.
pub fn acceptance_blocks(records: &[StoredTrace]) -> Vec<String> {
    records
        .iter()
        .filter_map(|r| r.accepted_hash.clone())
        .collect::<std::collections::BTreeSet<_>>()
        .into_iter()
        .collect()
}

/// Stock's bounded unsigned-opening cache, keyed by the exact draw seed block.
/// Retaining a body across reorgs is not permission to submit it for another seed.
pub struct BuiltOpenings { response:[u8;32], bodies:Vec<([u8;32],Vec<u8>)> }
impl BuiltOpenings {
    pub fn new(response:[u8;32])->Self {Self {response,bodies:Vec::new()}}
    pub fn remember(&mut self,seed:[u8;32],body:Vec<u8>)->io::Result<()> {
        use keryx_verifiable_inference::opening;
        if body.len()>opening::MAX_BODY_LEN || body.first()==Some(&opening::VERSION_SEEDED) {return Err(invalid());}
        let parsed=opening::Opening::decode(&body).ok_or_else(invalid)?;
        if parsed.response_txid!=self.response || parsed.points.len()!=keryx_verifiable_inference::draw::POINTS as usize {return Err(invalid());}
        self.bodies.retain(|(old,_)|old!=&seed);
        if self.bodies.len()>=4 {self.bodies.remove(0);}
        self.bodies.push((seed,body));Ok(())
    }
    pub fn find(&self,seed:&[u8;32])->Option<&[u8]> {self.bodies.iter().find(|(h,_)|h==seed).map(|(_,b)|b.as_slice())}
}

/// Bounded first phase of stock's chain rebuild. Draining this queue proves
/// only acceptance-header reads, never complete chain/AI coverage.
pub struct AcceptanceRestore {
    queued: std::collections::VecDeque<String>,
    in_flight: std::collections::BTreeSet<String>,
    retries: crate::trace_recovery::ChainRestoreRetries,
    blocked: std::collections::BTreeSet<String>,
}
impl AcceptanceRestore {
    pub fn new(records: &[StoredTrace]) -> Self {
        Self {
            queued: acceptance_blocks(records).into(),
            in_flight: Default::default(),
            retries: Default::default(),
            blocked: Default::default(),
        }
    }
    pub fn next_batch(&mut self) -> Vec<String> {
        let mut out = Vec::new();
        while self.in_flight.len() < CHAIN_RESTORE_INFLIGHT {
            let Some(hash) = self.queued.pop_front() else {
                break;
            };
            self.in_flight.insert(hash.clone());
            out.push(hash);
        }
        out
    }
    pub fn complete(&mut self, hash: &str, canonical_header_verified: bool) -> io::Result<()> {
        let key = decode_block_hash(hash)?;
        if !canonical_header_verified || !self.in_flight.remove(hash) {
            return Err(invalid());
        }
        self.blocked.remove(hash);
        self.retries.succeeded(&key);
        Ok(())
    }
    /// A failed read is retried at most twice. Exhaustion keeps the unresolved
    /// hash visible and does not drain the queue or delete a persisted trace.
    pub fn failed(&mut self, hash: &str) -> io::Result<bool> {
        let key = decode_block_hash(hash)?;
        if !self.in_flight.contains(hash) || self.blocked.contains(hash) { return Err(invalid()); }
        if self.retries.failed(key) {
            self.in_flight.remove(hash);
            self.queued.push_back(hash.to_owned());
            Ok(true)
        } else {
            self.blocked.insert(hash.to_owned());
            Ok(false)
        }
    }
    pub fn blocked_hashes(&self) -> &std::collections::BTreeSet<String> { &self.blocked }
    pub fn is_drained(&self) -> bool {
        self.queued.is_empty() && self.in_flight.is_empty()
    }
}

fn decode_block_hash(hash: &str) -> io::Result<[u8;32]> {
    if hash.len()!=64 || !hash.bytes().all(|c|c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) { return Err(invalid()); }
    hex::decode(hash).map_err(|_|invalid())?.try_into().map_err(|_|invalid())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn record() -> StoredTrace {
        StoredTrace {
            version: 1,
            response_txid: "a".repeat(64),
            model_id: [1; 32],
            tier: 0,
            request_hash: [2; 32],
            max_tokens: 2048,
            submitted_daa: 7,
            accepted_hash: Some("b".repeat(64)),
            opening: Some(StoredOpening {
                seed_hash: "c".repeat(64),
                deadline_daa: 42,
                payload: "00ff".into(),
            }),
            trace: ResponseTrace::test_sample(),
        }
    }
    #[test]
    fn exhausted_acceptance_reads_preserve_journal_and_block_recovery() {
        let dir=tempfile::tempdir().unwrap();let store=TraceStore::new(&dir.path().join("escrow.json"));
        let r=record();store.save(&r).unwrap();let hash=r.accepted_hash.clone().unwrap();
        let before=fs::read(store.dir().join(format!("{}.trace",r.response_txid))).unwrap();
        let mut restore=AcceptanceRestore::new(&[r]);
        for attempt in 0..3 {
            assert_eq!(restore.next_batch(),vec![hash.clone()]);
            assert_eq!(restore.failed(&hash).unwrap(),attempt<2);
        }
        assert!(!restore.is_drained());assert!(restore.next_batch().is_empty());
        assert!(restore.blocked_hashes().contains(&hash));assert!(restore.failed(&hash).is_err());
        let restored=store.load_all().unwrap();assert_eq!(restored.len(),1);
        assert_eq!(fs::read(store.dir().join(format!("{}.trace",restored[0].response_txid))).unwrap(),before);
        assert!(restore.complete(&hash,false).is_err());assert!(!restore.is_drained());
        restore.complete(&hash,true).unwrap();assert!(restore.is_drained());
    }
    #[test]
    fn pre_stage2_records_load_without_rewriting_or_losing_signed_openings() {
        let dir=tempfile::tempdir().unwrap();let store=TraceStore::new(&dir.path().join("escrow.json"));
        fs::create_dir_all(store.dir()).unwrap();
        let old=LegacyStored {version:1,response_txid:"a".repeat(64),model_id:[1;32],tier:0,
            request_hash:[2;32],max_tokens:2048,submitted_daa:7,accepted_hash:Some("b".repeat(64)),
            opening:Some(StoredOpening{seed_hash:"c".repeat(64),deadline_daa:42,payload:"00ff".into()}),
            trace:LegacyResponse{record:LegacyRecord{n_prompt:2,n_pos:3,ubatch:vec![2,2,1],n_leaves:3,
                leaves:vec![[1;32];9],pos_roots:vec![[2;32];3],trace_root:[170;32]},tokens:vec![5,6,7]}};
        let bytes=borsh::to_vec(&old).unwrap();let path=store.dir().join(format!("{}.trace",old.response_txid));fs::write(&path,&bytes).unwrap();
        let restored=store.load_all().unwrap();assert_eq!(restored[0].trace.record.stage,1);
        assert_eq!(restored[0].opening.as_ref().unwrap().payload,"00ff");
        assert_eq!(fs::read(path).unwrap(),bytes);
    }
    #[test]
    fn cached_openings_survive_seed_reinclusion_but_never_cross_response_or_seed() {
        use keryx_verifiable_inference::testkit::Model;
        let model=Model::new();let trace=model.response(3,9,true);let body=model.open(&trace,[9;32],&[7;32]).encode();
        let mut cache=BuiltOpenings::new([9;32]);cache.remember([1;32],body.clone()).unwrap();
        assert!(cache.find(&[2;32]).is_none());assert_eq!(cache.find(&[1;32]),Some(body.as_slice()));
        let wrong=model.open(&trace,[8;32],&[7;32]).encode();assert!(cache.remember([1;32],wrong).is_err());
        for n in 2..=5 {cache.remember([n;32],body.clone()).unwrap();}
        assert!(cache.find(&[1;32]).is_none());assert_eq!(cache.find(&[5;32]),Some(body.as_slice()));
    }
    #[test]
    fn crash_restore_retains_identity_and_exact_signed_payload() {
        let dir = tempfile::tempdir().unwrap();
        let store = TraceStore::new(&dir.path().join("escrow.json"));
        let record = record();
        store.save(&record).unwrap();
        let saved = fs::read(store.dir.join(format!("{}.trace", record.response_txid))).unwrap();
        let stock = crate::stock_trace_store_fixture::StoredTrace::try_from_slice(&saved).unwrap();
        assert_eq!(stock.response_txid, record.response_txid);
        assert_eq!(stock.request_hash, record.request_hash);
        assert_eq!(stock.opening.as_ref().unwrap().payload, "00ff");
        let records = store.load_all().unwrap();
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].request_hash, record.request_hash);
        assert_eq!(records[0].opening.as_ref().unwrap().payload, "00ff");
        assert_eq!(borsh::to_vec(&records[0]).unwrap(), saved);
        // A crash before rename leaves the previously committed record readable.
        fs::write(store.dir.join("unfinished.tmp"), b"partial").unwrap();
        assert_eq!(store.load_all().unwrap().len(), 1);
        assert_eq!(acceptance_blocks(&records), vec!["b".repeat(64)]);
    }
    #[test]
    fn corrupt_and_unknown_versions_are_preserved_and_block_recovery() {
        let dir = tempfile::tempdir().unwrap();
        let store = TraceStore::new(&dir.path().join("escrow.json"));
        let r = record();
        store.save(&r).unwrap();
        let path = store.dir.join(format!("{}.trace", r.response_txid));
        let mut bytes = borsh::to_vec(&r).unwrap();
        bytes[..4].copy_from_slice(&2u32.to_le_bytes());
        fs::write(&path, &bytes).unwrap();
        assert!(store.load_all().is_err());
        assert_eq!(fs::read(&path).unwrap(), bytes);
        fs::write(&path, b"partial").unwrap();
        assert!(store.load_all().is_err());
        assert_eq!(fs::read(path).unwrap(), b"partial");
    }
    #[test]
    fn traversal_and_invalid_binding_never_overwrite_a_record() {
        let dir = tempfile::tempdir().unwrap();
        let store = TraceStore::new(&dir.path().join("escrow.json"));
        let mut r = record();
        store.save(&r).unwrap();
        r.response_txid = "../escape".into();
        assert!(store.save(&r).is_err());
        assert_eq!(store.load_all().unwrap().len(), 1);
    }
    #[test]
    fn reorg_coverage_restores_acceptance_blocks_without_duplicates() {
        let mut a = record();
        let mut b = record();
        b.response_txid = "d".repeat(64);
        assert_eq!(acceptance_blocks(&[a, b]), vec!["b".repeat(64)]);
        a = record();
        a.accepted_hash = None;
        assert!(acceptance_blocks(&[a]).is_empty()); // needs acceptance discovery, never an absence verdict
        assert_eq!(CHAIN_RESTORE_INFLIGHT, 8);
    }
    #[test]
    fn reorg_clears_pending_opening_without_changing_request_identity() {
        let dir = tempfile::tempdir().unwrap();
        let store = TraceStore::new(&dir.path().join("escrow.json"));
        assert!(!store.is_present().unwrap());
        let mut r = record();
        store.save(&r).unwrap();
        assert!(store.is_present().unwrap());
        let original_request = r.request_hash;
        r.request_hash = [9; 32];
        assert!(store.save(&r).is_err());
        r.request_hash = original_request;
        r.accepted_hash = None;
        assert!(store.save(&r).is_err());
        r.opening = None;
        store.save(&r).unwrap();
        let restored = store.load_all().unwrap();
        assert_eq!(restored[0].request_hash, original_request);
        assert!(restored[0].opening.is_none());
        r.accepted_hash = Some("e".repeat(64));
        store.save(&r).unwrap();
        assert_eq!(
            acceptance_blocks(&store.load_all().unwrap()),
            vec!["e".repeat(64)]
        );
    }
    #[test]
    fn acceptance_restore_limits_concurrency_and_missing_blocks_fail_closed() {
        let records = (0..10)
            .map(|i| {
                let mut r = record();
                r.accepted_hash = Some(format!("{i:064x}"));
                r
            })
            .collect::<Vec<_>>();
        let mut restore = AcceptanceRestore::new(&records);
        let first = restore.next_batch();
        assert_eq!(first.len(), 8);
        assert!(restore.next_batch().is_empty());
        assert!(restore.complete(&first[0], false).is_err());
        assert!(!restore.is_drained());
        restore.complete(&first[1], true).unwrap();
        assert_eq!(restore.next_batch().len(), 1);
    }
}
