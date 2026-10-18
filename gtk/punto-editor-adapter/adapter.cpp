#include <gdk/gdkx.h>
#include <glib-unix.h>
#include <gmodule.h>
#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

#if PUNTO_WITH_XORG_RECIPIENT
#include "punto/xorg_recipient.hpp"
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaxMessage = 16 * 1024;
constexpr std::size_t kMaxOperations = 128;
constexpr std::int64_t kResultRetentionMs = 30'000;

enum class MutationKind { Delete, Insert };

struct MutationTrace {
  MutationKind expected_kind = MutationKind::Delete;
  std::uint32_t expected_start = 0;
  std::uint32_t expected_end = 0;
  std::string expected_text;
  std::size_t events = 0;
  bool valid = false;
  bool active = false;
};

struct WidgetRecord {
  GtkWidget *widget = nullptr;
  GObject *change_source = nullptr;
  gulong change_handler = 0;
  gulong mark_handler = 0;
  gulong delete_handler = 0;
  gulong insert_handler = 0;
  gulong buffer_handler = 0;
  gulong focus_handler = 0;
  gulong cursor_handler = 0;
  gulong selection_handler = 0;
  std::uint64_t epoch = 0;
  std::uint64_t revision = 1;
  std::uint64_t context_generation = 1;
  std::uint64_t focus_generation = 1;
  std::uint32_t context_start = 0;
  std::uint32_t context_end = 0;
  std::uint32_t context_caret = 0;
  bool context_has_selection = false;
  bool context_initialized = false;
  bool focus_state = false;
  bool focus_initialized = false;
  bool alive = true;
  MutationTrace mutation;
};

struct PreparedRecord {
  std::string editor_instance;
  std::uint64_t widget_epoch = 0;
  std::uint64_t context_generation = 0;
  std::uint64_t focus_generation = 0;
  std::uint64_t revision = 0;
  std::uint32_t window = 0;
  std::uint32_t start = 0;
  std::uint32_t end = 0;
  std::string fragment;
  std::string mode;
  std::string requested_fragment;
  std::string prepare_response;
  std::int64_t not_after_ms = 0;
  bool in_progress = false;
  std::string active_command_identity;
};

struct ApplyCommand {
  std::string editor_instance;
  std::string mode;
  std::uint32_t window = 0;
  std::uint64_t widget_epoch = 0;
  std::uint64_t context_generation = 0;
  std::uint64_t expected_revision = 0;
  std::uint32_t start = 0;
  std::uint32_t end = 0;
  std::string expected_fragment;
  std::string replacement;
  std::int64_t not_after_ms = 0;
  std::string command_identity;
};

struct TerminalRecord {
  ApplyCommand command;
  std::string result;
  std::int64_t retain_until_ms = 0;
  std::string frame_id;
  std::string frame_digest;
  std::uint32_t trigger_index = 0;
  std::string barrier_token;
  std::optional<PreparedRecord> prepared;
};

enum class GateState { Armed, Ready, Decided, Mutating, Expired };

struct TabGate {
  std::string operation_id;
  ApplyCommand command;
  PreparedRecord prepared;
  std::string frame_id;
  std::string frame_digest;
  std::uint32_t trigger_index = 0;
  std::string token;
  std::uint32_t marker_serial = 0;
  guint timeout_source = 0;
  GateState state = GateState::Armed;
  bool marker_entered = false;
  bool prior_can_recurse = false;
};

struct Connection {
  int fd = -1;
  std::string input;
};

int listener_fd = -1;
std::uint64_t next_widget_epoch = 1;
std::string editor_instance;
std::unordered_map<GtkWidget *, std::unique_ptr<WidgetRecord>> widgets;
std::unordered_map<std::string, PreparedRecord> prepared_operations;
std::unordered_map<std::string, TerminalRecord> terminal_results;
std::unordered_map<int, std::unique_ptr<Connection>> connections;
GtkWidget *gate_control = nullptr;
GdkAtom gate_atom = GDK_NONE;
GSource *gdk_event_source = nullptr;
std::unique_ptr<TabGate> tab_gate;
std::uint32_t next_gate_marker_serial = 2;

#if PUNTO_WITH_XORG_RECIPIENT
struct NativeBinding {
  std::int64_t retain_until_ms = 0;
};

punto::XorgRecipient native_recipient;
using NativeIdentity = std::pair<std::uint64_t, std::uint64_t>;
std::map<NativeIdentity, NativeBinding> native_bindings;
constexpr std::size_t kMaxNativeBindings = 128;
constexpr std::int64_t kNativeRegistrationWindowMs = 1000;
constexpr std::int64_t kNativeRetainMs = 10'000;
#endif

std::int64_t monotonic_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             Clock::now().time_since_epoch())
      .count();
}

std::string generate_instance() {
  return std::to_string(::getpid()) + "-" +
         std::to_string(static_cast<std::uint64_t>(g_get_monotonic_time()));
}

std::string json_string(JsonBuilder *builder) {
  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);
  gchar *data = json_generator_to_data(generator, nullptr);
  std::string result = data == nullptr ? std::string{} : std::string{data};
  g_free(data);
  json_node_free(root);
  g_object_unref(generator);
  return result;
}

std::string response(std::string_view operation_id, std::string_view status,
                     std::string_view reason, std::uint64_t revision = 0,
                     std::uint32_t start = 0, std::uint32_t end = 0,
                     std::string_view command_identity = {},
                     std::string_view barrier_token = {}) {
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  const auto add_string = [&](const char *name, std::string_view value) {
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, std::string{value}.c_str());
  };
  const auto add_int = [&](const char *name, std::int64_t value) {
    json_builder_set_member_name(builder, name);
    json_builder_add_int_value(builder, value);
  };
  add_int("protocol", 1);
  add_string("operation_id", operation_id);
  add_string("status", status);
  add_string("reason", reason);
  add_string("command_identity", command_identity);
  add_string("barrier_token", barrier_token);
  add_int("resulting_revision", static_cast<std::int64_t>(revision));
  add_int("resulting_start", start);
  add_int("resulting_end", end);
  json_builder_end_object(builder);
  std::string result = json_string(builder);
  g_object_unref(builder);
  return result;
}

bool required_string(JsonObject *object, const char *name, std::string &value,
                     std::size_t maximum = 4096) {
  JsonNode *node = json_object_get_member(object, name);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) {
    return false;
  }
  const char *raw = json_object_get_string_member(object, name);
  if (raw == nullptr) {
    return false;
  }
  value = raw;
  return value.size() <= maximum && value.find('\0') == std::string::npos;
}

std::optional<std::int64_t> required_int(JsonObject *object, const char *name) {
  JsonNode *node = json_object_get_member(object, name);
  if (node == nullptr || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_INT64) {
    return std::nullopt;
  }
  return json_object_get_int_member(object, name);
}

std::optional<std::uint64_t> required_u64(JsonObject *object,
                                          const char *name) {
  const auto value = required_int(object, name);
  if (!value || *value < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*value);
}

std::optional<std::uint32_t> required_u32(JsonObject *object,
                                          const char *name) {
  const auto value = required_u64(object, name);
  if (!value || *value > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*value);
}

void append_identity_field(std::string &value, std::string_view field) {
  value += std::to_string(field.size());
  value.push_back(':');
  value.append(field);
  value.push_back('|');
}

std::string command_identity(const ApplyCommand &command) {
  std::string canonical;
  canonical.reserve(command.expected_fragment.size() +
                    command.replacement.size() + 256);
  append_identity_field(canonical, command.editor_instance);
  append_identity_field(canonical, command.mode);
  append_identity_field(canonical, std::to_string(command.window));
  append_identity_field(canonical, std::to_string(command.widget_epoch));
  append_identity_field(canonical, std::to_string(command.context_generation));
  append_identity_field(canonical, std::to_string(command.expected_revision));
  append_identity_field(canonical, std::to_string(command.start));
  append_identity_field(canonical, std::to_string(command.end));
  append_identity_field(canonical, command.expected_fragment);
  append_identity_field(canonical, command.replacement);
  append_identity_field(canonical, std::to_string(command.not_after_ms));
  gchar *digest =
      g_compute_checksum_for_string(G_CHECKSUM_SHA256, canonical.data(),
                                    static_cast<gssize>(canonical.size()));
  std::string result = digest == nullptr ? std::string{} : std::string{digest};
  g_free(digest);
  return result;
}

