#include "signal_server.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

#include "common.h"
#include "log.h"
#include "signal_negotiation.h"

namespace {

constexpr long kRecoveredSessionCleanupDelayMs = 120000;
constexpr size_t kMaxClientNetworkInfoJobs = 1024;
constexpr int kDefaultGeoIpFailureTtlMs = 60000;
constexpr int kDefaultGeoIpFailureMaxTtlMs = 1800000;
constexpr int kDefaultTurnCredentialTtlSeconds = 3600;
constexpr auto kDiagnosticInterval = std::chrono::seconds(60);
constexpr auto kDiagnosticWarningInterval = std::chrono::seconds(30);

int EnvMillis(const char* name, int fallback, int min_value, int max_value) {
  const char* raw = std::getenv(name);
  if (!raw) {
    return fallback;
  }
  char* end = nullptr;
  long value = std::strtol(raw, &end, 10);
  if (*end != '\0' || end == raw || value < min_value || value > max_value) {
    return fallback;
  }
  return static_cast<int>(value);
}

std::string EnvString(const char* name) {
  const char* raw = std::getenv(name);
  return raw ? raw : "";
}

std::shared_ptr<IceServerConfigIssuer> CreateIceServerConfigIssuer() {
  const auto text = EnvString("CROSSDESK_ICE_SERVERS");
  json servers = json::array();
  if (!text.empty()) {
    servers = json::parse(text, nullptr, false);
    if (servers.is_discarded())
      throw std::invalid_argument("Invalid CROSSDESK_ICE_SERVERS JSON");
  } else {
    auto host = EnvString("COTURN_PUBLIC_HOST");
    if (host.empty()) host = EnvString("EXTERNAL_IP");
    if (!host.empty()) {
      if (host.find(':') != std::string::npos && host.front() != '[')
        host = "[" + host + "]";
      const auto endpoint =
          host + ":" + std::to_string(EnvMillis("COTURN_PORT", 3478, 1, 65535));
      servers.push_back({{"urls", {"stun:" + endpoint}}});
      if (!EnvString("COTURN_AUTH_SECRET").empty())
        servers.push_back({{"urls",
                            {"turn:" + endpoint + "?transport=udp",
                             "turn:" + endpoint + "?transport=tcp"}}});
    }
  }
  return std::make_shared<IceServerConfigIssuer>(
      servers, EnvString("COTURN_AUTH_SECRET"),
      EnvMillis("COTURN_CREDENTIAL_TTL_SECONDS",
                kDefaultTurnCredentialTtlSeconds, 60, 86400));
}

std::chrono::milliseconds GeoIpFailureRetryDelay(int failure_count) {
  int64_t delay_ms = EnvMillis("CROSSDESK_GEOIP_FAILURE_TTL_MS",
                               kDefaultGeoIpFailureTtlMs, 0, 3600000);
  int64_t max_delay_ms = EnvMillis("CROSSDESK_GEOIP_FAILURE_MAX_TTL_MS",
                                   kDefaultGeoIpFailureMaxTtlMs, 0, 86400000);
  max_delay_ms = std::max(delay_ms, max_delay_ms);
  if (delay_ms <= 0) {
    return std::chrono::milliseconds(0);
  }

  for (int i = 1; i < failure_count && delay_ms < max_delay_ms; ++i) {
    delay_ms = delay_ms > max_delay_ms / 2 ? max_delay_ms : delay_ms * 2;
  }
  return std::chrono::milliseconds(delay_ms);
}

void SetJsonResponse(server::connection_ptr con,
                     websocketpp::http::status_code::value status,
                     const json& body) {
  con->set_status(status);
  con->append_header("Content-Type", "application/json; charset=utf-8");
  con->append_header("Access-Control-Allow-Origin", "*");
  con->append_header("Access-Control-Allow-Methods", "GET, OPTIONS");
  con->append_header("Access-Control-Allow-Headers", "Content-Type");
  con->append_header("Cache-Control", "no-store");
  con->set_body(body.dump());
}

void SetAdminResponse(server::connection_ptr con,
                      const AdminHttpResponse& response) {
  con->set_status(
      static_cast<websocketpp::http::status_code::value>(response.status));
  if (!response.content_type.empty()) {
    con->append_header("Content-Type", response.content_type);
  }
  for (const auto& header : response.headers) {
    con->append_header(header.first, header.second);
  }
  con->set_body(response.body);
}

void RestorePersistedRemoteControlSessions(
    const std::shared_ptr<TransmissionManager>& transmission,
    DeviceDBManager* db) {
  if (!transmission || !db) {
    return;
  }

  size_t restored_connections = 0;
  for (const auto& session : db->ListRemoteControlSessions(
           static_cast<size_t>(std::numeric_limits<int>::max()), 0, "")) {
    transmission->BindHostToTransmission(session.host_id,
                                         session.transmission_id);
    for (const auto& guest_id : session.guest_ids) {
      if (transmission->BindGuestToTransmission(guest_id,
                                                session.transmission_id)) {
        ++restored_connections;
      }
    }
  }
  if (restored_connections > 0) {
    LOG_INFO("Restored {} persisted remote control connection(s)",
             restored_connections);
  }
}

}  // namespace

SignalServer::SignalServer()
    : SignalServer(9090, "/var/lib/crossdesk/certs",
                   "/var/lib/crossdesk/db/crossdesk-server.db") {}

