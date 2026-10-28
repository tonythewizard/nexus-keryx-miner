//! Incremental H15 acceptance/draw recovery, mirroring upstream a7eb9e4.
//! Callers supply verified selected-chain answers, not ACKs. This module never
//! authorizes submission or deletes a journal record after a transient RPC error.
use std::collections::{BTreeMap, BTreeSet};

pub type Hash = [u8; 32];
pub const REPLAY_MAX_FAILURES: u32 = 2;
pub const TRACE_UNDRAWN_LIMIT_DAA: u64 = 72_000;
pub const CHAIN_RESTORE_PAGE_MIN: usize = 100;
pub const REPLAY_URGENT_BASE_DAA: u64 = 1_500;

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

/// Transient restore failures keep obligations and retry with capped backoff.
#[derive(Default)]
pub struct ChainRestoreRetries { failures: BTreeMap<Hash,u32> }
impl ChainRestoreRetries {
    pub fn failed(&mut self, start: Hash) -> u64 {
        let failures = self.failures.entry(start).or_default();
        *failures = failures.saturating_add(1);
        (5u64 << (*failures - 1).min(4)).min(60)
    }
    pub fn succeeded(&mut self, start: &Hash) { self.failures.remove(start); }
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

/// Borrowed phase state supplied from verified acceptance/reorg recovery. An
/// RPC ACK alone must never populate accepted_blocks. This adds no disk format.
#[derive(Clone, Copy)]
pub struct PendingAttention<'a> {
    pub bodies: &'a [Vec<u8>],
    pub accepted_blocks: &'a [Option<Hash>],
    pub path: &'a [u64],
    pub deadline_daa: u64,
}
impl PendingAttention<'_> {
    /// Verified accepting block whose next child has not yet been drawn.
    pub fn awaiting_draw(&self) -> Result<Option<&Hash>, &'static str> {
        let n=self.bodies.len();
        if n==0 || n!=self.accepted_blocks.len() || self.path.len()>n {
            return Err("inconsistent attention phase state; preserve trace");
        }
        let last=keryx_verifiable_inference::opening::AttnOpening::decode(&self.bodies[n-1])
            .ok_or("malformed attention phase; preserve trace")?;
        if last.path.len()+1!=n || !self.path.starts_with(&last.path) {
            return Err("attention path not reconciled; preserve trace");
        }
        if self.path.len()+1!=n || last.partials.is_empty() {return Ok(None);}
        Ok(self.accepted_blocks[n-1].as_ref())
    }
}

