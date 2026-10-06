// Variant: holds/get, D-Bus signatures for every alternative, nested
// containers and equality. Portable: runs on every platform.
#include "check.h"
#include "broportal/types.h"

int main() {
    using namespace broportal;

    // Primitives
    Variant v_bool(true);
    CHECK(v_bool.holds<bool>());
    CHECK(v_bool.get_value_or<bool>(false) == true);
    CHECK(v_bool.signature() == "b");

    Variant v_u32(static_cast<uint32_t>(42));
    CHECK(v_u32.holds<uint32_t>());
    CHECK(v_u32.get_value_or<uint32_t>(0) == 42);
    CHECK(v_u32.signature() == "u");

    Variant v_str("hello world");
    CHECK(v_str.holds<std::string>());
    CHECK(v_str.get_value_or<std::string>("") == "hello world");
    CHECK(v_str.signature() == "s");

    Variant v_path(ObjectPath{"/org/test/path"});
    CHECK(v_path.holds<ObjectPath>());
    CHECK(v_path.signature() == "o");

    Variant v_fd(UnixFd{3});
    CHECK(v_fd.holds<UnixFd>());
    CHECK(v_fd.signature() == "h");

    Variant v_rgb(RgbColor{0.1, 0.5, 0.9});
    CHECK(v_rgb.holds<RgbColor>());
    CHECK(v_rgb.signature() == "(ddd)");

    Variant v_coord(Coord2D{1920, 1080});
    CHECK(v_coord.holds<Coord2D>());
    CHECK(v_coord.signature() == "(ii)");

    // Containers
    std::vector<std::string> list{"alpha", "beta", "gamma"};
    Variant v_list(list);
    CHECK(v_list.holds<std::vector<std::string>>());
    CHECK(v_list.signature() == "as");

    StringPairList pairs{{"key1", "val1"}, {"key2", "val2"}};
    Variant v_pairs(pairs);
    CHECK(v_pairs.holds<StringPairList>());
    CHECK(v_pairs.signature() == "a(ss)");

    VariantMap vmap;
    vmap["title"] = Variant("My Test");
    vmap["count"] = Variant(static_cast<uint32_t>(10));
    Variant v_map(vmap);
    CHECK(v_map.holds<VariantMap>());
    CHECK(v_map.signature() == "a{sv}");

    StreamList streams;
    VariantMap sprops;
    sprops["size"] = Variant(Coord2D{1280, 720});
    streams.emplace_back(100, sprops);
    Variant v_streams(streams);
    CHECK(v_streams.holds<StreamList>());
    CHECK(v_streams.signature() == "a(ua{sv})");

    ShortcutList shortcuts;
    VariantMap scprops;
    scprops["description"] = Variant("Take Screenshot");
    shortcuts.emplace_back("shortcut-1", scprops);
    Variant v_shortcuts(shortcuts);
    CHECK(v_shortcuts.holds<ShortcutList>());
    CHECK(v_shortcuts.signature() == "a(sa{sv})");

    SettingsMap settings;
    settings["org.freedesktop.appearance"]["color-scheme"] = Variant(static_cast<uint32_t>(1));
    Variant v_settings(settings);
    CHECK(v_settings.holds<SettingsMap>());
    CHECK(v_settings.signature() == "a{sa{sv}}");

    // Equality checks
    Variant v_bool_copy(true);
    CHECK(v_bool == v_bool_copy);
    CHECK(!(v_bool == v_u32));

    Variant v_map_copy(vmap);
    CHECK(v_map == v_map_copy);

    // Helpers on VariantMap.
    CHECK(get_string(vmap, "title") == std::optional<std::string>("My Test"));
    CHECK(!get_string(vmap, "count").has_value());
    CHECK(!get_string(vmap, "missing").has_value());
    CHECK_EQ(get_uint32_or(vmap, "count", 0), 10u);
    CHECK_EQ(get_uint32_or(vmap, "title", 7), 7u);
    CHECK_EQ(get_bool_or(vmap, "missing", true), true);
    CHECK(Variant().is_null());
    CHECK(Variant().signature() == "v");

    return bstest::finish("test_types");
}
