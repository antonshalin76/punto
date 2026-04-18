/**
 * @file layout_sync_sound.hpp
 * @brief Helper state for playing layout-switch sound after external hotkeys.
 */

#pragma once

#include <chrono>

namespace punto {

struct ExternalLayoutSoundState {
  bool pending = false;
  std::chrono::steady_clock::time_point deadline{};
};

inline void arm_external_layout_sound(
    ExternalLayoutSoundState &state,
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds window = std::chrono::milliseconds{1500}) {
  state.pending = true;
  state.deadline = now + window;
}

inline void clear_external_layout_sound(ExternalLayoutSoundState &state) {
  state.pending = false;
  state.deadline = {};
}

[[nodiscard]] inline bool external_layout_sound_expired(
    const ExternalLayoutSoundState &state,
    std::chrono::steady_clock::time_point now) {
  return state.pending && now > state.deadline;
}

[[nodiscard]] inline bool should_play_external_layout_sound(
    const ExternalLayoutSoundState &state,
    std::chrono::steady_clock::time_point now,
    int previous_layout,
    int synced_layout) {
  if (!state.pending || now > state.deadline) {
    return false;
  }

  if ((synced_layout != 0 && synced_layout != 1) ||
      synced_layout == previous_layout) {
    return false;
  }

  return true;
}

} // namespace punto