bool same_command(const ApplyCommand &left, const ApplyCommand &right) {
  return left.editor_instance == right.editor_instance &&
         left.mode == right.mode && left.window == right.window &&
         left.widget_epoch == right.widget_epoch &&
         left.context_generation == right.context_generation &&
         left.expected_revision == right.expected_revision &&
         left.start == right.start && left.end == right.end &&
         left.expected_fragment == right.expected_fragment &&
         left.replacement == right.replacement &&
         left.not_after_ms == right.not_after_ms &&
         left.command_identity == right.command_identity;
}

std::int64_t retention_deadline(std::int64_t base) {
  return base > std::numeric_limits<std::int64_t>::max() - kResultRetentionMs
             ? std::numeric_limits<std::int64_t>::max()
             : base + kResultRetentionMs;
}

void observe_mutation(WidgetRecord *record, MutationKind kind, gint start,
                      gint end, const char *text = nullptr,
                      gint text_length = 0) {
  if (!record->alive || !record->mutation.active) {
    return;
  }
  ++record->mutation.events;
  if (start < 0 || end < start || kind != record->mutation.expected_kind ||
      static_cast<std::uint32_t>(start) != record->mutation.expected_start ||
      static_cast<std::uint32_t>(end) != record->mutation.expected_end) {
    record->mutation.valid = false;
    return;
  }
  if (kind == MutationKind::Insert) {
    const gint size = text_length < 0 && text != nullptr
                          ? static_cast<gint>(std::strlen(text))
                          : text_length;
    if (text == nullptr || size < 0 ||
        std::string_view{text, static_cast<std::size_t>(size)} !=
            record->mutation.expected_text) {
      record->mutation.valid = false;
    }
  }
}

void entry_delete_observed(GtkEditable *, gint start, gint end, gpointer data) {
  observe_mutation(static_cast<WidgetRecord *>(data), MutationKind::Delete,
                   start, end);
}

void entry_insert_observed(GtkEditable *, gchar *text, gint length,
                           gint *position, gpointer data) {
  const gint start = position == nullptr ? -1 : *position;
  observe_mutation(static_cast<WidgetRecord *>(data), MutationKind::Insert,
                   start, start, text, length);
}

void buffer_delete_observed(GtkTextBuffer *, GtkTextIter *start,
                            GtkTextIter *end, gpointer data) {
  observe_mutation(static_cast<WidgetRecord *>(data), MutationKind::Delete,
                   start == nullptr ? -1 : gtk_text_iter_get_offset(start),
                   end == nullptr ? -1 : gtk_text_iter_get_offset(end));
}

void buffer_insert_observed(GtkTextBuffer *, GtkTextIter *location, gchar *text,
                            gint length, gpointer data) {
  const gint start =
      location == nullptr ? -1 : gtk_text_iter_get_offset(location);
  observe_mutation(static_cast<WidgetRecord *>(data), MutationKind::Insert,
                   start, start, text, length);
}

void begin_mutation(WidgetRecord *record, MutationKind kind,
                    std::uint32_t start, std::uint32_t end,
                    std::string_view text = {}) {
  record->mutation =
      MutationTrace{kind, start, end, std::string{text}, 0, true, true};
}

bool finish_mutation(WidgetRecord *record) {
  const bool valid = record->mutation.active && record->mutation.valid &&
                     record->mutation.events == 1;
  record->mutation.active = false;
  return valid;
}

bool text_iter_at(GtkTextBuffer *buffer, const GtkTextIter *iter,
                  std::uint32_t expected_offset) {
  return iter != nullptr && gtk_text_iter_get_buffer(iter) == buffer &&
         gtk_text_iter_get_offset(iter) == static_cast<gint>(expected_offset);
}

std::optional<std::string> fragment(GtkWidget *widget, std::uint32_t start,
                                    std::uint32_t end) {
  if (start > end) {
    return std::nullopt;
  }
  if (GTK_IS_ENTRY(widget)) {
    gchar *text = gtk_editable_get_chars(
        GTK_EDITABLE(widget), static_cast<gint>(start), static_cast<gint>(end));
    if (text == nullptr) {
      return std::nullopt;
    }
    std::string result{text};
    g_free(text);
    return result;
  }
  if (GTK_IS_TEXT_VIEW(widget)) {
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
    GtkTextIter begin;
    GtkTextIter finish;
    gtk_text_buffer_get_iter_at_offset(buffer, &begin,
                                       static_cast<gint>(start));
    gtk_text_buffer_get_iter_at_offset(buffer, &finish, static_cast<gint>(end));
    if (gtk_text_iter_get_offset(&begin) != static_cast<gint>(start) ||
        gtk_text_iter_get_offset(&finish) != static_cast<gint>(end)) {
      return std::nullopt;
    }
    gchar *text = gtk_text_buffer_get_text(buffer, &begin, &finish, TRUE);
    if (text == nullptr) {
      return std::nullopt;
    }
    std::string result{text};
    g_free(text);
    return result;
  }
  return std::nullopt;
}

void changed(GObject *, gpointer data) {
  auto *record = static_cast<WidgetRecord *>(data);
  if (record->alive && record->revision != UINT64_MAX) {
    ++record->revision;
  }
}

std::optional<std::pair<std::uint32_t, std::uint32_t>>
selection_range(GtkWidget *widget);
std::optional<std::uint32_t> caret(GtkWidget *widget);

void observe_context(WidgetRecord *record, bool count_transition = true) {
  const auto selected = selection_range(record->widget);
  const auto position = caret(record->widget);
  if (!position) {
    return;
  }
  const bool has_selection = selected.has_value();
  const std::uint32_t start = has_selection ? selected->first : *position;
  const std::uint32_t end = has_selection ? selected->second : *position;
  const bool changed =
      record->context_initialized &&
      (record->context_has_selection != has_selection ||
       record->context_start != start || record->context_end != end ||
       record->context_caret != *position);
  record->context_has_selection = has_selection;
  record->context_start = start;
  record->context_end = end;
  record->context_caret = *position;
  record->context_initialized = true;
  if (count_transition && changed && record->context_generation != UINT64_MAX) {
    ++record->context_generation;
  }
}

void context_changed(GObject *, gpointer data) {
  auto *record = static_cast<WidgetRecord *>(data);
  if (record->alive) {
    observe_context(record);
  }
}

void context_property_changed(GObject *, GParamSpec *, gpointer data) {
  context_changed(nullptr, data);
}

void focus_changed(GObject *, GParamSpec *, gpointer data) {
  auto *record = static_cast<WidgetRecord *>(data);
  if (!record->alive) {
    return;
  }
  GtkWidget *toplevel = gtk_widget_get_toplevel(record->widget);
  const bool focused =
      GTK_IS_WINDOW(toplevel) && gtk_window_is_active(GTK_WINDOW(toplevel)) &&
      gtk_window_get_focus(GTK_WINDOW(toplevel)) == record->widget &&
      gtk_widget_get_window(toplevel) != nullptr &&
      (gdk_window_get_state(gtk_widget_get_window(toplevel)) &
       GDK_WINDOW_STATE_FOCUSED) != 0;
  const bool changed =
      record->focus_initialized && record->focus_state != focused;
  record->focus_state = focused;
  record->focus_initialized = true;
  if (changed && record->focus_generation != UINT64_MAX) {
    ++record->focus_generation;
  }
}

void text_buffer_mark_changed(GtkTextBuffer *, GtkTextIter *, GtkTextMark *mark,
                              gpointer data) {
  const char *name = gtk_text_mark_get_name(mark);
  if (name != nullptr && (std::strcmp(name, "insert") == 0 ||
                          std::strcmp(name, "selection_bound") == 0)) {
    context_changed(nullptr, data);
  }
}

void bind_change_source(WidgetRecord *record, GObject *source) {
  g_object_ref(source);
  if (record->change_source != nullptr && record->change_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->change_handler);
  }
  if (record->change_source != nullptr && record->mark_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->mark_handler);
    record->mark_handler = 0;
  }
  if (record->change_source != nullptr && record->delete_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->delete_handler);
    record->delete_handler = 0;
  }
  if (record->change_source != nullptr && record->insert_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->insert_handler);
    record->insert_handler = 0;
  }
  if (record->change_source != nullptr) {
    g_object_unref(record->change_source);
  }
  record->change_source = source;
  record->change_handler = g_signal_connect(record->change_source, "changed",
                                            G_CALLBACK(changed), record);
  if (GTK_IS_TEXT_VIEW(record->widget)) {
    record->mark_handler =
        g_signal_connect(record->change_source, "mark-set",
                         G_CALLBACK(text_buffer_mark_changed), record);
    record->delete_handler =
        g_signal_connect(record->change_source, "delete-range",
                         G_CALLBACK(buffer_delete_observed), record);
    record->insert_handler =
        g_signal_connect(record->change_source, "insert-text",
                         G_CALLBACK(buffer_insert_observed), record);
  } else {
    record->delete_handler =
        g_signal_connect(record->change_source, "delete-text",
                         G_CALLBACK(entry_delete_observed), record);
    record->insert_handler =
        g_signal_connect(record->change_source, "insert-text",
                         G_CALLBACK(entry_insert_observed), record);
  }
}

