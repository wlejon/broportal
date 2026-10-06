// org.freedesktop.impl.portal.OpenURI (bro's own interface; the
// xdg-desktop-portal spec has no backend OpenURI) on a private bus: with no
// host handler both methods answer an error; with one, the URI reaches the
// host and OpenFile hands over a descriptor for the very file the caller
// opened (same inode, readable contents). Linux.
#include "check.h"
#include "fixture.h"

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace broportal;

namespace {

constexpr const char* kIface = "org.freedesktop.impl.portal.OpenURI";

std::function<void(dbus::Message&)> uri_args(const std::string& req, const std::string& uri) {
    return [=](dbus::Message& m) {
        m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/" + req});
        m.append_string("org.example.App");
        m.append_string("");
        m.append_string(uri);
        m.append_variant_map({});
    };
}

std::function<void(dbus::Message&)> file_args(const std::string& req, int fd, VariantMap opts) {
    return [=](dbus::Message& m) {
        m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/" + req});
        m.append_string("org.example.App");
        m.append_string("");
        m.append_unix_fd(UnixFd{fd});
        m.append_variant_map(opts);
    };
}

}  // namespace

int main() {
    bstest::PortalFixture f("test_openuri");

    // Nothing to open with: errors, not a pretended success.
    CHECK_EQ(f.request(kIface, "OpenURI", uri_args("u1", "https://example.com/")).code, 2u);

    char path[] = "/tmp/broportal-openuri-XXXXXX";
    int fd = mkstemp(path);
    REQUIRE(fd >= 0);
    const std::string payload = "broportal open-file payload\n";
    REQUIRE(write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size()));
    struct stat st{};
    REQUIRE(fstat(fd, &st) == 0);

    CHECK_EQ(f.request(kIface, "OpenFile", file_args("f1", fd, {})).code, 2u);

    std::string seen_uri;
    f.backend->open_uri().set_open_uri_callback(
        [&](const ObjectPath& handle, const std::string& app_id, const std::string& uri, const VariantMap&,
            VariantMap&) {
            if (app_id != "org.example.App" || handle.path.find("/request/u") == std::string::npos)
                return ResponseCode::OtherError;
            seen_uri = uri;
            return uri.starts_with("forbidden:") ? ResponseCode::Cancelled : ResponseCode::Success;
        });

    CHECK_EQ(f.request(kIface, "OpenURI", uri_args("u2", "https://example.com/portal?q=a%20b")).code, 0u);
    CHECK_EQ(seen_uri, std::string("https://example.com/portal?q=a%20b"));
    CHECK_EQ(f.request(kIface, "OpenURI", uri_args("u3", "forbidden:thing")).code, 1u);

    bool same_inode = false;
    std::string contents;
    bool writable = false;
    f.backend->open_uri().set_open_file_callback(
        [&](const ObjectPath&, const std::string&, int got, const VariantMap& opts, VariantMap&) {
            struct stat gst{};
            if (fstat(got, &gst) != 0) return ResponseCode::OtherError;
            same_inode = gst.st_ino == st.st_ino && gst.st_dev == st.st_dev;
            char buf[128] = {};
            ssize_t n = pread(got, buf, sizeof buf, 0);
            if (n > 0) contents.assign(buf, static_cast<size_t>(n));
            writable = get_bool_or(opts, "writable", false);
            return ResponseCode::Success;
        });

    VariantMap opts;
    opts["writable"] = Variant(true);
    CHECK_EQ(f.request(kIface, "OpenFile", file_args("f2", fd, opts)).code, 0u);
    CHECK(same_inode);
    CHECK_EQ(contents, payload);
    CHECK(writable);

    // OpenURI from an independent client.
    if (bstest::have_gdbus()) {
        seen_uri.clear();
        std::string out = f.gdbus(std::string(kIface) + ".OpenURI",
                                  "/org/freedesktop/portal/desktop/request/u4 org.example.App '' "
                                  "'mailto:someone@example.com' '{}'");
        CHECK(out.find("uint32 0") != std::string::npos);
        CHECK_EQ(seen_uri, std::string("mailto:someone@example.com"));
    } else {
        std::printf("Note: gdbus is not installed; the outside-client call did not run\n");
    }

    close(fd);
    unlink(path);
    return bstest::finish("test_openuri");
}
