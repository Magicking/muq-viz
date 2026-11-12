#include "Config.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
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

namespace fs = std::filesystem;

static bool Near(double a, double b)
{
    return std::fabs(a - b) < 1e-4;
}

int main(int argc, char** argv)
{
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "muq-viz-test-config";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const std::string path = (dir / "sub" / "muq-viz.ini").string();

    // Round trip: every persisted field survives Save -> Load.
    muq::Options a;
    a.mode = muq::OutputMode::Layout;
    a.layoutPath = "/x/layout.json";
    a.layout.prefix = "Surf";
    a.layout.scene = "Main scene";
    a.layout.canvasWidth = 1024;
    a.layout.canvasHeight = 768;
    a.monitor = "HDMI";
    a.origin = muq::Point{10, -20};
    a.width = 800;
    a.height = 600;
    a.presetDirs = {"/p/one", "/p/two"};
    a.source = "alsa_output.pci-0000.analog-stereo";
    a.startPreset = "cream-of-the-crop/Fractal/foo bar";
    a.fps = 50;
    a.vsync = true;
    a.shuffle = false;
    a.hardCuts = true;
    a.smooth = false;
    a.lock = true;
    a.presetDuration = 42.5;
    a.softCut = 1.25;
    a.beatSensitivity = 2.5f;
    a.brightness = 0.75f;
    a.showUi = false;
    a.overrideRedirect = false;
    a.uiScale = 1.5f;
    a.webEnabled = false;
    a.webBind = "127.0.0.1";
    a.webPort = 9090;

    std::string err;
    CHECK(muq::SaveConfig(path, a, err));
    CHECK(err.empty());
    CHECK(!fs::exists(path + ".tmp"));

    muq::Options b;
    std::vector<std::string> warnings;
    CHECK(muq::LoadConfig(path, b, warnings));
    for (const auto& w : warnings)
        std::fprintf(stderr, "unexpected warning: %s\n", w.c_str());
    CHECK(warnings.empty());
    CHECK(b.mode == a.mode);
    CHECK(b.layoutPath == a.layoutPath);
    CHECK(b.layout.prefix == a.layout.prefix);
    CHECK(b.layout.scene == a.layout.scene);
    CHECK(b.layout.canvasWidth == 1024 && b.layout.canvasHeight == 768);
    CHECK(b.monitor == a.monitor);
    CHECK(b.origin && b.origin->x == 10 && b.origin->y == -20);
    CHECK(b.width == 800 && b.height == 600);
    CHECK(b.presetDirs == a.presetDirs);
    CHECK(b.source && *b.source == *a.source);
    CHECK(b.startPreset == a.startPreset);
    CHECK(b.fps == 50);
    CHECK(b.vsync && !b.shuffle && b.hardCuts && !b.smooth && b.lock);
    CHECK(Near(b.presetDuration, 42.5));
    CHECK(Near(b.softCut, 1.25));
    CHECK(Near(b.beatSensitivity, 2.5));
    CHECK(Near(b.brightness, 0.75));
    CHECK(!b.showUi && !b.overrideRedirect);
    CHECK(Near(b.uiScale, 1.5));
    CHECK(!b.webEnabled && b.webBind == "127.0.0.1" && b.webPort == 9090);

    // Defaults round trip (empty origin/canvas/source stay empty).
    muq::Options d;
    CHECK(muq::SaveConfig(path, d, err));
    muq::Options e;
    e.origin = muq::Point{1, 1};
    warnings.clear();
    CHECK(muq::LoadConfig(path, e, warnings));
    CHECK(warnings.empty());
    CHECK(!e.origin);
    CHECK(e.layout.canvasWidth == 0);
    CHECK(e.source && e.source->empty());
    CHECK(e.presetDirs.empty());

    // Auto-detected preset dirs are not persisted.
    muq::Options autoDirs;
    autoDirs.presetDirs = {"/auto"};
    autoDirs.presetDirsAuto = true;
    CHECK(muq::FormatConfig(autoDirs).find("/auto") == std::string::npos);

    // Comments, whitespace, case, CRLF, bad values, unknown keys.
    muq::Options p;
    auto w = muq::ParseConfig("; comment\r\n"
                              "# another\n"
                              "  [Output]  \n"
                              "  MODE =  fullscreen  \r\n"
                              "brightness = 9\n"
                              "fps = abc\n"
                              "bogus = 1\n"
                              "no equals sign\n"
                              "[presets]\n"
                              "lock = yes\n"
                              "shuffle = maybe\n"
                              "[web\n",
                              p);
    CHECK(p.mode == muq::OutputMode::Fullscreen);
    CHECK(Near(p.brightness, 2.0)); // clamped
    CHECK(p.fps == 60);             // invalid -> unchanged
    CHECK(p.lock);
    CHECK(p.shuffle);               // invalid -> unchanged
    CHECK(w.size() == 5);           // fps, bogus, no '=', shuffle, bad section
    for (const auto& s : w)
        std::printf("warning (expected): %s\n", s.c_str());

    // Missing file: false, no warnings.
    muq::Options m;
    warnings.clear();
    CHECK(!muq::LoadConfig((dir / "missing.ini").string(), m, warnings));
    CHECK(warnings.empty());

    fs::remove_all(dir, ec);
    if (failures)
    {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("config: all checks passed\n");
    return 0;
}
