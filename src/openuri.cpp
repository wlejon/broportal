#include "broportal/openuri.h"
#include "broportal/backend.h"

namespace broportal {

const sd_bus_vtable OpenURIInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenURI", "osssa{sv}", "ua{sv}", &OpenURIInterface::dbus_open_uri, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("OpenFile", "ossha{sv}", "ua{sv}", &OpenURIInterface::dbus_open_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

OpenURIInterface::OpenURIInterface(PortalBackend& backend)
    : backend_(backend) {}

OpenURIInterface::~OpenURIInterface() = default;

ResponseCode OpenURIInterface::open_uri(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const std::string& uri,
    const VariantMap& options,
    VariantMap& results) {
    OpenUriCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = open_uri_callback_;
    }

    if (cb) {
        return cb(handle, app_id, uri, options, results);
    }

    // Nothing opened the URI: no host handler is set.
    return ResponseCode::OtherError;
}

ResponseCode OpenURIInterface::open_file(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    int fd,
    const VariantMap& options,
    VariantMap& results) {
    OpenFileCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = open_file_callback_;
    }

    if (cb) {
        return cb(handle, app_id, fd, options, results);
    }

    // Nothing opened the file: no host handler is set.
    (void)fd;
    return ResponseCode::OtherError;
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
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, req, handle, app_id, parent_window, uri, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->open_uri(handle, app_id, parent_window, uri, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (req) req->close();
    });

    return 1;
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
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, req, handle, app_id, parent_window, fd = ufd.fd, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->open_file(handle, app_id, parent_window, fd, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (req) req->close();
    });

    return 1;
}

} // namespace broportal
