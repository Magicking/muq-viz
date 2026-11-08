#pragma once
// PipeWire audio capture: enumerates sinks (captured through their monitor ports)
// and sources, captures float32 stereo into a lock-free ring buffer.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct pw_stream;
struct spa_dict;
struct spa_pod;

namespace muq {

struct AudioNode
{
    uint32_t id = 0;
    std::string name;        // node.name (stable identifier used for target.object)
    std::string description; // node.description / nick
    std::string mediaClass;  // Audio/Sink, Audio/Source, ...
    bool isSink = false;     // true: capture from the sink's monitor (= what is played)
};

class AudioPipeWire
{
public:
    static constexpr unsigned Channels = 2;

    AudioPipeWire();
    ~AudioPipeWire();

    AudioPipeWire(const AudioPipeWire&) = delete;
    AudioPipeWire& operator=(const AudioPipeWire&) = delete;

    /// Connects to the PipeWire daemon. Returns false on error (see LastError()).
    bool Start();
    void Stop();

    /// Snapshot of currently known audio nodes.
    std::vector<AudioNode> Nodes() const;

    /// Captures from the node with this node.name. Empty = default sink monitor.
    void Select(const std::string& nodeName);
    std::string Selected() const;

    /// Pops up to maxFrames interleaved stereo frames. Returns frames read.
    size_t Read(float* out, size_t maxFrames);

    float Peak() const { return m_peak.load(std::memory_order_relaxed); }
    bool Streaming() const { return m_streaming.load(); }
    std::string LastError() const;
    uint32_t Rate() const { return m_rate.load(); }

    // Internal callbacks (public for C trampolines).
    void OnCoreDone(uint32_t id, int seq);
    void OnGlobal(uint32_t id, const char* type, const ::spa_dict* props);
    void OnGlobalRemove(uint32_t id);
    void OnProcess();
    void OnStateChanged(int state, const char* error);
    void OnParamChanged(uint32_t id, const ::spa_pod* param);

private:
    void ConnectStreamLocked(); // thread loop must be locked
    void Push(const float* samples, size_t frames);

    pw_thread_loop* m_loop = nullptr;
    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    pw_registry* m_registry = nullptr;
    pw_stream* m_stream = nullptr;
    struct Hooks;
    std::unique_ptr<Hooks> m_hooks;

    mutable std::mutex m_mutex; // guards m_nodes, m_selected, m_error
    std::vector<AudioNode> m_nodes;
    std::string m_selected;
    std::string m_error;

    // SPSC ring buffer (producer: PipeWire RT thread, consumer: render thread)
    std::vector<float> m_ring;
    std::atomic<size_t> m_head{0}; // write index (frames)
    std::atomic<size_t> m_tail{0}; // read index (frames)
    size_t m_capacityFrames = 0;

    std::atomic<float> m_peak{0.0f};
    std::atomic<bool> m_streaming{false};
    std::atomic<uint32_t> m_rate{48000};
};

} // namespace muq
