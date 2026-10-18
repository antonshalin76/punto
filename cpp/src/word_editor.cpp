#include "punto/word_editor.hpp"

#include "punto/clipboard_manager.hpp"
#include "punto/desktop_layout_client.hpp"
#include "punto/key_entry_text.hpp"
#include "punto/macro_lock.hpp"
#include "punto/text_processor.hpp"
#include "punto/x11_session.hpp"

#include <glib.h>
#include <xcb/res.h>
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
#include <cstdio>
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
extern "C" void punto_e2e_before_paste_receipt_arm();
extern "C" void punto_e2e_before_paste_publication();
extern "C" void punto_e2e_after_replay_layout(std::int64_t deadline_ns);
extern "C" void punto_e2e_after_paste_receipt_arm();
extern "C" void punto_e2e_before_paste_chord();
extern "C" std::int64_t punto_e2e_after_paste_delivered(void (*pump)(void *),
                                                        void *owner);
extern "C" void punto_e2e_after_word_dispatch();
extern "C" void punto_e2e_before_post_dispatch_wait();
extern "C" void punto_e2e_before_raw_key_observation();
extern "C" void punto_e2e_after_raw_key_observation();
extern "C" void punto_e2e_set_layout_connection(xcb_connection_t *connection);
extern "C" void punto_e2e_before_word_replay();
extern "C" std::int64_t punto_e2e_layout_cleanup_phase(int phase);
extern "C" void punto_e2e_delay_layout_cleanup();
extern "C" xcb_window_t punto_e2e_observed_cleanup_focus(xcb_window_t focus);
extern "C" void punto_e2e_before_selection_revalidation();
extern "C" void punto_e2e_before_completed_selection_read();
extern "C" std::int64_t
punto_e2e_prepared_cleanup_hard_deadline(std::int64_t deadline_ns);
extern "C" std::int64_t
punto_e2e_selection_confirmation_deadline(std::int64_t deadline_ns);
extern "C" std::int64_t
punto_e2e_selection_observed_deadline(std::int64_t deadline_ns);
extern "C" void punto_e2e_cleanup_receipt_retained();
extern "C" bool punto_e2e_revoke_cleanup_lease();
extern "C" bool punto_e2e_bypass_cleanup_selection_identity();
extern "C" void punto_e2e_cleanup_authority_rejected(const char *reason);
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
constexpr auto kDesktopLayoutCleanupBudget = std::chrono::milliseconds{300};
constexpr auto kSelectionCleanupBudget = std::chrono::milliseconds{300};
constexpr auto kPendingPasteEditBlockBudget = std::chrono::milliseconds{500};
constexpr auto kPendingPasteNoReceiptBudget = std::chrono::seconds{2};
constexpr auto kPendingPasteReceiptQuietBudget = std::chrono::milliseconds{250};
constexpr auto kPendingPasteReceiptMaxBudget = std::chrono::milliseconds{1000};
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
constexpr xcb_keycode_t kTerminalPaste = KEY_INSERT + 8;
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

template <typename T>
Reply<T>
read_reply(BoundedXcbConnection &connection, unsigned int sequence,
           Deadline deadline,
           x11_detail::XcbOperationResult *operation_result = nullptr) {
  x11_detail::XcbOperationResult local_result{};
  auto &result = operation_result == nullptr ? local_result : *operation_result;
  return Reply<T>{static_cast<T *>(connection.wait_for_read_reply(
                      sequence, deadline, result)),
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
  bool operator==(const Stroke &) const = default;
};

struct KeyboardPlan {
  std::vector<Stroke> alphabet;
  std::array<std::uint8_t, 256> modifier_masks{};
  std::vector<std::uint64_t> xkb_identity;
  std::vector<std::uint64_t> xkb_map_identity;
  std::uint8_t num_lock_mask = 0;
  std::uint8_t caps_lock_mask = 0;
  bool operator==(const KeyboardPlan &) const = default;
};

std::optional<std::vector<std::uint64_t>>
xkb_map_identity(xcb_xkb_get_map_reply_t *map) {
  constexpr std::uint16_t parts = XCB_XKB_MAP_PART_KEY_TYPES |
                                  XCB_XKB_MAP_PART_KEY_SYMS |
                                  XCB_XKB_MAP_PART_VIRTUAL_MODS;
  if (map == nullptr || (map->present & parts) != parts)
    return std::nullopt;
  std::vector<std::uint64_t> identity;
  const auto append = [&](const auto value) {
    identity.push_back(static_cast<std::uint64_t>(value));
  };
  append(map->deviceID);
  append(map->minKeyCode);
  append(map->maxKeyCode);
  append(map->present);
  append(map->firstType);
  append(map->nTypes);
  append(map->totalTypes);
  append(map->firstKeySym);
  append(map->totalSyms);
  append(map->nKeySyms);
  append(map->virtualMods);
  xcb_xkb_get_map_map_t contents{};
  xcb_xkb_get_map_map_unpack(xcb_xkb_get_map_map(map), map->nTypes,
                             map->nKeySyms, map->nKeyActions, map->totalActions,
                             map->totalKeyBehaviors, map->virtualMods,
                             map->totalKeyExplicit, map->totalModMapKeys,
                             map->totalVModMapKeys, map->present, &contents);
  auto types = xcb_xkb_get_map_map_types_rtrn_iterator(map, &contents);
  for (; types.rem > 0; xcb_xkb_key_type_next(&types)) {
    const auto *type = types.data;
    append(type->mods_mask);
    append(type->mods_mods);
    append(type->mods_vmods);
    append(type->numLevels);
    append(type->nMapEntries);
    append(type->hasPreserve);
    auto entries = xcb_xkb_key_type_map_iterator(type);
    for (; entries.rem > 0; xcb_xkb_kt_map_entry_next(&entries)) {
      append(entries.data->active);
      append(entries.data->mods_mask);
      append(entries.data->level);
      append(entries.data->mods_mods);
      append(entries.data->mods_vmods);
    }
    auto preserves = xcb_xkb_key_type_preserve_iterator(type);
    for (; preserves.rem > 0; xcb_xkb_mod_def_next(&preserves)) {
      append(preserves.data->mask);
      append(preserves.data->realMods);
      append(preserves.data->vmods);
    }
  }
  auto syms = xcb_xkb_get_map_map_syms_rtrn_iterator(map, &contents);
  for (; syms.rem > 0; xcb_xkb_key_sym_map_next(&syms)) {
    for (const auto type_index : syms.data->kt_index)
      append(type_index);
    append(syms.data->groupInfo);
    append(syms.data->width);
    append(syms.data->nSyms);
    const auto *keysyms = xcb_xkb_key_sym_map_syms(syms.data);
    for (int index = 0; index < xcb_xkb_key_sym_map_syms_length(syms.data);
         ++index)
      append(keysyms[index]);
  }
  const auto *virtual_mods = xcb_xkb_get_map_map_vmods_rtrn(&contents);
  for (unsigned int bit = 0; bit < 16; ++bit) {
    if ((map->virtualMods & (1U << bit)) != 0)
      append(*virtual_mods++);
  }
  return identity;
}

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
  plan.xkb_identity.reserve(
      16U + static_cast<std::size_t>(
                xcb_get_keyboard_mapping_keysyms_length(map.get())));
  const auto append_identity = [&](const auto value) {
    plan.xkb_identity.push_back(static_cast<std::uint64_t>(value));
  };
  append_identity(setup->min_keycode);
  append_identity(setup->max_keycode);
  append_identity(map->keysyms_per_keycode);
  append_identity(xcb_get_keyboard_mapping_keysyms_length(map.get()));
  for (int index = 0;
       index < xcb_get_keyboard_mapping_keysyms_length(map.get()); ++index)
    append_identity(symbols[index]);
  const auto *modifier_keys =
      xcb_get_modifier_mapping_keycodes(modifiers.get());
  append_identity(modifiers->keycodes_per_modifier);
  append_identity(xcb_get_modifier_mapping_keycodes_length(modifiers.get()));
  for (int index = 0;
       index < xcb_get_modifier_mapping_keycodes_length(modifiers.get());
       ++index)
    append_identity(modifier_keys[index]);
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

  constexpr std::uint16_t identity_parts = XCB_XKB_MAP_PART_KEY_TYPES |
                                           XCB_XKB_MAP_PART_KEY_SYMS |
                                           XCB_XKB_MAP_PART_VIRTUAL_MODS;
  auto xkb_map = reply<xcb_xkb_get_map_reply_t>(
      connection,
      xcb_xkb_get_map(connection.get(), XCB_XKB_ID_USE_CORE_KBD, identity_parts,
                      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
          .sequence,
      deadline);
  if (!xkb_map || (xkb_map->present & identity_parts) != identity_parts)
    return std::nullopt;
  auto map_identity = xkb_map_identity(xkb_map.get());
  if (!map_identity)
    return std::nullopt;
  plan.xkb_map_identity = std::move(*map_identity);
  plan.xkb_identity.insert(plan.xkb_identity.end(),
                           plan.xkb_map_identity.begin(),
                           plan.xkb_map_identity.end());
  return plan;
}

bool resolve_locked_levels(BoundedXcbConnection &connection, std::uint8_t locks,
                           std::uint8_t num_lock_mask,
                           std::span<const std::uint64_t> expected_identity,
                           std::span<Stroke> expected,
                           std::span<Stroke> replacement, Deadline deadline,
                           bool terminal_paste = false) {
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
  const auto identity = xkb_map_identity(map.get());
  if (!identity || !std::ranges::equal(*identity, expected_identity))
    return false;
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
  if (terminal_paste)
    needed.set(kTerminalPaste);
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
    if (terminal_paste && key == kTerminalPaste) {
      const auto *symbols = xcb_xkb_key_sym_map_syms(keys.data);
      const int count = xcb_xkb_key_sym_map_syms_length(keys.data);
      if (count <= 0 || symbols[0] != XKB_KEY_Insert ||
          !std::all_of(symbols, symbols + count, [](auto symbol) {
            return symbol == XKB_KEY_NoSymbol || symbol == XKB_KEY_Insert;
          })) {
        return false;
      }
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
      x11_detail::XcbOperationResult *result = nullptr,
      bool retain_on_timeout = false,
      std::optional<xcb_window_t> *observed_focus = nullptr) {
  if (observed_focus != nullptr)
    observed_focus->reset();
  if (!connection.is_open()) {
    if (result != nullptr) {
      *result = x11_detail::XcbOperationResult::ConnectionFailed;
    }
    return std::nullopt;
  }
  const auto sequence = xcb_get_input_focus(connection.get()).sequence;
  auto value = retain_on_timeout ? read_reply<xcb_get_input_focus_reply_t>(
                                       connection, sequence, deadline, result)
                                 : reply<xcb_get_input_focus_reply_t>(
                                       connection, sequence, deadline, result);
  if (value && observed_focus != nullptr)
    *observed_focus = value->focus;
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
            KeyboardStateSnapshot *snapshot = nullptr,
            bool retain_on_timeout = false) {
  if (!connection.is_open()) {
    if (result != nullptr) {
      *result = x11_detail::XcbOperationResult::ConnectionFailed;
    }
    return std::nullopt;
  }
  const auto sequence =
      xcb_xkb_get_state(connection.get(), XCB_XKB_ID_USE_CORE_KBD).sequence;
  auto state = retain_on_timeout
                   ? read_reply<xcb_xkb_get_state_reply_t>(connection, sequence,
                                                           deadline, result)
                   : reply<xcb_xkb_get_state_reply_t>(connection, sequence,
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
         left.connection_generation == right.connection_generation &&
         left.selection_timestamp == right.selection_timestamp;
}

std::optional<std::uint32_t> application_pid(BoundedXcbConnection &connection,
                                             xcb_window_t window,
                                             Deadline deadline) {
  xcb_prefetch_extension_data(connection.get(), &xcb_res_id);
  const auto barrier = reply<xcb_get_input_focus_reply_t>(
      connection, xcb_get_input_focus(connection.get()).sequence, deadline);
  if (!barrier)
    return std::nullopt;
  const auto *extension = xcb_get_extension_data(connection.get(), &xcb_res_id);
  if (!extension || !extension->present)
    return std::nullopt;
  auto version = reply<xcb_res_query_version_reply_t>(
      connection, xcb_res_query_version(connection.get(), 1, 2).sequence,
      deadline);
  if (!version || version->server_major < 1 ||
      (version->server_major == 1 && version->server_minor < 2))
    return std::nullopt;
  const xcb_res_client_id_spec_t spec{window,
                                      XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
  auto result = reply<xcb_res_query_client_ids_reply_t>(
      connection, xcb_res_query_client_ids(connection.get(), 1, &spec).sequence,
      deadline);
  if (!result || result->num_ids != 1 || result->length > 16)
    return std::nullopt;
  const auto value = xcb_res_query_client_ids_ids_iterator(result.get());
  if (value.rem != 1 ||
      value.data->spec.mask != XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID ||
      xcb_res_client_id_value_value_length(value.data) != 1)
    return std::nullopt;
  const auto pid = *xcb_res_client_id_value_value(value.data);
  return pid > 0 ? std::optional{pid} : std::nullopt;
}

bool same_selection_identity(const SelectionRead &left,
                             const SelectionRead &right) {
  return left.owner == right.owner &&
         left.owner_generation == right.owner_generation &&
         left.connection_generation == right.connection_generation &&
         left.selection_timestamp == right.selection_timestamp;
}

bool has_word_character(std::string_view text) {
  while (!text.empty()) {
    const auto character = g_utf8_get_char_validated(
        text.data(), static_cast<gssize>(text.size()));
    if (character == static_cast<gunichar>(-1) ||
        character == static_cast<gunichar>(-2)) {
      return false;
    }
    if (g_unichar_isalnum(character)) {
      return true;
    }
    text.remove_prefix(
        static_cast<std::size_t>(g_utf8_next_char(text.data()) - text.data()));
  }
  return false;
}

bool is_keyboard_word_prefix(std::string_view text) {
  for (const char *cursor = text.data(); cursor < text.data() + text.size();
       cursor = g_utf8_next_char(cursor)) {
    const auto character = g_utf8_get_char(cursor);
    if (g_unichar_isalnum(character) || g_unichar_isspace(character)) {
      return false;
    }
  }
  return has_word_character(invert_layout(text));
}

bool terminal_text(std::string_view text) {
  if (!g_utf8_validate(text.data(), static_cast<gssize>(text.size()),
                       nullptr)) {
    return false;
  }
  for (const char *cursor = text.data(); cursor < text.data() + text.size();
       cursor = g_utf8_next_char(cursor)) {
    if (g_unichar_iscntrl(g_utf8_get_char(cursor))) {
      return false;
    }
  }
  return true;
}

} // namespace

struct WordEditor::PendingPaste {
  SelectionSnapshot previous;
  PayloadPublication publication;
  std::uint64_t generation = 0;
  std::optional<PasteReceiptToken> receipt;
  Deadline edit_block_deadline{};
  std::optional<Deadline> receipt_quiet_deadline;
  std::optional<Deadline> receipt_restore_deadline;
  std::uint64_t request_sequence = 0;
  bool restore_admitted = false;
  Deadline retirement_deadline{};
};

struct WordEditor::RetainedWordSelection {
  SelectionRead selection;
  xcb_window_t focus_window = XCB_WINDOW_NONE;
  std::uint64_t session_generation = 0;
  std::uint64_t fresh_text_input_after = 0;
  bool requires_fresh_text_input = false;
  std::string editor_token;
  std::pair<std::string, std::string> editor_child;
};

struct WordEditor::RevokedWordContext {
  xcb_window_t focus_window = XCB_WINDOW_NONE;
  std::uint64_t session_generation = 0;
  std::uint64_t text_input_sequence = 0;
  std::pair<std::string, std::string> editor_child;
};

WordEditor::WordEditor(X11Session &session, WaitFunction wait)
    : session_(session), wait_(std::move(wait)),
      desktop_layout_(std::make_unique<DesktopLayoutClient>()) {}

WordEditor::~WordEditor() = default;

bool WordEditor::busy() const noexcept {
  return pending_ && pending_->receipt && !pending_->receipt_quiet_deadline &&
         Clock::now() < pending_->edit_block_deadline;
}

bool WordEditor::has_pending_paste() const noexcept {
  return pending_ != nullptr;
}

std::optional<std::string>
WordEditor::editor_context(Deadline deadline, std::string_view expected_token,
                           WaitFunction wait, bool preparation) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  const auto started = Clock::now();
  const auto diagnose = [&](const char *stage, AccessibleStatus status) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             Clock::now() - started)
                             .count();
    std::fprintf(
        stderr,
        "[punto-e2e] origin_refused stage=%s status=%u elapsed_us=%lld\n",
        stage, static_cast<unsigned>(status), static_cast<long long>(elapsed));
  };
#endif
  const auto lease = session_.acquire_write_lease();
  if (!lease || !lease->valid() || !lease->info().wayland_display.empty()) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    diagnose("lease", AccessibleStatus::Unavailable);
#endif
    return std::nullopt;
  }
  const auto setup_deadline =
      std::min(deadline, Clock::now() + std::chrono::milliseconds{20});
  auto connection = lease->open_bounded_connection(setup_deadline);
  if (!connection.is_open()) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    diagnose("connection", AccessibleStatus::Unavailable);
