//! Incremental H15 acceptance/draw recovery, mirroring upstream a7eb9e4.
//! Callers supply verified selected-chain answers, not ACKs. This module never
//! authorizes submission or deletes a journal record after a transient RPC error.
use std::collections::{BTreeMap, BTreeSet};

pub type Hash = [u8; 32];
pub const CHAIN_RESTORE_MAX_FAILURES: u32 = 3;
pub const REPLAY_MAX_FAILURES: u32 = 2;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ChainEntry { pub hash: Hash, pub daa: Option<u64> }

/// Place the returned contiguous segment, preserving known DAA values and one
/// occurrence of each hash. Missing/pruned start is handled like upstream: the
/// segment is appended, but that alone does not prove historical coverage.
pub fn place_chain_after(log: &mut Vec<ChainEntry>, start: Hash, added: &[Hash]) -> Result<Vec<Hash>, &'static str> {
    let unique: BTreeSet<_> = added.iter().copied().collect();
    if unique.len() != added.len() || unique.contains(&start) { return Err("ambiguous chain segment"); }
    let mut known = BTreeMap::new();
    for entry in log.iter() {
        if let Some(previous) = known.insert(entry.hash, entry.daa) {
            if previous.is_some() && entry.daa.is_some() && previous != entry.daa {
                return Err("conflicting canonical DAA");
            }
            known.insert(entry.hash, previous.or(entry.daa));
        }
    }
    let mut kept = Vec::with_capacity(log.len() + added.len());
    let mut seen = BTreeSet::new();
    for entry in log.iter().filter(|e| !unique.contains(&e.hash)) {
        if seen.insert(entry.hash) { kept.push(ChainEntry {hash:entry.hash, daa:known[&entry.hash]}); }
    }
    let at = kept.iter().position(|e| e.hash == start).map_or(kept.len(), |p| p + 1);
    let entries: Vec<_> = added.iter().map(|hash| ChainEntry { hash:*hash, daa:known.get(hash).copied().flatten() }).collect();
    let missing = entries.iter().filter(|e| e.daa.is_none()).map(|e|e.hash).collect();
    kept.splice(at..at, entries);
    *log = kept;
    Ok(missing)
}

/// Bounded retries are keyed by accepting block, never shared by unrelated
/// responses. Exhaustion remains BLOCKED; stored obligations are preserved.
#[derive(Default)]
pub struct ChainRestoreRetries { failures: BTreeMap<Hash,u32> }
impl ChainRestoreRetries {
    pub fn failed(&mut self, start: Hash) -> bool {
        let failures = self.failures.entry(start).or_default();
        *failures = failures.saturating_add(1);
        *failures < CHAIN_RESTORE_MAX_FAILURES
    }
    pub fn succeeded(&mut self, start: &Hash) { self.failures.remove(start); }
    pub fn blocked(&self, start: &Hash) -> bool {
        self.failures.get(start).is_some_and(|n| *n >= CHAIN_RESTORE_MAX_FAILURES)
    }
}

/// One replay in flight per draw; late completions for an orphaned seed cannot
/// mutate the new draw. The initial replay plus two retries matches upstream.
#[derive(Debug)]
pub struct ReplayAttempt { seed: Hash, failures: u32, in_flight: bool, complete: bool }
impl ReplayAttempt {
    pub fn new(seed: Hash) -> Self { Self {seed, failures:0, in_flight:false, complete:false} }
    pub fn start(&mut self) -> bool {
        if self.in_flight || self.complete || self.failures > REPLAY_MAX_FAILURES { return false; }
        self.in_flight = true; true
    }
    pub fn finish(&mut self, seed: Hash, built: bool) -> Result<(), &'static str> {
        if seed != self.seed || !self.in_flight { return Err("stale or unsolicited replay result"); }
        self.in_flight = false;
        self.complete = built;
        if !built { self.failures = self.failures.saturating_add(1); }
        Ok(())
    }
    pub fn reset_for_reorg(&mut self, seed: Hash) { *self = Self::new(seed); }
    pub fn exhausted(&self) -> bool { self.failures > REPLAY_MAX_FAILURES }
}

