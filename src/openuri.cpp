#include "broportal/openuri.h"
#include "broportal/backend.h"

#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <unistd.h>

namespace broportal {

namespace fs = std::filesystem;

const sd_bus_vtable OpenURIInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenURI", "osssa{sv}", "ua{sv}", &OpenURIInterface::dbus_open_uri, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OpenFile", "ossha{sv}", "ua{sv}", &OpenURIInterface::dbus_open_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

OpenURIInterface::OpenURIInterface(PortalBackend& backend)
    : backend_(backend) {}

OpenURIInterface::~OpenURIInterface() = default;

bool OpenURIInterface::is_scheme_supported(const std::string& uri) {
    if (uri.starts_with("http://") ||
        uri.starts_with("https://") ||
        uri.starts_with("mailto:") ||
        uri.starts_with("file://") ||
        uri.starts_with("geo:") ||
        uri.starts_with("tel:")) {
        return true;
    }
    return false;
}

ResponseCode OpenURIInterface::open_uri(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const std::string& uri,
    const VariantMap& options,
    VariantMap& results) {
    if (open_uri_callback_) {
        return open_uri_callback_(handle, app_id, uri, options, results);
    }

    if (uri.empty() || !is_scheme_supported(uri)) {
        return ResponseCode::OtherError;
    }

    // Default handling success
    return ResponseCode::Success;
}

ResponseCode OpenURIInterface::open_file(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    int fd,
    const VariantMap& options,
    VariantMap& results) {
    if (open_file_callback_) {
        return open_file_callback_(handle, app_id, fd, options, results);
    }

    if (fd < 0) {
        return ResponseCode::OtherError;
    }

    // Check if fd is valid
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return ResponseCode::OtherError;
    }

    return ResponseCode::Success;
}

int OpenURIInterface::dbus_open_uri(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<OpenURIInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    std::string uri;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_string(&uri) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, app_id);
    VariantMap results;
    ResponseCode code = self->open_uri(handle, app_id, parent_window, uri, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

int OpenURIInterface::dbus_open_file(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<OpenURIInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    UnixFd ufd;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_unix_fd(&ufd) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto req = self->backend_.create_request(handle, app_id);
    VariantMap results;
    ResponseCode code = self->open_file(handle, app_id, parent_window, ufd.fd, options, results);

    sd_bus_message* reply = nullptr;
    int r = sd_bus_message_new_method_return(m, &reply);
    if (r < 0) return r;

    dbus::Message reply_msg(reply, true);
    reply_msg.append_uint32(static_cast<uint32_t>(code));
    reply_msg.append_variant_map(results);

    if (req) req->close();
    return sd_bus_send(self->backend_.bus().raw(), reply_msg.raw(), nullptr);
}

} // namespace broportal