void text_view_buffer_changed(GObject *object, GParamSpec *, gpointer data) {
  auto *record = static_cast<WidgetRecord *>(data);
  if (!record->alive || !GTK_IS_TEXT_VIEW(object)) {
    return;
  }
  bind_change_source(record,
                     G_OBJECT(gtk_text_view_get_buffer(GTK_TEXT_VIEW(object))));
  record->epoch = next_widget_epoch++;
  record->revision = 1;
  if (record->context_generation != UINT64_MAX) {
    ++record->context_generation;
  }
  observe_context(record, false);
}

void destroyed(GtkWidget *widget, gpointer data) {
  auto *record = static_cast<WidgetRecord *>(data);
  record->alive = false;
  if (record->change_source != nullptr && record->change_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->change_handler);
    record->change_handler = 0;
  }
  if (record->change_source != nullptr && record->mark_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->mark_handler);
    record->mark_handler = 0;
  }
  if (record->change_source != nullptr && record->delete_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->delete_handler);
    record->delete_handler = 0;
  }
  if (record->change_source != nullptr && record->insert_handler != 0) {
    g_signal_handler_disconnect(record->change_source, record->insert_handler);
    record->insert_handler = 0;
  }
  if (record->change_source != nullptr) {
    g_object_unref(record->change_source);
    record->change_source = nullptr;
  }
  if (record->buffer_handler != 0) {
    g_signal_handler_disconnect(widget, record->buffer_handler);
    record->buffer_handler = 0;
  }
  if (record->focus_handler != 0) {
    g_signal_handler_disconnect(widget, record->focus_handler);
  }
  if (record->cursor_handler != 0) {
    g_signal_handler_disconnect(widget, record->cursor_handler);
  }
  if (record->selection_handler != 0) {
    g_signal_handler_disconnect(widget, record->selection_handler);
  }
  widgets.erase(widget);
}

WidgetRecord *record_for(GtkWidget *widget) {
  const auto existing = widgets.find(widget);
  if (existing != widgets.end()) {
    return existing->second.get();
  }
  auto record = std::make_unique<WidgetRecord>();
  record->widget = widget;
  record->epoch = next_widget_epoch++;
  record->focus_handler = g_signal_connect(
      widget, "notify::has-focus", G_CALLBACK(focus_changed), record.get());
  if (GTK_IS_ENTRY(widget)) {
    bind_change_source(record.get(), G_OBJECT(widget));
    record->cursor_handler =
        g_signal_connect(widget, "notify::cursor-position",
                         G_CALLBACK(context_property_changed), record.get());
    record->selection_handler =
        g_signal_connect(widget, "notify::selection-bound",
                         G_CALLBACK(context_property_changed), record.get());
  } else if (GTK_IS_TEXT_VIEW(widget)) {
    bind_change_source(record.get(), G_OBJECT(gtk_text_view_get_buffer(
                                         GTK_TEXT_VIEW(widget))));
    record->buffer_handler =
        g_signal_connect(widget, "notify::buffer",
                         G_CALLBACK(text_view_buffer_changed), record.get());
  } else {
    return nullptr;
  }
  observe_context(record.get(), false);
  focus_changed(nullptr, nullptr, record.get());
  WidgetRecord *raw = record.get();
  g_signal_connect(widget, "destroy", G_CALLBACK(destroyed), raw);
  widgets.emplace(widget, std::move(record));
  return raw;
}

std::optional<std::pair<GtkWidget *, std::uint32_t>>
focused_widget(std::uint32_t expected_window) {
  GList *toplevels = gtk_window_list_toplevels();
  std::optional<std::pair<GtkWidget *, std::uint32_t>> found;
  for (GList *item = toplevels; item != nullptr; item = item->next) {
    GtkWidget *toplevel = GTK_WIDGET(item->data);
    if (!GTK_IS_WINDOW(toplevel) || !gtk_widget_get_visible(toplevel) ||
        !gtk_widget_get_sensitive(toplevel)) {
      continue;
    }
    GdkWindow *window = gtk_widget_get_window(toplevel);
    if (window == nullptr || !GDK_IS_X11_WINDOW(window)) {
      continue;
    }
    const auto xid = static_cast<std::uint32_t>(gdk_x11_window_get_xid(window));
    if (xid != expected_window) {
      continue;
    }
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(toplevel));
    if (focus == nullptr || !gtk_widget_has_focus(focus) ||
        !gtk_widget_get_visible(focus) || !gtk_widget_get_sensitive(focus)) {
      continue;
    }
    const bool entry_ok = GTK_IS_ENTRY(focus) &&
                          gtk_entry_get_visibility(GTK_ENTRY(focus)) &&
                          gtk_editable_get_editable(GTK_EDITABLE(focus));
    const bool text_view_ok = GTK_IS_TEXT_VIEW(focus) &&
                              gtk_text_view_get_editable(GTK_TEXT_VIEW(focus));
    if (!entry_ok && !text_view_ok) {
      continue;
    }
    if (found) {
      g_list_free(toplevels);
      return std::nullopt;
    }
    found = std::pair{focus, xid};
  }
  g_list_free(toplevels);
  return found;
}

#if PUNTO_WITH_XORG_RECIPIENT
void prune_native_bindings() {
  const auto now = monotonic_ms();
  for (auto it = native_bindings.begin(); it != native_bindings.end();) {
    if (now > it->second.retain_until_ms) {
      it = native_bindings.erase(it);
    } else {
      ++it;
    }
  }
}

GdkFilterReturn native_recipient_filter(GdkXEvent *raw, GdkEvent *, gpointer) {
  GdkDisplay *display = gdk_display_get_default();
  if (raw == nullptr || !native_recipient.ready() || display == nullptr ||
      !gdk_x11_display_bounded_progress_enabled_v3(display)) {
    return GDK_FILTER_CONTINUE;
  }
  const auto *event = static_cast<const XEvent *>(raw);
  if (event->type != native_recipient.event_type()) {
    return GDK_FILTER_CONTINUE;
  }
  const auto notification = native_recipient.notification(*event);
  if (!notification) {
    return GDK_FILTER_REMOVE;
  }
  prune_native_bindings();
  const NativeIdentity identity{notification->server_nonce,
                                notification->occurrence};
  if (native_bindings.contains(identity) ||
      native_bindings.size() >= kMaxNativeBindings) {
    return GDK_FILTER_REMOVE;
  }
  // A GDK filter precedes the widget handler and IM commit. Until a native
  // dispatch-boundary receipt exists, retain the occurrence but never claim it.
  native_bindings.emplace(identity,
                          NativeBinding{monotonic_ms() + kNativeRetainMs});
  return GDK_FILTER_REMOVE;
}

gboolean initialize_native_recipient(gpointer) {
  static const auto started_at = punto::XorgRecipient::Clock::now();
  const auto now = punto::XorgRecipient::Clock::now();
  GdkDisplay *display = gdk_display_get_default();
  if (display == nullptr || !GDK_IS_X11_DISPLAY(display)) {
    return now - started_at <
                   std::chrono::milliseconds{kNativeRegistrationWindowMs}
               ? G_SOURCE_CONTINUE
               : G_SOURCE_REMOVE;
  }
  if (!gdk_x11_display_bounded_progress_enabled_v3(display) &&
      !gdk_x11_display_enable_bounded_progress_v3(display)) {
    return now - started_at <
                   std::chrono::milliseconds{kNativeRegistrationWindowMs}
               ? G_SOURCE_CONTINUE
               : G_SOURCE_REMOVE;
  }
  if (native_recipient.state() == punto::XorgRecipient::State::Unavailable &&
      now - started_at <
          std::chrono::milliseconds{kNativeRegistrationWindowMs}) {
    (void)native_recipient.start_registration(
        gdk_x11_display_get_xdisplay(display), PUNTO_POLICY_HASH,
        PUNTO_BUILD_ID,
        started_at + std::chrono::milliseconds{kNativeRegistrationWindowMs});
  }
  const auto state = native_recipient.poll(now);
  if (state == punto::XorgRecipient::State::Ready) {
    gdk_window_add_filter(nullptr, native_recipient_filter, nullptr);
    return G_SOURCE_REMOVE;
  }
  if (state == punto::XorgRecipient::State::QueryPending ||
      state == punto::XorgRecipient::State::RegisterPending ||
      (state == punto::XorgRecipient::State::Expired &&
       now - started_at < std::chrono::milliseconds{kNativeRetainMs}))
    return G_SOURCE_CONTINUE;
  return G_SOURCE_REMOVE;
}
#endif

