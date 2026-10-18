#include "punto/accessible_context.hpp"

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace punto {
namespace {
constexpr char kDBus[] = "org.freedesktop.DBus";
constexpr char kDBusPath[] = "/org/freedesktop/DBus";
constexpr char kRegistry[] = "org.a11y.atspi.Registry";
constexpr char kRegistryPath[] = "/org/a11y/atspi/registry";
constexpr char kRoot[] = "/org/a11y/atspi/accessible/root";
constexpr char kAccessible[] = "org.a11y.atspi.Accessible";
constexpr char kText[] = "org.a11y.atspi.Text";
constexpr char kCollection[] = "org.a11y.atspi.Collection";
constexpr char kProperties[] = "org.freedesktop.DBus.Properties";
constexpr std::size_t kReplyBytes = 8192, kApplications = 32;
guint64 now() { return static_cast<guint64>(g_get_monotonic_time()); }
std::string digest(const void *data, gsize size) {
  gchar *value = g_compute_checksum_for_data(
      G_CHECKSUM_SHA256, static_cast<const guchar *>(data), size);
  std::string result = value;
  g_free(value);
  return result;
}
bool focused(GVariant *reply) {
  GVariant *bits = g_variant_get_child_value(reply, 0);
  gsize count = 0;
  const auto *values = static_cast<const guint32 *>(
      g_variant_get_fixed_array(bits, &count, sizeof(guint32)));
  // AT-SPI state bits: defunct=6, editable=7, focused=12.
  const bool result = count > 0 && count <= 2 && (values[0] & (1U << 7)) &&
                      (values[0] & (1U << 12)) && !(values[0] & (1U << 6));
  g_variant_unref(bits);
  return result;
}
GVariant *focus_rule(gint32 required = (1 << 7) | (1 << 12),
                     const char *interface = nullptr) {
  const std::array<gint32, 2> states{required, 0};
  const std::array<gint32, 4> roles{};
  const std::array<const gchar *, 1> interfaces{interface};
  return g_variant_new(
      "(@aii@a{ss}i@aii@asib)",
      g_variant_new_fixed_array(G_VARIANT_TYPE_INT32, states.data(), 2, 4), 1,
      g_variant_new_array(G_VARIANT_TYPE("{ss}"), nullptr, 0), 1,
      g_variant_new_fixed_array(G_VARIANT_TYPE_INT32, roles.data(), 4, 4), 1,
      g_variant_new_strv(interface ? interfaces.data() : nullptr,
                         interface ? 1 : 0),
      1, FALSE);
}
} // namespace
GVariant *AccessibleObservation::wire(std::string_view helper) const {
  const std::string identity(helper);
  return g_variant_new("(ustssottuiiiiiss)", static_cast<guint>(status),
                       identity.c_str(), bus_epoch, registry_owner.c_str(),
                       provider.c_str(), object.c_str(), focus_generation,
                       observed_us, provider_pid, caret, selection_start,
                       selection_end, range.start, range.end,
                       range.sha256.c_str(), token.c_str());
}

struct AccessibleContext::State : std::enable_shared_from_this<State> {
  struct Job;
  GDBusConnection *session, *bus = nullptr;
  std::string helper, registry, watched_provider, watched_object;
  guint uid, session_subscription = 0, object_subscription = 0;
  guint window_subscription = 0, focus_subscription = 0;
  guint cache_subscription = 0, owner_subscription = 0;
  guint64 epoch = 0, focus_generation = 1, range_generation = 1;
  bool ready = false;
  std::weak_ptr<Job> active;
  State(GDBusConnection *value, std::string id, guint user)
      : session(G_DBUS_CONNECTION(g_object_ref(value))), helper(std::move(id)),
        uid(user) {}
  ~State();
  void reset();
  void invalidate_editor();
  void drop_bus();
  void watch();
  static void event(GDBusConnection *, const gchar *, const gchar *,
                    const gchar *, const gchar *, GVariant *, gpointer);
};

struct AccessibleContext::State::Job : std::enable_shared_from_this<Job> {
  struct Meta {
    gint caret = -1, count = -1, start = -1, end = -1, selections = -1;
    bool operator==(const Meta &b) const {
      return caret == b.caret && count == b.count && start == b.start &&
             end == b.end && selections == b.selections;
    }
  };
  std::shared_ptr<State> state;
  AccessibleRequest request;
  Completion completion;
  GCancellable *cancel = g_cancellable_new();
  GSource *timer = nullptr, *discovery_timer = nullptr;
  AccessibleObservation observation;
  std::vector<std::pair<std::string, std::string>> applications;
  std::size_t application_index = 0;
  guint64 focus = 0, range = 0, epoch = 0;
  unsigned range_attempts = 0;
  bool activation_attempted = false;
  std::vector<std::string> document_paths;
  bool done = false;
#if defined(PUNTO_SESSION_BRIDGE_APP_E2E)
  const char *last_rpc = "none";
  unsigned rpc_count = 0;
#endif
  Job(std::shared_ptr<State> owner, AccessibleRequest input,
      Completion callback)
      : state(std::move(owner)), request(std::move(input)),
        completion(std::move(callback)) {}
  ~Job() { g_object_unref(cancel); }
  void finish(AccessibleStatus status) {
    if (done)
      return;
    done = true;
    if (timer) {
      g_source_destroy(timer);
      g_source_unref(timer);
      timer = nullptr;
    }
    if (discovery_timer) {
      g_source_destroy(discovery_timer);
      g_source_unref(discovery_timer);
      discovery_timer = nullptr;
    }
    g_cancellable_cancel(cancel);
    if (now() >= request.deadline_us)
      status = AccessibleStatus::Expired;
#if defined(PUNTO_SESSION_BRIDGE_APP_E2E)
    g_printerr(
        "[punto-e2e] accessible_done scope=%u status=%u rpc=%s "
        "calls=%u applications=%zu activated=%u "
        "epoch=%llu/%llu focus=%llu/%llu range=%llu/%llu "
        "range_attempts=%u ready=%u token_ready=%u token_match=%u\n",
        static_cast<unsigned>(request.scope), static_cast<unsigned>(status),
        last_rpc, rpc_count, applications.size(),
        static_cast<unsigned>(activation_attempted),
        static_cast<unsigned long long>(epoch),
        static_cast<unsigned long long>(state->epoch),
        static_cast<unsigned long long>(focus),
        static_cast<unsigned long long>(state->focus_generation),
        static_cast<unsigned long long>(range),
        static_cast<unsigned long long>(state->range_generation),
        range_attempts, static_cast<unsigned>(state->ready),
        static_cast<unsigned>(!observation.token.empty()),
        static_cast<unsigned>(!observation.token.empty() &&
                              request.expected_token == observation.token));
#endif
    observation.status = status;
    if (status != AccessibleStatus::Observed)
      observation = AccessibleObservation{status};
    if (!state->ready)
      state->drop_bus();
    state->active.reset();
    auto callback = std::move(completion);
    callback(std::move(observation));
  }
  using Reply = std::function<void(GVariant *)>;
  void call(GDBusConnection *connection, const std::string &destination,
            const char *path, const char *interface, const char *method,
            GVariant *parameters, const char *type, Reply reply) {
    const auto started = now();
    if (done || started >= request.deadline_us) {
      if (parameters)
        g_variant_unref(g_variant_ref_sink(parameters));
      finish(AccessibleStatus::Expired);
      return;
    }
    auto self = shared_from_this();
    using Callback = std::function<void(GObject *, GAsyncResult *)>;
    auto *callback = new Callback([self, method, reply = std::move(reply)](
                                      GObject *source, GAsyncResult *result) {
      GError *error = nullptr;
      GVariant *value = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                      result, &error);
      if (!self->done) {
        if (!value)
          self->finish(AccessibleStatus::Unavailable);
        else if (g_variant_get_size(value) > kReplyBytes)
          self->finish(AccessibleStatus::Unknown);
        else if (now() >= self->request.deadline_us)
          self->finish(AccessibleStatus::Expired);
        else
          reply(value);
      }
      if (value)
        g_variant_unref(value);
      g_clear_error(&error);
    });
    const auto timeout =
        static_cast<gint>((request.deadline_us - started + 999) / 1000);
    g_dbus_connection_call(
        connection, destination.c_str(), path, interface, method, parameters,
        G_VARIANT_TYPE(type), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        std::max(timeout, 1), cancel,
        [](GObject *source, GAsyncResult *result, gpointer data) {
          std::unique_ptr<Callback> callback(static_cast<Callback *>(data));
          (*callback)(source, result);
        },
        callback);
#if defined(PUNTO_SESSION_BRIDGE_APP_E2E)
    last_rpc = method;
    ++rpc_count;
#endif
  }
  void initialize() {
    if (state->bus && !g_dbus_connection_is_closed(state->bus) &&
        state->ready) {
      bootstrap();
      return;
    }
    state->drop_bus();
    auto self = shared_from_this();
    call(
        state->session, kDBus, kDBusPath, kDBus, "GetNameOwner",
        g_variant_new("(s)", "org.a11y.Bus"), "(s)", [self](GVariant *value) {
          const gchar *owner = nullptr;
          g_variant_get(value, "(&s)", &owner);
          self->call(
              self->state->session, owner, "/org/a11y/bus", "org.a11y.Bus",
              "GetAddress", nullptr, "(s)", [self](GVariant *address) {
                const gchar *text = nullptr;
                g_variant_get(address, "(&s)", &text);
                if (!g_str_has_prefix(text, "unix:")) {
                  self->finish(AccessibleStatus::Unavailable);
                  return;
                }
                auto *holder = new std::shared_ptr<Job>(self);
                g_dbus_connection_new_for_address(
                    text,
                    // GLib flags are combinable; Clang requires an upstream
                    // flag_enum annotation.
                    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
                    GDBusConnectionFlags(
                        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
                    nullptr, self->cancel,
                    [](GObject *, GAsyncResult *result, gpointer data) {
                      std::unique_ptr<std::shared_ptr<Job>> holder(
                          static_cast<std::shared_ptr<Job> *>(data));
                      auto job = *holder;
                      GError *error = nullptr;
                      GDBusConnection *bus =
                          g_dbus_connection_new_for_address_finish(result,
                                                                   &error);
                      g_clear_error(&error);
                      if (job->done) {
                        if (bus)
                          g_object_unref(bus);
                        return;
                      }
                      if (!bus) {
                        job->finish(AccessibleStatus::Unavailable);
                        return;
                      }
                      GIOStream *stream = g_dbus_connection_get_stream(bus);
                      GCredentials *credentials =
                          G_IS_SOCKET_CONNECTION(stream)
                              ? g_socket_get_credentials(
                                    g_socket_connection_get_socket(
                                        G_SOCKET_CONNECTION(stream)),
                                    &error)
                              : nullptr;
                      const bool owned =
                          credentials &&
                          g_credentials_get_unix_user(credentials, &error) ==
                              job->state->uid;
                      if (credentials)
                        g_object_unref(credentials);
                      g_clear_error(&error);
                      if (!owned) {
                        g_object_unref(bus);
                        job->finish(AccessibleStatus::Unavailable);
                        return;
                      }
                      job->state->bus = bus;
                      ++job->state->epoch;
                      g_dbus_connection_set_exit_on_close(bus, FALSE);
                      g_signal_connect(
                          bus, "closed",
                          G_CALLBACK(+[](GDBusConnection *, gboolean, GError *,
                                         gpointer data) {
                            static_cast<State *>(data)->reset();
                          }),
                          job->state.get());
                      job->state->watch();
                      job->register_events(0);
                    },
                    holder);
              });
        });
  }
  void register_events(std::size_t index) {
    auto self = shared_from_this();
    if (index == 0) {
      call(state->bus, kDBus, kDBusPath, kDBus, "GetNameOwner",
           g_variant_new("(s)", kRegistry), "(s)", [self](GVariant *value) {
             const gchar *owner = nullptr;
             g_variant_get(value, "(&s)", &owner);
             self->state->registry = owner;
             self->register_events(1);
           });
      return;
    }
    constexpr std::array<const char *, 3> events{
        "object:", "window:", "focus:"};
    if (index > events.size()) {
      state->ready = true;
      bootstrap();
      return;
    }
    call(state->bus, state->registry, kRegistryPath, kRegistry, "RegisterEvent",
         g_variant_new("(s@ass)", events[index - 1],
                       g_variant_new_strv(nullptr, 0), ""),
         "()", [self, index](GVariant *) { self->register_events(index + 1); });
  }
  void bootstrap() {
    focus = state->focus_generation;
    epoch = state->epoch;
    auto self = shared_from_this();
    call(state->bus, state->registry, kRoot, kAccessible, "GetChildren",
         nullptr, "(a(so))", [self](GVariant *value) {
           GVariant *children = g_variant_get_child_value(value, 0);
           if (g_variant_n_children(children) > kApplications) {
             g_variant_unref(children);
             self->finish(AccessibleStatus::Unavailable);
             return;
           }
           GVariantIter iter;
           g_variant_iter_init(&iter, children);
           const gchar *provider = nullptr, *object = nullptr;
           while (g_variant_iter_loop(&iter, "(&s&o)", &provider, &object)) {
             if (!g_dbus_is_unique_name(provider)) {
               g_variant_unref(children);
               self->finish(AccessibleStatus::Unknown);
               return;
             }
             self->applications.emplace_back(provider, object);
           }
           g_variant_unref(children);
           self->find_application();
         });
  }
  void find_application() {
    auto self = shared_from_this();
    if (application_index == applications.size()) {
      if (observation.provider.empty()) {
        finish(AccessibleStatus::Unavailable);
        return;
      }
      call(state->bus, kDBus, kDBusPath, kDBus, "GetConnectionUnixUser",
           g_variant_new("(s)", observation.provider.c_str()), "(u)",
           [self](GVariant *value) {
             guint uid = 0;
             g_variant_get(value, "(u)", &uid);
             if (uid != self->state->uid) {
               self->finish(AccessibleStatus::Unavailable);
               return;
             }
             self->matches(false);
           });
      return;
    }
    const auto application = applications[application_index++];
    call(state->bus, kDBus, kDBusPath, kDBus, "GetConnectionUnixProcessID",
         g_variant_new("(s)", application.first.c_str()), "(u)",
         [self, application](GVariant *value) {
           guint pid = 0;
           g_variant_get(value, "(u)", &pid);
           if (pid == self->request.provider_pid) {
             if (!self->observation.provider.empty()) {
               self->finish(AccessibleStatus::Unavailable);
               return;
             }
             self->observation.provider = application.first;
             self->observation.object = application.second;
           }
           self->find_application();
         });
  }
  void matches(bool final) {
    auto self = shared_from_this();
    const char *app_path = nullptr;
    for (const auto &app : applications)
      if (app.first == observation.provider) {
        app_path = app.second.c_str();
        break;
      }
    call(state->bus, observation.provider, app_path, kCollection, "GetMatches",
         g_variant_new("(@(aiia{ss}iaiiasib)uib)", focus_rule(), 1U, 2, TRUE),
         "(a(so))", [self, final](GVariant *value) {
           GVariant *children = g_variant_get_child_value(value, 0);
           if (!final && g_variant_n_children(children) == 0 &&
               !self->activation_attempted) {
             g_variant_unref(children);
             self->activate_documents();
             return;
           }
           if (!final && g_variant_n_children(children) == 0 &&
               !self->document_paths.empty()) {
             g_variant_unref(children);
             self->wait_for_document();
             return;
           }
           if (g_variant_n_children(children) != 1) {
             g_variant_unref(children);
             self->finish(AccessibleStatus::Unavailable);
             return;
           }
           const gchar *provider = nullptr, *object = nullptr;
           g_variant_get_child(children, 0, "(&s&o)", &provider, &object);
           const bool valid = self->observation.provider == provider &&
                              (!final || self->observation.object == object);
           const std::string path(object);
           g_variant_unref(children);
           if (!valid) {
             self->finish(AccessibleStatus::Stale);
             return;
           }
           if (final) {
             self->complete();
             return;
           }
           self->observation.object = path;
           if (self->request.expected_token.empty() &&
               !self->request.expected_range)
             self->focus = self->state->focus_generation;
           if (!self->state->watched_provider.empty() &&
               (self->state->watched_provider != self->observation.provider ||
                self->state->watched_object != path)) {
             const bool uninterrupted =
                 self->focus == self->state->focus_generation;
             ++self->state->focus_generation;
             if (uninterrupted)
               self->focus = self->state->focus_generation;
           }
           self->state->watched_provider = self->observation.provider;
           self->state->watched_object = path;
           self->range = self->state->range_generation;
           self->call(self->state->bus, self->observation.provider,
                      path.c_str(), kAccessible, "GetState", nullptr, "(au)",
                      [self](GVariant *state) {
                        if (!focused(state)) {
                          self->finish(AccessibleStatus::Stale);
                          return;
                        }
                        if (self->request.scope == AccessibleScope::Child) {
                          self->matches(true);
                          return;
                        }
                        self->metadata(
                            [self](Meta meta) { self->read_range(meta); });
                      });
         });
  }
  void activate_documents() {
    activation_attempted = true;
    auto self = shared_from_this();
    const char *app_path = nullptr;
    for (const auto &app : applications)
      if (app.first == observation.provider) {
        app_path = app.second.c_str();
        break;
      }
    call(state->bus, observation.provider, app_path, kCollection, "GetMatches",
         g_variant_new("(@(aiia{ss}iaiiasib)uib)", focus_rule(0, "Document"),
                       1U, 4, TRUE),
         "(a(so))", [self](GVariant *value) {
           GVariant *children = g_variant_get_child_value(value, 0);
           if (g_variant_n_children(children) == 0 ||
               g_variant_n_children(children) > 4) {
             g_variant_unref(children);
             self->finish(AccessibleStatus::Unavailable);
             return;
           }
           GVariantIter iter;
           g_variant_iter_init(&iter, children);
           const gchar *provider = nullptr, *object = nullptr;
           while (g_variant_iter_loop(&iter, "(&s&o)", &provider, &object)) {
             if (self->observation.provider != provider) {
               g_variant_unref(children);
               self->finish(AccessibleStatus::Stale);
               return;
             }
             self->document_paths.emplace_back(object);
           }
           g_variant_unref(children);
           self->activate_document(0);
         });
  }
  void activate_document(std::size_t index) {
    if (index == document_paths.size()) {
      matches(false);
      return;
    }
    auto self = shared_from_this();
    // Read standard extended properties on browser documents. This requests
    // their lazy editable tree, not text mutation or screen-reader activation.
    call(state->bus, observation.provider, document_paths[index].c_str(),
         kAccessible, "GetAttributes", nullptr, "(a{ss})",
         [self, index](GVariant *) { self->activate_document(index + 1); });
  }
  void wait_for_document() {
    if (epoch != state->epoch || !state->ready ||
        ((!request.expected_token.empty() || request.expected_range) &&
         focus != state->focus_generation)) {
      finish(AccessibleStatus::Stale);
      return;
    }
    // Renderer accessibility is asynchronous. GLib owns the short readiness
    // wait; the original hard timer remains the only observation deadline.
    discovery_timer = g_timeout_source_new(10);
    auto *holder = new std::shared_ptr<Job>(shared_from_this());
    g_source_set_callback(
        discovery_timer,
        [](gpointer data) {
          auto job = *static_cast<std::shared_ptr<Job> *>(data);
          GSource *source = job->discovery_timer;
          job->discovery_timer = nullptr;
          g_source_unref(source);
          job->matches(false);
          return G_SOURCE_REMOVE;
        },
        holder,
        [](gpointer data) {
          delete static_cast<std::shared_ptr<Job> *>(data);
        });
    g_source_attach(discovery_timer, nullptr);
  }
  void metadata(std::function<void(Meta)> callback) {
    auto self = shared_from_this();
    call(state->bus, observation.provider, observation.object.c_str(),
         kProperties, "GetAll", g_variant_new("(s)", kText), "(a{sv})",
         [self, callback = std::move(callback)](GVariant *value) {
           GVariant *properties = g_variant_get_child_value(value, 0);
           Meta meta;
           const bool valid =
               g_variant_lookup(properties, "CaretOffset", "i", &meta.caret) &&
               g_variant_lookup(properties, "CharacterCount", "i",
                                &meta.count) &&
               meta.caret >= 0 && meta.count >= meta.caret;
           g_variant_unref(properties);
           if (!valid) {
             self->finish(AccessibleStatus::Unknown);
             return;
           }
           self->call(
               self->state->bus, self->observation.provider,
               self->observation.object.c_str(), kText, "GetNSelections",
               nullptr, "(i)", [self, meta, callback](GVariant *value) mutable {
                 g_variant_get(value, "(i)", &meta.selections);
                 if (meta.selections == 0) {
                   meta.start = meta.end = meta.caret;
                   callback(meta);
                   return;
                 }
                 if (meta.selections != 1) {
                   self->finish(AccessibleStatus::Unavailable);
                   return;
                 }
                 self->call(self->state->bus, self->observation.provider,
                            self->observation.object.c_str(), kText,
                            "GetSelection", g_variant_new("(i)", 0), "(ii)",
                            [self, meta, callback](GVariant *value) mutable {
                              g_variant_get(value, "(ii)", &meta.start,
                                            &meta.end);
                              if (meta.start < 0 || meta.end < meta.start ||
                                  meta.end > meta.count) {
                                self->finish(AccessibleStatus::Unknown);
                                return;
                              }
                              callback(meta);
                            });
               });
         });
  }
  void read_range(Meta before) {
    ++range_attempts;
    observation.range = AccessibleRange{};
    auto self = shared_from_this();
    observation.caret = before.caret;
    observation.selection_start = before.start;
    observation.selection_end = before.end;
    if (before.end - before.start > kAccessibleRangeCharacters) {
      if (request.expected_range) {
        finish(AccessibleStatus::Stale);
        return;
      }
      // Child-only observation remains usable; no bounded range proof is
      // returned.
      metadata([self, before](Meta after) {
        if (!(before == after)) {
          self->reobserve_range();
          return;
        }
        self->matches(true);
      });
      return;
    }
    call(state->bus, observation.provider, observation.object.c_str(), kText,
         "GetText", g_variant_new("(ii)", before.start, before.end), "(s)",
         [self, before](GVariant *value) {
           const gchar *text = nullptr;
           g_variant_get(value, "(&s)", &text);
           const gsize bytes = std::char_traits<char>::length(text);
           if (bytes > kAccessibleRangeBytes ||
               g_utf8_strlen(text, -1) != before.end - before.start) {
             self->finish(AccessibleStatus::Unknown);
             return;
           }
           self->observation.range = {before.start, before.end,
                                      digest(text, bytes)};
           self->metadata([self, before](Meta after) {
             if (!(before == after)) {
               self->reobserve_range();
               return;
             }
             self->matches(true);
           });
         });
  }
  void reobserve_range() {
    if (epoch == state->epoch && focus == state->focus_generation &&
        state->ready) {
      // Let queued caret/selection signals settle within the original deadline
      // and this Job's authority; never retry an editor mutation.
      range = state->range_generation;
      auto self = shared_from_this();
      metadata([self](Meta value) { self->read_range(value); });
      return;
    }
    finish(AccessibleStatus::Stale);
  }
  void complete() {
    if (request.scope == AccessibleScope::Range &&
        range != state->range_generation) {
      reobserve_range();
      return;
    }
    if (epoch != state->epoch || focus != state->focus_generation ||
        (request.scope == AccessibleScope::Range &&
         range != state->range_generation) ||
        !state->ready) {
      finish(AccessibleStatus::Stale);
      return;
    }
    observation.bus_epoch = epoch;
    observation.focus_generation = focus;
    observation.registry_owner = state->registry;
    observation.provider_pid = request.provider_pid;
    observation.observed_us = now();
    GVariant *identity = g_variant_ref_sink(g_variant_new(
        "(stssotu)", state->helper.c_str(), epoch, state->registry.c_str(),
        observation.provider.c_str(), observation.object.c_str(), focus,
        observation.provider_pid));
    observation.token =
        digest(g_variant_get_data(identity), g_variant_get_size(identity));
    g_variant_unref(identity);
    const auto &expected = request.expected_range;
    if ((!request.expected_token.empty() &&
         request.expected_token != observation.token) ||
        (expected && (expected->start != observation.range.start ||
                      expected->end != observation.range.end ||
                      expected->sha256 != observation.range.sha256))) {
      finish(AccessibleStatus::Stale);
      return;
    }
    GVariant *reply = g_variant_ref_sink(observation.wire(state->helper));
    const bool bounded = g_variant_get_size(reply) <= 1024;
    g_variant_unref(reply);
    finish(bounded ? AccessibleStatus::Observed
                   : AccessibleStatus::Unavailable);
  }
};

void AccessibleContext::State::reset() {
  drop_bus();
  invalidate_editor();
}
void AccessibleContext::State::invalidate_editor() {
  ++focus_generation;
  ++range_generation;
  watched_provider.clear();
  watched_object.clear();
  if (auto job = active.lock())
    job->finish(AccessibleStatus::Stale);
}
void AccessibleContext::State::drop_bus() {
  ready = false;
  if (!bus)
    return;
  for (guint subscription :
       {object_subscription, window_subscription, focus_subscription,
        cache_subscription, owner_subscription})
    if (subscription)
      g_dbus_connection_signal_unsubscribe(bus, subscription);
  object_subscription = window_subscription = focus_subscription = 0;
  cache_subscription = owner_subscription = 0;
  g_signal_handlers_disconnect_by_data(bus, this);
  g_dbus_connection_close(bus, nullptr, nullptr, nullptr);
  g_object_unref(bus);
  bus = nullptr;
  registry.clear();
  watched_provider.clear();
  watched_object.clear();
}
AccessibleContext::State::~State() {
  reset();
  if (session_subscription)
    g_dbus_connection_signal_unsubscribe(session, session_subscription);
  g_object_unref(session);
}
void AccessibleContext::State::event(GDBusConnection *connection,
                                     const gchar *sender, const gchar *path,
                                     const gchar *interface,
                                     const gchar *member, GVariant *parameters,
                                     gpointer data) {
  auto *state = static_cast<State *>(data);
  if (g_str_equal(member, "NameOwnerChanged")) {
    const gchar *name = nullptr, *old_owner = nullptr, *new_owner = nullptr;
    g_variant_get(parameters, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (old_owner[0] &&
        (connection == state->session
             ? g_str_equal(name, "org.a11y.Bus")
             : (name == state->registry || g_str_equal(name, kRegistry))))
      state->reset();
    else if (old_owner[0] && connection == state->bus &&
             name == state->watched_provider)
      state->invalidate_editor();
    return;
  }
  if (g_str_equal(member, "StateChanged")) {
    if (g_variant_n_children(parameters) < 1) {
      ++state->focus_generation;
      return;
    }
    GVariant *detail = g_variant_get_child_value(parameters, 0);
    if (g_variant_is_of_type(detail, G_VARIANT_TYPE_STRING)) {
      const gchar *name = g_variant_get_string(detail, nullptr);
      bool repeated_focus = false;
      if (g_str_equal(name, "focused") && state->watched_provider == sender &&
          state->watched_object == path &&
          g_variant_n_children(parameters) > 1) {
        GVariant *value = g_variant_get_child_value(parameters, 1);
        repeated_focus = g_variant_is_of_type(value, G_VARIANT_TYPE_INT32) &&
                         g_variant_get_int32(value) != 0;
        g_variant_unref(value);
      }
      if ((g_str_equal(name, "focused") && !repeated_focus) ||
          (g_str_equal(name, "defunct") && state->watched_provider == sender &&
           state->watched_object == path))
        ++state->focus_generation;
    } else
      ++state->focus_generation;
    g_variant_unref(detail);
  } else if ((g_str_equal(interface, "org.a11y.atspi.Event.Window") &&
              (g_str_equal(member, "Activate") ||
               g_str_equal(member, "Deactivate"))) ||
             (g_str_equal(interface, "org.a11y.atspi.Event.Focus") &&
              (state->watched_provider != sender ||
               state->watched_object != path)))
    ++state->focus_generation;
  else if (g_str_equal(member, "RemoveAccessible") ||
           g_str_equal(member, "ChildrenChanged") ||
           g_str_equal(member, "ActiveDescendantChanged")) {
    const gsize index = g_str_equal(member, "RemoveAccessible") ? 0 : 3;
    if (g_variant_n_children(parameters) <= index)
      return;
    GVariant *value = g_variant_get_child_value(parameters, index);
    GVariant *reference = g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)
                              ? g_variant_get_variant(value)
                              : g_variant_ref(value);
    if (g_variant_is_of_type(reference, G_VARIANT_TYPE("(so)"))) {
      const gchar *provider = nullptr, *object = nullptr;
      g_variant_get(reference, "(&s&o)", &provider, &object);
      if (state->watched_provider == provider &&
          state->watched_object == object)
        ++state->focus_generation;
    }
    g_variant_unref(reference);
    g_variant_unref(value);
  } else if (state->watched_provider == sender &&
             state->watched_object == path &&
             (g_str_equal(member, "TextChanged") ||
              g_str_equal(member, "TextSelectionChanged") ||
              g_str_equal(member, "TextCaretMoved")))
    ++state->range_generation;
}
void AccessibleContext::State::watch() {
  auto subscribe = [this](const char *interface, const char *member) {
    return g_dbus_connection_signal_subscribe(
        bus, nullptr, interface, member, nullptr, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, &State::event, this, nullptr);
  };
  object_subscription = subscribe("org.a11y.atspi.Event.Object", nullptr);
  window_subscription = subscribe("org.a11y.atspi.Event.Window", nullptr);
  focus_subscription = subscribe("org.a11y.atspi.Event.Focus", nullptr);
  cache_subscription = subscribe("org.a11y.atspi.Cache", "RemoveAccessible");
  owner_subscription = subscribe(kDBus, "NameOwnerChanged");
}
AccessibleContext::AccessibleContext(GDBusConnection *session,
                                     std::string helper, std::uint32_t uid)
    : state_(std::make_shared<State>(session, std::move(helper), uid)) {
  state_->session_subscription = g_dbus_connection_signal_subscribe(
      session, kDBus, kDBus, "NameOwnerChanged", kDBusPath, "org.a11y.Bus",
      G_DBUS_SIGNAL_FLAGS_NONE, &State::event, state_.get(), nullptr);
}
AccessibleContext::~AccessibleContext() { cancel(); }
void AccessibleContext::cancel() { state_->reset(); }
void AccessibleContext::cancel_observation() {
  if (auto job = state_->active.lock())
    job->finish(AccessibleStatus::Stale);
}
void AccessibleContext::observe(AccessibleRequest request,
                                Completion completion) {
  if (auto active = state_->active.lock()) {
    if (!active->request.preparation || request.preparation) {
      completion(AccessibleObservation{AccessibleStatus::Unavailable});
      return;
    }
    // Optional preparation must yield the slot to a real observation.
    active->finish(AccessibleStatus::Stale);
  }
  auto job = std::make_shared<State::Job>(state_, std::move(request),
                                          std::move(completion));
  state_->active = job;
  job->timer = g_timeout_source_new(0);
  g_source_set_ready_time(job->timer,
                          static_cast<gint64>(job->request.deadline_us));
  auto *holder = new std::shared_ptr<State::Job>(job);
  g_source_set_callback(
      job->timer,
      [](gpointer data) {
        auto job = *static_cast<std::shared_ptr<State::Job> *>(data);
        job->finish(AccessibleStatus::Expired);
        return G_SOURCE_REMOVE;
      },
      holder,
      [](gpointer data) {
        delete static_cast<std::shared_ptr<State::Job> *>(data);
      });
  g_source_attach(job->timer, nullptr);
  job->initialize();
}
} // namespace punto
