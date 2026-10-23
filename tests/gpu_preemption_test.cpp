#include "keryx/nexus/gpu_preemption.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

int main() {
  try {
    keryx::nexus::GpuPreemptionCoordinator coordinator;
    std::mutex mutex;
    std::condition_variable changed;
    bool pom_entered = false;
    bool release_pom = false;
    bool inference_entered = false;
    bool release_inference = false;
    std::vector<std::string> order;
    std::atomic_bool second_pom_ran{false};
    std::atomic_bool second_pom_during_inference{false};
    std::atomic_bool first_pom_ran{false};
    std::atomic_bool opoi_ok{false};

    std::thread pom([&] {
      const bool ran = coordinator.run_pom([&] {
        std::unique_lock lock(mutex);
        order.push_back("pom-active");
        pom_entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release_pom; });
        order.push_back("pom-drained");
      });
      first_pom_ran.store(ran, std::memory_order_release);
    });
    {
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, 2s, [&] { return pom_entered; }))
        throw std::runtime_error("PoM critical section did not start");
    }

    std::thread opoi([&] {
      const auto result = coordinator.run_opoi(
          [&] { std::lock_guard lock(mutex); order.push_back("pom-released"); },
          [&] {
            std::unique_lock lock(mutex);
            order.push_back("opoi-inference");
            inference_entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release_inference; });
            return std::string("fixture-response");
          },
          [&] { std::lock_guard lock(mutex); order.push_back("pom-restored"); });
      opoi_ok.store(result == "fixture-response", std::memory_order_release);
    });
    const auto pending_deadline = std::chrono::steady_clock::now() + 2s;
    while (!coordinator.opoi_pending() && std::chrono::steady_clock::now() < pending_deadline)
      std::this_thread::yield();
    if (!coordinator.opoi_pending()) throw std::runtime_error("challenge did not close PoM admission");

    std::thread queued_pom([&] {
      second_pom_ran.store(coordinator.run_pom([&] {
        std::lock_guard lock(mutex);
        if (inference_entered && !release_inference)
          second_pom_during_inference.store(true, std::memory_order_release);
        order.push_back("pom-after-opoi");
      }), std::memory_order_release);
    });
    {
      std::lock_guard lock(mutex);
      release_pom = true;
    }
    changed.notify_all();
    {
      std::unique_lock lock(mutex);
      if (!changed.wait_for(lock, 2s, [&] { return inference_entered; }))
        throw std::runtime_error("OPoI did not wait for active PoM then take GPU ownership");
      if (order.size() < 3 || order[0] != "pom-active" || order[1] != "pom-drained" ||
          order[2] != "pom-released")
        throw std::runtime_error("PoM resources were not released at the drained batch boundary");
    }
    {
      std::lock_guard lock(mutex);
      release_inference = true;
    }
    changed.notify_all();
    pom.join();
    queued_pom.join();
    opoi.join();
    if (!first_pom_ran.load(std::memory_order_acquire))
      throw std::runtime_error("initial PoM batch was unexpectedly denied");
    if (!opoi_ok.load(std::memory_order_acquire)) throw std::runtime_error("OPoI result changed");
    if (second_pom_during_inference.load(std::memory_order_acquire))
      throw std::runtime_error("a PoM batch ran during OPoI inference");
    // A queued caller can acquire the mutex either before OPoI (and be
    // correctly denied while the challenge is pending) or after restoration
    // (and run). Do not require mutex fairness; the explicit resumed batch
    // below verifies that admission is reopened after OPoI completes.
    if (coordinator.opoi_pending()) throw std::runtime_error("OPoI admission flag was not cleared");
    const bool resumed = coordinator.run_pom([&] {
      std::lock_guard lock(mutex);
      order.push_back("pom-resumed");
    });
    if (!resumed || order.back() != "pom-resumed")
      throw std::runtime_error("PoM did not resume after OPoI restoration");

    bool restored_after_error = false;
    try {
      (void)coordinator.run_opoi([] {}, []() -> int { throw std::runtime_error("fixture failure"); },
                                  [&] { restored_after_error = true; });
      throw std::runtime_error("OPoI failure fixture unexpectedly succeeded");
    } catch (const std::runtime_error &e) {
      if (std::string(e.what()) != "fixture failure") throw;
    }
    if (!restored_after_error || coordinator.opoi_pending())
      throw std::runtime_error("PoM was not restored after inference failure");
    coordinator.request_stop();
    if (coordinator.run_pom([] {})) throw std::runtime_error("PoM admitted after cooperative stop");

    std::cout << "GPU_OPOI_PREEMPTION=PASS in_flight_pom_drained=true admission_paused=true "
                 "pom_resources_released=true inference_exclusive=true restore_after_error=true "
                 "pom_resumed=true cooperative_stop=true\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "GPU_OPOI_PREEMPTION=FAIL " << e.what() << '\n';
    return 1;
  }
}