std::optional<std::pair<std::uint32_t, std::uint32_t>>
selection_range(GtkWidget *widget) {
  if (GTK_IS_ENTRY(widget)) {
    gint start = 0;
    gint end = 0;
    if (!gtk_editable_get_selection_bounds(GTK_EDITABLE(widget), &start,
                                           &end) ||
        start < 0 || end <= start) {
      return std::nullopt;
    }
    return std::pair{static_cast<std::uint32_t>(start),
                     static_cast<std::uint32_t>(end)};
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
  GtkTextIter start;
  GtkTextIter end;
  if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end)) {
    return std::nullopt;
  }
  const gint start_offset = gtk_text_iter_get_offset(&start);
  const gint end_offset = gtk_text_iter_get_offset(&end);
  if (start_offset < 0 || end_offset <= start_offset) {
    return std::nullopt;
  }
  return std::pair{static_cast<std::uint32_t>(start_offset),
                   static_cast<std::uint32_t>(end_offset)};
}

std::optional<std::uint32_t> caret(GtkWidget *widget) {
  if (GTK_IS_ENTRY(widget)) {
    const gint position = gtk_editable_get_position(GTK_EDITABLE(widget));
    return position < 0 ? std::optional<std::uint32_t>{}
                        : std::optional<std::uint32_t>{
                              static_cast<std::uint32_t>(position)};
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
  GtkTextIter position;
  gtk_text_buffer_get_iter_at_mark(buffer, &position,
                                   gtk_text_buffer_get_insert(buffer));
  const gint offset = gtk_text_iter_get_offset(&position);
  return offset < 0
             ? std::optional<std::uint32_t>{}
             : std::optional<std::uint32_t>{static_cast<std::uint32_t>(offset)};
}

std::optional<std::uint32_t> text_length(GtkWidget *widget) {
  if (GTK_IS_ENTRY(widget)) {
    const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
    const glong length = g_utf8_strlen(text, -1);
    if (length < 0 || static_cast<std::uint64_t>(length) > UINT32_MAX) {
      return std::nullopt;
    }
    return static_cast<std::uint32_t>(length);
  }
  if (GTK_IS_TEXT_VIEW(widget)) {
    const gint length = gtk_text_buffer_get_char_count(
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget)));
    return length < 0 ? std::optional<std::uint32_t>{}
                      : std::optional<std::uint32_t>{
                            static_cast<std::uint32_t>(length)};
  }
  return std::nullopt;
}

struct EditBoundary {
  std::uint32_t prefix_start = 0;
  std::string prefix;
  std::string suffix;
};

std::optional<EditBoundary>
capture_boundary(GtkWidget *widget, std::uint32_t start, std::uint32_t end) {
  constexpr std::uint32_t kContextCharacters = 64;
  const auto length = text_length(widget);
  if (!length || start > end || end > *length) {
    return std::nullopt;
  }
  EditBoundary boundary;
  boundary.prefix_start =
      start > kContextCharacters ? start - kContextCharacters : 0;
  const std::uint32_t suffix_end =
      std::min(*length, end + std::min(kContextCharacters, *length - end));
  const auto prefix = fragment(widget, boundary.prefix_start, start);
  const auto suffix = fragment(widget, end, suffix_end);
  if (!prefix || !suffix) {
    return std::nullopt;
  }
  boundary.prefix = *prefix;
  boundary.suffix = *suffix;
  return boundary;
}

std::optional<std::uint32_t> utf8_length(std::string_view value) {
  const glong length =
      g_utf8_strlen(value.data(), static_cast<gssize>(value.size()));
  if (length < 0 || static_cast<std::uint64_t>(length) > UINT32_MAX) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(length);
}

bool boundary_matches(GtkWidget *widget, const EditBoundary &boundary,
                      std::string_view middle) {
  const auto prefix_length = utf8_length(boundary.prefix);
  const auto middle_length = utf8_length(middle);
  const auto suffix_length = utf8_length(boundary.suffix);
  if (!prefix_length || !middle_length || !suffix_length) {
    return false;
  }
  const std::uint64_t end = static_cast<std::uint64_t>(boundary.prefix_start) +
                            *prefix_length + *middle_length + *suffix_length;
  if (end > UINT32_MAX) {
    return false;
  }
  const auto actual =
      fragment(widget, boundary.prefix_start, static_cast<std::uint32_t>(end));
  return actual &&
         *actual == boundary.prefix + std::string{middle} + boundary.suffix;
}

bool exact_user_context(GtkWidget *widget, const PreparedRecord &prepared) {
  const auto selected = selection_range(widget);
  if (prepared.mode == "selection") {
    return selected && selected->first == prepared.start &&
           selected->second == prepared.end;
  }
  const auto position = caret(widget);
  return !selected && position && *position == prepared.end;
}

void prune_expired_operations() {
  const auto now = monotonic_ms();
  for (auto item = prepared_operations.begin();
       item != prepared_operations.end();) {
    if (!item->second.in_progress &&
        retention_deadline(item->second.not_after_ms) < now) {
      item = prepared_operations.erase(item);
    } else {
      ++item;
    }
  }
  for (auto item = terminal_results.begin(); item != terminal_results.end();) {
    if (item->second.retain_until_ms < now) {
      item = terminal_results.erase(item);
    } else {
      ++item;
    }
  }
}

std::string prepare(JsonObject *request, const std::string &operation_id) {
  std::string mode;
  std::string expected;
  const auto window = required_int(request, "window");
  const auto deadline = required_int(request, "not_after_ms");
  if (!required_string(request, "mode", mode, 16) ||
      !required_string(request, "expected_fragment", expected) || !window ||
      !deadline || mode.empty() || *window <= 0 ||
      *window > UINT32_MAX || (mode != "word" && mode != "selection")) {
    return response(operation_id, "REJECTED", "invalid_prepare");
  }
  const auto same_request = [&](const PreparedRecord &bound) {
    return bound.mode == mode && bound.requested_fragment == expected &&
           bound.window == static_cast<std::uint32_t>(*window) &&
           bound.not_after_ms == *deadline;
  };
  if (const auto prior = prepared_operations.find(operation_id);
      prior != prepared_operations.end()) {
    return same_request(prior->second)
               ? prior->second.prepare_response
               : response(operation_id, "REJECTED", "operation_conflict");
  }
  if (const auto terminal = terminal_results.find(operation_id);
      terminal != terminal_results.end()) {
    return terminal->second.prepared && same_request(*terminal->second.prepared)
               ? terminal->second.prepared->prepare_response
               : response(operation_id, "REJECTED", "operation_conflict");
  }
  if (monotonic_ms() > *deadline) {
    return response(operation_id, "REJECTED", "operation_expired");
  }
  const auto focused = focused_widget(static_cast<std::uint32_t>(*window));
  if (!focused) {
    return response(operation_id, "REJECTED", "unsupported_or_unfocused");
  }
  GtkWidget *widget = focused->first;
  WidgetRecord *record = record_for(widget);
  if (record == nullptr || !record->alive || record->epoch == 0 ||
      record->epoch > static_cast<std::uint64_t>(INT64_MAX) ||
      record->context_generation == 0 ||
      record->context_generation > static_cast<std::uint64_t>(INT64_MAX) ||
      record->focus_generation == 0 || record->focus_generation == UINT64_MAX ||
      record->revision == 0 ||
      record->revision > static_cast<std::uint64_t>(INT64_MAX) - 2) {
    return response(operation_id, "REJECTED", "widget_unavailable");
  }
  std::optional<std::pair<std::uint32_t, std::uint32_t>> range;
  if (mode == "selection") {
    range = selection_range(widget);
  } else {
    const auto position = caret(widget);
    const glong count =
        g_utf8_strlen(expected.c_str(), static_cast<gssize>(expected.size()));
    if (!selection_range(widget) && position && count >= 0 &&
        static_cast<std::uint64_t>(count) <= *position) {
      range =
          std::pair{*position - static_cast<std::uint32_t>(count), *position};
    }
  }
  if (!range) {
    return response(operation_id, "REJECTED", "range_unavailable");
  }
  const auto current = fragment(widget, range->first, range->second);
  if (!current || current->empty() || current->size() > 4096 ||
      (mode == "word" && *current != expected)) {
    return response(operation_id, "REJECTED", "fragment_mismatch");
  }
  prune_expired_operations();
  if (prepared_operations.size() + terminal_results.size() >= kMaxOperations) {
    return response(operation_id, "REJECTED", "operation_capacity");
  }
  PreparedRecord prepared;
  prepared.editor_instance = editor_instance;
  prepared.widget_epoch = record->epoch;
  prepared.context_generation = record->context_generation;
  prepared.focus_generation = record->focus_generation;
  prepared.revision = record->revision;
  prepared.window = focused->second;
  prepared.start = range->first;
  prepared.end = range->second;
  prepared.fragment = *current;
  prepared.mode = mode;
  prepared.requested_fragment = expected;
  prepared.not_after_ms = *deadline;
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_object(builder);
  const auto add_string = [&](const char *name, std::string_view value) {
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, std::string{value}.c_str());
  };
  const auto add_int = [&](const char *name, std::int64_t value) {
    json_builder_set_member_name(builder, name);
    json_builder_add_int_value(builder, value);
  };
  json_builder_set_member_name(builder, "ok");
  json_builder_add_boolean_value(builder, TRUE);
  add_int("protocol", 1);
  add_string("operation_id", operation_id);
  add_string("editor_instance", prepared.editor_instance);
  add_int("widget_epoch", static_cast<std::int64_t>(prepared.widget_epoch));
  add_int("context_generation",
          static_cast<std::int64_t>(prepared.context_generation));
  add_int("revision", static_cast<std::int64_t>(prepared.revision));
  add_int("start", prepared.start);
  add_int("end", prepared.end);
  add_string("fragment", prepared.fragment);
  json_builder_end_object(builder);
  std::string result = json_string(builder);
  g_object_unref(builder);
  prepared.prepare_response = result;
  prepared_operations[operation_id] = std::move(prepared);
  return result;
}

