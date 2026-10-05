#include "broportal/request.h"

#include <iostream>

namespace broportal {

static const sd_bus_vtable request_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Close", "", "", &Request::dbus_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("Response", "ua{sv}", 0),
    SD_BUS_VTABLE_END
};

Request::Request(
    dbus::Bus& bus,
    const ObjectPath& handle,
    const std::string& app_id,
    CloseCallback on_close)
    : bus_(&bus),
      handle_(handle),
      app_id_(app_id),
      on_close_(std::move(on_close)) {
    register_vtable();
}

Request::~Request() {
    close();
}

Request::Request(Request&& other) noexcept
    : bus_(other.bus_),
      handle_(std::move(other.handle_)),
      app_id_(std::move(other.app_id_)),
      on_close_(std::move(other.on_close_)),
      slot_(std::move(other.slot_)),
      closed_(other.closed_) {
    other.bus_ = nullptr;
    other.closed_ = true;
    if (slot_.is_valid()) {
        sd_bus_slot_set_userdata(slot_.get(), this);
    }
}

Request& Request::operator=(Request&& other) noexcept {
    if (this != &other) {
        close();
        bus_ = other.bus_;
        handle_ = std::move(other.handle_);
        app_id_ = std::move(other.app_id_);
        on_close_ = std::move(other.on_close_);
        slot_ = std::move(other.slot_);
        closed_ = other.closed_;

        other.bus_ = nullptr;
        other.closed_ = true;
        if (slot_.is_valid()) {
            sd_bus_slot_set_userdata(slot_.get(), this);
        }
    }
    return *this;
}

void Request::register_vtable() {
    if (!bus_ || handle_.path.empty()) return;

    slot_ = bus_->add_object_vtable(
        handle_.path,
        "org.freedesktop.impl.portal.Request",
        request_vtable,
        this);
}

bool Request::complete(ResponseCode code, const VariantMap& results) {
    if (closed_ || !bus_ || handle_.path.empty()) return false;

    bool ok = bus_->emit_signal(
        handle_.path,
        "org.freedesktop.impl.portal.Request",
        "Response",
        [code, &results](dbus::Message& msg) {
            msg.append_uint32(static_cast<uint32_t>(code));
            msg.append_variant_map(results);
        });

    close();
    return ok;
}

void Request::close() {
    if (closed_) return;
    closed_ = true;
    slot_.reset();

    if (on_close_) {
        auto cb = std::move(on_close_);
        cb(*this);
    }
}

int Request::dbus_close(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* req = static_cast<Request*>(userdata);
    if (req) {
        req->close();
    }
    return sd_bus_reply_method_return(m, "");
}

} // namespace broportal
