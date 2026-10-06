// org.freedesktop.impl.portal.GlobalShortcuts on a private bus, driven by
// gdbus with one thread dispatching the backend: binding fails with no
// compositor to grab keys; with one, its bound triggers come back, are
// listed, announced by ShortcutsChanged, and Activated/Deactivated reach a
// listener; ConfigureShortcuts is NotSupported until the host offers a UI.
// Linux.
#include "check.h"
#include "fixture.h"

#include <vector>

using namespace broportal;

int main() {
    bstest::PortalFixture f("test_globalshortcuts", false);
    if (!bstest::have_gdbus()) bstest::skip("test_globalshortcuts", "gdbus (the outside client) is not installed");
    std::string err;
    REQUIRE(f.backend->start(&err));
    auto& gs = f.backend->global_shortcuts();

    auto gdbus = [&](const std::string& method, const std::string& args) {
        return bstest::run_while_dispatching(
            f.backend->bus(), "gdbus call --address '" + f.bus.address + "' --dest " + f.config.bus_name +
                                  " --object-path " + f.config.object_path +
                                  " --method org.freedesktop.impl.portal.GlobalShortcuts." + method + " " + args);
    };
    const std::string req = "/org/freedesktop/portal/desktop/request/gs_req_";
    const std::string sess = "/org/freedesktop/portal/desktop/session/gs_sess_1";
    const std::string wanted =
        "\"[('mute_mic', {'description': <'Mute Microphone'>, 'preferred_trigger': <'F9'>}), "
        "('push_to_talk', {'description': <'Push to talk'>})]\"";

    auto created = gdbus("CreateSession", req + "1 " + sess + " org.example.App '{}'");
    CHECK_EQ(created.status, 0);
    CHECK(created.out.find("uint32 0") != std::string::npos);
    CHECK(created.out.find("'session_id'") != std::string::npos);

    // No compositor: nothing is bound.
    auto unbound = gdbus("BindShortcuts", req + "2 " + sess + " " + wanted + " '' '{}'");
    CHECK_EQ(unbound.status, 0);
    CHECK(unbound.out.find("uint32 2") != std::string::npos);

    // The compositor grabs only mute_mic, on Ctrl+Alt+M.
    std::string seen_app;
    size_t seen_count = 0;
    gs.set_bind_shortcuts_callback([&](const ObjectPath&, const ObjectPath&, const std::string& app_id,
                                       const ShortcutList& in, ShortcutList& out, VariantMap&) {
        seen_app = app_id;
        seen_count = in.size();
        for (const auto& [id, props] : in) {
            if (id != "mute_mic") continue;
            VariantMap p;
            p["description"] = props.at("description");
            p["trigger_description"] = Variant(std::string("Ctrl+Alt+M"));
            out.emplace_back(id, p);
        }
        return ResponseCode::Success;
    });

    std::vector<std::string> signals;
    auto slot = f.client->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.GlobalShortcuts'", [&](dbus::Message& m) {
            ObjectPath p;
            m.read_object_path(&p);
            std::string member = sd_bus_message_get_member(m.raw());
            if (member == "ShortcutsChanged") {
                ShortcutList l;
                m.read_shortcut_list(&l);
                signals.push_back(member + ":" + p.path + ":" + std::to_string(l.size()));
            } else {
                std::string id;
                uint64_t ts = 0;
                m.read_string(&id);
                m.read_uint64(&ts);
                signals.push_back(member + ":" + id + ":" + std::to_string(ts));
            }
        });
    REQUIRE(slot.is_valid());
    auto pump = [&](size_t want) {
        for (int i = 0; i < 100 && signals.size() < want; ++i) {
            while (f.backend->bus().process() > 0) {
            }
            f.client->wait(20000);
            while (f.client->process() > 0) {
            }
        }
    };

    auto bound = gdbus("BindShortcuts", req + "3 " + sess + " " + wanted + " '' '{}'");
    CHECK_EQ(bound.status, 0);
    CHECK(bound.out.find("uint32 0") != std::string::npos);
    CHECK(bound.out.find("'trigger_description': <'Ctrl+Alt+M'>") != std::string::npos);
    CHECK(bound.out.find("push_to_talk") == std::string::npos);
    CHECK_EQ(seen_app, std::string("org.example.App"));
    CHECK_EQ(seen_count, 2u);
    pump(1);
    REQUIRE(signals.size() >= 1);
    CHECK_EQ(signals[0], "ShortcutsChanged:" + sess + ":1");

    auto listed = gdbus("ListShortcuts", req + "4 " + sess);
    CHECK_EQ(listed.status, 0);
    CHECK(listed.out.find("'mute_mic'") != std::string::npos);
    CHECK(listed.out.find("Ctrl+Alt+M") != std::string::npos);

    CHECK(gs.activate_shortcut(ObjectPath{sess}, "mute_mic", 123456789));
    CHECK(gs.deactivate_shortcut(ObjectPath{sess}, "mute_mic", 123456999));
    pump(3);
    REQUIRE(signals.size() == 3);
    CHECK_EQ(signals[1], std::string("Activated:mute_mic:123456789"));
    CHECK_EQ(signals[2], std::string("Deactivated:mute_mic:123456999"));

    // ConfigureShortcuts.
    auto noui = gdbus("ConfigureShortcuts", sess + " '' '{}'");
    CHECK(noui.status != 0);
    CHECK(noui.out.find("NotSupported") != std::string::npos);
    std::string configured;
    gs.set_configure_callback([&](const ObjectPath& s, const std::string&, const VariantMap&) { configured = s.path; });
    CHECK_EQ(gdbus("ConfigureShortcuts", sess + " '' '{}'").status, 0);
    CHECK_EQ(configured, sess);

    // Unknown sessions.
    auto nosess = gdbus("ListShortcuts", req + "5 /org/freedesktop/portal/desktop/session/none");
    CHECK(nosess.out.find("uint32 2") != std::string::npos);

    return bstest::finish("test_globalshortcuts");
}