std::optional<ApplyCommand> parse_apply_request(JsonObject *request) {
  ApplyCommand command;
  const auto window = required_u32(request, "window");
  const auto epoch = required_u64(request, "widget_epoch");
  const auto context_generation = required_u64(request, "context_generation");
  const auto revision = required_u64(request, "expected_revision");
  const auto start = required_u32(request, "start");
  const auto end = required_u32(request, "end");
  const auto deadline = required_int(request, "not_after_ms");
  if (!required_string(request, "editor_instance", command.editor_instance,
                       128) ||
      !required_string(request, "mode", command.mode, 16) ||
      !required_string(request, "expected_fragment",
                       command.expected_fragment) ||
      !required_string(request, "replacement", command.replacement) ||
      !required_string(request, "command_identity", command.command_identity,
                       64) ||
      !window || !epoch || !context_generation || !revision || !start || !end ||
      !deadline || command.replacement.empty() || *start > *end ||
      (command.mode != "word" && command.mode != "selection")) {
    return std::nullopt;
  }
  command.window = *window;
  command.widget_epoch = *epoch;
  command.context_generation = *context_generation;
  command.expected_revision = *revision;
  command.start = *start;
  command.end = *end;
  command.not_after_ms = *deadline;
  if (command.command_identity.size() != 64 ||
      command_identity(command) != command.command_identity) {
    return std::nullopt;
  }
  return command;
}

bool matches_prepared(const ApplyCommand &command,
                      const PreparedRecord &prepared) {
  return command.editor_instance == prepared.editor_instance &&
         command.mode == prepared.mode && command.window == prepared.window &&
         command.widget_epoch == prepared.widget_epoch &&
         command.context_generation == prepared.context_generation &&
         command.expected_revision == prepared.revision &&
         command.start == prepared.start && command.end == prepared.end &&
         command.expected_fragment == prepared.fragment &&
         command.not_after_ms == prepared.not_after_ms;
}

bool replace(GtkWidget *widget, const PreparedRecord &prepared,
             const std::string &replacement, std::uint32_t &result_start,
             std::uint32_t &result_end) {
  const auto replacement_count = utf8_length(replacement);
  const auto boundary = capture_boundary(widget, prepared.start, prepared.end);
  const auto original_length = text_length(widget);
  const auto owner = widgets.find(widget);
  if (!replacement_count || !boundary || !original_length ||
      owner == widgets.end() || owner->second->epoch != prepared.widget_epoch ||
      prepared.end > *original_length || prepared.revision > UINT64_MAX - 2 ||
      static_cast<std::uint64_t>(prepared.start) + *replacement_count >
          UINT32_MAX) {
    return false;
  }
  WidgetRecord *const mutation_owner = owner->second.get();
  const std::uint32_t removed_count = prepared.end - prepared.start;
  const std::uint32_t deleted_length = *original_length - removed_count;
  const std::uint64_t final_length =
      static_cast<std::uint64_t>(deleted_length) + *replacement_count;
  if (final_length > UINT32_MAX) {
    return false;
  }
  result_start = prepared.start;
  result_end = prepared.start + *replacement_count;
  const auto same_live_record = [&](std::uint64_t expected_revision) {
    const auto current = widgets.find(widget);
    return current != widgets.end() && current->second->alive &&
           current->second->epoch == prepared.widget_epoch &&
           current->second->focus_generation == prepared.focus_generation &&
           current->second->revision == expected_revision;
  };
  const auto mutation_proved = [&](std::uint64_t expected_revision,
                                   std::uint32_t expected_length) {
    const auto current = widgets.find(widget);
    if (current == widgets.end() || current->second.get() != mutation_owner) {
      return false;
    }
    const bool trace_matches = finish_mutation(mutation_owner);
    return trace_matches && current->second->epoch == prepared.widget_epoch &&
           same_live_record(expected_revision) &&
           text_length(widget) == expected_length;
  };
  const auto final_context_matches = [&] {
    PreparedRecord result_context;
    result_context.start = result_start;
    result_context.end = result_end;
    result_context.mode = prepared.mode;
    return exact_user_context(widget, result_context);
  };
  if (GTK_IS_ENTRY(widget)) {
    gint position = static_cast<gint>(prepared.start);
    begin_mutation(mutation_owner, MutationKind::Delete, prepared.start,
                   prepared.end);
    gtk_editable_delete_text(GTK_EDITABLE(widget),
                             static_cast<gint>(prepared.start),
                             static_cast<gint>(prepared.end));
    if (!mutation_proved(prepared.revision + 1, deleted_length) ||
        !boundary_matches(widget, *boundary, {})) {
      return false;
    }
    begin_mutation(mutation_owner, MutationKind::Insert, prepared.start,
                   prepared.start, replacement);
    gtk_editable_insert_text(GTK_EDITABLE(widget), replacement.data(),
                             static_cast<gint>(replacement.size()), &position);
    if (!mutation_proved(prepared.revision + 2,
                         static_cast<std::uint32_t>(final_length)) ||
        !boundary_matches(widget, *boundary, replacement)) {
      return false;
    }
    if (prepared.mode == "selection") {
      gtk_editable_select_region(GTK_EDITABLE(widget),
                                 static_cast<gint>(result_start),
                                 static_cast<gint>(result_end));
    } else {
      gtk_editable_set_position(GTK_EDITABLE(widget),
                                static_cast<gint>(result_end));
    }
    return same_live_record(prepared.revision + 2) && final_context_matches();
  }
  if (!GTK_IS_TEXT_VIEW(widget)) {
    return false;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(widget));
  g_object_ref(buffer);
  GtkTextIter start;
  GtkTextIter end;
  gtk_text_buffer_get_iter_at_offset(buffer, &start,
                                     static_cast<gint>(prepared.start));
  gtk_text_buffer_get_iter_at_offset(buffer, &end,
                                     static_cast<gint>(prepared.end));
  begin_mutation(mutation_owner, MutationKind::Delete, prepared.start,
                 prepared.end);
  gtk_text_buffer_delete(buffer, &start, &end);
  const bool delete_committed = text_iter_at(buffer, &start, prepared.start) &&
                                text_iter_at(buffer, &end, prepared.start);
  if (!delete_committed ||
      !mutation_proved(prepared.revision + 1, deleted_length) ||
      !boundary_matches(widget, *boundary, {})) {
    g_object_unref(buffer);
    return false;
  }
  gtk_text_buffer_get_iter_at_offset(buffer, &start,
                                     static_cast<gint>(prepared.start));
  begin_mutation(mutation_owner, MutationKind::Insert, prepared.start,
                 prepared.start, replacement);
  gtk_text_buffer_insert(buffer, &start, replacement.data(),
                         static_cast<gint>(replacement.size()));
  const bool insert_committed = text_iter_at(buffer, &start, result_end);
  if (!insert_committed ||
      !mutation_proved(prepared.revision + 2,
                       static_cast<std::uint32_t>(final_length)) ||
      !boundary_matches(widget, *boundary, replacement)) {
    g_object_unref(buffer);
    return false;
  }
  GtkTextIter result_begin;
  GtkTextIter result_finish;
  gtk_text_buffer_get_iter_at_offset(buffer, &result_begin,
                                     static_cast<gint>(result_start));
  gtk_text_buffer_get_iter_at_offset(buffer, &result_finish,
                                     static_cast<gint>(result_end));
  if (prepared.mode == "selection") {
    gtk_text_buffer_select_range(buffer, &result_begin, &result_finish);
  } else {
    gtk_text_buffer_place_cursor(buffer, &result_finish);
  }
  g_object_unref(buffer);
  return same_live_record(prepared.revision + 2) && final_context_matches();
}

