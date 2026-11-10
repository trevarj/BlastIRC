#include "platform/file_picker.hpp"

#include <gio/gio.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace blast::platform {
namespace {
constexpr auto portalName = "org.freedesktop.portal.Desktop";
constexpr auto portalPath = "/org/freedesktop/portal/desktop";
constexpr auto requestInterface = "org.freedesktop.portal.Request";

FilePickerResult selectedFile(GVariant *response, GCancellable *cancellable) {
    if (g_cancellable_is_cancelled(cancellable))
        return {FilePickerStatus::Cancelled, {}, {}};
    guint32 code = 2;
    GVariant *options = nullptr;
    if (!g_variant_is_of_type(response, G_VARIANT_TYPE("(ua{sv})")))
        return {FilePickerStatus::Error, {}, "The file chooser returned an invalid response."};
    g_variant_get(response, "(u@a{sv})", &code, &options);
    if (code != 0) {
        g_variant_unref(options);
        return {code == 1 ? FilePickerStatus::Cancelled : FilePickerStatus::Error,
                {},
                code == 1 ? "" : "The file chooser could not select a file."};
    }
    auto *uris = g_variant_lookup_value(options, "uris", G_VARIANT_TYPE("as"));
    g_variant_unref(options);
    if (!uris || g_variant_n_children(uris) != 1) {
        if (uris)
            g_variant_unref(uris);
        return {FilePickerStatus::Error, {}, "Choose one local regular file."};
    }
    const char *uri = nullptr;
    g_variant_get_child(uris, 0, "&s", &uri);
    gchar *hostname = nullptr;
    GError *error = nullptr;
    // GLib rejects escaped NULs and non-file URIs. No URI is passed to a shell.
    gchar *path = g_filename_from_uri(uri, &hostname, &error);
    const bool local = !hostname || !*hostname || g_ascii_strcasecmp(hostname, "localhost") == 0;
    g_free(hostname);
    g_variant_unref(uris);
    if (!path || !local || !g_path_is_absolute(path)) {
        g_clear_error(&error);
        g_free(path);
        return {FilePickerStatus::Error, {}, "The chooser must return a local file path."};
    }
    PickedFile picked;
    picked.path = path;
    gchar *name = g_filename_display_basename(path);
    picked.name = name ? name : "file";
    g_free(name);
    // Size and identity come from the same original O_NOFOLLOW|O_NONBLOCK
    // regular-file open/fstat, not a second path lookup before showing review.
    const auto identity = inspectUploadFile(picked.path);
    if (!identity) {
        g_free(path);
        return {
            FilePickerStatus::Error, {}, "Select an accessible regular file, not a symbolic link."};
    }
    picked.identity = *identity;
    picked.size = identity->size;
    // Filename-based MIME is advisory; no separate file/path metadata can
    // silently describe a replacement inode.
    gchar *contentType = g_content_type_guess(path, nullptr, 0, nullptr);
    gchar *mime = contentType ? g_content_type_get_mime_type(contentType) : nullptr;
    picked.mime = mime ? mime : "application/octet-stream";
    g_free(mime);
    g_free(contentType);
    g_free(path);
    return {FilePickerStatus::Picked, std::move(picked), {}};
}
} // namespace

struct FilePicker::State {
    std::thread worker;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::optional<FilePickerResult> result;
    GCancellable *activeCancel = nullptr; // Protected by mutex, including during bus setup.

