#pragma once

#include "AudioPipeWire.hpp"
#include "ObsLayout.hpp"
#include "Options.hpp"
#include "WebServer.hpp"

#include <SDL.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct projectm;
struct projectm_playlist;

namespace muq {

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

    // Actions shared by the ImGui panel, keyboard and the web UI.
    void PlayIndex(uint32_t index);
    void PlayRandom();
    void PlayNext();
    void PlayPrev();
    void SetLock(bool lock);
    void SetShuffle(bool shuffle);
    void SetSmooth(bool smooth);
    void SetHardCuts(bool hardCuts);
    void SetDuration(double seconds);
    void SetSoftCut(double seconds);
    void SetBeatSensitivity(float sensitivity);
    void SetBrightness(float brightness);
    void SelectSource(const std::string& nodeName);
    void RequestMode(OutputMode mode);
    void SelectMonitor(int index);
    bool LoadLayoutAndSwitch(const std::string& path);
    void SetUiVisible(bool visible);
    void SaveConfig();

    void RefilterPresets();

    // Web UI bridge (render thread).
    void StartWeb();
    void ProcessWebCommands();
    void ApplyWebCommand(const nlohmann::json& cmd);
    void PublishState();
    void PublishPresets();

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

    std::unique_ptr<WebServer> m_web;
    std::chrono::steady_clock::time_point m_lastPublish{};
    unsigned m_presetsVersion = 0;
    std::string m_configStatus;
};

} // namespace muq
