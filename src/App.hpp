#pragma once

#include "AudioPipeWire.hpp"
#include "ObsLayout.hpp"

#include <SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

struct projectm;
struct projectm_playlist;

namespace muq {

enum class OutputMode
{
    Window,
    Fullscreen,
    Layout
};

const char* ModeName(OutputMode mode);

struct Options
{
    OutputMode mode = OutputMode::Window;
    std::string layoutPath;
    LayoutOptions layout;
    std::string monitor;              // display index or name substring; empty = 0
    std::optional<SDL_Point> origin;  // explicit layout origin (overrides monitor)
    int width = 1280;
    int height = 720;
    std::vector<std::string> presetDirs;
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
};

struct Output
{
    std::string name;
    SDL_Window* window = nullptr;
    Rect source;     // area of the canvas shown by this window (canvas coords, top-left origin)
    Rect placement;  // requested screen geometry
};

class App
{
public:
    explicit App(Options options);
    ~App();

    int Run();

private:
    bool Init();
    void Shutdown();

    bool InitProjectM();
    void LoadPresets();
    bool LoadLayout(const std::string& path);
    void BuildOutputs(OutputMode mode);
    void DestroyOutputs();
    int ResolveMonitor() const;
    void EnsureCanvas(int width, int height);
    bool InitBlitShader();

    void FeedAudio();
    void RenderOutputs();
    void DrawUi();
    void HandleEvent(const SDL_Event& event);
    void ToggleUi();

    void PlayIndex(uint32_t index);
    void PlayRandom();
    void RefilterPresets();

    static void OnPresetSwitched(bool hardCut, unsigned int index, void* user);
    static void OnPresetFailed(const char* file, const char* message, void* user);

    Options m_opt;
    AudioPipeWire m_audio;
    bool m_audioOk = false;

    SDL_Window* m_control = nullptr;
    SDL_GLContext m_gl = nullptr;
    std::vector<Output> m_outputs;
    OutputMode m_mode = OutputMode::Window;
    std::optional<Layout> m_layout;
    std::string m_layoutError;

    unsigned int m_fbo = 0;
    unsigned int m_fboTex = 0;
    int m_canvasW = 0;
    int m_canvasH = 0;

    unsigned int m_blitProg = 0;
    unsigned int m_blitVao = 0;
    int m_locRect = -1;
    int m_locGain = -1;

    projectm* m_pm = nullptr;
    projectm_playlist* m_playlist = nullptr;
    std::vector<std::string> m_presetPaths;
    std::vector<std::string> m_presetLabels;
    std::vector<uint32_t> m_filtered;
    uint32_t m_current = 0;
    bool m_scrollToCurrent = true;
    unsigned m_failures = 0;
    std::string m_lastFailure;

    std::vector<float> m_pcm;

    bool m_running = true;
    bool m_uiVisible = true;
    bool m_rebuildRequested = false;
    OutputMode m_requestedMode = OutputMode::Window;

    char m_search[256] = {};
    char m_layoutPathBuf[1024] = {};
    int m_uiMonitor = 0;

    double m_fpsSmoothed = 0.0;
};

} // namespace muq
