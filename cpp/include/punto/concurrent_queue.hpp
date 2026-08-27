/**
 * @file concurrent_queue.hpp
 * @brief Простейшая потокобезопасная очередь для обмена сообщениями между потоками
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

namespace punto {

template <class T> class ConcurrentQueue {
public:
  ConcurrentQueue() = default;

  ConcurrentQueue(const ConcurrentQueue &) = delete;
  ConcurrentQueue &operator=(const ConcurrentQueue &) = delete;

  void push(T value) {
    push(std::move(value), [](T &) noexcept {});
  }

  template <class OnCommit> void push(T value, OnCommit &&on_commit) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      q_.push_back(std::move(value));
      try {
        std::forward<OnCommit>(on_commit)(q_.back());
      } catch (...) {
        q_.pop_back();
        throw;
      }
    }
    cv_.notify_one();
  }

  [[nodiscard]] bool try_pop(T &out) {
    std::lock_guard<std::mutex> lock(mu_);
    if (q_.empty()) {
      return false;
    }
    out = std::move(q_.front());
    q_.pop_front();
    return true;
  }

  [[nodiscard]] std::optional<T> pop_wait(std::stop_token st) {
    std::unique_lock<std::mutex> lock(mu_);

    cv_.wait(lock, st, [this] { return !q_.empty(); });

    if (q_.empty()) {
      return std::nullopt;
    }

    T value = std::move(q_.front());
    q_.pop_front();
    return value;
  }

  void notify_all() { cv_.notify_all(); }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return q_.size();
  }

  template <class Predicate>
  [[nodiscard]] std::vector<T> extract_if(Predicate &&predicate) {
    std::vector<T> extracted;
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = q_.begin(); it != q_.end();) {
      if (std::forward<Predicate>(predicate)(*it)) {
        extracted.push_back(std::move(*it));
        it = q_.erase(it);
      } else {
        ++it;
      }
    }
    return extracted;
  }

private:
  mutable std::mutex mu_;
  std::condition_variable_any cv_;
  std::deque<T> q_;
};

} // namespace punto
