// org.freedesktop.impl.portal.Screenshot on a private bus: without a host
// capture the answer is an error (never a made-up picture); with one, the
// options reach the host and its uri comes back; PickColor relays the host's
// pick or a preselected color, and errors when neither exists. Linux.
#include "check.h"
#include "fixture.h"

#include <atomic>
#include <cmath>

using namespace broportal;

namespace {

constexpr const char* kIface = "org.freedesktop.impl.portal.Screenshot";

std::function<void(dbus::Message&)> args(const std::string& req, VariantMap opts) {
    return [=](dbus::Message& m) {
        m.append_object_path(ObjectPath{"/org/freedesktop/portal/desktop/request/" + req});
        m.append_string("org.example.App");
        m.append_string("");
        m.append_variant_map(opts);
    };
}

bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

}  // namespace

int main() {
    bstest::PortalFixture f("test_screenshot");

    // No capture: OtherError and no uri.
    auto none = f.request(kIface, "Screenshot", args("shot_1", {}));
    CHECK(none.called);
    CHECK_EQ(none.code, 2u);
    CHECK(!none.results.contains("uri"));

    std::atomic<bool> seen_interactive{false}, seen_modal{true};
    std::atomic<uint32_t> seen_target{0};
    f.backend->screenshot().set_screenshot_callback(
        [&](const ObjectPath&, const std::string& app_id, const ScreenshotOptions& o, std::string& uri,
            VariantMap&) {
            if (app_id != "org.example.App") return ResponseCode::OtherError;
            seen_interactive = o.interactive;
            seen_modal = o.modal;
            seen_target = o.target;
            uri = "file:///run/user/1000/shot%201.png";
            return ResponseCode::Success;
        });
    VariantMap opts;
    opts["interactive"] = Variant(true);
    opts["modal"] = Variant(false);
    opts["target"] = Variant(static_cast<uint32_t>(ScreenshotTarget::Window));
    auto shot = f.request(kIface, "Screenshot", args("shot_2", opts));
    CHECK_EQ(shot.code, 0u);
    CHECK(seen_interactive.load());
    CHECK(!seen_modal.load());
    CHECK_EQ(seen_target.load(), 2u);
    CHECK_EQ(get_string(shot.results, "uri").value_or(""), std::string("file:///run/user/1000/shot%201.png"));

    // A host that reports success with no uri gets an error, not an empty answer.
    f.backend->screenshot().set_screenshot_callback(
        [](const ObjectPath&, const std::string&, const ScreenshotOptions&, std::string&, VariantMap&) {
            return ResponseCode::Success;
        });
    auto empty = f.request(kIface, "Screenshot", args("shot_3", {}));
    CHECK_EQ(empty.code, 2u);

    // The user dismissing the capture.
    f.backend->screenshot().set_screenshot_callback(
        [](const ObjectPath&, const std::string&, const ScreenshotOptions&, std::string&, VariantMap&) {
            return ResponseCode::Cancelled;
        });
    CHECK_EQ(f.request(kIface, "Screenshot", args("shot_4", {})).code, 1u);

    // PickColor: nothing picked.
    auto nocolor = f.request(kIface, "PickColor", args("color_1", {}));
    CHECK_EQ(nocolor.code, 2u);
    CHECK(!nocolor.results.contains("color"));

    // A preselected color.
    f.backend->screenshot().set_default_color(RgbColor{0.25, 0.5, 0.75});
    auto preset = f.request(kIface, "PickColor", args("color_2", {}));
    CHECK_EQ(preset.code, 0u);
    auto it = preset.results.find("color");
    REQUIRE(it != preset.results.end());
    const RgbColor* c = it->second.get_if<RgbColor>();
    REQUIRE(c != nullptr);
    CHECK(near(c->r, 0.25) && near(c->g, 0.5) && near(c->b, 0.75));

    // The host's pick wins, and gdbus reads the (ddd) back.
    f.backend->screenshot().set_pick_color_callback(
        [](const ObjectPath&, const std::string&, const VariantMap&, RgbColor& out, VariantMap&) {
            out = RgbColor{1.0, 0.0, 0.5};
            return ResponseCode::Success;
        });
    auto picked = f.request(kIface, "PickColor", args("color_3", {}));
    CHECK_EQ(picked.code, 0u);
    const RgbColor* p = picked.results.at("color").get_if<RgbColor>();
    REQUIRE(p != nullptr);
    CHECK(near(p->r, 1.0) && near(p->g, 0.0) && near(p->b, 0.5));

    if (bstest::have_gdbus()) {
        std::string out = f.gdbus(std::string(kIface) + ".PickColor",
                                  "/org/freedesktop/portal/desktop/request/color_4 org.example.App '' '{}'");
        CHECK(out.find("uint32 0") != std::string::npos);
        CHECK(out.find("(1.0, 0.0, 0.5)") != std::string::npos);
    } else {
        std::printf("Note: gdbus is not installed; the outside-client call did not run\n");
    }

    return bstest::finish("test_screenshot");
}
