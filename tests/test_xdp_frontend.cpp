// The real xdg-desktop-portal frontend in front of this backend, on a private
// bus: x-d-p discovers the backend from a bro.portal file and portals.conf,
// and an application talking only to org.freedesktop.portal.Desktop gets the
// backend's Settings and FileChooser answers through it. This is the
// interoperability oracle: the frontend, not this library, decides whether
// the backend's signatures and replies are acceptable. Skips when
// xdg-desktop-portal is not installed. Linux.
#include "check.h"
#include "fixture.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace broportal;
namespace fs = std::filesystem;

namespace {

std::string find_xdp() {
    if (const char* env = std::getenv("BROPORTAL_XDP")) return env;
    for (const char* p : {"/usr/libexec/xdg-desktop-portal", "/usr/lib/xdg-desktop-portal",
                          "/usr/lib/x86_64-linux-gnu/xdg-desktop-portal"}) {
        if (access(p, X_OK) == 0) return p;
    }
    return {};
}

struct Frontend {
    pid_t pid = -1;
    ~Frontend() {
        if (pid > 0) {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }
    }
};

bool name_has_owner(dbus::Bus& bus, const std::string& name) {
    bool owned = false;
    bus.call_method("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                    [&](dbus::Message& m) { m.append_string(name); },
                    [&](dbus::Message& r) {
                        int v = 0;
                        sd_bus_message_read_basic(r.raw(), 'b', &v);
                        owned = v != 0;
                    });
    return owned;
}

}  // namespace