#endif
    return std::nullopt;
  }
  const auto window = focus(connection, setup_deadline);
  const auto pid = window ? application_pid(connection, *window, setup_deadline)
                          : std::nullopt;
  if (!pid) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    diagnose("application", AccessibleStatus::Unavailable);
#endif
    return std::nullopt;
  }
  auto observed = desktop_layout_->observe_editor(
      lease->info(), *pid, deadline, wait ? wait : wait_, expected_token,
      std::nullopt, AccessibleScope::Child, preparation);
  if (!lease->valid() || observed.status != AccessibleStatus::Observed) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    diagnose("child", observed.status);
#endif
    return std::nullopt;
  }
  return std::move(observed.token);
}

void WordEditor::pump() {
  if (!clipboard_) {
    return;
  }
  const auto lease = session_.acquire_write_lease();
  if (!lease || lease->generation() != clipboard_session_) {
    std::fprintf(
        stderr,
        "[punto] clipboard session retired lease_present=%d "
        "lease_generation=%llu clipboard_session=%llu\n",
        static_cast<int>(lease.has_value()),
        static_cast<unsigned long long>(lease ? lease->generation() : 0),
        static_cast<unsigned long long>(clipboard_session_));
    if (pending_)
      retired_clipboard_data_ = pending_->previous.payload;
    clipboard_.reset();
    pending_.reset();
    retained_word_selection_.reset();
    revoked_word_context_.reset();
    clipboard_session_ = 0;
    return;
  }
  (void)clipboard_->pump_events();
  if (!pending_) {
    return;
  }
  const auto receipt = pending_->receipt;
  const auto retire_pending = [&] {
    retired_clipboard_data_ = pending_->previous.payload;
    if (receipt) {
      clipboard_->cancel_paste_receipt(*receipt);
    }
    clipboard_->cancel_publication_witness(pending_->publication);
    pending_.reset();
  };
  const auto restore_pending = [&] {
    if (!clipboard_->restore_payload_if_generation(
            Selection::Clipboard, pending_->generation, pending_->previous)) {
      if (Clock::now() >= pending_->retirement_deadline)
        retire_pending();
      return;
    }
    if (receipt) {
      clipboard_->cancel_paste_receipt(*receipt);
    }
    pending_.reset();
  };
  if (!clipboard_->is_open()) {
    if (pending_->restore_admitted &&
        clipboard_->restore_payload_after_uncertain(
            Selection::Clipboard, pending_->publication, pending_->previous,
            Clock::now() + std::chrono::milliseconds{20})) {
      if (receipt)
        clipboard_->cancel_paste_receipt(*receipt);
      pending_.reset();
    } else if (Clock::now() >= pending_->retirement_deadline) {
      retire_pending();
    }
    return;
  }
  // Event pumping observes foreign ownership; quiet waiting must not destroy
  // our publisher on an unrelated owner-query timeout. Restoration still
  // obtains a fresh, server-fenced conditional ownership proof.
  if (!clipboard_->locally_owns_generation(Selection::Clipboard,
                                           pending_->generation)) {
    // A foreign copy wins, including copy followed by release to NONE.
    if (clipboard_->is_open())
      retire_pending();
    return;
  }
  if (!receipt) {
    if (pending_->restore_admitted) {
      restore_pending();
    }
    return;
  }
  const auto receipt_status = clipboard_->paste_receipt_status(*receipt);
  if (receipt_status == PasteReceiptStatus::Expired) {
    if (!pending_->restore_admitted) {
      return;
    }
    restore_pending();
    return;
  }
  if (receipt_status != PasteReceiptStatus::Delivered) {
    return;
  }
  const auto request_sequence = clipboard_->paste_receipt_request_seq(*receipt);
  const auto now = Clock::now();
  Deadline restore_deadline =
      pending_->receipt_restore_deadline.value_or(Deadline::max());
  const bool absolute_deadline_reached = now >= restore_deadline;
  if (!absolute_deadline_reached &&
      (!pending_->receipt_quiet_deadline ||
       request_sequence != pending_->request_sequence)) {
    if (!pending_->receipt_restore_deadline) {
      restore_deadline = now + kPendingPasteReceiptMaxBudget;
      pending_->receipt_restore_deadline = restore_deadline;
    }
    pending_->request_sequence = request_sequence;
    pending_->receipt_quiet_deadline =
        std::min(now + kPendingPasteReceiptQuietBudget, restore_deadline);
    return;
  }
  if (!absolute_deadline_reached &&
      now < pending_->receipt_quiet_deadline.value_or(now)) {
    return;
  }
  if (!pending_->restore_admitted) {
    return;
  }
  restore_pending();
}

void WordEditor::reset() {
  // A status/config reset cannot revoke a payload already requested by an app.
  // Session loss and foreign ownership are handled by the same pump authority.
  pump();
  // Cleanup identity, including failed lineage, is not an analysis intent.
  // Execute alone admits or invalidates it and always proves a new range.
  desktop_layout_->reset();
}

