#include "AudioPipeWire.hpp"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace muq {

struct AudioPipeWire::Hooks
{
    spa_hook registryListener{};
    spa_hook streamListener{};
    spa_hook coreListener{};
    pw_core_events coreEvents{};
    pw_registry_events registryEvents{};
    int syncSeq = -1;
    bool synced = false;
    pw_stream_events streamEvents{};
};

namespace {

void RegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type, uint32_t /*version*/,
                    const spa_dict* props)
{
    static_cast<AudioPipeWire*>(data)->OnGlobal(id, type, props);
}

void RegistryGlobalRemove(void* data, uint32_t id)
{
    static_cast<AudioPipeWire*>(data)->OnGlobalRemove(id);
}

void StreamProcess(void* data)
{
    static_cast<AudioPipeWire*>(data)->OnProcess();
}

void StreamStateChanged(void* data, pw_stream_state /*old*/, pw_stream_state state, const char* error)
{
    static_cast<AudioPipeWire*>(data)->OnStateChanged(static_cast<int>(state), error);
}

void StreamParamChanged(void* data, uint32_t id, const spa_pod* param)
{
    static_cast<AudioPipeWire*>(data)->OnParamChanged(id, param);
}

void CoreDone(void* data, uint32_t id, int seq);

const char* DictGet(const spa_dict* props, const char* key)
{
    return props ? spa_dict_lookup(props, key) : nullptr;
}

void CoreDone(void* data, uint32_t id, int seq)
{
    static_cast<AudioPipeWire*>(data)->OnCoreDone(id, seq);
}

} // namespace

AudioPipeWire::AudioPipeWire()
    : m_hooks(std::make_unique<Hooks>())
{
    m_capacityFrames = 48000; // 1 s
    m_ring.resize(m_capacityFrames * Channels);

    m_hooks->coreEvents.version = PW_VERSION_CORE_EVENTS;
    m_hooks->coreEvents.done = CoreDone;

    m_hooks->registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
    m_hooks->registryEvents.global = RegistryGlobal;
    m_hooks->registryEvents.global_remove = RegistryGlobalRemove;

    m_hooks->streamEvents.version = PW_VERSION_STREAM_EVENTS;
    m_hooks->streamEvents.process = StreamProcess;
    m_hooks->streamEvents.state_changed = StreamStateChanged;
    m_hooks->streamEvents.param_changed = StreamParamChanged;
}

AudioPipeWire::~AudioPipeWire()
{
    Stop();
}

bool AudioPipeWire::Start()
{
    pw_init(nullptr, nullptr);

    m_loop = pw_thread_loop_new("muq-audio", nullptr);
    if (!m_loop)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "pw_thread_loop_new failed";
        return false;
    }

    pw_thread_loop_lock(m_loop);
    if (pw_thread_loop_start(m_loop) < 0)
    {
        pw_thread_loop_unlock(m_loop);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "pw_thread_loop_start failed";
        return false;
    }

    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    m_core = m_context ? pw_context_connect(m_context, nullptr, 0) : nullptr;
    if (!m_core)
    {
        pw_thread_loop_unlock(m_loop);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "cannot connect to PipeWire daemon (is pipewire running for this user?)";
        return false;
    }

    m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(m_registry, &m_hooks->registryListener, &m_hooks->registryEvents, this);

    // Round-trip so the initial node list is known before the stream is connected.
    pw_core_add_listener(m_core, &m_hooks->coreListener, &m_hooks->coreEvents, this);
    m_hooks->synced = false;
    m_hooks->syncSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
    while (!m_hooks->synced)
    {
        if (pw_thread_loop_timed_wait(m_loop, 2) != 0)
        {
            break; // timeout: carry on with what we have
        }
    }

    ConnectStreamLocked();
    pw_thread_loop_unlock(m_loop);
    return true;
}

void AudioPipeWire::Stop()
{
    if (!m_loop)
    {
        return;
    }
    pw_thread_loop_lock(m_loop);
    if (m_stream)
    {
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }
    if (m_registry)
    {
        spa_hook_remove(&m_hooks->registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
        m_registry = nullptr;
    }
    if (m_core)
    {
        spa_hook_remove(&m_hooks->coreListener);
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    pw_thread_loop_unlock(m_loop);
    pw_thread_loop_stop(m_loop);
    if (m_context)
    {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    pw_thread_loop_destroy(m_loop);
    m_loop = nullptr;
    m_streaming = false;
    pw_deinit();
}

std::vector<AudioNode> AudioPipeWire::Nodes() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_nodes;
}

std::string AudioPipeWire::Selected() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_selected;
}

std::string AudioPipeWire::LastError() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_error;
}

void AudioPipeWire::Select(const std::string& nodeName)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_selected = nodeName;
    }
    if (!m_loop)
    {
        return;
    }
    pw_thread_loop_lock(m_loop);
    ConnectStreamLocked();
    pw_thread_loop_unlock(m_loop);
}

