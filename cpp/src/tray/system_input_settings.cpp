/**
 * @file system_input_settings.cpp
 * @brief Реализация чтения/записи системных настроек хоткея переключения раскладки
 */

#include "punto/system_input_settings.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>

namespace punto {

namespace {

constexpr const char* kGnomeKeybindingsSchema = "org.gnome.desktop.wm.keybindings";
constexpr const char* kGnomeKeySwitchNext = "switch-input-source";
constexpr const char* kGnomeKeySwitchPrev = "switch-input-source-backward";

struct SpawnOutcome {
  bool ok = false;
  int exit_status = -1;
  std::string stdout_str;
  std::string stderr_str;
  std::string error;
};

[[nodiscard]] SpawnOutcome spawn_sync(const std::vector<std::string>& args) {
  SpawnOutcome out;

  std::vector<gchar*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& a : args) {
    argv.push_back(const_cast<gchar*>(a.c_str()));
  }
  argv.push_back(nullptr);

  gchar* stdout_buf = nullptr;
  gchar* stderr_buf = nullptr;
  gint status = -1;
  GError* err = nullptr;

  const gboolean ok = g_spawn_sync(nullptr,
                                  argv.data(),
                                  nullptr,
                                  G_SPAWN_SEARCH_PATH,
                                  nullptr,
                                  nullptr,
                                  &stdout_buf,
                                  &stderr_buf,
                                  &status,
                                  &err);

  out.exit_status = status;
  if (stdout_buf) {
    out.stdout_str.assign(stdout_buf);
    g_free(stdout_buf);
  }
  if (stderr_buf) {
    out.stderr_str.assign(stderr_buf);
    g_free(stderr_buf);
  }

  if (!ok) {
    out.ok = false;
    if (err) {
      out.error.assign(err->message ? err->message : "unknown error");
      g_error_free(err);
    } else {
      out.error = "g_spawn_sync failed";
    }
    return out;
  }

  if (err) {
    // Теоретически возможно при ok==true (некоторые GLib API так делают)
    out.error.assign(err->message ? err->message : "unknown error");
    g_error_free(err);
  }

  if (status != 0) {
    out.ok = false;
    if (out.error.empty()) {
      out.error = "command exited with non-zero status";
    }
    return out;
  }

