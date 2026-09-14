/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_SERVER_H_
#define _SIGNAL_SERVER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/http/constants.hpp>
#include <websocketpp/server.hpp>

#include "admin_auth.h"
#include "admin_controller.h"
#include "bounded_executor.h"
#include "device_db_manager.h"
#include "geo_location_resolver.h"
#include "presence_manager.h"
#include "resource_monitor.h"
#include "signal_negotiation.h"

using nlohmann::json;

struct SignalServerConfig : websocketpp::config::asio_tls {
  struct transport_config : websocketpp::config::asio_tls::transport_config {
    static const long timeout_socket_shutdown = 1500;
  };
  using transport_type =
      websocketpp::transport::asio::endpoint<transport_config>;
};
using server = websocketpp::server<SignalServerConfig>;
typedef websocketpp::lib::shared_ptr<websocketpp::lib::asio::ssl::context>
    context_ptr;

class SignalServer {
 public:
  SignalServer();
  SignalServer(uint16_t port, std::string certs_dir, std::string db_path);
  ~SignalServer();

  bool OnOpen(websocketpp::connection_hdl hdl);
  void OnHttp(websocketpp::connection_hdl hdl);
  context_ptr OnTlsInit(websocketpp::connection_hdl hdl);
  bool OnHeartbeat(websocketpp::connection_hdl hdl, std::string payload);

  void Run();
  void Stop();  // Thread safe; requests graceful transport shutdown.
  void SendMsg(websocketpp::connection_hdl hdl, json message);
  void OnMessage(websocketpp::connection_hdl hdl, server::message_ptr msg);

 private:
  using Clock = std::chrono::steady_clock;
  struct ConnectionState {
    uint64_t id = 0;
    std::string ip;  // Set before publishing to the application worker.
    std::string device_id;  // Accessed only on the network thread.
    Clock::time_point accepted = Clock::now(), last_heartbeat = accepted;
    bool opened = false, authenticated = false;
    server::connection_ptr pending_http;  // Network thread; retains deferred HTTP.
    std::atomic<bool> alive{true};
    std::atomic<size_t> pending_messages{0};
  };
  void AcceptNext();
  void RetryAccept();
  void FinishConnection(server::connection_ptr con);
  void QueueSessionCleanup(websocketpp::connection_hdl hdl,
                           const std::shared_ptr<ConnectionState>& state);
  void CloseConnection(websocketpp::connection_hdl hdl, const char* reason,
                       websocketpp::close::status::value code);
  void ScheduleMaintenance();
  void ReloadTlsContext();
  void ProcessMessage(websocketpp::connection_hdl hdl, const json& message,
                      const std::shared_ptr<ConnectionState>& state);
  void CompleteHttp(websocketpp::connection_hdl hdl,
                    AdminHttpResponse response);
  void WorkerFailed(std::exception_ptr error);
  void RequestBackpressureClose(websocketpp::connection_hdl hdl);

  struct GeoIpLookupJob {
    std::string client_ip;
    std::chrono::steady_clock::time_point run_at =
        std::chrono::steady_clock::now();
  };

  struct GeoIpLookupJobLater {
    bool operator()(const GeoIpLookupJob& lhs,
                    const GeoIpLookupJob& rhs) const {
      return lhs.run_at > rhs.run_at;
    }
  };

  void ScheduleRuntimeHeartbeat();
  void ScheduleRecoveredSessionCleanup();
  std::string GetClientIp(websocketpp::connection_hdl hdl);
  void EnqueueClientNetworkInfo(const std::string& client_ip,
                                const std::string& device_id);
  void EnqueueGeoIpLookup(const std::string& client_ip,
                          std::chrono::milliseconds delay);
  void ProcessGeoIpLookup(const GeoIpLookupJob& job);
  void StartClientNetworkInfoWorker();
  void StopClientNetworkInfoWorker();
  void ProcessClientNetworkInfoJobs();

  server server_;
  uint16_t port_;
  std::string certs_dir_;
  // Only the network thread touches the endpoint and the connection map.
  std::map<websocketpp::connection_hdl, std::shared_ptr<ConnectionState>,
           std::owner_less<websocketpp::connection_hdl>>
      connections_;
  Clock::time_point fd_sampled_at_{};
  size_t fd_connections_at_sample_ = 0;
  uint64_t next_connection_id_ = 1;
  size_t pending_cleanup_ = 0;
  size_t max_connections_ = 2048;
  const size_t fd_reserve_ = 256, max_handshakes_ = 128;
  const long heartbeat_timeout_ms_ = 30000, check_interval_ms_ = 5000;
  const long authentication_timeout_ms_ = 15000, http_timeout_ms_ = 10000;
  const long tls_reload_interval_ms_ = 30000;
  Clock::time_point next_tls_reload_{};
  Clock::time_point next_accept_warning_{}, next_preopen_warning_{};
  FileDescriptorUsage fd_usage_;
  std::atomic<bool> stopping_{false};
  bool accept_pending_ = false, accept_retry_pending_ = false;
  bool runtime_job_pending_ = false, tls_job_pending_ = false;
  context_ptr
      tls_context_;  // Published atomically; never mutate a live context.
  std::string tls_generation_;
  std::unique_ptr<websocketpp::lib::asio::signal_set> signals_;
  std::unique_ptr<websocketpp::lib::asio::ip::tcp::acceptor> acceptor_;
  std::exception_ptr worker_error_;
  std::mutex worker_error_mutex_;
  std::atomic<size_t> pending_sends_{0};
  std::mutex backpressure_mutex_;
  std::set<websocketpp::connection_hdl,
           std::owner_less<websocketpp::connection_hdl>>
      backpressure_connections_;
  bool backpressure_posted_ = false;
  std::unique_ptr<BoundedExecutor> application_worker_, admin_worker_,
      maintenance_worker_;

  std::shared_ptr<TransmissionManager> transmission_manager_;
  std::unique_ptr<DeviceDBManager> device_db_manager_;
  std::unique_ptr<SignalNegotiation> signal_negotiation_;
  std::unique_ptr<GeoLocationResolver> geo_location_resolver_;
  std::unique_ptr<PresenceManager> presence_manager_;
  std::unique_ptr<AdminAuth> admin_auth_;
  std::unique_ptr<AdminController> admin_controller_;
  std::unique_ptr<DeviceDBManager> admin_read_db_;
  std::unique_ptr<AdminController> admin_read_controller_;

  std::mutex network_info_mutex_;
  std::condition_variable network_info_cv_;
  std::priority_queue<GeoIpLookupJob, std::vector<GeoIpLookupJob>,
                      GeoIpLookupJobLater>
      network_info_jobs_;
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      pending_ip_lookup_at_;
  std::unordered_map<std::string, int> geo_ip_failure_counts_;
  std::thread network_info_worker_;
  bool network_info_stop_ = false;
};

#endif
