#include "punto/word_editor.hpp"

#include "punto/clipboard_manager.hpp"
#include "punto/key_entry_text.hpp"
#include "punto/macro_lock.hpp"
#include "punto/text_processor.hpp"
#include "punto/x11_session.hpp"

#include <xcb/xinput.h>
#include <xcb/xtest.h>
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define explicit explicit_value
#include <xcb/xkb.h>
#undef explicit
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace punto {
namespace {

#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
extern "C" void punto_e2e_after_paste_receipt_arm();
extern "C" void punto_e2e_after_word_dispatch();
extern "C" void punto_e2e_before_post_dispatch_wait();
extern "C" void punto_e2e_after_internal_layout();
extern "C" void punto_e2e_before_internal_layout_change();
extern "C" void punto_e2e_before_internal_layout_restore();
extern "C" void
punto_e2e_before_internal_layout_work(std::int64_t cleanup_start_ns);
extern "C" std::int64_t
punto_e2e_internal_layout_cleanup_start(std::int64_t cleanup_start_ns);
extern "C" void punto_e2e_before_raw_key_observation();
extern "C" void punto_e2e_after_raw_key_observation();
extern "C" void punto_e2e_before_word_replay();
#endif

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr auto kClipboardBudget = std::chrono::milliseconds{10};
constexpr auto kMacroBudget = std::chrono::milliseconds{300};
constexpr auto kLayoutTransitionBudget = std::chrono::milliseconds{1000};
constexpr auto kTotalMacroBudget = std::chrono::milliseconds{3500};
constexpr auto kExtendedLayoutClientSettle = std::chrono::milliseconds{250};
constexpr auto kFastLayoutClientSettle = std::chrono::milliseconds{5};
constexpr auto kSelectionClientSettle = std::chrono::milliseconds{30};
constexpr auto kInternalLayoutCleanupBudget = std::chrono::milliseconds{100};
constexpr auto kRawKeyObservationBudget = std::chrono::milliseconds{5};
constexpr std::size_t kMaxRawKeyEvents = 256;
constexpr std::size_t kMaxCharacters = 128;
constexpr xcb_keycode_t kShift = KEY_LEFTSHIFT + 8;
constexpr xcb_keycode_t kLeft = KEY_LEFT + 8;
constexpr xcb_keycode_t kRight = KEY_RIGHT + 8;
constexpr xcb_keycode_t kBackspace = KEY_BACKSPACE + 8;
constexpr xcb_keycode_t kTab = KEY_TAB + 8;
constexpr xcb_keycode_t kControl = KEY_LEFTCTRL + 8;
constexpr xcb_keycode_t kPaste = KEY_V + 8;
constexpr xcb_keycode_t kUndo = KEY_Z + 8;

template <typename T> using Reply = std::unique_ptr<T, decltype(&std::free)>;

template <typename Function> class ScopeExit {
public:
  explicit ScopeExit(Function function) : function_(std::move(function)) {}
  ScopeExit(const ScopeExit &) = delete;
  ScopeExit &operator=(const ScopeExit &) = delete;
  ~ScopeExit() { function_(); }

private:
  Function function_;
};

template <typename T>
Reply<T> reply(BoundedXcbConnection &connection, unsigned int sequence,
               Deadline deadline,
               x11_detail::XcbOperationResult *operation_result = nullptr) {
  x11_detail::XcbOperationResult local_result{};
  auto &result = operation_result == nullptr ? local_result : *operation_result;
  return Reply<T>{
      static_cast<T *>(connection.wait_for_reply(sequence, deadline, result)),
      &std::free};
}

bool checked(BoundedXcbConnection &connection, xcb_void_cookie_t cookie,
             Deadline deadline,
             x11_detail::XcbOperationResult *operation_result = nullptr) {
  x11_detail::XcbOperationResult local_result{};
  auto &result = operation_result == nullptr ? local_result : *operation_result;
  return connection.check_request(cookie, deadline, result);
}

struct Stroke {
  std::string text;
  xcb_keycode_t key;
  std::uint8_t group;
  bool shifted;
};

struct KeyboardPlan {
  std::vector<Stroke> alphabet;
  std::array<std::uint8_t, 256> modifier_masks{};
  std::uint8_t num_lock_mask = 0;
  std::uint8_t caps_lock_mask = 0;
};

std::optional<KeyboardPlan> make_keyboard_plan(BoundedXcbConnection &connection,
                                               Deadline deadline) {
  if (!connection.is_open()) {
    return std::nullopt;
  }
  const auto *setup = xcb_get_setup(connection.get());
  const auto count =
      static_cast<std::uint8_t>(setup->max_keycode - setup->min_keycode + 1);
  auto map = reply<xcb_get_keyboard_mapping_reply_t>(
      connection,
      xcb_get_keyboard_mapping(connection.get(), setup->min_keycode, count)
          .sequence,
      deadline);
  if (!map || map->keysyms_per_keycode < 4) {
    return std::nullopt;
  }
  const auto *symbols = xcb_get_keyboard_mapping_keysyms(map.get());
  const auto symbol_at = [&](xcb_keycode_t key, unsigned int column) {
    const auto row = static_cast<std::size_t>(key - setup->min_keycode);
    return symbols[row * map->keysyms_per_keycode + column];
  };
  if (kBackspace < setup->min_keycode || kRight > setup->max_keycode ||
      symbol_at(kShift, 0) != XKB_KEY_Shift_L ||
      symbol_at(kControl, 0) != XKB_KEY_Control_L ||
      symbol_at(kLeft, 0) != XKB_KEY_Left ||
      symbol_at(kRight, 0) != XKB_KEY_Right ||
      symbol_at(kBackspace, 0) != XKB_KEY_BackSpace) {
    return std::nullopt;
  }

  auto modifiers = reply<xcb_get_modifier_mapping_reply_t>(
      connection, xcb_get_modifier_mapping(connection.get()).sequence,
      deadline);
  if (!modifiers) {
    return std::nullopt;
  }
  KeyboardPlan plan;
  const auto *modifier_keys =
      xcb_get_modifier_mapping_keycodes(modifiers.get());
  for (unsigned int slot = 0; slot < 8; ++slot) {
    for (unsigned int i = 0; i < modifiers->keycodes_per_modifier; ++i) {
      const auto key =
          modifier_keys[slot * modifiers->keycodes_per_modifier + i];
      plan.modifier_masks[key] |= static_cast<std::uint8_t>(1U << slot);
    }
  }
  bool caps_found = false;
  bool only_caps = true;
  bool shift_found = false;
  bool control_found = false;
  for (unsigned int i = 0; i < modifiers->keycodes_per_modifier; ++i) {
    shift_found |= modifier_keys[i] == kShift;
    control_found |=
        modifier_keys[2U * modifiers->keycodes_per_modifier + i] == kControl;
    const auto key = modifier_keys[modifiers->keycodes_per_modifier + i];
    if (key == 0) {
      continue;
    }
    if (key < setup->min_keycode || key > setup->max_keycode) {
      only_caps = false;
      continue;
    }
    caps_found = true;
    only_caps &= symbol_at(key, 0) == XKB_KEY_Caps_Lock;
    for (unsigned int column = 0; column < map->keysyms_per_keycode; ++column) {
      const auto symbol = symbol_at(key, column);
      only_caps &= symbol == XKB_KEY_NoSymbol || symbol == XKB_KEY_Caps_Lock;
    }
  }
  if (!shift_found || !control_found) {
    return std::nullopt;
  }
  if (caps_found && only_caps) {
    plan.caps_lock_mask = XCB_MOD_MASK_LOCK;
  }
  for (unsigned int slot = 3; slot < 8; ++slot) {
    bool found = false;
    bool only_num_lock = true;
    for (unsigned int i = 0; i < modifiers->keycodes_per_modifier; ++i) {
      const auto key =
          modifier_keys[slot * modifiers->keycodes_per_modifier + i];
      if (key == 0) {
        continue;
      }
      if (key < setup->min_keycode || key > setup->max_keycode) {
        only_num_lock = false;
        break;
      }
      bool num_lock_key = false;
      for (unsigned int column = 0; column < map->keysyms_per_keycode;
           ++column) {
        const auto symbol = symbol_at(key, column);
        num_lock_key |= symbol == XKB_KEY_Num_Lock;
        only_num_lock &=
            symbol == XKB_KEY_NoSymbol || symbol == XKB_KEY_Num_Lock;
      }
      only_num_lock &= num_lock_key;
      found = true;
    }
    if (found && only_num_lock) {
      plan.num_lock_mask |= static_cast<std::uint8_t>(1U << slot);
    }
  }

  for (unsigned int code = KEY_1; code <= KEY_SPACE; ++code) {
    if (code != KEY_SPACE &&
        (code > KEY_SLASH || kScancodeToChar[code] == '\0')) {
      continue;
    }
    const auto key = static_cast<xcb_keycode_t>(code + 8);
    if (key < setup->min_keycode || key > setup->max_keycode) {
      return std::nullopt;
    }
    for (std::uint8_t group = 0; group < 2; ++group) {
      for (unsigned int shift = 0; shift < 2; ++shift) {
        const KeyEntry entry{static_cast<std::uint16_t>(code), shift != 0};
        const auto expected = key_entries_to_visible_text_checked(
            std::span<const KeyEntry>{&entry, 1}, group);
        if (!expected || expected->empty()) {
          continue;
        }
        std::array<char, 8> bytes{};
        const int length = xkb_keysym_to_utf8(
            symbol_at(key, group * 2U + shift), bytes.data(), bytes.size());
        const bool matches =
            length > 1 &&
            *expected == std::string_view{bytes.data(),
                                          static_cast<std::size_t>(length - 1)};
        // Reject swapped or non-QWERTY language groups before any action.
        const char qwerty = kScancodeToChar[code];
        if (!matches && qwerty >= 'a' && qwerty <= 'z') {
          return std::nullopt;
        }
        if (matches) {
          plan.alphabet.push_back(Stroke{*expected, key, group, shift != 0});
        }
      }
    }
  }
  if (kTab >= setup->min_keycode && kTab <= setup->max_keycode &&
      symbol_at(kTab, 0) == XKB_KEY_Tab) {
    plan.alphabet.push_back(Stroke{"\t", kTab, 0, false});
  }
  return plan;
}

bool resolve_locked_levels(BoundedXcbConnection &connection, std::uint8_t locks,
                           std::uint8_t num_lock_mask,
                           std::span<Stroke> expected,
                           std::span<Stroke> replacement, Deadline deadline) {
  constexpr std::uint16_t parts = XCB_XKB_MAP_PART_KEY_TYPES |
                                  XCB_XKB_MAP_PART_KEY_SYMS |
                                  XCB_XKB_MAP_PART_VIRTUAL_MODS;
  auto map = reply<xcb_xkb_get_map_reply_t>(
      connection,
      xcb_xkb_get_map(connection.get(), XCB_XKB_ID_USE_CORE_KBD, parts, 0, 0, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
          .sequence,
      deadline);
  if (!map || (map->present & parts) != parts || map->firstType != 0 ||
      map->nTypes == 0) {
    return false;
  }
  xcb_xkb_get_map_map_t contents{};
  xcb_xkb_get_map_map_unpack(xcb_xkb_get_map_map(map.get()), map->nTypes,
                             map->nKeySyms, map->nKeyActions, map->totalActions,
                             map->totalKeyBehaviors, map->virtualMods,
                             map->totalKeyExplicit, map->totalModMapKeys,
                             map->totalVModMapKeys, map->present, &contents);
  std::array<std::uint8_t, 16> virtual_masks{};
  const auto *virtual_values = xcb_xkb_get_map_map_vmods_rtrn(&contents);
  for (unsigned int i = 0; i < virtual_masks.size(); ++i) {
    if ((map->virtualMods & (1U << i)) != 0) {
      virtual_masks[i] = *virtual_values++;
    }
  }
  struct Type {
    const xcb_xkb_key_type_t *value = nullptr;
    std::uint8_t mask = 0;
    bool valid = false;
  };
  std::array<Type, 256> key_types{};
  auto types = xcb_xkb_get_map_map_types_rtrn_iterator(map.get(), &contents);
  for (unsigned int i = 0; types.rem > 0; ++i, xcb_xkb_key_type_next(&types)) {
    if (i >= map->nTypes || types.data->numLevels == 0 ||
        (types.data->mods_vmods & ~map->virtualMods) != 0) {
      return false;
    }
    std::uint8_t mask = types.data->mods_mask | types.data->mods_mods;
    bool resolved = true;
    for (unsigned int bit = 0; bit < virtual_masks.size(); ++bit) {
      if ((types.data->mods_vmods & (1U << bit)) != 0) {
        resolved &= virtual_masks[bit] != 0;
        mask |= virtual_masks[bit];
      }
    }
    key_types[i] =
        Type{types.data, mask,
             resolved && (mask & num_lock_mask) == 0 &&
                 (mask & ~(XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_LOCK)) == 0};
  }
  std::bitset<256> needed;
  needed.set(kShift);
  needed.set(kLeft);
  needed.set(kRight);
  needed.set(kBackspace);
  needed.set(kControl);
  needed.set(kPaste);
  needed.set(kUndo);
  for (const auto &stroke : expected) {
    needed.set(stroke.key);
  }
  for (const auto &stroke : replacement) {
    needed.set(stroke.key);
  }
  auto keys = xcb_xkb_get_map_map_syms_rtrn_iterator(map.get(), &contents);
  unsigned int key = map->firstKeySym;
  for (; keys.rem > 0; ++key, xcb_xkb_key_sym_map_next(&keys)) {
    if (key >= needed.size()) {
      return false;
    }
    if (!needed.test(key)) {
      continue;
    }
    const unsigned int groups = keys.data->groupInfo & 0x0fU;
    if (groups == 0 || groups > std::size(keys.data->kt_index)) {
      return false;
    }
    // Prove all declared groups, including those reached by group redirects.
    // A non-neutral unused group is conservatively unsupported as well.
    for (unsigned int group = 0; group < groups; ++group) {
      const auto type = keys.data->kt_index[group];
      if (type >= map->nTypes || !key_types[type].valid) {
        return false;
      }
    }
    const auto solve = [&](Stroke &stroke) {
      if (stroke.key != key) {
        return true;
      }
      const auto &type = key_types[keys.data->kt_index[stroke.group % groups]];
      const unsigned int desired = stroke.shifted ? 1U : 0U;
      for (unsigned int shift = 0; shift < 2; ++shift) {
        const auto mask =
            (locks | (shift ? XCB_MOD_MASK_SHIFT : 0)) & type.mask;
        unsigned int level = 0;
        auto entries = xcb_xkb_key_type_map_iterator(type.value);
        for (; entries.rem > 0; xcb_xkb_kt_map_entry_next(&entries)) {
          if (entries.data->active && entries.data->mods_mask == mask) {
            level = entries.data->level;
          }
        }
        if (level == desired && level < type.value->numLevels) {
          stroke.shifted = shift != 0;
          return true;
        }
      }
      return false;
    };
    for (auto &stroke : expected) {
      if (!solve(stroke)) {
        return false;
      }
    }
    for (auto &stroke : replacement) {
      if (!solve(stroke)) {
        return false;
      }
    }
    needed.reset(key);
  }
  return needed.none();
}

std::optional<std::vector<Stroke>>
plan_text(std::string_view text, const std::vector<Stroke> &alphabet,
          std::uint8_t preferred_group) {
  if (text.empty() || text.size() > kMaxCharacters * 2) {
    return std::nullopt;
  }
  std::vector<Stroke> plan;
  while (!text.empty() && plan.size() < kMaxCharacters) {
    const Stroke *found = nullptr;
    for (const auto &stroke : alphabet) {
      if (stroke.group == preferred_group && text.starts_with(stroke.text)) {
        found = &stroke;
        break;
      }
    }
    for (const auto &stroke : alphabet) {
      if (found != nullptr) {
        break;
      }
      if (text.starts_with(stroke.text)) {
        found = &stroke;
        break;
      }
    }
    if (found == nullptr) {
      return std::nullopt;
    }
    plan.push_back(*found);
    preferred_group = found->group;
    text.remove_prefix(found->text.size());
  }
  return text.empty() ? std::optional{std::move(plan)} : std::nullopt;
}

std::optional<xcb_window_t>
focus(BoundedXcbConnection &connection, Deadline deadline,
      x11_detail::XcbOperationResult *result = nullptr) {
  if (!connection.is_open()) {
    if (result != nullptr) {
      *result = x11_detail::XcbOperationResult::ConnectionFailed;
    }
    return std::nullopt;
  }
  auto value = reply<xcb_get_input_focus_reply_t>(
      connection, xcb_get_input_focus(connection.get()).sequence, deadline,
      result);
  if (!value || value->focus == XCB_WINDOW_NONE ||
      value->focus == XCB_INPUT_FOCUS_POINTER_ROOT) {
    return std::nullopt;
  }
  return value->focus;
}

struct IdleKeyboardState {
  int group;
  std::uint8_t locked_mods;
  bool operator==(const IdleKeyboardState &) const = default;
};

struct KeyboardStateSnapshot {
  int group;
  std::uint8_t mods;
  std::uint8_t base_mods;
  std::uint8_t latched_mods;
  std::uint8_t locked_mods;
  std::int16_t base_group;
  std::int16_t latched_group;
  std::uint16_t pointer_buttons;
};

std::optional<IdleKeyboardState>
idle_layout(BoundedXcbConnection &connection, std::uint8_t allowed_locks,
            Deadline deadline, x11_detail::XcbOperationResult *result = nullptr,
            KeyboardStateSnapshot *snapshot = nullptr) {
  if (!connection.is_open()) {
    if (result != nullptr) {
      *result = x11_detail::XcbOperationResult::ConnectionFailed;
    }
    return std::nullopt;
  }
  auto state = reply<xcb_xkb_get_state_reply_t>(
      connection,
      xcb_xkb_get_state(connection.get(), XCB_XKB_ID_USE_CORE_KBD).sequence,
      deadline, result);
  if (state && snapshot != nullptr) {
    *snapshot = KeyboardStateSnapshot{state->group,        state->mods,
                                      state->baseMods,     state->latchedMods,
                                      state->lockedMods,   state->baseGroup,
                                      state->latchedGroup, state->ptrBtnState};
  }
  if (!state || (state->mods & ~allowed_locks) != 0 || state->baseMods != 0 ||
      state->latchedMods != 0 || (state->lockedMods & ~allowed_locks) != 0 ||
      state->baseGroup != 0 || state->latchedGroup != 0 || state->group > 1 ||
      state->ptrBtnState != 0) {
    return std::nullopt;
  }
  return IdleKeyboardState{state->group, state->lockedMods};
}

enum class LayoutHotkeyResult {
  Accepted,
  InvalidRequest,
  DeadlineExpired,
  XcbFailure,
};

LayoutHotkeyResult send_layout_hotkey(BoundedXcbConnection &connection,
                                      std::uint16_t modifier, std::uint16_t key,
                                      Deadline deadline) {
  if (!connection.is_open()) {
    return LayoutHotkeyResult::XcbFailure;
  }
  if (Clock::now() >= deadline) {
    return LayoutHotkeyResult::DeadlineExpired;
  }
  if (!is_modifier(modifier) || modifier == key) {
    return LayoutHotkeyResult::InvalidRequest;
  }
  const auto modifier_key = static_cast<xcb_keycode_t>(modifier + 8U);
  const auto trigger_key = static_cast<xcb_keycode_t>(key + 8U);
  const std::array events{
      xcb_test_fake_input_checked(connection.get(), XCB_KEY_PRESS, modifier_key,
                                  XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0, 0),
      xcb_test_fake_input_checked(connection.get(), XCB_KEY_PRESS, trigger_key,
                                  XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0, 0),
      xcb_test_fake_input_checked(connection.get(), XCB_KEY_RELEASE,
                                  trigger_key, XCB_CURRENT_TIME,
                                  XCB_WINDOW_NONE, 0, 0, 0),
      xcb_test_fake_input_checked(connection.get(), XCB_KEY_RELEASE,
                                  modifier_key, XCB_CURRENT_TIME,
                                  XCB_WINDOW_NONE, 0, 0, 0),
  };
  for (const auto cookie : events) {
    x11_detail::XcbOperationResult operation_result{};
    if (!checked(connection, cookie, deadline, &operation_result)) {
      return operation_result == x11_detail::XcbOperationResult::TimedOut
                 ? LayoutHotkeyResult::DeadlineExpired
                 : LayoutHotkeyResult::XcbFailure;
    }
  }
  return LayoutHotkeyResult::Accepted;
}

bool set_internal_layout(BoundedXcbConnection &connection, int group,
                         Deadline deadline) {
  if (!connection.is_open() || Clock::now() >= deadline) {
    return false;
  }
  return checked(connection,
                 xcb_xkb_latch_lock_state_checked(
                     connection.get(), XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1,
                     static_cast<std::uint8_t>(group), 0, 0, 0),
                 deadline);
}

bool tap(BoundedXcbConnection &connection, xcb_keycode_t key, bool shifted,
         Deadline deadline, bool control = false, bool *queued = nullptr) {
  if (queued != nullptr) {
    *queued = false;
  }
  if (!connection.is_open() || Clock::now() >= deadline) {
    return false;
  }
  std::array<xcb_void_cookie_t, 6> cookies{};
  std::size_t used = 0;
  const auto send = [&](std::uint8_t type, xcb_keycode_t code) {
    cookies[used++] =
        xcb_test_fake_input_checked(connection.get(), type, code,
                                    XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0, 0);
  };
  // Queue each complete stroke, including modifier release, before flushing.
  if (control) {
    send(XCB_KEY_PRESS, kControl);
  }
  if (shifted) {
    send(XCB_KEY_PRESS, kShift);
  }
  send(XCB_KEY_PRESS, key);
  send(XCB_KEY_RELEASE, key);
  if (shifted) {
    send(XCB_KEY_RELEASE, kShift);
  }
  if (control) {
    send(XCB_KEY_RELEASE, kControl);
  }
  if (queued != nullptr) {
    *queued = true;
  }
  for (std::size_t i = 0; i < used; ++i) {
    if (!checked(connection, cookies[i], deadline)) {
      return false;
    }
  }
  return true;
}

bool same_selection(const SelectionRead &left, const SelectionRead &right) {
  return left.owner == right.owner && left.text == right.text &&
         left.owner_generation == right.owner_generation &&
         left.selection_timestamp == right.selection_timestamp;
}

bool terminal_text(std::string_view text) {
  for (std::size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if (byte < 0x20U || byte == 0x7fU ||
        (byte == 0xc2U && i + 1 < text.size() &&
         static_cast<unsigned char>(text[i + 1]) >= 0x80U &&
         static_cast<unsigned char>(text[i + 1]) <= 0x9fU)) {
      return false;
    }
  }
  return true;
}

} // namespace

struct WordEditor::PendingPaste {
  std::string previous;
  std::uint64_t generation = 0;
  PasteReceiptToken receipt;
};

struct WordEditor::RetainedWordSelection {
  SelectionRead selection;
  xcb_window_t focus_window = XCB_WINDOW_NONE;
  std::uint64_t session_generation = 0;
};

WordEditor::WordEditor(X11Session &session, WaitFunction wait)
    : session_(session), wait_(std::move(wait)) {}

WordEditor::~WordEditor() = default;

bool WordEditor::busy() const noexcept { return pending_ != nullptr; }

void WordEditor::pump() {
  if (!clipboard_) {
    return;
  }
  const auto lease = session_.acquire_write_lease();
  if (!lease || lease->generation() != clipboard_session_) {
    clipboard_.reset();
    pending_.reset();
    retained_word_selection_.reset();
    clipboard_session_ = 0;
    return;
  }
  (void)clipboard_->pump_events();
  if (!pending_) {
    return;
  }
  if (!clipboard_->is_open() ||
      !clipboard_->owns_generation(Selection::Clipboard,
                                   pending_->generation)) {
    clipboard_->cancel_paste_receipt(pending_->receipt);
    pending_.reset();
    return;
  }
  if (clipboard_->paste_receipt_seen(pending_->receipt)) {
    (void)clipboard_->restore_text_if_generation(
        Selection::Clipboard, pending_->generation, pending_->previous);
    clipboard_->cancel_paste_receipt(pending_->receipt);
    pending_.reset();
  }
}

void WordEditor::reset() {
  // A status/config reset cannot revoke a payload already requested by an app.
  // Session loss and foreign ownership are handled by the same pump authority.
  pump();
  retained_word_selection_.reset();
}

WordEditOutcome WordEditor::execute(const WordEditRequest &request) {
  WordEditOutcome outcome;
  const bool word = request.operation == WordEditOperation::Word;
  const bool paste_word =
      word && (request.expected.find('\t') != std::string::npos ||
               request.replacement.find('\t') != std::string::npos);
  const bool native_undo = request.operation == WordEditOperation::NativeUndo;
  const bool selection =
      request.operation == WordEditOperation::SelectionLayout ||
      request.operation == WordEditOperation::SelectionCase ||
      request.operation == WordEditOperation::SelectionTranslit;
  if ((!word && !native_undo && !selection) ||
      (word &&
       (request.expected.empty() || request.expected == request.replacement)) ||
      request.target_layout < -1 || request.target_layout > 1 ||
      request.source_layout < 0 || request.source_layout > 1 ||
      request.session_generation == 0 || request.source_locked_mods < -1 ||
      request.source_locked_mods > 255 ||
      !is_modifier(request.layout_hotkey_modifier) ||
      request.layout_hotkey_modifier == request.layout_hotkey_key) {
    return outcome;
  }
  pump();
  auto previous_selection = std::move(retained_word_selection_);
  outcome.rejection_stage = "busy";
  if (busy() && !native_undo) {
    return outcome;
  }
  const auto started = Clock::now();
  const auto hard_deadline = started + kTotalMacroBudget;
  auto deadline = started + kMacroBudget;
  outcome.rejection_stage = "macro_lock";
  MacroLock lock;
  MacroLockGuard guard{lock, std::chrono::milliseconds{0}};
  if (!guard.owns_lock()) {
    return outcome;
  }
  outcome.rejection_stage = "session";
  auto lease = session_.acquire_write_lease();
  if (!lease || !lease->valid() || !lease->info().wayland_display.empty() ||
      lease->generation() != request.session_generation) {
    return outcome;
  }
  outcome.rejection_stage = "connection";
  auto connection = lease->open_bounded_connection(deadline);
  if (!connection.is_open()) {
    return outcome;
  }
  outcome.rejection_stage = "xkb";
  auto extension = reply<xcb_xkb_use_extension_reply_t>(
      connection, xcb_xkb_use_extension(connection.get(), 1, 0).sequence,
      deadline);
  if (!extension || !extension->supported) {
    return outcome;
  }
  outcome.rejection_stage = "keymap";
  const auto keyboard = make_keyboard_plan(connection, deadline);
  if (!keyboard) {
    return outcome;
  }
  const auto *setup = xcb_get_setup(connection.get());
  if (setup == nullptr) {
    return outcome;
  }
  const auto min_keycode = setup->min_keycode;
  const auto max_keycode = setup->max_keycode;
  const auto hotkey_modifier =
      static_cast<unsigned int>(request.layout_hotkey_modifier) + 8U;
  const auto hotkey_key =
      static_cast<unsigned int>(request.layout_hotkey_key) + 8U;
  if (hotkey_modifier < min_keycode || hotkey_modifier > max_keycode ||
      hotkey_key < min_keycode || hotkey_key > max_keycode) {
    return outcome;
  }
  const auto hotkey_modifier_mask = keyboard->modifier_masks[hotkey_modifier];
  if (hotkey_modifier_mask == 0) {
    return outcome;
  }
  outcome.rejection_stage = "context";
  const auto allowed_locks = static_cast<std::uint8_t>(
      keyboard->num_lock_mask | keyboard->caps_lock_mask);
  const auto initial_state = idle_layout(connection, allowed_locks, deadline);
  const auto initial_focus = focus(connection, deadline);
  if (!initial_state || !initial_focus ||
      initial_state->group != request.source_layout ||
      (request.expected_focus != 0 &&
       request.expected_focus != *initial_focus) ||
      (request.source_locked_mods >= 0 &&
       request.source_locked_mods != initial_state->locked_mods)) {
    return outcome;
  }
  outcome.rejection_stage = "text_plan";
  auto expected =
      word ? plan_text(request.expected, keyboard->alphabet,
                       static_cast<std::uint8_t>(request.source_layout))
           : std::optional<std::vector<Stroke>>{std::vector<Stroke>{}};
  const auto replacement_group = static_cast<std::uint8_t>(
      request.target_layout < 0 ? request.source_layout
                                : request.target_layout);
  auto replacement =
      word && !request.replacement.empty()
          ? plan_text(request.replacement, keyboard->alphabet,
                      replacement_group)
          : std::optional<std::vector<Stroke>>{std::vector<Stroke>{}};
  if (!expected || !replacement ||
      !resolve_locked_levels(connection, initial_state->locked_mods,
                             keyboard->num_lock_mask, *expected, *replacement,
                             deadline)) {
    return outcome;
  }

  outcome.rejection_stage = "clipboard_init";
  const auto clipboard_time = [&] {
    return Clock::now() + kClipboardBudget < deadline;
  };
  if (!clipboard_) {
    clipboard_ = std::make_unique<ClipboardManager>(session_, kClipboardBudget);
    clipboard_session_ = lease->generation();
  }
  if (!clipboard_time() || !clipboard_->open(deadline) || !clipboard_time()) {
    return outcome;
  }
  outcome.rejection_stage = "active_window";
  const auto kind = clipboard_->active_window_kind();
  if (kind == ActiveWindowKind::Unknown ||
      (word && kind == ActiveWindowKind::Terminal && !request.allow_terminal) ||
      (word && kind == ActiveWindowKind::Terminal &&
       (!terminal_text(request.expected) ||
        !terminal_text(request.replacement))) ||
      (word && kind == ActiveWindowKind::Gui && replacement->empty())) {
    return outcome;
  }
  const bool observe_gui_word_transition =
      word && kind == ActiveWindowKind::Gui && request.target_layout >= 0 &&
      request.target_layout != request.source_layout;
  std::uint8_t xinput_opcode = 0;
  const auto prepare_transition_extensions = [&] {
    // Generated extension requests otherwise perform a blocking first lookup.
    outcome.rejection_stage = "xtest";
    xcb_prefetch_extension_data(connection.get(), &xcb_test_id);
    auto extension_barrier = reply<xcb_get_input_focus_reply_t>(
        connection, xcb_get_input_focus(connection.get()).sequence, deadline);
    if (!extension_barrier || !connection.is_open()) {
      return false;
    }
    const auto *xtest = xcb_get_extension_data(connection.get(), &xcb_test_id);
    if (xtest == nullptr || xtest->present == 0) {
      return false;
    }
    if (!observe_gui_word_transition) {
      return true;
    }
    outcome.rejection_stage = "xinput";
    xcb_prefetch_extension_data(connection.get(), &xcb_input_id);
    auto xinput_barrier = reply<xcb_get_input_focus_reply_t>(
        connection, xcb_get_input_focus(connection.get()).sequence, deadline);
    if (!xinput_barrier || !connection.is_open()) {
      return false;
    }
    const auto *xinput =
        xcb_get_extension_data(connection.get(), &xcb_input_id);
    if (xinput == nullptr || xinput->present == 0) {
      return false;
    }
    auto xinput_version = reply<xcb_input_xi_query_version_reply_t>(
        connection, xcb_input_xi_query_version(connection.get(), 2, 0).sequence,
        deadline);
    if (!xinput_version || !connection.is_open()) {
      return false;
    }
    struct RawKeyMask {
      xcb_input_event_mask_t header;
      std::uint32_t bits;
    } raw_key_mask{{XCB_INPUT_DEVICE_ALL_MASTER, 1},
                   XCB_INPUT_XI_EVENT_MASK_RAW_KEY_PRESS |
                       XCB_INPUT_XI_EVENT_MASK_RAW_KEY_RELEASE};
    const auto *current_setup = xcb_get_setup(connection.get());
    if (current_setup == nullptr) {
      return false;
    }
    const auto screen = xcb_setup_roots_iterator(current_setup);
    if (screen.rem == 0 || !checked(connection,
                                    xcb_input_xi_select_events_checked(
                                        connection.get(), screen.data->root, 1,
                                        &raw_key_mask.header),
                                    deadline)) {
      return false;
    }
    xinput_opcode = xinput->major_opcode;
    return true;
  };
  if (!prepare_transition_extensions()) {
    return outcome;
  }
  outcome.rejection_stage = "active_client";
  const auto active_client = clipboard_->active_client_id();
  const auto resource_mask = xcb_get_setup(connection.get())->resource_id_mask;
  const auto client_of = [&](xcb_window_t window) {
    return window & ~resource_mask;
  };
  auto owner = reply<xcb_get_selection_owner_reply_t>(
      connection,
      xcb_get_selection_owner(connection.get(), XCB_ATOM_PRIMARY).sequence,
      deadline);
  if (!active_client || !owner || client_of(*initial_focus) != *active_client) {
    return outcome;
  }
  bool previous_context_admitted = false;
  bool current_receipt_active = false;
  if (word && owner->owner != XCB_WINDOW_NONE &&
      client_of(owner->owner) == *active_client) {
    outcome.rejection_stage = "retained_selection_context";
    // Chromium retains PRIMARY after our temporary selection has collapsed.
    // Only our exact prior receipt can admit it; a new user selection cannot.
    if (kind != ActiveWindowKind::Gui || !previous_selection ||
        previous_selection->focus_window != *initial_focus ||
        previous_selection->session_generation != lease->generation() ||
        !clipboard_time()) {
      return outcome;
    }
    outcome.rejection_stage = "retained_selection_read";
    const auto current = clipboard_->get_text_with_owner(Selection::Primary);
    if (!current || !same_selection(previous_selection->selection, *current)) {
      return outcome;
    }
    previous_context_admitted = true;
  }
  const auto invalidate_retained_context = [&] {
    if (previous_context_admitted) {
      previous_selection.reset();
      previous_context_admitted = false;
    }
    if (current_receipt_active) {
      retained_word_selection_.reset();
    }
  };
  const auto preserve_previous_context = [&] {
    if (previous_context_admitted) {
      retained_word_selection_ = std::move(previous_selection);
      previous_context_admitted = false;
    }
  };
  const auto observation_failure_invalidates =
      [&](x11_detail::XcbOperationResult result,
          bool interrupted_wait_is_safe) {
        return !interrupted_wait_is_safe ||
               result != x11_detail::XcbOperationResult::TimedOut;
      };
  outcome.rejection_stage = "pointer";
  auto pointer = reply<xcb_query_pointer_reply_t>(
      connection, xcb_query_pointer(connection.get(), *initial_focus).sequence,
      deadline);
  if (!pointer) {
    invalidate_retained_context();
    return outcome;
  }
  outcome.source_layout = initial_state->group;
  outcome.target_layout =
      request.target_layout < 0 ? initial_state->group : request.target_layout;
  outcome.session_generation = lease->generation();
  outcome.focused_window = *initial_focus;
  int group = initial_state->group;
  bool cancelled = false;
  auto work_deadline = hard_deadline;
  const auto renew_budget = [&](auto budget) {
    const auto now = Clock::now();
    if (now >= deadline || now >= work_deadline) {
      return false;
    }
    deadline = std::min(work_deadline, now + budget);
    return true;
  };
  const auto wait = [&](Deadline until) {
    if (cancelled || Clock::now() >= deadline) {
      return false;
    }
    if (wait_) {
      try {
        cancelled = !wait_(std::min(until, deadline));
      } catch (...) {
        cancelled = true;
      }
    } else if (until > Clock::now()) {
      std::this_thread::sleep_until(std::min(until, deadline));
    }
    return !cancelled && Clock::now() < deadline;
  };
  enum class StableContextFailure {
    None,
    Wait,
    Session,
    FocusObservation,
    FocusChanged,
    PointerObservation,
    PointerChanged,
  };
  StableContextFailure stable_context_failure = StableContextFailure::None;
  const auto stable_context_matches = [&](bool interrupted_wait_is_safe = false,
                                          bool allow_focus_transition = false) {
    stable_context_failure = StableContextFailure::None;
    if (!wait(Clock::now())) {
      stable_context_failure = StableContextFailure::Wait;
      if (!interrupted_wait_is_safe) {
        invalidate_retained_context();
      }
      return false;
    }
    if (!connection.is_open() || !lease->valid()) {
      stable_context_failure = StableContextFailure::Session;
      invalidate_retained_context();
      return false;
    }
    x11_detail::XcbOperationResult focus_result{};
    const auto current_focus = focus(connection, deadline, &focus_result);
    if (!current_focus) {
      stable_context_failure = StableContextFailure::FocusObservation;
      if (observation_failure_invalidates(focus_result,
                                          interrupted_wait_is_safe)) {
        invalidate_retained_context();
      }
      return false;
    }
    const bool focus_changed = current_focus != initial_focus;
    if (focus_changed && !allow_focus_transition) {
      stable_context_failure = StableContextFailure::FocusChanged;
      invalidate_retained_context();
      return false;
    }
    x11_detail::XcbOperationResult pointer_result{};
    auto current = reply<xcb_query_pointer_reply_t>(
        connection,
        xcb_query_pointer(connection.get(), *initial_focus).sequence, deadline,
        &pointer_result);
    constexpr std::uint16_t buttons = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 |
                                      XCB_BUTTON_MASK_3 | XCB_BUTTON_MASK_4 |
                                      XCB_BUTTON_MASK_5;
    // Core pointer state also carries the XKB group. idle_layout validates
    // keyboard state against the group selected by this executor.
    if (!current) {
      stable_context_failure = StableContextFailure::PointerObservation;
      if (observation_failure_invalidates(pointer_result,
                                          interrupted_wait_is_safe)) {
        invalidate_retained_context();
      }
      return false;
    }
    const bool matches = current->root_x == pointer->root_x &&
                         current->root_y == pointer->root_y &&
                         (current->mask & buttons) == (pointer->mask & buttons);
    if (!matches) {
      stable_context_failure = StableContextFailure::PointerChanged;
      invalidate_retained_context();
      return false;
    }
    if (focus_changed) {
      stable_context_failure = StableContextFailure::FocusChanged;
      return false;
    }
    return true;
  };
  const auto context_matches = [&](bool interrupted_wait_is_safe = false) {
    if (!stable_context_matches(interrupted_wait_is_safe)) {
      return false;
    }
    x11_detail::XcbOperationResult layout_result{};
    const auto observed =
        idle_layout(connection, allowed_locks, deadline, &layout_result);
    if (!observed) {
      if (observation_failure_invalidates(layout_result,
                                          interrupted_wait_is_safe)) {
        invalidate_retained_context();
      }
      return false;
    }
    const bool matches =
        observed ==
        std::optional{IdleKeyboardState{group, initial_state->locked_mods}};
    if (!matches) {
      invalidate_retained_context();
    }
    return matches;
  };
  enum class HotkeyKeysResult { Clear, OwnChord, Foreign, ObservationFailed };
  const auto hotkey_keys = [&] {
    const auto keys = reply<xcb_query_keymap_reply_t>(
        connection, xcb_query_keymap(connection.get()).sequence, deadline);
    if (!keys) {
      return HotkeyKeysResult::ObservationFailed;
    }
    bool own_chord_down = false;
    for (unsigned int key = min_keycode; key <= max_keycode; ++key) {
      const bool down = (keys->keys[key / 8U] & (1U << (key % 8U))) != 0;
      if (down && key != hotkey_modifier && key != hotkey_key) {
        return HotkeyKeysResult::Foreign;
      }
      own_chord_down |= down;
    }
    return own_chord_down ? HotkeyKeysResult::OwnChord
                          : HotkeyKeysResult::Clear;
  };
  enum class RawKeyHistoryResult { Clear, Foreign, Incomplete, Failed };
  const auto observe_transition_raw_keys = [&](bool validate) {
    const auto observation_deadline =
        std::min(deadline, Clock::now() + kRawKeyObservationBudget);
    for (std::size_t count = 0; count < kMaxRawKeyEvents; ++count) {
      if (Clock::now() >= observation_deadline) {
        return RawKeyHistoryResult::Incomplete;
      }
      auto *event = xcb_poll_for_event(connection.get());
      if (event == nullptr) {
        return xcb_connection_has_error(connection.get()) == 0
                   ? RawKeyHistoryResult::Clear
                   : RawKeyHistoryResult::Failed;
      }
      const auto type = static_cast<std::uint8_t>(event->response_type & 0x7fU);
      bool foreign = false;
      if (validate && type == XCB_GE_GENERIC) {
        const auto *generic =
            reinterpret_cast<const xcb_ge_generic_event_t *>(event);
        if (generic->extension == xinput_opcode &&
            (generic->event_type == XCB_INPUT_RAW_KEY_PRESS ||
             generic->event_type == XCB_INPUT_RAW_KEY_RELEASE)) {
          const auto *raw =
              reinterpret_cast<const xcb_input_raw_key_press_event_t *>(event);
          foreign = raw->detail != hotkey_modifier && raw->detail != hotkey_key;
        }
      }
      std::free(event);
      if (foreign) {
        return RawKeyHistoryResult::Foreign;
      }
    }
    return RawKeyHistoryResult::Incomplete;
  };
  bool validate_transition_raw_keys = false;
  const auto ensure_layout = [&](int target) {
    if (target == group) {
      return context_matches();
    }
    if (!renew_budget(kLayoutTransitionBudget)) {
      outcome.rejection_stage = "layout_transition_budget";
      return false;
    }
    if (!context_matches()) {
      outcome.rejection_stage = "layout_transition_context_before";
      return false;
    }
    if (validate_transition_raw_keys) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      punto_e2e_before_raw_key_observation();
#endif
      const auto history = observe_transition_raw_keys(false);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      punto_e2e_after_raw_key_observation();
#endif
      if (history != RawKeyHistoryResult::Clear) {
        outcome.rejection_stage = history == RawKeyHistoryResult::Failed
                                      ? "layout_transition_raw_key_observation"
                                      : "layout_transition_raw_key_incomplete";
        invalidate_retained_context();
        return false;
      }
    }
    const auto hotkey =
        send_layout_hotkey(connection, request.layout_hotkey_modifier,
                           request.layout_hotkey_key, deadline);
    if (hotkey != LayoutHotkeyResult::Accepted) {
      outcome.rejection_stage = "layout_transition_hotkey";
      // A failed checked request may have sent only part of the chord. The
      // retained selection is safe only after a complete chord was accepted
      // and the desktop merely failed to activate it before the macro deadline.
      invalidate_retained_context();
      return false;
    }
    std::optional<Deadline> target_idle_since;
    bool extended_client_settle = false;
    for (;;) {
      if (Clock::now() + kClipboardBudget >= deadline) {
        deadline =
            std::min(hard_deadline, Clock::now() + kSelectionClientSettle);
        if (!context_matches()) {
          outcome.rejection_stage = "layout_transition_final_context";
          return false;
        }
        outcome.rejection_stage = "layout_transition_timeout";
        preserve_previous_context();
        return false;
      }
      if (!stable_context_matches(false, true)) {
        if (stable_context_failure == StableContextFailure::FocusChanged) {
          extended_client_settle = true;
          const auto keys = hotkey_keys();
          if (keys == HotkeyKeysResult::Foreign ||
              keys == HotkeyKeysResult::ObservationFailed) {
            outcome.rejection_stage = keys == HotkeyKeysResult::Foreign
                                          ? "layout_transition_foreign_key"
                                          : "layout_transition_keymap";
            invalidate_retained_context();
            return false;
          }
          target_idle_since.reset();
          if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
            outcome.rejection_stage = cancelled
                                          ? "layout_transition_cancelled"
                                          : "layout_transition_focus_timeout";
            invalidate_retained_context();
            return false;
          }
          continue;
        }
        switch (stable_context_failure) {
        case StableContextFailure::Wait:
          outcome.rejection_stage = "layout_transition_context_wait";
          break;
        case StableContextFailure::Session:
          outcome.rejection_stage = "layout_transition_context_session";
          break;
        case StableContextFailure::FocusObservation:
          outcome.rejection_stage = "layout_transition_focus_observation";
          break;
        case StableContextFailure::FocusChanged:
          outcome.rejection_stage = "layout_transition_focus_changed";
          break;
        case StableContextFailure::PointerObservation:
          outcome.rejection_stage = "layout_transition_pointer_observation";
          break;
        case StableContextFailure::PointerChanged:
          outcome.rejection_stage = "layout_transition_pointer_changed";
          break;
        case StableContextFailure::None:
          outcome.rejection_stage = "layout_transition_context_after";
          break;
        }
        return false;
      }
      x11_detail::XcbOperationResult layout_result{};
      KeyboardStateSnapshot keyboard_state{};
      const auto observed = idle_layout(connection, allowed_locks, deadline,
                                        &layout_result, &keyboard_state);
      if (!observed) {
        const bool own_chord_transition =
            layout_result == x11_detail::XcbOperationResult::Success &&
            (keyboard_state.group == group || keyboard_state.group == target) &&
            keyboard_state.mods ==
                static_cast<std::uint8_t>(initial_state->locked_mods |
                                          hotkey_modifier_mask) &&
            keyboard_state.base_mods == hotkey_modifier_mask &&
            keyboard_state.latched_mods == 0 &&
            keyboard_state.locked_mods == initial_state->locked_mods &&
            keyboard_state.base_group == 0 &&
            keyboard_state.latched_group == 0 &&
            keyboard_state.pointer_buttons == 0;
        if (!own_chord_transition) {
          outcome.rejection_stage =
              layout_result == x11_detail::XcbOperationResult::Success
                  ? "layout_transition_not_idle"
              : layout_result == x11_detail::XcbOperationResult::TimedOut
                  ? "layout_transition_timeout"
                  : "layout_transition_observation";
          invalidate_retained_context();
          return false;
        }
        extended_client_settle = true;
        const auto keys = hotkey_keys();
        if (keys == HotkeyKeysResult::Foreign ||
            keys == HotkeyKeysResult::ObservationFailed) {
          outcome.rejection_stage = keys == HotkeyKeysResult::Foreign
                                        ? "layout_transition_foreign_key"
                                        : "layout_transition_keymap";
          invalidate_retained_context();
          return false;
        }
        target_idle_since.reset();
        if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
          outcome.rejection_stage = cancelled ? "layout_transition_cancelled"
                                              : "layout_transition_timeout";
          invalidate_retained_context();
          return false;
        }
        continue;
      }
      if (observed->locked_mods != initial_state->locked_mods) {
        outcome.rejection_stage = "layout_transition_locks";
        invalidate_retained_context();
        return false;
      }
      if (observed->group == target) {
        const auto keys = hotkey_keys();
        if (keys == HotkeyKeysResult::Foreign ||
            keys == HotkeyKeysResult::ObservationFailed) {
          outcome.rejection_stage = keys == HotkeyKeysResult::Foreign
                                        ? "layout_transition_foreign_key"
                                        : "layout_transition_keymap";
          invalidate_retained_context();
          return false;
        }
        if (keys == HotkeyKeysResult::OwnChord) {
          target_idle_since.reset();
        } else if (!target_idle_since) {
          target_idle_since = Clock::now();
        } else if (Clock::now() - *target_idle_since >=
                   (extended_client_settle ? kExtendedLayoutClientSettle
                                           : kFastLayoutClientSettle)) {
          const auto history = validate_transition_raw_keys
                                   ? observe_transition_raw_keys(true)
                                   : RawKeyHistoryResult::Clear;
          if (history != RawKeyHistoryResult::Clear) {
            outcome.rejection_stage =
                history == RawKeyHistoryResult::Foreign
                    ? "layout_transition_foreign_raw_key"
                : history == RawKeyHistoryResult::Failed
                    ? "layout_transition_raw_key_observation"
                    : "layout_transition_raw_key_incomplete";
            invalidate_retained_context();
            return false;
          }
          group = target;
          deadline = std::min(hard_deadline, Clock::now() + kMacroBudget);
          return true;
        }
        if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
          outcome.rejection_stage = cancelled
                                        ? "layout_transition_cancelled"
                                        : "layout_transition_settle_timeout";
          invalidate_retained_context();
          return false;
        }
        continue;
      }
      if (observed->group != group) {
        outcome.rejection_stage = "layout_transition_unexpected_group";
        invalidate_retained_context();
        return false;
      }
      target_idle_since.reset();
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        if (cancelled) {
          outcome.rejection_stage = "layout_transition_cancelled";
          invalidate_retained_context();
        } else {
          outcome.rejection_stage = "layout_transition_timeout";
          preserve_previous_context();
        }
        // Reaching the shared deadline here is the sole safe retained retry:
        // the complete desktop chord was accepted and the old group remained
        // observed under the same context until time expired.
        return false;
      }
    }
  };
  // Forwarded input can reach X after our local key-up bookkeeping. XTEST
  // ignores a press of an already-held key, then releases that user's key.
  const auto wait_for_key_release = [&] {
    for (;;) {
      if (!context_matches())
        return false;
      x11_detail::XcbOperationResult operation_result{};
      const auto keys = reply<xcb_query_keymap_reply_t>(
          connection, xcb_query_keymap(connection.get()).sequence, deadline,
          &operation_result);
      if (!keys) {
        invalidate_retained_context();
        return false;
      }
      if (std::ranges::all_of(keys->keys,
                              [](auto byte) { return byte == 0; })) {
        return true;
      }
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        invalidate_retained_context();
        return false;
      }
    }
  };
  outcome.rejection_stage = "key_release";
  if (!wait_for_key_release())
    return outcome;
  outcome.rejection_stage = "context_changed";
  if (!context_matches()) {
    return outcome;
  }

  std::optional<SelectionRead> previous_clipboard;
  const auto snapshot_clipboard = [&] {
    outcome.rejection_stage = "clipboard_snapshot_read";
    previous_clipboard = clipboard_->get_text_with_owner(Selection::Clipboard);
    if (!previous_clipboard) {
      return false;
    }
    outcome.rejection_stage = "clipboard_snapshot_budget";
    if (!clipboard_time()) {
      return false;
    }
    outcome.rejection_stage = "clipboard_snapshot_targets";
    if (clipboard_->has_only_text_targets(Selection::Clipboard,
                                          *previous_clipboard) !=
        std::optional<bool>{true}) {
      return false;
    }
    return true;
  };
  if (paste_word && !snapshot_clipboard()) {
    return outcome;
  }
  const auto cleanup_selection = [&](const SelectionRead &prepared) {
    if (!clipboard_time() || !context_matches()) {
      return false;
    }
    const auto current = clipboard_->get_text_with_owner(Selection::Primary);
    if (current && same_selection(prepared, *current) && context_matches()) {
      return tap(connection, kRight, false, deadline);
    }
    return false;
  };

  const auto finish_layout = [&] {
    return ensure_layout(outcome.target_layout);
  };
  struct InternalLayoutMutation {
    int original_group;
    int requested_group;
  };
  std::optional<InternalLayoutMutation> internal_layout_mutation;
  const auto change_internal_layout = [&](int target) {
    if (target == group) {
      return context_matches();
    }
    if (!context_matches()) {
      return false;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_internal_layout_change();
#endif
    auto cleanup_reserve_start = hard_deadline - kInternalLayoutCleanupBudget;
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    cleanup_reserve_start = Deadline{
        std::chrono::nanoseconds{punto_e2e_internal_layout_cleanup_start(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                cleanup_reserve_start.time_since_epoch())
                .count())}};
#endif
    const auto now = Clock::now();
    if (now >= cleanup_reserve_start) {
      return false;
    }
    if (!internal_layout_mutation) {
      internal_layout_mutation = InternalLayoutMutation{group, target};
      work_deadline = std::min(work_deadline, cleanup_reserve_start);
      deadline = std::min(deadline, work_deadline);
    } else {
      internal_layout_mutation->requested_group = target;
    }
    if (!set_internal_layout(connection, target,
                             std::min(deadline, cleanup_reserve_start))) {
      return false;
    }
    group = target;
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_after_internal_layout();
    punto_e2e_before_internal_layout_work(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            cleanup_reserve_start.time_since_epoch())
            .count());
