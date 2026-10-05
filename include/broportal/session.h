#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <any>
#include <functional>
#include <memory>
#include <string>

namespace broportal {

enum class SessionType {
    ScreenCast,
    RemoteDesktop,
    InhibitMonitor,
    GlobalShortcuts,
    Custom
};

class Session {
public:
    using CloseCallback = std::function<void(Session&)>;

    Session(
        dbus::Bus& bus,
        const ObjectPath& session_handle,
        const std::string& app_id = "",
        SessionType type = SessionType::Custom,
        CloseCallback on_close = nullptr);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;

    const ObjectPath& handle() const noexcept { return session_handle_; }
    const std::string& app_id() const noexcept { return app_id_; }
    const std::string& session_id() const noexcept { return session_id_; }
    SessionType type() const noexcept { return type_; }
    bool is_closed() const noexcept { return closed_; }

    void set_session_id(std::string id) { session_id_ = std::move(id); }
    void set_close_callback(CloseCallback cb) { on_close_ = std::move(cb); }

    // Session-specific context storage
    void set_context(const std::string& key, std::any value);
    std::any get_context(const std::string& key) const;
    bool has_context(const std::string& key) const;

    // Closes session and emits Closed signal
    void close();

    // D-Bus handlers
    static int dbus_close(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_get_property(
        sd_bus* bus,
        const char* path,
        const char* interface,
        const char* property,
        sd_bus_message* reply,
        void* userdata,
        sd_bus_error* ret_error);

private:
    dbus::Bus* bus_ = nullptr;
    ObjectPath session_handle_;
    std::string app_id_;
    std::string session_id_;
    SessionType type_;
    CloseCallback on_close_;
    dbus::Slot slot_;
    bool closed_ = false;
    std::map<std::string, std::any> context_;

    void register_vtable();
};

} // namespace broportal