    void run(std::string title, std::string parentWindow) {
        GMainContext *context = g_main_context_new();
        g_main_context_push_thread_default(context);
        GCancellable *cancellable = g_cancellable_new();
        {
            std::lock_guard lock(mutex);
            activeCancel = cancellable;
            if (cancelRequested.load())
                g_cancellable_cancel(cancellable);
        }
        GError *error = nullptr;
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, cancellable, &error);
        FilePickerResult outcome{
            FilePickerStatus::Error, {}, "The desktop file chooser is unavailable."};
        if (bus) {
            struct Request {
                bool replied = false;
                bool failed = false;
                bool overflow = false;
                bool serviceGone = false;
                std::string path;
                std::vector<std::pair<std::string, GVariant *>> responses;
            } request;
            static std::atomic<unsigned long> sequence{0};
            const std::string token = "blastirc_" + std::to_string(++sequence);
            std::string sender = g_dbus_connection_get_unique_name(bus);
            if (!sender.empty() && sender.front() == ':')
                sender.erase(0, 1);
            std::replace(sender.begin(), sender.end(), '.', '_');
            const std::string predicted =
                std::string(portalPath) + "/request/" + sender + "/" + token;
            // Subscribe before OpenFile: portals may emit Response before its method reply.
            // Keep early replies until the returned request path establishes correlation.
            const guint subscription = g_dbus_connection_signal_subscribe(
                bus, portalName, requestInterface, "Response", nullptr, nullptr,
                G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection *, const gchar *, const gchar *path, const gchar *,
                   const gchar *, GVariant *parameters, gpointer data) {
                    auto &req = *static_cast<Request *>(data);
                    if (req.replied && req.path != path)
                        return;
                    if (req.responses.size() >= 16) {
                        req.overflow = true;
                        return;
                    }
                    req.responses.emplace_back(path, g_variant_ref(parameters));
                },
                &request, nullptr);
            const guint ownerSubscription = g_dbus_connection_signal_subscribe(
                bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                "/org/freedesktop/DBus", portalName, G_DBUS_SIGNAL_FLAGS_NONE,
                [](GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *,
                   GVariant *parameters, gpointer data) {
                    const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
                    g_variant_get(parameters, "(&s&s&s)", &name, &oldOwner, &newOwner);
                    if (*oldOwner && std::string(oldOwner) != newOwner)
                        static_cast<Request *>(data)->serviceGone = true;
                },
                &request, nullptr);
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&options, "{sv}", "handle_token",
                                  g_variant_new_string(token.c_str()));
            g_variant_builder_add(&options, "{sv}", "multiple", g_variant_new_boolean(false));
            g_variant_builder_add(&options, "{sv}", "directory", g_variant_new_boolean(false));
            g_variant_builder_add(&options, "{sv}", "modal", g_variant_new_boolean(true));
            // No exported Wayland foreign handle is available from our SDK. An empty
            // parent is valid; inventing a wayland:object-id would not identify a window.
            g_dbus_connection_call(
                bus, portalName, portalPath, "org.freedesktop.portal.FileChooser", "OpenFile",
                g_variant_new("(ss@a{sv})", parentWindow.c_str(), title.c_str(),
                              g_variant_builder_end(&options)),
                G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 15000, cancellable,
                [](GObject *object, GAsyncResult *result, gpointer data) {
                    auto &req = *static_cast<Request *>(data);
                    GError *callError = nullptr;
                    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object),
                                                                    result, &callError);
                    req.replied = true;
                    req.failed = !reply;
                    if (reply) {
                        const char *path = nullptr;
                        g_variant_get(reply, "(&o)", &path);
                        req.path = path;
                        g_variant_unref(reply);
                    }
                    g_clear_error(&callError);
                },
                &request);
            bool cancelled = false;
            for (;;) {
                while (g_main_context_pending(context))
                    g_main_context_iteration(context, false);
                if (cancelRequested.load()) {
                    cancelled = true;
                    g_cancellable_cancel(cancellable);
                    if (request.replied)
                        break;
                } else if (request.replied) {
                    if (request.failed || request.overflow)
                        break;
                    const auto match = std::find_if(
                        request.responses.begin(), request.responses.end(),
                        [&](const auto &response) { return response.first == request.path; });
                    if (match != request.responses.end()) {
                        outcome = selectedFile(match->second, cancellable);
                        break;
                    }
                }
                if (request.serviceGone || g_dbus_connection_is_closed(bus)) {
                    g_cancellable_cancel(cancellable);
                    if (request.replied)
                        break;
                }
                // A worker-only wait keeps cancellation bounded without UI callbacks.
                g_usleep(10000);
            }
            g_dbus_connection_signal_unsubscribe(bus, subscription);
            g_dbus_connection_signal_unsubscribe(bus, ownerSubscription);
            if (!request.serviceGone && (cancelled || request.failed || request.overflow)) {
                // Close the predicted handle even if cancellation hid the method reply.
                const auto close = [&](const std::string &path) {
                    GVariant *reply = g_dbus_connection_call_sync(
                        bus, portalName, path.c_str(), requestInterface, "Close", nullptr, nullptr,
                        G_DBUS_CALL_FLAGS_NONE, 1000, nullptr, nullptr);
                    if (reply)
                        g_variant_unref(reply);
                };
                close(predicted);
                if (!request.path.empty() && request.path != predicted)
                    close(request.path);
            }
            if (cancelled)
                outcome = {FilePickerStatus::Cancelled, {}, {}};
            for (auto &response : request.responses)
                g_variant_unref(response.second);
            g_object_unref(bus);
        }
        g_clear_error(&error);
        {
            std::lock_guard lock(mutex);
            activeCancel = nullptr;
        }
        g_object_unref(cancellable);
        g_main_context_pop_thread_default(context);
        g_main_context_unref(context);
        if (cancelRequested.load())
            outcome = {FilePickerStatus::Cancelled, {}, {}};
        {
            std::lock_guard lock(mutex);
            result = std::move(outcome);
        }
        running.store(false);
    }
};

FilePicker::FilePicker() : state_(std::make_unique<State>()) {}
FilePicker::~FilePicker() {
    cancel();
    if (state_->worker.joinable())
        state_->worker.join();
}
bool FilePicker::open(std::string title, std::string parentWindow) {
    if (busy())
        return false;
    if (state_->worker.joinable())
        state_->worker.join();
    {
        std::lock_guard lock(state_->mutex);
        state_->result.reset();
    }
    state_->cancelRequested.store(false);
    state_->running.store(true);
    try {
        state_->worker = std::thread([state = state_.get(), title = std::move(title),
                                      parentWindow = std::move(parentWindow)]() mutable {
            state->run(std::move(title), std::move(parentWindow));
        });
    } catch (...) {
        state_->running.store(false);
        std::lock_guard lock(state_->mutex);
        state_->result =
            FilePickerResult{FilePickerStatus::Error, {}, "Could not start the file chooser."};
        return false;
    }
    return true;
}
void FilePicker::cancel() {
    state_->cancelRequested.store(true);
    std::lock_guard lock(state_->mutex);
    if (state_->activeCancel)
        g_cancellable_cancel(state_->activeCancel);
}
bool FilePicker::busy() const { return state_->running.load(); }
std::optional<FilePickerResult> FilePicker::poll() {
    if (busy())
        return std::nullopt;
    if (state_->worker.joinable())
        state_->worker.join();
    std::lock_guard lock(state_->mutex);
    return std::exchange(state_->result, std::nullopt);
}
} // namespace blast::platform
