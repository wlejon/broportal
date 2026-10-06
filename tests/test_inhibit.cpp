// org.freedesktop.impl.portal.Inhibit on a private bus, driven by gdbus with
// one thread dispatching the backend: Inhibit records the flags and reason
// and tells the host, Request.Close lifts it, a monitor session receives
// StateChanged and its QueryEndResponse reaches the host, and closing the
// monitor stops both. Linux.
#include "check.h"
#include "fixture.h"

#include <vector>

using namespace broportal;

int main() {
    bstest::PortalFixture f("test_inhibit", false);
    if (!bstest::have_gdbus()) bstest::skip("test_inhibit", "gdbus (the outside client) is not installed");
    std::string err;
    REQUIRE(f.backend->start(&err));
    auto& inh = f.backend->inhibit();

    auto gdbus = [&](const std::string& path, const std::string& method, const std::string& args) {
        return bstest::run_while_dispatching(f.backend->bus(), "gdbus call --address '" + f.bus.address +
                                                                   "' --dest " + f.config.bus_name +
                                                                   " --object-path " + path + " --method " +
                                                                   method + " " + args);
    };
    const std::string obj = f.config.object_path;

    std::vector<std::pair<bool, InhibitEntry>> changes;
    inh.set_inhibit_change_listener([&](bool added, const InhibitEntry& e) { changes.emplace_back(added, e); });

    const std::string req = "/org/freedesktop/portal/desktop/request/inhibit_req_1";
    auto r = gdbus(obj, "org.freedesktop.impl.portal.Inhibit.Inhibit",
                   req + " org.example.App main_window 12 \"{'reason': <'Playing a video'>}\"");
    CHECK_EQ(r.status, 0);
    CHECK(inh.is_inhibited(InhibitFlag::Idle));
    CHECK(inh.is_inhibited(InhibitFlag::Suspend));
    CHECK(!inh.is_inhibited(InhibitFlag::Logout));
    auto active = inh.active_inhibitions();
    REQUIRE(active.size() == 1);
    CHECK_EQ(active.front().reason, std::string("Playing a video"));
    CHECK_EQ(active.front().app_id, std::string("org.example.App"));
    CHECK_EQ(active.front().window, std::string("main_window"));
    REQUIRE(changes.size() == 1);
    CHECK(changes[0].first);

    // A second inhibitor, then the first is withdrawn: only its flags lift.
    const std::string req2 = "/org/freedesktop/portal/desktop/request/inhibit_req_2";
    CHECK_EQ(gdbus(obj, "org.freedesktop.impl.portal.Inhibit.Inhibit", req2 + " org.example.Other '' 1 '{}'").status, 0);
    CHECK(inh.is_inhibited(InhibitFlag::Logout));
    CHECK_EQ(gdbus(req, "org.freedesktop.impl.portal.Request.Close", "").status, 0);
    CHECK(!inh.is_inhibited(InhibitFlag::Idle));
    CHECK(!inh.is_inhibited(InhibitFlag::Suspend));
    CHECK(inh.is_inhibited(InhibitFlag::Logout));
    REQUIRE(changes.size() == 3);
    CHECK(!changes[2].first);
    CHECK_EQ(changes[2].second.handle.path, req);
    CHECK_EQ(gdbus(req2, "org.freedesktop.impl.portal.Request.Close", "").status, 0);
    CHECK(inh.active_inhibitions().empty());
    CHECK(f.backend->get_request(ObjectPath{req}) == nullptr);
    CHECK(f.backend->get_request(ObjectPath{req2}) == nullptr);

    // A monitor session.
    const std::string mon = "/org/freedesktop/portal/desktop/session/inhibit_mon_1";
    auto created = gdbus(obj, "org.freedesktop.impl.portal.Inhibit.CreateMonitor",
                         "/org/freedesktop/portal/desktop/request/inhibit_mon_req " + mon + " org.example.App ''");
    CHECK_EQ(created.status, 0);
    CHECK(created.out.find("uint32 0") != std::string::npos);

    std::vector<std::pair<std::string, uint32_t>> states;
    auto slot = f.client->add_match(
        "type='signal',interface='org.freedesktop.impl.portal.Inhibit',member='StateChanged'",
        [&](dbus::Message& m) {
            ObjectPath p;
            VariantMap st;
            m.read_object_path(&p);
            m.read_variant_map(&st);
            states.emplace_back(p.path, get_uint32_or(st, "session-state", 0));
        });
    REQUIRE(slot.is_valid());
    auto pump = [&](size_t want) {
        for (int i = 0; i < 100 && states.size() < want; ++i) {
            while (f.backend->bus().process() > 0) {
            }
            f.client->wait(20000);
            while (f.client->process() > 0) {
            }
        }
    };
    while (f.client->process() > 0) {
    }
    inh.notify_state_changed(false, SessionState::QueryEnd);
    pump(1);
    REQUIRE(states.size() == 1);
    CHECK_EQ(states[0].first, mon);
    CHECK_EQ(states[0].second, static_cast<uint32_t>(SessionState::QueryEnd));

    std::vector<std::string> acks;
    inh.set_query_end_listener([&](const ObjectPath& p) { acks.push_back(p.path); });
    CHECK_EQ(gdbus(obj, "org.freedesktop.impl.portal.Inhibit.QueryEndResponse", mon).status, 0);
    REQUIRE(acks.size() == 1);
    CHECK_EQ(acks[0], mon);

    // Closed: no more state, and its acknowledgements are ignored.
    CHECK_EQ(gdbus(mon, "org.freedesktop.impl.portal.Session.Close", "").status, 0);
    CHECK(f.backend->get_session(ObjectPath{mon}) == nullptr);
    inh.notify_state_changed(false, SessionState::Ending);
    pump(2);
    CHECK_EQ(states.size(), 1u);
    CHECK_EQ(gdbus(obj, "org.freedesktop.impl.portal.Inhibit.QueryEndResponse", mon).status, 0);
    CHECK_EQ(acks.size(), 1u);

    return bstest::finish("test_inhibit");
}
