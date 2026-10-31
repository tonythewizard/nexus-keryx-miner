# H15 parallel proof construction and post-submit Rust auditing

The CUDA resident H15 path performs its independent CPU/GPU walk and target
checks before submission. Proof tiles and Merkle paths are constructed with a
fixed eight-worker pool; NEXUS_POM_PROOF_THREADS selects 1 through 8 (default 8).
All jobs are joined before reader failures propagate. The canonical proof and
Borsh wire formats are unchanged. NEXUS_POM_BATCH_SIZE accepts multiples of 256
from 256 through 18432; its default remains 4608.

One immutable H15 submission may be in flight. RPC streams persist across
successful requests, each with a monotonically increasing correlation ID.
Timeout, AI cancellation, write/read failure or an incorrect ID discard the
stream. Ambiguous candidates are not retried; only a new candidate can reconnect.
A live freshness predicate is checked before reserving and immediately before
Write. Expiry after Write does not cancel the ACK wait; stop, AI and audit failure
still cancel an in-flight request. ACK means a correlated node response, not
chain acceptance or a paid reward.

Rust auditing is deferred until the corresponding RPC attempt completes.
Scheduling an async submission alone is not treated as completed submission.
The initial startup sample is audited synchronously; subsequent audits run in
a bounded queue of at most four tasks. A failed RPC attempt still gets audited.
Artifact persistence is part of that audit. Rust verifier access is serialized
across H14, H15 and other users of the same verifier instance.

An audit error permanently blocks new calls through the shared submission
client for that process. Already transmitted candidates cannot be recalled.
The original Rust-before-submit guarantee therefore does not apply to the H15
fast path: CPU/GPU parity and model/Merkle/header/target checks precede sending,
while independent official Rust verification follows it. This ordering was
explicitly requested. Shadow and disabled submission modes remain disabled.
Shutdown drains the pending submission and every audit before destroying the
verifier, telemetry and captured resources. AI/stop/stale proof cancellation is
an expected discard; integrity failures remain errors.

Validation includes synthetic and authentic GGUF parallel/serial byte equality,
independent Rust/Borsh fixture checks, exception propagation, audit ordering and
queue bounds, sticky failure, and a loopback authentic-proof RPC mock covering
reuse, correlation mismatch, reconnect, timeout, AI cancellation, pre-Write
expiry and duplicate prevention. No new mainnet or GPU-execution result is
implied by these offline tests. No general speedup has been established.