  out.ok = true;
  return out;
}

[[nodiscard]] bool gnome_schema_available() {
  GSettingsSchemaSource* source = g_settings_schema_source_get_default();
  if (!source) {
    return false;
  }

  GSettingsSchema* schema = g_settings_schema_source_lookup(
      source, kGnomeKeybindingsSchema, TRUE);
  if (!schema) {
    return false;
  }

  g_settings_schema_unref(schema);
  return true;
}

[[nodiscard]] bool is_gnome_desktop_session() {
  const gchar* desktop = g_getenv("XDG_CURRENT_DESKTOP");
  if (!desktop || *desktop == '\0') {
    return false;
  }

  std::string s{desktop};
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

  // Ubuntu обычно выставляет "ubuntu:GNOME".
  return s.find("gnome") != std::string::npos;
}

[[nodiscard]] std::optional<std::string_view> first_strv(char** strv) {
  if (!strv) {
    return std::nullopt;
  }
  if (!strv[0]) {
    return std::nullopt;
  }
  return std::string_view{strv[0]};
}

[[nodiscard]] std::optional<std::string> gdk_keyval_to_id(guint keyval) {
  switch (keyval) {
  case GDK_KEY_space:
    return std::string{"space"};
  case GDK_KEY_grave:
    return std::string{"grave"};
  case GDK_KEY_Tab:
    return std::string{"tab"};
  case GDK_KEY_Caps_Lock:
    return std::string{"capslock"};
  case GDK_KEY_backslash:
    return std::string{"backslash"};

  case GDK_KEY_Shift_L:
    return std::string{"leftshift"};
  case GDK_KEY_Shift_R:
    return std::string{"rightshift"};

  case GDK_KEY_Control_L:
    return std::string{"leftctrl"};
  case GDK_KEY_Control_R:
    return std::string{"rightctrl"};

  case GDK_KEY_Alt_L:
  case GDK_KEY_Meta_L:
    return std::string{"leftalt"};
  case GDK_KEY_Alt_R:
  case GDK_KEY_Meta_R:
    return std::string{"rightalt"};

  case GDK_KEY_Super_L:
    return std::string{"leftmeta"};
  case GDK_KEY_Super_R:
    return std::string{"rightmeta"};
  default:
    break;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<guint> id_to_gdk_keyval(std::string_view id) {
  if (id == "space") return GDK_KEY_space;
  if (id == "grave") return GDK_KEY_grave;
  if (id == "tab") return GDK_KEY_Tab;
  if (id == "capslock") return GDK_KEY_Caps_Lock;
  if (id == "backslash") return GDK_KEY_backslash;

  if (id == "leftshift") return GDK_KEY_Shift_L;
  if (id == "rightshift") return GDK_KEY_Shift_R;

  if (id == "leftctrl") return GDK_KEY_Control_L;
  if (id == "rightctrl") return GDK_KEY_Control_R;

  if (id == "leftalt") return GDK_KEY_Alt_L;
  if (id == "rightalt") return GDK_KEY_Alt_R;

  if (id == "leftmeta") return GDK_KEY_Super_L;
  if (id == "rightmeta") return GDK_KEY_Super_R;

  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> gdk_mods_to_modifier_id(GdkModifierType mods) {
  bool has_ctrl = (mods & GDK_CONTROL_MASK) != 0;
  bool has_alt = (mods & GDK_MOD1_MASK) != 0;
  bool has_shift = (mods & GDK_SHIFT_MASK) != 0;
  bool has_super = (mods & (GDK_SUPER_MASK | GDK_META_MASK)) != 0;

  const int count = static_cast<int>(has_ctrl) + static_cast<int>(has_alt) +
                    static_cast<int>(has_shift) + static_cast<int>(has_super);
  if (count != 1) {
    return std::nullopt;
  }

  if (has_ctrl) return std::string{"leftctrl"};
  if (has_alt) return std::string{"leftalt"};
  if (has_shift) return std::string{"leftshift"};
  if (has_super) return std::string{"leftmeta"};

  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> build_gnome_accel(const LayoutToggle& t,
                                                          std::string& out_error) {
  std::string mod;
  if (t.modifier == "leftctrl" || t.modifier == "rightctrl") {
    mod = "<Control>";
  } else if (t.modifier == "leftalt" || t.modifier == "rightalt") {
    mod = "<Alt>";
  } else if (t.modifier == "leftshift" || t.modifier == "rightshift") {
    mod = "<Shift>";
  } else if (t.modifier == "leftmeta" || t.modifier == "rightmeta") {
    mod = "<Super>";
  } else {
    out_error = "Unsupported modifier id: " + t.modifier;
    return std::nullopt;
  }

  auto keyval = id_to_gdk_keyval(t.key);
  if (!keyval) {
    out_error = "Unsupported key id: " + t.key;
    return std::nullopt;
  }

  const gchar* key_name = gdk_keyval_name(*keyval);
  if (!key_name || *key_name == '\0') {
    out_error = "Failed to resolve key name for id: " + t.key;
    return std::nullopt;
  }

  std::string accel;
  accel.reserve(mod.size() + std::strlen(key_name));
  accel += mod;
  accel += key_name;
  return accel;
}

[[nodiscard]] SystemInputReadOutcome read_gnome() {
  SystemInputReadOutcome out;
  out.backend = "gnome";

  GSettings* s = g_settings_new(kGnomeKeybindingsSchema);
  if (!s) {
    out.result = SystemInputResult::Error;
    out.error = "Failed to create GSettings for GNOME";
    return out;
  }

  gchar** strv = g_settings_get_strv(s, kGnomeKeySwitchNext);
  auto first = first_strv(strv);
  if (!first) {
    out.result = SystemInputResult::Error;
    out.error = "GNOME keybinding is empty: switch-input-source";
    if (strv) {
      g_strfreev(strv);
    }
    g_object_unref(s);
    return out;
  }

  out.raw.assign(first->begin(), first->end());

  guint accel_key = 0;
  GdkModifierType accel_mods = static_cast<GdkModifierType>(0);
  gtk_accelerator_parse(out.raw.c_str(), &accel_key, &accel_mods);

  auto mod_id = gdk_mods_to_modifier_id(accel_mods);
  auto key_id = gdk_keyval_to_id(accel_key);

  if (!mod_id || !key_id) {
    out.result = SystemInputResult::Unsupported;
    out.error = "Системная комбинация не представима как 'один модификатор + одна клавиша' из поддерживаемого набора";
  } else {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{*mod_id, *key_id};
  }

  if (strv) {
    g_strfreev(strv);
  }
  g_object_unref(s);
  return out;
}

[[nodiscard]] bool has_display() {
  const gchar* disp = g_getenv("DISPLAY");
  return disp && *disp != '\0';
}

[[nodiscard]] std::vector<std::string> split_csv_to_strings(std::string_view csv) {
  std::vector<std::string> out;

  size_t start = 0;
  while (start < csv.size()) {
    size_t comma = csv.find(',', start);
    if (comma == std::string_view::npos) {
      comma = csv.size();
    }

    std::string_view part = csv.substr(start, comma - start);

    // Trim через GLib, чтобы не плодить локальные trim() в проекте.
    gchar* buf = g_strndup(part.data(), part.size());
    gchar* stripped = g_strstrip(buf);
    if (stripped && *stripped) {
      out.emplace_back(stripped);
    }
    g_free(buf);

    start = comma + 1;
  }

  return out;
}

[[nodiscard]] SystemInputReadOutcome read_x11() {
  SystemInputReadOutcome out;
  out.backend = "x11";

  if (!has_display()) {
    out.result = SystemInputResult::NotAvailable;
    out.error = "DISPLAY is not set";
    return out;
  }

  const SpawnOutcome q = spawn_sync({"setxkbmap", "-query"});
  if (!q.ok) {
    out.result = SystemInputResult::Error;
    out.error = q.error.empty() ? "setxkbmap -query failed" : q.error;
    if (!q.stderr_str.empty()) {
      out.error += ": ";
      out.error += q.stderr_str;
    }
    return out;
  }

  std::istringstream ss(q.stdout_str);
  std::string line;
  std::string options_csv;

  while (std::getline(ss, line)) {
    const std::string_view sv{line};
    const std::string_view prefix = "options:";
    if (sv.substr(0, prefix.size()) == prefix) {
      options_csv = std::string{sv.substr(prefix.size())};
      break;
    }
  }

  // options_csv может быть пустым.
  std::vector<std::string> opts = split_csv_to_strings(options_csv);

  std::string grp;
  for (const auto& o : opts) {
    if (o.rfind("grp:", 0) == 0 && o.ends_with("_toggle")) {
      grp = o;
      break;
    }
  }

  if (grp.empty()) {
    out.result = SystemInputResult::Unsupported;
    out.error = "Не найден XKB параметр grp:*_toggle (setxkbmap -query)";
    return out;
  }

  out.raw = grp;

  // Маппинг самых распространённых grp:*_toggle опций.
  if (grp == "grp:alt_shift_toggle" || grp == "grp:lalt_lshift_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftalt", "leftshift"};
    return out;
  }
  if (grp == "grp:ralt_rshift_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"rightalt", "rightshift"};
    return out;
  }
  if (grp == "grp:ctrl_shift_toggle" || grp == "grp:lctrl_lshift_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftctrl", "leftshift"};
    return out;
  }
  if (grp == "grp:rctrl_rshift_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"rightctrl", "rightshift"};
    return out;
  }
  if (grp == "grp:ctrl_alt_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftctrl", "leftalt"};
    return out;
  }
  if (grp == "grp:alt_space_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftalt", "space"};
    return out;
  }
  if (grp == "grp:win_space_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftmeta", "space"};
    return out;
  }
  if (grp == "grp:ctrl_space_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftctrl", "space"};
    return out;
  }
  if (grp == "grp:shift_caps_toggle") {
    out.result = SystemInputResult::Ok;
    out.toggle = LayoutToggle{"leftshift", "capslock"};
    return out;
  }

  // Есть много вариантов, включая одиночные клавиши (grp:caps_toggle и т.д.),
  // которые текущая модель (modifier+key) не может описать.
  out.result = SystemInputResult::Unsupported;
  out.error = "XKB опция не поддерживается текущей моделью хоткея";
  return out;
}

