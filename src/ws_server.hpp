#pragma once

#include <atomic>
#include <string>
#include <thread>

class AdapterRegistry;
class CapabilityRegistry;
class SessionManager;

struct WsServerConfig {
  std::string bind_addr = "0.0.0.0";
  int port = 0;
  std::string default_codec = "mjpeg";
};

class WebSocketServer {
public:
  WebSocketServer(SessionManager &sessions, const CapabilityRegistry &registry,
                  const AdapterRegistry &adapters, WsServerConfig config);
  ~WebSocketServer();

  WebSocketServer(const WebSocketServer &) = delete;
  WebSocketServer &operator=(const WebSocketServer &) = delete;

  bool start();
  void stop();

  bool running() const { return running_.load(); }
  int port() const { return config_.port; }

private:
  void accept_loop();
  void handle_client(int client_fd);

  SessionManager &sessions_;
  const CapabilityRegistry &registry_;
  const AdapterRegistry &adapters_;
  WsServerConfig config_;

  std::thread accept_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> running_{false};
  int listen_fd_ = -1;
};
