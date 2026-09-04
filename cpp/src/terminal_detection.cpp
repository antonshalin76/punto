/**
 * @file terminal_detection.cpp
 * @brief Реализация утилит детекции терминала по WM_CLASS
 */

#include "punto/terminal_detection.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace punto {

namespace {

// Список терминальных эмуляторов (WM_CLASS instance/class)
//
// Важно: WM_CLASS состоит из двух строк (res_name/res_class) и зависит от
// терминала/DE/Wayland(XWayland). Токены совпадают только с целым компонентом
// идентификатора: короткое имя "st" не должно матчить Postman или Studio.
constexpr std::array kTerminalTokens = {
    "gnome-terminal",
    "gnome-terminal-server",
    "org.gnome.Terminal",
    "kgx",               // GNOME Console
    "org.gnome.Console", // GNOME Console
    "ptyxis",            // GNOME Ptyxis
    "org.gnome.Ptyxis",  // GNOME Ptyxis

    // Warp terminal
    "dev.warp.Warp",

    "konsole",
    "org.kde.konsole",

    "xterm",
    "rxvt",
    "urxvt",

    "terminator",
    "tilix",

    "alacritty",
    "org.alacritty.Alacritty",

    "kitty",

    "wezterm",
    "org.wezfurlong.wezterm",

    "ghostty",
    "com.mitchellh.ghostty",

    "terminology",
    "xfce4-terminal",
    "mate-terminal",
    "lxterminal",
    "qterminal",
    "sakura",
    "termite",
    "st",
    "foot",

    // Common generic token (covers e.g. "*Terminal*")
    "terminal",
};

[[nodiscard]] bool is_identifier_byte(char value) noexcept {
  const auto byte = static_cast<unsigned char>(value);
  return std::isalnum(byte) != 0;
}

/// Проверяет наличие отдельного компонента идентификатора (case insensitive).
[[nodiscard]] bool contains_component_ci(std::string_view haystack,
                                         std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (haystack.size() < needle.size()) {
    return false;
  }

  auto it = std::search(haystack.begin(), haystack.end(), needle.begin(),
                        needle.end(), [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });

  while (it != haystack.end()) {
    const std::size_t offset =
        static_cast<std::size_t>(std::distance(haystack.begin(), it));
    const std::size_t end = offset + needle.size();
    const bool left_boundary =
        offset == 0 || !is_identifier_byte(haystack[offset - 1]);
    const bool right_boundary =
        end == haystack.size() || !is_identifier_byte(haystack[end]);
    if (left_boundary && right_boundary) {
      return true;
    }
    it = std::search(it + 1, haystack.end(), needle.begin(), needle.end(),
                     [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                     });
  }
  return false;
}

} // namespace

bool is_terminal_wm_class(std::string_view res_name,
                          std::string_view res_class) noexcept {
  if (res_name.empty() && res_class.empty()) {
    return false;
  }

  for (const auto &token : kTerminalTokens) {
    if (contains_component_ci(res_class, token) ||
        contains_component_ci(res_name, token)) {
      return true;
    }
  }

  return false;
}

} // namespace punto
