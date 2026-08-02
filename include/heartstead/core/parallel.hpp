#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace heartstead {

template <typename Function>
void parallel_for(std::size_t count, Function&& function) {
    if (count == 0) return;
    const auto hardware_threads = std::max(1U, std::thread::hardware_concurrency());
    const auto available_threads = hardware_threads > 2U ? hardware_threads - 2U : 1U;
    const auto worker_count = std::min(count, static_cast<std::size_t>(available_threads));
    std::atomic_size_t next_index{0};
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                const auto index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= count) return;
                function(index);
            }
        });
    }
}

} // namespace heartstead
