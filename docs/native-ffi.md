# Native bridge ownership and memory contract

The Rust bridge exports C entry points declared in `escrow_bridge.h`,
`h15_bridge.h` and `trace_manifest_bridge.h`. This contract applies to the
unsafe entry points, including read-only verification. Return codes and
protocol authorization remain specific to each function.

## Borrowed spans and outputs

Every non-null pointer must identify a live allocation, aligned for its element
type. An input span must be readable for its entire declared byte or element
length throughout the call. Lengths must fit the allocation and the platform's
pointer-offset range. A null pointer is usable only on a documented zero-length,
size-query or error path; it does not create an allocation. Bounds and null checks
cannot establish that an arbitrary non-null address is valid.

Writable outputs must have their declared capacity. Scalar/struct output slots
must be aligned and valid for a complete value. Input/output length pointers
must be initialized to the destination capacity where specified. Mutable outputs
must not overlap any borrowed input, handle, context, callback state or another
output. Fixed hashes and raw secp256k1 keys occupy 32 bytes; certificates occupy
64 bytes and response CIDs 34 bytes. Individual functions document other fixed
sizes. Length/count units matter: a manifest layer is FIVE u32 values and a
response-ID count describes consecutive 32-byte hashes.

## Handles, allocations and callbacks

Opaque service, watcher and capture pointers must originate from the matching
constructor/decoder in the same loaded bridge and retain their original type.
They cannot refer to freed, relocated or foreign objects. The caller owns each
returned handle once and must use its matching destructor. Freeing a null handle
is permitted by the matching free functions. No call may race with destruction.

The caller serializes mutable access to watchers/services, including query
entry points that borrow them mutably. Shared access cannot overlap mutation.
Capture borrows remain valid throughout synchronous encoding, verification and
replay. Callback user state and every nested array must outlive the call that
uses them. Callbacks cannot unwind across the C boundary or reenter a service
that is already borrowed. GPU replay additionally requires the existing identity,
capability and exclusive-owner gates; a valid pointer grants no GPU permission.

`Buffer` results transfer a Rust-owned boxed byte slice. Release the exact
unchanged pointer/length pair once with `nexus_h15_buffer_free`; do not use a
C allocator, modify the length, or free the storage independently. Release old
output ownership before overwriting an output slot. Error strings from
`nexus_h15_error` are borrowed thread-local data, valid only until that thread's
next error update; copy them before retaining them and never free them.

Panic guards handle Rust errors; they cannot recover from invalid foreign
pointers, aliasing, use after free or an exception escaping a foreign callback.
Memory validity is separate from proof/signature validity, durable intent,
canonical chain acceptance and permission to submit. All of those existing
checks remain necessary.
