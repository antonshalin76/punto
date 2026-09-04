#include "punto/clipboard_manager.hpp"

#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <xcb/xcb.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace punto {

struct ClipboardManagerTestAccess {
  static std::unique_ptr<ClipboardManager> make(xcb_connection_t *connection,
                                                int screen_number) {
    return std::unique_ptr<ClipboardManager>{new ClipboardManager{
        connection, screen_number, std::chrono::milliseconds{500}}};
  }
};

} // namespace punto

namespace {

using namespace std::chrono_literals;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error{std::string{message}};
  }
}

xcb_screen_t *screen_at(xcb_connection_t *connection, int index) {
  xcb_screen_iterator_t screen =
      xcb_setup_roots_iterator(xcb_get_setup(connection));
  for (int current = 0; current < index && screen.rem > 0; ++current) {
    xcb_screen_next(&screen);
  }
  return screen.rem > 0 ? screen.data : nullptr;
}

xcb_atom_t intern_atom(xcb_connection_t *connection, std::string_view name) {
  const auto cookie = xcb_intern_atom(
      connection, 0, static_cast<std::uint16_t>(name.size()), name.data());
  xcb_generic_error_t *error = nullptr;
  xcb_intern_atom_reply_t *reply =
      xcb_intern_atom_reply(connection, cookie, &error);
  const xcb_atom_t atom = error == nullptr && reply != nullptr
                              ? reply->atom
                              : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
  std::free(error);
  std::free(reply);
  return atom;
}

void pump_gtk() {
  while (gtk_events_pending() != 0) {
    (void)gtk_main_iteration_do(FALSE);
  }
}

void test_gtk_invisible_requestor_confirms_active_client_receipt() {
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GtkWidget *entry = gtk_entry_new();
  gtk_container_add(GTK_CONTAINER(window), entry);
  gtk_entry_set_text(GTK_ENTRY(entry), "source");
  gtk_widget_show_all(window);
  gtk_widget_grab_focus(entry);
  pump_gtk();

  // This contract drives GTK and XCB on one thread. Do not leave a GTK-owned
  // selection that would require re-entering that same thread while the
  // manager establishes its timestamp-fenced startup baseline.
  gtk_clipboard_clear(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
  gtk_clipboard_clear(gtk_clipboard_get(GDK_SELECTION_PRIMARY));
  pump_gtk();

  GdkWindow *gdk_window = gtk_widget_get_window(window);
  expect(gdk_window != nullptr, "GTK top-level window is realized");
  const xcb_window_t active_window =
      static_cast<xcb_window_t>(gdk_x11_window_get_xid(gdk_window));

  int screen_number = -1;
  xcb_connection_t *connection = xcb_connect(nullptr, &screen_number);
  expect(connection != nullptr && xcb_connection_has_error(connection) == 0,
         "connect ClipboardManager to nested X11");

  xcb_screen_t *screen = screen_at(connection, screen_number);
  expect(screen != nullptr, "resolve nested X11 screen");
  const xcb_atom_t net_active = intern_atom(connection, "_NET_ACTIVE_WINDOW");
  expect(net_active != XCB_ATOM_NONE, "intern _NET_ACTIVE_WINDOW");
  xcb_generic_error_t *property_error = xcb_request_check(
      connection, xcb_change_property_checked(
                      connection, XCB_PROP_MODE_REPLACE, screen->root,
                      net_active, XCB_ATOM_WINDOW, 32, 1, &active_window));
  expect(property_error == nullptr && xcb_flush(connection) > 0,
         "publish active GTK top-level");
  std::free(property_error);

  // The test-only constructor transfers ownership of this connection through
  // BoundedXcbConnection; the manager closes it on reset.
  auto manager =
      punto::ClipboardManagerTestAccess::make(connection, screen_number);
  expect(manager->is_open(), "ClipboardManager opens in nested X11");
  expect(manager->set_text(punto::Selection::Clipboard, "replacement") ==
             punto::ClipboardResult::Ok,
         "ClipboardManager owns replacement payload");
  const auto receipt = manager->arm_paste_receipt(punto::Selection::Clipboard);
  expect(receipt.has_value(), "arm active GTK client receipt");

  gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
  gtk_editable_paste_clipboard(GTK_EDITABLE(entry));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    (void)manager->pump_events();
    pump_gtk();
    if (manager->paste_receipt_seen(*receipt) &&
        std::string_view{gtk_entry_get_text(GTK_ENTRY(entry))} ==
            "replacement") {
      break;
    }
    std::this_thread::sleep_for(1ms);
  }

  expect(std::string_view{gtk_entry_get_text(GTK_ENTRY(entry))} ==
             "replacement",
         "real GTK paste replaces selected editor text");
  expect(manager->paste_receipt_seen(*receipt),
         "GtkInvisible requestor confirms the active GDK client receipt");
  manager.reset();
  gtk_widget_destroy(window);
  pump_gtk();
  GdkDisplay *display = gdk_display_get_default();
  if (display != nullptr) {
    gdk_display_close(display);
  }
}

} // namespace

int main(int argc, char **argv) {
  try {
    if (gtk_init_check(&argc, &argv) == FALSE) {
      std::cerr << "SKIP: GTK could not open nested X11\n";
      return 77;
    }
    test_gtk_invisible_requestor_confirms_active_client_receipt();
    std::cout << "test_clipboard_gtk_contract: OK\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "test_clipboard_gtk_contract: FAIL: " << error.what() << '\n';
    return 1;
  }
}
