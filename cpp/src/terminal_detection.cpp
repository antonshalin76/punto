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
// терминала/DE/Wayland(XWayland). Здесь храним *токены* для подстрочного поиска.
constexpr std::array kTerminalTokens = {
    "gnome-terminal",
    "gnome-terminal-server",
    "org.gnome.Terminal",
    "kgx",                // GNOME Console
    "org.gnome.Console",  // GNOME Console
    "ptyxis",             // GNOME Ptyxis
    "org.gnome.Ptyxis",   // GNOME Ptyxis

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

/// Проверяет, содержит ли строка подстроку (case insensitive)
[[nodiscard]] bool contains_ci(std::string_view haystack,
                              std::string_view needle) noexcept {
  if (needle.empty()) {
    return true;
  }
  if (haystack.size() < needle.size()) {
    return false;
  }

  auto it = std::search(
      haystack.begin(), haystack.end(), needle.begin(), needle.end(),
      [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
      });

  return it != haystack.end();
}

} // namespace

bool is_terminal_wm_class(std::string_view res_name,
                          std::string_view res_class) noexcept {
  if (res_name.empty() && res_class.empty()) {
    return false;
  }

  for (const auto &token : kTerminalTokens) {
    if (contains_ci(res_class, token) || contains_ci(res_name, token)) {
      return true;
    }
  }

  return false;
}

} // namespace punto
