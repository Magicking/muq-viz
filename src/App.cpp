#include "App.hpp"
#include "Config.hpp"

#include <projectM-4/playlist.h>
#include <projectM-4/projectM.h>

#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

namespace fs = std::filesystem;

namespace muq {

namespace {

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string NodeLabel(const AudioNode& n)
{
    return (n.isSink ? "[Output] Monitor of " : "[Input] ") + n.description;
}

} // namespace

App::App(Options options)
    : m_opt(std::move(options))
{
    m_uiVisible = m_opt.showUi;
    std::snprintf(m_layoutPathBuf, sizeof(m_layoutPathBuf), "%s", m_opt.layoutPath.c_str());
}

App::~App()
{
    Shutdown();
}

bool App::Init()
{
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
    {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    if (!m_opt.layoutPath.empty())
    {
        if (!LoadLayout(m_opt.layoutPath))
        {
            // Keep running (window mode) so the panel / web UI can load another layout.
            std::fprintf(stderr, "layout: %s\n", m_layoutError.c_str());
        }
    }

    // Control window: owns the GL context, shared with every output window.
    const int monitor = ResolveMonitor();
    m_uiMonitor = monitor;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!m_uiVisible)
    {
        flags |= SDL_WINDOW_HIDDEN;
    }
    // Put the control panel on a monitor that is not used for output when possible.
    int uiDisplay = 0;
    if (SDL_GetNumVideoDisplays() > 1 && m_opt.mode != OutputMode::Window)
    {
        uiDisplay = monitor == 0 ? 1 : 0;
    }
    const int uiW = static_cast<int>(560 * m_opt.uiScale);
    const int uiH = static_cast<int>(820 * m_opt.uiScale);
    // Same monitor as the output (window mode / single display): dock the panel on the left edge.
    SDL_Rect uiBounds{0, 0, 0, 0};
    SDL_GetDisplayUsableBounds(uiDisplay, &uiBounds);
    const bool dockLeft = uiDisplay == monitor;
    const int uiX = dockLeft ? uiBounds.x + 16 : static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(uiDisplay));
    const int uiY = dockLeft ? uiBounds.y + 40 : static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(uiDisplay));
    m_control = SDL_CreateWindow("muq-viz control", uiX, uiY, uiW, uiH, flags);
    if (!m_control)
    {
        std::fprintf(stderr, "SDL_CreateWindow(control): %s\n", SDL_GetError());
        return false;
    }
    m_gl = SDL_GL_CreateContext(m_control);
    if (!m_gl)
    {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(m_control, m_gl);
    SDL_GL_SetSwapInterval(0);
    std::printf("OpenGL: %s / %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = m_opt.uiScale;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(m_opt.uiScale);
    ImGui_ImplSDL2_InitForOpenGL(m_control, m_gl);
    ImGui_ImplOpenGL3_Init("#version 330");

    if (!InitBlitShader())
    {
        std::fprintf(stderr, "brightness shader unavailable, using plain blit\n");
    }

    if (!InitProjectM())
    {
        return false;
    }
    PublishPresets();

    m_audioOk = m_audio.Start();
    if (!m_audioOk)
    {
        std::fprintf(stderr, "audio: %s\n", m_audio.LastError().c_str());
    }
    else if (m_opt.source)
    {
        m_audio.Select(*m_opt.source);
    }
    m_pcm.resize(4096 * AudioPipeWire::Channels);

    BuildOutputs(m_opt.mode);
    StartWeb();
    PublishState();
    return !m_outputs.empty();
}

namespace {

GLuint CompileShader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile: %s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

} // namespace

// Output pass: copies a canvas slice to the current window, scaled by brightness.
bool App::InitBlitShader()
{
    static const char* vs = R"(#version 330 core
out vec2 uv;
void main()
{
    // Fullscreen triangle from vertex id, no vertex buffer needed.
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
    static const char* fs = R"(#version 330 core
in vec2 uv;
out vec4 color;
uniform sampler2D tex;
uniform vec4 rect; // x, y, w, h in normalized texture coords (bottom-left origin)
uniform float gain;
void main()
{
    color = vec4(texture(tex, rect.xy + uv * rect.zw).rgb * gain, 1.0);
}
)";
    GLuint v = CompileShader(GL_VERTEX_SHADER, vs);
    GLuint f = CompileShader(GL_FRAGMENT_SHADER, fs);
    if (!v || !f)
    {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return false;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok)
    {
        char log[1024] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader link: %s\n", log);
        glDeleteProgram(prog);
        return false;
    }
    m_blitProg = prog;
    m_locRect = glGetUniformLocation(prog, "rect");
    m_locGain = glGetUniformLocation(prog, "gain");
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "tex"), 0);
    glUseProgram(0);
    glGenVertexArrays(1, &m_blitVao);
    return true;
}

