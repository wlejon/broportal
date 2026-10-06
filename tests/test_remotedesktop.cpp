// org.freedesktop.impl.portal.RemoteDesktop on a private bus: CreateSession
// (session_id), SelectDevices, Start refused without a host to grant control
// and narrowed by one that does, and the Notify* input methods reaching the
// host's listener only for devices the session was granted. Linux.
#include "check.h"
#include "fixture.h"

#include <atomic>
#include <mutex>

using namespace broportal;

namespace {

constexpr const char* kIface = "org.freedesktop.impl.portal.RemoteDesktop";
const std::string kReq = "/org/freedesktop/portal/desktop/request/rd_req_";

std::function<void(dbus::Message&)> session_args(const std::string& req, const ObjectPath& sess, VariantMap opts,
                                                 bool with_parent) {
    return [=](dbus::Message& m) {
        m.append_object_path(ObjectPath{kReq + req});
        m.append_object_path(sess);
        m.append_string("org.example.App");
        if (with_parent) m.append_string("");
        m.append_variant_map(opts);
    };
}

struct Seen {
    std::mutex mu;
    int32_t keycode = 0;
    uint32_t keystate = 0;
    double dx = 0, dy = 0;
    int32_t button = 0;
    uint32_t touch_slot = 99;
    int events = 0;
};

}  // namespace

