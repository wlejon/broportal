#include "broportal/globalshortcuts.h"
#include "broportal/backend.h"

#include <chrono>
#include <iostream>

namespace broportal {

const sd_bus_vtable GlobalShortcutsInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("CreateSession", "oosa{sv}", "ua{sv}", &GlobalShortcutsInterface::dbus_create_session, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("BindShortcuts", "ooa(sa{sv})sa{sv}", "ua{sv}", &GlobalShortcutsInterface::dbus_bind_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ListShortcuts", "oo", "ua{sv}", &GlobalShortcutsInterface::dbus_list_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ConfigureShortcuts", "osa{sv}", "", &GlobalShortcutsInterface::dbus_configure_shortcuts, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Activated", "osta{sv}", 0),
    SD_BUS_SIGNAL("Deactivated", "osta{sv}", 0),
    SD_BUS_SIGNAL("ShortcutsChanged", "oa(sa{sv})", 0),
    SD_BUS_PROPERTY("version", "u", &GlobalShortcutsInterface::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

GlobalShortcutsInterface::GlobalShortcutsInterface(PortalBackend& backend)
    : backend_(backend) {}

GlobalShortcutsInterface::~GlobalShortcutsInterface() = default;

ResponseCode GlobalShortcutsInterface::create_session(
    const ObjectPath& /*handle*/,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const VariantMap& /*options*/,
    VariantMap& results) {
    auto session = backend_.create_session(session_handle, app_id, SessionType::GlobalShortcuts);
    if (!session) {
        return ResponseCode::OtherError;
    }

    std::string sess_id = "globalshortcuts-" + std::to_string(reinterpret_cast<uintptr_t>(session.get()));
    session->set_session_id(sess_id);
    results["session_id"] = Variant(sess_id);

    return ResponseCode::Success;
}

ResponseCode GlobalShortcutsInterface::bind_shortcuts(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const ShortcutList& shortcuts,
    const std::string& /*parent_window*/,
    const VariantMap& /*options*/,
    VariantMap& results) {
    auto session = backend_.get_session(session_handle);
    if (!session || session->type() != SessionType::GlobalShortcuts) {
        return ResponseCode::OtherError;
    }
    BindShortcutsCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = bind_callback_;
    }

    if (!cb) {
        return ResponseCode::OtherError;
    }

    ShortcutList bound;
    ResponseCode code = cb(handle, session_handle, app_id.empty() ? session->app_id() : app_id,
                           shortcuts, bound, results);

    if (code == ResponseCode::Success) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_shortcuts_[session_handle.path] = bound;
        }

        results["shortcuts"] = Variant(bound);
        notify_shortcuts_changed(session_handle, bound);
    }

    return code;
}

ResponseCode GlobalShortcutsInterface::list_shortcuts(
    const ObjectPath& /*handle*/,
    const ObjectPath& session_handle,
    VariantMap& results) {
    auto session = backend_.get_session(session_handle);
    if (!session) {
        return ResponseCode::OtherError;
    }

    ShortcutList list;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = session_shortcuts_.find(session_handle.path);
        if (it != session_shortcuts_.end()) {
            list = it->second;
        }
    }

    results["shortcuts"] = Variant(list);
    return ResponseCode::Success;
}

bool GlobalShortcutsInterface::activate_shortcut(
    const ObjectPath& session_handle,
    const std::string& shortcut_id,
    uint64_t timestamp,
    const VariantMap& options) {
    if (timestamp == 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    return backend_.emit_signal(
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "Activated",
        [&session_handle, &shortcut_id, timestamp, &options](dbus::Message& msg) {
            msg.append_object_path(session_handle);
            msg.append_string(shortcut_id);
            msg.append_uint64(timestamp);
            msg.append_variant_map(options);
        });
}

bool GlobalShortcutsInterface::deactivate_shortcut(
    const ObjectPath& session_handle,
    const std::string& shortcut_id,
    uint64_t timestamp,
    const VariantMap& options) {
    if (timestamp == 0) {
        auto now = std::chrono::steady_clock::now().time_since_epoch();
        timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    return backend_.emit_signal(
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "Deactivated",
        [&session_handle, &shortcut_id, timestamp, &options](dbus::Message& msg) {
            msg.append_object_path(session_handle);
            msg.append_string(shortcut_id);
            msg.append_uint64(timestamp);
            msg.append_variant_map(options);
        });
}

bool GlobalShortcutsInterface::notify_shortcuts_changed(
    const ObjectPath& session_handle,
    const ShortcutList& shortcuts) {
    return backend_.emit_signal(
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.impl.portal.GlobalShortcuts",
        "ShortcutsChanged",
        [&session_handle, &shortcuts](dbus::Message& msg) {
            msg.append_object_path(session_handle);
            msg.append_shortcut_list(shortcuts);
        });
}

int GlobalShortcutsInterface::dbus_create_session(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<GlobalShortcutsInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    std::string app_id;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, app_id);
    VariantMap results;
    ResponseCode code = self->create_session(handle, session_handle, app_id, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int GlobalShortcutsInterface::dbus_bind_shortcuts(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<GlobalShortcutsInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    ShortcutList shortcuts;
    std::string parent_window;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_shortcut_list(&shortcuts) ||
        !msg.read_string(&parent_window) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, "");
    VariantMap results;
    ResponseCode code = self->bind_shortcuts(handle, session_handle, "", shortcuts, parent_window, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int GlobalShortcutsInterface::dbus_list_shortcuts(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<GlobalShortcutsInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, "");
    VariantMap results;
    ResponseCode code = self->list_shortcuts(handle, session_handle, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int GlobalShortcutsInterface::dbus_configure_shortcuts(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<GlobalShortcutsInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session_handle;
    std::string parent_window;
    VariantMap options;
    if (!msg.read_object_path(&session_handle) ||
        !msg.read_string(&parent_window) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }
    if (!self->backend_.get_session(session_handle)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "No such session %s",
                                          session_handle.path.c_str());
    }
    std::function<void(const ObjectPath&, const std::string&, const VariantMap&)> cb;
    {
        std::lock_guard<std::mutex> lock(self->cb_mutex_);
        cb = self->configure_callback_;
    }
    if (!cb) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_NOT_SUPPORTED, "No shortcut configuration UI");
    }
    cb(session_handle, parent_window, options);
    return sd_bus_reply_method_return(m, "");
}

int GlobalShortcutsInterface::dbus_get_property(
    sd_bus* /*bus*/,
    const char* /*path*/,
    const char* /*interface*/,
    const char* property,
    sd_bus_message* reply,
    void* /*userdata*/,
    sd_bus_error* /*ret_error*/) {
    if (std::string(property) == "version") {
        uint32_t val = 2;
        return sd_bus_message_append_basic(reply, 'u', &val);
    }
    return -EINVAL;
}

} // namespace broportal