void remember_terminal(const std::string &operation_id,
                       const ApplyCommand &command, std::string result) {
  std::optional<PreparedRecord> prepared;
  if (const auto prior = prepared_operations.find(operation_id);
      prior != prepared_operations.end()) {
    prepared = prior->second;
  }
  prepared_operations.erase(operation_id);
  terminal_results[operation_id] = TerminalRecord{
      command, std::move(result),
      retention_deadline(std::max(monotonic_ms(), command.not_after_ms)), {}, {},
      0, {}, std::move(prepared)};
}

std::string compare_apply(JsonObject *request,
                          const std::string &operation_id) {
  prune_expired_operations();
  const auto command = parse_apply_request(request);
  if (!command) {
    return response(operation_id, "REJECTED", "invalid_apply_request");
  }
  const auto terminal = terminal_results.find(operation_id);
  if (terminal != terminal_results.end()) {
    return same_command(*command, terminal->second.command)
               ? terminal->second.result
               : response(operation_id, "REJECTED", "operation_conflict", 0, 0,
                          0, command->command_identity);
  }
  if (tab_gate) {
    return response(operation_id, "REJECTED", "tab_gate_active", 0, 0, 0,
                    command->command_identity);
  }
  const auto prepared_it = prepared_operations.find(operation_id);
  if (prepared_it == prepared_operations.end()) {
    return response(operation_id, "UNKNOWN", "result_unavailable", 0, 0, 0,
                    command->command_identity);
  }
  if (prepared_it->second.in_progress) {
    if (prepared_it->second.active_command_identity !=
        command->command_identity) {
      return response(operation_id, "REJECTED", "operation_conflict", 0, 0,
                      0, command->command_identity);
    }
    return response(operation_id, "UNKNOWN", "operation_in_progress", 0, 0, 0,
                    command->command_identity);
  }
  const PreparedRecord prepared = prepared_it->second;
  if (!matches_prepared(*command, prepared)) {
    return response(operation_id, "REJECTED", "request_mismatch", 0, 0, 0,
                    command->command_identity);
  }
  if (monotonic_ms() > command->not_after_ms) {
    std::string result = response(operation_id, "REJECTED", "operation_expired",
                                  0, 0, 0, command->command_identity);
    remember_terminal(operation_id, *command, result);
    return result;
  }
  prepared_it->second.in_progress = true;
  prepared_it->second.active_command_identity = command->command_identity;
  const auto focused = focused_widget(prepared.window);
  if (!focused) {
    std::string result = response(operation_id, "REJECTED", "context_changed",
                                  0, 0, 0, command->command_identity);
    remember_terminal(operation_id, *command, result);
    return result;
  }
  WidgetRecord *record = record_for(focused->first);
  const auto current = fragment(focused->first, prepared.start, prepared.end);
  if (record == nullptr || !record->alive ||
      record->epoch != prepared.widget_epoch ||
      record->context_generation != prepared.context_generation ||
      record->focus_generation != prepared.focus_generation ||
      record->revision != prepared.revision || !current ||
      *current != prepared.fragment ||
      !exact_user_context(focused->first, prepared)) {
    std::string result = response(operation_id, "REJECTED", "stale_context", 0,
                                  0, 0, command->command_identity);
    remember_terminal(operation_id, *command, result);
    return result;
  }
  GtkWidget *widget = focused->first;
  const std::uint64_t applied_epoch = record->epoch;
  g_object_ref(widget);
  std::uint32_t result_start = 0;
  std::uint32_t result_end = 0;
  const bool replaced =
      replace(widget, prepared, command->replacement, result_start, result_end);
  const auto current_record = widgets.find(widget);
  if (!replaced || current_record == widgets.end() ||
      !current_record->second->alive ||
      current_record->second->epoch != applied_epoch) {
    g_object_unref(widget);
    std::string result = response(operation_id, "UNKNOWN", "apply_failed", 0, 0,
                                  0, command->command_identity);
    remember_terminal(operation_id, *command, result);
    return result;
  }
  record = current_record->second.get();
  const auto resulting = fragment(widget, result_start, result_end);
  const std::uint64_t resulting_revision = record->revision;
  g_object_unref(widget);
  if (!resulting || *resulting != command->replacement ||
      resulting_revision != prepared.revision + 2) {
    std::string result = response(operation_id, "UNKNOWN", "result_unverified",
                                  0, 0, 0, command->command_identity);
    remember_terminal(operation_id, *command, result);
    return result;
  }
  std::string result =
      response(operation_id, "APPLIED", "applied", resulting_revision,
               result_start, result_end, command->command_identity);
  remember_terminal(operation_id, *command, result);
  return result;
}

struct FrameBinding {
  std::string id;
  std::string digest;
  std::uint32_t trigger_index = 0;
  std::string token;
};

std::optional<FrameBinding> parse_frame_binding(JsonObject *request,
                                                bool require_token) {
  FrameBinding frame;
  const auto trigger = required_u32(request, "trigger_index");
  if (!trigger || *trigger > kMaxMessage ||
      !required_string(request, "frame_id", frame.id, 128) ||
      !required_string(request, "frame_digest", frame.digest, 64) ||
      frame.id.empty() || frame.digest.size() != 64 ||
      (require_token &&
       (!required_string(request, "barrier_token", frame.token, 128) ||
        frame.token.empty()))) {
    return std::nullopt;
  }
  if (!std::all_of(frame.digest.begin(), frame.digest.end(), [](char byte) {
        return (byte >= '0' && byte <= '9') ||
               (byte >= 'a' && byte <= 'f');
      })) {
    return std::nullopt;
  }
  frame.trigger_index = *trigger;
  return frame;
}

bool frame_matches(const FrameBinding &frame, const TabGate &gate) {
  return frame.id == gate.frame_id && frame.digest == gate.frame_digest &&
         frame.trigger_index == gate.trigger_index &&
         frame.token == gate.token;
}

bool frame_matches(const FrameBinding &frame, const TerminalRecord &record) {
  return frame.id == record.frame_id &&
         frame.digest == record.frame_digest &&
         frame.trigger_index == record.trigger_index &&
         frame.token == record.barrier_token;
}

std::string new_gate_token() {
  std::array<unsigned char, 16> bytes{};
  if (::getrandom(bytes.data(), bytes.size(), GRND_NONBLOCK) !=
      static_cast<ssize_t>(bytes.size())) {
    return {};
  }
  constexpr char hex[] = "0123456789abcdef";
  std::string token;
  token.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    token.push_back(hex[byte >> 4U]);
    token.push_back(hex[byte & 0x0fU]);
  }
  return token;
}

void remember_gate_terminal(const TabGate &gate, std::string result) {
  remember_terminal(gate.operation_id, gate.command, std::move(result));
  auto &terminal = terminal_results.at(gate.operation_id);
  terminal.frame_id = gate.frame_id;
  terminal.frame_digest = gate.frame_digest;
  terminal.trigger_index = gate.trigger_index;
  terminal.barrier_token = gate.token;
}

void finish_gate(std::string result) {
  if (!tab_gate) {
    return;
  }
  if (tab_gate->timeout_source != 0) {
    g_source_remove(tab_gate->timeout_source);
    tab_gate->timeout_source = 0;
  }
  // Only finish_gate can certify that the marker's own write has ended.
  JsonParser *parser = json_parser_new();
  if (json_parser_load_from_data(parser, result.data(),
                                 static_cast<gssize>(result.size()), nullptr)) {
    JsonNode *root = json_parser_get_root(parser);
    if (root != nullptr && JSON_NODE_HOLDS_OBJECT(root)) {
      json_object_set_string_member(json_node_get_object(root), "lifecycle",
                                    "QUIESCENT");
      JsonGenerator *generator = json_generator_new();
      json_generator_set_root(generator, root);
      gchar *encoded = json_generator_to_data(generator, nullptr);
      if (encoded != nullptr) {
        result = encoded;
        g_free(encoded);
      }
      g_object_unref(generator);
    }
  }
  g_object_unref(parser);
  remember_gate_terminal(*tab_gate, std::move(result));
  if (gdk_event_source != nullptr &&
      !g_source_is_destroyed(gdk_event_source)) {
    g_source_set_can_recurse(gdk_event_source,
                             tab_gate->prior_can_recurse);
  }
  tab_gate.reset();
}