SignalServer::SignalServer(uint16_t port, std::string certs_dir,
                           std::string db_path)
    : port_(port), certs_dir_(std::move(certs_dir)) {
  LOG_INFO(
      "Starting CrossDesk Signaling Server on port {}, certs_dir: {}, "
      "db_path: {}",
      port_, certs_dir_, db_path);

  server_.set_error_channels(websocketpp::log::elevel::none);
  server_.set_access_channels(websocketpp::log::alevel::none);
  server_.init_asio();

  server_.set_open_handler(
      std::bind(&SignalServer::OnOpen, this, std::placeholders::_1));
  server_.set_message_handler(std::bind(&SignalServer::OnMessage, this,
                                        std::placeholders::_1,
                                        std::placeholders::_2));
  server_.set_http_handler(
      std::bind(&SignalServer::OnHttp, this, std::placeholders::_1));
  server_.set_tls_init_handler(
      std::bind(&SignalServer::OnTlsInit, this, std::placeholders::_1));
  auto heartbeat_handler = std::bind(&SignalServer::OnHeartbeat, this,
                                     std::placeholders::_1,
                                     std::placeholders::_2);
  server_.set_ping_handler(heartbeat_handler);
  server_.set_pong_handler(heartbeat_handler);

  transmission_manager_ = std::make_shared<TransmissionManager>(false);
  device_db_manager_ = std::make_unique<DeviceDBManager>(db_path);
  transmission_manager_->SetRemoteControlSessionCallback(
      [this](const std::string& transmission_id, const std::string& host_id,
             const std::string& guest_id, bool started) {
        if (!device_db_manager_) {
          return;
        }
        if (started) {
          device_db_manager_->StartRemoteControlSession(transmission_id,
                                                        host_id, guest_id);
        } else {
          device_db_manager_->EndRemoteControlSession(transmission_id, host_id,
                                                      guest_id);
        }
      });
  RestorePersistedRemoteControlSessions(transmission_manager_,
                                        device_db_manager_.get());
  signal_negotiation_ = std::make_unique<SignalNegotiation>(
      transmission_manager_, device_db_manager_.get(), nullptr,
      CreateIceServerConfigIssuer());
  signal_negotiation_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg,
                                                    this, std::placeholders::_1,
                                                    std::placeholders::_2));
  if (GeoLocationResolver::IsEnabled()) {
    geo_location_resolver_ = std::make_unique<GeoLocationResolver>();
  }
  presence_manager_ = std::make_unique<PresenceManager>();
  presence_manager_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg, this,
                                                  std::placeholders::_1,
                                                  std::placeholders::_2));
  presence_manager_->SetDeviceDB(device_db_manager_.get());
  presence_manager_->SetSendToDeviceCallback(
      [this](const std::string& id, json msg) {
        SendMsg(transmission_manager_->GetWsHandle(id), msg);
      });
  admin_auth_ = std::make_unique<AdminAuth>();
  admin_controller_ = std::make_unique<AdminController>(
      admin_auth_.get(), presence_manager_.get(), transmission_manager_,
      device_db_manager_.get(), [this](const std::string& id, json msg) {
        SendMsg(transmission_manager_->GetWsHandle(id), msg);
      });
  admin_read_db_ = std::make_unique<DeviceDBManager>(
      db_path, DeviceDBManager::OpenMode::ReadOnly);
  admin_read_controller_ = std::make_unique<AdminController>(
      admin_auth_.get(), presence_manager_.get(), transmission_manager_,
      admin_read_db_.get(), nullptr, std::chrono::milliseconds(1000));
  max_connections_ = EnvMillis("CROSSDESK_MAX_CONNECTIONS", 2048, 1, 65536);
  auto on_error = [this](std::exception_ptr error) { WorkerFailed(error); };
  application_worker_ =
      std::make_unique<BoundedExecutor>(1024, max_connections_ + 4, on_error);
  admin_worker_ = std::make_unique<BoundedExecutor>(32, 0, on_error);
  maintenance_worker_ = std::make_unique<BoundedExecutor>(2, 0, on_error);
  server_.set_max_message_size(64 * 1024);
  server_.set_max_http_body_size(16 * 1024);
  server_.set_open_handshake_timeout(10000);
  server_.set_close_handshake_timeout(3000);
  if (geo_location_resolver_) StartClientNetworkInfoWorker();
}

SignalServer::~SignalServer() {
  stopping_ = true;
  StopClientNetworkInfoWorker();
  // Join before any manager or endpoint is destroyed. Worker completions only
  // post back to the still-owned io_service; no worker accesses a connection.
  if (admin_worker_) admin_worker_->Stop();
  if (application_worker_) application_worker_->Stop();
  if (maintenance_worker_) maintenance_worker_->Stop();
}

std::string SignalServer::GetClientIp(websocketpp::connection_hdl hdl,
                                    uint64_t id) {
  auto warn = [this, id](const std::string& error) {
    ++diagnostics_.peer_failed;
    if (Clock::now() < next_peer_warning_) return;
    LOG_WARN("Failed to get websocket peer endpoint: {} connection={} "
             "peer_endpoint_failures_total={}",
             error, id, diagnostics_.peer_failed);
    next_peer_warning_ = Clock::now() + kDiagnosticWarningInterval;
  };
  try {
    server::connection_ptr con = server_.get_con_from_hdl(hdl);
    websocketpp::lib::asio::error_code ec;
    auto endpoint = con->get_raw_socket().remote_endpoint(ec);
    if (ec) {
      warn(ec.message());
      return "";
    }
    return endpoint.address().to_string();
  } catch (const std::exception& e) {
    warn(e.what());
  }
  return "";
}

void SignalServer::EnqueueClientNetworkInfo(const std::string& client_ip,
                                            const std::string& device_id) {
  if (!presence_manager_ || device_id.empty()) return;
  ClientNetworkInfo network_info;
  network_info.client_ip = client_ip;
  presence_manager_->SetDeviceNetworkInfo(device_id, network_info);
  if (geo_location_resolver_) {
    EnqueueGeoIpLookup(client_ip, std::chrono::milliseconds(0));
  }
}

void SignalServer::EnqueueGeoIpLookup(const std::string& client_ip,
                                      std::chrono::milliseconds delay) {
  if (client_ip.empty()) {
    return;
  }

  const auto run_at = std::chrono::steady_clock::now() +
                      std::max(delay, std::chrono::milliseconds(0));
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    if (network_info_stop_) {
      return;
    }
    auto pending = pending_ip_lookup_at_.find(client_ip);
    if (pending != pending_ip_lookup_at_.end()) {
      return;
    }
    if (network_info_jobs_.size() >= kMaxClientNetworkInfoJobs) {
      if (GeoLocationResolver::IsEnabled()) {
        LOG_WARN("GeoIP lookup queue is full, dropping [{}]", client_ip);
      }
      return;
    }
    pending_ip_lookup_at_[client_ip] = run_at;
    network_info_jobs_.push({client_ip, run_at});
  }
  network_info_cv_.notify_one();
}

