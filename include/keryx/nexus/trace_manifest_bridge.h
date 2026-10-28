#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Read-only query against pinned network parameters. This grants no worker admission.
typedef struct NexusTraceManifestInfo {
  uint32_t application_abi, n_products, position_leaves, n_attn, n_recur, heads, interval;
  uint8_t manifest_root[32];
} NexusTraceManifestInfo;
// network: 0 mainnet, 1 testnet. Manifest capacity is bytes; layout capacity is
// rows of five u32 (layer, kind, slot, heads, head_dim). Both length pointers and
// info are required. Return 2 queries required sizes without writing buffers;
// return 0 fills both, negative values reject invalid arguments or inactive era.
int32_t nexus_ai_h15_manifest(uint32_t network,uint32_t tier,uint64_t accepted_daa,
    uint8_t *manifest,size_t manifest_capacity,size_t *manifest_len,
    uint32_t *layers,size_t layer_capacity,size_t *layer_count,NexusTraceManifestInfo *info);
// Pure deadline duration query; the accepted phase/seed DAA is supplied separately.
uint64_t nexus_ai_attention_phase_window(uint8_t tier,uint32_t max_tokens,uint32_t n_prompt,uint32_t n_pos);
#ifdef __cplusplus
}
static_assert(sizeof(NexusTraceManifestInfo)==60);
#endif