gboolean gate_timeout(gpointer) {
  if (!tab_gate) {
    return G_SOURCE_REMOVE;
  }
  tab_gate->timeout_source = 0;
  if (tab_gate->state == GateState::Mutating ||
      tab_gate->state == GateState::Decided) {
    return G_SOURCE_REMOVE;
  }
  tab_gate->state = GateState::Expired;
  if (!tab_gate->marker_entered) {
    finish_gate(response(tab_gate->operation_id, "REJECTED", "gate_expired",
                         0, 0, 0, tab_gate->command.command_identity));
  }
  return G_SOURCE_REMOVE;
}

void put_gate_marker(std::uint32_t serial) {
  GdkWindow *window = gate_control == nullptr
                          ? nullptr
                          : gtk_widget_get_window(gate_control);
  if (window == nullptr) {
    return;
  }
  GdkEvent *event = gdk_event_new(GDK_PROPERTY_NOTIFY);
  event->property.window = GDK_WINDOW(g_object_ref(window));
  event->property.atom = gate_atom;
  event->property.time = serial;
  event->property.state = GDK_PROPERTY_NEW_VALUE;
  gdk_event_put(event);
  gdk_event_free(event);
}

bool gate_context_matches(const TabGate &gate) {
  const auto focused = focused_widget(gate.prepared.window);
  if (!focused) {
    return false;
  }
  WidgetRecord *record = record_for(focused->first);
  const auto current = fragment(focused->first, gate.prepared.start,
                                gate.prepared.end);
  return record != nullptr && record->alive &&
         record->epoch == gate.prepared.widget_epoch &&
         record->context_generation == gate.prepared.context_generation &&
         record->focus_generation == gate.prepared.focus_generation &&
         record->revision == gate.prepared.revision && current &&
         *current == gate.prepared.fragment &&
         exact_user_context(focused->first, gate.prepared);
}

std::string apply_in_gate(const TabGate &gate) {
  const auto focused = focused_widget(gate.prepared.window);
  if (!focused) {
    return response(gate.operation_id, "REJECTED", "context_changed", 0, 0,
                    0, gate.command.command_identity);
  }
  GtkWidget *widget = focused->first;
  WidgetRecord *record = record_for(widget);
  const auto current = fragment(widget, gate.prepared.start,
                                gate.prepared.end);
  if (record == nullptr || !record->alive ||
      record->epoch != gate.prepared.widget_epoch ||
      record->context_generation != gate.prepared.context_generation ||
      record->focus_generation != gate.prepared.focus_generation ||
      record->revision != gate.prepared.revision || !current ||
      *current != gate.prepared.fragment ||
      !exact_user_context(widget, gate.prepared)) {
    return response(gate.operation_id, "REJECTED", "stale_context", 0, 0, 0,
                    gate.command.command_identity);
  }
  const std::uint64_t epoch = record->epoch;
  g_object_ref(widget);
  std::uint32_t start = 0;
  std::uint32_t end = 0;
  const bool replaced =
      replace(widget, gate.prepared, gate.command.replacement, start, end);
  const auto current_record = widgets.find(widget);
  if (!replaced || current_record == widgets.end() ||
      !current_record->second->alive || current_record->second->epoch != epoch) {
    g_object_unref(widget);
    return response(gate.operation_id, "UNKNOWN", "apply_failed", 0, 0, 0,
                    gate.command.command_identity);
  }
  record = current_record->second.get();
  const auto resulting = fragment(widget, start, end);
  const std::uint64_t revision = record->revision;
  g_object_unref(widget);
  if (!resulting || *resulting != gate.command.replacement ||
      revision != gate.prepared.revision + 2) {
    return response(gate.operation_id, "UNKNOWN", "result_unverified", 0, 0,
                    0, gate.command.command_identity);
  }
  return response(gate.operation_id, "APPLIED", "applied", revision, start,
                  end, gate.command.command_identity);
}

gboolean gate_control_event(GtkWidget *, GdkEvent *event, gpointer) {
  if (event->type != GDK_PROPERTY_NOTIFY ||
      event->property.atom != gate_atom ||
      event->property.window != gtk_widget_get_window(gate_control)) {
    return FALSE;
  }
  GSource *const current = g_main_current_source();
  const std::uint32_t serial = event->property.time;
  if (serial == 1 && gdk_event_source == nullptr && current != nullptr &&
      g_main_depth() == 1) {
    const char *name = g_source_get_name(current);
    if (name != nullptr &&
        g_str_has_prefix(name, "GDK X11 Event source")) {
      gdk_event_source = g_source_ref(current);
    }
    return TRUE;
  }
  if (!tab_gate || serial != tab_gate->marker_serial) {
    return TRUE;
  }
  if (current != gdk_event_source ||
      g_source_get_can_recurse(gdk_event_source) ||
      monotonic_ms() > tab_gate->command.not_after_ms ||
      !gate_context_matches(*tab_gate)) {
    finish_gate(response(tab_gate->operation_id, "REJECTED", "gate_source",
                         0, 0, 0, tab_gate->command.command_identity));
    return TRUE;
  }
  tab_gate->marker_entered = true;
  tab_gate->state = GateState::Ready;
  while (tab_gate && tab_gate->marker_serial == serial &&
         tab_gate->state == GateState::Ready) {
    g_main_context_iteration(nullptr, TRUE);
  }
  if (!tab_gate || tab_gate->marker_serial != serial) {
    return TRUE;
  }
  if (tab_gate->state != GateState::Decided) {
    finish_gate(response(tab_gate->operation_id, "REJECTED", "gate_expired",
                         0, 0, 0, tab_gate->command.command_identity));
    return TRUE;
  }
  tab_gate->state = GateState::Mutating;
  const std::string result = apply_in_gate(*tab_gate);
  finish_gate(result);
  return TRUE;
}

gboolean initialize_gate_control(gpointer) {
  if (gate_control != nullptr) {
    return G_SOURCE_REMOVE;
  }
  if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) {
    return G_SOURCE_REMOVE;
  }
  gate_control = gtk_invisible_new();
  gate_atom = gdk_atom_intern_static_string("_PUNTO_EDITOR_GATE_MARKER");
  g_signal_connect(gate_control, "event", G_CALLBACK(gate_control_event),
                   nullptr);
  gtk_widget_realize(gate_control);
  if (gtk_widget_get_window(gate_control) != nullptr) {
    put_gate_marker(1);
  }
  return G_SOURCE_REMOVE;
}

std::string arm_tab(JsonObject *request, const std::string &operation_id) {
  const auto command = parse_apply_request(request);
  const auto frame = parse_frame_binding(request, false);
  if (!command || !frame || command->mode != "word") {
    return response(operation_id, "REJECTED", "invalid_tab_arm");
  }
  if (tab_gate || terminal_results.contains(operation_id)) {
    return response(operation_id, "REJECTED", "gate_or_operation_active", 0,
                    0, 0, command->command_identity);
  }
  const auto prepared_it = prepared_operations.find(operation_id);
  if (prepared_it == prepared_operations.end() ||
      prepared_it->second.in_progress ||
      !matches_prepared(*command, prepared_it->second)) {
    return response(operation_id, "REJECTED", "unprepared_or_conflicting",
                    0, 0, 0, command->command_identity);
  }
  if (monotonic_ms() >= command->not_after_ms || g_main_depth() != 1 ||
      g_main_current_source() == gdk_event_source ||
      gdk_event_source == nullptr ||
      g_source_is_destroyed(gdk_event_source) || gate_control == nullptr ||
      gtk_widget_get_window(gate_control) == nullptr ||
      next_gate_marker_serial == UINT32_MAX) {
    return response(operation_id, "REJECTED", "gate_unavailable", 0, 0, 0,
                    command->command_identity);
  }
  const auto focused = focused_widget(prepared_it->second.window);
  if (!focused || !GTK_IS_WIDGET(focused->first) ||
      gtk_widget_get_window(focused->first) == nullptr ||
      gdk_window_get_display(gtk_widget_get_window(focused->first)) !=
          gdk_window_get_display(gtk_widget_get_window(gate_control))) {
    return response(operation_id, "REJECTED", "gate_display_or_focus", 0,
                    0, 0, command->command_identity);
  }
  const std::string token = new_gate_token();
  if (token.empty()) {
    return response(operation_id, "REJECTED", "gate_token_unavailable", 0,
                    0, 0, command->command_identity);
  }
  auto gate = std::make_unique<TabGate>();
  gate->operation_id = operation_id;
  gate->command = *command;
  gate->prepared = prepared_it->second;
  gate->frame_id = frame->id;
  gate->frame_digest = frame->digest;
  gate->trigger_index = frame->trigger_index;
  gate->token = token;
  gate->marker_serial = next_gate_marker_serial++;
  gate->prior_can_recurse = g_source_get_can_recurse(gdk_event_source);
  prepared_it->second.in_progress = true;
  g_source_set_can_recurse(gdk_event_source, FALSE);
  tab_gate = std::move(gate);
  const auto remaining = std::max<std::int64_t>(
      1, command->not_after_ms - monotonic_ms() + 1);
  tab_gate->timeout_source = g_timeout_add(
      static_cast<guint>(std::min<std::int64_t>(remaining, G_MAXUINT)),
      gate_timeout, nullptr);
  put_gate_marker(tab_gate->marker_serial);
  return response(operation_id, "ARMED", "marker_queued", 0, 0, 0,
                  command->command_identity, token);
}

