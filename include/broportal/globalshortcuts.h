#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace broportal {

class PortalBackend;

using BindShortcutsCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const ObjectPath& session_handle,
    const std::string& app_id,
    const ShortcutList& shortcuts,
    ShortcutList& out_bound_shortcuts,
    VariantMap& out_results)>;

class GlobalShortcutsInterface {
public:
    explicit GlobalShortcutsInterface(PortalBackend& backend);
    ~GlobalShortcutsInterface();

    // The compositor binds: it fills out_bound_shortcuts with what it actually
    // grabbed (each with a "trigger_description"). Without a callback,
    // BindShortcuts answers OtherError.
    void set_bind_shortcuts_callback(BindShortcutsCallback callback) {
        bind_callback_ = std::move(callback);
    }

    // Opens the host's shortcut settings (ConfigureShortcuts). Without one,
    // ConfigureShortcuts answers org.freedesktop.DBus.Error.NotSupported.
    void set_configure_callback(std::function<void(const ObjectPath& session_handle,
                                                   const std::string& parent_window,
                                                   const VariantMap& options)> callback) {
        configure_callback_ = std::move(callback);
    }

    ResponseCode create_session(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode bind_shortcuts(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        const std::string& app_id,
        const ShortcutList& shortcuts,
        const std::string& parent_window,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode list_shortcuts(
        const ObjectPath& handle,
        const ObjectPath& session_handle,
        VariantMap& results);

    // Trigger shortcut signals
    bool activate_shortcut(
        const ObjectPath& session_handle,
        const std::string& shortcut_id,
        uint64_t timestamp = 0,
        const VariantMap& options = {});

    bool deactivate_shortcut(
        const ObjectPath& session_handle,
        const std::string& shortcut_id,
        uint64_t timestamp = 0,
        const VariantMap& options = {});

    bool notify_shortcuts_changed(
        const ObjectPath& session_handle,
        const ShortcutList& shortcuts);

    // D-Bus method and property handlers
    static int dbus_create_session(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_bind_shortcuts(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_list_shortcuts(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_configure_shortcuts(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_get_property(
        sd_bus* bus,
        const char* path,
        const char* interface,
        const char* property,
        sd_bus_message* reply,
        void* userdata,
        sd_bus_error* ret_error);

    static const sd_bus_vtable vtable[];

private:
    PortalBackend& backend_;
    mutable std::mutex mutex_;
    std::map<std::string, ShortcutList> session_shortcuts_;
    BindShortcutsCallback bind_callback_;
    std::function<void(const ObjectPath&, const std::string&, const VariantMap&)> configure_callback_;
};

} // namespace broportal
