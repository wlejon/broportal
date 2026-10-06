#include "broportal/backend.h"

#if defined(__linux__)
#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <iostream>
#include <queue>
#include <condition_variable>

namespace broportal {

class PortalBackend::WorkerPool {
public:
    explicit WorkerPool(size_t threads = 4) {
        for (size_t i = 0; i < threads; ++i) {
            workers_.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mu_);
                        cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
                        if (stop_ && tasks_.empty()) return;
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    if (task) {
                        try {
                            task();
                        } catch (...) {
                            // Suppress worker exceptions
                        }
                    }
                }
            });
        }
    }

    ~WorkerPool() {
        stop();
    }

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (stop_) return;
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) {
                w.join();
            }
        }
        workers_.clear();
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> workers_;
};

PortalBackend::PortalBackend(std::unique_ptr<dbus::Bus> bus, BackendConfig config)
    : bus_(std::move(bus)),
      config_(std::move(config)) {
#if defined(__linux__)
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
#endif
    worker_pool_ = std::make_unique<WorkerPool>(4);

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
#if defined(__linux__)
    if (wake_fd_ >= 0) {
        close(wake_fd_);
        wake_fd_ = -1;
    }
#endif
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
    std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
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

    if (worker_pool_) {
        worker_pool_->stop();
    }

    std::lock_guard<std::recursive_mutex> lock(bus_mutex_);

    // Close all open requests and sessions
    std::vector<std::shared_ptr<Request>> reqs_to_close;
    {
        std::lock_guard<std::mutex> rlock(req_mutex_);
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
        std::lock_guard<std::mutex> slock(sess_mutex_);
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
    std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
    return bus_ ? bus_->process() : -1;
}

int PortalBackend::wait(uint64_t timeout_usec) {
    return bus_ ? bus_->wait(timeout_usec) : -1;
}

void PortalBackend::wake() {
#if defined(__linux__)
    if (wake_fd_ >= 0) {
        uint64_t val = 1;
        (void)::write(wake_fd_, &val, sizeof(val));
    }
#endif
}

bool PortalBackend::run_in_background() {
    if (bg_thread_.joinable()) return true;

    bg_stop_.store(false);
    bg_thread_ = std::thread([this]() {
        while (!bg_stop_.load()) {
            {
                std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
                if (bus_ && bus_->is_valid()) {
                    while (bus_->process() > 0) {}
                }
            }
            if (bg_stop_.load()) break;

#if defined(__linux__)
            pollfd pfds[2];
            int num_fds = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
                if (bus_ && bus_->is_valid()) {
                    pfds[0].fd = bus_->get_fd();
                    pfds[0].events = POLLIN | POLLPRI;
                    num_fds = 1;
                }
            }
            if (wake_fd_ >= 0) {
                pfds[num_fds].fd = wake_fd_;
                pfds[num_fds].events = POLLIN;
                num_fds++;
            }

            if (num_fds == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            int r = ::poll(pfds, num_fds, 50); // 50ms timeout
            if (r > 0) {
                for (int i = 0; i < num_fds; ++i) {
                    if (pfds[i].fd == wake_fd_ && (pfds[i].revents & POLLIN)) {
                        uint64_t val = 0;
                        (void)::read(wake_fd_, &val, sizeof(val));
                    }
                }
            }
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
#endif
        }
    });

    return true;
}

void PortalBackend::stop_background() {
    if (bg_thread_.joinable()) {
        bg_stop_.store(true);
        wake();
        {
            std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
            if (bus_) {
                bus_->flush();
            }
        }
        bg_thread_.join();
    }
}

bool PortalBackend::emit_signal(
    const std::string& path,
    const std::string& interface,
    const std::string& member,
    std::function<void(dbus::Message&)> build_args,
    std::string* error) {
    std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
    if (!bus_ || !bus_->is_valid()) {
        if (error) *error = "Invalid bus connection";
        return false;
    }
    bool ok = bus_->emit_signal(path, interface, member, std::move(build_args), error);
    if (ok) {
        bus_->flush();
        wake();
    }
    return ok;
}

bool PortalBackend::send_method_reply_and_unref(
    sd_bus_message* request_msg,
    std::function<void(dbus::Message&)> build_reply,
    std::string* error) {
    if (!request_msg) return false;
    std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
    if (!bus_ || !bus_->is_valid()) {
        if (error) *error = "Invalid bus connection";
        sd_bus_message_unref(request_msg);
        return false;
    }

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(request_msg, &reply);
    if (r < 0) {
        if (error) *error = strerror(-r);
        sd_bus_message_unref(request_msg);
        return false;
    }

    dbus::Message reply_msg(reply, true);
    if (build_reply) {
        build_reply(reply_msg);
    }

    int send_r = sd_bus_send(bus_->raw(), reply_msg.raw(), nullptr);
    if (send_r < 0) {
        if (error) *error = strerror(-send_r);
    } else {
        bus_->flush();
    }
    sd_bus_message_unref(request_msg);
    wake();
    return send_r >= 0;
}

bool PortalBackend::send_method_error_and_unref(
    sd_bus_message* request_msg,
    const std::string& name,
    const std::string& message) {
    if (!request_msg) return false;
    std::lock_guard<std::recursive_mutex> lock(bus_mutex_);
    if (!bus_ || !bus_->is_valid()) {
        sd_bus_message_unref(request_msg);
        return false;
    }
    sd_bus_reply_method_errorf(request_msg, name.c_str(), "%s", message.c_str());
    bus_->flush();
    sd_bus_message_unref(request_msg);
    wake();
    return true;
}

void PortalBackend::post_worker(std::function<void()> task) {
    if (worker_pool_) {
        worker_pool_->post(std::move(task));
    } else if (task) {
        task();
    }
}

std::shared_ptr<Request> PortalBackend::create_request(
    const ObjectPath& handle,
    const std::string& app_id) {
    if (!bus_ || handle.path.empty()) return nullptr;

    auto req = std::make_shared<Request>(*this, handle, app_id);
    std::string path_str = handle.path;

    {
        std::lock_guard<std::mutex> lock(req_mutex_);
        requests_[path_str] = req;
    }
    events_.push(PortalEvent{PortalEvent::Kind::RequestCreated, handle, app_id});

    req->set_close_callback([this, path_str](Request&) {
        {
            std::lock_guard<std::mutex> lock(req_mutex_);
            requests_.erase(path_str);
        }
        events_.push(PortalEvent{PortalEvent::Kind::RequestClosed, ObjectPath{path_str}, ""});
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

    auto sess = std::make_shared<Session>(*this, session_handle, app_id, type);
    std::string path_str = session_handle.path;

    {
        std::lock_guard<std::mutex> lock(sess_mutex_);
        sessions_[path_str] = sess;
    }
    events_.push(PortalEvent{PortalEvent::Kind::SessionCreated, session_handle, app_id});

    sess->set_close_callback([this, path_str](Session&) {
        {
            std::lock_guard<std::mutex> lock(sess_mutex_);
            sessions_.erase(path_str);
        }
        events_.push(PortalEvent{PortalEvent::Kind::SessionClosed, ObjectPath{path_str}, ""});
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
