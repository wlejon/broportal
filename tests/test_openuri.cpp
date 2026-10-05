#include "broportal/backend.h"

#include <cassert>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

int main() {
    using namespace broportal;

    std::string err;
    BackendConfig config;
    config.bus_name = "org.freedesktop.impl.portal.desktop.bro.test_openuri";
    config.object_path = "/org/freedesktop/portal/desktop";

    auto backend = PortalBackend::create_on_user_bus(config, &err);
    if (!backend) {
        std::cout << "Skipping test_openuri: user bus not available: " << err << "\n";
        return 77;
    }

    bool uri_opened = false;
    std::string opened_uri_str;
    backend->open_uri().set_open_uri_callback(
        [&uri_opened, &opened_uri_str](
            const ObjectPath&,
            const std::string&,
            const std::string& uri,
            const VariantMap&,
            VariantMap&) {
            uri_opened = true;
            opened_uri_str = uri;
            return ResponseCode::Success;
        });

    bool file_opened = false;
    int opened_fd_val = -1;
    backend->open_uri().set_open_file_callback(
        [&file_opened, &opened_fd_val](
            const ObjectPath&,
            const std::string&,
            int fd,
            const VariantMap&,
            VariantMap&) {
            file_opened = true;
            opened_fd_val = fd;
            return ResponseCode::Success;
        });

    bool ok = backend->start(&err);
    assert(ok);
    assert(backend->run_in_background());

    auto client_bus = dbus::Bus::open_user(&err);
    assert(client_bus);

    // 1. Call OpenURI
    uint32_t resp_code = 999;
    VariantMap results;

    bool called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.OpenURI",
        "OpenURI",
        [](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/openuri_req_1"});
            msg.append_string("org.example.App");
            msg.append_string("");
            msg.append_string("https://example.com/portal");
            VariantMap opts;
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    assert(called);
    assert(resp_code == 0);
    assert(uri_opened);
    assert(opened_uri_str == "https://example.com/portal");

    // 2. Call OpenFile with real Unix file descriptor
    int test_fd = open("/dev/null", O_RDONLY);
    assert(test_fd >= 0);

    resp_code = 999;
    results.clear();

    called = client_bus->call_method(
        config.bus_name,
        config.object_path,
        "org.freedesktop.impl.portal.OpenURI",
        "OpenFile",
        [test_fd](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/openuri_req_2"});
            msg.append_string("org.example.App");
            msg.append_string("");
            msg.append_unix_fd(UnixFd{test_fd});
            VariantMap opts;
            msg.append_variant_map(opts);
        },
        [&resp_code, &results](dbus::Message& reply) {
            reply.read_uint32(&resp_code);
            reply.read_variant_map(&results);
        },
        &err);

    close(test_fd);

    assert(called);
    assert(resp_code == 0);
    assert(file_opened);
    assert(opened_fd_val >= 0);

    backend->stop();
    std::cout << "test_openuri PASSED\n";
    return 0;
}