int main() {
    const std::string xdp = find_xdp();
    if (xdp.empty()) bstest::skip("test_xdp_frontend", "xdg-desktop-portal is not installed");
    if (!bstest::have_gdbus()) bstest::skip("test_xdp_frontend", "gdbus (the outside client) is not installed");

    bstest::PortalFixture f("test_xdp_frontend", false);
    f.backend->settings().set_color_scheme(2);
    f.backend->settings().set_accent_color(RgbColor{1.0, 0.5, 0.0});
    std::string picked_title;
    f.backend->file_chooser().set_file_picker_callback(
        [&](const ObjectPath&, const std::string&, const std::string& title, const FileChooserOptions&,
            std::vector<std::string>& uris, VariantMap&) {
            picked_title = title;
            uris.push_back("file:///home/user/picked.txt");
            return ResponseCode::Success;
        });
    f.start();

    // The portal description and configuration, in a scratch directory.
    char tmpl[] = "/tmp/broportal-xdp-XXXXXX";
    REQUIRE(mkdtemp(tmpl) != nullptr);
    const fs::path root = tmpl;
    fs::create_directories(root / "portals");
    fs::create_directories(root / "config" / "xdg-desktop-portal");
    {
        std::ofstream p(root / "portals" / "bro.portal");
        p << "[portal]\nDBusName=" << f.config.bus_name
          << "\nInterfaces=org.freedesktop.impl.portal.Settings;org.freedesktop.impl.portal.FileChooser;\n"
             "UseIn=bro\n";
        // x-d-p 1.18 reads portals.conf beside the portal files when
        // XDG_DESKTOP_PORTAL_DIR is set; later versions read the config dirs.
        // Settings ignores the deprecated UseIn fallback, so both are needed.
        for (const fs::path& dir : {root / "config" / "xdg-desktop-portal", root / "portals"}) {
            std::ofstream c(dir / "bro-portals.conf");
            c << "[preferred]\ndefault=bro\n";
        }
    }
    const std::string log = (root / "xdp.log").string();

    Frontend fe;
    fe.pid = fork();
    if (fe.pid == 0) {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        setenv("DBUS_SESSION_BUS_ADDRESS", f.bus.address.c_str(), 1);
        setenv("XDG_CURRENT_DESKTOP", "bro", 1);
        setenv("XDG_DESKTOP_PORTAL_DIR", (root / "portals").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (root / "config").c_str(), 1);
        setenv("XDG_CONFIG_DIRS", (root / "config").c_str(), 1);
        FILE* out = std::fopen(log.c_str(), "w");
        if (out) {
            dup2(fileno(out), STDOUT_FILENO);
            dup2(fileno(out), STDERR_FILENO);
        }
        execl(xdp.c_str(), xdp.c_str(), "--verbose", static_cast<char*>(nullptr));
        _exit(127);
    }
    REQUIRE(fe.pid > 0);

    auto dump_log = [&] {
        std::ifstream in(log);
        std::string line;
        int n = 0;
        while (std::getline(in, line) && n++ < 80) std::fprintf(stderr, "  xdp| %s\n", line.c_str());
    };

    bool up = bstest::wait_until([&] { return name_has_owner(*f.client, "org.freedesktop.portal.Desktop"); },
                                 std::chrono::seconds(20), std::chrono::milliseconds(100));
    if (!up) {
        dump_log();
        bstest::fail(__FILE__, __LINE__, "xdg-desktop-portal never owned org.freedesktop.portal.Desktop");
        fs::remove_all(root);
        return bstest::finish("test_xdp_frontend");
    }

    auto gdbus = [&](const std::string& method, const std::string& args) {
        std::string cmd = "gdbus call --address '" + f.bus.address +
                          "' --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop"
                          " --method " + method + " " + args + " 2>&1";
        std::string out;
        FILE* p = popen(cmd.c_str(), "r");
        if (!p) return out;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
        return out;
    };

    // Settings: the frontend reads the backend's values.
    std::string scheme = gdbus("org.freedesktop.portal.Settings.ReadOne", "org.freedesktop.appearance color-scheme");
    CHECK(scheme.find("uint32 2") != std::string::npos);
    std::string accent = gdbus("org.freedesktop.portal.Settings.ReadOne", "org.freedesktop.appearance accent-color");
    CHECK(accent.find("(1.0, 0.5, 0.0)") != std::string::npos);
    if (scheme.find("uint32 2") == std::string::npos) {
        std::fprintf(stderr, "ReadOne answered: %s\n", scheme.c_str());
        dump_log();
    }

    // FileChooser: a portal request answered through the backend's picker.
    const char* unique = nullptr;
    REQUIRE(sd_bus_get_unique_name(f.client->raw(), &unique) >= 0);
    std::string sender = unique + 1;
    std::replace(sender.begin(), sender.end(), '.', '_');
    const std::string request_path = "/org/freedesktop/portal/desktop/request/" + sender + "/bro1";

    bool responded = false;
    uint32_t code = 999;
    VariantMap results;
    auto slot = f.client->add_match("type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" +
                                        request_path + "'",
                                    [&](dbus::Message& m) {
                                        responded = true;
                                        m.read_uint32(&code);
                                        m.read_variant_map(&results);
                                    });
    REQUIRE(slot.is_valid());
    std::string handle, err;
    CHECK(f.client->call_method(
        "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.FileChooser",
        "OpenFile",
        [](dbus::Message& m) {
            m.append_string("");
            m.append_string("Pick a file");
            VariantMap opts;
            opts["handle_token"] = Variant(std::string("bro1"));
            m.append_variant_map(opts);
        },
        [&](dbus::Message& r) {
            ObjectPath o;
            r.read_object_path(&o);
            handle = o.path;
        },
        &err, 20000000));
    if (!err.empty()) std::fprintf(stderr, "OpenFile: %s\n", err.c_str());
    CHECK_EQ(handle, request_path);
    for (int i = 0; i < 200 && !responded; ++i) {
        f.client->wait(50000);
        while (f.client->process() > 0) {
        }
    }
    CHECK(responded);
    CHECK_EQ(code, 0u);
    CHECK_EQ(picked_title, std::string("Pick a file"));
    auto it = results.find("uris");
    REQUIRE(it != results.end());
    const auto* uris = it->second.get_if<std::vector<std::string>>();
    REQUIRE(uris != nullptr);
    REQUIRE(uris->size() == 1);
    CHECK_EQ(uris->front(), std::string("file:///home/user/picked.txt"));
    if (!responded || code != 0) dump_log();

    fs::remove_all(root);
    return bstest::finish("test_xdp_frontend");
}