bool App::InitProjectM()
{
    m_pm = projectm_create();
    if (!m_pm)
    {
        std::fprintf(stderr, "projectm_create failed (OpenGL 3.3 core required)\n");
        return false;
    }
    projectm_set_fps(m_pm, m_opt.fps);
    projectm_set_preset_duration(m_pm, m_opt.presetDuration);
    projectm_set_soft_cut_duration(m_pm, m_opt.softCut);
    projectm_set_hard_cut_enabled(m_pm, m_opt.hardCuts);
    projectm_set_beat_sensitivity(m_pm, m_opt.beatSensitivity);
    projectm_set_preset_locked(m_pm, m_opt.lock);
    projectm_set_aspect_correction(m_pm, true);

    std::vector<const char*> texPaths;
    for (const auto& d : m_opt.presetDirs)
    {
        texPaths.push_back(d.c_str());
    }
    if (!texPaths.empty())
    {
        projectm_set_texture_search_paths(m_pm, texPaths.data(), texPaths.size());
    }

    m_playlist = projectm_playlist_create(m_pm);
    projectm_playlist_set_shuffle(m_playlist, m_opt.shuffle);
    projectm_playlist_set_retry_count(m_playlist, 5);
    projectm_playlist_set_preset_switched_event_callback(m_playlist, &App::OnPresetSwitched, this);
    projectm_playlist_set_preset_switch_failed_event_callback(m_playlist, &App::OnPresetFailed, this);
    LoadPresets();
    return true;
}

void App::LoadPresets()
{
    for (const auto& dir : m_opt.presetDirs)
    {
        const uint32_t added = projectm_playlist_add_path(m_playlist, dir.c_str(), true, false);
        std::printf("presets: %u from %s\n", added, dir.c_str());
    }
    // Sort so the browser groups by pack/category; shuffle still picks randomly.
    projectm_playlist_sort(m_playlist, 0, projectm_playlist_size(m_playlist), SORT_PREDICATE_FULL_PATH,
                           SORT_ORDER_ASCENDING);

    const uint32_t count = projectm_playlist_size(m_playlist);
    m_presetPaths.clear();
    m_presetLabels.clear();
    if (count > 0)
    {
        char** items = projectm_playlist_items(m_playlist, 0, count);
        for (uint32_t i = 0; items && items[i]; ++i)
        {
            std::string path = items[i];
            std::string label = path;
            for (const auto& dir : m_opt.presetDirs)
            {
                std::string prefix = fs::path(dir).parent_path().string();
                if (path.compare(0, dir.size(), dir) == 0 && !prefix.empty())
                {
                    label = path.substr(prefix.size() + 1);
                    break;
                }
            }
            if (label.size() > 5 && label.compare(label.size() - 5, 5, ".milk") == 0)
            {
                label.resize(label.size() - 5);
            }
            m_presetPaths.push_back(std::move(path));
            m_presetLabels.push_back(std::move(label));
        }
        projectm_playlist_free_string_array(items);
    }
    RefilterPresets();

    if (count == 0)
    {
        std::fprintf(stderr, "presets: none found, showing idle preset\n");
        return;
    }
    uint32_t start = 0;
    bool found = false;
    if (!m_opt.startPreset.empty())
    {
        // Exact label (as saved in the config) first, then substring match.
        for (uint32_t i = 0; i < m_presetLabels.size() && !found; ++i)
        {
            if (m_presetLabels[i] == m_opt.startPreset)
            {
                start = i;
                found = true;
            }
        }
        const std::string needle = Lower(m_opt.startPreset);
        for (uint32_t i = 0; i < m_presetLabels.size() && !found; ++i)
        {
            if (Lower(m_presetLabels[i]).find(needle) != std::string::npos)
            {
                start = i;
                found = true;
            }
        }
    }
    if (found)
    {
        PlayIndex(start);
    }
    else if (m_opt.shuffle)
    {
        PlayRandom();
    }
    else
    {
        PlayIndex(0);
    }
}

bool App::LoadLayout(const std::string& path)
{
    try
    {
        Layout layout = LoadObsLayout(path, m_opt.layout);
        std::printf("layout: %s, scene \"%s\", %zu surface(s), canvas %dx%d @ %d,%d\n", path.c_str(),
                    layout.sceneName.c_str(), layout.surfaces.size(), layout.bounds.w, layout.bounds.h,
                    layout.bounds.x, layout.bounds.y);
        for (const auto& s : layout.surfaces)
        {
            std::printf("  %-28s %5dx%-5d @ %d,%d\n", s.name.c_str(), s.rect.w, s.rect.h, s.rect.x, s.rect.y);
        }
        for (const auto& w : layout.warnings)
        {
            std::printf("  warning: %s\n", w.c_str());
        }
        m_layout = std::move(layout);
        m_layoutError.clear();
        m_opt.layoutPath = path;
        return true;
    }
    catch (const std::exception& e)
    {
        m_layoutError = e.what();
        return false;
    }
}

int App::ResolveMonitor() const
{
    const int count = SDL_GetNumVideoDisplays();
    if (m_opt.monitor.empty() || count <= 0)
    {
        return 0;
    }
    char* end = nullptr;
    const long idx = std::strtol(m_opt.monitor.c_str(), &end, 10);
    if (end && *end == '\0')
    {
        return (idx >= 0 && idx < count) ? static_cast<int>(idx) : 0;
    }
    const std::string needle = Lower(m_opt.monitor);
    for (int i = 0; i < count; ++i)
    {
        const char* name = SDL_GetDisplayName(i);
        if (name && Lower(name).find(needle) != std::string::npos)
        {
            return i;
        }
    }
    std::fprintf(stderr, "monitor \"%s\" not found, using 0\n", m_opt.monitor.c_str());
    return 0;
}

