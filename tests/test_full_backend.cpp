// The whole backend on a private bus: it owns its well-known name, its
// object introspects with all eight impl interfaces and their version
// properties, the Request/Session registry tracks objects, and stop()
// releases the name. Linux.
#include "check.h"
#include "fixture.h"
#include "broportal/availability.h"

using namespace broportal;

namespace {

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

uint32_t version_of(bstest::PortalFixture& f, const std::string& iface) {
    uint32_t v = 0;
    f.call("org.freedesktop.DBus.Properties", "Get",
           [&](dbus::Message& m) {
               m.append_string(iface);
               m.append_string("version");
           },
           [&](dbus::Message& r) {
               Variant var;
               if (r.read_variant(&var)) v = var.get_value_or<uint32_t>(0);
           });
    return v;
}

}  // namespace

int main() {
    std::string why = "unset";
    CHECK(available(&why));
    CHECK_EQ(why, std::string("unset"));

    bstest::PortalFixture f("test_full_backend");
    CHECK(f.backend->is_running());
    CHECK(name_has_owner(*f.client, f.config.bus_name));

    std::string xml;
    CHECK(f.call("org.freedesktop.DBus.Introspectable", "Introspect", nullptr,
                 [&](dbus::Message& r) { r.read_string(&xml); }));
    for (const char* iface : {"FileChooser", "Screenshot", "ScreenCast", "RemoteDesktop", "Settings", "Inhibit",
                              "OpenURI", "GlobalShortcuts"}) {
        std::string full = std::string("org.freedesktop.impl.portal.") + iface;
        if (xml.find("\"" + full + "\"") == std::string::npos) {
            bstest::fail(__FILE__, __LINE__, "Introspect lacks " + full);
        }
    }

    // The interfaces that carry a version property answer it.
    CHECK_EQ(version_of(f, "org.freedesktop.impl.portal.ScreenCast"), 6u);
    CHECK_EQ(version_of(f, "org.freedesktop.impl.portal.RemoteDesktop"), 2u);
    CHECK_EQ(version_of(f, "org.freedesktop.impl.portal.Settings"), 1u);
    CHECK_EQ(version_of(f, "org.freedesktop.impl.portal.GlobalShortcuts"), 2u);

    if (bstest::have_gdbus()) {
        std::string out = f.gdbus("org.freedesktop.DBus.Properties.Get",
                                  "org.freedesktop.impl.portal.ScreenCast AvailableSourceTypes");
        CHECK(out.find("<uint32 7>") != std::string::npos);
    } else {
        std::printf("Note: gdbus is not installed; the outside-client call did not run\n");
    }

    // The registry.
    ObjectPath req_path{"/org/freedesktop/portal/desktop/request/full_test_req"};
    auto req = f.backend->create_request(req_path, "org.test.App");
    REQUIRE(req != nullptr);
    CHECK(f.backend->get_request(req_path) == req);
    f.backend->remove_request(req_path);
    CHECK(f.backend->get_request(req_path) == nullptr);
    CHECK(req->is_closed());

    ObjectPath sess_path{"/org/freedesktop/portal/desktop/session/full_test_sess"};
    auto sess = f.backend->create_session(sess_path, "org.test.App", SessionType::Custom);
    REQUIRE(sess != nullptr);
    CHECK(f.backend->get_session(sess_path) == sess);
    f.backend->remove_session(sess_path);
    CHECK(f.backend->get_session(sess_path) == nullptr);

    f.backend->stop_background();
    f.backend->stop();
    CHECK(!f.backend->is_running());
    CHECK(bstest::wait_until([&] { return !name_has_owner(*f.client, f.config.bus_name); },
                             std::chrono::seconds(5)));

    return bstest::finish("test_full_backend");
}
