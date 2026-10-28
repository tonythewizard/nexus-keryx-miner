//! On-disk copy of each traced response, kept until its opening window closes so a restart or a
//! reconnection does not lose an audit in flight.

use borsh::{BorshDeserialize, BorshSerialize};
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{mpsc, Arc};

const STORE_VERSION: u32 = 5;
/// Records written before the chain anchor: read and kept, without it.
const V4_VERSION: u32 = 4;
/// Records written before attention phases: read and kept, without them.
const V3_VERSION: u32 = 3;
/// Records written before sequence leaves: read and kept, without them.
const LEGACY_VERSION: u32 = 2;
const FILE_EXT: &str = "trace";

/// The opening already built for a response: resent as is after a restart.
#[derive(Clone, Debug, BorshSerialize, BorshDeserialize)]
pub struct StoredOpening {
    pub seed_hash: String,
    pub deadline_daa: u64,
    /// Hex payload of the signed opening transaction.
    pub payload: String,
    /// Hex payloads of the other pieces of the opening, one transaction each.
    pub extra: Vec<String>,
}

/// Attention phases after the first, once the first is submitted.
#[derive(Clone, Debug, Default, PartialEq, BorshSerialize, BorshDeserialize)]
pub struct StoredAttn {
    /// Unsigned body of each submitted phase, phase order.
    pub bodies: Vec<Vec<u8>>,
    /// Hex payload of the signed transaction of the last submitted phase.
    pub payload: String,
    pub deadline_daa: u64,
    /// Chain block that accepted the last submitted phase.
    pub accepted_hash: Option<String>,
}

#[derive(BorshDeserialize)]
struct LegacyStoredOpening {
    seed_hash: String,
    deadline_daa: u64,
    payload: String,
}

#[derive(BorshDeserialize)]
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
    pub trace: wire::ResponseTrace,
    pub attn: Option<StoredAttn>,
    /// Last chain block known when the response was submitted.
    pub anchor_hash: Option<String>,
    /// Measured duration of the generation, in seconds.
    pub gen_secs: Option<f64>,
}

#[derive(BorshDeserialize)]
#[derive(BorshSerialize)]
struct V4StoredTrace {
    version: u32,
    response_txid: String,
    model_id: [u8; 32],
    tier: u8,
    request_hash: [u8; 32],
    max_tokens: u32,
    submitted_daa: u64,
    accepted_hash: Option<String>,
    opening: Option<StoredOpening>,
    trace: wire::ResponseTrace,
    attn: Option<StoredAttn>,
}

impl From<V4StoredTrace> for StoredTrace {
    fn from(t: V4StoredTrace) -> Self {
        Self {
            version: STORE_VERSION,
            response_txid: t.response_txid,
            model_id: t.model_id,
            tier: t.tier,
            request_hash: t.request_hash,
            max_tokens: t.max_tokens,
            submitted_daa: t.submitted_daa,
            accepted_hash: t.accepted_hash,
            opening: t.opening,
            trace: t.trace,
            attn: t.attn,
            anchor_hash: None,
            gen_secs: None,
        }
    }
}

#[derive(BorshDeserialize)]
#[derive(BorshSerialize)]
struct V3StoredTrace {
    version: u32,
    response_txid: String,
    model_id: [u8; 32],
    tier: u8,
    request_hash: [u8; 32],
    max_tokens: u32,
    submitted_daa: u64,
    accepted_hash: Option<String>,
    opening: Option<StoredOpening>,
    trace: wire::ResponseTrace,
}

impl From<V3StoredTrace> for StoredTrace {
    fn from(t: V3StoredTrace) -> Self {
        Self {
            version: STORE_VERSION,
            response_txid: t.response_txid,
            model_id: t.model_id,
            tier: t.tier,
            request_hash: t.request_hash,
            max_tokens: t.max_tokens,
            submitted_daa: t.submitted_daa,
            accepted_hash: t.accepted_hash,
            opening: t.opening,
            trace: t.trace,
            attn: None,
            anchor_hash: None,
            gen_secs: None,
        }
    }
}

