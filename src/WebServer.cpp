#include "WebServer.hpp"

#include <httplib.h>

#include <cstdio>

namespace muq {

extern const char* const kWebIndexHtml;

namespace {
constexpr size_t MaxPending = 256; // drop commands if the render thread stalls
}

WebServer::WebServer() = default;

WebServer::~WebServer()
{
    Stop();
}

bool WebServer::Start(const std::string& bind, int port)
{
    Stop();
    m_server = std::make_unique<httplib::Server>();
    auto& svr = *m_server;
    svr.set_payload_max_length(64 * 1024);
    svr.set_keep_alive_max_count(1000);
    // httplib's default also sets SO_REUSEPORT, which lets a second muq-viz silently share
    // the port (requests then hit a random instance). Only allow quick rebinding after restart.
    svr.set_socket_options([](socket_t sock) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    });

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache");
        res.set_content(kWebIndexHtml, "text/html; charset=utf-8");
    });
    svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& res) {
        std::string body;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            body = m_state;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::move(body), "application/json");
    });
    svr.Get("/api/presets", [this](const httplib::Request&, httplib::Response& res) {
        std::string body;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            body = m_presets;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_content(std::move(body), "application/json");
    });
    svr.Post("/api/cmd", [this](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json cmd = nlohmann::json::parse(req.body, nullptr, false);
        if (cmd.is_discarded() || !cmd.is_object() || !cmd.contains("cmd") || !cmd["cmd"].is_string())
        {
            res.status = 400;
            res.set_content(R"({"error":"expected JSON object with string field \"cmd\""})", "application/json");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_pending.size() >= MaxPending)
            {
                res.status = 503;
                res.set_content(R"({"error":"command queue full"})", "application/json");
                return;
            }
            m_pending.push_back(std::move(cmd));
        }
        res.status = 202;
        res.set_content(R"({"ok":true})", "application/json");
    });

    if (!svr.bind_to_port(bind, port))
    {
        m_error = "cannot bind " + bind + ":" + std::to_string(port);
        m_server.reset();
        return false;
    }
    m_error.clear();
    m_thread = std::thread([this] { m_server->listen_after_bind(); });
    return true;
}

void WebServer::Stop()
{
    if (m_server)
    {
        m_server->stop();
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
    m_server.reset();
}

void WebServer::SetState(std::string json)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = std::move(json);
}

void WebServer::SetPresets(std::string json)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_presets = std::move(json);
}

std::vector<nlohmann::json> WebServer::TakeCommands()
{
    std::vector<nlohmann::json> out;
    std::lock_guard<std::mutex> lock(m_mutex);
    out.swap(m_pending);
    return out;
}

} // namespace muq