void AudioPipeWire::ConnectStreamLocked()
{
    if (m_stream)
    {
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
        m_streaming = false;
    }

    std::string target;
    bool captureSink = true; // default target: default sink monitor
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        target = m_selected;
        if (!target.empty())
        {
            captureSink = false;
            for (const auto& n : m_nodes)
            {
                if (n.name == target)
                {
                    captureSink = n.isSink;
                    break;
                }
            }
            // Unknown yet (registry not populated): guess from naming convention.
            if (std::none_of(m_nodes.begin(), m_nodes.end(), [&](const AudioNode& n) { return n.name == target; }))
            {
                captureSink = target.find("output") != std::string::npos || target.find("sink") != std::string::npos;
            }
        }
    }

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "muq-viz",
        PW_KEY_NODE_NAME, "muq-viz",
        PW_KEY_NODE_DESCRIPTION, "muq-viz visualizer input",
        nullptr);
    if (captureSink)
    {
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }
    if (!target.empty())
    {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target.c_str());
    }
    pw_properties_set(props, PW_KEY_NODE_LATENCY, "512/48000");

    m_stream = pw_stream_new(m_core, "muq-viz capture", props);
    if (!m_stream)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = "pw_stream_new failed";
        return;
    }
    pw_stream_add_listener(m_stream, &m_hooks->streamListener, &m_hooks->streamEvents, this);

    uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = Channels;
    info.rate = 48000;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    const int res = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                      static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                                   PW_STREAM_FLAG_MAP_BUFFERS |
                                                                   PW_STREAM_FLAG_RT_PROCESS),
                                      params, 1);
    if (res < 0)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = std::string("pw_stream_connect: ") + spa_strerror(res);
    }
}

void AudioPipeWire::OnCoreDone(uint32_t id, int seq)
{
    if (id == PW_ID_CORE && seq == m_hooks->syncSeq)
    {
        m_hooks->synced = true;
        pw_thread_loop_signal(m_loop, false);
    }
}

void AudioPipeWire::OnGlobal(uint32_t id, const char* type, const spa_dict* props)
{
    if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    {
        return;
    }
    const char* mediaClass = DictGet(props, PW_KEY_MEDIA_CLASS);
    if (!mediaClass || std::strncmp(mediaClass, "Audio/", 6) != 0)
    {
        return;
    }
    const bool isSink = std::strstr(mediaClass, "Sink") != nullptr;
    const bool isSource = std::strstr(mediaClass, "Source") != nullptr || std::strstr(mediaClass, "Duplex") != nullptr;
    if (!isSink && !isSource)
    {
        return;
    }
    const char* name = DictGet(props, PW_KEY_NODE_NAME);
    if (!name || std::strcmp(name, "muq-viz") == 0)
    {
        return;
    }
    const char* desc = DictGet(props, PW_KEY_NODE_DESCRIPTION);
    if (!desc)
    {
        desc = DictGet(props, PW_KEY_NODE_NICK);
    }

    AudioNode node;
    node.id = id;
    node.name = name;
    node.description = desc ? desc : name;
    node.mediaClass = mediaClass;
    node.isSink = isSink;

    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const AudioNode& n) { return n.id == id; }),
                  m_nodes.end());
    m_nodes.push_back(std::move(node));
    std::stable_sort(m_nodes.begin(), m_nodes.end(), [](const AudioNode& a, const AudioNode& b) {
        if (a.isSink != b.isSink)
        {
            return a.isSink; // outputs (monitors) first
        }
        return a.description < b.description;
    });
}

void AudioPipeWire::OnGlobalRemove(uint32_t id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_nodes.erase(std::remove_if(m_nodes.begin(), m_nodes.end(), [&](const AudioNode& n) { return n.id == id; }),
                  m_nodes.end());
}

void AudioPipeWire::OnStateChanged(int state, const char* error)
{
    m_streaming = (state == PW_STREAM_STATE_STREAMING);
    if (state == PW_STREAM_STATE_ERROR)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = error ? error : "stream error";
    }
    else if (state == PW_STREAM_STATE_STREAMING)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error.clear();
    }
}

void AudioPipeWire::OnParamChanged(uint32_t id, const spa_pod* param)
{
    if (!param || id != SPA_PARAM_Format)
    {
        return;
    }
    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) >= 0 && info.rate > 0)
    {
        m_rate = info.rate;
    }
}

void AudioPipeWire::OnProcess()
{
    pw_buffer* buf = pw_stream_dequeue_buffer(m_stream);
    if (!buf)
    {
        return;
    }
    spa_buffer* sbuf = buf->buffer;
    if (sbuf->n_datas > 0 && sbuf->datas[0].data && sbuf->datas[0].chunk)
    {
        const spa_data& d = sbuf->datas[0];
        const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
        const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
        const auto* samples = reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + offset);
        Push(samples, size / (sizeof(float) * Channels));
    }
    pw_stream_queue_buffer(m_stream, buf);
}

void AudioPipeWire::Push(const float* samples, size_t frames)
{
    float peak = 0.0f;
    size_t head = m_head.load(std::memory_order_relaxed);
    const size_t tail = m_tail.load(std::memory_order_acquire);
    const size_t freeFrames = m_capacityFrames - (head - tail);
    frames = std::min(frames, freeFrames); // drop on overflow (consumer stalled)
    for (size_t i = 0; i < frames; ++i)
    {
        const size_t slot = (head % m_capacityFrames) * Channels;
        for (unsigned c = 0; c < Channels; ++c)
        {
            const float v = samples[i * Channels + c];
            m_ring[slot + c] = v;
            peak = std::max(peak, std::fabs(v));
        }
        ++head;
    }
    m_head.store(head, std::memory_order_release);
    // Decaying peak meter.
    const float prev = m_peak.load(std::memory_order_relaxed);
    m_peak.store(std::max(peak, prev * 0.9f), std::memory_order_relaxed);
}

size_t AudioPipeWire::Read(float* out, size_t maxFrames)
{
    size_t tail = m_tail.load(std::memory_order_relaxed);
    const size_t head = m_head.load(std::memory_order_acquire);
    const size_t frames = std::min(maxFrames, head - tail);
    for (size_t i = 0; i < frames; ++i)
    {
        const size_t slot = (tail % m_capacityFrames) * Channels;
        for (unsigned c = 0; c < Channels; ++c)
        {
            out[i * Channels + c] = m_ring[slot + c];
        }
        ++tail;
    }
    m_tail.store(tail, std::memory_order_release);
    return frames;
}

} // namespace muq
