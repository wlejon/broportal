#pragma once

#include "broportal/dbus_helpers.h"
#include "broportal/types.h"

#include <functional>
#include <string>
#include <vector>

#include <mutex>

namespace broportal {

class PortalBackend;

struct FileChooserOptions {
    std::string accept_label;
    bool modal = true;
    bool multiple = false;
    bool directory = false;
    std::string current_name;
    std::string current_folder;
    std::string current_file;
    VariantMap extra_options;
};

using FilePickerCallback = std::function<ResponseCode(
    const ObjectPath& handle,
    const std::string& app_id,
    const std::string& title,
    const FileChooserOptions& options,
    std::vector<std::string>& out_uris,
    VariantMap& out_results)>;

class FileChooserInterface {
public:
    explicit FileChooserInterface(PortalBackend& backend);
    ~FileChooserInterface();

    void set_file_picker_callback(FilePickerCallback callback) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        callback_ = std::move(callback);
    }

    void set_default_selected_files(std::vector<std::string> uris) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        default_uris_ = std::move(uris);
    }

    // Direct C++ methods
    ResponseCode open_file(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        const std::string& title,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode save_file(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        const std::string& title,
        const VariantMap& options,
        VariantMap& results);

    ResponseCode save_files(
        const ObjectPath& handle,
        const std::string& app_id,
        const std::string& parent_window,
        const std::string& title,
        const VariantMap& options,
        VariantMap& results);

    // D-Bus method handlers
    static int dbus_open_file(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_save_file(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);
    static int dbus_save_files(sd_bus_message* m, void* userdata, sd_bus_error* ret_error);

    static const sd_bus_vtable vtable[];

private:
    PortalBackend& backend_;
    mutable std::mutex cb_mutex_;
    FilePickerCallback callback_;
    std::vector<std::string> default_uris_;

    static std::string normalize_file_uri(const std::string& path_or_uri);
    FileChooserOptions parse_options(const VariantMap& options);
};

} // namespace broportal
