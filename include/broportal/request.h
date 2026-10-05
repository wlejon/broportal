#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <memory>
#include <string>

namespace broportal {

class RequestManager;

class Request {
public:
    using CloseCallback = std::function<void(Request&)>;

    Request(
        dbus::Bus& bus,
        const ObjectPath& handle,
        const std::string& app_id = "",
        CloseCallback on_close = nullptr);
    ~Request();

    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    Request(Request&& other) noexcept;
    Request& operator=(Request&& other) noexcept;

    const ObjectPath& handle() const noexcept { return handle_; }
    const std::string& app_id() const noexcept { return app_id_; }
    bool is_closed() const noexcept { return closed_; }

    // Emits the Response signal and marks request as completed
    bool complete(ResponseCode code, const VariantMap& results = {});

    // Closes the request (e.g. aborted by caller)
    void close();

    void set_close_callback(CloseCallback cb) { on_close_ = std::move(cb); }

    // D-Bus vtable handler for Close()
    static int dbus_close(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);

private:
    dbus::Bus* bus_ = nullptr;
    ObjectPath handle_;
    std::string app_id_;
    CloseCallback on_close_;
    dbus::Slot slot_;
    bool closed_ = false;

    void register_vtable();
};

} // namespace broportal
