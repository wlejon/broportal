#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/filechooser.h"
#include "broportal/globalshortcuts.h"
#include "broportal/inhibit.h"
#include "broportal/openuri.h"
#include "broportal/remotedesktop.h"
#include "broportal/request.h"
#include "broportal/screencast.h"
#include "broportal/screenshot.h"
#include "broportal/session.h"
#include "broportal/settings.h"
#include "broportal/types.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace broportal {

struct BackendConfig {
    std::string bus_name = "org.freedesktop.impl.portal.desktop.bro";
    std::string object_path = "/org/freedesktop/portal/desktop";
    bool request_bus_name = true;
};

class PortalBackend {
public:
    explicit PortalBackend(std::unique_ptr<dbus::Bus> bus, BackendConfig config = {});
    ~PortalBackend();

    PortalBackend(const PortalBackend&) = delete;
    PortalBackend& operator=(const PortalBackend&) = delete;

    static std::unique_ptr<PortalBackend> create_on_user_bus(
        BackendConfig config = {},
        std::string* error = nullptr);

    static std::unique_ptr<PortalBackend> create_on_address(
        const std::string& address,
        BackendConfig config = {},
        std::string* error = nullptr);

    bool start(std::string* error = nullptr);
    void stop();
    bool is_running() const noexcept { return running_; }

    int process();
    int wait(uint64_t timeout_usec = UINT64_MAX);

    // Run event processing on a background thread
    bool run_in_background();
    void stop_background();

    dbus::Bus& bus() noexcept { return *bus_; }
    const BackendConfig& config() const noexcept { return config_; }

    // Interfaces
    FileChooserInterface& file_chooser() noexcept { return *file_chooser_; }
    ScreenshotInterface& screenshot() noexcept { return *screenshot_; }
    ScreenCastInterface& screencast() noexcept { return *screencast_; }
    RemoteDesktopInterface& remote_desktop() noexcept { return *remote_desktop_; }
    SettingsInterface& settings() noexcept { return *settings_; }
    InhibitInterface& inhibit() noexcept { return *inhibit_; }
    OpenURIInterface& open_uri() noexcept { return *open_uri_; }
    GlobalShortcutsInterface& global_shortcuts() noexcept { return *global_shortcuts_; }

    // Request & Session Management
    std::shared_ptr<Request> create_request(
        const ObjectPath& handle,
        const std::string& app_id = "");
    std::shared_ptr<Request> get_request(const ObjectPath& handle);
    void remove_request(const ObjectPath& handle);

    std::shared_ptr<Session> create_session(
        const ObjectPath& session_handle,
        const std::string& app_id = "",
        SessionType type = SessionType::Custom);
    std::shared_ptr<Session> get_session(const ObjectPath& session_handle);
    void remove_session(const ObjectPath& session_handle);

private:
    std::unique_ptr<dbus::Bus> bus_;
    BackendConfig config_;
    bool running_ = false;

    // Interface instances
    std::unique_ptr<FileChooserInterface> file_chooser_;
    std::unique_ptr<ScreenshotInterface> screenshot_;
    std::unique_ptr<ScreenCastInterface> screencast_;
    std::unique_ptr<RemoteDesktopInterface> remote_desktop_;
    std::unique_ptr<SettingsInterface> settings_;
    std::unique_ptr<InhibitInterface> inhibit_;
    std::unique_ptr<OpenURIInterface> open_uri_;
    std::unique_ptr<GlobalShortcutsInterface> global_shortcuts_;

    // Slots for exported interface vtables on object_path
    std::vector<dbus::Slot> interface_slots_;

    // Active requests and sessions
    mutable std::mutex req_mutex_;
    std::map<std::string, std::shared_ptr<Request>> requests_;

    mutable std::mutex sess_mutex_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;

    // Background thread runner
    std::atomic<bool> bg_stop_{false};
    std::thread bg_thread_;

    bool register_interfaces(std::string* error);
    void unregister_interfaces();
};

} // namespace broportal
