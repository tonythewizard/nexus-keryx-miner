#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// All spans are borrowed for one synchronous call. No callback may retain them.
typedef struct NexusH15Capture {
  const int32_t *tokens;
  const uint32_t *ubatch;
  const uint8_t *leaves;
  const uint8_t *sequence;
  size_t positions, leaves_per_position, sequence_count;
  uint32_t prompt_positions;
} NexusH15Capture;
typedef int (*NexusH15Weight)(void *, uint64_t, uint8_t *, uint8_t *, size_t *);
typedef int (*NexusH15Replay)(void *, const NexusH15Capture *, uint32_t,
                              const uint64_t *, size_t, uint32_t, uint32_t,
                              uint8_t *, size_t, size_t *);
typedef struct NexusH15Api {
  void *user;
  NexusH15Weight weight;
  NexusH15Replay replay;
  uint8_t model_root[32];
  uint64_t chunks;
} NexusH15Api;
typedef struct NexusH15Buffer {
  uint8_t *data;
  size_t length;
} NexusH15Buffer;
int nexus_h15_capture(const NexusH15Capture *, uint8_t, const uint8_t *,
                      uint32_t, uint32_t, const uint8_t *, size_t, double,
                      void **, uint8_t *);
int nexus_h15_plaintext(void *, NexusH15Buffer *);
int nexus_h15_prefix(uint8_t, const uint8_t *, int32_t *, size_t *, uint32_t *);
int nexus_h15_open(void *, const NexusH15Api *, const uint8_t *,
                   const uint8_t *, const uint64_t *, size_t, const uint8_t *,
                   size_t, NexusH15Buffer *);
int nexus_h15_encode_capture(void *, NexusH15Buffer *);
int nexus_h15_decode_capture(const uint8_t *, size_t, void **);
void nexus_h15_capture_free(void *);
const char *nexus_h15_error(void);
int nexus_h15_selftest(void *, const NexusH15Api *, uint32_t, uint32_t *);
int nexus_h15_service_selftest(void *, void *, const NexusH15Api *, uint32_t *);
void nexus_h15_buffer_free(NexusH15Buffer);
#ifdef __cplusplus
}
#endif
#ifdef __cplusplus
extern "C" {
#endif
int nexus_h15_service_open(const uint8_t *, size_t, const uint8_t *, uint8_t,
                           const NexusH15Api *, const uint8_t *, void **);
void nexus_h15_service_free(void *);
int nexus_h15_service_commit(void *, void *, const uint8_t *, size_t, uint64_t,
                             const uint8_t *, uint64_t);
int nexus_h15_service_anchors(void *, NexusH15Buffer *);
int nexus_h15_service_local_estimates(void *);
int nexus_h15_service_awaited_ids(void *, uint64_t, NexusH15Buffer *);
int nexus_h15_service_apply_awaited(void *, const uint8_t *, size_t);
int nexus_h15_service_next_span(void *, uint32_t, uint32_t);
int nexus_h15_service_chain(void *, const uint8_t *, size_t);
int nexus_h15_service_advance(void *, const NexusH15Api *, uint64_t, uint64_t);
int nexus_h15_service_restoring(void *);
int nexus_h15_service_refund(void *, const uint8_t *, size_t);
uint64_t nexus_h15_service_estimate(const void *, uint32_t, uint32_t);
int nexus_h15_service_response_due(void *, uint64_t, uint64_t,
                                   NexusH15Buffer *);
int nexus_h15_service_response_intent(void *, const uint8_t *, uint64_t);
int nexus_h15_service_response_result(void *, const uint8_t *, uint8_t);
int nexus_h15_service_response_for_request(const void *, const uint8_t *,
                                           NexusH15Buffer *);
int nexus_h15_service_due(void *, uint64_t, uint64_t, NexusH15Buffer *);
#ifdef __cplusplus
}
#endif

#if defined(__cplusplus) && INTPTR_MAX == INT64_MAX
static_assert(sizeof(NexusH15Capture) == 64);
static_assert(offsetof(NexusH15Capture, prompt_positions) == 56);
static_assert(sizeof(NexusH15Api) == 64);
static_assert(sizeof(NexusH15Buffer) == 16);
#endif