#endif
    return context_matches();
  };
  const auto restore_internal_layout = [&] {
    if (!internal_layout_mutation) {
      return true;
    }
    const auto mutation = *internal_layout_mutation;
    const auto restore_group = mutation.original_group;
    const auto now = Clock::now();
    if (now >= hard_deadline || !lease->valid()) {
      return false;
    }
    deadline = std::min(hard_deadline, now + kInternalLayoutCleanupBudget);
    bool reconnected = false;
    const auto ensure_cleanup_connection = [&] {
      if (connection.is_open()) {
        return true;
      }
      connection = lease->open_bounded_connection(deadline);
      if (connection.is_open()) {
        reconnected = true;
        auto cleanup_extension = reply<xcb_xkb_use_extension_reply_t>(
            connection, xcb_xkb_use_extension(connection.get(), 1, 0).sequence,
            deadline);
        if (!cleanup_extension || !cleanup_extension->supported) {
          connection.close();
        }
      }
      return connection.is_open();
    };
    if (!ensure_cleanup_connection()) {
      return false;
    }
    KeyboardStateSnapshot cleanup_state{};
    auto current_layout = idle_layout(connection, allowed_locks, deadline,
                                      nullptr, &cleanup_state);
    while (!current_layout && connection.is_open() &&
           Clock::now() + std::chrono::milliseconds{1} < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
      current_layout = idle_layout(connection, allowed_locks, deadline, nullptr,
                                   &cleanup_state);
    }
    if (!current_layout || !connection.is_open() || !lease->valid()) {
      return false;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_internal_layout_restore();
#endif
    constexpr std::uint16_t buttons = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 |
                                      XCB_BUTTON_MASK_3 | XCB_BUTTON_MASK_4 |
                                      XCB_BUTTON_MASK_5;
    const auto authorized_layout = [&] {
      if (!connection.is_open() || !lease->valid()) {
        return std::optional<IdleKeyboardState>{};
      }
      const auto current_focus = focus(connection, deadline);
      if (!current_focus || !connection.is_open() ||
          current_focus != initial_focus) {
        return std::optional<IdleKeyboardState>{};
      }
      const auto current_pointer = reply<xcb_query_pointer_reply_t>(
          connection,
          xcb_query_pointer(connection.get(), *current_focus).sequence,
          deadline);
      if (!current_pointer || !connection.is_open() || !lease->valid()) {
        return std::optional<IdleKeyboardState>{};
      }
      KeyboardStateSnapshot fresh_state{};
      const auto observed = idle_layout(connection, allowed_locks, deadline,
                                        nullptr, &fresh_state);
      if (!observed || !connection.is_open() || !lease->valid() ||
          (observed->group != group &&
           observed->group != mutation.requested_group &&
           observed->group != restore_group) ||
          fresh_state.locked_mods != initial_state->locked_mods ||
          current_pointer->root_x != pointer->root_x ||
          current_pointer->root_y != pointer->root_y ||
          (current_pointer->mask & buttons) != (pointer->mask & buttons)) {
        return std::optional<IdleKeyboardState>{};
      }
      return observed;
    };
    current_layout = authorized_layout();
    if (!current_layout) {
      return false;
    }
    if (current_layout->group != restore_group &&
        !set_internal_layout(connection, restore_group, deadline)) {
      if (!ensure_cleanup_connection()) {
        return false;
      }
    }
    const auto restored = authorized_layout();
    if (restored != std::optional{IdleKeyboardState{
                        restore_group, initial_state->locked_mods}}) {
      return false;
    }
    group = restore_group;
    internal_layout_mutation.reset();
    if (reconnected && Clock::now() < work_deadline) {
      deadline = std::min(deadline, work_deadline);
      if (!prepare_transition_extensions()) {
        return false;
      }
    }
    return true;
  };
  std::optional<bool> internal_layout_finalization;
  const auto finalize_internal_layout = [&] {
    if (!internal_layout_finalization) {
      internal_layout_finalization = restore_internal_layout();
    }
    return *internal_layout_finalization;
  };
  ScopeExit internal_layout_scope{[&] {
    if (!finalize_internal_layout() &&
        outcome.status == WordEditStatus::Dispatched) {
      outcome.status = WordEditStatus::PartialFailure;
    }
  }};
  const auto paste_selection = [&](const SelectionRead &source, bool terminal) {
    outcome.rejection_stage = "selection_confirmation";
    auto confirmed = clipboard_->get_text_with_owner(Selection::Primary);
    if (!confirmed || !same_selection(source, *confirmed) ||
        !context_matches()) {
      return;
    }
    outcome.rejection_stage = "paste_internal_layout";
    if (!change_internal_layout(0)) {
      return;
    }
    if (!renew_budget(kMacroBudget)) {
      return;
    }
    auto pending = std::make_unique<PendingPaste>();
    pending->previous = previous_clipboard->text;
    outcome.rejection_stage = "clipboard_ownership";
    const bool owned =
        previous_clipboard->owner == XCB_WINDOW_NONE
            ? clipboard_->set_text(Selection::Clipboard, outcome.replacement) ==
                  ClipboardResult::Ok
            : clipboard_->set_text_if_owner(Selection::Clipboard,
                                            *previous_clipboard,
                                            outcome.replacement);
    if (!owned) {
      return;
    }
    outcome.status = WordEditStatus::PreparedNotReplayed;
    pending->generation =
        clipboard_->selection_generation(Selection::Clipboard);
    const auto receipt = clipboard_->arm_paste_receipt(Selection::Clipboard);
    if (!receipt) {
      // No paste chord was sent, so this rollback cannot overtake its payload.
      (void)clipboard_->restore_text_if_generation(
          Selection::Clipboard, pending->generation, pending->previous);
      return;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_after_paste_receipt_arm();
#endif
    if (!context_matches()) {
      clipboard_->cancel_paste_receipt(*receipt);
      // No paste chord was sent, so this rollback cannot overtake its payload.
      (void)clipboard_->restore_text_if_generation(
          Selection::Clipboard, pending->generation, pending->previous);
      return;
    }
    pending->receipt = *receipt;
    bool paste_queued = false;
    const bool paste_accepted =
        tap(connection, kPaste, terminal, deadline, true, &paste_queued);
    if (!paste_queued) {
      clipboard_->cancel_paste_receipt(*receipt);
      (void)clipboard_->restore_text_if_generation(
          Selection::Clipboard, pending->generation, pending->previous);
      return;
    }
    invalidate_retained_context();
    pending_ = std::move(pending);
    outcome.status = WordEditStatus::PartialFailure;
    if (!paste_accepted) {
      return;
    }
    while (busy() && clipboard_time() &&
           wait(Clock::now() + std::chrono::milliseconds{1})) {
      pump();
    }
    if (!clipboard_ || !clipboard_->paste_receipt_seen(*receipt)) {
      return;
    }
    if (!renew_budget(kMacroBudget) ||
        !wait(Clock::now() + kExtendedLayoutClientSettle)) {
      return;
    }
    if (!finalize_internal_layout() || !renew_budget(kMacroBudget)) {
      return;
    }
    if (finish_layout()) {
      outcome.status = WordEditStatus::Dispatched;
    }
  };
  if (native_undo) {
    outcome.status = WordEditStatus::PartialFailure;
    if (ensure_layout(0) && renew_budget(kMacroBudget) &&
        tap(connection, kUndo, false, deadline, true) && finish_layout()) {
      outcome.status = WordEditStatus::Dispatched;
    }
    return outcome;
  }

  if (selection) {
    outcome.rejection_stage = "selection_source";
    auto source = clipboard_->get_text_with_owner(Selection::Primary);
    if (!source || source->text.empty() || source->text.size() > 4096 ||
        client_of(source->owner) != *active_client || !context_matches()) {
      return outcome;
    }
    outcome.rejection_stage = "selection_transform";
    switch (request.operation) {
    case WordEditOperation::SelectionLayout:
      outcome.replacement = invert_layout(source->text);
      break;
    case WordEditOperation::SelectionCase:
      outcome.replacement = invert_case(source->text);
      break;
    case WordEditOperation::SelectionTranslit:
      outcome.replacement = transliterate(source->text);
      break;
    default:
      return outcome;
    }
    const bool terminal = kind == ActiveWindowKind::Terminal;
    if (outcome.replacement.empty() || outcome.replacement == source->text ||
        outcome.replacement.size() > 4096 ||
        (terminal && (!terminal_text(source->text) ||
                      !terminal_text(outcome.replacement)))) {
      return outcome;
    }
    outcome.original = terminal ? std::string{} : source->text;
    outcome.terminal_insert = terminal;
    const auto stable_selection_receipt = [&](const SelectionRead &expected) {
      std::optional<SelectionRead> stable_selection;
      std::optional<Deadline> stable_since;
      while (clipboard_->is_open() &&
             Clock::now() + std::chrono::milliseconds{30} < deadline &&
             context_matches()) {
        (void)clipboard_->pump_events();
        auto observed = clipboard_->get_text_with_owner(Selection::Primary);
        const bool authorized = observed && same_selection(expected, *observed);
        if (!authorized) {
          stable_selection.reset();
          stable_since.reset();
        } else if (!stable_selection ||
                   !same_selection(*stable_selection, *observed)) {
          stable_selection = std::move(observed);
          stable_since = Clock::now();
        } else if (stable_since &&
                   Clock::now() - *stable_since >= kSelectionClientSettle) {
          return stable_selection;
        }
        if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
          break;
        }
      }
      return std::optional<SelectionRead>{};
    };
    auto direct_replay =
        !terminal ? plan_text(outcome.replacement, keyboard->alphabet,
                              static_cast<std::uint8_t>(outcome.target_layout))
                  : std::optional<std::vector<Stroke>>{};
    std::vector<Stroke> no_expected;
    if (direct_replay &&
        !resolve_locked_levels(connection, initial_state->locked_mods,
                               keyboard->num_lock_mask, no_expected,
                               *direct_replay, deadline)) {
      direct_replay.reset();
    }
    const bool clipboard_restorable = snapshot_clipboard();
    if (direct_replay &&
        (!clipboard_restorable ||
         request.operation == WordEditOperation::SelectionLayout)) {
      outcome.rejection_stage = "selection_confirmation";
      auto confirmed = stable_selection_receipt(*source);
      if (!confirmed || !context_matches()) {
        return outcome;
      }
      if (!renew_budget(kMacroBudget)) {
        return outcome;
      }
      outcome.rejection_stage = "selection_replay";
      bool replayed = true;
      for (const auto &stroke : *direct_replay) {
        if (stroke.group != group && !change_internal_layout(stroke.group)) {
          replayed = false;
          break;
        }
        if (!context_matches()) {
          replayed = false;
          break;
        }
        outcome.status = WordEditStatus::PartialFailure;
        if (!tap(connection, stroke.key, stroke.shifted, deadline)) {
          replayed = false;
          break;
        }
      }
      if (replayed && (!renew_budget(kMacroBudget) ||
                       !wait(Clock::now() + kExtendedLayoutClientSettle) ||
                       !stable_context_matches())) {
        replayed = false;
      }
      if (!finalize_internal_layout() || !renew_budget(kMacroBudget)) {
        return outcome;
      }
      if (!replayed) {
        return outcome;
      }
      outcome.rejection_stage = "selection_layout_finalize";
      if (!ensure_layout(outcome.target_layout)) {
        return outcome;
      }
      auto retained = clipboard_->get_text_with_owner(Selection::Primary);
      if (retained && same_selection(*confirmed, *retained) &&
          stable_context_matches(true)) {
        retained_word_selection_ =
            std::make_unique<RetainedWordSelection>(RetainedWordSelection{
                *confirmed, *initial_focus, lease->generation()});
        current_receipt_active = true;
      }
      outcome.status = WordEditStatus::Dispatched;
      return outcome;
    }
    if (!clipboard_restorable) {
      return outcome;
    }
    paste_selection(*source, terminal);
    return outcome;
  }

  outcome.original = request.expected;
  outcome.replacement = request.replacement;
  if (!renew_budget(kMacroBudget)) {
    return outcome;
  }
  const bool terminal = kind == ActiveWindowKind::Terminal;
  // A terminal replacement is destructive from its first Backspace and has no
  // selection receipt that can protect an exact GUI range. Keep the desktop
  // transition as a preflight there. GUI editors select first and revalidate
  // that exact receipt after their desktop transition.
  if (terminal && !finish_layout()) {
    return outcome;
  }
  invalidate_retained_context();
  outcome.rejection_stage = "selection_prepare";
  for (std::size_t i = 0; i < expected->size(); ++i) {
    if (!context_matches()) {
      return outcome;
    }
    outcome.status = terminal ? WordEditStatus::PartialFailure
                              : WordEditStatus::PreparedNotReplayed;
    if (!tap(connection, terminal ? kBackspace : kLeft, !terminal, deadline)) {
      return outcome;
    }
  }
  xcb_window_t prepared_owner = XCB_WINDOW_NONE;
  std::unique_ptr<RetainedWordSelection> prepared_receipt;
  if (!terminal) {
    outcome.rejection_stage = "selection_confirmation";
    std::unique_ptr<SelectionRead> confirmed_selection;
    std::unique_ptr<SelectionRead> prepared_selection;
    Deadline confirmed_since{};
    bool selection_confirmed = false;
    while (clipboard_->is_open() &&
           Clock::now() + std::chrono::milliseconds{30} < deadline &&
           context_matches()) {
      (void)clipboard_->pump_events();
      auto observed = clipboard_->get_text_with_owner(Selection::Primary);
      if (observed) {
        SelectionRead current = std::move(observed).value();
        if (client_of(current.owner) == *active_client) {
          if (!prepared_selection) {
            prepared_selection = std::make_unique<SelectionRead>(current);
          }
          if (current.text == request.expected) {
            if (!confirmed_selection ||
                !same_selection(*confirmed_selection, current)) {
              confirmed_selection =
                  std::make_unique<SelectionRead>(std::move(current));
              confirmed_since = Clock::now();
            } else if (Clock::now() - confirmed_since >=
                       kSelectionClientSettle) {
              selection_confirmed = true;
              break;
            }
          } else {
            confirmed_selection.reset();
            confirmed_since = {};
          }
        }
      }
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        break;
      }
    }
    if (!selection_confirmed || !confirmed_selection || !context_matches()) {
      if (prepared_selection) {
        (void)cleanup_selection(*prepared_selection);
      }
      return outcome;
    }
    if (paste_word) {
      paste_selection(*confirmed_selection, false);
      if (outcome.status == WordEditStatus::PreparedNotReplayed) {
        (void)cleanup_selection(*confirmed_selection);
      }
      return outcome;
    }
    prepared_owner = confirmed_selection->owner;
    prepared_receipt =
        std::make_unique<RetainedWordSelection>(RetainedWordSelection{
            *confirmed_selection, *initial_focus, lease->generation()});
  }

  if (!terminal) {
    outcome.rejection_stage = "layout_transition_preflight";
    validate_transition_raw_keys = observe_gui_word_transition;
    const bool layout_ready = finish_layout();
    validate_transition_raw_keys = false;
    if (!layout_ready) {
      const auto cleanup_started = Clock::now();
      if (cleanup_started >= hard_deadline) {
        return outcome;
      }
      deadline = std::min(hard_deadline,
                          cleanup_started + kInternalLayoutCleanupBudget);
      if (cleanup_selection(prepared_receipt->selection) &&
          wait(Clock::now() + kSelectionClientSettle) &&
          context_matches(true)) {
        retained_word_selection_ = std::move(prepared_receipt);
        outcome.status = WordEditStatus::Rejected;
      }
      return outcome;
    }
    outcome.rejection_stage = "selection_revalidation";
    (void)clipboard_->pump_events();
    const auto revalidated =
        clipboard_->get_text_with_owner(Selection::Primary);
    if (!revalidated ||
        !same_selection(prepared_receipt->selection, *revalidated) ||
        !context_matches()) {
      return outcome;
    }
  }

  outcome.rejection_stage = "replacement_replay";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  punto_e2e_before_word_replay();
