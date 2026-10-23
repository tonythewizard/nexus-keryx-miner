#pragma once

#include <grpc/grpc.h>
#include <openssl/crypto.h>
#include <stdexcept>

namespace keryx::nexus::network {

// Keep one process-owned gRPC reference alive until all application RPC
// objects have been destroyed, then wait for final event-engine teardown
// before OpenSSL's process-exit cleanup runs.
class GrpcRuntime final {
public:
  GrpcRuntime() {
    // The distro gRPC event engine can retain process-wide workers past the
    // final channel release. OpenSSL's atexit cleanup races those workers
    // (observed as heap corruption in OPENSSL_cleanup). OpenSSL explicitly
    // supports process-lifetime allocations when NO_ATEXIT is selected; the
    // OS reclaims them after all process threads exit.
    if (OPENSSL_init_crypto(OPENSSL_INIT_NO_ATEXIT, nullptr) != 1)
      throw std::runtime_error("OpenSSL process-lifetime initialization failed");
    grpc_init();
  }
  ~GrpcRuntime() { grpc_shutdown(); }
  GrpcRuntime(const GrpcRuntime &) = delete;
  GrpcRuntime &operator=(const GrpcRuntime &) = delete;
};

} // namespace keryx::nexus::network