#[derive(BorshDeserialize)]
struct LegacyTrace {
    record: crate::trace_producer::TraceRecord,
    tokens: Vec<i32>,
}

#[derive(BorshDeserialize)]
struct LegacyStoredTrace {
    version: u32,
    response_txid: String,
    model_id: [u8; 32],
    tier: u8,
    request_hash: [u8; 32],
    max_tokens: u32,
    submitted_daa: u64,
    accepted_hash: Option<String>,
    opening: Option<LegacyStoredOpening>,
    trace: LegacyTrace,
}

impl From<LegacyStoredTrace> for StoredTrace {
    fn from(t: LegacyStoredTrace) -> Self {
        Self {
            version: STORE_VERSION,
            response_txid: t.response_txid,
            model_id: t.model_id,
            tier: t.tier,
            request_hash: t.request_hash,
            max_tokens: t.max_tokens,
            submitted_daa: t.submitted_daa,
            accepted_hash: t.accepted_hash,
            opening: t.opening.map(|o| StoredOpening { seed_hash: o.seed_hash, deadline_daa: o.deadline_daa, payload: o.payload, extra: Vec::new() }),
            trace: wire::ResponseTrace { record: t.trace.record, tokens: t.trace.tokens, seq: None },
            attn: None,
            anchor_hash: None,
            gen_secs: None,
        }
    }
}

fn decode(b: &[u8]) -> Result<StoredTrace, String> {
    match StoredTrace::try_from_slice(b) {
        Ok(t) => Ok(t),
        Err(e) => match V4StoredTrace::try_from_slice(b) {
            Ok(t) if t.version == V4_VERSION => Ok(t.into()),
            _ => match V3StoredTrace::try_from_slice(b) {
                Ok(t) if t.version == V3_VERSION => Ok(t.into()),
                _ => match LegacyStoredTrace::try_from_slice(b) {
                    Ok(t) if t.version == LEGACY_VERSION => Ok(t.into()),
                    _ => Err(e.to_string()),
                },
            },
        },
    }
}

/// [`StoredTrace`] by reference, for writing.
#[derive(BorshSerialize)]
pub struct StoredTraceRef<'a> {
    pub version: u32,
    pub response_txid: &'a str,
    pub model_id: &'a [u8; 32],
    pub tier: u8,
    pub request_hash: &'a [u8; 32],
    pub max_tokens: u32,
    pub submitted_daa: u64,
    pub accepted_hash: &'a Option<String>,
    pub opening: &'a Option<StoredOpening>,
    pub trace: &'a wire::ResponseTrace,
    pub attn: &'a Option<StoredAttn>,
    pub anchor_hash: &'a Option<String>,
    pub gen_secs: Option<f64>,
}

impl<'a> StoredTraceRef<'a> {
    pub fn version() -> u32 {
        STORE_VERSION
    }
}

/// [`StoredTrace`] owned, for a queued write.
pub struct StoredTraceOwned {
    pub response_txid: String,
    pub model_id: [u8; 32],
    pub tier: u8,
    pub request_hash: [u8; 32],
    pub max_tokens: u32,
    pub submitted_daa: u64,
    pub accepted_hash: Option<String>,
    pub opening: Option<StoredOpening>,
    pub trace: Arc<wire::ResponseTrace>,
    pub attn: Option<StoredAttn>,
    pub anchor_hash: Option<String>,
    pub gen_secs: Option<f64>,
}

impl StoredTraceOwned {
    fn record(&self) -> StoredTraceRef<'_> {
        StoredTraceRef {
            version: STORE_VERSION,
            response_txid: &self.response_txid,
            model_id: &self.model_id,
            tier: self.tier,
            request_hash: &self.request_hash,
            max_tokens: self.max_tokens,
            submitted_daa: self.submitted_daa,
            accepted_hash: &self.accepted_hash,
            opening: &self.opening,
            trace: &self.trace,
            attn: &self.attn,
            anchor_hash: &self.anchor_hash,
            gen_secs: self.gen_secs,
        }
    }
}