void SignalServer::ProcessGeoIpLookup(const GeoIpLookupJob& job) {
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    auto pending = pending_ip_lookup_at_.find(job.client_ip);
    if (pending == pending_ip_lookup_at_.end() ||
        pending->second != job.run_at) {
      return;
    }
  }

  if (!presence_manager_ ||
      !presence_manager_->HasDeviceWithClientIp(job.client_ip)) {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
    return;
  }

  GeoLocationResolveResult resolve_result;
  resolve_result.info.client_ip = job.client_ip;
  if (geo_location_resolver_) {
    resolve_result =
        geo_location_resolver_->ResolveWithRetryInfo(job.client_ip);
  }

  if (!presence_manager_->HasDeviceWithClientIp(job.client_ip)) {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
    return;
  }

  const ClientNetworkInfo& network_info = resolve_result.info;
  if (resolve_result.resolved) {
    {
      std::lock_guard<std::mutex> lock(network_info_mutex_);
      pending_ip_lookup_at_.erase(job.client_ip);
      geo_ip_failure_counts_.erase(job.client_ip);
    }
    size_t updated = presence_manager_->UpdateDevicesWithClientIp(job.client_ip,
                                                                  network_info);
    if (GeoLocationResolver::IsEnabled()) {
      LOG_INFO("GeoIP lookup for [{}] resolved [{}] and updated {} client(s)",
               job.client_ip, network_info.location, updated);
    }
    return;
  }

  if (GeoLocationResolver::IsEnabled()) {
    LOG_INFO("GeoIP lookup for [{}] returned Unknown", job.client_ip);
  }
  if (!resolve_result.retryable) {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
    return;
  }

  int failure_count = 0;
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    failure_count = ++geo_ip_failure_counts_[job.client_ip];
  }
  std::chrono::milliseconds retry_delay = GeoIpFailureRetryDelay(failure_count);
  if (retry_delay.count() > 0 &&
      presence_manager_->HasDeviceWithClientIp(job.client_ip)) {
    const auto retry_at = std::chrono::steady_clock::now() + retry_delay;
    {
      std::lock_guard<std::mutex> lock(network_info_mutex_);
      pending_ip_lookup_at_.erase(job.client_ip);
      if (!network_info_stop_ &&
          network_info_jobs_.size() < kMaxClientNetworkInfoJobs) {
        pending_ip_lookup_at_[job.client_ip] = retry_at;
        network_info_jobs_.push({job.client_ip, retry_at});
      } else {
        if (GeoLocationResolver::IsEnabled()) {
          LOG_WARN("GeoIP lookup queue is full, dropping [{}]", job.client_ip);
        }
      }
    }
    network_info_cv_.notify_one();
  } else {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    pending_ip_lookup_at_.erase(job.client_ip);
    geo_ip_failure_counts_.erase(job.client_ip);
  }
}

void SignalServer::StartClientNetworkInfoWorker() {
  if (network_info_worker_.joinable()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    network_info_stop_ = false;
  }
  network_info_worker_ =
      std::thread(&SignalServer::ProcessClientNetworkInfoJobs, this);
}

void SignalServer::StopClientNetworkInfoWorker() {
  {
    std::lock_guard<std::mutex> lock(network_info_mutex_);
    network_info_stop_ = true;
    decltype(network_info_jobs_) empty_jobs;
    network_info_jobs_.swap(empty_jobs);
    pending_ip_lookup_at_.clear();
    geo_ip_failure_counts_.clear();
  }
  network_info_cv_.notify_all();
  if (network_info_worker_.joinable()) {
    network_info_worker_.join();
  }
}

void SignalServer::ProcessClientNetworkInfoJobs() {
  while (true) {
    GeoIpLookupJob job;
    {
      std::unique_lock<std::mutex> lock(network_info_mutex_);
      while (true) {
        network_info_cv_.wait(lock, [this] {
          return network_info_stop_ || !network_info_jobs_.empty();
        });
        if (network_info_stop_) {
          return;
        }

        const auto run_at = network_info_jobs_.top().run_at;
        const auto now = std::chrono::steady_clock::now();
        if (run_at <= now) {
          job = network_info_jobs_.top();
          network_info_jobs_.pop();
          break;
        }

        network_info_cv_.wait_until(lock, run_at, [this, run_at] {
          return network_info_stop_ ||
                 (!network_info_jobs_.empty() &&
                  network_info_jobs_.top().run_at < run_at);
        });
        if (network_info_stop_) {
          return;
        }
      }
    }

    try {
      ProcessGeoIpLookup(job);
    } catch (...) {
      WorkerFailed(std::current_exception());
      return;
    }
  }
}

// TLS contexts are immutable once published. Existing SSL streams keep the old
// shared_ptr alive, including across certificate rotation and failed reloads.
context_ptr SignalServer::OnTlsInit(websocketpp::connection_hdl) {
  return std::atomic_load(&tls_context_);
}