void App::BuildOutputs(OutputMode mode)
{
    DestroyOutputs();
    if (mode == OutputMode::Layout && !m_layout)
    {
        std::fprintf(stderr, "no layout loaded, falling back to window mode\n");
        mode = OutputMode::Window;
    }
    m_mode = mode;

    const int monitor = ResolveMonitor();
    SDL_Rect display{0, 0, 1920, 1080};
    SDL_GetDisplayBounds(monitor, &display);

    auto create = [&](const std::string& name, const Rect& place, Uint32 flags) -> SDL_Window* {
        const std::string title = "muq-viz: " + name;
        SDL_Window* win = SDL_CreateWindow(title.c_str(), place.x, place.y, place.w, place.h,
                                           SDL_WINDOW_OPENGL | flags);
        if (!win)
        {
            std::fprintf(stderr, "SDL_CreateWindow(%s): %s\n", name.c_str(), SDL_GetError());
            return nullptr;
        }
        SDL_GL_MakeCurrent(win, m_gl);
        // Only the first output waits for vblank, otherwise N windows would divide the frame rate.
        SDL_GL_SetSwapInterval(m_opt.vsync && m_outputs.empty() ? 1 : 0);
        return win;
    };

    switch (mode)
    {
        case OutputMode::Window:
        {
            Rect place{static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(monitor)),
                       static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(monitor)), m_opt.width, m_opt.height};
            if (SDL_Window* w = create("window", place, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI))
            {
                m_outputs.push_back({"window", w, {0, 0, m_opt.width, m_opt.height}, place});
            }
            break;
        }
        case OutputMode::Fullscreen:
        {
            Rect place{display.x, display.y, display.w, display.h};
            if (SDL_Window* w = create("fullscreen", place, SDL_WINDOW_BORDERLESS))
            {
                SDL_SetWindowFullscreen(w, SDL_WINDOW_FULLSCREEN_DESKTOP);
                SDL_ShowCursor(SDL_DISABLE);
                m_outputs.push_back({"fullscreen", w, {0, 0, display.w, display.h}, place});
            }
            break;
        }
        case OutputMode::Layout:
        {
            const Point origin = m_opt.origin ? *m_opt.origin : Point{display.x, display.y};
            const Rect& b = m_layout->bounds;
            if (m_opt.overrideRedirect)
            {
                // Bypass the window manager so it cannot move/resize/decorate surfaces.
                SDL_SetHint(SDL_HINT_X11_FORCE_OVERRIDE_REDIRECT, "1");
            }
            for (const auto& s : m_layout->surfaces)
            {
                Rect place{origin.x + s.rect.x, origin.y + s.rect.y, s.rect.w, s.rect.h};
                SDL_Window* w = create(s.name, place,
                                       SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_SKIP_TASKBAR);
                if (!w)
                {
                    continue;
                }
                SDL_SetWindowPosition(w, place.x, place.y);
                m_outputs.push_back({s.name, w, {s.rect.x - b.x, s.rect.y - b.y, s.rect.w, s.rect.h}, place});
            }
            SDL_SetHint(SDL_HINT_X11_FORCE_OVERRIDE_REDIRECT, "0");
            break;
        }
    }

    if (m_control)
    {
        SDL_RaiseWindow(m_control);
    }
    std::printf("output: %s, %zu window(s)\n", ModeName(m_mode), m_outputs.size());
}

void App::DestroyOutputs()
{
    if (m_control && m_gl)
    {
        SDL_GL_MakeCurrent(m_control, m_gl);
    }
    for (auto& o : m_outputs)
    {
        SDL_DestroyWindow(o.window);
    }
    m_outputs.clear();
    SDL_ShowCursor(SDL_ENABLE);
}

