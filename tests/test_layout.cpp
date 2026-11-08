#include "ObsLayout.hpp"

#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                    \
    do                                                                 \
    {                                                                  \
        if (!(cond))                                                   \
        {                                                              \
            std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                \
        }                                                              \
    } while (0)

static bool Eq(const muq::Rect& r, int x, int y, int w, int h)
{
    return r.x == x && r.y == y && r.w == w && r.h == h;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: test_layout <obs.json>\n");
        return 2;
    }

    // Example file shipped with the project.
    auto layout = muq::LoadObsLayout(argv[1]);
    std::map<std::string, muq::Rect> got;
    for (const auto& s : layout.surfaces)
    {
        got[s.name] = s.rect;
        std::printf("%-24s %4dx%-4d @ %d,%d\n", s.name.c_str(), s.rect.w, s.rect.h, s.rect.x, s.rect.y);
    }
    CHECK(layout.surfaces.size() == 6);
    CHECK(got.count("UNUSEDS2-240x320@400x0") == 0);
    CHECK(Eq(got["S1-400x320@0x0"], 0, 0, 400, 320));
    CHECK(Eq(got["S3-160x240@0x320"], 640, 0, 160, 240));
    CHECK(Eq(got["S4-240x360@160x320"], 0, 320, 240, 360));
    CHECK(Eq(got["S5-320x120@400x320"], 240, 320, 320, 120));
    CHECK(Eq(got["S6-80x480@400x400"], 640, 240, 80, 480));
    CHECK(Eq(got["S7-160x360@480x440"], 800, 0, 160, 480));
    CHECK(Eq(layout.bounds, 0, 0, 960, 720));

    // Synthetic: alignment, crop, bounds, arbitrary surface count.
    const char* synthetic = R"({
      "current_scene": "Main",
      "sources": [
        {"name": "Main", "id": "scene", "uuid": "sc", "settings": {"items": [
          {"name": "Sa", "source_uuid": "a", "pos": {"x": 100, "y": 100}, "scale": {"x": 2, "y": 2}, "align": 10},
          {"name": "Sb", "source_uuid": "b", "pos": {"x": 50, "y": 60}, "scale": {"x": 1, "y": 1}, "align": 0},
          {"name": "Sc", "source_uuid": "a", "pos": {"x": 0, "y": 0}, "scale": {"x": 1, "y": 1}, "align": 5,
           "crop_left": 10, "crop_right": 10, "crop_top": 5},
          {"name": "Sd", "source_uuid": "a", "pos": {"x": 300, "y": 0}, "align": 5, "bounds_type": 2,
           "bounds": {"x": 64, "y": 32}},
          {"name": "Xe", "source_uuid": "a", "pos": {"x": 0, "y": 0}},
          {"name": "Sf", "source_uuid": "a", "visible": false, "pos": {"x": 0, "y": 0}}
        ]}},
        {"name": "srcA", "id": "color_source", "uuid": "a", "settings": {"width": 40, "height": 30}},
        {"name": "srcB", "id": "image_source", "uuid": "b", "settings": {}}
      ]
    })";
    muq::LayoutOptions opts;
    opts.canvasWidth = 100;
    opts.canvasHeight = 50;
    auto syn = muq::ParseObsLayout(synthetic, opts);
    std::map<std::string, muq::Rect> s;
    for (const auto& surf : syn.surfaces)
    {
        s[surf.name] = surf.rect;
    }
    CHECK(syn.surfaces.size() == 4);
    CHECK(Eq(s["Sa"], 20, 40, 80, 60));   // align = right|bottom
    CHECK(Eq(s["Sb"], 0, 35, 100, 50));   // align = center, base = canvas
    CHECK(Eq(s["Sc"], 0, 0, 20, 25));     // crop
    CHECK(Eq(s["Sd"], 300, 0, 64, 32));   // bounds
    CHECK(Eq(syn.bounds, 0, 0, 364, 100));

    bool threw = false;
    try
    {
        muq::LayoutOptions none;
        none.prefix = "Z";
        muq::ParseObsLayout(synthetic, none);
    }
    catch (const std::runtime_error&)
    {
        threw = true;
    }
    CHECK(threw);

    if (failures)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("all layout tests passed\n");
    return 0;
}