/// Reaccept only a tracked response whose acceptance was cleared by reorg.
/// The caller must have verified this block's membership in the returned chain.
/// A contradictory acceptance is an error, not a silent overwrite.
pub fn note_verified_acceptance(
    response: &mut crate::trace_store::StoredTrace, response_txid: &str, accepting_block: &str,
) -> Result<bool, &'static str> {
    if response.response_txid != response_txid || accepting_block.len()!=64
        || !accepting_block.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
    { return Err("invalid acceptance binding"); }
    if let Some(previous) = &response.accepted_hash {
        return if previous == accepting_block { Ok(false) } else { Err("acceptance not reconciled") };
    }
    if response.opening.is_some() { return Err("orphaned opening not cleared"); }
    response.accepted_hash = Some(accepting_block.to_owned());
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn entry(id:u8, daa:Option<u64>) -> ChainEntry { ChainEntry {hash:[id;32],daa} }
    #[test]
    fn chain_placement_deduplicates_retains_daa_and_handles_pruned_start() {
        let mut log=vec![entry(1,Some(10)),entry(2,Some(12)),entry(2,None),entry(3,None)];
        assert_eq!(place_chain_after(&mut log,[1;32],&[[3;32],[2;32]]).unwrap(),vec![[3;32]]);
        assert_eq!(log,vec![entry(1,Some(10)),entry(3,None),entry(2,Some(12))]);
        assert_eq!(place_chain_after(&mut log,[9;32],&[[2;32],[4;32]]).unwrap(),vec![[4;32]]);
        assert_eq!(log,vec![entry(1,Some(10)),entry(3,None),entry(2,Some(12)),entry(4,None)]);
    }
    #[test]
    fn ambiguous_chain_answer_is_atomic() {
        let mut log=vec![entry(1,Some(10)),entry(2,Some(12))];let before=log.clone();
        assert!(place_chain_after(&mut log,[1;32],&[[2;32],[2;32]]).is_err());assert_eq!(log,before);
        assert!(place_chain_after(&mut log,[1;32],&[[1;32]]).is_err());assert_eq!(log,before);
        log.push(entry(2,Some(13)));let before=log.clone();
        assert!(place_chain_after(&mut log,[1;32],&[[3;32]]).is_err());assert_eq!(log,before);
    }
    #[test]
    fn restore_retry_is_bounded_per_block_and_success_resets_only_that_block() {
        let mut r=ChainRestoreRetries::default();
        assert!(r.failed([1;32]));assert!(r.failed([1;32]));assert!(!r.failed([1;32]));
        assert!(!r.failed([1;32]));assert!(r.blocked(&[1;32]));assert!(r.failed([2;32]));
        r.succeeded(&[2;32]);assert!(r.blocked(&[1;32]));r.succeeded(&[1;32]);assert!(r.failed([1;32]));
    }
    #[test]
    fn replay_failures_retry_without_duplicate_or_cross_seed_completion() {
        let mut r=ReplayAttempt::new([1;32]);
        for _ in 0..3 {assert!(r.start());assert!(!r.start());assert!(r.finish([2;32],true).is_err());r.finish([1;32],false).unwrap();}
        assert!(r.exhausted());assert!(!r.start());
        r.reset_for_reorg([3;32]);assert!(r.start());assert!(r.finish([1;32],true).is_err());
        r.finish([3;32],true).unwrap();assert!(!r.start());
    }
    #[test]
    fn acceptance_recovery_never_overwrites_an_unreconciled_record() {
        let mut r=crate::trace_store::StoredTrace {
            version:1,response_txid:"a".repeat(64),model_id:[1;32],tier:0,request_hash:[2;32],
            max_tokens:2048,submitted_daa:7,accepted_hash:Some("b".repeat(64)),
            opening:Some(crate::trace_store::StoredOpening {seed_hash:"c".repeat(64),deadline_daa:42,payload:"00ff".into()}),
            trace:crate::trace_producer::ResponseTrace::test_sample(),
        };
        let id=r.response_txid.clone();let old=r.accepted_hash.clone();
        assert!(note_verified_acceptance(&mut r,&id,&"e".repeat(64)).is_err());assert_eq!(r.accepted_hash,old);
        r.accepted_hash=None;assert!(note_verified_acceptance(&mut r,&id,&"e".repeat(64)).is_err());
        r.opening=None;assert!(note_verified_acceptance(&mut r,&id,&"e".repeat(64)).unwrap());
        assert!(!note_verified_acceptance(&mut r,&id,&"e".repeat(64)).unwrap());
        assert!(note_verified_acceptance(&mut r,&"f".repeat(64),&"e".repeat(64)).is_err());
    }
}