std::string tab_status_or_decide(JsonObject *request,
                                 const std::string &operation_id,
                                 bool decide) {
  const auto command = parse_apply_request(request);
  const auto frame = parse_frame_binding(request, true);
  if (!command || !frame || command->mode != "word") {
    return response(operation_id, "REJECTED", "invalid_gate_request");
  }
  if (tab_gate && tab_gate->operation_id == operation_id) {
    if (!same_command(*command, tab_gate->command) ||
        !frame_matches(*frame, *tab_gate)) {
      return response(operation_id, "REJECTED", "gate_conflict", 0, 0, 0,
                      command->command_identity);
    }
    if (decide) {
      if (tab_gate->state != GateState::Ready ||
          monotonic_ms() > command->not_after_ms) {
        return response(operation_id, "REJECTED", "gate_not_ready", 0, 0, 0,
                        command->command_identity);
      }
      tab_gate->state = GateState::Decided;
      return response(operation_id, "DECIDED", "decision_recorded", 0, 0, 0,
                      command->command_identity);
    }
    const char *status = tab_gate->state == GateState::Armed    ? "ARMED"
                         : tab_gate->state == GateState::Ready ? "READY"
                                                               : "ACTIVE";
    return response(operation_id, status, "gate_active", 0, 0, 0,
                    command->command_identity);
  }
  const auto terminal = terminal_results.find(operation_id);
  if (terminal != terminal_results.end() && !decide &&
      same_command(*command, terminal->second.command) &&
      frame_matches(*frame, terminal->second)) {
    return terminal->second.result;
  }
  return response(operation_id, "REJECTED", "gate_unavailable", 0, 0, 0,
                  command->command_identity);
}

std::string handle_request(const std::string &input) {
  JsonParser *parser = json_parser_new();
  GError *error = nullptr;
  if (!json_parser_load_from_data(parser, input.data(),
                                  static_cast<gssize>(input.size()), &error)) {
    g_clear_error(&error);
    g_object_unref(parser);
    return response("", "REJECTED", "invalid_json");
  }
  JsonNode *root = json_parser_get_root(parser);
  if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_object_unref(parser);
    return response("", "REJECTED", "invalid_request");
  }
  JsonObject *object = json_node_get_object(root);
  std::string operation_id;
  std::string action;
  const auto protocol = required_int(object, "protocol");
  if (!protocol || *protocol != 1 ||
      !required_string(object, "operation_id", operation_id, 128) ||
      operation_id.empty() || !required_string(object, "action", action, 32)) {
    g_object_unref(parser);
    return response(operation_id, "REJECTED", "invalid_request");
  }
  std::string result;
  if (action == "prepare") {
    prune_expired_operations();
    result = prepare(object, operation_id);
  } else if (action == "compare_apply") {
    result = compare_apply(object, operation_id);
  } else if (action == "arm_tab") {
    result = arm_tab(object, operation_id);
  } else if (action == "tab_status") {
    result = tab_status_or_decide(object, operation_id, false);
  } else if (action == "decide_tab") {
    result = tab_status_or_decide(object, operation_id, true);
  } else {
    result = response(operation_id, "REJECTED", "unknown_action");
  }
  g_object_unref(parser);
  return result;
}

void close_connection(int fd) {
  const auto connection = connections.find(fd);
  if (connection != connections.end()) {
    (void)::close(connection->second->fd);
    connections.erase(connection);
  }
}

gboolean client_ready(gint fd, GIOCondition condition, gpointer) {
  if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0 &&
      (condition & G_IO_IN) == 0) {
    close_connection(fd);
    return G_SOURCE_REMOVE;
  }
  const auto found = connections.find(fd);
  if (found == connections.end()) {
    return G_SOURCE_REMOVE;
  }
  Connection &connection = *found->second;
  char buffer[1024];
  for (;;) {
    const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
    if (count > 0) {
      connection.input.append(buffer, static_cast<std::size_t>(count));
      if (connection.input.size() > kMaxMessage) {
        close_connection(fd);
        return G_SOURCE_REMOVE;
      }
      const auto newline = connection.input.find('\n');
      if (newline != std::string::npos) {
        if (newline + 1 != connection.input.size()) {
          close_connection(fd);
          return G_SOURCE_REMOVE;
        }
        connection.input.resize(newline);
        std::string result = handle_request(connection.input);
        result.push_back('\n');
        std::size_t sent = 0;
        while (sent < result.size()) {
          const ssize_t written = ::send(fd, result.data() + sent,
                                         result.size() - sent, MSG_NOSIGNAL);
          if (written > 0) {
            sent += static_cast<std::size_t>(written);
          } else if (written < 0 && errno == EINTR) {
            continue;
          } else {
            break;
          }
        }
        close_connection(fd);
        return G_SOURCE_REMOVE;
      }
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return G_SOURCE_CONTINUE;
    }
    close_connection(fd);
    return G_SOURCE_REMOVE;
  }
}

gboolean listener_ready(gint fd, GIOCondition condition, gpointer) {
  if ((condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) != 0) {
    return G_SOURCE_CONTINUE;
  }
  for (;;) {
    const int client =
        ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      return G_SOURCE_CONTINUE;
    }
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    const uid_t own_uid = ::getuid();
    if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &size) !=
            0 ||
        (credentials.uid != own_uid && credentials.uid != 0)) {
      (void)::close(client);
      continue;
    }
    auto connection = std::make_unique<Connection>();
    connection->fd = client;
    connections.emplace(client, std::move(connection));
    (void)g_unix_fd_add(
        client,
        static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
        client_ready, nullptr);
  }
}

void initialize_listener() {
  const uid_t uid = ::getuid();
  const char *runtime = g_get_user_runtime_dir();
  const std::string expected = "/run/user/" + std::to_string(uid);
  if (runtime == nullptr || expected != runtime) {
    return;
  }
  struct stat directory {};
  if (::lstat(expected.c_str(), &directory) != 0 ||
      !S_ISDIR(directory.st_mode) || directory.st_uid != uid ||
      (directory.st_mode & 0022) != 0) {
    return;
  }
  const std::string listener_name = "punto-editor-adapter-" +
                                    std::to_string(uid) + "-" +
                                    std::to_string(::getpid()) + ".sock";
  if (listener_name.size() + 1 >= sizeof(sockaddr_un::sun_path)) {
    return;
  }
  listener_fd =
      ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener_fd < 0) {
    return;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  address.sun_path[0] = '\0';
  std::memcpy(address.sun_path + 1, listener_name.data(), listener_name.size());
  const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                             listener_name.size() + 1);
  if (::bind(listener_fd, reinterpret_cast<sockaddr *>(&address), length) !=
          0 ||
      ::listen(listener_fd, 8) != 0) {
    (void)::close(listener_fd);
    listener_fd = -1;
    return;
  }
  (void)g_unix_fd_add(
      listener_fd,
      static_cast<GIOCondition>(G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL),
      listener_ready, nullptr);
}

} // namespace

extern "C" G_MODULE_EXPORT void gtk_module_init(gint *, gchar ***) {
  editor_instance = generate_instance();
  initialize_listener();
  g_idle_add_full(G_PRIORITY_HIGH_IDLE, initialize_gate_control, nullptr,
                  nullptr);
#if PUNTO_WITH_XORG_RECIPIENT
  g_timeout_add_full(G_PRIORITY_DEFAULT_IDLE, 10,
                     initialize_native_recipient, nullptr, nullptr);
#endif
}