int main() {
    bstest::PortalFixture f("test_remotedesktop", false);

    Seen seen;
    RemoteDesktopInputListener listener;
    listener.on_keyboard_keycode = [&](const ObjectPath&, int32_t code, uint32_t state) {
        std::lock_guard lock(seen.mu);
        seen.keycode = code;
        seen.keystate = state;
        ++seen.events;
    };
    listener.on_pointer_motion = [&](const ObjectPath&, double dx, double dy) {
        std::lock_guard lock(seen.mu);
        seen.dx = dx;
        seen.dy = dy;
        ++seen.events;
    };
    listener.on_pointer_button = [&](const ObjectPath&, int32_t button, uint32_t) {
        std::lock_guard lock(seen.mu);
        seen.button = button;
        ++seen.events;
    };
    listener.on_touch_up = [&](const ObjectPath&, uint32_t slot) {
        std::lock_guard lock(seen.mu);
        seen.touch_slot = slot;
        ++seen.events;
    };
    f.backend->remote_desktop().set_input_listener(std::move(listener));
    f.start();

    const ObjectPath sess{"/org/freedesktop/portal/desktop/session/rd_sess_1"};
    auto notify_key = [&](const ObjectPath& s, int32_t code, std::string* err) {
        return f.call(kIface, "NotifyKeyboardKeycode",
                      [=](dbus::Message& m) {
                          m.append_object_path(s);
                          m.append_variant_map({});
                          m.append_int32(code);
                          m.append_uint32(1);
                      },
                      nullptr, err);
    };

    auto created = f.request(kIface, "CreateSession", session_args("1", sess, {}, false));
    CHECK_EQ(created.code, 0u);
    CHECK(get_string(created.results, "session_id").has_value());

    // Input before Start is refused, and the host never sees it.
    std::string err;
    CHECK(!notify_key(sess, 30, &err));
    CHECK(err.find("AccessDenied") != std::string::npos || err.find("not granted") != std::string::npos);

    VariantMap select;
    select["types"] = Variant(static_cast<uint32_t>(DeviceType::Keyboard) | static_cast<uint32_t>(DeviceType::Pointer));
    CHECK_EQ(f.request(kIface, "SelectDevices", session_args("2", sess, select, false)).code, 0u);

    // Nobody to grant control: Start fails.
    auto refused = f.request(kIface, "Start", session_args("3", sess, {}, true));
    CHECK_EQ(refused.code, 2u);
    CHECK(!refused.results.contains("devices"));
    CHECK(!notify_key(sess, 30, nullptr));

    // The host grants keyboard only, and tries to add touch: neither the
    // pointer (narrowed away) nor touch (never requested) is granted.
    std::atomic<uint32_t> asked{0};
    f.backend->remote_desktop().set_start_callback(
        [&](const ObjectPath&, const ObjectPath& s, const std::string&, uint32_t& devices, VariantMap&) {
            if (s != sess) return ResponseCode::OtherError;
            asked = devices;
            devices = static_cast<uint32_t>(DeviceType::Keyboard) | static_cast<uint32_t>(DeviceType::Touchscreen);
            return ResponseCode::Success;
        });
    auto started = f.request(kIface, "Start", session_args("4", sess, {}, true));
    CHECK_EQ(started.code, 0u);
    CHECK_EQ(asked.load(), 3u);
    CHECK_EQ(get_uint32_or(started.results, "devices", 0), 1u);
    CHECK(started.results.contains("clipboard_enabled"));

    CHECK(notify_key(sess, 30, &err));
    {
        std::lock_guard lock(seen.mu);
        CHECK_EQ(seen.keycode, 30);
        CHECK_EQ(seen.keystate, 1u);
        CHECK_EQ(seen.events, 1);
    }

    // Pointer and touch were not granted.
    CHECK(!f.call(kIface, "NotifyPointerMotion",
                  [&](dbus::Message& m) {
                      m.append_object_path(sess);
                      m.append_variant_map({});
                      m.append_double(12.5);
                      m.append_double(-8.25);
                  },
                  nullptr, &err));
    CHECK(!f.call(kIface, "NotifyTouchUp",
                  [&](dbus::Message& m) {
                      m.append_object_path(sess);
                      m.append_variant_map({});
                      m.append_uint32(3);
                  },
                  nullptr, &err));

    // A second session, granted the pointer, from an independent client.
    const ObjectPath sess2{"/org/freedesktop/portal/desktop/session/rd_sess_2"};
    CHECK_EQ(f.request(kIface, "CreateSession", session_args("5", sess2, {}, false)).code, 0u);
    VariantMap pointer;
    pointer["types"] = Variant(static_cast<uint32_t>(DeviceType::Pointer));
    CHECK_EQ(f.request(kIface, "SelectDevices", session_args("6", sess2, pointer, false)).code, 0u);
    f.backend->remote_desktop().set_start_callback(
        [](const ObjectPath&, const ObjectPath&, const std::string&, uint32_t&, VariantMap&) {
            return ResponseCode::Success;
        });
    auto started2 = f.request(kIface, "Start", session_args("7", sess2, {}, true));
    CHECK_EQ(started2.code, 0u);
    CHECK_EQ(get_uint32_or(started2.results, "devices", 0), 2u);

    if (bstest::have_gdbus()) {
        std::string out = f.gdbus(std::string(kIface) + ".NotifyPointerMotion", sess2.path + " '{}' 12.5 8.25");
        CHECK(out.find("()") != std::string::npos);
        out = f.gdbus(std::string(kIface) + ".NotifyPointerButton", sess2.path + " '{}' 272 1");
        CHECK(out.find("()") != std::string::npos);
        std::lock_guard lock(seen.mu);
        CHECK_EQ(seen.dx, 12.5);
        CHECK_EQ(seen.dy, 8.25);
        CHECK_EQ(seen.button, 272);
    } else {
        std::printf("Note: gdbus is not installed; the outside-client calls did not run\n");
    }

    // sess2's grant does not extend to keyboard, and an unknown session gets nothing.
    CHECK(!notify_key(sess2, 31, nullptr));
    CHECK(!notify_key(ObjectPath{"/org/freedesktop/portal/desktop/session/none"}, 31, nullptr));

    // Closing ends the grant.
    CHECK(f.client->call_method(f.config.bus_name, sess.path, "org.freedesktop.impl.portal.Session", "Close",
                                nullptr, nullptr, &err));
    CHECK(bstest::wait_until([&] { return f.backend->get_session(sess) == nullptr; }, std::chrono::seconds(5)));
    CHECK(!notify_key(sess, 32, nullptr));
    {
        std::lock_guard lock(seen.mu);
        CHECK_EQ(seen.keycode, 30);
    }

    return bstest::finish("test_remotedesktop");
}
