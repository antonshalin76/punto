/**
 * @file runtime_tuning.hpp
 * @brief Runtime budget helpers for multi-daemon deployments
 */

#pragma once

#include <algorithm>
#include <cstddef>

namespace punto {

struct AnalysisThreadBudget {
  std::size_t worker_threads = 1;
  std::size_t daemon_count = 1;
  bool manual_override = false;
};

[[nodiscard]] inline AnalysisThreadBudget
compute_analysis_thread_budget(std::size_t hardware_threads,
                               std::size_t daemon_count,
                               std::size_t analysis_threads_override,
                               std::size_t max_threads_per_daemon) noexcept {
  daemon_count = std::max<std::size_t>(daemon_count, 1);

  if (analysis_threads_override > 0) {
    return {
        /*worker_threads=*/analysis_threads_override,
        /*daemon_count=*/daemon_count,
        /*manual_override=*/true,
    };
  }

  hardware_threads = std::max<std::size_t>(hardware_threads, 1);
  const std::size_t reserved_cores = hardware_threads > 1 ? 1 : 0;
  const std::size_t shared_budget =
      std::max<std::size_t>(hardware_threads - reserved_cores, 1);

  std::size_t worker_threads = shared_budget / daemon_count;
  if (worker_threads == 0) {
    worker_threads = 1;
  }

  worker_threads = std::min(worker_threads,
                            std::max<std::size_t>(max_threads_per_daemon, 1));

  return {
      /*worker_threads=*/worker_threads,
      /*daemon_count=*/daemon_count,
      /*manual_override=*/false,
  };
}

} // namespace punto