void SignalServer::ReloadTlsContext() {
  const auto cert = certs_dir_ + "/api.crossdesk.cn_bundle.crt";
  const auto key = certs_dir_ + "/api.crossdesk.cn.key";
  auto stamp = [](const std::string& path) {
    return std::to_string(
               std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::filesystem::last_write_time(path).time_since_epoch())
                   .count()) +
           ":" + std::to_string(std::filesystem::file_size(path));
  };
  const auto generation = stamp(cert) + ":" + stamp(key);
  if (generation == tls_generation_) return;
  namespace asio = websocketpp::lib::asio;
  auto ctx = websocketpp::lib::make_shared<asio::ssl::context>(
      asio::ssl::context::sslv23);
  ctx->set_options(asio::ssl::context::default_workarounds |
                   asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 |
                   asio::ssl::context::single_dh_use);
  ctx->use_certificate_chain_file(cert);
  ctx->use_private_key_file(key, asio::ssl::context::pem);
  if (SSL_CTX_check_private_key(ctx->native_handle()) != 1)
    throw std::runtime_error("TLS certificate and private key do not match");
  if (SSL_CTX_set_cipher_list(
          ctx->native_handle(),
          "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
          "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256") != 1)
    throw std::runtime_error("TLS cipher configuration failed");
  if (generation != stamp(cert) + ":" + stamp(key))
    throw std::runtime_error("TLS files changed during reload; retrying later");
  std::atomic_store(&tls_context_, ctx);
  tls_generation_ = generation;
  LOG_INFO("TLS context loaded and published for new connections");
}

void SignalServer::RetryAccept() {
  if (stopping_ || accept_retry_pending_) return;
  accept_retry_pending_ = true;
  server_.set_timer(500, [this](websocketpp::lib::error_code ec) {
    accept_retry_pending_ = false;
    if (!ec) AcceptNext();
  });
}

int64_t SignalServer::EstimateFileDescriptors() const {
  return fd_usage_.open < 0
             ? -1
             : fd_usage_.open + static_cast<int64_t>(connections_.size()) -
                   static_cast<int64_t>(fd_connections_at_sample_);
}

std::string SignalServer::DescribeConnectionResources() {
  size_t opened = 0, authenticated = 0, pending_http = 0, inactive = 0;
  size_t expired_handles = 0;
  int64_t oldest_unopened_ms = 0;
  const auto now = Clock::now();
  for (const auto& entry : connections_) {
    const auto& state = *entry.second;
    opened += state.opened;
    authenticated += state.authenticated;
    pending_http += static_cast<bool>(state.pending_http);
    inactive += !state.alive.load();
    expired_handles += entry.first.expired();
    if (!state.opened)
      oldest_unopened_ms = std::max<int64_t>(
          oldest_unopened_ms,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - state.accepted).count());
  }
  // Fresh diagnostic sampling does not change the admission control sample.
  const auto fds = ReadFileDescriptorUsage();
  return fmt::format(
      "connections={} connection_limit={} ws_opened={} authenticated={} "
      "unopened={} unopened_limit={} pending_http={} inactive={} "
      "expired_handles={} oldest_unopened_ms={} pending_cleanup={} "
      "pending_sends={} fd_open={} fd_estimated={} fd_sample_age_ms={} fd_soft_limit={} "
      "fd_reserve={} admission_paused={} pause_ms={}",
      connections_.size(), max_connections_, opened, authenticated,
      connections_.size() - opened, max_handshakes_, pending_http, inactive,
      expired_handles, oldest_unopened_ms, pending_cleanup_,
      pending_sends_.load(), fds.open, EstimateFileDescriptors(),
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - fd_sampled_at_).count(), fds.soft_limit,
      fd_reserve_, admission_paused_,
      admission_paused_
          ? std::chrono::duration_cast<std::chrono::milliseconds>(
                now - admission_paused_since_).count()
          : 0);
}

void SignalServer::LogConnectionDiagnostics() {
  LOG_INFO(
      "Connection diagnostics: {} accepted_total={} ws_opened_total={} "
      "preopen_failures_total={} peer_endpoint_failures_total={} "
      "accept_failures_total={} init_resource_failures_total={} "
      "connection_limit_checks_total={} unopened_limit_checks_total={} "
      "fd_limit_checks_total={} fd_sample_failed_checks_total={} "
      "event_loop_stalls_total={}",
      DescribeConnectionResources(), diagnostics_.accepted, diagnostics_.opened,
      diagnostics_.preopen_failed, diagnostics_.peer_failed,
      diagnostics_.accept_failed, diagnostics_.init_resource_failed,
      diagnostics_.connection_limit_checks, diagnostics_.unopened_limit_checks,
      diagnostics_.fd_limit_checks, diagnostics_.fd_sample_failed_checks,
      diagnostics_.loop_stalls);
}