/// This distinct wire type leaves the legacy positional ResponseTrace unchanged.
pub mod wire {
    use borsh::{BorshSerialize,BorshDeserialize};
    #[derive(BorshSerialize,BorshDeserialize)]
    pub struct ResponseTrace {
        pub record: crate::trace_producer::TraceRecord,
        pub tokens: Vec<i32>,
        pub seq: Option<crate::trace_producer::SeqTrace>,
    }
}

const MAX_FILE: u64 = 512 * 1024 * 1024;
fn invalid() -> io::Error {io::Error::new(io::ErrorKind::InvalidData,"audit recovery blocked; original preserved")}
fn valid_id(id:&str)->bool {id.len()==64 && id.bytes().all(|c|c.is_ascii_digit()||(b'a'..=b'f').contains(&c))}
fn check_dir(dir:&Path)->io::Result<()> {
    #[cfg(unix)] {use std::os::unix::fs::{DirBuilderExt,PermissionsExt};
        fs::DirBuilder::new().recursive(true).mode(0o700).create(dir)?;
        let m=fs::symlink_metadata(dir)?;
        if !m.is_dir() || m.permissions().mode()&0o077!=0 {return Err(invalid());}
    }
    #[cfg(not(unix))] {fs::create_dir_all(dir)?;if !fs::symlink_metadata(dir)?.is_dir(){return Err(invalid());}}
    Ok(())
}
fn validate(t:&StoredTrace)->io::Result<()> {
    if t.version!=STORE_VERSION || !valid_id(&t.response_txid) || t.tier>4 || t.max_tokens==0
        || !t.trace.record.storage_shape_valid() || t.trace.tokens.len()<t.trace.record.n_pos as usize
        || [&t.accepted_hash,&t.anchor_hash].iter().any(|v|v.as_deref().is_some_and(|s|!valid_id(s)))
        || t.gen_secs.is_some_and(|s|!s.is_finite()||s<=0.0)
        || t.trace.seq.as_ref().is_some_and(|s|s.leaves.is_empty() || keryx_verifiable_inference::merkle::b3_root(&s.leaves)!=s.root)
    {return Err(invalid());}
    Ok(())
}
fn write_record(dir:&Path,record:&StoredTraceRef<'_>)->io::Result<()> {
    check_dir(dir)?;
    let bytes=borsh::to_vec(record)?;
    if bytes.len() as u64>MAX_FILE{return Err(invalid());}
    validate(&decode(&bytes).map_err(|_|invalid())?)?;
    let path=dir.join(format!("{}.{}",record.response_txid,FILE_EXT));
    if let Ok(meta)=fs::symlink_metadata(&path) {
        if !meta.is_file()||meta.len()>MAX_FILE{return Err(invalid());}
        let old=decode(&fs::read(&path)?).map_err(|_|invalid())?;validate(&old)?;
        if old.request_hash!=*record.request_hash || old.model_id!=*record.model_id
            || old.tier!=record.tier || old.submitted_daa!=record.submitted_daa
            || old.max_tokens!=record.max_tokens || old.trace.record.trace_root!=record.trace.record.trace_root
            || borsh::to_vec(&old.trace)?!=borsh::to_vec(record.trace)?
        {return Err(invalid());}
    }
    use std::io::Write;
    let mut tmp=tempfile::NamedTempFile::new_in(dir)?;tmp.write_all(&bytes)?;tmp.as_file().sync_all()?;
    tmp.persist(path).map_err(|e|e.error)?;
    #[cfg(unix)] fs::File::open(dir)?.sync_all()?;
    Ok(())
}
enum Job {Save(Box<StoredTraceOwned>),Remove(String),Flush(mpsc::Sender<Result<(),String>>)}
/// Separate audit namespace; this never opens or migrates live positional journals.
/// A single bounded writer preserves save/remove ordering; IO failure is sticky
/// and reported by flush/load instead of dropping records or claiming durability.
pub struct TraceStore {dir:PathBuf,tx:mpsc::SyncSender<Job>}
impl TraceStore {
    pub fn new(state:&Path)->io::Result<Self> {
        let dir=state.with_extension("audit-traces");check_dir(&dir)?;
        let (tx,rx)=mpsc::sync_channel::<Job>(16);let root=dir.clone();
        std::thread::Builder::new().name("nexus-audit-store".into()).spawn(move || {
            let mut error:Option<String>=None;
            for job in rx {
                let result=match job {
                    Job::Flush(done)=>{let _=done.send(error.clone().map_or(Ok(()),Err));continue;},
                    _ if error.is_some()=>continue,
                    Job::Save(r)=>write_record(&root,&r.record()),
                    Job::Remove(id)=> {
                        let p=root.join(format!("{}.{}",id,FILE_EXT));
                        match fs::symlink_metadata(&p) {
                            Ok(m) if m.is_file()=>fs::remove_file(p).and_then(|_| {
                                #[cfg(unix)] fs::File::open(&root)?.sync_all()?;
                                Ok(())
                            }),
                            Ok(_)=>Err(invalid()),
                            Err(e) if e.kind()==io::ErrorKind::NotFound=>Ok(()),
                            Err(e)=>Err(e),
                        }
                    }
                };
                if let Err(e)=result {error=Some(e.to_string());}
            }
        })?;
        Ok(Self{dir,tx})
    }
    pub fn save_queued(&self,r:StoredTraceOwned)->io::Result<()> {
        if !valid_id(&r.response_txid){return Err(invalid());}
        self.tx.send(Job::Save(Box::new(r))).map_err(|_|invalid())
    }
    /// Caller must prove expiry/reorg reconciliation before removal.
    pub fn remove(&self,id:&str)->io::Result<()> {
        if !valid_id(id){return Err(invalid());}
        self.tx.send(Job::Remove(id.into())).map_err(|_|invalid())
    }
    pub fn flush(&self)->io::Result<()> {
        let (tx,rx)=mpsc::channel();self.tx.send(Job::Flush(tx)).map_err(|_|invalid())?;
        rx.recv().map_err(|_|invalid())?.map_err(io::Error::other)
    }
    pub fn load_all(&self)->io::Result<Vec<StoredTrace>> {self.load_filtered(|_|Ok(true))}
    pub fn load_filtered(&self,mut include:impl FnMut(&str)->io::Result<bool>)->io::Result<Vec<StoredTrace>> {
        self.flush()?;let mut total=0u64;check_dir(&self.dir)?;let mut out=Vec::new();
        for entry in fs::read_dir(&self.dir)? {
            let p=entry?.path();if p.extension().and_then(|s|s.to_str())!=Some(FILE_EXT){continue;}
            let m=fs::symlink_metadata(&p)?;
            if !m.is_file()||m.len()>MAX_FILE||out.len()>=1024{return Err(invalid());}
            let stem=p.file_stem().and_then(|s|s.to_str()).ok_or_else(invalid)?;if !include(stem)?{continue;}
            total=total.checked_add(m.len()).ok_or_else(invalid)?;if total>1024*1024*1024{return Err(invalid());}
            let t=decode(&fs::read(&p)?).map_err(|_|invalid())?;validate(&t)?;
            if p.file_stem().and_then(|s|s.to_str())!=Some(t.response_txid.as_str()){return Err(invalid());}
            out.push(t);
        }
        Ok(out)
    }
}
impl Drop for TraceStore {fn drop(&mut self){let _=self.flush();}}