[[nodiscard]] std::optional<std::string> map_toggle_to_xkb_option(
    const LayoutToggle& t,
    std::string& out_error) {
  // Поддерживаем только варианты, которые есть в evdev.lst и которые описываются как 2-key chord.

  if ((t.modifier == "leftalt" || t.modifier == "rightalt") &&
      (t.key == "leftshift" || t.key == "rightshift")) {
    if (t.modifier == "rightalt" && t.key == "rightshift") {
      return std::string{"grp:ralt_rshift_toggle"};
    }
    if (t.modifier == "leftalt" && t.key == "leftshift") {
      return std::string{"grp:lalt_lshift_toggle"};
    }
    return std::string{"grp:alt_shift_toggle"};
  }

  if ((t.modifier == "leftctrl" || t.modifier == "rightctrl") &&
      (t.key == "leftshift" || t.key == "rightshift")) {
    if (t.modifier == "rightctrl" && t.key == "rightshift") {
      return std::string{"grp:rctrl_rshift_toggle"};
    }
    if (t.modifier == "leftctrl" && t.key == "leftshift") {
      return std::string{"grp:lctrl_lshift_toggle"};
    }
    return std::string{"grp:ctrl_shift_toggle"};
  }

  if ((t.modifier == "leftctrl" || t.modifier == "rightctrl") &&
      (t.key == "leftalt" || t.key == "rightalt")) {
    return std::string{"grp:ctrl_alt_toggle"};
  }

  if ((t.modifier == "leftalt" || t.modifier == "rightalt") && t.key == "space") {
    return std::string{"grp:alt_space_toggle"};
  }

  if ((t.modifier == "leftctrl" || t.modifier == "rightctrl") && t.key == "space") {
    return std::string{"grp:ctrl_space_toggle"};
  }

  if ((t.modifier == "leftmeta" || t.modifier == "rightmeta") && t.key == "space") {
    return std::string{"grp:win_space_toggle"};
  }

  if ((t.modifier == "leftshift" || t.modifier == "rightshift") && t.key == "capslock") {
    return std::string{"grp:shift_caps_toggle"};
  }

  out_error = "Не удалось сопоставить выбранную комбинацию с XKB grp:*_toggle опцией";
  return std::nullopt;
}

