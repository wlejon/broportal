// org.freedesktop.impl.portal.ScreenCast on a private bus: CreateSession,
// SelectSources (options reach the host), Start relaying the host's streams
// (a(ua{sv}) on the wire, read back by the library and by gdbus), Start with
// no host callback failing, and Session.Close ending the session. Linux.
#include "check.h"
#include "fixture.h"

#include <atomic>

using namespace broportal;

namespace {

constexpr const char* kIface = "org.freedesktop.impl.portal.ScreenCast";

std::function<void(dbus::Message&)> session_args(const std::string& req, const ObjectPath& sess,
                                                 VariantMap opts, bool with_parent) {
    return [=](dbus::Message& m) {
        m.append_object_path(ObjectPath{req});
        m.append_object_path(sess);
        m.append_string("org.example.App");
        if (with_parent) m.append_string("");
        m.append_variant_map(opts);
    };
}

}  // namespace

int main() {
    bstest::PortalFixture f("test_screencast");
    const ObjectPath sess{"/org/freedesktop/portal/desktop/session/sc_sess_1"};
    const std::string req = "/org/freedesktop/portal/desktop/request/sc_req_";

    auto created = f.request(kIface, "CreateSession", session_args(req + "1", sess, {}, false));
    CHECK(created.called);
    CHECK_EQ(created.code, 0u);
    CHECK(created.results.contains("session_id"));
    CHECK(f.backend->get_session(sess) != nullptr);

    // Start before the host can stream: an error, not an invented stream.
    auto early = f.request(kIface, "Start", session_args(req + "2", sess, {}, true));
    CHECK_EQ(early.code, 2u);
    CHECK(!early.results.contains("streams"));

    // SelectSources: the host sees the parsed options at Start.
    VariantMap select;
    select["types"] = Variant(static_cast<uint32_t>(SourceType::Monitor) | static_cast<uint32_t>(SourceType::Window));
    select["multiple"] = Variant(true);
    select["cursor_mode"] = Variant(static_cast<uint32_t>(CursorMode::Embedded));
    select["persist_mode"] = Variant(static_cast<uint32_t>(2));
    auto selected = f.request(kIface, "SelectSources", session_args(req + "3", sess, select, false));
    CHECK_EQ(selected.code, 0u);

    std::atomic<uint32_t> seen_types{0}, seen_cursor{0}, seen_persist{0};
    std::atomic<bool> seen_multiple{false};
    f.backend->screencast().set_negotiate_callback(
        [&](const ObjectPath&, const ObjectPath& s, const std::string&, const ScreenCastSourceOptions& o,
            StreamList& streams, VariantMap& results) {
            if (s != sess) return ResponseCode::OtherError;
            seen_types = o.types;
            seen_cursor = o.cursor_mode;
            seen_persist = o.persist_mode;
            seen_multiple = o.multiple;
            VariantMap props;
            props["position"] = Variant(Coord2D{0, 0});
            props["size"] = Variant(Coord2D{2560, 1440});
            props["source_type"] = Variant(static_cast<uint32_t>(SourceType::Monitor));
            streams.emplace_back(57u, props);
            results["persist_mode"] = Variant(o.persist_mode);
            return ResponseCode::Success;
        });

    auto started = f.request(kIface, "Start", session_args(req + "4", sess, {}, true));
    CHECK_EQ(started.code, 0u);
    CHECK_EQ(seen_types.load(), 3u);
    CHECK_EQ(seen_cursor.load(), 2u);
    CHECK_EQ(seen_persist.load(), 2u);
    CHECK(seen_multiple.load());
    auto it = started.results.find("streams");
    REQUIRE(it != started.results.end());
    const StreamList* streams = it->second.get_if<StreamList>();
    REQUIRE(streams != nullptr);
    REQUIRE(streams->size() == 1);
    CHECK_EQ(streams->front().first, 57u);
    const VariantMap& props = streams->front().second;
    REQUIRE(props.contains("size"));
    const Coord2D* size = props.at("size").get_if<Coord2D>();
    REQUIRE(size != nullptr);
    CHECK_EQ(size->x, 2560);
    CHECK_EQ(size->y, 1440);
    CHECK_EQ(get_uint32_or(started.results, "persist_mode", 0), 2u);

    if (bstest::have_gdbus()) {
        std::string out = f.gdbus(std::string(kIface) + ".Start",
                                  req + "5 " + sess.path + " org.example.App '' '{}'");
        CHECK(out.find("uint32 0") != std::string::npos);
        CHECK(out.find("(uint32 57,") != std::string::npos || out.find("(57,") != std::string::npos);
        CHECK(out.find("(2560, 1440)") != std::string::npos);
    } else {
        std::printf("Note: gdbus is not installed; the outside-client call did not run\n");
    }

    // Start on a session that does not exist.
    auto unknown = f.request(kIface, "Start",
                             session_args(req + "6", ObjectPath{"/org/freedesktop/portal/desktop/session/none"}, {},
                                          true));
    CHECK_EQ(unknown.code, 2u);

    // Session.Close ends it: the backend forgets it and Start fails.
    std::string err;
    CHECK(f.client->call_method(f.config.bus_name, sess.path, "org.freedesktop.impl.portal.Session", "Close",
                                nullptr, nullptr, &err));
    CHECK(bstest::wait_until([&] { return f.backend->get_session(sess) == nullptr; }, std::chrono::seconds(5)));
    auto after = f.request(kIface, "Start", session_args(req + "7", sess, {}, true));
    CHECK_EQ(after.code, 2u);

    return bstest::finish("test_screencast");
}
