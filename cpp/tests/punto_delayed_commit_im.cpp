#include <gtk/gtkimmodule.h>
#include <gdk/gdkkeysyms.h>
#include <gmodule.h>

#include <cstring>

namespace {

struct DelayedContext {
  GtkIMContext parent;
  GString *pending;
  guint timeout;
  gboolean focused;
};

struct DelayedContextClass {
  GtkIMContextClass parent_class;
};

GType delayed_type = 0;
GObjectClass *parent_object_class = nullptr;
constexpr char kContextId[] = "punto-delayed-commit";
const GtkIMContextInfo context_info = {
    kContextId, "Punto controlled delayed commit", "", "", ""};
const GtkIMContextInfo *context_list[] = {&context_info};

guint configured_delay() {
  const char *raw = g_getenv("PUNTO_PROTO_IM_DELAY_MS");
  if (raw == nullptr || *raw == '\0')
    return 50;
  char *end = nullptr;
  const guint64 value = g_ascii_strtoull(raw, &end, 10);
  return end != raw && *end == '\0' && value <= 1000
             ? static_cast<guint>(value)
             : 50;
}

void clear_pending(DelayedContext *self) {
  if (self->timeout != 0) {
    g_source_remove(self->timeout);
    self->timeout = 0;
  }
  if (self->pending->len == 0)
    return;
  g_string_truncate(self->pending, 0);
  g_signal_emit_by_name(self, "preedit-changed");
  g_signal_emit_by_name(self, "preedit-end");
}

void commit_pending(DelayedContext *self) {
  if (self->timeout != 0) {
    g_source_remove(self->timeout);
    self->timeout = 0;
  }
  if (self->pending->len == 0)
    return;
  gchar *text = g_strdup(self->pending->str);
  g_signal_emit_by_name(self, "commit", text);
  g_free(text);
  clear_pending(self);
}

gboolean delayed_commit(gpointer data) {
  auto *self = static_cast<DelayedContext *>(data);
  self->timeout = 0;
  if (self->focused)
    commit_pending(self);
  else
    clear_pending(self);
  return G_SOURCE_REMOVE;
}

gboolean filter_keypress(GtkIMContext *context, GdkEventKey *event) {
  auto *self = reinterpret_cast<DelayedContext *>(context);
  if (event->type != GDK_KEY_PRESS)
    return FALSE;
  if (event->keyval == GDK_KEY_space) {
    commit_pending(self);
    g_signal_emit_by_name(self, "commit", " ");
    return TRUE;
  }
  if (event->keyval == GDK_KEY_Tab ||
      event->keyval == GDK_KEY_ISO_Left_Tab ||
      event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter) {
    commit_pending(self);
    return FALSE;
  }
  if ((event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SUPER_MASK)) !=
      0)
    return FALSE;
  const gunichar character = gdk_keyval_to_unicode(event->keyval);
  if (!g_unichar_isprint(character))
    return FALSE;
  gchar utf8[7]{};
  const gint length = g_unichar_to_utf8(character, utf8);
  if (length <= 0)
    return FALSE;
  if (self->pending->len == 0)
    g_signal_emit_by_name(self, "preedit-start");
  g_string_append_len(self->pending, utf8, length);
  g_signal_emit_by_name(self, "preedit-changed");
  if (self->timeout != 0)
    g_source_remove(self->timeout);
  self->timeout = g_timeout_add_full(G_PRIORITY_DEFAULT, configured_delay(),
                                     delayed_commit, g_object_ref(self),
                                     g_object_unref);
  return TRUE;
}

void reset(GtkIMContext *context) {
  clear_pending(reinterpret_cast<DelayedContext *>(context));
}

void focus_in(GtkIMContext *context) {
  reinterpret_cast<DelayedContext *>(context)->focused = TRUE;
}

void focus_out(GtkIMContext *context) {
  auto *self = reinterpret_cast<DelayedContext *>(context);
  self->focused = FALSE;
  clear_pending(self);
}

void get_preedit(GtkIMContext *context, gchar **text, PangoAttrList **attrs,
                 gint *cursor_pos) {
  const auto *self = reinterpret_cast<DelayedContext *>(context);
  if (text != nullptr)
    *text = g_strdup(self->pending->str);
  if (attrs != nullptr)
    *attrs = pango_attr_list_new();
  if (cursor_pos != nullptr)
    *cursor_pos = static_cast<gint>(g_utf8_strlen(self->pending->str, -1));
}

void finalize(GObject *object) {
  auto *self = reinterpret_cast<DelayedContext *>(object);
  clear_pending(self);
  g_string_free(self->pending, TRUE);
  parent_object_class->finalize(object);
}

void class_init(gpointer type_class, gpointer) {
  auto *klass = static_cast<DelayedContextClass *>(type_class);
  parent_object_class = static_cast<GObjectClass *>(
      g_type_class_peek_parent(type_class));
  auto *im = GTK_IM_CONTEXT_CLASS(klass);
  im->filter_keypress = filter_keypress;
  im->reset = reset;
  im->focus_in = focus_in;
  im->focus_out = focus_out;
  im->get_preedit_string = get_preedit;
  G_OBJECT_CLASS(klass)->finalize = finalize;
}

void instance_init(GTypeInstance *instance, gpointer) {
  auto *self = reinterpret_cast<DelayedContext *>(instance);
  self->pending = g_string_new(nullptr);
  self->timeout = 0;
  self->focused = FALSE;
}

} // namespace

extern "C" G_MODULE_EXPORT void im_module_init(GTypeModule *module) {
  const GTypeInfo info = {sizeof(DelayedContextClass), nullptr, nullptr,
                          class_init, nullptr, nullptr,
                          sizeof(DelayedContext), 0, instance_init, nullptr};
  delayed_type = g_type_module_register_type(
      module, GTK_TYPE_IM_CONTEXT, "PuntoDelayedCommitContext", &info,
      static_cast<GTypeFlags>(0));
}

extern "C" G_MODULE_EXPORT void im_module_exit() {}

extern "C" G_MODULE_EXPORT void
im_module_list(const GtkIMContextInfo ***contexts, int *count) {
  *contexts = context_list;
  *count = 1;
}

extern "C" G_MODULE_EXPORT GtkIMContext *
im_module_create(const gchar *context_id) {
  if (delayed_type == 0 || context_id == nullptr ||
      std::strcmp(context_id, kContextId) != 0)
    return nullptr;
  return GTK_IM_CONTEXT(g_object_new(delayed_type, nullptr));
}