/// Upstream pending-trace retention: a passed deadline cannot evict a trace
/// while an accepted attention phase awaits its chain draw. Errors mean blocked
/// recovery with the original record retained; this function never deletes it.
pub fn retain_pending_trace(daa:u64,draw_deadline:u64,attention:Option<PendingAttention<'_>>)
    ->Result<bool,&'static str> {
    match attention {
        None=>Ok(daa<=draw_deadline),
        Some(a)=>Ok(daa<=draw_deadline.max(a.deadline_daa) ||
            (a.awaiting_draw()?.is_some() && daa<=a.deadline_daa.saturating_add(2*crate::trace_producer::TRACE_SEED_DELAY_DAA))),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn body(path:Vec<u64>,partials:usize)->Vec<u8> {
        keryx_verifiable_inference::opening::AttnOpening{path,partials:vec![7;partials],..Default::default()}.encode()
    }
    #[test]
    fn accepted_phase_survives_deadline_until_its_child_is_drawn() {
        let bodies=vec![body(vec![],8)];let accepted=[Some([1;32])];
        let phase=PendingAttention{bodies:&bodies,accepted_blocks:&accepted,path:&[],deadline_daa:120};
        assert_eq!(phase.awaiting_draw(),Ok(Some(&[1;32])));
        assert_eq!(retain_pending_trace(320,100,Some(phase)),Ok(true));
        assert_eq!(retain_pending_trace(321,100,Some(phase)),Ok(false));
        let drawn=PendingAttention{path:&[2],..phase};
        assert_eq!(drawn.awaiting_draw(),Ok(None));
        assert_eq!(retain_pending_trace(120,100,Some(drawn)),Ok(true));
        assert_eq!(retain_pending_trace(121,100,Some(drawn)),Ok(false));
    }
    #[test]
    fn unaccepted_reorged_and_final_phases_do_not_wait_for_a_child() {
        let bodies=vec![body(vec![],8)];let unknown=[None];
        let phase=PendingAttention{bodies:&bodies,accepted_blocks:&unknown,path:&[],deadline_daa:120};
        assert_eq!(retain_pending_trace(121,100,Some(phase)),Ok(false));
        let final_body=vec![body(vec![],0)];let accepted=[Some([1;32])];
        let complete=PendingAttention{bodies:&final_body,accepted_blocks:&accepted,..phase};
        assert_eq!(retain_pending_trace(121,100,Some(complete)),Ok(false));
        assert_eq!(retain_pending_trace(100,100,None),Ok(true));
        assert_eq!(retain_pending_trace(101,100,None),Ok(false));
    }
    #[test]
    fn indeterminate_phase_state_blocks_eviction_without_mutating_input() {
        let bodies=vec![body(vec![],8)];let accepted=[Some([1;32])];
        let phase=PendingAttention{bodies:&bodies,accepted_blocks:&accepted,path:&[2,3],deadline_daa:120};
        assert!(retain_pending_trace(121,100,Some(phase)).is_err());
        let bad=vec![vec![255]];
        assert!(retain_pending_trace(121,100,Some(PendingAttention{bodies:&bad,path:&[],..phase})).is_err());
        assert_eq!(accepted,[Some([1;32])]);assert_eq!(bodies,vec![body(vec![],8)]);
    }
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
    fn restore_retry_backoff_preserves_obligations_and_resets_per_block() {
        let mut r=ChainRestoreRetries::default();
        for delay in [5,10,20,40,60,60,60] {assert_eq!(r.failed([1;32]),delay);}
        assert_eq!(r.failed([2;32]),5);r.succeeded(&[2;32]);
        assert_eq!(r.failed([1;32]),60);r.succeeded(&[1;32]);assert_eq!(r.failed([1;32]),5);
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

/// Last accepted phase is the restore anchor; absent that, use response
/// acceptance and finally the submission-time chain anchor.
pub fn restore_anchor(response:Option<Hash>,anchor:Option<Hash>,phases:&[Option<Hash>])->Option<Hash>{
    phases.iter().rev().flatten().next().copied().or(response).or(anchor)
}
pub fn restore_continuation(added:&[Hash])->Option<Hash>{
    (added.len()>=CHAIN_RESTORE_PAGE_MIN).then(||added.last().copied()).flatten()
}
pub fn retain_undrawn(now:u64,submitted:u64,_accepted:bool,_backfill_window:u64)->bool {
 submitted.saturating_add(TRACE_UNDRAWN_LIMIT_DAA)>now
}
/// Replay gets the GPU before a queued request if both would exhaust its margin.
pub fn replay_urgent(now:u64,deadline:u64,next_request:u64,replay_work:u64)->bool {
    deadline.saturating_sub(now)<=REPLAY_URGENT_BASE_DAA.saturating_add(next_request).saturating_add(replay_work)
}
/// Submission bookkeeping counts only pieces whose transport was attempted.
/// A failed send leaves that piece and all later unsent pieces at zero attempts.
pub fn opening_attempts_after_send(piece_count:usize,failed_at:Option<usize>)->Result<Vec<u32>,&'static str>{
    if failed_at.is_some_and(|i|i>=piece_count){return Err("invalid opening piece index");}
    let sent=failed_at.unwrap_or(piece_count);Ok((0..piece_count).map(|i|u32::from(i<sent)).collect())
}
/// Parked tasks retain their handles and immutable transaction bytes across
/// reconnects; response/publication IDs remain answered and cannot be requeued.
#[derive(Default)]
pub struct ParkedWork<Q,I,R,P,T> {
    pub requests:Q,pub inference:Option<I>,pub replay:Option<R>,pub publication:Option<P>,
    pub responses:BTreeMap<Hash,T>,pub request_escrows:BTreeMap<Hash,(Hash,u64)>,pub answered:BTreeSet<Hash>,
}
impl<Q,I,R,P,T> ParkedWork<Q,I,R,P,T> {
    pub fn note_answered(&mut self,publication:Option<Hash>){
        self.answered.extend(self.responses.keys().copied());self.answered.extend(publication);
    }
    pub fn note_request_escrow(&mut self,request:Hash,outpoint:Hash,reward:u64,limit:usize)->Result<(),&'static str>{
        if limit==0{return Err("empty escrow identity budget");}
        if reward>0 {if self.request_escrows.len()>=limit{self.request_escrows.clear();}self.request_escrows.insert(request,(outpoint,reward));}
        Ok(())
    }
}
/// Replay products are keyed by both draw seed and path, so a late completion
/// cannot attach an old phase to a new draw. One pending phase mirrors upstream.
#[derive(Default)]
pub struct BuiltPhase {phase:Option<(Hash,Vec<u64>,Vec<u8>)>}
impl BuiltPhase {
    pub fn park(&mut self,seed:Hash,path:Vec<u64>,body:Vec<u8>)->Result<(),&'static str>{
        if body.len().saturating_add(33)>keryx_verifiable_inference::opening::MAX_BODY_LEN
            || keryx_verifiable_inference::opening::AttnOpening::decode(&body).is_none_or(|o|o.path!=path)
        {return Err("invalid cached attention phase");}
        self.phase=Some((seed,path,body));Ok(())
    }
    pub fn take(&mut self,seed:Hash,path:&[u64])->Option<Vec<u8>> {
        if self.phase.as_ref().is_some_and(|(s,p,_)|*s==seed&&p==path){self.phase.take().map(|(_,_,b)|b)}else{None}
    }
}
#[cfg(test)]
mod refresh_tests {
    use super::*;
    #[test] fn restore_anchors_pages_horizon_and_replay_priority(){
        assert_eq!(restore_anchor(Some([1;32]),Some([2;32]),&[Some([3;32]),None,Some([4;32])]),Some([4;32]));
        assert_eq!(restore_anchor(None,Some([2;32]),&[]),Some([2;32]));
        assert_eq!(restore_continuation(&[[1;32];99]),None);assert_eq!(restore_continuation(&[[1;32];100]),Some([1;32]));
        assert!(retain_undrawn(72006,7,true,100));assert!(!retain_undrawn(72007,7,true,100));
        assert!(replay_urgent(100,2100,200,300));assert!(!replay_urgent(100,2101,200,300));
    }
    #[test] fn reconnect_retains_handles_answers_and_bounded_escrow_ids(){
        let mut p=ParkedWork::<Vec<Hash>,u64,u64,Hash,Vec<u8>>::default();p.inference=Some(1);p.replay=Some(2);p.publication=Some([3;32]);p.responses.insert([4;32],vec![9]);p.note_answered(p.publication);
        assert!(p.answered.contains(&[3;32])&&p.answered.contains(&[4;32]));assert_eq!(p.replay,Some(2));
        p.note_request_escrow([1;32],[9;32],1,1).unwrap();p.note_request_escrow([2;32],[8;32],1,1).unwrap();assert_eq!(p.request_escrows.len(),1);assert!(p.request_escrows.contains_key(&[2;32]));
        assert_eq!(opening_attempts_after_send(4,Some(1)),Ok(vec![1,0,0,0]));assert_eq!(opening_attempts_after_send(2,None),Ok(vec![1,1]));
        assert!(opening_attempts_after_send(2,Some(2)).is_err());
    }
    #[test] fn replay_cache_refuses_other_seed_or_path_and_preserves_pending(){
        let mut p=BuiltPhase::default();let b=keryx_verifiable_inference::opening::AttnOpening{path:vec![1],..Default::default()}.encode();
        p.park([1;32],vec![1],b.clone()).unwrap();assert!(p.take([2;32],&[1]).is_none());assert!(p.take([1;32],&[2]).is_none());assert_eq!(p.take([1;32],&[1]),Some(b));assert!(p.take([1;32],&[1]).is_none());
    }
}

