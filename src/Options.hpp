#pragma once
// Runtime options: defaults < INI config file < command line. Kept free of SDL/GL
// so the config loader/saver can be unit-tested on its own.

#include "ObsLayout.hpp"

#include <optional>
#include <string>
#include <vector>

namespace muq {

enum class OutputMode
{
    Window,
    Fullscreen,
    Layout
};

const char* ModeName(OutputMode mode);
/// Parses "window" / "fullscreen" / "layout". Returns false on unknown names.
bool ParseMode(const std::string& name, OutputMode& mode);

struct Point
{
    int x = 0;
    int y = 0;
};

struct Options
{
    OutputMode mode = OutputMode::Window;
    std::string layoutPath;
    LayoutOptions layout;
    std::string monitor;              // display index or name substring; empty = 0
    std::optional<Point> origin;      // explicit layout origin (overrides monitor)
    int width = 1280;
    int height = 720;
    std::vector<std::string> presetDirs;
    bool presetDirsAuto = false;      // presetDirs were auto-detected (not saved to config)
    std::optional<std::string> source; // PipeWire node.name; "" = default sink monitor
    std::string startPreset;          // substring match of first preset
    int fps = 60;
    bool vsync = false;
    bool shuffle = true;
    bool hardCuts = false;
    bool smooth = true;
    bool lock = false;
    double presetDuration = 30.0;
    double softCut = 3.0;
    float beatSensitivity = 1.0f;
    float brightness = 1.0f;          // output gain, 0 = black, 1 = unchanged, >1 boost
    bool showUi = true;
    bool overrideRedirect = true;
    float uiScale = 1.0f;

    // Web control UI
    bool webEnabled = true;
    std::string webBind = "0.0.0.0";
    int webPort = 8080;

    // Config file used for "Save config" (empty = saving disabled)
    std::string configPath;
};

} // namespace muq
