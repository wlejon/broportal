#include "broportal/backend.h"

#include <iostream>

namespace broportal {

PortalBackend::PortalBackend(std::unique_ptr<dbus::Bus> bus, BackendConfig config)
    : bus_(std::move(bus)),
      config_(std::move(config)) {
    file_chooser_ = std::make_unique<FileChooserInterface>(*this);
    screenshot_ = std::make_unique<ScreenshotInterface>(*this);
    screencast_ = std::make_unique<ScreenCastInterface>(*this);
    remote_desktop_ = std::make_unique<RemoteDesktopInterface>(*this);
    settings_ = std::make_unique<SettingsInterface>(*this);
    inhibit_ = std::make_unique<InhibitInterface>(*this);
    open_uri_ = std::make_unique<OpenURIInterface>(*this);
    global_shortcuts_ = std::make_unique<GlobalShortcutsInterface>(*this);
}

PortalBackend::~PortalBackend() {
    stop();
}

std::unique_ptr<PortalBackend> PortalBackend::create_on_user_bus(
    BackendConfig config,
    std::string* error) {
    auto bus = dbus::Bus::open_user(error);
    if (!bus) return nullptr;
    return std::make_unique<PortalBackend>(std::move(bus), std::move(config));
}

std::unique_ptr<PortalBackend> PortalBackend::create_on_address(
    const std::string& address,
    BackendConfig config,
    std::string* error) {
    auto bus = dbus::Bus::open_address(address, error);
    if (!bus) return nullptr;
    return std::make_unique<PortalBackend>(std::move(bus), std::move(config));
}

bool PortalBackend::register_interfaces(std::string* error) {
    if (!bus_ || !bus_->is_valid()) {
        if (error) *error = "Invalid D-Bus connection";
        return false;
    }

    const std::string& path = config_.object_path;

    auto s1 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.FileChooser", FileChooserInterface::vtable, file_chooser_.get(), error);
    if (!s1) return false;
    interface_slots_.push_back(std::move(s1));

    auto s2 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.Screenshot", ScreenshotInterface::vtable, screenshot_.get(), error);
    if (!s2) return false;
    interface_slots_.push_back(std::move(s2));

    auto s3 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.ScreenCast", ScreenCastInterface::vtable, screencast_.get(), error);
    if (!s3) return false;
    interface_slots_.push_back(std::move(s3));

    auto s4 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.RemoteDesktop", RemoteDesktopInterface::vtable, remote_desktop_.get(), error);
    if (!s4) return false;
    interface_slots_.push_back(std::move(s4));

    auto s5 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.Settings", SettingsInterface::vtable, settings_.get(), error);
    if (!s5) return false;
    interface_slots_.push_back(std::move(s5));

    auto s6 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.Inhibit", InhibitInterface::vtable, inhibit_.get(), error);
    if (!s6) return false;
    interface_slots_.push_back(std::move(s6));

    auto s7 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.OpenURI", OpenURIInterface::vtable, open_uri_.get(), error);
    if (!s7) return false;
    interface_slots_.push_back(std::move(s7));

    auto s8 = bus_->add_object_vtable(path, "org.freedesktop.impl.portal.GlobalShortcuts", GlobalShortcutsInterface::vtable, global_shortcuts_.get(), error);
    if (!s8) return false;
    interface_slots_.push_back(std::move(s8));

    return true;
}

void PortalBackend::unregister_interfaces() {
    interface_slots_.clear();
}

bool PortalBackend::start(std::string* error) {
    if (running_) return true;

    if (!bus_ || !bus_->is_valid()) {
        if (error) *error = "Invalid D-Bus connection";
        return false;
    }

    if (config_.request_bus_name && !config_.bus_name.empty()) {
        uint64_t flags = SD_BUS_NAME_REPLACE_EXISTING | SD_BUS_NAME_ALLOW_REPLACEMENT;
        if (!bus_->request_name(config_.bus_name, flags, error)) {
            return false;
        }
    }

    if (!register_interfaces(error)) {
        if (config_.request_bus_name && !config_.bus_name.empty()) {
            bus_->release_name(config_.bus_name);
        }
        return false;
    }

    running_ = true;
    return true;
}

void PortalBackend::stop() {
    if (!running_) return;

    stop_background();

    // Close all open requests and sessions
    std::vector<std::shared_ptr<Request>> reqs_to_close;
    {
        std::lock_guard<std::mutex> lock(req_mutex_);
        for (auto& [_, req] : requests_) {
            if (req) reqs_to_close.push_back(req);
        }
        requests_.clear();
    }
    for (auto& req : reqs_to_close) {
        req->close();
    }

    std::vector<std::shared_ptr<Session>> sess_to_close;
    {
        std::lock_guard<std::mutex> lock(sess_mutex_);
        for (auto& [_, sess] : sessions_) {
            if (sess) sess_to_close.push_back(sess);
        }
        sessions_.clear();
    }
    for (auto& sess : sess_to_close) {
        sess->close();
    }

    unregister_interfaces();

    if (bus_ && bus_->is_valid() && config_.request_bus_name && !config_.bus_name.empty()) {
        bus_->release_name(config_.bus_name);
    }

    running_ = false;
}

int PortalBackend::process() {
    return bus_ ? bus_->process() : -1;
}

int PortalBackend::wait(uint64_t timeout_usec) {
    return bus_ ? bus_->wait(timeout_usec) : -1;
}

bool PortalBackend::run_in_background() {
    if (bg_thread_.joinable()) return true;

    bg_stop_.store(false);
    bg_thread_ = std::thread([this]() {
        while (!bg_stop_.load()) {
            if (bus_) {
                while (bus_->process() > 0) {}
                bus_->wait(50000); // 50ms wait
            } else {
                break;
            }
        }
    });

    return true;
}

void PortalBackend::stop_background() {
    if (bg_thread_.joinable()) {
        bg_stop_.store(true);
        if (bus_) {
            bus_->flush();
        }
        bg_thread_.join();
    }
}

std::shared_ptr<Request> PortalBackend::create_request(
    const ObjectPath& handle,
    const std::string& app_id) {
    if (!bus_ || handle.path.empty()) return nullptr;

    auto req = std::make_shared<Request>(*bus_, handle, app_id);
    std::string path_str = handle.path;

    {
        std::lock_guard<std::mutex> lock(req_mutex_);
        requests_[path_str] = req;
    }

    req->set_close_callback([this, path_str](Request&) {
        std::lock_guard<std::mutex> lock(req_mutex_);
        requests_.erase(path_str);
    });

    return req;
}

std::shared_ptr<Request> PortalBackend::get_request(const ObjectPath& handle) {
    std::lock_guard<std::mutex> lock(req_mutex_);
    auto it = requests_.find(handle.path);
    if (it != requests_.end()) {
        return it->second;
    }
    return nullptr;
}

void PortalBackend::remove_request(const ObjectPath& handle) {
    std::shared_ptr<Request> req;
    {
        std::lock_guard<std::mutex> lock(req_mutex_);
        auto it = requests_.find(handle.path);
        if (it != requests_.end()) {
            req = it->second;
            requests_.erase(it);
        }
    }
    if (req) {
        req->close();
    }
}

std::shared_ptr<Session> PortalBackend::create_session(
    const ObjectPath& session_handle,
    const std::string& app_id,
    SessionType type) {
    if (!bus_ || session_handle.path.empty()) return nullptr;

    auto sess = std::make_shared<Session>(*bus_, session_handle, app_id, type);
    std::string path_str = session_handle.path;

    {
        std::lock_guard<std::mutex> lock(sess_mutex_);
        sessions_[path_str] = sess;
    }

    sess->set_close_callback([this, path_str](Session&) {
        std::lock_guard<std::mutex> lock(sess_mutex_);
        sessions_.erase(path_str);
    });

    return sess;
}

std::shared_ptr<Session> PortalBackend::get_session(const ObjectPath& session_handle) {
    std::lock_guard<std::mutex> lock(sess_mutex_);
    auto it = sessions_.find(session_handle.path);
    if (it != sessions_.end()) {
        return it->second;
    }
    return nullptr;
}

void PortalBackend::remove_session(const ObjectPath& session_handle) {
    std::shared_ptr<Session> sess;
    {
        std::lock_guard<std::mutex> lock(sess_mutex_);
        auto it = sessions_.find(session_handle.path);
        if (it != sessions_.end()) {
            sess = it->second;
            sessions_.erase(it);
        }
    }
    if (sess) {
        sess->close();
    }
}

} // namespace broportal
