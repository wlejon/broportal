#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

namespace broportal {

class PortalBackend;

enum class InhibitFlag : uint32_t {
    Logout = 1,
    UserSwitch = 2,
    Suspend = 4,
    Idle = 8
};

enum class SessionState : uint32_t {
    Running = 1,
    QueryEnd = 2,
    Ending = 3
};

struct InhibitEntry {
    ObjectPath handle;
    std::string app_id;
    std::string window;
    uint32_t flags = 0;
    std::string reason;
};

using InhibitChangeListener = std::function<void(bool added, const InhibitEntry& entry)>;

class InhibitInterface {
public:
    explicit InhibitInterface(PortalBackend& backend);
    ~InhibitInterface();

    void set_inhibit_change_listener(InhibitChangeListener listener) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        listener_ = std::move(listener);
    }

    // Called when a monitor acknowledges a QueryEnd (QueryEndResponse).
    void set_query_end_listener(std::function<void(const ObjectPath& session_handle)> listener) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        query_end_listener_ = std::move(listener);
    }

    void inhibit(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& window,
        uint32_t flags,
        const VariantMap& options);

    ResponseCode create_monitor(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const std::string& window);

    void query_end_response(const ObjectPath& session_handle);

    void notify_state_changed(bool screensaver_active, SessionState state);

    std::vector<InhibitEntry> active_inhibitions() const;
    bool is_inhibited(InhibitFlag flag) const;

    // D-Bus method handlers
    static int dbus_inhibit(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_create_monitor(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_query_end_response(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);

    static const sd_bus_vtable vtable[];

private:
    PortalBackend& backend_;
    mutable std::mutex mutex_;
    mutable std::mutex cb_mutex_;
    std::map<std::string, InhibitEntry> inhibitions_;
    std::set<std::string> active_monitors_;
    InhibitChangeListener listener_;
    std::function<void(const ObjectPath&)> query_end_listener_;
};

} // namespace broportal