void SignalServer::AcceptNext() {
  if (stopping_ || accept_pending_) return;
  const auto now = Clock::now();
  if (now - fd_sampled_at_ >= std::chrono::seconds(1)) {
    fd_usage_ = ReadFileDescriptorUsage();
    fd_sampled_at_ = now;
    fd_connections_at_sample_ = connections_.size();
  }
  const auto estimated_fds = EstimateFileDescriptors();
  size_t handshakes = 0;
  for (const auto& entry : connections_)
    if (!entry.second->opened) ++handshakes;
  const bool connection_limit =
      connections_.size() + pending_cleanup_ >= max_connections_;
  const bool unopened_limit = handshakes >= max_handshakes_;
  const bool fd_sample_failed = fd_usage_.soft_limit >= 0 && estimated_fds < 0;
  const bool fd_limit = fd_usage_.soft_limit >= 0 && estimated_fds >= 0 &&
      estimated_fds + static_cast<int64_t>(fd_reserve_) >= fd_usage_.soft_limit;
  if (connection_limit || unopened_limit || fd_sample_failed || fd_limit) {
    diagnostics_.connection_limit_checks += connection_limit;
    diagnostics_.unopened_limit_checks += unopened_limit;
    diagnostics_.fd_limit_checks += fd_limit;
    diagnostics_.fd_sample_failed_checks += fd_sample_failed;
    if (!admission_paused_) {
      admission_paused_ = true;
      admission_paused_since_ = now;
      admission_pause_checks_ = 0;
    }
    ++admission_pause_checks_;
    if (now >= next_admission_warning_) {
      LOG_WARN("Connection admission paused: blocked_by_connections={} "
               "blocked_by_unopened={} blocked_by_fd={} blocked_by_fd_sample={} "
               "pause_checks={} retry_ms=500 {}",
               connection_limit, unopened_limit, fd_limit, fd_sample_failed,
               admission_pause_checks_, DescribeConnectionResources());
      admission_pause_logged_ = true;
      next_admission_warning_ = now + kDiagnosticWarningInterval;
    }
    RetryAccept();
    return;
  }
  if (admission_paused_) {
    admission_paused_ = false;
    // Pair only reported pauses with a recovery record, so flapping around a
    // limit cannot bypass rate limiting. Cumulative counters include all checks.
    if (admission_pause_logged_)
      LOG_INFO("Connection admission resumed: paused_ms={} pause_checks={} {}",
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   now - admission_paused_since_).count(),
               admission_pause_checks_, DescribeConnectionResources());
    admission_pause_logged_ = false;
  }

  namespace asio = websocketpp::lib::asio;
  auto socket =
      std::make_shared<asio::ip::tcp::socket>(server_.get_io_service());
  accept_pending_ = true;
  acceptor_->async_accept(*socket, [this, socket](asio::error_code error) {
    accept_pending_ = false;
    if (stopping_) return;  // RAII closes the accepted socket, if any.
    if (error) {
      ++diagnostics_.accept_failed;
      if (error == asio::error::bad_descriptor ||
          error == asio::error::invalid_argument)
        throw std::runtime_error("Accept listener state is invalid: " +
                                 error.message());
      if (Clock::now() >= next_accept_warning_) {
        LOG_WARN("Connection admission failed: {} ({}) category={} "
                 "accept_failures_total={}; retry_ms=500 {}",
                 error.message(), error.value(), error.category().name(),
                 diagnostics_.accept_failed, DescribeConnectionResources());
        next_accept_warning_ = Clock::now() + std::chrono::seconds(30);
      }
      fd_sampled_at_ = {};
      RetryAccept();
      return;
    }
    ++diagnostics_.accepted;
    server::connection_ptr con;
    try {
      // Construct SSL only after TCP accept. A pending accept must not pin an
      // obsolete TLS context across a certificate reload.
      con = server_.get_connection();
      if (!con) throw std::runtime_error("Connection initialization failed");
      con->get_raw_socket() = std::move(*socket);
      auto state = std::make_shared<ConnectionState>();
      state->id = next_connection_id_++;
      state->ip = GetClientIp(con->get_handle(), state->id);
      connections_.emplace(con->get_handle(), state);
      con->set_termination_handler([this](server::connection_ptr finished) {
        FinishConnection(finished);
      });
      con->start();
    } catch (const std::system_error& e) {
      if (con)
        con->terminate(
            websocketpp::error::make_error_code(websocketpp::error::general));
      if (e.code() != std::errc::too_many_files_open &&
          e.code() != std::errc::too_many_files_open_in_system &&
          e.code() != std::errc::no_buffer_space)
        throw;
      ++diagnostics_.init_resource_failed;
      if (Clock::now() >= next_accept_warning_) {
        LOG_WARN("Connection initialization resource failure: {} ({}) "
                 "category={} init_resource_failures_total={}; retry_ms=500 {}",
                 e.code().message(), e.code().value(), e.code().category().name(),
                 diagnostics_.init_resource_failed, DescribeConnectionResources());
        next_accept_warning_ = Clock::now() + kDiagnosticWarningInterval;
      }
      fd_sampled_at_ = {};
      RetryAccept();
      return;
    }
    AcceptNext();
  });
}

bool SignalServer::OnOpen(websocketpp::connection_hdl hdl) {
  auto it = connections_.find(hdl);
  if (it == connections_.end()) return false;
  auto& state = *it->second;
  state.opened = true;
  ++diagnostics_.opened;
  state.last_heartbeat = Clock::now();
  LOG_INFO("Websocket connection [{}] opened from [{}]", state.id, state.ip);
  return true;
}

void SignalServer::QueueSessionCleanup(
    websocketpp::connection_hdl hdl,
    const std::shared_ptr<ConnectionState>& state) {
  if (!state->alive.exchange(false) || !state->opened) return;
  ++pending_cleanup_;
  if (!application_worker_->Submit(
          [this, hdl] {
            // Login, release and presence changes are ordered on this one
            // worker. A release of an old handle cannot log out a newer handle
            // for that ID.
            const auto id = transmission_manager_->ReleaseUserSession(hdl);
            if (!id.empty()) {
              presence_manager_->OnLogout(id);
              signal_negotiation_->OnWebClientDisconnect(id);
            }
            server_.get_io_service().post([this] { --pending_cleanup_; });
          },
          true)) {
    throw std::runtime_error("Lifecycle queue reserve exhausted");
  }
}

// All teardown converges here, including TLS failures and plain HTTP requests.
void SignalServer::FinishConnection(server::connection_ptr con) {
  auto hdl = con->get_handle();
  auto it = connections_.find(hdl);
  if (it == connections_.end()) return;
  auto state = it->second;
  QueueSessionCleanup(hdl, state);
  if (state->opened) {
    LOG_INFO(
        "Websocket connection [{}|{}] closed code={} error=[{}]",
        state->id, state->device_id, con->get_local_close_code(),
        con->get_ec().message());
  } else if (con->get_ec() != websocketpp::error::http_connection_ended) {
    ++diagnostics_.preopen_failed;
    // Bound warning volume for failed/unauthenticated connection floods.
    if (Clock::now() >= next_preopen_warning_) {
      LOG_WARN(
          "Connection [{}] failed before websocket open error=[{}] "
          "transport=[{}] ip=[{}] age_ms={} preopen_failures_total={}",
          state->id, con->get_ec().message(),
          con->get_transport_ec().message(), state->ip,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              Clock::now() - state->accepted).count(),
          diagnostics_.preopen_failed);
      next_preopen_warning_ = Clock::now() + std::chrono::seconds(30);
    }
  }
  state->pending_http.reset();
  connections_.erase(it);
}

