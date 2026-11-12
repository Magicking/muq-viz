// muq-viz: MilkDrop preset renderer (libprojectM) with PipeWire capture and
// window / fullscreen / OBS-layout multi-window output.

#include "App.hpp"
#include "Config.hpp"

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <algorithm>
#include <thread>

namespace fs = std::filesystem;

namespace {

void Usage(const char* argv0)
{
    std::printf(
        "muq-viz " MUQ_VERSION " - MilkDrop visualizer (projectM + PipeWire)\n\n"
        "Usage: %s [options]\n\n"
        "Output:\n"
        "  -m, --mode MODE         window | fullscreen | layout (default: window, or layout when --layout given)\n"
        "  -l, --layout FILE       OBS scene-collection JSON; every scene item named S* becomes a window\n"
        "      --prefix STR        surface name prefix (default: S)\n"
        "      --scene NAME        OBS scene to use (default: current_scene)\n"
        "      --canvas WxH        OBS base canvas size for sources without explicit size (default: item scale_ref)\n"
        "      --monitor N|NAME    monitor for fullscreen / layout origin (index or name substring, default 0)\n"
        "      --origin X,Y        explicit screen position of OBS canvas (0,0) in layout mode\n"
        "      --size WxH          window size in window mode (default 1280x720)\n"
        "      --no-override-redirect  let the window manager manage layout windows\n"
        "      --vsync             sync first output to vblank\n"
        "      --fps N             target frame rate (default 60)\n"
        "Audio:\n"
        "  -s, --source NODE       PipeWire node.name to capture (sink = its monitor). Default: default output\n"
        "      --list-sources      print PipeWire audio nodes and exit\n"
        "Presets:\n"
        "  -p, --preset-dir DIR    preset/texture directory (repeatable). Default: <exe>/../presets/*\n"
        "      --preset STR        start with first preset whose name contains STR\n"
        "      --duration SEC      preset duration (default 30)\n"
        "      --transition SEC    soft transition time (default 3)\n"
        "      --no-shuffle        play sequentially\n"
        "      --hard-cuts         enable beat-triggered hard cuts\n"
        "      --lock              keep the first preset\n"
        "      --beat-sensitivity F (default 1.0)\n"
        "      --brightness F      output brightness 0..2 (default 1.0; keys [ / ])\n"
        "Misc:\n"
        "      --no-ui             start with the control window hidden (toggle: M or Tab)\n"
        "      --ui-scale F        control window scale (default 1.0)\n"
        "      --list-monitors     print monitors and exit\n"
        "Web UI:\n"
        "      --no-web            disable the web control UI\n"
        "      --web-port N        web UI port (default 8080)\n"
        "      --web-bind ADDR     web UI listen address (default 0.0.0.0 = all interfaces, no auth)\n"
        "Config:\n"
        "      --config FILE       INI config file to load at start and save to (default %s)\n"
        "      --no-config         do not load the config file (saving still goes to --config if given)\n"
        "      --print-layout      parse --layout, print surfaces and exit\n"
        "  -h, --help\n",
        argv0, muq::DefaultConfigPath().empty() ? "(none)" : muq::DefaultConfigPath().c_str());
}

bool ParseSize(const char* s, int& w, int& h)
{
    return std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0;
}

std::string Normalize(const std::string& p)
{
    std::error_code ec;
    fs::path abs = fs::weakly_canonical(fs::absolute(p, ec), ec);
    std::string s = abs.string();
    while (s.size() > 1 && s.back() == '/')
    {
        s.pop_back();
    }
    return s;
}

std::vector<std::string> DefaultPresetDirs()
{
    std::vector<std::string> dirs;
    std::vector<fs::path> roots;
    if (char* base = SDL_GetBasePath())
    {
        roots.emplace_back(fs::path(base) / ".." / "presets");
        roots.emplace_back(fs::path(base) / "presets");
        SDL_free(base);
    }
    if (const char* home = std::getenv("HOME"))
    {
        roots.emplace_back(fs::path(home) / "muq-viz" / "presets");
    }
    for (const auto& root : roots)
    {
        std::error_code ec;
        if (!fs::is_directory(root, ec))
        {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(root, ec))
        {
            if (entry.is_directory() && entry.path().filename().string()[0] != '.')
            {
                dirs.push_back(Normalize(entry.path().string()));
            }
        }
        if (!dirs.empty())
        {
            break;
        }
    }
    std::sort(dirs.begin(), dirs.end());
    return dirs;
}

int ListSources()
{
    muq::AudioPipeWire audio;
    if (!audio.Start())
    {
        std::fprintf(stderr, "PipeWire: %s\n", audio.LastError().c_str());
        return 1;
    }
    std::printf("%-6s %-8s %-60s %s\n", "id", "kind", "node.name (use with --source)", "description");
    for (const auto& n : audio.Nodes())
    {
        std::printf("%-6u %-8s %-60s %s\n", n.id, n.isSink ? "output" : "input", n.name.c_str(), n.description.c_str());
    }
    audio.Stop();
    return 0;
}

int ListMonitors()
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i)
    {
        SDL_Rect r{};
        SDL_GetDisplayBounds(i, &r);
        std::printf("%d: %-24s %dx%d+%d+%d\n", i, SDL_GetDisplayName(i), r.w, r.h, r.x, r.y);
    }
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    muq::Options opt;
    bool modeGiven = false;
    bool layoutGiven = false;
    bool printLayout = false;

    // Config file first (defaults < config < command line).
    bool loadConfig = true;
    std::string configPath = muq::DefaultConfigPath();
    bool configGiven = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--no-config")
            loadConfig = false;
        else if (a == "--config" && i + 1 < argc)
        {
            configPath = argv[++i];
            configGiven = true;
        }
    }
    if (!loadConfig && !configGiven)
    {
        configPath.clear(); // nothing to save to
    }
    if (loadConfig && !configPath.empty())
    {
        std::vector<std::string> warnings;
        const bool loaded = muq::LoadConfig(configPath, opt, warnings);
        for (const auto& w : warnings)
        {
            std::fprintf(stderr, "config %s: %s\n", configPath.c_str(), w.c_str());
        }
        if (loaded)
        {
            std::printf("config: loaded %s\n", configPath.c_str());
        }
        else if (configGiven && warnings.empty())
        {
            std::printf("config: %s does not exist yet (Save config creates it)\n", configPath.c_str());
        }
    }
    opt.configPath = configPath;
    const std::vector<std::string> configPresetDirs = opt.presetDirs;
    opt.presetDirs.clear();

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "missing value for %s\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help")
        {
            Usage(argv[0]);
            return 0;
        }
        else if (a == "-m" || a == "--mode")
        {
            const std::string m = next();
            modeGiven = true;
            if (!muq::ParseMode(m, opt.mode))
            {
                std::fprintf(stderr, "unknown mode: %s\n", m.c_str());
                return 2;
            }
        }
        else if (a == "-l" || a == "--layout")
        {
            opt.layoutPath = next();
            layoutGiven = true;
        }
        else if (a == "--prefix")
            opt.layout.prefix = next();
        else if (a == "--scene")
            opt.layout.scene = next();
        else if (a == "--canvas")
        {
            if (!ParseSize(next(), opt.layout.canvasWidth, opt.layout.canvasHeight))
            {
                std::fprintf(stderr, "--canvas expects WxH\n");
                return 2;
            }
        }
        else if (a == "--monitor")
            opt.monitor = next();
        else if (a == "--origin")
        {
            muq::Point p{};
            if (std::sscanf(next(), "%d,%d", &p.x, &p.y) != 2)
            {
                std::fprintf(stderr, "--origin expects X,Y\n");
                return 2;
            }
            opt.origin = p;
        }
        else if (a == "--size")
        {
            if (!ParseSize(next(), opt.width, opt.height))
            {
                std::fprintf(stderr, "--size expects WxH\n");
                return 2;
            }
        }
        else if (a == "--no-override-redirect")
            opt.overrideRedirect = false;
        else if (a == "--vsync")
            opt.vsync = true;
        else if (a == "--fps")
            opt.fps = std::max(1, std::atoi(next()));
        else if (a == "-s" || a == "--source")
            opt.source = std::string(next());
        else if (a == "--list-sources")
            return ListSources();
        else if (a == "--list-monitors")
            return ListMonitors();
        else if (a == "-p" || a == "--preset-dir")
            opt.presetDirs.push_back(Normalize(next()));
        else if (a == "--preset")
            opt.startPreset = next();
        else if (a == "--duration")
            opt.presetDuration = std::atof(next());
        else if (a == "--transition")
            opt.softCut = std::atof(next());
        else if (a == "--no-shuffle")
            opt.shuffle = false;
        else if (a == "--hard-cuts")
            opt.hardCuts = true;
        else if (a == "--lock")
            opt.lock = true;
        else if (a == "--beat-sensitivity")
            opt.beatSensitivity = static_cast<float>(std::atof(next()));
        else if (a == "--brightness")
            opt.brightness = std::clamp(static_cast<float>(std::atof(next())), 0.0f, 2.0f);
        else if (a == "--no-ui")
            opt.showUi = false;
        else if (a == "--ui-scale")
            opt.uiScale = std::max(0.5f, static_cast<float>(std::atof(next())));
        else if (a == "--print-layout")
            printLayout = true;
        else if (a == "--no-web")
            opt.webEnabled = false;
        else if (a == "--web-port")
        {
            const int port = std::atoi(next());
            if (port < 1 || port > 65535)
            {
                std::fprintf(stderr, "--web-port expects 1..65535\n");
                return 2;
            }
            opt.webPort = port;
        }
        else if (a == "--web-bind")
            opt.webBind = next();
        else if (a == "--config")
            ++i; // handled above
        else if (a == "--no-config")
            ;    // handled above
        else
        {
            std::fprintf(stderr, "unknown option: %s (see --help)\n", a.c_str());
            return 2;
        }
    }

    if (printLayout)
    {
        if (opt.layoutPath.empty())
        {
            std::fprintf(stderr, "--print-layout needs --layout FILE\n");
            return 2;
        }
        try
        {
            auto layout = muq::LoadObsLayout(opt.layoutPath, opt.layout);
            std::printf("scene \"%s\": %zu surface(s), canvas %dx%d @ %d,%d\n", layout.sceneName.c_str(),
                        layout.surfaces.size(), layout.bounds.w, layout.bounds.h, layout.bounds.x, layout.bounds.y);
            for (const auto& s : layout.surfaces)
            {
                std::printf("  %-28s %5dx%-5d @ %d,%d\n", s.name.c_str(), s.rect.w, s.rect.h, s.rect.x, s.rect.y);
            }
            for (const auto& w : layout.warnings)
            {
                std::printf("  warning: %s\n", w.c_str());
            }
            return 0;
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "layout: %s\n", e.what());
            return 1;
        }
    }

    if (layoutGiven && !modeGiven)
    {
        opt.mode = muq::OutputMode::Layout;
    }
    if (opt.presetDirs.empty())
    {
        // Command line -p replaces the config list; otherwise config, otherwise auto-detect.
        for (const auto& d : configPresetDirs)
        {
            opt.presetDirs.push_back(Normalize(d));
        }
    }
    if (opt.presetDirs.empty())
    {
        opt.presetDirs = DefaultPresetDirs();
        opt.presetDirsAuto = true;
    }
    if (opt.presetDirs.empty())
    {
        std::fprintf(stderr, "warning: no preset directory found (use --preset-dir)\n");
    }

    muq::App app(std::move(opt));
    return app.Run();
}
