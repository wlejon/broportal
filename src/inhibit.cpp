#include "broportal/inhibit.h"
#include "broportal/backend.h"

#include <iostream>

namespace broportal {

const sd_bus_vtable InhibitInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Inhibit", "ossua{sv}", "", &InhibitInterface::dbus_inhibit, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CreateMonitor", "ooss", "u", &InhibitInterface::dbus_create_monitor, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("QueryEndResponse", "o", "", &InhibitInterface::dbus_query_end_response, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("StateChanged", "oa{sv}", 0),
    SD_BUS_VTABLE_END
};

InhibitInterface::InhibitInterface(PortalBackend& backend)
    : backend_(backend) {}

InhibitInterface::~InhibitInterface() = default;

void InhibitInterface::inhibit(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& window,
    uint32_t flags,
    const VariantMap& options) {
    InhibitEntry entry;
    entry.handle = handle;
    entry.app_id = app_id;
    entry.window = window;
    entry.flags = flags;
    entry.reason = get_string(options, "reason").value_or("");

    InhibitChangeListener listener_copy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inhibitions_[handle.path] = entry;
        listener_copy = listener_;
    }

    if (listener_copy) {
        listener_copy(true, entry);
    }

    // Export Request object that stays alive until closed
    auto req = backend_.create_request(handle, app_id);
    if (req) {
        std::string p = handle.path;
        req->set_close_callback([this, p](Request&) {
            InhibitEntry removed_entry;
            InhibitChangeListener cb;
            bool found = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = inhibitions_.find(p);
                if (it != inhibitions_.end()) {
                    removed_entry = it->second;
                    inhibitions_.erase(it);
                    found = true;
                    cb = listener_;
                }
            }
            if (found && cb) {
                cb(false, removed_entry);
            }
            // This replaced the backend's own close callback: drop it from
            // the backend's registry too.
            backend_.remove_request(ObjectPath{p});
        });
    }
}

ResponseCode InhibitInterface::create_monitor(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const std::string& /*window*/) {
    auto req = backend_.create_request(handle, app_id);
    auto sess = backend_.create_session(session_handle, app_id, SessionType::InhibitMonitor);
    if (!sess) {
        if (req) req->close();
        return ResponseCode::OtherError;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_monitors_.insert(session_handle.path);
    }

    std::string path_str = session_handle.path;
    sess->set_close_callback([this, path_str](Session&) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_monitors_.erase(path_str);
        }
        backend_.remove_session(ObjectPath{path_str});
    });

    if (req) {
        req->close();
    }

    return ResponseCode::Success;
}

void InhibitInterface::query_end_response(const ObjectPath& session_handle) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_monitors_.contains(session_handle.path)) return;
    }
    if (query_end_listener_) {
        query_end_listener_(session_handle);
    }
}

void InhibitInterface::notify_state_changed(bool screensaver_active, SessionState state) {
    std::vector<std::string> monitors;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        monitors.assign(active_monitors_.begin(), active_monitors_.end());
    }

    VariantMap state_map;
    state_map["screensaver-active"] = Variant(screensaver_active);
    state_map["session-state"] = Variant(static_cast<uint32_t>(state));

    for (const auto& mon_path : monitors) {
        backend_.bus().emit_signal(
            "/org/freedesktop/portal/desktop",
            "org.freedesktop.impl.portal.Inhibit",
            "StateChanged",
            [&mon_path, &state_map](dbus::Message& msg) {
                msg.append_object_path(ObjectPath{mon_path});
                msg.append_variant_map(state_map);
            });
    }
}

std::vector<InhibitEntry> InhibitInterface::active_inhibitions() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<InhibitEntry> result;
    for (const auto& [_, entry] : inhibitions_) {
        result.push_back(entry);
    }
    return result;
}

bool InhibitInterface::is_inhibited(InhibitFlag flag) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t mask = static_cast<uint32_t>(flag);
    for (const auto& [_, entry] : inhibitions_) {
        if (entry.flags & mask) {
            return true;
        }
    }
    return false;
}

int InhibitInterface::dbus_inhibit(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<InhibitInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string window;
    uint32_t flags = 0;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&window) ||
        !msg.read_uint32(&flags) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    self->inhibit(handle, app_id, window, flags, options);
    return sd_bus_reply_method_return(m, "");
}

int InhibitInterface::dbus_create_monitor(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<InhibitInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle, session_handle;
    std::string app_id, window;

    if (!msg.read_object_path(&handle) ||
        !msg.read_object_path(&session_handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&window)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    ResponseCode code = self->create_monitor(handle, session_handle, app_id, window);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));

    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int InhibitInterface::dbus_query_end_response(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<InhibitInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath session_handle;
    if (!msg.read_object_path(&session_handle)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    self->query_end_response(session_handle);
    return sd_bus_reply_method_return(m, "");
}

} // namespace broportal
