#include "punto/word_editor.hpp"

#include "punto/clipboard_manager.hpp"
#include "punto/key_entry_text.hpp"
#include "punto/macro_lock.hpp"
#include "punto/text_processor.hpp"
#include "punto/x11_session.hpp"

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

#include <array>
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace punto {
namespace {

using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr auto kClipboardBudget = std::chrono::milliseconds{10};
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

template <typename T>
Reply<T> reply(BoundedXcbConnection &connection, unsigned int sequence,
               Deadline deadline) {
  x11_detail::XcbOperationResult result{};
  return Reply<T>{static_cast<T *>(
                      connection.wait_for_reply(sequence, deadline, result)),
                  &std::free};
}

bool checked(BoundedXcbConnection &connection, xcb_void_cookie_t cookie,
             Deadline deadline) {
  x11_detail::XcbOperationResult result{};
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
  std::uint8_t num_lock_mask = 0;
  std::uint8_t caps_lock_mask = 0;
};

std::optional<KeyboardPlan> make_keyboard_plan(
    BoundedXcbConnection &connection, Deadline deadline) {
  if (!connection.is_open()) {
    return std::nullopt;
  }
  const auto *setup = xcb_get_setup(connection.get());
  const auto count = static_cast<std::uint8_t>(
      setup->max_keycode - setup->min_keycode + 1);
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
      connection, xcb_get_modifier_mapping(connection.get()).sequence, deadline);
  if (!modifiers) {
    return std::nullopt;
  }
  KeyboardPlan plan;
  const auto *modifier_keys = xcb_get_modifier_mapping_keycodes(modifiers.get());
  bool caps_found = false;
  bool only_caps = true;
  bool shift_found = false;
  bool control_found = false;
  for (unsigned int i = 0; i < modifiers->keycodes_per_modifier; ++i) {
    shift_found |= modifier_keys[i] == kShift;
    control_found |= modifier_keys[2U * modifiers->keycodes_per_modifier + i] == kControl;
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
      const auto key = modifier_keys[slot * modifiers->keycodes_per_modifier + i];
      if (key == 0) {
        continue;
      }
      if (key < setup->min_keycode || key > setup->max_keycode) {
        only_num_lock = false;
        break;
      }
      bool num_lock_key = false;
      for (unsigned int column = 0; column < map->keysyms_per_keycode; ++column) {
        const auto symbol = symbol_at(key, column);
        num_lock_key |= symbol == XKB_KEY_Num_Lock;
        only_num_lock &= symbol == XKB_KEY_NoSymbol || symbol == XKB_KEY_Num_Lock;
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
        const bool matches = length > 1 &&
                             *expected == std::string_view{
                                              bytes.data(),
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
      xcb_xkb_get_map(connection.get(), XCB_XKB_ID_USE_CORE_KBD, parts, 0,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0).sequence,
      deadline);
  if (!map || (map->present & parts) != parts || map->firstType != 0 ||
      map->nTypes == 0) {
    return false;
  }
  xcb_xkb_get_map_map_t contents{};
  xcb_xkb_get_map_map_unpack(
      xcb_xkb_get_map_map(map.get()), map->nTypes, map->nKeySyms,
      map->nKeyActions, map->totalActions, map->totalKeyBehaviors,
      map->virtualMods, map->totalKeyExplicit, map->totalModMapKeys,
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
    key_types[i] = Type{types.data, mask,
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
        const auto mask = (locks | (shift ? XCB_MOD_MASK_SHIFT : 0)) & type.mask;
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

std::optional<std::vector<Stroke>> plan_text(
    std::string_view text, const std::vector<Stroke> &alphabet) {
  if (text.empty() || text.size() > kMaxCharacters * 2) {
    return std::nullopt;
  }
  std::vector<Stroke> plan;
  while (!text.empty() && plan.size() < kMaxCharacters) {
    const Stroke *found = nullptr;
    for (const auto &stroke : alphabet) {
      if (text.starts_with(stroke.text)) {
        found = &stroke;
        break;
      }
    }
    if (found == nullptr) {
      return std::nullopt;
    }
    plan.push_back(*found);
    text.remove_prefix(found->text.size());
  }
  return text.empty() ? std::optional{std::move(plan)} : std::nullopt;
}

std::optional<xcb_window_t> focus(BoundedXcbConnection &connection,
                                  Deadline deadline) {
  if (!connection.is_open()) {
    return std::nullopt;
  }
  auto value = reply<xcb_get_input_focus_reply_t>(
      connection, xcb_get_input_focus(connection.get()).sequence, deadline);
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

std::optional<IdleKeyboardState> idle_layout(BoundedXcbConnection &connection,
                                            std::uint8_t allowed_locks,
                                            Deadline deadline) {
  if (!connection.is_open()) {
    return std::nullopt;
  }
  auto state = reply<xcb_xkb_get_state_reply_t>(
      connection,
      xcb_xkb_get_state(connection.get(), XCB_XKB_ID_USE_CORE_KBD).sequence,
      deadline);
  if (!state || (state->mods & ~allowed_locks) != 0 || state->baseMods != 0 ||
      state->latchedMods != 0 || (state->lockedMods & ~allowed_locks) != 0 ||
      state->baseGroup != 0 || state->latchedGroup != 0 || state->group > 1 ||
      state->ptrBtnState != 0) {
    return std::nullopt;
  }
  return IdleKeyboardState{state->group, state->lockedMods};
}

bool set_layout(BoundedXcbConnection &connection, int group,
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
          Deadline deadline, bool control = false) {
  if (!connection.is_open() || Clock::now() >= deadline) {
    return false;
  }
  std::array<xcb_void_cookie_t, 6> cookies{};
  std::size_t used = 0;
  const auto send = [&](std::uint8_t type, xcb_keycode_t code) {
    cookies[used++] = xcb_test_fake_input_checked(
        connection.get(), type, code, XCB_CURRENT_TIME, XCB_WINDOW_NONE, 0, 0,
        0);
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
      !clipboard_->owns_generation(Selection::Clipboard, pending_->generation)) {
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
  const bool paste_word = word &&
      (request.expected.find('\t') != std::string::npos ||
       request.replacement.find('\t') != std::string::npos);
  const bool native_undo = request.operation == WordEditOperation::NativeUndo;
  const bool selection = request.operation == WordEditOperation::SelectionLayout ||
                         request.operation == WordEditOperation::SelectionCase ||
                         request.operation == WordEditOperation::SelectionTranslit;
  if ((!word && !native_undo && !selection) ||
      (word && (request.expected.empty() || request.expected == request.replacement)) ||
      request.target_layout < -1 ||
      request.target_layout > 1 || request.source_layout < 0 ||
      request.source_layout > 1 || request.session_generation == 0 ||
      request.source_locked_mods < -1 || request.source_locked_mods > 255) {
    return outcome;
  }
  pump();
  auto previous_selection = std::move(retained_word_selection_);
  if (busy() && !native_undo) {
    return outcome;
  }
  const auto deadline = Clock::now() + std::chrono::milliseconds{300};
  MacroLock lock;
  MacroLockGuard guard{lock, std::chrono::milliseconds{0}};
  if (!guard.owns_lock()) {
    return outcome;
  }
  auto lease = session_.acquire_write_lease();
  if (!lease || !lease->valid() || !lease->info().wayland_display.empty() ||
      lease->generation() != request.session_generation) {
    return outcome;
  }
  auto connection = lease->open_bounded_connection(deadline);
  if (!connection.is_open()) {
    return outcome;
  }
  // Generated extension requests otherwise perform a blocking first lookup.
  xcb_prefetch_extension_data(connection.get(), &xcb_test_id);
  auto extension_barrier = reply<xcb_get_input_focus_reply_t>(
      connection, xcb_get_input_focus(connection.get()).sequence, deadline);
  if (!extension_barrier || !connection.is_open()) {
    return outcome;
  }
  const auto *xtest = xcb_get_extension_data(connection.get(), &xcb_test_id);
  if (xtest == nullptr || xtest->present == 0) {
    return outcome;
  }
  auto extension = reply<xcb_xkb_use_extension_reply_t>(
      connection, xcb_xkb_use_extension(connection.get(), 1, 0).sequence,
      deadline);
  if (!extension || !extension->supported) {
    return outcome;
  }
  const auto keyboard = make_keyboard_plan(connection, deadline);
  if (!keyboard) {
    return outcome;
  }
  const auto allowed_locks = static_cast<std::uint8_t>(
      keyboard->num_lock_mask | keyboard->caps_lock_mask);
  const auto initial_state =
      idle_layout(connection, allowed_locks, deadline);
  const auto initial_focus = focus(connection, deadline);
  if (!initial_state || !initial_focus ||
      initial_state->group != request.source_layout ||
      (request.expected_focus != 0 && request.expected_focus != *initial_focus) ||
      (request.source_locked_mods >= 0 &&
       request.source_locked_mods != initial_state->locked_mods)) {
    return outcome;
  }
  auto expected = word ? plan_text(request.expected, keyboard->alphabet)
                       : std::optional<std::vector<Stroke>>{std::vector<Stroke>{}};
  auto replacement = word && !request.replacement.empty()
                         ? plan_text(request.replacement, keyboard->alphabet)
                         : std::optional<std::vector<Stroke>>{std::vector<Stroke>{}};
  if (!expected || !replacement ||
      !resolve_locked_levels(connection, initial_state->locked_mods,
                              keyboard->num_lock_mask, *expected,
                              *replacement, deadline)) {
    return outcome;
  }

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
  const auto kind = clipboard_->active_window_kind();
  if (kind == ActiveWindowKind::Unknown ||
      (word && kind == ActiveWindowKind::Terminal && !request.allow_terminal) ||
      (word && kind == ActiveWindowKind::Terminal &&
       (!terminal_text(request.expected) || !terminal_text(request.replacement))) ||
      (word && kind == ActiveWindowKind::Gui && replacement->empty())) {
    return outcome;
  }
  const auto active_client = clipboard_->active_client_id();
  const auto resource_mask = xcb_get_setup(connection.get())->resource_id_mask;
  const auto client_of = [&](xcb_window_t window) {
    return window & ~resource_mask;
  };
  auto owner = reply<xcb_get_selection_owner_reply_t>(
      connection,
      xcb_get_selection_owner(connection.get(), XCB_ATOM_PRIMARY).sequence,
      deadline);
  if (!active_client || !owner ||
      client_of(*initial_focus) != *active_client) {
    return outcome;
  }
  if (word && owner->owner != XCB_WINDOW_NONE &&
      client_of(owner->owner) == *active_client) {
    // Chromium retains PRIMARY after our temporary selection has collapsed.
    // Only our exact prior receipt can admit it; a new user selection cannot.
    if (kind != ActiveWindowKind::Gui || !previous_selection ||
        previous_selection->focus_window != *initial_focus ||
        previous_selection->session_generation != lease->generation() ||
        !clipboard_time()) {
      return outcome;
    }
    const auto current = clipboard_->get_text_with_owner(Selection::Primary);
    if (!current || !same_selection(previous_selection->selection, *current)) {
      return outcome;
    }
  }
  auto pointer = reply<xcb_query_pointer_reply_t>(
      connection, xcb_query_pointer(connection.get(), *initial_focus).sequence,
      deadline);
  if (!pointer) {
    return outcome;
  }
  outcome.source_layout = initial_state->group;
  outcome.target_layout = request.target_layout < 0 ? initial_state->group
                                                   : request.target_layout;
  outcome.session_generation = lease->generation();
  outcome.focused_window = *initial_focus;
  int group = initial_state->group;
  bool cancelled = false;
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
  const auto context_matches = [&] {
    if (!wait(Clock::now()) || !connection.is_open() || !lease->valid() ||
        focus(connection, deadline) != initial_focus ||
        idle_layout(connection, allowed_locks, deadline) !=
            std::optional{IdleKeyboardState{group, initial_state->locked_mods}}) {
      return false;
    }
    auto current = reply<xcb_query_pointer_reply_t>(
        connection, xcb_query_pointer(connection.get(), *initial_focus).sequence,
        deadline);
    constexpr std::uint16_t buttons = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 |
                                      XCB_BUTTON_MASK_3 | XCB_BUTTON_MASK_4 |
                                      XCB_BUTTON_MASK_5;
    // Core pointer state also carries the XKB group. idle_layout validates
    // keyboard state against the group selected by this executor.
    return current && current->root_x == pointer->root_x &&
           current->root_y == pointer->root_y &&
           (current->mask & buttons) == (pointer->mask & buttons);
  };
  if (!context_matches()) {
    return outcome;
  }

  std::optional<SelectionRead> previous_clipboard;
  if (selection || paste_word) {
    previous_clipboard = clipboard_->get_text_with_owner(Selection::Clipboard);
    if (!previous_clipboard || !clipboard_time() ||
        clipboard_->has_only_text_targets(Selection::Clipboard,
                                          *previous_clipboard) !=
            std::optional<bool>{true}) {
      return outcome;
    }
  }
  const auto cleanup_selection = [&](const SelectionRead &prepared) {
    if (!clipboard_time() || !context_matches()) {
      return;
    }
    const auto current = clipboard_->get_text_with_owner(Selection::Primary);
    if (current && same_selection(prepared, *current) && context_matches()) {
      (void)tap(connection, kRight, false, deadline);
    }
  };

  const auto shortcut = [&](xcb_keycode_t key, bool shift) {
    if (!context_matches()) {
      return false;
    }
    if (group != 0) {
      if (!set_layout(connection, 0, deadline)) {
        return false;
      }
      group = 0;
    }
    return tap(connection, key, shift, deadline, true);
  };
  const auto finish_layout = [&] {
    if (!context_matches() ||
        (group != outcome.target_layout &&
         !set_layout(connection, outcome.target_layout, deadline))) {
      return false;
    }
    group = outcome.target_layout;
    return true;
  };
  const auto paste_selection = [&](const SelectionRead &source, bool terminal) {
    auto confirmed = clipboard_->get_text_with_owner(Selection::Primary);
    if (!confirmed || !same_selection(source, *confirmed) || !context_matches()) {
      return;
    }
    auto pending = std::make_unique<PendingPaste>();
    pending->previous = previous_clipboard->text;
    const bool owned = previous_clipboard->owner == XCB_WINDOW_NONE
                           ? clipboard_->set_text(Selection::Clipboard,
                                                  outcome.replacement) == ClipboardResult::Ok
                           : clipboard_->set_text_if_owner(
                                 Selection::Clipboard, *previous_clipboard,
                                 outcome.replacement);
    if (!owned) {
      return;
    }
    outcome.status = WordEditStatus::PreparedNotReplayed;
    pending->generation = clipboard_->selection_generation(Selection::Clipboard);
    const auto receipt = clipboard_->arm_paste_receipt(Selection::Clipboard);
    if (!receipt || !context_matches()) {
      // No paste chord was sent, so this rollback cannot overtake its payload.
      (void)clipboard_->restore_text_if_generation(
          Selection::Clipboard, pending->generation, pending->previous);
      return;
    }
    pending->receipt = *receipt;
    pending_ = std::move(pending);
    outcome.status = WordEditStatus::PartialFailure;
    if (!shortcut(kPaste, terminal) || !finish_layout()) {
      return;
    }
    while (busy() && clipboard_time() && wait(Clock::now() + std::chrono::milliseconds{1})) {
      pump();
    }
    if (clipboard_ && clipboard_->paste_receipt_seen(*receipt)) {
      outcome.status = WordEditStatus::Dispatched;
    }
  };
  if (native_undo) {
    outcome.status = WordEditStatus::PartialFailure;
    if (shortcut(kUndo, false) && finish_layout()) {
      outcome.status = WordEditStatus::Dispatched;
    }
    return outcome;
  }

  if (selection) {
    auto source = clipboard_->get_text_with_owner(Selection::Primary);
    if (!source || source->text.empty() || source->text.size() > 4096 ||
        client_of(source->owner) != *active_client || !context_matches()) {
      return outcome;
    }
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
    paste_selection(*source, terminal);
    return outcome;
  }

  outcome.original = request.expected;
  outcome.replacement = request.replacement;
  const bool terminal = kind == ActiveWindowKind::Terminal;
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
    std::optional<SelectionRead> observed;
    std::optional<SelectionRead> prepared_selection;
    while (clipboard_->is_open() &&
           Clock::now() + std::chrono::milliseconds{30} < deadline &&
           context_matches()) {
      observed = clipboard_->get_text_with_owner(Selection::Primary);
      if (!prepared_selection && observed &&
          client_of(observed->owner) == *active_client) {
        prepared_selection = observed;
      }
      if (observed && observed->text == request.expected &&
          client_of(observed->owner) == *active_client) {
        break;
      }
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        break;
      }
    }
    if (!observed || observed->text != request.expected ||
        client_of(observed->owner) != *active_client || !context_matches()) {
      if (prepared_selection) {
        cleanup_selection(*prepared_selection);
      }
      return outcome;
    }
    if (paste_word) {
      paste_selection(*observed, false);
      if (outcome.status == WordEditStatus::PreparedNotReplayed) {
        cleanup_selection(*observed);
      }
      return outcome;
    }
    prepared_owner = observed->owner;
    prepared_receipt = std::make_unique<RetainedWordSelection>(
        RetainedWordSelection{*observed, *initial_focus, lease->generation()});
  }

  for (const auto &stroke : *replacement) {
    if (!context_matches()) {
      return outcome;
    }
    if (stroke.group != group) {
      if (!set_layout(connection, stroke.group, deadline)) {
        outcome.status = WordEditStatus::PartialFailure;
        return outcome;
      }
      group = stroke.group;
    }
    outcome.status = WordEditStatus::PartialFailure;
    if (!tap(connection, stroke.key, stroke.shifted, deadline)) {
      return outcome;
    }
  }
  if (finish_layout()) {
    outcome.status = WordEditStatus::Dispatched;
    retained_word_selection_ = std::move(prepared_receipt);
    // Server acceptance can precede the client's processing of the replacement.
    // Let our prepared selection settle before another word macro's preflight.
    while (prepared_owner != XCB_WINDOW_NONE && context_matches()) {
      const auto current_owner = reply<xcb_get_selection_owner_reply_t>(
          connection,
          xcb_get_selection_owner(connection.get(), XCB_ATOM_PRIMARY).sequence,
          deadline);
      if (!current_owner || current_owner->owner != prepared_owner ||
          !wait(Clock::now() + std::chrono::milliseconds{1})) {
        break;
      }
    }
  }
  return outcome;
}

} // namespace punto
