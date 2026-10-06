// Off Linux the library builds its value types and reports, with a reason,
// that it cannot serve portals. Windows / macOS.
#include "check.h"
#include "broportal/availability.h"
#include "broportal/types.h"

int main() {
    std::string why;
    CHECK(!broportal::available(&why));
    CHECK(why.find("D-Bus") != std::string::npos);
    CHECK(!broportal::available());
    std::printf("unavailable: %s\n", why.c_str());

    // The value types still work.
    broportal::VariantMap m;
    m["color-scheme"] = broportal::Variant(static_cast<uint32_t>(1));
    CHECK_EQ(broportal::get_uint32_or(m, "color-scheme", 0), 1u);

    return bstest::finish("test_unavailable");
}