void SignalServer::CloseConnection(websocketpp::connection_hdl hdl,
                                   const char* reason,
                                   websocketpp::close::status::value code) {
  auto it = connections_.find(hdl);
  if (it == connections_.end() || !it->second->alive) return;
  QueueSessionCleanup(hdl, it->second);
  websocketpp::lib::error_code ec;
  auto con = server_.get_con_from_hdl(hdl, ec);
  if (ec) return;
  if (con->get_state() == websocketpp::session::state::open) {
    con->close(code, reason, ec);
  } else if (con->get_state() == websocketpp::session::state::connecting) {
    con->terminate(websocketpp::error::make_error_code(
        websocketpp::error::open_handshake_timeout));
  }
}

bool SignalServer::OnHeartbeat(websocketpp::connection_hdl hdl, std::string) {
  auto it = connections_.find(hdl);
  if (it == connections_.end() || !it->second->alive) return false;
  it->second->last_heartbeat = Clock::now();
  return true;
}

void SignalServer::OnHttp(websocketpp::connection_hdl hdl) {
  auto con = server_.get_con_from_hdl(hdl);
  auto it = connections_.find(hdl);
  if (it == connections_.end()) return;
  const auto resource = con->get_resource();
  const bool admin = AdminController::IsAdminRoute(resource);
  if (!admin && resource != "/stats" && resource != "/api/stats") {
    SetJsonResponse(con, websocketpp::http::status_code::not_found,
                    {{"error", "not_found"}});
    return;
  }
  AdminHttpRequest request{con->get_request().get_method(), resource,
                           con->get_request_body(),
                           con->get_request_header("Cookie")};
  auto state = it->second;
  // Explicitly retain deferred connections: defer_http_response cancels the
  // library's handshake timer. Our deadline below bounds their lifetime.
  con->defer_http_response();
  state->pending_http = con;
  state->accepted = Clock::now();
  const bool reader = !admin || request.method != "POST";
  auto* worker = reader ? admin_worker_.get() : application_worker_.get();
  if (!worker->Submit([this, hdl, state, request = std::move(request), admin,
                       reader] {
        if (!state->alive) return;
        const auto started = Clock::now();
        AdminHttpResponse response;
        if (reader)
          admin_read_db_->SetReadDeadline(
              started + std::chrono::milliseconds(http_timeout_ms_ / 2));
        try {
          if (admin) {
            auto* controller = reader ? admin_read_controller_.get()
                                      : admin_controller_.get();
            response = controller->Handle(request);
          } else {
            response = {200,
                        "application/json; charset=utf-8",
                        {{"Cache-Control", "no-store"},
                         {"Access-Control-Allow-Origin", "*"}},
                        admin_read_controller_->GetPublicStats().dump()};
          }
        } catch (const std::bad_alloc&) {
          throw;
        } catch (...) {
          response = {
              500, "application/json", {}, "{\"error\":\"internal_error\"}"};
        }
        if (reader && admin_read_db_->ClearReadDeadline()) {
          admin_read_controller_->InvalidateStatsCache();
          response = {503,
                      "application/json",
                      {{"Retry-After", "1"}},
                      "{\"error\":\"query_timeout\"}"};
        }
        server_.get_io_service().post(
            [this, hdl, response = std::move(response)]() mutable {
              CompleteHttp(hdl, std::move(response));
            });
      })) {
    CompleteHttp(hdl, {503,
                       "application/json",
                       {{"Retry-After", "1"}},
                       "{\"error\":\"busy\"}"});
  }
}

void SignalServer::CompleteHttp(websocketpp::connection_hdl hdl,
                                AdminHttpResponse response) {
  auto it = connections_.find(hdl);
  if (it == connections_.end() || !it->second->alive) return;
  auto con = std::move(it->second->pending_http);
  if (!con) return;
  SetAdminResponse(con, response);
  websocketpp::lib::error_code ec;
  con->send_http_response(ec);
  if (ec) con->terminate(ec);
}

void SignalServer::ScheduleRuntimeHeartbeat() {
  if (runtime_job_pending_ || stopping_) return;
  runtime_job_pending_ = application_worker_->Submit([this] {
    device_db_manager_->RecordRuntimeHeartbeat();
    server_.get_io_service().post([this] { runtime_job_pending_ = false; });
  });
}

void SignalServer::ScheduleRecoveredSessionCleanup() {
  server_.set_timer(
      kRecoveredSessionCleanupDelayMs, [this](websocketpp::lib::error_code ec) {
        if (ec || stopping_) return;
        if (!application_worker_->Submit(
                [this] {
                  const auto pruned =
                      transmission_manager_->PruneDisconnectedTransmissions();
                  LOG_INFO(
                      "Pruned {} disconnected restored remote control "
                      "connection(s)",
                      pruned);
                },
                true))
          throw std::runtime_error("Cannot enqueue recovered session cleanup");
      });
}

