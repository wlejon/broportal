#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <mutex>
#include <string>

namespace broportal {

class PortalBackend;

using OpenUriCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& uri,
    const VariantMap& options,
    VariantMap& out_results)>;

using OpenFileCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const std::string& app_id,
    int fd,
    const VariantMap& options,
    VariantMap& out_results)>;

class OpenURIInterface {
public:
    explicit OpenURIInterface(PortalBackend& backend);
    ~OpenURIInterface();

    void set_open_uri_callback(OpenUriCallback callback) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        open_uri_callback_ = std::move(callback);
    }

    void set_open_file_callback(OpenFileCallback callback) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        open_file_callback_ = std::move(callback);
    }

    ResponseCode open_uri(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        const std::string& uri,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode open_file(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        int fd,
        const VariantMap& options,
        VariantMap& results);

    // D-Bus method handlers
    static int dbus_open_uri(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_open_file(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);

    static const sd_bus_vtable vtable[];

private:
    PortalBackend& backend_;
    mutable std::mutex cb_mutex_;
    OpenUriCallback open_uri_callback_;
    OpenFileCallback open_file_callback_;
};

} // namespace broportal
