#include "Config.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

namespace fs = std::filesystem;

namespace muq {

const char* ModeName(OutputMode mode)
{
    switch (mode)
    {
        case OutputMode::Window:
            return "window";
        case OutputMode::Fullscreen:
            return "fullscreen";
        case OutputMode::Layout:
            return "layout";
    }
    return "?";
}

bool ParseMode(const std::string& name, OutputMode& mode)
{
    if (name == "window")
        mode = OutputMode::Window;
    else if (name == "fullscreen")
        mode = OutputMode::Fullscreen;
    else if (name == "layout")
        mode = OutputMode::Layout;
    else
        return false;
    return true;
}

namespace {

std::string Trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ParseBool(const std::string& v, bool& out)
{
    const std::string l = Lower(v);
    if (l == "1" || l == "true" || l == "yes" || l == "on")
        out = true;
    else if (l == "0" || l == "false" || l == "no" || l == "off")
        out = false;
    else
        return false;
    return true;
}

bool ParseInt(const std::string& v, int& out)
{
    if (v.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const long n = std::strtol(v.c_str(), &end, 10);
    if (errno || !end || *end != '\0')
        return false;
    out = static_cast<int>(n);
    return true;
}

bool ParseDouble(const std::string& v, double& out)
{
    if (v.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const double d = std::strtod(v.c_str(), &end);
    if (errno || !end || *end != '\0')
        return false;
    out = d;
    return true;
}

bool ParseFloat(const std::string& v, float& out)
{
    double d = 0;
    if (!ParseDouble(v, d))
        return false;
    out = static_cast<float>(d);
    return true;
}

bool ParseSize(const std::string& v, int& w, int& h)
{
    int a = 0, b = 0;
    char extra = 0;
    if (std::sscanf(v.c_str(), "%dx%d%c", &a, &b, &extra) != 2 || a < 0 || b < 0)
        return false;
    w = a;
    h = b;
    return true;
}

std::string FormatDouble(double d)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", d);
    return buf;
}

const char* Bool(bool b)
{
    return b ? "true" : "false";
}

using Setter = std::function<bool(const std::string&, Options&)>;

const std::map<std::string, Setter>& Setters()
{
    static const std::map<std::string, Setter> setters = {
        // [output]
        {"output.mode", [](const std::string& v, Options& o) { return ParseMode(Lower(v), o.mode); }},
        {"output.monitor", [](const std::string& v, Options& o) { o.monitor = v; return true; }},
        {"output.origin",
         [](const std::string& v, Options& o) {
             if (v.empty())
             {
                 o.origin.reset();
                 return true;
             }
             Point p;
             char extra = 0;
             if (std::sscanf(v.c_str(), "%d,%d%c", &p.x, &p.y, &extra) != 2)
                 return false;
             o.origin = p;
             return true;
         }},
        {"output.size",
         [](const std::string& v, Options& o) {
             int w = 0, h = 0;
             if (!ParseSize(v, w, h) || w == 0 || h == 0)
                 return false;
             o.width = w;
             o.height = h;
             return true;
         }},
        {"output.fps",
         [](const std::string& v, Options& o) {
             int n = 0;
             if (!ParseInt(v, n) || n < 1)
                 return false;
             o.fps = n;
             return true;
         }},
        {"output.vsync", [](const std::string& v, Options& o) { return ParseBool(v, o.vsync); }},
        {"output.override_redirect", [](const std::string& v, Options& o) { return ParseBool(v, o.overrideRedirect); }},
        {"output.brightness",
         [](const std::string& v, Options& o) {
             float f = 0;
             if (!ParseFloat(v, f))
                 return false;
             o.brightness = std::clamp(f, 0.0f, 2.0f);
             return true;
         }},
        {"output.layout", [](const std::string& v, Options& o) { o.layoutPath = v; return true; }},
        {"output.prefix", [](const std::string& v, Options& o) { o.layout.prefix = v; return true; }},
        {"output.scene", [](const std::string& v, Options& o) { o.layout.scene = v; return true; }},
        {"output.canvas",
         [](const std::string& v, Options& o) {
             if (v.empty())
             {
                 o.layout.canvasWidth = o.layout.canvasHeight = 0;
                 return true;
             }
             return ParseSize(v, o.layout.canvasWidth, o.layout.canvasHeight);
         }},
        // [audio]
        {"audio.source", [](const std::string& v, Options& o) { o.source = v; return true; }},
        // [presets]
        {"presets.dirs",
         [](const std::string& v, Options& o) {
             o.presetDirs.clear();
             std::stringstream ss(v);
             std::string item;
             while (std::getline(ss, item, ':'))
             {
                 item = Trim(item);
                 if (!item.empty())
                     o.presetDirs.push_back(item);
             }
             return true;
         }},
        {"presets.preset", [](const std::string& v, Options& o) { o.startPreset = v; return true; }},
        {"presets.duration",
         [](const std::string& v, Options& o) {
             double d = 0;
             if (!ParseDouble(v, d) || d <= 0)
                 return false;
             o.presetDuration = d;
             return true;
         }},
        {"presets.transition",
         [](const std::string& v, Options& o) {
             double d = 0;
             if (!ParseDouble(v, d) || d < 0)
                 return false;
             o.softCut = d;
             return true;
         }},
        {"presets.shuffle", [](const std::string& v, Options& o) { return ParseBool(v, o.shuffle); }},
        {"presets.smooth", [](const std::string& v, Options& o) { return ParseBool(v, o.smooth); }},
        {"presets.hard_cuts", [](const std::string& v, Options& o) { return ParseBool(v, o.hardCuts); }},
        {"presets.lock", [](const std::string& v, Options& o) { return ParseBool(v, o.lock); }},
        {"presets.beat_sensitivity",
         [](const std::string& v, Options& o) {
             float f = 0;
             if (!ParseFloat(v, f) || f < 0)
                 return false;
             o.beatSensitivity = f;
             return true;
         }},
        // [ui]
        {"ui.show_ui", [](const std::string& v, Options& o) { return ParseBool(v, o.showUi); }},
        {"ui.ui_scale",
         [](const std::string& v, Options& o) {
             float f = 0;
             if (!ParseFloat(v, f))
                 return false;
             o.uiScale = std::max(0.5f, f);
             return true;
         }},
        // [web]
        {"web.enabled", [](const std::string& v, Options& o) { return ParseBool(v, o.webEnabled); }},
        {"web.bind",
         [](const std::string& v, Options& o) {
             if (v.empty())
                 return false;
             o.webBind = v;
             return true;
         }},
        {"web.port",
         [](const std::string& v, Options& o) {
             int n = 0;
             if (!ParseInt(v, n) || n < 1 || n > 65535)
                 return false;
             o.webPort = n;
             return true;
         }},
    };
    return setters;
}

} // namespace

std::string DefaultConfigPath()
{
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
    {
        return (fs::path(xdg) / "muq-viz" / "muq-viz.ini").string();
    }
    const char* home = std::getenv("HOME");
    if (home && *home)
    {
        return (fs::path(home) / ".config" / "muq-viz" / "muq-viz.ini").string();
    }
    return {};
}

std::vector<std::string> ParseConfig(const std::string& text, Options& opt)
{
    std::vector<std::string> warnings;
    std::stringstream in(text);
    std::string line;
    std::string section;
    int lineNo = 0;
    const auto& setters = Setters();
    while (std::getline(in, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#' || t[0] == ';')
            continue;
        auto warn = [&](const std::string& msg) { warnings.push_back("line " + std::to_string(lineNo) + ": " + msg); };
        if (t.front() == '[')
        {
            if (t.back() != ']')
            {
                warn("malformed section header");
                continue;
            }
            section = Lower(Trim(t.substr(1, t.size() - 2)));
            continue;
        }
        const size_t eq = t.find('=');
        if (eq == std::string::npos)
        {
            warn("expected key = value");
            continue;
        }
        const std::string key = Lower(Trim(t.substr(0, eq)));
        const std::string value = Trim(t.substr(eq + 1));
        const std::string full = section + "." + key;
        auto it = setters.find(full);
        if (it == setters.end())
        {
            warn("unknown key " + full);
            continue;
        }
        if (!it->second(value, opt))
        {
            warn("invalid value for " + full + ": \"" + value + "\"");
        }
    }
    return warnings;
}

bool LoadConfig(const std::string& path, Options& opt, std::vector<std::string>& warnings)
{
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec))
    {
        return false;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        warnings.push_back(std::string("cannot read: ") + std::strerror(errno));
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    auto w = ParseConfig(ss.str(), opt);
    warnings.insert(warnings.end(), w.begin(), w.end());
    return true;
}

std::string FormatConfig(const Options& o)
{
    std::ostringstream s;
    s << "# muq-viz configuration (loaded at start, command-line options override it)\n\n";
    s << "[output]\n";
    s << "mode = " << ModeName(o.mode) << "\n";
    s << "monitor = " << o.monitor << "\n";
    s << "origin = ";
    if (o.origin)
        s << o.origin->x << "," << o.origin->y;
    s << "\n";
    s << "size = " << o.width << "x" << o.height << "\n";
    s << "fps = " << o.fps << "\n";
    s << "vsync = " << Bool(o.vsync) << "\n";
    s << "override_redirect = " << Bool(o.overrideRedirect) << "\n";
    s << "brightness = " << FormatDouble(o.brightness) << "\n";
    s << "layout = " << o.layoutPath << "\n";
    s << "prefix = " << o.layout.prefix << "\n";
    s << "scene = " << o.layout.scene << "\n";
    s << "canvas = ";
    if (o.layout.canvasWidth > 0 && o.layout.canvasHeight > 0)
        s << o.layout.canvasWidth << "x" << o.layout.canvasHeight;
    s << "\n\n";

    s << "[audio]\n";
    s << "# PipeWire node.name (see --list-sources); empty = default output monitor\n";
    s << "source = " << o.source.value_or("") << "\n\n";

    s << "[presets]\n";
    s << "# colon-separated; empty = auto-detect next to the binary / ~/muq-viz/presets\n";
    s << "dirs = ";
    for (size_t i = 0; !o.presetDirsAuto && i < o.presetDirs.size(); ++i)
        s << (i ? ":" : "") << o.presetDirs[i];
    s << "\n";
    s << "preset = " << o.startPreset << "\n";
    s << "duration = " << FormatDouble(o.presetDuration) << "\n";
    s << "transition = " << FormatDouble(o.softCut) << "\n";
    s << "shuffle = " << Bool(o.shuffle) << "\n";
    s << "smooth = " << Bool(o.smooth) << "\n";
    s << "hard_cuts = " << Bool(o.hardCuts) << "\n";
    s << "lock = " << Bool(o.lock) << "\n";
    s << "beat_sensitivity = " << FormatDouble(o.beatSensitivity) << "\n\n";

    s << "[ui]\n";
    s << "show_ui = " << Bool(o.showUi) << "\n";
    s << "ui_scale = " << FormatDouble(o.uiScale) << "\n\n";

    s << "[web]\n";
    s << "enabled = " << Bool(o.webEnabled) << "\n";
    s << "bind = " << o.webBind << "\n";
    s << "port = " << o.webPort << "\n";
    return s.str();
}

bool SaveConfig(const std::string& path, const Options& opt, std::string& error)
{
    if (path.empty())
    {
        error = "no config path";
        return false;
    }
    std::error_code ec;
    const fs::path p(path);
    if (p.has_parent_path())
    {
        fs::create_directories(p.parent_path(), ec);
        if (ec)
        {
            error = "mkdir " + p.parent_path().string() + ": " + ec.message();
            return false;
        }
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            error = "cannot write " + tmp + ": " + std::strerror(errno);
            return false;
        }
        f << FormatConfig(opt);
        f.flush();
        if (!f)
        {
            error = "write failed: " + tmp;
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec)
    {
        error = "rename to " + path + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace muq