void SignalServer::ScheduleMaintenance() {
  if (stopping_) return;
  const auto due = Clock::now() + std::chrono::milliseconds(check_interval_ms_);
  server_.set_timer(check_interval_ms_, [this,
                                         due](websocketpp::lib::error_code ec) {
    if (ec || stopping_) return;
    const auto now = Clock::now();
    if (now - due >= std::chrono::milliseconds(check_interval_ms_)) {
      ++diagnostics_.loop_stalls;
      if (now >= next_loop_warning_) {
        LOG_WARN("Signal event loop delayed: delay_ms={} event_loop_stalls_total={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     now - due).count(), diagnostics_.loop_stalls);
        next_loop_warning_ = now + kDiagnosticWarningInterval;
      }
    }
    if (now >= next_diagnostics_) {
      LogConnectionDiagnostics();
      next_diagnostics_ = now + kDiagnosticInterval;
    }
    // If the loop stalled, give queued ping frames one full check interval to
    // drain before considering expiry; never use a wall-clock timestamp.
    if (now - due < std::chrono::milliseconds(check_interval_ms_)) {
      size_t expired = 0;
      for (const auto& entry : connections_) {
        const auto& state = entry.second;
        if (!state->alive) continue;
        websocketpp::lib::error_code state_error;
        auto con = server_.get_con_from_hdl(entry.first, state_error);
        if (state->opened && !state_error &&
            con->get_state() != websocketpp::session::state::open) {
          // The peer may have completed WebSocket close while TLS shutdown is
          // still pending. Stop routing immediately; don't label this a
          // heartbeat timeout.
          QueueSessionCleanup(entry.first, state);
          if (++expired == 64) break;
          continue;
        }
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - state->last_heartbeat)
                       .count();
        auto lifetime = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - state->accepted)
                            .count();
        if (state->opened && state->authenticated &&
            age > heartbeat_timeout_ms_) {
          LOG_INFO("Device [{}] heartbeat timed out connection={}",
                   state->device_id, state->id);
          CloseConnection(entry.first, "Heartbeat timeout",
                          websocketpp::close::status::going_away);
        } else if (state->opened && !state->authenticated &&
                   lifetime > authentication_timeout_ms_) {
          CloseConnection(entry.first, "Authentication timeout",
                          websocketpp::close::status::policy_violation);
        } else if (state->pending_http && lifetime > http_timeout_ms_) {
          CloseConnection(entry.first, "HTTP response timeout",
                          websocketpp::close::status::going_away);
        } else {
          continue;
        }
        // Limit callback/close work per tick. Database teardown is queued.
        if (++expired == 64) break;
      }
    }
    ScheduleRuntimeHeartbeat();
    if (now >= next_tls_reload_ && !tls_job_pending_) {
      tls_job_pending_ = maintenance_worker_->Submit([this] {
        try {
          ReloadTlsContext();
        } catch (const std::bad_alloc&) {
          throw;
        } catch (const std::exception& e) {
          LOG_WARN("TLS reload rejected; retaining active context: {}",
                   e.what());
        }
        server_.get_io_service().post([this] { tls_job_pending_ = false; });
      });
      next_tls_reload_ =
          now + std::chrono::milliseconds(tls_reload_interval_ms_);
    }
    ScheduleMaintenance();
  });
}

void SignalServer::WorkerFailed(std::exception_ptr error) {
  {
    std::lock_guard<std::mutex> lock(worker_error_mutex_);
    if (!worker_error_) worker_error_ = error;
  }
  Stop();
}

void SignalServer::Stop() {
  if (stopping_.exchange(true)) return;
  server_.get_io_service().post([this] {
    LOG_INFO("Signal server shutdown requested");
    LogConnectionDiagnostics();
    if (signals_) signals_->cancel();
    websocketpp::lib::error_code ec;
    if (acceptor_) acceptor_->close(ec);
    for (const auto& entry : connections_)
      CloseConnection(entry.first, "Server shutdown",
                      websocketpp::close::status::going_away);
    // Bound process shutdown even when a peer never acknowledges close/TLS
    // shutdown. Workers are joined before dependent objects are destroyed.
    server_.set_timer(5000,
                      [this](websocketpp::lib::error_code) { server_.stop(); });
  });
}

void SignalServer::Run() {
  ReloadTlsContext();  // Invalid startup configuration is a process-level
                       // error.
  fd_usage_ = ReadFileDescriptorUsage();
  const auto configured_max_connections = max_connections_;
  if (fd_usage_.soft_limit >= 0) {
    if (fd_usage_.open < 0 ||
        fd_usage_.soft_limit <=
            fd_usage_.open + static_cast<int64_t>(fd_reserve_))
      throw std::runtime_error(
          "Insufficient file descriptors for configured reserve");
    max_connections_ = std::min<size_t>(
        max_connections_, fd_usage_.soft_limit - fd_usage_.open - fd_reserve_);
  }
  LOG_INFO("Connection capacity: configured_max_connections={} "
           "effective_max_connections={} fd_open={} fd_soft_limit={} "
           "fd_reserve={} unopened_limit={} accept_retry_ms=500 listen_backlog=128",
           configured_max_connections, max_connections_, fd_usage_.open,
           fd_usage_.soft_limit, fd_reserve_, max_handshakes_);
  if (max_connections_ < configured_max_connections)
    LOG_WARN("Configured connection capacity reduced by process nofile limit: "
             "configured={} effective={} fd_soft_limit={}",
             configured_max_connections, max_connections_, fd_usage_.soft_limit);
  namespace asio = websocketpp::lib::asio;
  acceptor_ =
      std::make_unique<asio::ip::tcp::acceptor>(server_.get_io_service());
  acceptor_->open(asio::ip::tcp::v4());
  acceptor_->set_option(asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_->bind(asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port_));
  acceptor_->listen(128);
  signals_ = std::make_unique<websocketpp::lib::asio::signal_set>(
      server_.get_io_service(), SIGINT, SIGTERM);
  signals_->async_wait([this](websocketpp::lib::error_code ec, int) {
    if (!ec) Stop();
  });
  AcceptNext();
  ScheduleMaintenance();
  ScheduleRecoveredSessionCleanup();
  LOG_INFO("Signal server listening on port [{}], waiting for connections...",
           port_);
  // Never stop/re-listen a used endpoint on exception. Unknown exceptions,
  // invalid listener state and allocation failure exit for the supervisor.
  server_.run();
  std::lock_guard<std::mutex> lock(worker_error_mutex_);
  if (worker_error_) std::rethrow_exception(worker_error_);
  if (!stopping_)
    throw std::runtime_error("Signal event loop exited unexpectedly");
}

void SignalServer::RequestBackpressureClose(websocketpp::connection_hdl hdl) {
  std::lock_guard<std::mutex> lock(backpressure_mutex_);
  // Unique live handles are bounded by admission. Coalesce all overload
  // notifications into one I/O callback rather than flooding asio::post.
  if (backpressure_connections_.size() < max_connections_)
    backpressure_connections_.insert(hdl);
  if (backpressure_posted_) return;
  backpressure_posted_ = true;
  server_.get_io_service().post([this] {
    decltype(backpressure_connections_) affected;
    {
      std::lock_guard<std::mutex> lock(backpressure_mutex_);
      affected.swap(backpressure_connections_);
      backpressure_posted_ = false;
    }
    for (const auto& handle : affected)
      CloseConnection(handle, "Send backlog limit",
                      websocketpp::close::status::try_again_later);
  });
}