/// In-memory continuation retries survive transient failures without evicting
/// trace obligations. The caller supplies a monotonic millisecond clock and
/// rechecks whether each anchor/page is still needed before issuing RPC.
#[derive(Default)]
pub struct ChainRestorePlan {
    retry:ChainRestoreRetries,due:BTreeMap<Hash,u64>,continuations:BTreeSet<Hash>,
}
impl ChainRestorePlan {
    pub fn failed(&mut self,start:Hash,now_ms:u64)->u64 {
        let delay=self.retry.failed(start)*1000;self.due.insert(start,now_ms.saturating_add(delay));delay
    }
    pub fn succeeded(&mut self,start:Hash,added:&[Hash])->Option<Hash> {
        self.retry.succeeded(&start);self.due.remove(&start);self.continuations.remove(&start);
        let next=restore_continuation(added);if let Some(next)=next{self.continuations.insert(next);}next
    }
    pub fn ready(&mut self,now_ms:u64,restoring:bool,needed:impl Fn(&Hash)->bool)->Vec<Hash>{
        let due:Vec<_>=self.due.iter().filter(|(_,at)|**at<=now_ms).map(|(h,_)|*h).collect();let mut out=Vec::new();
        for h in due {self.due.remove(&h);if needed(&h)||(restoring&&self.continuations.contains(&h)){out.push(h);}else{self.retry.succeeded(&h);self.continuations.remove(&h);}}
        out
    }
}
/// A reorg may drop a phase that is later accepted again before replay finishes.
/// Only previously dropped tracked transactions can acquire a late acceptance.
#[derive(Default)]
pub struct LatePhases {dropped:BTreeSet<Hash>,accepted:BTreeMap<Hash,Hash>}
impl LatePhases {
    pub fn dropped(&mut self,tx:Hash){self.dropped.insert(tx);}
    pub fn note_verified(&mut self,tx:Hash,block:Hash)->bool {
        if !self.dropped.contains(&tx){return false;}self.accepted.insert(tx,block);true
    }
    pub fn reorg(&mut self,removed:&BTreeSet<Hash>){self.accepted.retain(|_,b|!removed.contains(b));}
    pub fn take(&mut self,tx:Hash)->Option<Hash>{self.accepted.remove(&tx)}
}
#[cfg(test)]
mod continuation_tests {
    use super::*;
    #[test] fn a_failed_continuation_retries_while_an_audit_still_needs_it(){
        let mut p=ChainRestorePlan::default();let next=p.succeeded([1;32],&[[2;32];100]).unwrap();assert_eq!(next,[2;32]);
        assert_eq!(p.failed(next,1000),5000);assert!(p.ready(5999,true,|_|false).is_empty());assert_eq!(p.ready(6000,true,|_|false),vec![next]);
        p.failed(next,6000);assert!(p.ready(16000,false,|_|false).is_empty());assert_eq!(p.failed(next,16000),5000);
    }
    #[test] fn late_phase_acceptance_is_bound_and_invalidated_by_reorg(){
        let mut p=LatePhases::default();assert!(!p.note_verified([1;32],[2;32]));p.dropped([1;32]);assert!(p.note_verified([1;32],[2;32]));
        p.reorg(&BTreeSet::from([[2;32]]));assert_eq!(p.take([1;32]),None);p.note_verified([1;32],[3;32]);assert_eq!(p.take([1;32]),Some([3;32]));
    }
}

