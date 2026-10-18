#include "punto/session_resolver.hpp"

#include <gio/gio.h>

#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <string>

namespace {

constexpr char kShellName[] = "org.gnome.Shell";

bool bus_uint(GDBusConnection *connection, const char *method,
              const std::string &owner, guint &value) {
  GError *error = nullptr;
  GVariant *reply = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method, g_variant_new("(s)", owner.c_str()),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr,
      &error);
  if (reply == nullptr) {
    g_clear_error(&error);
    return false;
  }
  g_variant_get(reply, "(u)", &value);
  g_variant_unref(reply);
  return true;
}

} // namespace

int main() {
  GError *error = nullptr;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
  if (connection == nullptr) {
    g_clear_error(&error);
    std::cerr << "probe_stage=session-bus\n";
    return 2;
  }
  GVariant *reply = g_dbus_connection_call_sync(
      connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", kShellName),
      G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 500, nullptr,
      &error);
  if (reply == nullptr) {
    g_clear_error(&error);
    g_object_unref(connection);
    std::cerr << "probe_stage=shell-owner\n";
    return 2;
  }
  const char *native_owner = nullptr;
  g_variant_get(reply, "(&s)", &native_owner);
  const std::string owner = native_owner == nullptr ? "" : native_owner;
  g_variant_unref(reply);
  guint pid = 0;
  guint uid = 0;
  const bool identity_ok =
      !owner.empty() &&
      bus_uint(connection, "GetConnectionUnixProcessID", owner, pid) &&
      bus_uint(connection, "GetConnectionUnixUser", owner, uid);
  g_object_unref(connection);
  if (!identity_ok || pid == 0) {
    std::cerr << "probe_stage=shell-identity\n";
    return 2;
  }
  auto query = punto::make_systemd_session_query();
  punto::SessionResolver resolver(*query);
  const auto selected =
      resolver.resolve(static_cast<uid_t>(uid), static_cast<pid_t>(pid));
  const char *route = selected.route == punto::SessionRoute::direct
                          ? "direct"
                          : "user-inventory";
  std::cout << "shell_uid_matches_helper=" << (uid == getuid() ? "yes" : "no")
            << '\n'
            << "route=" << route << '\n'
            << "result_stage=" << selected.stage << '\n'
            << "selected_session="
            << (selected.session.empty() ? "none" : selected.session) << '\n';
  return uid == getuid() &&
                 selected.status == punto::SessionResolutionStatus::ready
             ? 0
             : 1;
}