[[nodiscard]] SystemInputWriteOutcome write_x11(const LayoutToggle& toggle) {
  SystemInputWriteOutcome out;
  out.backend = "x11";

  if (!has_display()) {
    out.result = SystemInputResult::NotAvailable;
    out.error = "DISPLAY is not set";
    return out;
  }

  std::string map_err;
  auto xkb_opt = map_toggle_to_xkb_option(toggle, map_err);
  if (!xkb_opt) {
    out.result = SystemInputResult::Unsupported;
    out.error = std::move(map_err);
    return out;
  }

  // Считываем текущие опции, чтобы не затирать несвязанные настройки.
  const SpawnOutcome q = spawn_sync({"setxkbmap", "-query"});
  if (!q.ok) {
    out.result = SystemInputResult::Error;
    out.error = q.error.empty() ? "setxkbmap -query failed" : q.error;
    if (!q.stderr_str.empty()) {
      out.error += ": ";
      out.error += q.stderr_str;
    }
    return out;
  }

  std::istringstream ss(q.stdout_str);
  std::string line;
  std::string options_csv;

  while (std::getline(ss, line)) {
    const std::string_view sv{line};
    const std::string_view prefix = "options:";
    if (sv.substr(0, prefix.size()) == prefix) {
      options_csv = std::string{sv.substr(prefix.size())};
      break;
    }
  }

  std::vector<std::string> opts = split_csv_to_strings(options_csv);

  // Убираем все grp:* опции и ставим ровно одну grp:*_toggle.
  opts.erase(std::remove_if(opts.begin(), opts.end(),
                            [](const std::string& o) {
                              return o.rfind("grp:", 0) == 0;
                            }),
            opts.end());

  opts.push_back(*xkb_opt);

  std::string new_csv;
  for (size_t i = 0; i < opts.size(); ++i) {
    if (i) new_csv += ",";
    new_csv += opts[i];
  }

  const SpawnOutcome set = spawn_sync({"setxkbmap", "-option", new_csv});
  if (!set.ok) {
    out.result = SystemInputResult::Error;
    out.error = set.error.empty() ? "setxkbmap -option failed" : set.error;
    if (!set.stderr_str.empty()) {
      out.error += ": ";
      out.error += set.stderr_str;
    }
    return out;
  }

  out.result = SystemInputResult::Ok;
  return out;
}

