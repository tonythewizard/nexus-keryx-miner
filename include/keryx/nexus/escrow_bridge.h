#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *nexus_escrow_watcher_new(const uint8_t *secret_hex, size_t secret_len,
                              const uint8_t *payout, size_t payout_len,
                              const uint8_t *state_path, size_t state_path_len);
int32_t nexus_escrow_claim_outpoints(void*, uint64_t, uint8_t*, size_t, size_t*);
int32_t nexus_escrow_verified_claim(void*, const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t, size_t*);
void nexus_escrow_watcher_free(void *handle);
int32_t nexus_escrow_set_claims_enabled(void *handle, int32_t enabled);
// `capacity_bytes` is measured in bytes; output is `count` consecutive 32-byte hashes.
int32_t nexus_escrow_start_validation(void *handle, uint8_t *out,
                                      size_t capacity_bytes, size_t *count);
int32_t nexus_escrow_validation_result(void *handle, const uint8_t hash[32],
                                       int32_t is_chain_block);
int32_t nexus_escrow_validation_error(void *handle, const uint8_t *message,
                                      size_t message_len);
int32_t nexus_escrow_chain_block_removed(void *handle, const uint8_t hash[32]);
int32_t nexus_escrow_handle_block(void *handle, const uint8_t *block_wire,
                                  size_t block_len, uint8_t *claim_out,
                                  size_t claim_capacity, size_t *claim_len,
                                  uint64_t *pending_count, uint64_t *pending_amount);
int32_t nexus_escrow_track_inference_outpoint(void *handle, const uint8_t *txid,
                                              size_t txid_len, uint64_t daa,
                                              uint64_t amount);
int32_t nexus_escrow_submit_result(void *handle, const uint8_t *txid,
                                   size_t txid_len, const uint8_t *error,
                                   size_t error_len);
int32_t nexus_escrow_validation_in_progress(void *handle);
int32_t nexus_escrow_flush(void *handle);

#ifdef __cplusplus
}
#endif