/// Official earliest-deadline work accounting, including accepted undrawn audits.
pub fn replays_urgent(now:u64,next:u64,jobs:&[(u64,u64,bool)])->bool{let mut jobs=jobs.to_vec();jobs.sort_unstable();let mut at=now.saturating_add(1500).saturating_add(next);let mut ready=false;jobs.into_iter().filter(|j|j.0>now).any(|(deadline,work,startable)|{at=at.saturating_add(work);ready|=startable;at>=deadline&&ready})}
pub fn replay_backoff_ms(failures:u32)->u64{30000u64.saturating_mul(1u64<<failures.min(5)).min(600000)}

#[cfg(test)]mod new_refresh_tests{
 use super::*;
 #[test]fn replay_budget_includes_earlier_and_undrawn_audits(){
  assert!(replays_urgent(100,100,&[(2100,300,true),(2150,300,true)]));
  assert!(!replays_urgent(100,100,&[(2100,300,true)]));
  assert!(!replays_urgent(100,100,&[(1800,300,false)]));
  assert!(replays_urgent(100,100,&[(1800,300,false),(2200,300,true)]));
  assert!(!replays_urgent(100,100,&[(100,300,true)]));
 }
 #[test]fn failed_replays_back_off_without_giving_up_after_two(){assert_eq!(replay_backoff_ms(1),60000);assert_eq!(replay_backoff_ms(2),120000);assert_eq!(replay_backoff_ms(5),600000);assert_eq!(replay_backoff_ms(u32::MAX),600000);}
 #[test]fn unaccepted_responses_share_the_full_ledger_horizon(){assert!(retain_undrawn(70000,0,false,6000));assert!(!retain_undrawn(72000,0,false,6000));}
}