void App::EnsureCanvas(int width, int height)
{
    width = std::max(1, width);
    height = std::max(1, height);
    if (m_fbo && width == m_canvasW && height == m_canvasH)
    {
        return;
    }
    if (!m_fbo)
    {
        glGenFramebuffers(1, &m_fbo);
        glGenTextures(1, &m_fboTex);
    }
    glBindTexture(GL_TEXTURE_2D, m_fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fboTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    {
        std::fprintf(stderr, "canvas framebuffer incomplete\n");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_canvasW = width;
    m_canvasH = height;
    projectm_set_window_size(m_pm, static_cast<size_t>(width), static_cast<size_t>(height));
}

void App::FeedAudio()
{
    if (!m_audioOk)
    {
        return;
    }
    const size_t maxFrames = std::min<size_t>(projectm_pcm_get_max_samples(), m_pcm.size() / AudioPipeWire::Channels);
    for (int guard = 0; guard < 64; ++guard)
    {
        const size_t n = m_audio.Read(m_pcm.data(), maxFrames);
        if (n == 0)
        {
            break;
        }
        projectm_pcm_add_float(m_pm, m_pcm.data(), static_cast<unsigned int>(n), PROJECTM_STEREO);
    }
}

void App::RenderOutputs()
{
    if (m_outputs.empty())
    {
        return;
    }

    // Canvas size: layout bbox, or the single output's drawable size.
    if (m_mode == OutputMode::Layout)
    {
        EnsureCanvas(m_layout->bounds.w, m_layout->bounds.h);
    }
    else
    {
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(m_outputs[0].window, &w, &h);
        m_outputs[0].source = {0, 0, w, h};
        EnsureCanvas(w, h);
    }

    SDL_GL_MakeCurrent(m_outputs[0].window, m_gl);
    projectm_opengl_render_frame_fbo(m_pm, m_fbo);

    for (auto& o : m_outputs)
    {
        SDL_GL_MakeCurrent(o.window, m_gl);
        int dw = 0, dh = 0;
        SDL_GL_GetDrawableSize(o.window, &dw, &dh);
        glDisable(GL_SCISSOR_TEST);
        glViewport(0, 0, dw, dh);
        // Canvas rects are top-left based, GL framebuffers are bottom-left based.
        const Rect& s = o.source;
        const int y0 = m_canvasH - (s.y + s.h);
        if (m_blitProg)
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glUseProgram(m_blitProg);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m_fboTex);
            const float cw = static_cast<float>(m_canvasW);
            const float ch = static_cast<float>(m_canvasH);
            glUniform4f(m_locRect, s.x / cw, y0 / ch, s.w / cw, s.h / ch);
            glUniform1f(m_locGain, m_opt.brightness);
            glBindVertexArray(m_blitVao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glBindTexture(GL_TEXTURE_2D, 0);
            glUseProgram(0);
        }
        else
        {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(s.x, y0, s.x + s.w, y0 + s.h, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        SDL_GL_SwapWindow(o.window);
    }
}

void App::PlayIndex(uint32_t index)
{
    if (index < m_presetPaths.size())
    {
        projectm_playlist_set_position(m_playlist, index, !m_opt.smooth);
    }
}

void App::PlayRandom()
{
    if (m_presetPaths.empty())
    {
        return;
    }
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist(0, static_cast<uint32_t>(m_presetPaths.size() - 1));
    PlayIndex(dist(rng));
}

void App::PlayNext()
{
    projectm_playlist_play_next(m_playlist, !m_opt.smooth);
}

void App::PlayPrev()
{
    projectm_playlist_play_previous(m_playlist, !m_opt.smooth);
}

void App::SetLock(bool lock)
{
    m_opt.lock = lock;
    projectm_set_preset_locked(m_pm, lock);
}

void App::SetShuffle(bool shuffle)
{
    m_opt.shuffle = shuffle;
    projectm_playlist_set_shuffle(m_playlist, shuffle);
}

void App::SetSmooth(bool smooth)
{
    m_opt.smooth = smooth;
}

void App::SetHardCuts(bool hardCuts)
{
    m_opt.hardCuts = hardCuts;
    projectm_set_hard_cut_enabled(m_pm, hardCuts);
}

void App::SetDuration(double seconds)
{
    m_opt.presetDuration = std::clamp(seconds, 5.0, 300.0);
    projectm_set_preset_duration(m_pm, m_opt.presetDuration);
}

void App::SetSoftCut(double seconds)
{
    m_opt.softCut = std::clamp(seconds, 0.0, 10.0);
    projectm_set_soft_cut_duration(m_pm, m_opt.softCut);
}

void App::SetBeatSensitivity(float sensitivity)
{
    m_opt.beatSensitivity = std::clamp(sensitivity, 0.0f, 5.0f);
    projectm_set_beat_sensitivity(m_pm, m_opt.beatSensitivity);
}

void App::SetBrightness(float brightness)
{
    m_opt.brightness = std::clamp(brightness, 0.0f, 2.0f);
}

void App::SelectSource(const std::string& nodeName)
{
    if (m_audioOk)
    {
        m_audio.Select(nodeName);
    }
    m_opt.source = nodeName;
}

void App::RequestMode(OutputMode mode)
{
    if (mode == OutputMode::Layout && !m_layout)
    {
        return;
    }
    m_rebuildRequested = true;
    m_requestedMode = mode;
}

void App::SelectMonitor(int index)
{
    if (index < 0 || index >= SDL_GetNumVideoDisplays())
    {
        return;
    }
    m_opt.monitor = std::to_string(index);
    m_opt.origin.reset();
    if (!m_rebuildRequested)
    {
        m_requestedMode = m_mode;
    }
    m_rebuildRequested = true;
}

bool App::LoadLayoutAndSwitch(const std::string& path)
{
    std::snprintf(m_layoutPathBuf, sizeof(m_layoutPathBuf), "%s", path.c_str());
    if (!LoadLayout(path))
    {
        return false;
    }
    RequestMode(OutputMode::Layout);
    return true;
}

void App::SetUiVisible(bool visible)
{
    m_uiVisible = visible;
    if (visible)
    {
        SDL_ShowWindow(m_control);
        SDL_RaiseWindow(m_control);
    }
    else
    {
        SDL_HideWindow(m_control);
    }
}

void App::SaveConfig()
{
    if (m_opt.configPath.empty())
    {
        m_configStatus = "no config path (started with --no-config and no --config)";
        return;
    }
    Options snapshot = m_opt;
    snapshot.mode = m_rebuildRequested ? m_requestedMode : m_mode;
    snapshot.showUi = m_uiVisible;
    if (!snapshot.layoutPath.empty())
    {
        std::error_code ec;
        const fs::path abs = fs::absolute(snapshot.layoutPath, ec);
        if (!ec)
        {
            snapshot.layoutPath = abs.lexically_normal().string();
        }
    }
    if (m_audioOk)
    {
        snapshot.source = m_audio.Selected();
    }
    if (m_current < m_presetLabels.size())
    {
        snapshot.startPreset = m_presetLabels[m_current];
    }
    std::string err;
    if (muq::SaveConfig(m_opt.configPath, snapshot, err))
    {
        m_configStatus = "saved to " + m_opt.configPath;
        std::printf("config: %s\n", m_configStatus.c_str());
    }
    else
    {
        m_configStatus = "save failed: " + err;
        std::fprintf(stderr, "config: %s\n", m_configStatus.c_str());
    }
}

void App::RefilterPresets()
{
    m_filtered.clear();
    const std::string needle = Lower(m_search);
    for (uint32_t i = 0; i < m_presetLabels.size(); ++i)
    {
        if (needle.empty() || Lower(m_presetLabels[i]).find(needle) != std::string::npos)
        {
            m_filtered.push_back(i);
        }
    }
}

void App::OnPresetSwitched(bool /*hardCut*/, unsigned int index, void* user)
{
    auto* self = static_cast<App*>(user);
    self->m_current = index;
    self->m_scrollToCurrent = true;
    if (index < self->m_presetLabels.size())
    {
        std::printf("preset: %s\n", self->m_presetLabels[index].c_str());
        std::fflush(stdout);
    }
}

void App::OnPresetFailed(const char* file, const char* message, void* user)
{
    auto* self = static_cast<App*>(user);
    ++self->m_failures;
    self->m_lastFailure = std::string(file ? file : "?") + ": " + (message ? message : "");
    std::fprintf(stderr, "preset failed: %s\n", self->m_lastFailure.c_str());
}

void App::ToggleUi()
{
    SetUiVisible(!m_uiVisible);
}

void App::HandleEvent(const SDL_Event& e)
{
    ImGui_ImplSDL2_ProcessEvent(&e);
    const Uint32 controlId = SDL_GetWindowID(m_control);

    switch (e.type)
    {
        case SDL_QUIT:
            m_running = false;
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_CLOSE)
            {
                m_running = false;
            }
            break;
        case SDL_KEYDOWN:
        {
            if (e.key.windowID == controlId && ImGui::GetIO().WantCaptureKeyboard)
            {
                break;
            }
            const auto mod = e.key.keysym.mod;
            switch (e.key.keysym.sym)
            {
                case SDLK_n:
                    PlayNext();
                    break;
                case SDLK_p:
                    PlayPrev();
                    break;
                case SDLK_r:
                    PlayRandom();
                    break;
                case SDLK_l:
                    SetLock(!m_opt.lock);
                    break;
                case SDLK_s:
                    if (mod & KMOD_CTRL)
                    {
                        SaveConfig();
                    }
                    break;
                case SDLK_LEFTBRACKET:
                    SetBrightness(m_opt.brightness - 0.05f);
                    break;
                case SDLK_RIGHTBRACKET:
                    SetBrightness(m_opt.brightness + 0.05f);
                    break;
                case SDLK_m:
                case SDLK_TAB:
                    ToggleUi();
                    break;
                case SDLK_f:
                    if (m_mode != OutputMode::Layout)
                    {
                        RequestMode(m_mode == OutputMode::Fullscreen ? OutputMode::Window : OutputMode::Fullscreen);
                    }
                    break;
                case SDLK_q:
                    if (mod & KMOD_CTRL || e.key.windowID != controlId)
                    {
                        m_running = false;
                    }
                    break;
                case SDLK_ESCAPE:
                    if (e.key.windowID != controlId)
                    {
                        m_running = false;
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }
}

void App::DrawUi()
{
    if (!m_uiVisible || (SDL_GetWindowFlags(m_control) & SDL_WINDOW_MINIMIZED))
    {
        return;
    }
    SDL_GL_MakeCurrent(m_control, m_gl);
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("muq-viz", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- Audio input
    if (ImGui::CollapsingHeader("Audio input (PipeWire)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (!m_audioOk)
        {
            ImGui::TextColored({1, 0.4f, 0.4f, 1}, "PipeWire unavailable: %s", m_audio.LastError().c_str());
        }
        else
        {
            const auto nodes = m_audio.Nodes();
            const std::string selected = m_audio.Selected();
            std::string preview = "Default output (monitor)";
            for (const auto& n : nodes)
            {
                if (n.name == selected)
                {
                    preview = NodeLabel(n);
                }
            }
            if (!selected.empty() && preview == "Default output (monitor)")
            {
                preview = selected + " (not present)";
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##source", preview.c_str()))
            {
                if (ImGui::Selectable("Default output (monitor)", selected.empty()))
                {
                    SelectSource("");
                }
                for (const auto& n : nodes)
                {
                    ImGui::PushID(static_cast<int>(n.id));
                    if (ImGui::Selectable(NodeLabel(n).c_str(), n.name == selected))
                    {
                        SelectSource(n.name);
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s\n%s (id %u)", n.name.c_str(), n.mediaClass.c_str(), n.id);
                    }
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            const float peak = std::min(1.0f, m_audio.Peak());
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%s  %u Hz  peak %.2f",
                          m_audio.Streaming() ? "streaming" : "waiting", m_audio.Rate(), peak);
            ImGui::ProgressBar(peak, ImVec2(-1, 0), overlay);
            const std::string err = m_audio.LastError();
            if (!err.empty())
            {
                ImGui::TextColored({1, 0.6f, 0.3f, 1}, "%s", err.c_str());
            }
        }
    }

    // ---- Presets
    if (ImGui::CollapsingHeader("Presets", ImGuiTreeNodeFlags_DefaultOpen))
    {
        const char* current = m_current < m_presetLabels.size() ? m_presetLabels[m_current].c_str() : "(idle)";
        ImGui::TextWrapped("Now: %s", current);
        if (ImGui::Button("<< Prev"))
        {
            PlayPrev();
        }
        ImGui::SameLine();
        if (ImGui::Button("Next >>"))
        {
            PlayNext();
        }
        ImGui::SameLine();
        if (ImGui::Button("Random"))
        {
            PlayRandom();
        }
        ImGui::SameLine();
        bool lock = m_opt.lock;
        if (ImGui::Checkbox("Lock", &lock))
        {
            SetLock(lock);
        }
        ImGui::SameLine();
        bool shuffle = m_opt.shuffle;
        if (ImGui::Checkbox("Shuffle", &shuffle))
        {
            SetShuffle(shuffle);
        }
        bool smooth = m_opt.smooth;
        if (ImGui::Checkbox("Smooth transitions", &smooth))
        {
            SetSmooth(smooth);
        }
        ImGui::SameLine();
        bool hardCuts = m_opt.hardCuts;
        if (ImGui::Checkbox("Beat hard cuts", &hardCuts))
        {
            SetHardCuts(hardCuts);
        }
        float duration = static_cast<float>(m_opt.presetDuration);
        if (ImGui::SliderFloat("Duration (s)", &duration, 5.0f, 300.0f, "%.0f"))
        {
            SetDuration(duration);
        }
        float soft = static_cast<float>(m_opt.softCut);
        if (ImGui::SliderFloat("Transition (s)", &soft, 0.0f, 10.0f, "%.1f"))
        {
            SetSoftCut(soft);
        }
        float beat = m_opt.beatSensitivity;
        if (ImGui::SliderFloat("Beat sensitivity", &beat, 0.0f, 5.0f, "%.2f"))
        {
            SetBeatSensitivity(beat);
        }

        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##search", "search presets...", m_search, sizeof(m_search)))
        {
            RefilterPresets();
            m_scrollToCurrent = false;
        }
        ImGui::Text("%zu / %zu presets", m_filtered.size(), m_presetPaths.size());
        if (ImGui::BeginChild("##presetlist", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 14),
                              ImGuiChildFlags_Borders))
        {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(m_filtered.size()));
            if (m_scrollToCurrent)
            {
                auto it = std::find(m_filtered.begin(), m_filtered.end(), m_current);
                if (it != m_filtered.end())
                {
                    clipper.IncludeItemByIndex(static_cast<int>(it - m_filtered.begin()));
                }
            }
            while (clipper.Step())
            {
                for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
                {
                    const uint32_t idx = m_filtered[row];
                    ImGui::PushID(static_cast<int>(idx));
                    const bool isCurrent = idx == m_current;
                    if (ImGui::Selectable(m_presetLabels[idx].c_str(), isCurrent, ImGuiSelectableFlags_AllowDoubleClick))
                    {
                        PlayIndex(idx);
                    }
                    if (isCurrent && m_scrollToCurrent)
                    {
                        ImGui::SetScrollHereY(0.5f);
                        m_scrollToCurrent = false;
                    }
                    ImGui::PopID();
                }
            }
        }
        ImGui::EndChild();
        if (m_failures)
        {
            ImGui::TextColored({1, 0.6f, 0.3f, 1}, "%u failed load(s); last: %s", m_failures, m_lastFailure.c_str());
        }
    }

    // ---- Output
    if (ImGui::CollapsingHeader("Output", ImGuiTreeNodeFlags_DefaultOpen))
    {
        int mode = static_cast<int>(m_mode);
        bool changed = false;
        changed |= ImGui::RadioButton("Window", &mode, 0);
        ImGui::SameLine();
        changed |= ImGui::RadioButton("Fullscreen", &mode, 1);
        ImGui::SameLine();
        ImGui::BeginDisabled(!m_layout);
        changed |= ImGui::RadioButton("OBS layout", &mode, 2);
        ImGui::EndDisabled();
        if (changed)
        {
            RequestMode(static_cast<OutputMode>(mode));
        }

        ImGui::BeginDisabled(m_blitProg == 0);
        ImGui::SetNextItemWidth(-160);
        float brightness = m_opt.brightness;
        if (ImGui::SliderFloat("Brightness", &brightness, 0.0f, 2.0f, "%.2f"))
        {
            SetBrightness(brightness);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset##brightness"))
        {
            SetBrightness(1.0f);
        }
        ImGui::EndDisabled();

        const int displays = SDL_GetNumVideoDisplays();
        int monitor = ResolveMonitor();
        std::string monLabel = std::to_string(monitor) + ": " + (SDL_GetDisplayName(monitor) ? SDL_GetDisplayName(monitor) : "?");
        ImGui::SetNextItemWidth(-90);
        if (ImGui::BeginCombo("Monitor", monLabel.c_str()))
        {
            for (int i = 0; i < displays; ++i)
            {
                SDL_Rect r{};
                SDL_GetDisplayBounds(i, &r);
                char label[160];
                std::snprintf(label, sizeof(label), "%d: %s  %dx%d+%d+%d", i,
                              SDL_GetDisplayName(i) ? SDL_GetDisplayName(i) : "?", r.w, r.h, r.x, r.y);
                if (ImGui::Selectable(label, i == monitor))
                {
                    SelectMonitor(i);
                }
            }
            ImGui::EndCombo();
        }

        ImGui::SetNextItemWidth(-90);
        ImGui::InputText("##layoutpath", m_layoutPathBuf, sizeof(m_layoutPathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Load JSON"))
        {
            LoadLayoutAndSwitch(std::string(m_layoutPathBuf));
        }
        if (!m_layoutError.empty())
        {
            ImGui::TextColored({1, 0.4f, 0.4f, 1}, "layout: %s", m_layoutError.c_str());
        }

        ImGui::Text("Canvas %dx%d, %zu window(s)", m_canvasW, m_canvasH, m_outputs.size());
        if (ImGui::BeginTable("##outputs", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Surface");
            ImGui::TableSetupColumn("Screen geometry");
            ImGui::TableSetupColumn("Canvas slice");
            ImGui::TableHeadersRow();
            for (const auto& o : m_outputs)
            {
                int x = 0, y = 0, w = 0, h = 0;
                SDL_GetWindowPosition(o.window, &x, &y);
                SDL_GetWindowSize(o.window, &w, &h);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(o.name.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%dx%d+%d+%d", w, h, x, y);
                ImGui::TableNextColumn();
                ImGui::Text("%dx%d+%d+%d", o.source.w, o.source.h, o.source.x, o.source.y);
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Config"))
    {
        ImGui::TextWrapped("File: %s", m_opt.configPath.empty() ? "(none)" : m_opt.configPath.c_str());
        ImGui::BeginDisabled(m_opt.configPath.empty());
        if (ImGui::Button("Save config"))
        {
            SaveConfig();
        }
        ImGui::EndDisabled();
        if (!m_configStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextWrapped("%s", m_configStatus.c_str());
        }
    }

    if (ImGui::CollapsingHeader("Info"))
    {
        ImGui::Text("%.1f FPS (target %d)", m_fpsSmoothed, m_opt.fps);
        if (m_web && m_web->Running())
        {
            ImGui::Text("Web UI: http://%s:%d/", m_opt.webBind.c_str(), m_opt.webPort);
        }
        else if (m_opt.webEnabled)
        {
            ImGui::TextColored({1, 0.6f, 0.3f, 1}, "Web UI: %s", m_web ? m_web->LastError().c_str() : "off");
        }
        ImGui::TextWrapped("Keys: N next, P prev, R random, L lock, F fullscreen, [ / ] brightness, M/Tab hide panel, "
                           "Ctrl+S save config, Esc (on output) / Ctrl+Q quit");
    }
    ImGui::End();

    ImGui::Render();
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(m_control, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0.08f, 0.08f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(m_control);
}

int App::Run()
{
    if (!Init())
    {
        return 1;
    }

    using clock = std::chrono::steady_clock;
    const auto frameBudget = std::chrono::microseconds(1000000 / std::max(1, m_opt.fps));
    auto last = clock::now();

    while (m_running)
    {
        const auto frameStart = clock::now();
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            HandleEvent(e);
        }
        ProcessWebCommands();
        if (m_rebuildRequested)
        {
            m_rebuildRequested = false;
            BuildOutputs(m_requestedMode);
        }

        FeedAudio();
        RenderOutputs();
        DrawUi();
        if (m_web && clock::now() - m_lastPublish >= std::chrono::milliseconds(100))
        {
            PublishState();
        }

        const auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        if (dt > 0)
        {
            m_fpsSmoothed = m_fpsSmoothed * 0.95 + (1.0 / dt) * 0.05;
        }
        const auto spent = clock::now() - frameStart;
        if (spent < frameBudget)
        {
            std::this_thread::sleep_for(frameBudget - spent);
        }
    }
    return 0;
}

void App::Shutdown()
{
    if (m_web)
    {
        m_web->Stop();
        m_web.reset();
    }
    if (m_control && m_gl)
    {
        SDL_GL_MakeCurrent(m_control, m_gl);
    }
    if (m_playlist)
    {
        projectm_playlist_destroy(m_playlist);
        m_playlist = nullptr;
    }
    if (m_pm)
    {
        projectm_destroy(m_pm);
        m_pm = nullptr;
    }
    if (m_fbo)
    {
        glDeleteFramebuffers(1, &m_fbo);
        glDeleteTextures(1, &m_fboTex);
        m_fbo = 0;
    }
    if (m_blitProg)
    {
        glDeleteProgram(m_blitProg);
        glDeleteVertexArrays(1, &m_blitVao);
        m_blitProg = 0;
        m_blitVao = 0;
    }
    m_audio.Stop();
    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
    DestroyOutputs();
    if (m_gl)
    {
        SDL_GL_DeleteContext(m_gl);
        m_gl = nullptr;
    }
    if (m_control)
    {
        SDL_DestroyWindow(m_control);
        m_control = nullptr;
    }
    SDL_Quit();
}

// ---- Web UI bridge ---------------------------------------------------------

void App::StartWeb()
{
    if (!m_opt.webEnabled)
    {
        return;
    }
    m_web = std::make_unique<WebServer>();
    if (m_web->Start(m_opt.webBind, m_opt.webPort))
    {
        std::printf("web: http://%s:%d/\n", m_opt.webBind.c_str(), m_opt.webPort);
    }
    else
    {
        std::fprintf(stderr, "web: %s (web UI disabled)\n", m_web->LastError().c_str());
    }
    PublishPresets();
}

void App::PublishPresets()
{
    if (!m_web)
    {
        return;
    }
    ++m_presetsVersion;
    nlohmann::json j;
    j["version"] = m_presetsVersion;
    j["labels"] = m_presetLabels;
    m_web->SetPresets(j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
}

void App::ProcessWebCommands()
{
    if (!m_web)
    {
        return;
    }
    auto cmds = m_web->TakeCommands();
    for (const auto& c : cmds)
    {
        try
        {
            ApplyWebCommand(c);
        }
        catch (const std::exception& e)
        {
            std::fprintf(stderr, "web: bad command %s: %s\n", c.dump().c_str(), e.what());
        }
    }
    if (!cmds.empty())
    {
        PublishState(); // reflect the change in the next poll right away
    }
}

void App::ApplyWebCommand(const nlohmann::json& c)
{
    const std::string cmd = c.at("cmd").get<std::string>();
    if (cmd == "next")
        PlayNext();
    else if (cmd == "prev")
        PlayPrev();
    else if (cmd == "random")
        PlayRandom();
    else if (cmd == "play")
        PlayIndex(c.at("index").get<uint32_t>());
    else if (cmd == "set")
    {
        const std::string key = c.at("key").get<std::string>();
        const auto& v = c.at("value");
        if (key == "lock")
            SetLock(v.get<bool>());
        else if (key == "shuffle")
            SetShuffle(v.get<bool>());
        else if (key == "smooth")
            SetSmooth(v.get<bool>());
        else if (key == "hardCuts")
            SetHardCuts(v.get<bool>());
        else if (key == "duration")
            SetDuration(v.get<double>());
        else if (key == "softCut")
            SetSoftCut(v.get<double>());
        else if (key == "beatSensitivity")
            SetBeatSensitivity(v.get<float>());
        else if (key == "brightness")
            SetBrightness(v.get<float>());
        else
            throw std::runtime_error("unknown key " + key);
    }
    else if (cmd == "source")
        SelectSource(c.at("name").get<std::string>());
    else if (cmd == "mode")
    {
        OutputMode mode{};
        if (!ParseMode(c.at("mode").get<std::string>(), mode))
            throw std::runtime_error("unknown mode");
        RequestMode(mode);
    }
    else if (cmd == "monitor")
        SelectMonitor(c.at("index").get<int>());
    else if (cmd == "loadLayout")
        LoadLayoutAndSwitch(c.at("path").get<std::string>());
    else if (cmd == "ui")
        SetUiVisible(c.at("visible").get<bool>());
    else if (cmd == "saveConfig")
        SaveConfig();
    else
        throw std::runtime_error("unknown command");
}

void App::PublishState()
{
    if (!m_web)
    {
        return;
    }
    m_lastPublish = std::chrono::steady_clock::now();
    using nlohmann::json;
    auto rect = [](int x, int y, int w, int h) { return json{{"x", x}, {"y", y}, {"w", w}, {"h", h}}; };

    json audio;
    audio["ok"] = m_audioOk;
    audio["error"] = m_audio.LastError();
    if (m_audioOk)
    {
        json nodes = json::array();
        for (const auto& n : m_audio.Nodes())
        {
            nodes.push_back({{"id", n.id},
                             {"name", n.name},
                             {"description", n.description},
                             {"mediaClass", n.mediaClass},
                             {"isSink", n.isSink},
                             {"label", NodeLabel(n)}});
        }
        audio["nodes"] = std::move(nodes);
        audio["selected"] = m_audio.Selected();
        audio["streaming"] = m_audio.Streaming();
        audio["rate"] = m_audio.Rate();
        audio["peak"] = std::min(1.0f, m_audio.Peak());
    }

    json preset;
    const bool hasCurrent = m_current < m_presetLabels.size();
    preset["index"] = hasCurrent ? json(m_current) : json(nullptr);
    preset["label"] = hasCurrent ? m_presetLabels[m_current] : std::string("(idle)");
    preset["lock"] = m_opt.lock;
    preset["shuffle"] = m_opt.shuffle;
    preset["smooth"] = m_opt.smooth;
    preset["hardCuts"] = m_opt.hardCuts;
    preset["duration"] = m_opt.presetDuration;
    preset["softCut"] = m_opt.softCut;
    preset["beatSensitivity"] = m_opt.beatSensitivity;
    preset["count"] = m_presetPaths.size();
    preset["failures"] = m_failures;
    preset["lastFailure"] = m_lastFailure;
    preset["presetsVersion"] = m_presetsVersion;

    json output;
    output["mode"] = ModeName(m_mode);
    output["pendingMode"] = m_rebuildRequested ? json(ModeName(m_requestedMode)) : json(nullptr);
    output["layoutLoaded"] = m_layout.has_value();
    output["brightness"] = m_opt.brightness;
    output["brightnessAvailable"] = m_blitProg != 0;
    output["monitor"] = ResolveMonitor();
    json monitors = json::array();
    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i)
    {
        SDL_Rect r{};
        SDL_GetDisplayBounds(i, &r);
        const char* name = SDL_GetDisplayName(i);
        json m = rect(r.x, r.y, r.w, r.h);
        m["index"] = i;
        m["name"] = name ? name : "?";
        monitors.push_back(std::move(m));
    }
    output["monitors"] = std::move(monitors);
    output["layoutPath"] = std::string(m_layoutPathBuf);
    output["layoutError"] = m_layoutError;
    output["canvas"] = {{"w", m_canvasW}, {"h", m_canvasH}};
    json outs = json::array();
    for (const auto& o : m_outputs)
    {
        int x = 0, y = 0, w = 0, h = 0;
        SDL_GetWindowPosition(o.window, &x, &y);
        SDL_GetWindowSize(o.window, &w, &h);
        outs.push_back({{"name", o.name},
                        {"screen", rect(x, y, w, h)},
                        {"slice", rect(o.source.x, o.source.y, o.source.w, o.source.h)}});
    }
    output["outputs"] = std::move(outs);

    json state;
    state["audio"] = std::move(audio);
    state["preset"] = std::move(preset);
    state["output"] = std::move(output);
    state["config"] = {{"path", m_opt.configPath}, {"status", m_configStatus}};
    state["info"] = {{"fps", m_fpsSmoothed}, {"targetFps", m_opt.fps}, {"uiVisible", m_uiVisible},
                     {"version", MUQ_VERSION}};
    m_web->SetState(state.dump(-1, ' ', false, json::error_handler_t::replace));
}

} // namespace muq