#endif
  bool replacement_replayed = true;
  for (const auto &stroke : *replacement) {
    if (!context_matches()) {
      replacement_replayed = false;
      break;
    }
    if (stroke.group != group) {
      outcome.rejection_stage = "internal_layout_replay";
      if (!change_internal_layout(stroke.group)) {
        replacement_replayed = false;
        break;
      }
    }
    outcome.status = WordEditStatus::PartialFailure;
    if (!tap(connection, stroke.key, stroke.shifted, deadline)) {
      replacement_replayed = false;
      break;
    }
  }
  if (!finalize_internal_layout() || !replacement_replayed ||
      !renew_budget(kMacroBudget)) {
    return outcome;
  }
  outcome.rejection_stage = "layout_transition_finalize";
  if (finish_layout()) {
    outcome.status = WordEditStatus::Dispatched;
    retained_word_selection_ = std::move(prepared_receipt);
    current_receipt_active = retained_word_selection_ != nullptr;
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_after_word_dispatch();
#endif
    // Server acceptance can precede the client's processing of the replacement.
    // Let our prepared selection settle before another word macro's preflight.
    while (prepared_owner != XCB_WINDOW_NONE && context_matches(true)) {
      x11_detail::XcbOperationResult owner_result{};
      const auto current_owner = reply<xcb_get_selection_owner_reply_t>(
          connection,
          xcb_get_selection_owner(connection.get(), XCB_ATOM_PRIMARY).sequence,
          deadline, &owner_result);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      punto_e2e_before_post_dispatch_wait();
#endif
      if (!current_owner) {
        if (owner_result != x11_detail::XcbOperationResult::TimedOut) {
          invalidate_retained_context();
        }
        break;
      }
      if (current_owner->owner != prepared_owner) {
        break;
      }
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        break;
      }
    }
    current_receipt_active = false;
  }
  return outcome;
}

} // namespace punto
