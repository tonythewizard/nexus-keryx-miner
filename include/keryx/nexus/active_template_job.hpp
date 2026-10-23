#pragma once
#include "keryx/nexus/pipeline.hpp"
#include <utility>

namespace keryx::nexus {
// Single mining-thread ownership: reuse the snapshot without copying its RPC
// envelopes between batches. A received replacement is always adopted, even
// within the same generation. Admission/submission gates remain external.
inline TemplateJob *select_active_template_job(std::optional<TemplateJob> &active,
                                               std::optional<TemplateJob> incoming) {
  if (incoming) active = std::move(incoming);
  return active && active->lease.valid() ? &*active : nullptr;
}
} // namespace keryx::nexus
