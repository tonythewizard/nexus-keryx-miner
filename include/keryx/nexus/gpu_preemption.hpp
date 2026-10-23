#pragma once

#include <atomic>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace keryx::nexus {

// Gives OPoI exclusive ownership of the authorized GPU at a PoM batch boundary.
// Admission closes before the challenge waits for the currently running batch.
class GpuPreemptionCoordinator final {
 public:
  template <class F>
  bool run_pom(F &&operation) {
    std::unique_lock lock(mutex_);
    if (opoi_pending_.load(std::memory_order_acquire) ||
        stopping_.load(std::memory_order_acquire)) return false;
    std::forward<F>(operation)();
    return true;
  }

  void announce_opoi() noexcept {
    if (!stopping_.load(std::memory_order_acquire))
      opoi_pending_.store(true, std::memory_order_release);
  }

  template <class Release, class Inference, class Restore>
  auto run_opoi(Release &&release, Inference &&inference, Restore &&restore)
      -> decltype(std::forward<Inference>(inference)()) {
    using Result = decltype(std::forward<Inference>(inference)());
    if (stopping_.load(std::memory_order_acquire))
      throw std::runtime_error("GPU preemption rejected after cooperative stop");
    announce_opoi();
    std::unique_lock lock(mutex_);
    if (stopping_.load(std::memory_order_acquire)) {
      opoi_pending_.store(false, std::memory_order_release);
      throw std::runtime_error("GPU preemption cancelled by cooperative stop");
    }
    try {
      std::forward<Release>(release)();
    } catch (...) {
      const auto release_error = std::current_exception();
      try { std::forward<Restore>(restore)(); }
      catch (...) {
        opoi_pending_.store(false, std::memory_order_release);
        throw std::runtime_error("GPU PoM restoration failed after release error");
      }
      opoi_pending_.store(false, std::memory_order_release);
      std::rethrow_exception(release_error);
    }
    if constexpr (std::is_void_v<Result>) {
      std::exception_ptr inference_error;
      try { std::forward<Inference>(inference)(); }
      catch (...) { inference_error = std::current_exception(); }
      try { std::forward<Restore>(restore)(); }
      catch (...) {
        opoi_pending_.store(false, std::memory_order_release);
        throw std::runtime_error("GPU PoM resource restoration failed after OPoI");
      }
      opoi_pending_.store(false, std::memory_order_release);
      if (inference_error) std::rethrow_exception(inference_error);
    } else {
      std::optional<Result> result;
      std::exception_ptr inference_error;
      try { result.emplace(std::forward<Inference>(inference)()); }
      catch (...) { inference_error = std::current_exception(); }
      try { std::forward<Restore>(restore)(); }
      catch (...) {
        opoi_pending_.store(false, std::memory_order_release);
        throw std::runtime_error("GPU PoM resource restoration failed after OPoI");
      }
      opoi_pending_.store(false, std::memory_order_release);
      if (inference_error) std::rethrow_exception(inference_error);
      return std::move(*result);
    }
  }

  void request_stop() noexcept {
    stopping_.store(true, std::memory_order_release);
    opoi_pending_.store(true, std::memory_order_release);
  }
  [[nodiscard]] bool opoi_pending() const noexcept {
    return opoi_pending_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool stopping() const noexcept {
    return stopping_.load(std::memory_order_acquire);
  }

 private:
  std::mutex mutex_;
  std::atomic_bool opoi_pending_{false};
  std::atomic_bool stopping_{false};
};

}  // namespace keryx::nexus
