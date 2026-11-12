#pragma once
// Embedded HTTP control server. Runs on its own thread and never touches
// projectM / SDL / GL: the render thread publishes state snapshots and drains
// the command queue (see App::PublishState / App::ProcessWebCommands).
//
//   GET  /             web control page (embedded)
//   GET  /api/state    last published state (JSON)
//   GET  /api/presets  {"version": N, "labels": [...]}
//   POST /api/cmd      {"cmd": "...", ...}  -> 202 queued, 400 bad request

#include <nlohmann/json.hpp>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
}

namespace muq {

class WebServer
{
public:
    WebServer();
    ~WebServer();

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    /// Binds and starts the listener thread. Returns false (see LastError()) when binding fails.
    bool Start(const std::string& bind, int port);
    void Stop();
    bool Running() const { return m_thread.joinable(); }
    const std::string& LastError() const { return m_error; }

    void SetState(std::string json);
    void SetPresets(std::string json);

    /// Removes and returns all queued commands (render thread).
    std::vector<nlohmann::json> TakeCommands();

private:
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
    std::string m_error;

    std::mutex m_mutex; // guards the three members below
    std::string m_state = "{}";
    std::string m_presets = "{\"version\":0,\"labels\":[]}";
    std::vector<nlohmann::json> m_pending;
};

} // namespace muq