#[cfg(test)]
mod tests {
    use super::*;
    fn trace()->Arc<wire::ResponseTrace>{let t=crate::trace_producer::ResponseTrace::test_sample();Arc::new(wire::ResponseTrace{record:t.record,tokens:t.tokens,seq:crate::trace_producer::SeqTrace::new(vec![[3;32],[4;32]])})}
    fn rec(id:&str,anchor:Option<String>)->StoredTraceOwned {StoredTraceOwned{
        response_txid:id.into(),model_id:[1;32],tier:0,request_hash:[2;32],max_tokens:20,submitted_daa:7,
        accepted_hash:None,opening:None,trace:trace(),attn:None,anchor_hash:anchor,gen_secs:Some(12.5)}}
    #[test]
    fn queued_order_v4_migration_sequence_and_anchor_survive_restart(){
        let dir=tempfile::tempdir().unwrap();let state=dir.path().join("state");let store=TraceStore::new(&state).unwrap();
        let a="a".repeat(64);let b="b".repeat(64);let c="c".repeat(64);
        store.save_queued(rec(&a,None)).unwrap();store.remove(&a).unwrap();store.remove(&b).unwrap();
        store.save_queued(rec(&b,Some(a.clone()))).unwrap();store.save_queued(rec(&b,Some(c.clone()))).unwrap();store.flush().unwrap();
        let old=V4StoredTrace{version:4,response_txid:a.clone(),model_id:[1;32],tier:0,request_hash:[2;32],max_tokens:20,submitted_daa:7,accepted_hash:None,opening:None,trace:Arc::try_unwrap(trace()).ok().unwrap(),attn:None};
        fs::write(store.dir.join(format!("{a}.trace")),borsh::to_vec(&old).unwrap()).unwrap();drop(store);
        let store=TraceStore::new(&state).unwrap();let mut loaded=store.load_all().unwrap();loaded.sort_by(|a,b|a.response_txid.cmp(&b.response_txid));
        assert_eq!(loaded.len(),2);assert_eq!(loaded[0].version,5);assert!(loaded[0].anchor_hash.is_none());
        assert_eq!(loaded[1].anchor_hash.as_deref(),Some(c.as_str()));assert_eq!(loaded[1].gen_secs,Some(12.5));
        assert_eq!(loaded[1].trace.seq.as_ref().unwrap().leaves,vec![[3;32],[4;32]]);
    }
    #[test]
    fn corruption_and_unknown_versions_are_retained_and_block_recovery(){
        let dir=tempfile::tempdir().unwrap();let store=TraceStore::new(&dir.path().join("state")).unwrap();let id="a".repeat(64);
        let path=store.dir.join(format!("{id}.trace"));fs::write(&path,b"unknown").unwrap();
        assert!(store.load_all().is_err());assert_eq!(fs::read(&path).unwrap(),b"unknown");
        store.save_queued(rec(&id,None)).unwrap();assert!(store.flush().is_err());assert_eq!(fs::read(&path).unwrap(),b"unknown");
    }
    #[test]
    fn v2_and_v3_records_keep_trace_bytes_without_inventing_anchors(){
        let dir=tempfile::tempdir().unwrap();let store=TraceStore::new(&dir.path().join("state")).unwrap();
        let a="a".repeat(64);let b="b".repeat(64);let t=trace();
        let legacy=borsh::to_vec(&(2u32,&a,[1u8;32],0u8,[2u8;32],20u32,7u64,
            Option::<String>::None,Option::<(String,u64,String)>::None,&t.record,&t.tokens)).unwrap();
        fs::write(store.dir.join(format!("{a}.trace")),&legacy).unwrap();
        let v3=V3StoredTrace{version:3,response_txid:b.clone(),model_id:[1;32],tier:0,request_hash:[2;32],max_tokens:20,submitted_daa:7,accepted_hash:None,opening:None,trace:Arc::try_unwrap(trace()).ok().unwrap()};
        fs::write(store.dir.join(format!("{b}.trace")),borsh::to_vec(&v3).unwrap()).unwrap();
        let loaded=store.load_all().unwrap();assert_eq!(loaded.len(),2);
        let old=loaded.iter().find(|x|x.response_txid==a).unwrap();assert_eq!(old.trace.record.trace_root,t.record.trace_root);assert!(old.trace.seq.is_none());
        assert!(loaded.iter().all(|x|x.version==5&&x.gen_secs.is_none()&&x.anchor_hash.is_none()));
        assert_eq!(fs::read(store.dir.join(format!("{a}.trace"))).unwrap(),legacy);
    }
}
