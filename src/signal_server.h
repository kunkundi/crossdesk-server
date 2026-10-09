/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_SERVER_H_
#define _SIGNAL_SERVER_H_

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>
#include <websocketpp/config/asio.hpp>
#include <websocketpp/http/constants.hpp>
#include <websocketpp/server.hpp>

#include "admin_auth.h"
#include "admin_controller.h"
#include "bounded_executor.h"
#include "device_db_manager.h"
#include "notification_service.h"
#include "presence_manager.h"
#include "resource_monitor.h"
#include "retention_policy.h"
#include "session_recovery.h"
#include "credential_queue.h"
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
    websocketpp::connection_hdl handle;
    std::string source_address;  // Set before publication; transport peer IP.
    std::string device_id;  // Accessed only on the network thread.
    Clock::time_point accepted = Clock::now(), last_heartbeat = accepted;
    bool opened = false, authenticated = false;
    server::connection_ptr pending_http;  // Network thread; retains deferred HTTP.
    std::atomic<bool> alive{true};
    std::atomic<size_t> pending_messages{0};
    // Steady-clock epoch milliseconds; application worker publishes, I/O reads.
    std::atomic<int64_t> credential_deadline_ms{0};
    // Application worker only. Preserve message order across async hashing.
    bool credential_pending = false;
    std::deque<json> deferred_messages;
  };
  void AcceptNext();
  void RetryAccept();
  int64_t EstimateFileDescriptors() const;
  std::string DescribeConnectionResources();
  void LogConnectionDiagnostics();
  void FinishConnection(server::connection_ptr con);
  void QueueSessionCleanup(websocketpp::connection_hdl hdl,
                           const std::shared_ptr<ConnectionState>& state);
  void CloseConnection(websocketpp::connection_hdl hdl, const char* reason,
                       websocketpp::close::status::value code);
  void ScheduleMaintenance();
  void ReloadTlsContext();
  void ProcessMessage(websocketpp::connection_hdl hdl, const json& message,
                      const std::shared_ptr<ConnectionState>& state);
  void DispatchMessage(websocketpp::connection_hdl hdl, json message,
                       const std::shared_ptr<ConnectionState>& state);
  bool SubmitCredentialWork(websocketpp::connection_hdl hdl,
                            const std::shared_ptr<ConnectionState>& state,
                            SignalNegotiation::CredentialWork work,
                            std::function<int()> admit,
                            std::function<void()> expire);
  void StartCredentialWork(websocketpp::connection_hdl hdl,
                           const std::shared_ptr<ConnectionState>& state,
                           SignalNegotiation::CredentialWork work, size_t worker);
  void FinishCredentialRequest(const std::shared_ptr<ConnectionState>& state);
  void ScheduleCredentialPump();
  void LogCredentialDiagnostics();
  void CompleteHttp(websocketpp::connection_hdl hdl,
                    AdminHttpResponse response);
  void WorkerFailed(std::exception_ptr error);
  void RequestBackpressureClose(websocketpp::connection_hdl hdl);
  void BroadcastToClients(json message);

  void ScheduleRuntimeHeartbeat();
  void ScheduleRecoveredSessionCleanup();
  void ScheduleRetentionCleanup();

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
  size_t max_connections_ = 8192;
  const size_t fd_reserve_ = 256, max_handshakes_ = 128;
  static constexpr int kListenBacklog = 4096;
  const long heartbeat_timeout_ms_ = 30000, check_interval_ms_ = 5000;
  const long authentication_timeout_ms_ = 15000, http_timeout_ms_ = 10000;
  const long tls_reload_interval_ms_ = 30000;
  Clock::time_point next_tls_reload_{};
  Clock::time_point next_accept_warning_{}, next_preopen_warning_{};
  // Diagnostics are updated only on the network thread. Counters are cumulative.
  struct ConnectionDiagnostics {
    uint64_t accepted = 0, opened = 0, preopen_failed = 0;
    uint64_t accept_failed = 0, init_resource_failed = 0, loop_stalls = 0;
    uint64_t connection_limit_checks = 0, unopened_limit_checks = 0;
    uint64_t fd_limit_checks = 0, fd_sample_failed_checks = 0;
    uint64_t login_success = 0, authentication_failed = 0;
    uint64_t reconnect_success = 0, reconnect_failed = 0;
    uint64_t credential_busy = 0, authentication_throttled = 0;
    uint64_t authentication_timeouts = 0;
  } diagnostics_;
  Clock::time_point next_diagnostics_{}, next_admission_warning_{};
  Clock::time_point next_loop_warning_{};
  Clock::time_point admission_paused_since_{};
  bool admission_paused_ = false, admission_pause_logged_ = false;
  uint64_t admission_pause_checks_ = 0;
  FileDescriptorUsage fd_usage_;
  std::atomic<bool> stopping_{false};
  bool accept_pending_ = false, accept_retry_pending_ = false;
  bool runtime_job_pending_ = false, tls_job_pending_ = false;
  bool credential_pump_pending_ = false;  // I/O thread only.
  const RetentionPolicy retention_policy_ = RetentionPolicy::FromEnvironment();
  bool metadata_cleanup_pending_ = false, log_cleanup_pending_ = false;
  Clock::time_point next_metadata_cleanup_{}, next_log_cleanup_{};
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
  std::vector<std::unique_ptr<BoundedExecutor>> credential_workers_;
  std::unique_ptr<CredentialQueue> credential_queue_;  // Application worker only.
  size_t credential_worker_count_ = 2;
  int credential_timeout_seconds_ = 600;
  Clock::time_point next_credential_diagnostics_{};
  size_t deferred_message_count_ = 0;  // Application worker; bounded globally.
  std::atomic<uint64_t> credential_work_us_{0}, credential_work_count_{0};

  std::shared_ptr<TransmissionManager> transmission_manager_;
  std::unique_ptr<DeviceDBManager> device_db_manager_;
  std::unique_ptr<SignalNegotiation> signal_negotiation_;
  std::unique_ptr<SessionRecovery> session_recovery_;
  std::unique_ptr<PresenceManager> presence_manager_;
  std::unique_ptr<NotificationService> notification_service_;
  std::unique_ptr<NotificationService> notification_read_service_;
  std::unique_ptr<AdminAuth> admin_auth_;
  std::unique_ptr<AdminController> admin_controller_;
  std::unique_ptr<DeviceDBManager> admin_read_db_;
  std::unique_ptr<AdminController> admin_read_controller_;
};

#endif
