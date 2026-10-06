#include "broportal/filechooser.h"
#include "broportal/backend.h"

#include <filesystem>
#include <iostream>

namespace broportal {

namespace fs = std::filesystem;

const sd_bus_vtable FileChooserInterface::vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", &FileChooserInterface::dbus_open_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFile", "osssa{sv}", "ua{sv}", &FileChooserInterface::dbus_save_file, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SaveFiles", "osssa{sv}", "ua{sv}", &FileChooserInterface::dbus_save_files, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

FileChooserInterface::FileChooserInterface(PortalBackend& backend)
    : backend_(backend) {}

FileChooserInterface::~FileChooserInterface() = default;

std::string FileChooserInterface::normalize_file_uri(const std::string& path_or_uri) {
    if (path_or_uri.starts_with("file://")) {
        return path_or_uri;
    }
    std::string path;
    try {
        path = fs::absolute(path_or_uri).string();
    } catch (...) {
        path = path_or_uri;
    }
    // RFC 3986: a path may carry unreserved characters, '/' and a few
    // sub-delimiters as they are; everything else (spaces, '%', '#', '?',
    // non-ASCII bytes) is percent-encoded.
    static const char* kHex = "0123456789ABCDEF";
    std::string uri = "file://";
    for (unsigned char c : path) {
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '.' || c == '_' || c == '~' || c == '/' || c == '!' || c == '$' || c == '&' ||
                    c == '\'' || c == '(' || c == ')' || c == '*' || c == '+' || c == ',' || c == ';' ||
                    c == '=' || c == ':' || c == '@';
        if (keep) {
            uri.push_back(static_cast<char>(c));
        } else {
            uri.push_back('%');
            uri.push_back(kHex[c >> 4]);
            uri.push_back(kHex[c & 15]);
        }
    }
    return uri;
}

FileChooserOptions FileChooserInterface::parse_options(const VariantMap& options) {
    FileChooserOptions opts;
    opts.accept_label = get_string(options, "accept_label").value_or("");
    opts.modal = get_bool_or(options, "modal", true);
    opts.multiple = get_bool_or(options, "multiple", false);
    opts.directory = get_bool_or(options, "directory", false);
    opts.current_name = get_string(options, "current_name").value_or("");
    opts.extra_options = options;

    if (auto it = options.find("current_folder"); it != options.end()) {
        if (const auto* bytes = it->second.get_if<std::vector<uint8_t>>()) {
            opts.current_folder = std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            // Strip null terminator if present
            if (!opts.current_folder.empty() && opts.current_folder.back() == '\0') {
                opts.current_folder.pop_back();
            }
        }
    }

    if (auto it = options.find("current_file"); it != options.end()) {
        if (const auto* bytes = it->second.get_if<std::vector<uint8_t>>()) {
            opts.current_file = std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
            if (!opts.current_file.empty() && opts.current_file.back() == '\0') {
                opts.current_file.pop_back();
            }
        }
    }

    return opts;
}

ResponseCode FileChooserInterface::open_file(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const std::string& title,
    const VariantMap& options,
    VariantMap& results) {
    FileChooserOptions opts = parse_options(options);
    std::vector<std::string> selected_uris;

    FilePickerCallback cb;
    std::vector<std::string> def_uris;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = callback_;
        def_uris = default_uris_;
    }

    ResponseCode code = ResponseCode::Success;
    if (cb) {
        code = cb(handle, app_id, title, opts, selected_uris, results);
    } else if (!def_uris.empty()) {
        // The host preselected the answer (set_default_selected_files).
        for (const auto& u : def_uris) {
            selected_uris.push_back(normalize_file_uri(u));
        }
        results["uris"] = Variant(selected_uris);
        results["writable"] = Variant(false);
    } else {
        // No picker and no preselection: nobody chose anything, so say so
        // rather than inventing a file.
        return ResponseCode::OtherError;
    }

    if (code == ResponseCode::Success && !results.contains("uris")) {
        std::vector<std::string> norm;
        for (const auto& u : selected_uris) {
            norm.push_back(normalize_file_uri(u));
        }
        results["uris"] = Variant(norm);
    }

    return code;
}

ResponseCode FileChooserInterface::save_file(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const std::string& title,
    const VariantMap& options,
    VariantMap& results) {
    FileChooserOptions opts = parse_options(options);
    std::vector<std::string> selected_uris;

    FilePickerCallback cb;
    std::vector<std::string> def_uris;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = callback_;
        def_uris = default_uris_;
    }

    ResponseCode code = ResponseCode::Success;
    if (cb) {
        code = cb(handle, app_id, title, opts, selected_uris, results);
    } else if (!def_uris.empty()) {
        // The host preselected the answer (set_default_selected_files).
        selected_uris.push_back(normalize_file_uri(def_uris.front()));
        results["uris"] = Variant(selected_uris);
    } else {
        // No picker and no preselection: nobody chose a destination.
        return ResponseCode::OtherError;
    }

    if (code == ResponseCode::Success && !results.contains("uris")) {
        std::vector<std::string> norm;
        for (const auto& u : selected_uris) {
            norm.push_back(normalize_file_uri(u));
        }
        results["uris"] = Variant(norm);
    }

    return code;
}

ResponseCode FileChooserInterface::save_files(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& /*parent_window*/,
    const std::string& title,
    const VariantMap& options,
    VariantMap& results) {
    return save_file(handle, app_id, "", title, options, results);
}

int FileChooserInterface::dbus_open_file(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<FileChooserInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    std::string title;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_string(&title) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, request, handle, app_id, parent_window, title, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->open_file(handle, app_id, parent_window, title, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (request) {
            request->close();
        }
    });

    return 1;
}

int FileChooserInterface::dbus_save_file(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<FileChooserInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    std::string title;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_string(&title) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, request, handle, app_id, parent_window, title, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->save_file(handle, app_id, parent_window, title, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (request) {
            request->close();
        }
    });

    return 1;
}

int FileChooserInterface::dbus_save_files(sd_bus_message* m, void* userdata, sd_bus_error* /*ret_error*/) {
    auto* self = static_cast<FileChooserInterface*>(userdata);
    dbus::Message msg(m, false);

    ObjectPath handle;
    std::string app_id;
    std::string parent_window;
    std::string title;
    VariantMap options;

    if (!msg.read_object_path(&handle) ||
        !msg.read_string(&app_id) ||
        !msg.read_string(&parent_window) ||
        !msg.read_string(&title) ||
        !msg.read_variant_map(&options)) {
        return sd_bus_reply_method_errorf(m, SD_BUS_ERROR_INVALID_ARGS, "Invalid parameters");
    }

    auto request = self->backend_.create_request(handle, app_id);
    sd_bus_message_ref(m);

    self->backend_.post_worker([self, m, request, handle, app_id, parent_window, title, options = std::move(options)]() {
        VariantMap results;
        ResponseCode code = self->save_files(handle, app_id, parent_window, title, options, results);

        self->backend_.send_method_reply_and_unref(m, [code, &results](dbus::Message& reply_msg) {
            reply_msg.append_uint32(static_cast<uint32_t>(code));
            reply_msg.append_variant_map(results);
        });

        if (request) {
            request->close();
        }
    });

    return 1;
}

} // namespace broportal