[[nodiscard]] SystemInputWriteOutcome write_gnome(const LayoutToggle& toggle) {
  SystemInputWriteOutcome out;
  out.backend = "gnome";

  std::string build_err;
  auto accel = build_gnome_accel(toggle, build_err);
  if (!accel) {
    out.result = SystemInputResult::Unsupported;
    out.error = std::move(build_err);
    return out;
  }

  GSettings* s = g_settings_new(kGnomeKeybindingsSchema);
  if (!s) {
    out.result = SystemInputResult::Error;
    out.error = "Failed to create GSettings for GNOME";
    return out;
  }

  const std::array<const gchar*, 2> next = {accel->c_str(), nullptr};
  if (!g_settings_set_strv(s, kGnomeKeySwitchNext, next.data())) {
    out.result = SystemInputResult::Error;
    out.error = "Failed to write GNOME keybinding: switch-input-source";
    g_object_unref(s);
    return out;
  }

  // Стараемся синхронизировать backward только если это безопасно.
  const bool modifier_is_shift = (toggle.modifier == "leftshift" || toggle.modifier == "rightshift");
  const bool key_is_shift = (toggle.key == "leftshift" || toggle.key == "rightshift");

  if (!modifier_is_shift && !key_is_shift) {
    std::string prev = "<Shift>";
    prev += *accel;
    const std::array<const gchar*, 2> prev_arr = {prev.c_str(), nullptr};
    (void)g_settings_set_strv(s, kGnomeKeySwitchPrev, prev_arr.data());
  }

  g_object_unref(s);
  out.result = SystemInputResult::Ok;
  return out;
}

} // namespace

SystemInputReadOutcome SystemInputSettings::read_layout_toggle() {
  // Выбор backend без скрытых фолбэков: GNOME используем только когда похоже на GNOME сессию.
  if (gnome_schema_available() && is_gnome_desktop_session()) {
    return read_gnome();
  }

  if (has_display()) {
    return read_x11();
  }

  SystemInputReadOutcome out;
  out.result = SystemInputResult::NotAvailable;
  out.error = "No supported backend detected (GNOME/X11)";
  return out;
}

SystemInputWriteOutcome SystemInputSettings::validate_layout_toggle(const LayoutToggle& toggle) {
  // Валидация без изменения системы.
  if (gnome_schema_available() && is_gnome_desktop_session()) {
    SystemInputWriteOutcome out;
    out.backend = "gnome";

    std::string err;
    auto accel = build_gnome_accel(toggle, err);
    if (!accel) {
      out.result = SystemInputResult::Unsupported;
      out.error = std::move(err);
      return out;
    }

    out.result = SystemInputResult::Ok;
    return out;
  }

  if (has_display()) {
    SystemInputWriteOutcome out;
    out.backend = "x11";

    std::string err;
    auto xkb_opt = map_toggle_to_xkb_option(toggle, err);
    if (!xkb_opt) {
      out.result = SystemInputResult::Unsupported;
      out.error = std::move(err);
      return out;
    }

    out.result = SystemInputResult::Ok;
    return out;
  }

  SystemInputWriteOutcome out;
  out.result = SystemInputResult::NotAvailable;
  out.error = "No supported backend detected (GNOME/X11)";
  return out;
}

SystemInputWriteOutcome SystemInputSettings::write_layout_toggle(const LayoutToggle& toggle) {
  // Для записи также избегаем "магии": предпочитаем GNOME только в GNOME сессии.
  if (gnome_schema_available() && is_gnome_desktop_session()) {
    return write_gnome(toggle);
  }

  if (has_display()) {
    return write_x11(toggle);
  }

  SystemInputWriteOutcome out;
  out.result = SystemInputResult::NotAvailable;
  out.error = "No supported backend detected (GNOME/X11)";
  return out;
}

} // namespace punto
