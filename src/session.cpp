#include "broportal/session.h"
#include "broportal/backend.h"

#include <iostream>

namespace broportal {

static const sd_bus_vtable session_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", &Session::dbus_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Closed", "", 0),
    SD_BUS_PROPERTY("version", "u", &Session::dbus_get_property, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

Session::Session(
    PortalBackend& backend,
    const ObjectPath& session_handle,
    const std::string& app_id,
    SessionType type,
    CloseCallback on_close)
    : backend_(&backend),
      bus_(&backend.bus()),
      session_handle_(session_handle),
      app_id_(app_id),
      type_(type),
      on_close_(std::move(on_close)) {
    register_vtable();
}

Session::Session(
    dbus::Bus& bus,
    const ObjectPath& session_handle,
    const std::string& app_id,
    SessionType type,
    CloseCallback on_close)
    : bus_(&bus),
      session_handle_(session_handle),
      app_id_(app_id),
      type_(type),
      on_close_(std::move(on_close)) {
    register_vtable();
}

Session::~Session() {
    close();
}

Session::Session(Session&& other) noexcept {
    std::lock_guard<std::mutex> lock(other.mu_);
    backend_ = other.backend_;
    bus_ = other.bus_;
    session_handle_ = std::move(other.session_handle_);
    app_id_ = std::move(other.app_id_);
    session_id_ = std::move(other.session_id_);
    type_ = other.type_;
    on_close_ = std::move(other.on_close_);
    slot_ = std::move(other.slot_);
    closed_.store(other.closed_.load());
    context_ = std::move(other.context_);

    other.backend_ = nullptr;
    other.bus_ = nullptr;
    other.closed_.store(true);
    if (slot_.is_valid()) {
        sd_bus_slot_set_userdata(slot_.get(), this);
    }
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        close();
        std::lock_guard<std::mutex> lock(other.mu_);
        backend_ = other.backend_;
        bus_ = other.bus_;
        session_handle_ = std::move(other.session_handle_);
        app_id_ = std::move(other.app_id_);
        session_id_ = std::move(other.session_id_);
        type_ = other.type_;
        on_close_ = std::move(other.on_close_);
        slot_ = std::move(other.slot_);
        closed_.store(other.closed_.load());
        context_ = std::move(other.context_);

        other.backend_ = nullptr;
        other.bus_ = nullptr;
        other.closed_.store(true);
        if (slot_.is_valid()) {
            sd_bus_slot_set_userdata(slot_.get(), this);
        }
    }
    return *this;
}

void Session::register_vtable() {
    if (!bus_ || session_handle_.path.empty()) return;

    slot_ = bus_->add_object_vtable(
        session_handle_.path,
        "org.freedesktop.impl.portal.Session",
        session_vtable,
        this);
}

void Session::set_context(const std::string& key, std::any value) {
    std::lock_guard<std::mutex> lock(mu_);
    context_[key] = std::move(value);
}

std::any Session::get_context(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = context_.find(key);
    if (it != context_.end()) {
        return it->second;
    }
    return {};
}

bool Session::has_context(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mu_);
    return context_.find(key) != context_.end();
}

void Session::close() {
    CloseCallback cb;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (closed_.exchange(true)) return;

        if (!session_handle_.path.empty()) {
            if (backend_) {
                backend_->emit_signal(
                    session_handle_.path,
                    "org.freedesktop.impl.portal.Session",
                    "Closed");
            } else if (bus_) {
                bus_->emit_signal(
                    session_handle_.path,
                    "org.freedesktop.impl.portal.Session",
                    "Closed");
            }
        }

        slot_.reset();
        cb = std::move(on_close_);
    }

    if (cb) {
        cb(*this);
    }
}

int Session::dbus_close(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* session = static_cast<Session*>(userdata);
    if (session) {
        session->close();
    }
    return sd_bus_reply_method_return(m, "");
}

int Session::dbus_get_property(
    sd_bus* /*bus*/,
    const char* /*path*/,
    const char* /*interface*/,
    const char* property,
    sd_bus_message* reply,
    void* /*userdata*/,
    sd_bus_error* /*ret_error*/) {
    if (std::string(property) == "version") {
        uint32_t v = 1;
        return sd_bus_message_append_basic(reply, 'u', &v);
    }
    return -EINVAL;
}

} // namespace broportal
