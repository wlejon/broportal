// org.freedesktop.impl.portal.FileChooser on a private bus. The host's picker
// callback answers OpenFile / SaveFile / SaveFiles; a preselection answers
// when there is no picker; with neither, the backend reports an error instead
// of inventing a file. Calls come from the library's client and from gdbus.
// Linux.
#include "check.h"
#include "fixture.h"

#include <filesystem>
#include <string>

using namespace broportal;

namespace {

struct Reply {
    bool called = false;
    uint32_t code = 999;
    VariantMap results;
};

Reply call(bstest::PortalFixture& f, const char* method, const char* handle, const std::string& title,
           VariantMap opts) {
    Reply r;
    std::string err;
    r.called = f.call(
        "org.freedesktop.impl.portal.FileChooser", method,
        [&](dbus::Message& msg) {
            msg.append_object_path(ObjectPath{handle});
            msg.append_string("org.example.App");
            msg.append_string("");
            msg.append_string(title);
            msg.append_variant_map(opts);
        },
        [&](dbus::Message& reply) {
            reply.read_uint32(&r.code);
            reply.read_variant_map(&r.results);
        },
        &err);
    if (!r.called) std::fprintf(stderr, "call %s: %s\n", method, err.c_str());
    return r;
}

std::vector<std::string> uris(const Reply& r) {
    auto it = r.results.find("uris");
    if (it == r.results.end()) return {};
    const auto* v = it->second.get_if<std::vector<std::string>>();
    return v ? *v : std::vector<std::string>{};
}

}  // namespace

int main() {
    bstest::PortalFixture f("test_filechooser");
    auto& fc = f.backend->file_chooser();

    // No picker, no preselection: an error response, no uris.
    Reply none = call(f, "OpenFile", "/org/freedesktop/portal/desktop/request/fc_0", "Open", {});
    CHECK(none.called);
    CHECK_EQ(none.code, 2u);
    CHECK(uris(none).empty());
    Reply none_save = call(f, "SaveFile", "/org/freedesktop/portal/desktop/request/fc_0s", "Save",
                           VariantMap{{"current_name", Variant("x.txt")}});
    CHECK_EQ(none_save.code, 2u);

    // A preselection answers OpenFile, percent-encoded as a URI.
    fc.set_default_selected_files({"/tmp/broportal dir/a#1.txt", "file:///already/a/uri"});
    Reply pre = call(f, "OpenFile", "/org/freedesktop/portal/desktop/request/fc_1", "Open", {});
    CHECK_EQ(pre.code, 0u);
    auto pre_uris = uris(pre);
    REQUIRE(pre_uris.size() == 2);
    CHECK_EQ(pre_uris[0], std::string("file:///tmp/broportal%20dir/a%231.txt"));
    CHECK_EQ(pre_uris[1], std::string("file:///already/a/uri"));

    // The picker callback sees the parsed options and decides.
    std::string seen_title, seen_app, seen_name, seen_folder;
    bool seen_multiple = false, seen_directory = false;
    fc.set_file_picker_callback([&](const ObjectPath&, const std::string& app_id, const std::string& title,
                                    const FileChooserOptions& o, std::vector<std::string>& out,
                                    VariantMap& results) {
        seen_app = app_id;
        seen_title = title;
        seen_name = o.current_name;
        seen_folder = o.current_folder;
        seen_multiple = o.multiple;
        seen_directory = o.directory;
        if (title == "Cancel me") return ResponseCode::Cancelled;
        out.push_back("/home/user/picked.txt");
        if (o.multiple) out.push_back("/home/user/second.txt");
        results["choices"] = Variant(StringPairList{{"encoding", "utf8"}});
        return ResponseCode::Success;
    });

    std::string folder = "/home/user/docs";
    std::vector<uint8_t> folder_bytes(folder.begin(), folder.end());
    folder_bytes.push_back(0);  // portal byte strings are NUL-terminated
    Reply open = call(f, "OpenFile", "/org/freedesktop/portal/desktop/request/fc_2", "Pick two",
                      VariantMap{{"multiple", Variant(true)}, {"current_folder", Variant(folder_bytes)}});
    CHECK_EQ(open.code, 0u);
    CHECK_EQ(seen_app, std::string("org.example.App"));
    CHECK_EQ(seen_title, std::string("Pick two"));
    CHECK(seen_multiple);
    CHECK(!seen_directory);
    CHECK_EQ(seen_folder, folder);
    auto open_uris = uris(open);
    REQUIRE(open_uris.size() == 2);
    CHECK_EQ(open_uris[0], std::string("file:///home/user/picked.txt"));
    CHECK(open.results.contains("choices"));

    Reply save = call(f, "SaveFile", "/org/freedesktop/portal/desktop/request/fc_3", "Save",
                      VariantMap{{"current_name", Variant("exported document.pdf")}});
    CHECK_EQ(save.code, 0u);
    CHECK_EQ(seen_name, std::string("exported document.pdf"));
    CHECK_EQ(uris(save).size(), size_t(1));

    Reply cancelled = call(f, "OpenFile", "/org/freedesktop/portal/desktop/request/fc_4", "Cancel me", {});
    CHECK_EQ(cancelled.code, 1u);

    // gdbus, an outside client, gets the same answer.
    if (bstest::have_gdbus()) {
        std::string out = f.gdbus("org.freedesktop.impl.portal.FileChooser.OpenFile",
                                  "/org/freedesktop/portal/desktop/request/fc_5 org.example.App '' 'From gdbus' "
                                  "\"{'directory': <true>}\"");
        CHECK(out.find("uint32 0") != std::string::npos);
        CHECK(out.find("file:///home/user/picked.txt") != std::string::npos);
        CHECK(seen_directory);
        CHECK_EQ(seen_title, std::string("From gdbus"));
    } else {
        std::printf("Note: gdbus is not installed; the outside-client call did not run\n");
    }

    // Wrong argument types are rejected, not answered.
    std::string err;
    CHECK(!f.call("org.freedesktop.impl.portal.FileChooser", "OpenFile",
                  [](dbus::Message& m) { m.append_string("not an object path"); }, nullptr, &err));

    return bstest::finish("test_filechooser");
}