WordEditOutcome WordEditor::execute(const WordEditRequest &request) {
  WordEditOutcome outcome;
  bool current_word =
      request.operation == WordEditOperation::CurrentWordLayout ||
      request.operation == WordEditOperation::CurrentWordCase;
  const bool word =
      request.operation == WordEditOperation::Word || current_word;
  bool paste_word =
      word && (request.expected.find('\t') != std::string::npos ||
               request.replacement.find('\t') != std::string::npos);
  const bool native_undo = request.operation == WordEditOperation::NativeUndo;
  const bool selection =
      request.operation == WordEditOperation::SelectionLayout ||
      request.operation == WordEditOperation::SelectionCase ||
      request.operation == WordEditOperation::SelectionTranslit;
  if ((!word && !native_undo && !selection) ||
      (request.held_tab_deadline &&
       (!word || current_word ||
        request.expected.find('\t') != std::string::npos ||
        request.replacement.find('\t') != std::string::npos)) ||
      (word && !current_word &&
       (request.expected.empty() || request.expected == request.replacement)) ||
      request.target_layout < -1 || request.target_layout > 1 ||
      request.source_layout < 0 || request.source_layout > 1 ||
      request.session_generation == 0 || request.source_locked_mods < -1 ||
      request.source_locked_mods > 255) {
    return outcome;
  }
  if (word) {
    outcome.retry_policy = WordEditRetryPolicy::AfterFreshInput;
  }
  const auto started = Clock::now();
  auto hard_deadline = started + kTotalMacroBudget;
  if (request.held_tab_deadline) {
    hard_deadline = std::min(hard_deadline, *request.held_tab_deadline);
    if (started >= hard_deadline) {
      return outcome;
    }
  }
  pump();
  outcome.rejection_stage = "busy";
  const bool held_tab_paste = request.held_tab_deadline.has_value();
  const bool needs_clipboard_payload = word || held_tab_paste || selection;
  if (needs_clipboard_payload) {
    const auto retirement_deadline =
        hard_deadline - kMacroBudget - kSelectionCleanupBudget;
    while (pending_ && pending_->receipt && pending_->restore_admitted &&
           clipboard_ &&
           clipboard_->paste_receipt_status(*pending_->receipt) ==
               PasteReceiptStatus::Delivered &&
           Clock::now() < retirement_deadline) {
      const auto until = std::min(retirement_deadline,
                                  Clock::now() + std::chrono::milliseconds{1});
      if (wait_) {
        try {
          if (!wait_(until)) {
            return outcome;
          }
        } catch (...) {
          return outcome;
        }
      } else {
        std::this_thread::sleep_until(until);
      }
      pump();
    }
  }
  if (pending_ && (native_undo || needs_clipboard_payload ||
                   Clock::now() < pending_->edit_block_deadline)) {
    return outcome;
  }
  std::optional<std::uint64_t> published_paste_generation;
  // Publication owns rollback too; this runs after bounded layout cleanup.
  ScopeExit paste_lifetime{[&] {
    if (published_paste_generation && pending_ &&
        pending_->generation == *published_paste_generation) {
      pending_->restore_admitted = true;
      pending_->receipt_quiet_deadline.reset();
      pending_->receipt_restore_deadline.reset();
    }
  }};
  auto deadline = std::min(Clock::now() + kMacroBudget, hard_deadline);
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
      word && !current_word
          ? plan_text(request.expected, keyboard->alphabet,
                      static_cast<std::uint8_t>(request.source_layout))
          : std::optional<std::vector<Stroke>>{std::vector<Stroke>{}};
  const auto replacement_group = static_cast<std::uint8_t>(
      request.target_layout < 0 ? request.source_layout
                                : request.target_layout);
  auto replacement =
      word && !current_word && !request.replacement.empty()
          ? plan_text(request.replacement, keyboard->alphabet,
                      replacement_group)
          : std::optional<std::vector<Stroke>>{std::vector<Stroke>{}};
  if (!expected || !replacement ||
      !resolve_locked_levels(
          connection, initial_state->locked_mods, keyboard->num_lock_mask,
          keyboard->xkb_map_identity, *expected, *replacement, deadline)) {
    return outcome;
  }

  const auto clipboard_time = [&] {
    return Clock::now() + kClipboardBudget < deadline;
  };
  outcome.rejection_stage = "clipboard_budget_before_open";
  if (!clipboard_time()) {
    return outcome;
  }
  if (!clipboard_) {
    clipboard_ = std::make_unique<ClipboardManager>(session_, kClipboardBudget);
    clipboard_session_ = lease->generation();
  }
  outcome.rejection_stage = "clipboard_open";
  if (!clipboard_->open(deadline)) {
    return outcome;
  }
  outcome.rejection_stage = "clipboard_budget_after_open";
  if (!clipboard_time()) {
    return outcome;
  }
  outcome.rejection_stage = "active_window";
  const auto kind = clipboard_->active_window_kind();
  if (kind == ActiveWindowKind::Unknown ||
      (word && !current_word && kind == ActiveWindowKind::Gui &&
       replacement->empty())) {
    return outcome;
  }
  std::string terminal_replacement;
  int terminal_target = request.target_layout;
  if (kind == ActiveWindowKind::Terminal && !selection) {
    outcome.rejection_stage = "terminal_history_unavailable";
    // Ctrl+Z stops a terminal child; only our counted inverse is permitted.
    if (native_undo || !request.allow_terminal || request.expected.empty() ||
        request.expected.find('\t') != std::string::npos ||
        !terminal_text(request.expected)) {
      return outcome;
    }
    if (current_word) {
      terminal_replacement =
          request.operation == WordEditOperation::CurrentWordCase
              ? invert_case(request.expected)
              : invert_layout(request.expected);
      if (request.operation == WordEditOperation::CurrentWordLayout)
        terminal_target = is_predominantly_cyrillic(request.expected) ? 0 : 1;
      expected = plan_text(request.expected, keyboard->alphabet,
                           static_cast<std::uint8_t>(request.source_layout));
      replacement = plan_text(terminal_replacement, keyboard->alphabet,
                              static_cast<std::uint8_t>(terminal_target));
      if (!expected || !replacement || replacement->empty() ||
          terminal_replacement == request.expected ||
          !terminal_text(terminal_replacement) ||
          !resolve_locked_levels(
              connection, initial_state->locked_mods, keyboard->num_lock_mask,
              keyboard->xkb_map_identity, *expected, *replacement, deadline)) {
        return outcome;
      }
      current_word = false;
    } else {
      terminal_replacement = request.replacement;
      if (!terminal_replacement.empty() && !terminal_text(terminal_replacement))
        return outcome;
    }
    outcome.terminal_history = true;
  }
  const bool observe_gui_word_transition =
      word && kind == ActiveWindowKind::Gui &&
      (request.operation == WordEditOperation::CurrentWordLayout ||
       (request.target_layout >= 0 &&
        request.target_layout != request.source_layout));
  std::optional<AccessibleObservation> initial_editor;
  std::optional<std::uint32_t> editor_pid;
  if (kind == ActiveWindowKind::Gui) {
    outcome.rejection_stage = "editor_context";
    if (request.operation == WordEditOperation::Word &&
        request.expected_editor_token.empty()) {
      outcome.rejection_stage = "editor_origin_missing";
      return outcome;
    }
    editor_pid = application_pid(connection, *initial_focus, deadline);
    if (!editor_pid) {
      outcome.rejection_stage = "editor_process_unavailable";
      return outcome;
    }
    auto observed =
        desktop_layout_->observe_editor(lease->info(), *editor_pid, deadline,
                                        wait_, request.expected_editor_token);
    if (observed.status != AccessibleStatus::Observed || !lease->valid()) {
      const auto now = Clock::now();
      std::fprintf(
          stderr,
          "[punto] Editor admission refused scope=1 status=%u operation=%u "
          "expected_token_present=%d lease_valid=%d elapsed_us=%lld "
          "remaining_us=%lld\n",
          static_cast<unsigned>(observed.status),
          static_cast<unsigned>(request.operation),
          !request.expected_editor_token.empty(), lease->valid(),
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(now -
                                                                    started)
                  .count()),
          static_cast<long long>(
              std::chrono::duration_cast<std::chrono::microseconds>(deadline -
                                                                    now)
                  .count()));
      return outcome;
    }
    outcome.editor_token = observed.token;
    initial_editor = std::move(observed);
  }
  const auto editor_child =
      initial_editor
          ? std::pair{initial_editor->provider, initial_editor->object}
          : std::pair<std::string, std::string>{};
  const auto revoke_word_context = [&] {
    if (word && initial_editor) {
      revoked_word_context_ = std::make_unique<RevokedWordContext>(
          RevokedWordContext{*initial_focus, lease->generation(),
                             request.text_input_sequence, editor_child});
    }
  };
  if (current_word && revoked_word_context_ &&
      revoked_word_context_->focus_window == *initial_focus &&
      revoked_word_context_->session_generation == lease->generation() &&
      revoked_word_context_->editor_child == editor_child &&
      request.text_input_sequence <=
          revoked_word_context_->text_input_sequence) {
    outcome.rejection_stage = "retained_selection_untrusted";
    return outcome;
  }
  const auto editor_matches = [&](const std::optional<AccessibleRange> &range =
                                      std::nullopt) {
    if (!initial_editor)
      return kind == ActiveWindowKind::Terminal;
    const auto observed = desktop_layout_->observe_editor(
        lease->info(), *editor_pid, deadline, wait_, initial_editor->token,
        range, range ? AccessibleScope::Range : AccessibleScope::Child);
    return observed.status == AccessibleStatus::Observed && lease->valid();
  };
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
  std::optional<SelectionRead> primary_selection_baseline;
  auto previous_selection = std::move(retained_word_selection_);
  if (word && owner->owner != XCB_WINDOW_NONE &&
      client_of(owner->owner) == *active_client) {
    outcome.rejection_stage = "retained_selection_context";
    // Chromium can retain PRIMARY after our temporary selection collapses.
    // A receipt authorizes reuse only in its original child. In a fresh child,
    // an independently observed empty selection permits native preparation;
    // stale PRIMARY data is only a baseline, never the requested word.
    if (kind != ActiveWindowKind::Gui || !clipboard_time()) {
      return outcome;
    }
    outcome.rejection_stage = "retained_selection_read";
    ClipboardResult read_result = ClipboardResult::ConversionFailed;
    const auto current = clipboard_->get_text_with_owner(
        Selection::Primary, deadline, &read_result);
    const bool previous_context_matches =
        previous_selection &&
        previous_selection->focus_window == *initial_focus &&
        previous_selection->session_generation == lease->generation() &&
        previous_selection->editor_token == outcome.editor_token;
    const bool fresh_physical_text =
        previous_context_matches &&
        request.text_input_sequence >
            previous_selection->fresh_text_input_after;
    const bool empty_editor_selection =
        initial_editor && initial_editor->caret >= 0 &&
        initial_editor->range.start == initial_editor->caret &&
        initial_editor->range.end == initial_editor->caret;
    const bool fresh_manual_origin =
        current_word && !previous_selection && empty_editor_selection;
    const bool fresh_word_origin =
        request.operation == WordEditOperation::Word && previous_selection &&
        previous_selection->focus_window == *initial_focus &&
        previous_selection->session_generation == lease->generation() &&
        previous_selection->editor_child == editor_child &&
        previous_selection->editor_token != outcome.editor_token &&
        !request.expected_editor_token.empty() &&
        request.expected_editor_token == outcome.editor_token &&
        request.text_input_sequence >
            previous_selection->fresh_text_input_after;
    const auto unreadable_identity =
        (previous_context_matches || fresh_word_origin ||
         fresh_manual_origin) &&
                !current && clipboard_->is_open() &&
                read_result == ClipboardResult::NoSelection
            ? clipboard_->get_selection_identity(Selection::Primary, deadline)
            : std::nullopt;
    const bool same_owner_unreadable =
        previous_selection && unreadable_identity &&
        unreadable_identity->owner == previous_selection->selection.owner &&
        unreadable_identity->connection_generation ==
            previous_selection->selection.connection_generation;
    if (previous_context_matches &&
        previous_selection->requires_fresh_text_input &&
        request.text_input_sequence <=
            previous_selection->fresh_text_input_after &&
        clipboard_->is_open() &&
        (!current || same_selection(previous_selection->selection, *current))) {
      // A safe stale refusal consumes neither the original stamp nor identity.
      retained_word_selection_ = std::move(previous_selection);
      outcome.rejection_stage = "retained_lineage_stale_input";
      return outcome;
    }
    if (same_owner_unreadable && !fresh_physical_text && !fresh_word_origin &&
        !same_selection_identity(previous_selection->selection,
                                 *unreadable_identity)) {
      retained_word_selection_ = std::move(previous_selection);
      outcome.rejection_stage = "retained_lineage_stale_input";
      return outcome;
    }
    // Only an explicit NONE from the same healthy owner may recover unreadable
    // lineage. A timeout cannot authorize native selection. Fresh text still
    // needs an independently proven range newer than the current identity.
    const bool fresh_lineage_admitted =
        previous_context_matches && fresh_physical_text &&
        clipboard_->is_open() &&
        ((previous_selection->requires_fresh_text_input && current &&
          same_selection(previous_selection->selection, *current)) ||
         same_owner_unreadable);
    const bool previous_receipt_matches =
        previous_context_matches &&
        (fresh_lineage_admitted ||
         (!previous_selection->requires_fresh_text_input &&
          ((current &&
            same_selection(previous_selection->selection, *current)) ||
           (!current && clipboard_->selection_identity_matches(
                            Selection::Primary, previous_selection->selection,
                            deadline)))));
    const bool previous_child_matches =
        previous_selection &&
        previous_selection->focus_window == *initial_focus &&
        previous_selection->session_generation == lease->generation() &&
        previous_selection->editor_child == editor_child;
    if (current_word && previous_child_matches && !previous_receipt_matches &&
        request.text_input_sequence <=
            previous_selection->fresh_text_input_after) {
      revoke_word_context();
      outcome.rejection_stage = "retained_selection_untrusted";
      return outcome;
    }
    // New physical provenance may prepare a new range even when the client
    // still serves cached PRIMARY. These bytes supply only the current
    // baseline; neither an old receipt nor a token acquired at Pause is proof.
    const bool fresh_readable_word_origin =
        !previous_receipt_matches && word && empty_editor_selection &&
        !request.expected_editor_token.empty() &&
        request.expected_editor_token == outcome.editor_token &&
        request.text_input_sequence > 0 &&
        (!previous_selection ||
         request.text_input_sequence >
             previous_selection->fresh_text_input_after) &&
        current && read_result == ClipboardResult::Ok &&
        clipboard_->is_open() && current->owner == owner->owner &&
        clipboard_->selection_identity_matches(Selection::Primary, *current,
                                               deadline);
    // Explicit manual commands prepare a new range at the observed caret.
    // Cached PRIMARY is its baseline, never authority to replay old bytes.
    const bool fresh_readable_manual_origin =
        current_word &&
        (!previous_child_matches ||
         request.text_input_sequence >
             previous_selection->fresh_text_input_after) &&
        current && read_result == ClipboardResult::Ok &&
        clipboard_->is_open() && current->owner == owner->owner &&
        clipboard_->selection_identity_matches(Selection::Primary, *current,
                                               deadline);
    const bool fresh_origin_empty_selection =
        empty_editor_selection &&
        ((current_word && previous_selection &&
          previous_selection->focus_window == *initial_focus &&
          previous_selection->session_generation == lease->generation() &&
          previous_selection->editor_token != outcome.editor_token &&
          previous_selection->editor_child != editor_child &&
          (!previous_selection->requires_fresh_text_input ||
           request.text_input_sequence >
               previous_selection->fresh_text_input_after) &&
          current) ||
         (fresh_word_origin && same_owner_unreadable) ||
         (fresh_manual_origin && unreadable_identity &&
          unreadable_identity->owner == owner->owner) ||
         fresh_readable_word_origin || fresh_readable_manual_origin);
    if (previous_receipt_matches) {
      previous_context_admitted = true;
      primary_selection_baseline = current ? current : unreadable_identity;
      if (!primary_selection_baseline) {
        primary_selection_baseline = previous_selection->selection;
      }
    } else if (fresh_origin_empty_selection) {
      primary_selection_baseline = current ? current : unreadable_identity;
      previous_selection.reset();
    } else {
      if (previous_selection && previous_selection->requires_fresh_text_input &&
          !current) {
        outcome.rejection_stage =
            !clipboard_->is_open()     ? "retained_lineage_clipboard_transport"
            : previous_context_matches ? "retained_lineage_stale_input"
                                       : "retained_lineage_context";
      } else {
        outcome.rejection_stage = current ? "retained_selection_untrusted"
                                          : "retained_selection_unreadable";
      }
      return outcome;
    }
  }
  bool preparation_lineage_revoked = false;
  const auto retire_retained_context = [&] {
    if (previous_context_admitted) {
      previous_selection.reset();
      previous_context_admitted = false;
    }
    if (current_receipt_active) {
      retained_word_selection_.reset();
    }
  };
  const auto invalidate_retained_context = [&](bool revoke_preparation = true) {
    if (previous_context_admitted || current_receipt_active ||
        outcome.status == WordEditStatus::PreparedNotReplayed) {
      revoke_word_context();
    }
    if (revoke_preparation &&
        outcome.status == WordEditStatus::PreparedNotReplayed) {
      preparation_lineage_revoked = true;
    }
    retire_retained_context();
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
  if (outcome.terminal_history && terminal_target >= 0)
    outcome.target_layout = terminal_target;
  outcome.session_generation = lease->generation();
  outcome.focused_window = *initial_focus;
  outcome.observed_layout = initial_state->group;
  int group = initial_state->group;
  bool cancelled = false;
  const auto renew_budget = [&](auto budget) {
    const auto now = Clock::now();
    if (now >= deadline || now >= hard_deadline) {
      return false;
    }
    deadline = std::min(hard_deadline, now + budget);
    return true;
  };
  const auto begin_phase_budget = [&](auto budget, Deadline phase_limit) {
    const auto now = Clock::now();
    const auto limit = std::min(hard_deadline, phase_limit);
    if (now >= limit) {
      return false;
    }
    deadline = std::min(limit, now + budget);
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
  std::optional<x11_detail::XcbOperationResult>
      stable_context_observation_result;
  std::optional<x11_detail::XcbOperationResult> diagnostic_focus_result;
  std::optional<xcb_window_t> diagnostic_focus_reply;
  bool focus_return_pending = false;
  const auto handle_stable_observation_failure =
      [&](x11_detail::XcbOperationResult result, bool interrupted_wait_is_safe,
          bool retain_on_timeout) {
        if (!observation_failure_invalidates(result, interrupted_wait_is_safe))
          return;
        if (result == x11_detail::XcbOperationResult::TimedOut &&
            retain_on_timeout && connection.is_open() && lease->valid() &&
            !cancelled && !focus_return_pending) {
          retire_retained_context();
        } else {
          invalidate_retained_context();
        }
      };
  const auto stable_context_matches = [&](bool interrupted_wait_is_safe = false,
                                          bool allow_focus_transition = false,
                                          bool retain_on_timeout = false) {
    stable_context_failure = StableContextFailure::None;
    stable_context_observation_result.reset();
    diagnostic_focus_result.reset();
    diagnostic_focus_reply.reset();
    if (!wait(Clock::now())) {
      stable_context_failure = StableContextFailure::Wait;
      if (!interrupted_wait_is_safe) {
        retire_retained_context();
      }
      return false;
    }
    if (!connection.is_open() || !lease->valid()) {
      stable_context_failure = StableContextFailure::Session;
      invalidate_retained_context();
      return false;
    }
    x11_detail::XcbOperationResult focus_result{};
    auto current_focus = focus(connection, deadline, &focus_result,
                               retain_on_timeout, &diagnostic_focus_reply);
    diagnostic_focus_result = focus_result;
    if (!current_focus) {
      stable_context_failure = StableContextFailure::FocusObservation;
      stable_context_observation_result = focus_result;
      handle_stable_observation_failure(focus_result, interrupted_wait_is_safe,
                                        retain_on_timeout);
      return false;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    *current_focus = punto_e2e_observed_cleanup_focus(*current_focus);
#endif
    const bool focus_changed = current_focus != initial_focus;
    if (focus_changed && !allow_focus_transition) {
      stable_context_failure = StableContextFailure::FocusChanged;
      invalidate_retained_context();
      return false;
    }
    if (focus_changed) {
      focus_return_pending = true;
    }
    x11_detail::XcbOperationResult pointer_result{};
    const auto pointer_sequence =
        xcb_query_pointer(connection.get(), *initial_focus).sequence;
    auto current =
        retain_on_timeout
            ? read_reply<xcb_query_pointer_reply_t>(
                  connection, pointer_sequence, deadline, &pointer_result)
            : reply<xcb_query_pointer_reply_t>(connection, pointer_sequence,
                                               deadline, &pointer_result);
    constexpr std::uint16_t buttons = XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 |
                                      XCB_BUTTON_MASK_3 | XCB_BUTTON_MASK_4 |
                                      XCB_BUTTON_MASK_5;
    // Core pointer state also carries the XKB group. idle_layout validates
    // keyboard state against the group selected by this executor.
    if (!current) {
      stable_context_failure = StableContextFailure::PointerObservation;
      stable_context_observation_result = pointer_result;
      handle_stable_observation_failure(
          pointer_result, interrupted_wait_is_safe, retain_on_timeout);
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
    focus_return_pending = false;
    return true;
  };
  KeyboardStateSnapshot context_keyboard_snapshot{};
  std::optional<x11_detail::XcbOperationResult> context_layout_result;
  const auto report_context_failure = [&](std::string_view stage) {
    std::fprintf(
        stderr,
        "[punto] selection admission stage=%.*s context_result=%d "
        "xkb_result=%d group=%d expected_group=%d mods=%u base_mods=%u "
        "latched_mods=%u locked_mods=%u expected_locks=%u base_group=%d "
        "latched_group=%d buttons=%u remaining_us=%lld "
        "focus_result=%d focus_reply=%lld\n",
        static_cast<int>(stage.size()), stage.data(),
        static_cast<int>(stable_context_failure),
        context_layout_result ? static_cast<int>(*context_layout_result) : -1,
        context_layout_result ==
                std::optional{x11_detail::XcbOperationResult::Success}
            ? context_keyboard_snapshot.group
            : -1,
        group, static_cast<unsigned>(context_keyboard_snapshot.mods),
        static_cast<unsigned>(context_keyboard_snapshot.base_mods),
        static_cast<unsigned>(context_keyboard_snapshot.latched_mods),
        static_cast<unsigned>(context_keyboard_snapshot.locked_mods),
        static_cast<unsigned>(initial_state->locked_mods),
        static_cast<int>(context_keyboard_snapshot.base_group),
        static_cast<int>(context_keyboard_snapshot.latched_group),
        static_cast<unsigned>(context_keyboard_snapshot.pointer_buttons),
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(deadline -
                                                                  Clock::now())
                .count()),
        diagnostic_focus_result ? static_cast<int>(*diagnostic_focus_result)
                                : -1,
        diagnostic_focus_reply ? static_cast<long long>(*diagnostic_focus_reply)
                               : -1);
  };
  const auto context_matches = [&](bool interrupted_wait_is_safe = false,
                                   bool retain_on_timeout = false) {
    context_keyboard_snapshot = {};
    context_layout_result.reset();
    if (!stable_context_matches(interrupted_wait_is_safe, false,
                                retain_on_timeout)) {
      report_context_failure(outcome.rejection_stage);
      return false;
    }
    x11_detail::XcbOperationResult layout_result{};
    const auto observed =
        idle_layout(connection, allowed_locks, deadline, &layout_result,
                    &context_keyboard_snapshot, retain_on_timeout);
    context_layout_result = layout_result;
    if (!observed) {
      if (observation_failure_invalidates(layout_result,
                                          interrupted_wait_is_safe)) {
        invalidate_retained_context();
      }
      report_context_failure(outcome.rejection_stage);
      return false;
    }
    const bool matches =
        observed ==
        std::optional{IdleKeyboardState{group, initial_state->locked_mods}};
    if (!matches) {
      invalidate_retained_context();
      report_context_failure(outcome.rejection_stage);
    }
    return matches;
  };
  enum class HotkeyKeysResult { Clear, Foreign, ObservationFailed };
  const auto hotkey_keys = [&] {
    const auto keys = read_reply<xcb_query_keymap_reply_t>(
        connection, xcb_query_keymap(connection.get()).sequence, deadline);
    if (!keys) {
      return HotkeyKeysResult::ObservationFailed;
    }
    for (unsigned int key = min_keycode; key <= max_keycode; ++key) {
      const bool down = (keys->keys[key / 8U] & (1U << (key % 8U))) != 0;
      if (down) {
        return HotkeyKeysResult::Foreign;
      }
    }
    return HotkeyKeysResult::Clear;
  };
  enum class RawKeyHistoryResult { Clear, Foreign, Incomplete, Failed };
  const auto observe_transition_raw_keys = [&](bool validate,
                                               bool expect_cleanup_right =
                                                   false) {
    if (!connection.is_open()) {
      return RawKeyHistoryResult::Failed;
    }
    const auto observation_deadline =
        std::min(deadline, Clock::now() + kRawKeyObservationBudget);
    bool cleanup_right_pressed = false;
    bool cleanup_right_released = false;
    for (std::size_t count = 0; count < kMaxRawKeyEvents; ++count) {
      if (Clock::now() >= observation_deadline) {
        return RawKeyHistoryResult::Incomplete;
      }
      auto *event = xcb_poll_for_event(connection.get());
      if (event == nullptr) {
        if (xcb_connection_has_error(connection.get()) != 0) {
          return RawKeyHistoryResult::Failed;
        }
        return !expect_cleanup_right || cleanup_right_released
                   ? RawKeyHistoryResult::Clear
                   : RawKeyHistoryResult::Incomplete;
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
          if (
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
              raw->detail == KEY_LEFTCTRL + 8U ||
              raw->detail == KEY_GRAVE + 8U ||
              raw->detail == KEY_RIGHTCTRL + 8U ||
              raw->detail == KEY_LEFTALT + 8U ||
              raw->detail == KEY_BACKSLASH + 8U ||
#endif
              false) {
            foreign = false;
          } else if (expect_cleanup_right && raw->detail == kRight) {
            if (generic->event_type == XCB_INPUT_RAW_KEY_PRESS) {
              foreign = cleanup_right_pressed || cleanup_right_released;
              cleanup_right_pressed = true;
            } else {
              foreign = !cleanup_right_pressed || cleanup_right_released;
              cleanup_right_released = true;
            }
          } else {
            foreign = true;
          }
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
  bool post_bridge_keymap_connection_lost = false;
  const auto ensure_layout = [&](int target) {
    const bool changed_group = target != group;
    const Deadline layout_deadline =
        hard_deadline - kDesktopLayoutCleanupBudget;
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
        outcome.rejection_stage = "layout_bridge_raw_key_preflight";
        invalidate_retained_context();
        return false;
      }
    }
    const auto bridge_wait = [&](Deadline until) {
      if (cancelled)
        return false;
      if (!wait_) {
        if (until > Clock::now())
          std::this_thread::sleep_until(std::min(until, layout_deadline));
        return Clock::now() < layout_deadline;
      }
      try {
        cancelled = !wait_(std::min(until, layout_deadline));
      } catch (...) {
        cancelled = true;
      }
      return !cancelled && Clock::now() < layout_deadline;
    };
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_set_layout_connection(connection.get());
#endif
    const DesktopLayoutResult bridge = desktop_layout_->switch_to(
        lease->info(), group, target, layout_deadline, bridge_wait);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_set_layout_connection(nullptr);
#endif
    if (bridge.status == DesktopLayoutStatus::Applied ||
        bridge.status == DesktopLayoutStatus::RequestedUnresolved) {
      outcome.layout_transition =
          DesktopLayoutTransitionState::RequestedUnresolved;
    }
    outcome.rejection_stage = bridge.stage;
    if (bridge.status != DesktopLayoutStatus::Applied &&
        bridge.status != DesktopLayoutStatus::Verified) {
      if (bridge.status == DesktopLayoutStatus::RequestedUnresolved)
        preserve_previous_context();
      else
        retire_retained_context();
      return false;
    }
    deadline = layout_deadline;
    std::optional<Deadline> bridge_target_idle_since;
    for (;;) {
      if (!stable_context_matches(
              false,
              outcome.layout_transition ==
                  DesktopLayoutTransitionState::RequestedUnresolved,
              true)) {
        const bool observation_timed_out =
            stable_context_observation_result ==
                std::optional{x11_detail::XcbOperationResult::TimedOut} &&
            (stable_context_failure == StableContextFailure::FocusObservation ||
             (stable_context_failure ==
                  StableContextFailure::PointerObservation &&
              !focus_return_pending));
        outcome.rejection_stage =
            (stable_context_failure == StableContextFailure::Wait ||
             observation_timed_out) &&
                    !cancelled && Clock::now() >= layout_deadline
                ? "layout_bridge_xkb_timeout"
                : "layout_bridge_context_after";
        report_context_failure(outcome.rejection_stage);
        return false;
      }
      x11_detail::XcbOperationResult layout_result{};
      const auto observed = idle_layout(connection, allowed_locks, deadline,
                                        &layout_result, nullptr, true);
      if (!observed) {
        outcome.rejection_stage =
            layout_result == x11_detail::XcbOperationResult::TimedOut
                ? "layout_bridge_xkb_observation_timeout"
                : "layout_bridge_xkb_state";
        if (observation_failure_invalidates(layout_result, true) ||
            !connection.is_open() || !lease->valid()) {
          invalidate_retained_context();
        } else {
          retire_retained_context();
        }
        return false;
      }
      if (observed->locked_mods != initial_state->locked_mods) {
        outcome.rejection_stage = "layout_bridge_xkb_state";
        invalidate_retained_context();
        return false;
      }
      if (observed->group != target) {
        if (observed->group != group) {
          outcome.rejection_stage = "layout_bridge_xkb_diverged";
          invalidate_retained_context();
          return false;
        }
        bridge_target_idle_since.reset();
      } else if (!bridge_target_idle_since) {
        bridge_target_idle_since = Clock::now();
      } else if (Clock::now() - *bridge_target_idle_since >=
                 kFastLayoutClientSettle) {
        const auto fresh_keyboard = make_keyboard_plan(connection, deadline);
        if (!fresh_keyboard) {
          post_bridge_keymap_connection_lost = !connection.is_open();
          outcome.rejection_stage = "layout_bridge_keymap_changed";
          invalidate_retained_context(!post_bridge_keymap_connection_lost);
          return false;
        }
        const auto history = validate_transition_raw_keys
                                 ? observe_transition_raw_keys(true)
                                 : RawKeyHistoryResult::Clear;
        if (*fresh_keyboard != *keyboard ||
            history != RawKeyHistoryResult::Clear) {
          outcome.rejection_stage = *fresh_keyboard != *keyboard
                                        ? "layout_bridge_keymap_changed"
                                        : "layout_bridge_foreign_key";
          invalidate_retained_context();
          return false;
        }
        group = target;
        outcome.observed_layout = target;
        outcome.layout_transition =
            DesktopLayoutTransitionState::TargetObserved;
        deadline = std::min(hard_deadline, Clock::now() + kMacroBudget);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
        if (changed_group) {
          punto_e2e_after_replay_layout(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  hard_deadline.time_since_epoch())
                  .count());
        }
#else
        (void)changed_group;
#endif
        return true;
      }
      if (!bridge_wait(Clock::now() + std::chrono::milliseconds{1})) {
        outcome.rejection_stage =
            cancelled ? "layout_bridge_cancelled" : "layout_bridge_xkb_timeout";
        retire_retained_context();
        return false;
      }
    }
  };

  // Forwarded input can reach X after our local key-up bookkeeping.
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
        retire_retained_context();
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

  std::optional<SelectionSnapshot> previous_clipboard;
  const auto snapshot_clipboard = [&] {
    outcome.rejection_stage = "clipboard_snapshot_targets";
    previous_clipboard =
        clipboard_->capture_payload(Selection::Clipboard, deadline);
    if (!previous_clipboard) {
      return false;
    }
    outcome.rejection_stage = "clipboard_snapshot_budget";
    if (!clipboard_time()) {
      return false;
    }
    return true;
  };
  const auto cleanup_context_matches = [&](bool expect_cleanup_right = false) {
    if (outcome.layout_transition !=
        DesktopLayoutTransitionState::RequestedUnresolved) {
      if (!context_matches(true, true)) {
        return false;
      }
      const auto history = expect_cleanup_right && validate_transition_raw_keys
                               ? observe_transition_raw_keys(true, true)
                               : RawKeyHistoryResult::Clear;
      if (history != RawKeyHistoryResult::Clear) {
        outcome.rejection_stage = history == RawKeyHistoryResult::Foreign
                                      ? "layout_cleanup_foreign_raw_key"
                                  : history == RawKeyHistoryResult::Failed
                                      ? "layout_cleanup_raw_key_observation"
                                      : "layout_cleanup_raw_key_incomplete";
        invalidate_retained_context();
        return false;
      }
      return true;
    }
    std::optional<Deadline> idle_since;
    std::optional<int> stable_group;
    while (Clock::now() + kClipboardBudget < deadline) {
      if (!stable_context_matches(true, true, true)) {
        if (stable_context_failure == StableContextFailure::FocusChanged) {
          if (wait(Clock::now() + std::chrono::milliseconds{1})) {
            continue;
          }
        }
        if (focus_return_pending) {
          invalidate_retained_context();
        }
        return false;
      }
      x11_detail::XcbOperationResult layout_result{};
      const auto observed = idle_layout(connection, allowed_locks, deadline,
                                        &layout_result, nullptr, true);
      if (!observed) {
        if (observation_failure_invalidates(layout_result, true) ||
            !connection.is_open() || !lease->valid()) {
          invalidate_retained_context();
        } else {
          retire_retained_context();
        }
        return false;
      }
      if (observed->locked_mods != initial_state->locked_mods ||
          (observed->group != group &&
           observed->group != outcome.target_layout)) {
        invalidate_retained_context();
        return false;
      }
      const auto keys = hotkey_keys();
      if (keys == HotkeyKeysResult::Foreign ||
          keys == HotkeyKeysResult::ObservationFailed) {
        invalidate_retained_context();
        return false;
      }
      if (!stable_group || *stable_group != observed->group) {
        stable_group = observed->group;
        idle_since = Clock::now();
      } else if (idle_since &&
                 Clock::now() - *idle_since >= kFastLayoutClientSettle) {
        const auto history =
            validate_transition_raw_keys
                ? observe_transition_raw_keys(true, expect_cleanup_right)
                : RawKeyHistoryResult::Clear;
        if (history != RawKeyHistoryResult::Clear) {
          outcome.rejection_stage = history == RawKeyHistoryResult::Foreign
                                        ? "layout_cleanup_foreign_raw_key"
                                    : history == RawKeyHistoryResult::Failed
                                        ? "layout_cleanup_raw_key_observation"
                                        : "layout_cleanup_raw_key_incomplete";
          invalidate_retained_context();
          return false;
        }
        group = observed->group;
        outcome.observed_layout = group;
        if (group == outcome.target_layout) {
          outcome.layout_transition =
              DesktopLayoutTransitionState::TargetObserved;
        }
        return true;
      }
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        return false;
      }
    }
    if (focus_return_pending) {
      invalidate_retained_context();
    }
    return false;
  };
  const auto cleanup_selection = [&](const SelectionRead &prepared,
                                     Deadline cleanup_limit) {
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    hard_deadline = Deadline{
        std::chrono::nanoseconds{punto_e2e_prepared_cleanup_hard_deadline(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                hard_deadline.time_since_epoch())
                .count())}};
    const auto fixture_pause =
        std::chrono::nanoseconds{punto_e2e_layout_cleanup_phase(1)};
    hard_deadline += fixture_pause;
    cleanup_limit += fixture_pause;
#endif
    deadline = std::min(
        {hard_deadline, cleanup_limit, Clock::now() + kSelectionCleanupBudget});
    if (!clipboard_time()) {
      return false;
    }
    if (!cleanup_context_matches()) {
      return false;
    }
    if (!clipboard_time()) {
      return false;
    }
    if (initial_editor) {
      auto observed = desktop_layout_->observe_editor(
          lease->info(), *editor_pid, deadline, wait_, initial_editor->token);
      gchar *hash = g_compute_checksum_for_data(
          G_CHECKSUM_SHA256,
          reinterpret_cast<const guchar *>(prepared.text.data()),
          prepared.text.size());
      const bool exact =
          observed.status == AccessibleStatus::Observed &&
          observed.range.start >= 0 &&
          observed.range.end == initial_editor->caret &&
          observed.range.end - observed.range.start ==
              g_utf8_strlen(prepared.text.data(),
                            static_cast<gssize>(prepared.text.size())) &&
          observed.range.sha256 == hash;
      g_free(hash);
      if (!exact || !editor_matches(observed.range)) {
        invalidate_retained_context();
        return false;
      }
    } else {
      const auto current =
          clipboard_->get_text_with_owner(Selection::Primary, deadline);
      if (!current || !same_selection(prepared, *current)) {
        invalidate_retained_context();
        return false;
      }
    }
    if (!cleanup_context_matches()) {
      return false;
    }
    if (!tap(connection, kRight, false, deadline)) {
      return false;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    const auto phase3_fixture_pause =
        std::chrono::nanoseconds{punto_e2e_layout_cleanup_phase(3)};
    hard_deadline += phase3_fixture_pause;
    deadline += phase3_fixture_pause;
#endif
    return true;
  };
  const auto cleanup_and_retain =
      [&](std::unique_ptr<RetainedWordSelection> &receipt, bool mark_rejected,
          Deadline cleanup_limit) {
        if (held_tab_paste && mark_rejected) {
          std::fprintf(
              stderr,
              "[punto] Held Tab refusal status=%d stage=%.*s "
              "remaining_ms=%lld\n",
              static_cast<int>(outcome.status),
              static_cast<int>(outcome.rejection_stage.size()),
              outcome.rejection_stage.data(),
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      hard_deadline - Clock::now())
                      .count()));
        }
        if (!receipt || !cleanup_selection(receipt->selection, cleanup_limit)) {
          return false;
        }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
        if (punto_e2e_revoke_cleanup_lease()) {
          lease->revoke_for_testing();
        }
#endif
        if (!wait(Clock::now() + kSelectionClientSettle)) {
          outcome.rejection_stage = "layout_cleanup_settle_timeout";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
          punto_e2e_cleanup_authority_rejected("timeout");
#endif
          return false;
        }
        if (!cleanup_context_matches(true)) {
          if (stable_context_failure == StableContextFailure::Session) {
            outcome.rejection_stage = "layout_cleanup_session";
          }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
          punto_e2e_cleanup_authority_rejected(
              stable_context_failure == StableContextFailure::Session
                  ? "session"
                  : "context");
#endif
          return false;
        }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
        const bool bypass_identity =
            punto_e2e_bypass_cleanup_selection_identity();
#else
        constexpr bool bypass_identity = false;
#endif
        if (!bypass_identity) {
          if (!clipboard_time()) {
            return false;
          }
          if (!clipboard_->selection_identity_matches(
                  Selection::Primary, receipt->selection, deadline)) {
            invalidate_retained_context();
            outcome.rejection_stage = "layout_cleanup_selection_identity";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
            punto_e2e_cleanup_authority_rejected("selection-identity");
#endif
            return false;
          }
        }
        if (mark_rejected) {
          outcome.status = WordEditStatus::Rejected;
        }
        if (preparation_lineage_revoked || focus_return_pending) {
          return true;
        }
        retained_word_selection_ = std::move(receipt);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
        punto_e2e_cleanup_receipt_retained();
#endif
        return true;
      };

  const auto finish_layout = [&] {
    return ensure_layout(outcome.target_layout);
  };
  const auto retain_completed_selection = [&](const SelectionRead &source) {
    if (!clipboard_time() || !stable_context_matches(true)) {
      return;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_completed_selection_read();
#endif
    const auto retained =
        clipboard_->get_text_with_owner(Selection::Primary, deadline);
    if (retained && same_selection(source, *retained) &&
        stable_context_matches(true)) {
      retained_word_selection_ = std::make_unique<RetainedWordSelection>(
          RetainedWordSelection{source, *initial_focus, lease->generation(),
                                request.text_input_sequence, false,
                                outcome.editor_token, editor_child});
      current_receipt_active = true;
    }
  };
  const auto selection_source_matches = [&](const SelectionRead &source,
                                            bool describe_rejection = false) {
    const auto reject = [&](const char *stage) {
      if (describe_rejection) {
        outcome.rejection_stage = stage;
        if (std::string_view{stage} != "selection_source_context_before" &&
            std::string_view{stage} != "selection_source_context_after")
          report_context_failure(stage);
      }
      return false;
    };
    if (!clipboard_time())
      return reject("selection_source_budget");
    if (!context_matches())
      return reject("selection_source_context_before");
    if (!editor_matches())
      return reject("selection_source_editor");
    const auto current =
        clipboard_->get_text_with_owner(Selection::Primary, deadline);
    if (!current)
      return reject("selection_source_primary_read");
    if (!same_selection(source, *current))
      return reject("selection_source_primary_identity");
    if (!context_matches())
      return reject("selection_source_context_after");
    return true;
  };
  const auto paste_replacement = [&](const SelectionRead *source, bool terminal,
                                     std::size_t erase_count = 0) {
    if (terminal) {
      outcome.rejection_stage = "terminal_paste_keymap";
      if (!resolve_locked_levels(
              connection, initial_state->locked_mods, keyboard->num_lock_mask,
              keyboard->xkb_map_identity, {}, {}, deadline, true)) {
        return;
      }
    }
    // This transport owns snapshot admission, including unrepresentable text.
    if (!snapshot_clipboard()) {
      return;
    }
    outcome.rejection_stage = "selection_confirmation";
    std::optional<SelectionRead> confirmed;
    if (source) {
      confirmed = clipboard_->get_text_with_owner(Selection::Primary, deadline);
      if (!confirmed || !same_selection(*source, *confirmed)) {
        return;
      }
    }
    const auto source_matches = [&] {
      return confirmed ? selection_source_matches(*confirmed)
                       : context_matches() && editor_matches();
    };
    if (!source_matches()) {
      return;
    }
    outcome.rejection_stage = "paste_layout_preflight";
    if (!finish_layout()) {
      return;
    }
    if (!renew_budget(kMacroBudget)) {
      return;
    }
    outcome.rejection_stage = "paste_source_before_publication";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_paste_publication();
#endif
    if (!source_matches()) {
      return;
    }
    pending_ = std::make_unique<PendingPaste>();
    pending_->previous = *previous_clipboard;
    pending_->retirement_deadline = Clock::now() +
                                    kPendingPasteNoReceiptBudget +
                                    kPendingPasteReceiptMaxBudget;
    outcome.rejection_stage = "clipboard_ownership";
    pending_->publication = clipboard_->set_payload_if_owner(
        Selection::Clipboard, previous_clipboard->origin,
        clipboard_->text_payload(outcome.replacement), deadline);
    if (!pending_->publication.submitted) {
      pending_.reset();
      return;
    }
    published_paste_generation = pending_->publication.generation;
    pending_->generation = pending_->publication.generation;
    if (!pending_->publication.completed) {
      outcome.rejection_stage = "clipboard_publication_uncertain";
      return;
    }
    outcome.status = WordEditStatus::PreparedNotReplayed;
    const auto generation = pending_->generation;
    const auto owns_pending = [&] {
      return clipboard_ && pending_ && pending_->generation == generation &&
             clipboard_->owns_generation(Selection::Clipboard, generation);
    };
    outcome.rejection_stage = "paste_receipt_arm";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_paste_receipt_arm();
#endif
    if (!owns_pending()) {
      return;
    }
    const auto receipt =
        confirmed
            ? clipboard_->arm_paste_receipt(Selection::Clipboard, *confirmed)
            : clipboard_->arm_paste_receipt(Selection::Clipboard);
    if (!receipt) {
      return;
    }
    const auto cancel_unsent_receipt = [&] {
      if (clipboard_) {
        clipboard_->cancel_paste_receipt(*receipt);
      }
      if (pending_ && pending_->generation == generation) {
        pending_->receipt.reset();
      }
    };
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_after_paste_receipt_arm();
#endif
    outcome.rejection_stage = "paste_context_before_dispatch";
    if (!source_matches() || !owns_pending()) {
      cancel_unsent_receipt();
      return;
    }
    if (erase_count != 0) {
      // Admit the UTF-8 transfer before the first destructive key. Paste does
      // not encode replacement characters through the client's input method.
      if (!begin_phase_budget(kTotalMacroBudget,
                              hard_deadline - kDesktopLayoutCleanupBudget)) {
        cancel_unsent_receipt();
        return;
      }
      outcome.rejection_stage = "terminal_erase";
      outcome.retry_policy = WordEditRetryPolicy::Never;
      for (std::size_t i = 0; i < erase_count; ++i) {
        if (!source_matches() || !owns_pending()) {
          cancel_unsent_receipt();
          return;
        }
        bool erase_queued = false;
        const bool accepted =
            tap(connection, kBackspace, false, deadline, false, &erase_queued);
        if (erase_queued) {
          outcome.status = WordEditStatus::PartialFailure;
        }
        if (!accepted) {
          cancel_unsent_receipt();
          return;
        }
      }
    }
    pending_->receipt = *receipt;
    clipboard_->activate_paste_receipt(
        *receipt, Clock::now() + kPendingPasteNoReceiptBudget);
    outcome.rejection_stage = "paste_submission";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_paste_chord();
#endif
    if (!source_matches() || !owns_pending()) {
      cancel_unsent_receipt();
      return;
    }
    bool paste_queued = false;
    // VTE's Ctrl+Shift+Insert requests CLIPBOARD without a layout-dependent
    // letter accelerator. Shift+Insert alone would request PRIMARY instead.
    const bool paste_accepted =
        tap(connection, terminal ? kTerminalPaste : kPaste, terminal, deadline,
            true, &paste_queued);
    if (!paste_queued) {
      cancel_unsent_receipt();
      return;
    }
    retire_retained_context();
    outcome.status = WordEditStatus::PartialFailure;
    if (pending_ && pending_->generation == generation) {
      pending_->edit_block_deadline =
          Clock::now() + kPendingPasteEditBlockBudget;
    }
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
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    const auto fixture_pause =
        std::chrono::nanoseconds{punto_e2e_after_paste_delivered(
            [](void *owner) { static_cast<WordEditor *>(owner)->pump(); },
            this)};
    deadline += fixture_pause;
    hard_deadline += fixture_pause;
#endif
    if (!renew_budget(kMacroBudget) ||
        !wait(Clock::now() + kExtendedLayoutClientSettle)) {
      return;
    }
    if (finish_layout()) {
      if (erase_count != 0) {
        // A counted terminal edit completes only after its temporary payload
        // retires. Preserve the delivered receipt's existing quiet deadline;
        // restarting it here would delay the immediately queued inverse.
        if (!begin_phase_budget(kTotalMacroBudget, hard_deadline)) {
          return;
        }
        if (pending_ && pending_->generation == generation) {
          pending_->restore_admitted = true;
        }
        while (pending_ && pending_->generation == generation) {
          pump();
          if (pending_ && pending_->generation == generation &&
              !wait(Clock::now() + std::chrono::milliseconds{1})) {
            return;
          }
        }
        if (!clipboard_ || !clipboard_->is_open() ||
            clipboard_->locally_owns_generation(Selection::Clipboard,
                                                generation) ||
            !context_matches()) {
          return;
        }
      }
      outcome.status = WordEditStatus::Dispatched;
      if (source) {
        retain_completed_selection(*source);
      }
    }
  };
  if (native_undo) {
    if (!ensure_layout(0) || !renew_budget(kMacroBudget) || !editor_matches())
      return outcome;
    bool undo_queued = false;
    const bool undo_accepted =
        tap(connection, kUndo, false, deadline, true, &undo_queued);
    if (!undo_queued)
      return outcome;
    outcome.status = WordEditStatus::PartialFailure;
    if (undo_accepted && finish_layout()) {
      outcome.status = WordEditStatus::Dispatched;
    }
    return outcome;
  }

  if (selection) {
    outcome.rejection_stage = "selection_source";
    auto source = clipboard_->get_text_with_owner(Selection::Primary, deadline);
    if (!source || source->text.empty() || source->text.size() > 4096 ||
        client_of(source->owner) != *active_client || !context_matches()) {
      return outcome;
    }
    const bool terminal = kind == ActiveWindowKind::Terminal;
    if (terminal && !terminal_text(source->text)) {
      outcome.rejection_stage = "terminal_source_text";
      return outcome;
    }
    outcome.rejection_stage = "selection_transform";
    switch (request.operation) {
    case WordEditOperation::SelectionLayout:
      outcome.replacement = invert_layout(source->text);
      outcome.target_layout = is_predominantly_cyrillic(source->text) ? 0 : 1;
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
    if (outcome.replacement.empty() || outcome.replacement == source->text ||
        outcome.replacement.size() > 4096 ||
        (terminal && !terminal_text(outcome.replacement))) {
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
        auto observed =
            clipboard_->get_text_with_owner(Selection::Primary, deadline);
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
        !terminal && outcome.replacement.find('\t') == std::string::npos
            ? plan_text(outcome.replacement, keyboard->alphabet,
                        static_cast<std::uint8_t>(outcome.target_layout))
            : std::optional<std::vector<Stroke>>{};
    std::vector<Stroke> no_expected;
    if (direct_replay &&
        !resolve_locked_levels(connection, initial_state->locked_mods,
                               keyboard->num_lock_mask,
                               keyboard->xkb_map_identity, no_expected,
                               *direct_replay, deadline)) {
      direct_replay.reset();
    }
    if (direct_replay &&
        request.operation != WordEditOperation::SelectionLayout &&
        std::ranges::any_of(*direct_replay, [&](const Stroke &stroke) {
          return stroke.group != group;
        })) {
      // A non-layout selection edit need not change the user's source merely
      // to synthesize characters. The existing paste transport is independent
      // of the active group and keeps its own clipboard lifetime.
      direct_replay.reset();
    }
    if (direct_replay) {
      outcome.rejection_stage = "selection_confirmation";
      auto confirmed = stable_selection_receipt(*source);
      if (!confirmed || !context_matches()) {
        return outcome;
      }
      // Each awaited hop shares this original replay deadline.
      const auto replay_limit = hard_deadline - kDesktopLayoutCleanupBudget -
                                kExtendedLayoutClientSettle;
      if (!begin_phase_budget(kTotalMacroBudget, replay_limit)) {
        return outcome;
      }
      outcome.rejection_stage = "selection_replay";
      bool replayed = true;
      bool replacement_started = false;
      for (const auto &stroke : *direct_replay) {
        if (stroke.group != group &&
            (!ensure_layout(stroke.group) ||
             !begin_phase_budget(kTotalMacroBudget, replay_limit))) {
          replayed = false;
          break;
        }
        if (!context_matches() || !editor_matches()) {
          replayed = false;
          break;
        }
        if (!replacement_started &&
            !selection_source_matches(*confirmed, true)) {
          replayed = false;
          break;
        }
        bool stroke_queued = false;
        const bool accepted = tap(connection, stroke.key, stroke.shifted,
                                  deadline, false, &stroke_queued);
        if (stroke_queued) {
          replacement_started = true;
          outcome.status = WordEditStatus::PartialFailure;
        }
        if (!accepted) {
          replayed = false;
          break;
        }
      }
      if (replayed && (!renew_budget(kMacroBudget) ||
                       !wait(Clock::now() + kExtendedLayoutClientSettle) ||
                       !stable_context_matches())) {
        replayed = false;
      }
      if (!replayed) {
        return outcome;
      }
      outcome.rejection_stage = "selection_layout_finalize";
      if (!ensure_layout(outcome.target_layout)) {
        return outcome;
      }
      outcome.status = WordEditStatus::Dispatched;
      retain_completed_selection(*confirmed);
      return outcome;
    }
    // Unrepresentable text has a separate clipboard-transfer lifetime. A held
    // Tab never enters this path, and direct replay does not touch CLIPBOARD.
    auto confirmed = stable_selection_receipt(*source);
    if (confirmed) {
      paste_replacement(&*confirmed, terminal);
    }
    return outcome;
  }

  outcome.original = request.expected;
  outcome.replacement =
      outcome.terminal_history ? terminal_replacement : request.replacement;
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
  if (terminal && !outcome.replacement.empty()) {
    paste_replacement(nullptr, true, expected->size());
    return outcome;
  }
  retire_retained_context();
  outcome.rejection_stage = "selection_prepare";
  outcome.retry_policy = WordEditRetryPolicy::Never;
  xcb_window_t prepared_owner = XCB_WINDOW_NONE;
  std::unique_ptr<RetainedWordSelection> prepared_receipt;
  std::unique_ptr<SelectionRead> confirmed_selection;
  std::unique_ptr<SelectionRead> prepared_selection;
  Deadline prepared_selection_since{};
  bool prepared_selection_stable = false;
  bool prepared_selection_foreign = false;
  bool current_word_extending = false;
  bool current_word_prefix_extending = false;
  std::size_t prefix_steps_remaining = 0;
  if (!terminal) {
    if (!primary_selection_baseline) {
      (void)clipboard_->pump_events();
      primary_selection_baseline =
          clipboard_->get_selection_identity(Selection::Primary, deadline);
      if (!primary_selection_baseline) {
        outcome.rejection_stage = "selection_prepare_primary_baseline";
        return outcome;
      }
    }
  }
  const auto observe_prepared_selection = [&](const SelectionRead &current,
                                              bool allow_stale_lineage) {
    if (prepared_selection_foreign) {
      return;
    }
    const auto current_extent = utf8_codepoint_count(current.text);
    const bool bounded_extent =
        client_of(current.owner) == *active_client && current_extent > 0 &&
        (current_word ? current.text.size() <= 4096
                      : current_extent <= expected->size());
    if (!bounded_extent) {
      if (prepared_selection) {
        prepared_selection_foreign = true;
      }
      return;
    }
    if (!prepared_selection) {
      if ((primary_selection_baseline->owner != XCB_WINDOW_NONE &&
           same_selection_identity(*primary_selection_baseline, current)) ||
          (!current_word && !allow_stale_lineage &&
           !request.expected.ends_with(current.text))) {
        return;
      }
      prepared_selection = std::make_unique<SelectionRead>(current);
      prepared_selection_since = Clock::now();
    } else if (same_selection(*prepared_selection, current)) {
      prepared_selection_stable =
          Clock::now() - prepared_selection_since >= kSelectionClientSettle;
      return;
    } else if (((!current_word && !prepared_selection_stable) ||
                current_word_extending) &&
               current_extent >
                   utf8_codepoint_count(prepared_selection->text) &&
               current.text.ends_with(prepared_selection->text) &&
               (!current_word_extending ||
                (current.owner == prepared_selection->owner &&
                 !same_selection_identity(*prepared_selection, current)))) {
      prepared_selection = std::make_unique<SelectionRead>(current);
      prepared_selection_since = Clock::now();
      prepared_selection_stable = false;
      current_word_extending = false;
    } else {
      prepared_selection_foreign = true;
      return;
    }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    if (current_word || current_extent == expected->size()) {
      deadline = Deadline{
          std::chrono::nanoseconds{punto_e2e_selection_observed_deadline(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  deadline.time_since_epoch())
                  .count())}};
    }
#endif
  };
  const auto cleanup_prepared_selection = [&](bool allow_stale_lineage) {
    const auto cleanup_started = Clock::now();
    if (cleanup_started >= hard_deadline) {
      return false;
    }
    deadline =
        std::min(hard_deadline, cleanup_started + kSelectionCleanupBudget);
    while (!prepared_selection_foreign &&
           (!prepared_selection ||
            (!prepared_selection_stable &&
             (current_word || prepared_selection->text != request.expected))) &&
           clipboard_->is_open() &&
           Clock::now() + kClipboardBudget < deadline) {
      (void)clipboard_->pump_events();
      const auto observed =
          clipboard_->get_text_with_owner(Selection::Primary, deadline);
      if (observed) {
        observe_prepared_selection(*observed, allow_stale_lineage);
      }
      if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
        break;
      }
    }
    if (!prepared_selection || prepared_selection_foreign ||
        (!prepared_selection_stable &&
         (current_word || prepared_selection->text != request.expected))) {
      if (!initial_editor || request.expected.empty())
        return false;
      auto observed = desktop_layout_->observe_editor(
          lease->info(), *editor_pid, deadline, wait_, initial_editor->token);
      gchar *hash = g_compute_checksum_for_data(
          G_CHECKSUM_SHA256,
          reinterpret_cast<const guchar *>(request.expected.data()),
          request.expected.size());
      const bool exact =
          observed.status == AccessibleStatus::Observed &&
          observed.range.end == initial_editor->caret &&
          observed.range.end - observed.range.start ==
              g_utf8_strlen(request.expected.data(),
                            static_cast<gssize>(request.expected.size())) &&
          observed.range.sha256 == hash && observed.range.start >= 0;
      g_free(hash);
      if (!exact || !cleanup_context_matches() ||
          !editor_matches(observed.range))
        return false;
      // PRIMARY's reader may have closed, but this independent continuous child
      // and exact range still authorize only selection collapse, never replay.
      return tap(connection, kRight, false, deadline);
    }
    if (!connection.is_open()) {
      if (!lease->valid()) {
        return false;
      }
      connection = lease->open_bounded_connection(deadline);
      if (!connection.is_open()) {
        return false;
      }
      auto cleanup_xkb = reply<xcb_xkb_use_extension_reply_t>(
          connection, xcb_xkb_use_extension(connection.get(), 1, 0).sequence,
          deadline);
      if (!cleanup_xkb || !cleanup_xkb->supported ||
          !prepare_transition_extensions()) {
        return false;
      }
    }
    auto receipt = std::make_unique<RetainedWordSelection>(
        RetainedWordSelection{*prepared_selection, *initial_focus,
                              lease->generation(), request.text_input_sequence,
                              false, outcome.editor_token, editor_child});
    return cleanup_and_retain(receipt, false, deadline);
  };
  bool selection_write_queued = false;
  if (terminal &&
      !begin_phase_budget(kTotalMacroBudget,
                          hard_deadline - kDesktopLayoutCleanupBudget)) {
    return outcome;
  }
  const auto selection_strokes =
      current_word ? std::size_t{1} : expected->size();
  const auto log_selection_refusal = [&](const char *phase,
                                         std::size_t stroke) {
    const auto now = Clock::now();
    std::fprintf(
        stderr,
        "[punto] Selection preparation refused phase=%s stroke=%zu "
        "context_failure=%u prepared=%d cancelled=%d connection_open=%d "
        "elapsed_us=%lld remaining_us=%lld\n",
        phase, stroke, static_cast<unsigned>(stable_context_failure),
        selection_write_queued, cancelled, connection.is_open(),
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(now - started)
                .count()),
        static_cast<long long>(
            std::chrono::duration_cast<std::chrono::microseconds>(deadline -
                                                                  now)
                .count()));
  };
  for (std::size_t i = 0; i < selection_strokes; ++i) {
    if (!context_matches() || !editor_matches()) {
      log_selection_refusal("context", i);
      if (!terminal && selection_write_queued) {
        (void)cleanup_prepared_selection(false);
      }
      return outcome;
    }
    bool stroke_queued = false;
    const bool accepted =
        tap(connection, terminal ? kBackspace : kLeft, !terminal, deadline,
            current_word, &stroke_queued);
    if (stroke_queued) {
      outcome.status = terminal ? WordEditStatus::PartialFailure
                                : WordEditStatus::PreparedNotReplayed;
      selection_write_queued = true;
    }
    if (!accepted) {
      log_selection_refusal("tap", i);
      if (!terminal && selection_write_queued) {
        (void)cleanup_prepared_selection(false);
      }
      return outcome;
    }
  }
  if (!terminal) {
    std::size_t native_units = 1;
    for (;;) {
      outcome.rejection_stage = "selection_confirmation";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      deadline = Deadline{
          std::chrono::nanoseconds{punto_e2e_selection_confirmation_deadline(
              std::chrono::duration_cast<std::chrono::nanoseconds>(
                  deadline.time_since_epoch())
                  .count())}};
#endif
      const auto confirmation_limit = hard_deadline - kSelectionCleanupBudget;
      if (!begin_phase_budget(kMacroBudget, confirmation_limit)) {
        (void)cleanup_prepared_selection(true);
        return outcome;
      }
      Deadline confirmed_since{};
      bool selection_confirmed = false;
      std::size_t confirmation_reads = 0;
      std::size_t confirmation_read_failures = 0;
      std::size_t confirmation_expected_reads = 0;
      while (clipboard_->is_open() &&
             Clock::now() + std::chrono::milliseconds{30} < deadline &&
             context_matches()) {
        (void)clipboard_->pump_events();
        auto observed =
            clipboard_->get_text_with_owner(Selection::Primary, deadline);
        if (observed) {
          ++confirmation_reads;
          SelectionRead current = std::move(observed).value();
          if (client_of(current.owner) == *active_client) {
            observe_prepared_selection(current, true);
            if (!prepared_selection_foreign && !current_word_extending &&
                prepared_selection &&
                (current_word ? same_selection(*prepared_selection, current)
                              : current.text == request.expected)) {
              ++confirmation_expected_reads;
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
          } else if (prepared_selection) {
            prepared_selection_foreign = true;
          }
        } else {
          ++confirmation_read_failures;
        }
        if (!wait(Clock::now() + std::chrono::milliseconds{1})) {
          break;
        }
      }
      const bool confirmation_context_matches = context_matches();
      if (!selection_confirmed || !confirmed_selection ||
          !confirmation_context_matches) {
        const char *confirmation_failure =
            !confirmation_context_matches ? "selection_confirmation_context"
            : prepared_selection_foreign  ? "selection_confirmation_foreign"
            : !confirmed_selection        ? "selection_confirmation_missing"
                                          : "selection_confirmation_unstable";
        outcome.rejection_stage = confirmation_failure;
        std::fprintf(
            stderr,
            "[punto] Selection confirmation failed stage=%s reads=%zu "
            "read_failures=%zu expected_reads=%zu prepared=%d stable=%d\n",
            confirmation_failure, confirmation_reads,
            confirmation_read_failures, confirmation_expected_reads,
            prepared_selection != nullptr, prepared_selection_stable);
        if (!cleanup_prepared_selection(true) &&
            outcome.rejection_stage == confirmation_failure) {
          outcome.rejection_stage = "selection_confirmation_cleanup_unresolved";
        }
        return outcome;
      }
      if (!current_word) {
        break;
      }
      if (current_word_prefix_extending) {
        const auto expected_suffix =
            std::string_view{request.expected}.substr(static_cast<std::size_t>(
                g_utf8_offset_to_pointer(
                    request.expected.data(),
                    static_cast<glong>(prefix_steps_remaining)) -
                request.expected.data()));
        if (confirmed_selection->text != expected_suffix) {
          outcome.rejection_stage = "current_word_prefix_range";
          (void)cleanup_prepared_selection(false);
          return outcome;
        }
        if (prefix_steps_remaining == 0) {
          break;
        }
      } else if (has_word_character(confirmed_selection->text)) {
        if (request.operation != WordEditOperation::CurrentWordLayout ||
            request.expected.size() <= confirmed_selection->text.size() ||
            !request.expected.ends_with(confirmed_selection->text) ||
            !g_utf8_validate(request.expected.data(),
                             static_cast<gssize>(request.expected.size()),
                             nullptr) ||
            utf8_codepoint_count(request.expected) > kMaxCharacters ||
            !is_keyboard_word_prefix(std::string_view{request.expected}.substr(
                0,
                request.expected.size() - confirmed_selection->text.size()))) {
          break;
        }
        current_word_prefix_extending = true;
        prefix_steps_remaining =
            utf8_codepoint_count(request.expected) -
            utf8_codepoint_count(confirmed_selection->text);
      }
      // Each fixed prefix step needs its own exact fresh range. Delimiters
      // permit only one additional native word unit, never an open-ended scan.
      outcome.rejection_stage = current_word_prefix_extending
                                    ? "current_word_prefix_range"
                                    : "current_word_delimiter_range";
      if ((!current_word_prefix_extending && native_units == 2) ||
          !g_utf8_validate(
              confirmed_selection->text.data(),
              static_cast<gssize>(confirmed_selection->text.size()), nullptr) ||
          utf8_codepoint_count(confirmed_selection->text) >= kMaxCharacters ||
          !context_matches()) {
        (void)cleanup_prepared_selection(false);
        return outcome;
      }
      const auto revalidated =
          clipboard_->get_text_with_owner(Selection::Primary, deadline);
      if (!revalidated || !same_selection(*confirmed_selection, *revalidated) ||
          !context_matches()) {
        (void)cleanup_prepared_selection(false);
        return outcome;
      }
      if (current_word_prefix_extending) {
        --prefix_steps_remaining;
      } else {
        ++native_units;
      }
      current_word_extending = true;
      confirmed_selection.reset();
      if (!tap(connection, kLeft, true, deadline,
               !current_word_prefix_extending)) {
        (void)cleanup_prepared_selection(false);
        return outcome;
      }
    }
    if (current_word) {
      // Receiving a bounded cleanup receipt is not permission to replay it.
      if (utf8_codepoint_count(confirmed_selection->text) > kMaxCharacters ||
          !g_utf8_validate(
              confirmed_selection->text.data(),
              static_cast<gssize>(confirmed_selection->text.size()), nullptr)) {
        outcome.rejection_stage = "current_word_range_limit";
        (void)cleanup_prepared_selection(false);
        return outcome;
      }
      outcome.original = confirmed_selection->text;
      outcome.replacement =
          request.operation == WordEditOperation::CurrentWordCase
              ? invert_case(outcome.original)
              : invert_layout(outcome.original);
      if (request.operation == WordEditOperation::CurrentWordLayout)
        outcome.target_layout =
            is_predominantly_cyrillic(outcome.original) ? 0 : 1;
      paste_word = outcome.original.find('\t') != std::string::npos ||
                   outcome.replacement.find('\t') != std::string::npos;
      replacement = plan_text(outcome.replacement, keyboard->alphabet,
                              static_cast<std::uint8_t>(outcome.target_layout));
      std::vector<Stroke> no_expected;
      if (!replacement || replacement->empty() ||
          outcome.replacement == outcome.original ||
          !resolve_locked_levels(connection, initial_state->locked_mods,
                                 keyboard->num_lock_mask,
                                 keyboard->xkb_map_identity, no_expected,
                                 *replacement, deadline)) {
        outcome.rejection_stage = "current_word_text_plan";
        (void)cleanup_prepared_selection(false);
        return outcome;
      }
    }
    if (paste_word) {
      paste_replacement(confirmed_selection.get(), false);
      if (outcome.status == WordEditStatus::PreparedNotReplayed) {
        (void)cleanup_selection(*confirmed_selection, hard_deadline);
      }
      return outcome;
    }
    prepared_owner = confirmed_selection->owner;
    prepared_receipt = std::make_unique<RetainedWordSelection>(
        RetainedWordSelection{*confirmed_selection, *initial_focus,
                              lease->generation(), request.text_input_sequence,
                              false, outcome.editor_token, editor_child});
  }

  if (!terminal) {
    outcome.rejection_stage = "layout_transition_preflight";
    validate_transition_raw_keys =
        observe_gui_word_transition &&
        outcome.target_layout != initial_state->group;
    const bool layout_ready = finish_layout();
    if (!layout_ready) {
      // Failed cleanup must not orphan our confirmed temporary selection.
      // Retain lineage only; fresh input still needs a new proven range.
      ScopeExit failed_preparation_lineage{[&] {
        if (prepared_receipt && !preparation_lineage_revoked &&
            !focus_return_pending) {
          prepared_receipt->requires_fresh_text_input = true;
          if (revoked_word_context_ &&
              revoked_word_context_->focus_window == *initial_focus &&
              revoked_word_context_->session_generation ==
                  lease->generation() &&
              revoked_word_context_->editor_child == editor_child &&
              revoked_word_context_->text_input_sequence ==
                  request.text_input_sequence) {
            revoked_word_context_.reset();
          }
          retained_word_selection_ = std::move(prepared_receipt);
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
          punto_e2e_cleanup_receipt_retained();
#endif
        }
      }};
      // A protocol error closes the transaction connection.  The XI2 interval
      // across a replacement connection is unknowable, so do not collapse the
      // selection.  Keep only its exact ClipboardManager identity as lineage;
      // it cannot authorize replay of the old request.
      if (post_bridge_keymap_connection_lost && prepared_receipt) {
        validate_transition_raw_keys = false;
        return outcome;
      }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      const auto fixture_pause =
          std::chrono::nanoseconds{punto_e2e_layout_cleanup_phase(0)};
      hard_deadline += fixture_pause;
      deadline += fixture_pause;
#endif
      const auto cleanup_started = Clock::now();
      if (cleanup_started >= hard_deadline) {
        return outcome;
      }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      punto_e2e_delay_layout_cleanup();
#endif
      deadline = std::min(hard_deadline,
                          cleanup_started + kDesktopLayoutCleanupBudget);
      if (cleanup_and_retain(prepared_receipt, true, deadline) &&
          !preparation_lineage_revoked && !focus_return_pending) {
        if (outcome.layout_transition !=
            DesktopLayoutTransitionState::TargetObserved) {
          outcome.retry_policy = WordEditRetryPolicy::AfterFreshInput;
        }
      }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
      (void)punto_e2e_layout_cleanup_phase(2);
#endif
      validate_transition_raw_keys = false;
      return outcome;
    }
    validate_transition_raw_keys = false;
    outcome.rejection_stage = "selection_revalidation";
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
    punto_e2e_before_selection_revalidation();
#endif
    (void)clipboard_->pump_events();
    const auto revalidated =
        clipboard_->get_text_with_owner(Selection::Primary, deadline);
    if (!revalidated ||
        !same_selection(prepared_receipt->selection, *revalidated) ||
        !context_matches(false, true)) {
      (void)cleanup_and_retain(prepared_receipt, true, hard_deadline);
      return outcome;
    }
    if (held_tab_paste) {
      // GTK retains the source widget for a whole paste, unlike later key
      // strokes that a nested deletion handler can deliver to a new focus.
      paste_replacement(&prepared_receipt->selection, false);
      if (outcome.status == WordEditStatus::PreparedNotReplayed) {
        (void)cleanup_and_retain(prepared_receipt, true, hard_deadline);
      }
      return outcome;
    }
  }

  outcome.rejection_stage = "replacement_replay";
  if (!terminal &&
      !begin_phase_budget(kTotalMacroBudget,
                          hard_deadline - kDesktopLayoutCleanupBudget)) {
    (void)cleanup_and_retain(prepared_receipt, true, hard_deadline);
    return outcome;
  }
#if defined(PUNTO_EVENT_LOOP_E2E_TESTING)
  punto_e2e_before_word_replay();
#endif
  bool replacement_replayed = true;
  bool replacement_started = false;
  for (const auto &stroke : *replacement) {
    if (!context_matches() || (!terminal && !editor_matches())) {
      replacement_replayed = false;
      break;
    }
    if (stroke.group != group) {
      outcome.rejection_stage = "replay_layout";
      if (!ensure_layout(stroke.group) ||
          !begin_phase_budget(kTotalMacroBudget,
                              hard_deadline - kDesktopLayoutCleanupBudget) ||
          !context_matches() || (!terminal && !editor_matches())) {
        replacement_replayed = false;
        break;
      }
    }
    if (!replacement_started && !terminal &&
        !selection_source_matches(prepared_receipt->selection)) {
      outcome.rejection_stage = "word_source_before_replay";
      (void)cleanup_and_retain(prepared_receipt, true, hard_deadline);
      replacement_replayed = false;
      break;
    }
    bool stroke_queued = false;
    const bool accepted = tap(connection, stroke.key, stroke.shifted, deadline,
                              false, &stroke_queued);
    if (stroke_queued) {
      replacement_started = true;
      outcome.status = WordEditStatus::PartialFailure;
    }
    if (!accepted) {
      replacement_replayed = false;
      break;
    }
  }
  if (!replacement_replayed || !renew_budget(kMacroBudget)) {
    return outcome;
  }
  outcome.rejection_stage = "layout_transition_finalize";
  if (finish_layout()) {
    outcome.status = WordEditStatus::Dispatched;
    revoked_word_context_.reset();
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