void SignalServer::SendMsg(websocketpp::connection_hdl hdl, json message) {
  const auto type = message.value("type", "");
  if (hdl.expired()) return;
  if (stopping_) return;
  if (pending_sends_.fetch_add(1) >= 1024) {
    --pending_sends_;
    RequestBackpressureClose(hdl);
    return;
  }
  std::string login_id;
  if (type == "login" && message.value("status", "") == "success") {
    login_id = message.value("user_id", "");
    login_id = login_id.substr(0, login_id.find('@'));
  }
  auto payload = message.dump();
  server_.get_io_service().post(
      [this, hdl, payload = std::move(payload), login_id] {
        --pending_sends_;
        auto it = connections_.find(hdl);
        if (it == connections_.end() || !it->second->alive) return;
        if (!login_id.empty()) {
          it->second->device_id = login_id;
          it->second->authenticated = true;
        }
        websocketpp::lib::error_code ec;
        auto con = server_.get_con_from_hdl(hdl, ec);
        if (ec) return;
        if (con->get_buffered_amount() + payload.size() > 1024 * 1024) {
          CloseConnection(hdl, "Send backlog limit",
                          websocketpp::close::status::policy_violation);
          return;
        }
        ec = con->send(payload, websocketpp::frame::opcode::text);
        if (ec) LOG_ERROR("Failed to send message: {}", ec.message());
      });
}

void SignalServer::OnMessage(websocketpp::connection_hdl hdl,
                             server::message_ptr msg) {
  auto it = connections_.find(hdl);
  if (it == connections_.end() || !it->second->alive) return;
  // Parsing is size bounded; malformed input is rejected without logging body
  // fragments (nlohmann parse errors can include passwords or ICE credentials).
  auto j = json::parse(msg->get_payload(), nullptr, false);
  if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
    CloseConnection(hdl, "Invalid signaling message",
                    websocketpp::close::status::invalid_payload);
    return;
  }
  const auto type = j["type"].get<std::string>();
  if (type == "ping") {
    if (OnHeartbeat(hdl, "")) SendMsg(hdl, {{"type", "pong"}});
    return;
  }
  auto state = it->second;
  if (state->pending_messages.fetch_add(1) >= 128) {
    --state->pending_messages;
    CloseConnection(hdl, "Message backlog limit",
                    websocketpp::close::status::policy_violation);
    return;
  }
  if (!application_worker_->Submit([this, hdl, state, j = std::move(j)] {
        if (state->alive) ProcessMessage(hdl, j, state);
        --state->pending_messages;
      })) {
    --state->pending_messages;
    CloseConnection(hdl, "Server busy",
                    websocketpp::close::status::try_again_later);
  }
}

void SignalServer::ProcessMessage(
    websocketpp::connection_hdl hdl, const json& j,
    const std::shared_ptr<ConnectionState>& state) {
  try {
    const auto type = j["type"].get<std::string>();
    switch (HASH_STRING_PIECE(type.c_str())) {
      case "login"_H:
        signal_negotiation_->login_user(hdl, j);
        if (presence_manager_) {
          std::string id = transmission_manager_->GetUserId(hdl);
          if (!id.empty()) {
            presence_manager_->OnLogin(id, id, hdl);
            EnqueueClientNetworkInfo(state->ip, id);
          }
        }
        break;
      case "user_leave_transmission"_H:
        signal_negotiation_->leave_transmission(hdl, j);
        break;
      case "query_user_id_list"_H:
        signal_negotiation_->query_user_id_list(hdl, j);
        break;
      case "join_transmission"_H:
        signal_negotiation_->join_transmission(hdl, j);
        break;
      case "offer"_H:
        signal_negotiation_->offer(hdl, j);
        break;
      case "answer"_H:
        signal_negotiation_->answer(hdl, j);
        break;
      case "new_candidate"_H:
        signal_negotiation_->new_candidate(hdl, j);
        break;
      case "new_candidate_mid"_H:
        signal_negotiation_->new_candidate_mid(hdl, j);
        break;
      case "change_password"_H:
        signal_negotiation_->change_password(hdl, j);
        break;
      case "turn_credentials"_H:
        signal_negotiation_->turn_credentials(hdl, j);
        break;
      case "client_info"_H:
        signal_negotiation_->client_info(hdl, j);
        break;
      case "recent_connections_presence"_H: {
        const std::string user_id = transmission_manager_->GetUserId(hdl);
        if (user_id.empty() ||
            (j.contains("user_id") && j["user_id"] != user_id)) {
          LOG_WARN(
              "Ignore presence request for unauthenticated or mismatched user");
          break;
        }
        if (j.contains("subscribe") && !j["subscribe"].is_boolean()) {
          LOG_WARN("Ignore presence request with invalid subscribe field");
          break;
        }
        std::vector<std::string> device_ids;
        if (j.contains("devices") && j["devices"].is_array()) {
          for (auto& v : j["devices"]) {
            if (v.is_string()) device_ids.push_back(v.get<std::string>());
          }
        }
        if (presence_manager_) {
          // New clients explicitly replace subscriptions or only query.
          // Legacy queries may contain just one device, so merge them.
          if (!j.contains("subscribe") || j["subscribe"].get<bool>()) {
            presence_manager_->UpdateUserDevices(user_id, device_ids,
                                                 j.contains("subscribe"));
          }
          auto statuses = presence_manager_->BatchQuery(device_ids);
          json resp = {{"type", "presence"}, {"devices", json::array()}};
          for (const auto& p : statuses) {
            resp["devices"].push_back({{"id", p.first}, {"online", p.second}});
          }
          SendMsg(hdl, resp);
        }
        break;
      }
      default:
        LOG_WARN("Unknown signaling message type");
        break;
    }
  } catch (const json::exception&) {
    LOG_WARN("Invalid signaling fields on connection [{}]", state->id);
  }
}
