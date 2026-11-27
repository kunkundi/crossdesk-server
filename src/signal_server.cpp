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
    DeviceDBManager* db, SessionRecovery* recovery) {
  if (!transmission || !db) {
    return;
  }

  size_t restored_connections = 0;
  for (const auto& session : db->ListRemoteControlSessions(
           static_cast<size_t>(std::numeric_limits<int>::max()), 0, "")) {
    transmission->BindHostToTransmission(session.host_id,
                                         session.transmission_id);
    for (const auto& guest_id : session.guest_ids) {
      if (recovery && recovery->Manages(session.transmission_id, guest_id)) continue;
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
    : port_(port), certs_dir_(std::move(certs_dir)),
      announcement_interval_ms_(
          EnvMillis("CROSSDESK_ANNOUNCEMENT_INTERVAL_MS", 250, 10, 60000)) {
  LOG_INFO(
      "Starting CrossDesk Signaling Server on port {}, certs_dir: {}, "
      "db_path: {}",
      port_, certs_dir_, db_path);

  DevicePassword::CheckSupport();
  credential_worker_count_ = EnvMillis("CROSSDESK_AUTH_WORKERS", 2, 1, 32);
  credential_timeout_seconds_ =
      EnvMillis("CROSSDESK_AUTH_TIMEOUT_SECONDS", 600, 15, 900);
  const auto credential_capacity =
      EnvMillis("CROSSDESK_AUTH_QUEUE_CAPACITY", 4096, 1, 16384);
  const auto source_capacity =
      EnvMillis("CROSSDESK_AUTH_SOURCE_CAPACITY", 256, 1, 4096);
  const auto source_rate =
      EnvMillis("CROSSDESK_AUTH_SOURCE_PER_MINUTE", 60, 1, 6000);
  credential_queue_ = std::make_unique<CredentialQueue>(
      credential_worker_count_, credential_capacity, source_capacity);
  LOG_INFO("Credential capacity: workers={} outstanding_limit={} "
           "source_outstanding_limit={} source_per_minute={} timeout_seconds={} "
           "global_rate_limit=disabled", credential_worker_count_,
           credential_capacity, source_capacity, source_rate,
           credential_timeout_seconds_);

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
  server_.set_ping_handler([this](websocketpp::connection_hdl hdl, std::string payload) {
    if (!OnHeartbeat(hdl, {})) return false;
    heartbeat_frame_bytes_ += 2 + payload.size();  // Automatic unmasked Pong.
    return true;
  });
  server_.set_pong_timeout(0);  // The connection owns one correlated probe deadline.
  server_.set_pong_handler([this](websocketpp::connection_hdl hdl, std::string payload) {
    auto it = connections_.find(hdl);
    if (it == connections_.end()) return;
    auto& state = *it->second;
    if (state.pending_probe && payload == std::to_string(state.pending_probe)) {
      state.pending_probe = 0;
      OnHeartbeat(hdl, {});
    } else if (!state.heartbeat_seconds) {
      OnHeartbeat(hdl, {});
    }
  });

  transmission_manager_ = std::make_shared<TransmissionManager>(false);
  auto routes = std::make_shared<SessionRoutes>();
  device_db_manager_ = std::make_unique<DeviceDBManager>(
      db_path, DeviceDBManager::OpenMode::ReadWrite, routes);
  notification_service_ = std::make_unique<NotificationService>(db_path);
  notification_read_service_ = std::make_unique<NotificationService>(
      db_path, NotificationStore::OpenMode::ReadOnly);
  announcement_catalog_ = std::make_shared<const NotificationStore::Catalog>(
      notification_service_->PublicCatalog());
  notification_service_->SetPublicChangeCallback(
      [this] { QueueAnnouncementNotifications(); });
  session_recovery_ = std::make_unique<SessionRecovery>(db_path,
      device_db_manager_.get(), transmission_manager_,
      [this](websocketpp::connection_hdl hdl, json msg) { SendMsg(hdl, std::move(msg)); },
      std::max(120, credential_timeout_seconds_ + 15));
  // Reconstruct routing before installing the live-session callback. Replaying
  // persisted connections must not count as confirmed controller activity.
  RestorePersistedRemoteControlSessions(transmission_manager_,
                                        device_db_manager_.get(), session_recovery_.get());
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
  signal_negotiation_ = std::make_unique<SignalNegotiation>(
      transmission_manager_, device_db_manager_.get(), nullptr,
      CreateIceServerConfigIssuer(), nullptr, source_rate);
  signal_negotiation_->SetSessionRecovery(session_recovery_.get());
  signal_negotiation_->SetSendMsgCallback(std::bind(&SignalServer::SendMsg,
                                                    this, std::placeholders::_1,
                                                    std::placeholders::_2));
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
        SendMsg(transmission_manager_->GetWsHandle(
                    id, msg.value("transmission_id", "")),
                msg);
      });
  admin_read_db_ = std::make_unique<DeviceDBManager>(
      db_path, DeviceDBManager::OpenMode::ReadOnly, routes);
  admin_controller_->SetNotificationService(notification_service_.get());
  admin_controller_->SetAuthLimiter(signal_negotiation_->AuthLimiter());
  admin_controller_->SetDeviceDataCleanupCallback([this](const std::string& id) {
    signal_negotiation_->ForgetPasswordChangeResults(id);
  });
  admin_read_controller_ = std::make_unique<AdminController>(
      admin_auth_.get(), presence_manager_.get(), transmission_manager_,
      admin_read_db_.get(), nullptr, std::chrono::milliseconds(1000));
  admin_read_controller_->SetNotificationService(
      notification_read_service_.get());
  admin_read_controller_->SetAuthLimiter(signal_negotiation_->AuthLimiter());
  admin_controller_->SetSessionRecovery(session_recovery_.get());
  admin_read_controller_->SetSessionRecovery(session_recovery_.get());
  max_connections_ = EnvMillis("CROSSDESK_MAX_CONNECTIONS", 8192, 1, 65536);
  max_handshakes_ = EnvMillis("CROSSDESK_MAX_HANDSHAKES", 128, 1, 4096);
  max_source_handshakes_ =
      EnvMillis("CROSSDESK_HANDSHAKE_SOURCE_CONCURRENCY", 16, 1, 4096);
  source_handshakes_per_minute_ =
      EnvMillis("CROSSDESK_HANDSHAKE_SOURCE_PER_MINUTE", 120, 20, 60000);
  auto on_error = [this](std::exception_ptr error) { WorkerFailed(error); };
  application_worker_ =
      std::make_unique<BoundedExecutor>(
          std::max<size_t>(1024, credential_capacity),
          max_connections_ + credential_worker_count_ + 4, on_error);
  admin_worker_ = std::make_unique<BoundedExecutor>(32, 0, on_error);
  maintenance_worker_ = std::make_unique<BoundedExecutor>(2, 0, on_error);
  for (size_t i = 0; i < credential_worker_count_; ++i)
    credential_workers_.push_back(
        std::make_unique<BoundedExecutor>(1, 0, on_error));
  server_.set_max_message_size(64 * 1024);
  server_.set_max_http_body_size(16 * 1024);
  server_.set_open_handshake_timeout(10000);
  server_.set_close_handshake_timeout(3000);
}

SignalServer::~SignalServer() {
  stopping_ = true;
  // Join before any manager or endpoint is destroyed. Worker completions only
  // post back to the still-owned io_service; no worker accesses a connection.
  if (admin_worker_) admin_worker_->Stop();
  for (auto& worker : credential_workers_) worker->Stop();
  if (application_worker_) application_worker_->Stop();
  if (maintenance_worker_) maintenance_worker_->Stop();
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
  SSL_CTX_set_msg_callback_arg(ctx->native_handle(), this);
  SSL_CTX_set_msg_callback(ctx->native_handle(),
      [](int write, int, int type, const void* buffer, size_t length, SSL* ssl, void* arg) {
        if (!write || type != SSL3_RT_HEADER || length != 5 || SSL_is_init_finished(ssl)) return;
        const auto* header = static_cast<const unsigned char*>(buffer);
        static_cast<SignalServer*>(arg)->tls_handshake_bytes_ +=
            5u + (static_cast<unsigned>(header[3]) << 8) + header[4];
      });
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

void SignalServer::WakeAccept() {
  if (stopping_ || !admission_paused_ || accept_pending_ ||
      accept_wake_pending_)
    return;
  accept_wake_pending_ = true;
  server_.get_io_service().post([this] {
    accept_wake_pending_ = false;
    AcceptNext();
  });
}

bool SignalServer::AdmitSource(const std::string& address) {
  const auto now = Clock::now();
  auto it = handshake_sources_.find(address);
  if (it == handshake_sources_.end()) {
    if (handshake_sources_.size() >= 4096) {
      ++source_tracking_rejections_;
      return false;
    }
    it = handshake_sources_.try_emplace(address).first;
  }
  auto& source = it->second;
  source.last_seen = now;
  if (now - source.window >= std::chrono::minutes(1)) {
    const auto active = source.active;
    source = SourceActivity{};
    source.active = active;
  }
  // Shared NATs may reconnect many healthy clients together. Apply tighter
  // admission only after at least 20 failures and an over-80% failure ratio.
  const bool failing =
      source.failed >= 20 && source.failed > source.succeeded * 4;
  if (failing && (source.active >= max_source_handshakes_ ||
                  source.accepted >= source_handshakes_per_minute_)) {
    ++source.rejected;
    ++source_rejections_;
    return false;
  }
  ++source.accepted;
  ++source.active;
  return true;
}

void SignalServer::CompleteHandshake(
    const std::shared_ptr<ConnectionState>& state, bool success) {
  if (!state->handshake_pending) return;
  state->handshake_pending = false;
  auto it = handshake_sources_.find(state->source_address);
  if (it != handshake_sources_.end()) {
    auto& source = it->second;
    if (source.active) --source.active;
    source.last_seen = Clock::now();
    if (success)
      ++source.succeeded;
    else
      ++source.failed;
  }
  if (success) {
    if (handshake_times_ms_.size() == 4096) handshake_times_ms_.pop_front();
    handshake_times_ms_.push_back(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                              state->accepted)
            .count());
  }
  WakeAccept();
}

void SignalServer::ScheduleLoopMonitor() {
  if (stopping_) return;
  const auto due = Clock::now() + std::chrono::milliseconds(250);
  server_.set_timer(250, [this, due](websocketpp::lib::error_code ec) {
    if (ec || stopping_) return;
    const auto now = Clock::now();
    const auto delay = std::max<int64_t>(
        0, std::chrono::duration_cast<std::chrono::milliseconds>(now - due)
               .count());
    loop_delays_ms_.push_back(delay);
    if (delay >= 5000) {
      ++diagnostics_.loop_stalls;
      heartbeat_grace_until_ =
          now + std::chrono::milliseconds(check_interval_ms_);
    }
    if (delay >= 250 && now >= next_loop_warning_) {
      LOG_WARN(
          "Signal event loop delayed: delay_ms={} event_loop_stalls_total={}",
          delay, diagnostics_.loop_stalls);
      next_loop_warning_ = now + kDiagnosticWarningInterval;
    }
    ScheduleLoopMonitor();
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
  size_t expired_handles = 0, handshaking = 0, closing = 0;
  int64_t oldest_unopened_ms = 0;
  const auto now = Clock::now();
  for (const auto& entry : connections_) {
    const auto& state = *entry.second;
    opened += state.opened;
    authenticated += state.authenticated;
    pending_http += static_cast<bool>(state.pending_http);
    inactive += !state.alive.load();
    expired_handles += entry.first.expired();
    websocketpp::lib::error_code ec;
    auto con = server_.get_con_from_hdl(entry.first, ec);
    if (!ec && con) {
      handshaking +=
          state.handshake_pending &&
          con->get_state() == websocketpp::session::state::connecting;
      closing += con->get_state() == websocketpp::session::state::closing ||
                 con->get_state() == websocketpp::session::state::closed;
    }
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
      "unopened={} unopened_limit={} handshaking={} closing={} pending_http={} "
      "inactive={} "
      "expired_handles={} oldest_unopened_ms={} pending_cleanup={} "
      "pending_sends={} fd_open={} fd_estimated={} fd_sample_age_ms={} "
      "fd_soft_limit={} "
      "fd_reserve={} admission_paused={} pause_ms={}",
      connections_.size(), max_connections_, opened, authenticated,
      connections_.size() - opened, max_handshakes_, handshaking, closing,
      pending_http, inactive, expired_handles, oldest_unopened_ms,
      pending_cleanup_, pending_sends_.load(), fds.open,
      EstimateFileDescriptors(),
      std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                            fd_sampled_at_)
          .count(),
      fds.soft_limit, fd_reserve_, admission_paused_,
      admission_paused_ ? std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - admission_paused_since_)
                              .count()
                        : 0);
}

void SignalServer::LogConnectionDiagnostics() {
  LOG_INFO("Signal egress totals: tls_handshake_record_bytes={} heartbeat_ws_frame_bytes={} signaling_json_bytes={}",
           tls_handshake_bytes_.load(), heartbeat_frame_bytes_, signaling_payload_bytes_);
  const auto summary = [](auto values) {
    std::sort(values.begin(), values.end());
    const auto percentile = [&](size_t percent) -> int64_t {
      if (values.empty()) return 0;
      return values[(values.size() * percent + 99) / 100 - 1];
    };
    return fmt::format("samples={} p50_ms={} p95_ms={} p99_ms={} max_ms={}",
                       values.size(), percentile(50), percentile(95),
                       percentile(99), values.empty() ? 0 : values.back());
  };
  LOG_INFO(
      "Signal latency: loop=[{}] handshake=[{}] heartbeat_frames_total={} "
      "source_rejections_total={} source_tracking_rejections_total={}",
      summary(loop_delays_ms_), summary(handshake_times_ms_),
      received_heartbeats_, source_rejections_, source_tracking_rejections_);
  loop_delays_ms_.clear();
  handshake_times_ms_.clear();
  for (const auto& [reason, count] : handshake_failure_reasons_)
    LOG_INFO("Handshake failures: reason={} interval_count={}", reason, count);
  handshake_failure_reasons_.clear();
  std::vector<std::pair<std::string, SourceActivity>> sources;
  for (const auto& item : handshake_sources_)
    if (item.second.failed || item.second.rejected) sources.push_back(item);
  std::sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
    return a.second.failed + a.second.rejected >
           b.second.failed + b.second.rejected;
  });
  for (size_t i = 0; i < std::min<size_t>(5, sources.size()); ++i) {
    const auto& [address, source] = sources[i];
    LOG_INFO(
        "Handshake source: address={} accepted={} succeeded={} failed={} "
        "rejected={} active={} window_age_ms={}",
        address, source.accepted, source.succeeded, source.failed,
        source.rejected, source.active,
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                              source.window)
            .count());
  }
  const auto now = Clock::now();
  for (auto it = handshake_sources_.begin(); it != handshake_sources_.end();) {
    if (!it->second.active &&
        now - it->second.last_seen >= std::chrono::minutes(2))
      it = handshake_sources_.erase(it);
    else
      ++it;
  }
  LOG_INFO(
      "Connection diagnostics: {} accepted_total={} ws_opened_total={} "
      "preopen_failures_total={} "
      "accept_failures_total={} init_resource_failures_total={} "
      "connection_limit_checks_total={} unopened_limit_checks_total={} "
      "fd_limit_checks_total={} fd_sample_failed_checks_total={} "
      "event_loop_stalls_total={} login_success_total={} "
      "authentication_failed_total={} credential_busy_replies_total={} "
      "authentication_cooldown_replies_total={} authentication_timeouts_total={} "
      "reconnect_success_total={} reconnect_failed_total={}",
      DescribeConnectionResources(), diagnostics_.accepted, diagnostics_.opened,
      diagnostics_.preopen_failed,
      diagnostics_.accept_failed, diagnostics_.init_resource_failed,
      diagnostics_.connection_limit_checks, diagnostics_.unopened_limit_checks,
      diagnostics_.fd_limit_checks, diagnostics_.fd_sample_failed_checks,
      diagnostics_.loop_stalls, diagnostics_.login_success,
      diagnostics_.authentication_failed, diagnostics_.credential_busy,
      diagnostics_.authentication_throttled, diagnostics_.authentication_timeouts,
      diagnostics_.reconnect_success, diagnostics_.reconnect_failed);
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
  for (const auto& entry : connections_) {
    websocketpp::lib::error_code ec;
    auto con = server_.get_con_from_hdl(entry.first, ec);
    if (entry.second->handshake_pending && !ec && con &&
        con->get_state() == websocketpp::session::state::connecting)
      ++handshakes;
  }
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
    asio::error_code peer_error;
    auto peer = socket->remote_endpoint(peer_error);
    std::string source_address = "unknown";
    if (!peer_error) {
      auto address = peer.address();
      if (address.is_v6() && address.to_v6().is_v4_mapped())
        address =
            asio::ip::make_address_v4(asio::ip::v4_mapped, address.to_v6());
      source_address = address.to_string();
    }
    if (!AdmitSource(source_address)) {
      socket->close(peer_error);
      AcceptNext();
      return;
    }
    auto state = std::make_shared<ConnectionState>();
    state->source_address = source_address;
    server::connection_ptr con;
    try {
      con = server_.get_connection();
      if (!con) throw std::runtime_error("Connection initialization failed");
      con->get_raw_socket() = std::move(*socket);
      state->id = next_connection_id_++;
      state->handle = con->get_handle();
      connections_.emplace(con->get_handle(), state);
      con->set_termination_handler([this](server::connection_ptr finished) {
        FinishConnection(finished);
      });
      con->start();
    } catch (const std::system_error& e) {
      CompleteHandshake(state, false);
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
  CompleteHandshake(it->second, true);
  ++diagnostics_.opened;
  state.last_heartbeat = Clock::now();
  LOG_INFO("Websocket connection [{}] opened", state.id);
  return true;
}

void SignalServer::QueueSessionCleanup(
    websocketpp::connection_hdl hdl,
    const std::shared_ptr<ConnectionState>& state) {
  if (!state->alive.exchange(false) || !state->opened) return;
  if (state->probe_timer) {
    state->probe_timer->cancel();
    state->probe_timer.reset();
  }
  ++pending_cleanup_;
  if (!application_worker_->Submit(
          [this, hdl, state] {
            signal_negotiation_->ForgetSignalingHandle(hdl);
            credential_queue_->Cancel(state->id);
            state->pending_messages -= state->deferred_messages.size();
            deferred_message_count_ -= state->deferred_messages.size();
            state->deferred_messages.clear();
            // On process shutdown the durable session ledger is the checkpoint.
            // Releasing individual users here would erase it before restart.
            if (stopping_) {
              server_.get_io_service().post([this] {
                --pending_cleanup_;
                WakeAccept();
              });
              return;
            }
            // Login, release and presence changes are ordered on this one
            // worker. A release of an old handle cannot log out a newer handle
            // for that ID.
            const auto id = transmission_manager_->ReleaseUserSession(hdl);
            if (!id.empty()) {
              presence_manager_->OnLogout(id);
              signal_negotiation_->OnTemporaryControllerDisconnect(id);
            }
            server_.get_io_service().post([this] {
              --pending_cleanup_;
              WakeAccept();
            });
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
  CompleteHandshake(
      state, !con->get_ec() ||
                 con->get_ec() == websocketpp::error::http_connection_ended);
  if (state->opened && (con->get_ec() ||
      con->get_remote_close_code() == websocketpp::close::status::abnormal_close))
    RecordIdleFailure(*state);
  QueueSessionCleanup(hdl, state);
  if (state->opened) {
    LOG_INFO(
        "Websocket connection [{}|{}] closed code={} error=[{}]",
        state->id, state->device_id, con->get_local_close_code(),
        con->get_ec().message());
  } else if (con->get_ec() != websocketpp::error::http_connection_ended) {
    ++diagnostics_.preopen_failed;
    const auto message = con->get_ec().message();
    const std::string reason =
        message.find("TLS handshake timed out") != std::string::npos
            ? "tls_timeout"
        : message.find("TLS") != std::string::npos ? "tls_error"
        : message.find("handshake timed out") != std::string::npos
            ? "websocket_timeout"
            : "transport_error";
    ++handshake_failure_reasons_[reason];
    // Bound warning volume for failed/unauthenticated connection floods.
    if (Clock::now() >= next_preopen_warning_) {
      LOG_WARN(
          "Connection [{}] failed before websocket open error=[{}] "
          "transport=[{}] age_ms={} preopen_failures_total={}",
          state->id, con->get_ec().message(),
          con->get_transport_ec().message(),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              Clock::now() - state->accepted).count(),
          diagnostics_.preopen_failed);
      next_preopen_warning_ = Clock::now() + std::chrono::seconds(30);
    }
  }
  state->pending_http.reset();
  pending_announcements_.erase(state->id);
  connections_.erase(it);
  WakeAccept();
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

void SignalServer::RecordIdleFailure(ConnectionState& state) {
  if (state.heartbeat_seconds != 60 || state.idle_failure_recorded ||
      Clock::now() - state.last_heartbeat < std::chrono::seconds(15)) return;
  state.idle_failure_recorded = true;
  auto key = std::make_pair(state.device_id, state.source_address);
  if (idle_failures_.size() >= max_connections_ && !idle_failures_.count(key)) return;
  auto& failure = idle_failures_[key];
  if (failure.expires <= Clock::now()) failure.count = 0;
  failure.count = std::min(2, failure.count + 1);
  failure.expires = Clock::now() + std::chrono::hours(1);
}

void SignalServer::ArmProbeTimer(const std::shared_ptr<ConnectionState>& state) {
  // Incoming traffic only moves the deadline. Keep one timer per connection.
  const auto deadline = state->pending_probe
      ? state->probe_sent + std::chrono::seconds(15) : state->next_probe;
  const auto due = std::max(deadline, heartbeat_grace_until_);
  const auto delay = std::max<int64_t>(1, std::chrono::ceil<std::chrono::milliseconds>(
      due - Clock::now()).count());
  state->probe_timer = server_.set_timer(delay, [this, state, due](websocketpp::lib::error_code ec) {
    if (ec || stopping_ || !state->alive) return;
    state->probe_timer.reset();
    const auto activity = Clock::time_point(std::chrono::milliseconds(state->valid_signal_ms.load()));
    if (activity > state->last_heartbeat) {
      state->last_heartbeat = activity;
      state->next_probe = activity + std::chrono::seconds(state->heartbeat_seconds);
    }
    const auto now = Clock::now();
    if (now - due > std::chrono::seconds(5))
      heartbeat_grace_until_ = now + std::chrono::seconds(5);
    if (now >= heartbeat_grace_until_) {
      if (state->pending_probe && now - state->probe_sent >= std::chrono::seconds(15)) {
        RecordIdleFailure(*state);
        CloseConnection(state->handle, "Heartbeat timeout", websocketpp::close::status::going_away);
        return;
      }
      if (state->heartbeat_seconds && !state->pending_probe && now >= state->next_probe)
        SendProbe(*state);
    }
    if (state->alive && (state->heartbeat_seconds || state->pending_probe))
      ArmProbeTimer(state);
  });
}

void SignalServer::SendProbe(ConnectionState& state) {
  if (!state.alive || state.pending_probe) return;
  websocketpp::lib::error_code ec;
  const auto payload = std::to_string(++probe_sequence_);
  server_.ping(state.handle, payload, ec);
  if (ec) {
    CloseConnection(state.handle, "Probe send failed", websocketpp::close::status::going_away);
    return;
  }
  state.pending_probe = probe_sequence_;
  state.probe_sent = Clock::now();
  heartbeat_frame_bytes_ += 2 + payload.size();
}

bool SignalServer::OnHeartbeat(websocketpp::connection_hdl hdl, std::string) {
  auto it = connections_.find(hdl);
  if (it == connections_.end() || !it->second->alive) return false;
  auto& state = *it->second;
  state.last_heartbeat = Clock::now();
  state.next_probe = state.last_heartbeat + std::chrono::seconds(state.heartbeat_seconds);
  ++received_heartbeats_;
  return true;
}

void SignalServer::OnHttp(websocketpp::connection_hdl hdl) {
  auto con = server_.get_con_from_hdl(hdl);
  auto it = connections_.find(hdl);
  if (it == connections_.end()) return;
  CompleteHandshake(it->second, true);
  const auto resource = con->get_resource();
  const bool admin = AdminController::IsAdminRoute(resource);
  if (!admin && resource != "/stats" && resource != "/api/stats") {
    SetJsonResponse(con, websocketpp::http::status_code::not_found,
                    {{"error", "not_found"}});
    return;
  }
  AdminHttpRequest request{con->get_request().get_method(), resource,
                           con->get_request_body(),
                           con->get_request_header("Cookie"),
                           con->get_request_header("X-CrossDesk-CSRF"),
                           con->get_request_header("Content-Type"),
                           it->second->source_address};
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
        if (reader) {
          const auto deadline =
              started + std::chrono::milliseconds(http_timeout_ms_ / 2);
          admin_read_db_->SetReadDeadline(deadline);
          notification_read_service_->SetReadDeadline(deadline);
        }
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
        bool read_expired = false;
        if (reader) {
          const bool device_expired = admin_read_db_->ClearReadDeadline();
          const bool notification_expired =
              notification_read_service_->ClearReadDeadline();
          read_expired = device_expired || notification_expired;
        }
        if (read_expired) {
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
    session_recovery_->Expire();
    server_.get_io_service().post([this] { runtime_job_pending_ = false; });
  });
}

void SignalServer::ScheduleRecoveredSessionCleanup() {
  server_.set_timer(
      std::max(120, credential_timeout_seconds_ + 15) * 1000,
      [this](websocketpp::lib::error_code ec) {
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

void SignalServer::ScheduleRetentionCleanup() {
  if (stopping_) return;
  const auto now = Clock::now();
  if (!metadata_cleanup_pending_ && now >= next_metadata_cleanup_) {
    metadata_cleanup_pending_ = application_worker_->Submit([this] {
      int retry_seconds = retention_policy_.interval_seconds;
      try {
        const auto temporary = device_db_manager_->CleanupTemporaryControllers();
        for (const auto& id : temporary.device_ids)
          presence_manager_->ForgetOfflineDeviceData(id);
        if (temporary.devices || temporary.associations || temporary.credentials)
          LOG_INFO("Temporary controller cleanup: devices={} associations={} credentials={}",
                   temporary.devices, temporary.associations, temporary.credentials);
        const auto result = device_db_manager_->CleanupExpiredDevices(
            retention_policy_.offline_days);
        for (const auto& id : result.device_ids) {
          presence_manager_->ForgetOfflineDeviceData(id);
          signal_negotiation_->ForgetPasswordChangeResults(id);
        }
        const bool more_audit = device_db_manager_->CleanupAdminDataAudit(
            retention_policy_.log_days);
        if (result.devices || result.associations || result.credentials)
          LOG_INFO("Retention cleanup: devices={} associations={} credentials={} "
                   "offline_days={}",
                   result.devices, result.associations, result.credentials,
                   retention_policy_.offline_days);
        // The network maintenance timer already spaces batches five seconds
        // apart; adding another delay can miss the next tick.
        if (temporary.more || result.more || more_audit) retry_seconds = 0;
      } catch (const std::bad_alloc&) {
        throw;
      } catch (const std::exception& e) {
        LOG_ERROR("Device retention cleanup failed; retrying: {}", e.what());
        retry_seconds = 60;
      }
      server_.get_io_service().post([this, retry_seconds] {
        metadata_cleanup_pending_ = false;
        next_metadata_cleanup_ = Clock::now() + std::chrono::seconds(retry_seconds);
      });
    });
  }
  if (!log_cleanup_pending_ && now >= next_log_cleanup_) {
    log_cleanup_pending_ = maintenance_worker_->Submit([this] {
      int retry_seconds = retention_policy_.interval_seconds;
      try {
        const auto removed = CleanupExpiredLogs(retention_policy_.log_days);
        if (removed)
          LOG_INFO("Retention cleanup: log_files={} log_days={}", removed,
                   retention_policy_.log_days);
      } catch (const std::bad_alloc&) {
        throw;
      } catch (const std::exception& e) {
        LOG_ERROR("Log retention cleanup failed; retrying: {}", e.what());
        retry_seconds = 60;
      }
      server_.get_io_service().post([this, retry_seconds] {
        log_cleanup_pending_ = false;
        next_log_cleanup_ = Clock::now() + std::chrono::seconds(retry_seconds);
      });
    });
  }
}

void SignalServer::ScheduleMaintenance() {
  if (stopping_) return;
  const auto due = Clock::now() + std::chrono::milliseconds(check_interval_ms_);
  server_.set_timer(check_interval_ms_, [this,
                                         due](websocketpp::lib::error_code ec) {
    if (ec || stopping_) return;
    const auto now = Clock::now();
    if (now >= next_diagnostics_) {
      for (auto it = idle_failures_.begin(); it != idle_failures_.end();) {
        if (it->second.expires <= now) it = idle_failures_.erase(it);
        else ++it;
      }
      LogConnectionDiagnostics();
      next_diagnostics_ = now + kDiagnosticInterval;
    }
    // If the loop stalled, give queued ping frames one full check interval to
    // drain before considering expiry; never use a wall-clock timestamp.
    if (now >= heartbeat_grace_until_ &&
        now - due < std::chrono::milliseconds(check_interval_ms_)) {
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
            !state->heartbeat_seconds && age > heartbeat_timeout_ms_) {
          LOG_INFO(
              "Device [{}] heartbeat timed out connection={} age_ms={} "
              "source={}",
              state->device_id, state->id, age, state->source_address);
          CloseConnection(entry.first, "Heartbeat timeout",
                          websocketpp::close::status::going_away);
        } else if (state->opened && !state->authenticated &&
                   lifetime > authentication_timeout_ms_ &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch()).count() >=
                       state->credential_deadline_ms.load()) {
          ++diagnostics_.authentication_timeouts;
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
    ScheduleRetentionCleanup();
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
    application_worker_->Submit([this] {
      device_db_manager_->RecordRuntimeHeartbeat();
      LogCredentialDiagnostics();
      signal_negotiation_->LogDeliveryDiagnostics();
    }, true);
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
           "fd_reserve={} unopened_limit={} accept_retry_ms=500 listen_backlog={}",
           configured_max_connections, max_connections_, fd_usage_.open,
           fd_usage_.soft_limit, fd_reserve_, max_handshakes_, kListenBacklog);
  LOG_INFO("Retention policy: log_days={} offline_days={} interval_seconds={} "
           "device_credentials=expire_with_device", retention_policy_.log_days,
           retention_policy_.offline_days, retention_policy_.interval_seconds);
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
  // Absorb restart bursts in the kernel while keeping TLS handshakes bounded.
  // The OS may clamp this request to its own listen queue limit.
  acceptor_->listen(kListenBacklog);
  signals_ = std::make_unique<websocketpp::lib::asio::signal_set>(
      server_.get_io_service(), SIGINT, SIGTERM);
  signals_->async_wait([this](websocketpp::lib::error_code ec, int) {
    if (!ec) Stop();
  });
  AcceptNext();
  ScheduleMaintenance();
  ScheduleLoopMonitor();
  ScheduleCredentialPump();
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

void SignalServer::QueueAnnouncementNotifications() {
  if (stopping_) return;
  auto catalog = std::make_shared<const NotificationStore::Catalog>(
      notification_service_->PublicCatalog());
  server_.get_io_service().post([this, catalog = std::move(catalog)] {
    announcement_catalog_ = catalog;
    if (stopping_) return;
    for (const auto& entry : connections_) {
      const auto& state = entry.second;
      if (state->alive && state->authenticated &&
          state->device_id.rfind("C-", 0) != 0) {
        state->announcement_page = 0;
        pending_announcements_.try_emplace(state->id, state);
      }
    }
    LOG_INFO("Announcement catalog changed: pending_devices={} interval_ms={}",
             pending_announcements_.size(), announcement_interval_ms_);
    ScheduleAnnouncementNotification();
  });
}

void SignalServer::ScheduleAnnouncementNotification() {
  if (announcement_timer_pending_ || pending_announcements_.empty() || stopping_)
    return;
  announcement_timer_pending_ = true;
  server_.set_timer(announcement_interval_ms_, [this](websocketpp::lib::error_code ec) {
    announcement_timer_pending_ = false;
    if (ec || stopping_ || pending_announcements_.empty()) return;
    // Keep our place across publications so repeated edits cannot starve later devices.
    auto it = pending_announcements_.upper_bound(last_announcement_connection_);
    if (it == pending_announcements_.end()) it = pending_announcements_.begin();
    last_announcement_connection_ = it->first;
    const auto& state = it->second;
    auto con = server_.get_con_from_hdl(state->handle, ec);
    if (ec || !state->alive || !state->authenticated ||
        state->device_id.rfind("C-", 0) == 0 ||
        con->get_state() != websocketpp::session::state::open) {
      pending_announcements_.erase(it);
    } else if (con->get_buffered_amount() < 64 * 1024) {
      // Leave slow peers in the rotation and give signaling traffic priority.
      static const std::string message = R"({"type":"announcements_changed"})";
      const auto& pages = announcement_catalog_->pages;
      const auto& payload = state->announcement_v2 ? pages.at(state->announcement_page) : message;
      ec = con->send(payload, websocketpp::frame::opcode::text);
      if (!ec) signaling_payload_bytes_ += payload.size();
      if (!ec && (!state->announcement_v2 || ++state->announcement_page == pages.size()))
        pending_announcements_.erase(it);
    }
    ScheduleAnnouncementNotification();
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
  const auto failure_reason = message.value("reason", "");
  const auto failure_code = message.value("error_code", "");
  const bool reconnected = type == "login" && message.value("reconnected", false);
  const bool reconnect_rejected = type == "login" && message.value("reconnect_rejected", false);
  if (type == "login" && message.value("status", "") == "success") {
    login_id = message.value("user_id", "");
    login_id = login_id.substr(0, login_id.find('@'));
  }
  const bool heartbeat_reply = type == "pong";
  const bool v2 = !login_id.empty();
  json login = v2 ? std::move(message) : json();
  auto payload = v2 ? std::string() : message.dump();
  server_.get_io_service().post(
      [this, hdl, payload = std::move(payload), login = std::move(login), login_id,
       heartbeat_reply, failure_reason, failure_code, reconnected, reconnect_rejected]() mutable {
        --pending_sends_;
        auto it = connections_.find(hdl);
        if (it == connections_.end() || !it->second->alive) return;
        if (!login_id.empty()) {
          ++diagnostics_.login_success;
          if (reconnected) ++diagnostics_.reconnect_success;
          it->second->device_id = login_id;
          it->second->authenticated = true;
          if (login.contains("heartbeat_seconds")) {
            auto& state = *it->second;
            const auto failure = idle_failures_.find({login_id, state.source_address});
            state.heartbeat_seconds = login["heartbeat_seconds"].get<int>();
            if (failure != idle_failures_.end() && failure->second.expires > Clock::now() &&
                failure->second.count >= 2) state.heartbeat_seconds = 20;
            login["heartbeat_seconds"] = state.heartbeat_seconds;
            // Spread the first probes after a restart, even for simultaneous logins.
            state.next_probe = Clock::now() + std::chrono::milliseconds(
                1000 + (state.id * 2654435761ULL) % ((state.heartbeat_seconds - 1) * 1000));
            ArmProbeTimer(it->second);
          }
          if (login.contains("announcement_sync_version")) {
            auto& state = it->second;
            state->announcement_v2 = true;
            if (login["catalog_revision"] != announcement_catalog_->revision ||
                login.value("catalog_id", "") != announcement_catalog_->id) {
              state->announcement_page = 0;
              pending_announcements_.try_emplace(state->id, state);
              ScheduleAnnouncementNotification();
            }
            login["catalog_id"] = announcement_catalog_->id;
            login["catalog_revision"] = announcement_catalog_->revision;
          }
          payload = login.dump();
          // Clients may start heartbeats only after login. Give them a full
          // heartbeat interval even if credential work spent minutes queued.
          it->second->last_heartbeat = Clock::now();
        }
        if (reconnect_rejected) ++diagnostics_.reconnect_failed;
        if (failure_code == "AUTHENTICATION_FAILED" ||
            (failure_code.empty() && failure_reason == "Authentication failed"))
          ++diagnostics_.authentication_failed;
        else if (failure_code == "CREDENTIAL_SERVICE_BUSY" ||
                 (failure_code.empty() && failure_reason == "Credential service busy"))
          ++diagnostics_.credential_busy;
        else if (failure_code == "AUTHENTICATION_THROTTLED" ||
                 (failure_code.empty() && failure_reason == "Too many authentication attempts"))
          ++diagnostics_.authentication_throttled;
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
        else if (heartbeat_reply) heartbeat_frame_bytes_ += 2 + payload.size();
        else signaling_payload_bytes_ += payload.size();
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
    if (OnHeartbeat(hdl, "")) {
      json pong = {{"type", "pong"}};
      if (j.contains("nonce") && j["nonce"].is_string() && j["nonce"].get_ref<const std::string&>().size() <= 64)
        pong["nonce"] = j["nonce"];
      SendMsg(hdl, std::move(pong));
    }
    return;
  }
  auto state = it->second;
  if (state->pending_messages.fetch_add(1) >= 128) {
    --state->pending_messages;
    CloseConnection(hdl, "Message backlog limit",
                    websocketpp::close::status::policy_violation);
    return;
  }
  if (!application_worker_->Submit(
          [this, hdl, state, j = std::move(j)]() mutable {
            DispatchMessage(hdl, std::move(j), state);
          })) {
    --state->pending_messages;
    CloseConnection(hdl, "Server busy",
                    websocketpp::close::status::try_again_later);
  }
}

void SignalServer::DispatchMessage(
    websocketpp::connection_hdl hdl, json message,
    const std::shared_ptr<ConnectionState>& state) {
  if (state->alive && !stopping_) {
    if (state->credential_pending) {
      // A large login queue must not multiply the per-connection message
      // backlog into hundreds of thousands of retained JSON documents.
      if (deferred_message_count_ >= 1024) {
        --state->pending_messages;
        RequestBackpressureClose(hdl);
        return;
      }
      ++deferred_message_count_;
      state->deferred_messages.push_back(std::move(message));
      return;
    }
    ProcessMessage(hdl, message, state);
  }
  --state->pending_messages;
}

bool SignalServer::SubmitCredentialWork(
    websocketpp::connection_hdl hdl,
    const std::shared_ptr<ConnectionState>& state,
    SignalNegotiation::CredentialWork work, std::function<int()> admit,
    std::function<void()> expire) {
  if (stopping_ || !state->alive || state->credential_pending) return false;
  const auto deadline = Clock::now() +
                        std::chrono::seconds(credential_timeout_seconds_);
  const auto controller = transmission_manager_->GetVerifiedDeviceId(hdl);
  const auto source = json::array({controller.empty() ? "ip" : "controller",
      controller.empty() ? (state->source_address.empty() ? "unknown" : state->source_address) : controller}).dump();
  CredentialQueue::Job job{
      state->id, source, deadline,
      [this, state] { return !stopping_ && state->alive.load(); },
      std::move(admit),
      [this, hdl, state, work = std::move(work)](size_t worker) mutable {
        StartCredentialWork(hdl, state, std::move(work), worker);
      },
      [this, state, expire = std::move(expire)] {
        if (!stopping_ && state->alive) expire();
        FinishCredentialRequest(state);
      }};
  if (!credential_queue_->Enqueue(std::move(job))) return false;
  state->credential_pending = true;
  // Keep deadlines distinct even for successive requests in one millisecond.
  const auto deadline_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      deadline.time_since_epoch()).count();
  state->credential_deadline_ms =
      std::max(deadline_ms, state->credential_deadline_ms.load() + 1);
  credential_queue_->Pump();
  return true;
}

void SignalServer::FinishCredentialRequest(
    const std::shared_ptr<ConnectionState>& state) {
  state->credential_pending = false;
  const auto deadline = state->credential_deadline_ms.load();
  // Successful login replies are posted before this reset. Do not reset the
  // deadline of a subsequent request that was queued before this callback ran.
  server_.get_io_service().post([state, deadline]() mutable {
    auto expected = deadline;
    state->credential_deadline_ms.compare_exchange_strong(expected, 0);
  });
  while (!state->credential_pending && !state->deferred_messages.empty()) {
    auto message = std::move(state->deferred_messages.front());
    state->deferred_messages.pop_front();
    --deferred_message_count_;
    DispatchMessage(state->handle, std::move(message), state);
  }
}

void SignalServer::StartCredentialWork(
    websocketpp::connection_hdl hdl,
    const std::shared_ptr<ConnectionState>& state,
    SignalNegotiation::CredentialWork work, size_t worker) {
  auto job = [this, hdl, state, work = std::move(work), worker] {
    std::function<void()> finish;
    if (!stopping_ && state->alive) {
      const auto started = Clock::now();
      finish = work();
      credential_work_us_.fetch_add(
          std::chrono::duration_cast<std::chrono::microseconds>(
              Clock::now() - started).count());
      ++credential_work_count_;
    }
    // One reserved completion per worker, independent of the waiting capacity.
    if (!application_worker_->Submit(
            [this, state, finish = std::move(finish), worker] {
              credential_queue_->Complete(worker);
              try {
                if (!stopping_ && state->alive && finish) finish();
              } catch (const json::exception&) {
                LOG_WARN("Invalid signaling fields on connection [{}]", state->id);
              }
              FinishCredentialRequest(state);
              if (!stopping_) credential_queue_->Pump();
            }, true))
      throw std::runtime_error("Credential completion reserve exhausted");
  };
  // The scheduler assigns only idle workers. Failure indicates shutdown or an
  // invariant violation, not normal overload; queued jobs consume no tokens.
  if (!credential_workers_.at(worker)->Submit(std::move(job)))
    throw std::runtime_error("Reserved credential worker unavailable");
}

void SignalServer::LogCredentialDiagnostics() {
  const auto& stats = credential_queue_->GetStats();
  const auto count = credential_work_count_.load();
  LOG_INFO("Credential diagnostics: active={} waiting={} outstanding_limit={} deferred_messages={} "
           "accepted_total={} started_total={} completed_total={} canceled_total={} "
           "queue_full_total={} source_full_total={} expired_total={} "
           "source_rate_waits_total={} max_queue_wait_ms={} max_execution_ms={} "
           "work_count={} average_work_ms={}",
           credential_queue_->Active(), credential_queue_->Waiting(),
           credential_queue_->Capacity(), deferred_message_count_, stats.accepted, stats.started,
           stats.completed, stats.canceled, stats.full, stats.source_full,
           stats.expired, stats.source_waits, stats.max_wait_ms,
           stats.max_execution_ms, count,
           count ? credential_work_us_.load() / count / 1000 : 0);
}

void SignalServer::ScheduleCredentialPump() {
  if (stopping_) return;
  server_.set_timer(250, [this](websocketpp::lib::error_code ec) {
    if (ec || stopping_) return;
    if (!credential_pump_pending_) {
      credential_pump_pending_ = true;
      if (!application_worker_->Submit([this] {
            if (!stopping_) {
              credential_queue_->Pump();
              signal_negotiation_->TickJoinDeliveries([this](websocketpp::connection_hdl host) {
                server_.get_io_service().post([this, host] {
                  auto it = connections_.find(host);
                  if (it == connections_.end() || it->second->pending_probe || !it->second->alive) return;
                  const auto& state = it->second;
                  SendProbe(*state);
                  if (state->alive) {
                    if (state->probe_timer) state->probe_timer->cancel();
                    ArmProbeTimer(state);
                  }
                });
              });
              const auto now = Clock::now();
              if (now >= next_credential_diagnostics_) {
                LogCredentialDiagnostics();
                signal_negotiation_->LogDeliveryDiagnostics();
                next_credential_diagnostics_ = now + kDiagnosticInterval;
              }
            }
            server_.get_io_service().post([this] { credential_pump_pending_ = false; });
          })) credential_pump_pending_ = false;
    }
    ScheduleCredentialPump();
  });
}

void SignalServer::ProcessMessage(
    websocketpp::connection_hdl hdl, const json& j,
    const std::shared_ptr<ConnectionState>& state) {
  try {
    const auto type = j["type"].get<std::string>();
    const auto mark_activity = [&state] {
      state->valid_signal_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          Clock::now().time_since_epoch()).count();
    };
    if (NotificationService::IsClientMessage(type)) {
      const auto device = transmission_manager_->GetUserId(hdl);
      if (!device.empty() && device.rfind("C-", 0) != 0) {
        auto response = notification_service_->HandleClientMessage(j);
        if (response) {
          mark_activity();
          SendMsg(hdl, std::move(*response));
        }
      }
      return;
    }
    auto dispatch = [this, hdl, state](SignalNegotiation::CredentialWork work,
                                        std::function<int()> admit,
                                        std::function<void()> expire) {
      return SubmitCredentialWork(hdl, state, std::move(work),
                                  std::move(admit), std::move(expire));
    };
    bool valid = false;
    switch (HASH_STRING_PIECE(type.c_str())) {
      case "login"_H:
        signal_negotiation_->login_user(
            hdl, j, state->source_address, dispatch, [this, hdl, j] {
              session_recovery_->Login(hdl, j);
              const auto id = transmission_manager_->GetUserId(hdl);
              if (presence_manager_ && !id.empty())
                presence_manager_->OnLogin(id, id, hdl);
            });
        break;
      case "session_route"_H:
        valid = signal_negotiation_->session_route(hdl, j);
        break;
      case "session_report"_H:
        valid = session_recovery_->Report(hdl, j);
        break;
      case "revoke_reconnect_credential"_H:
        valid = signal_negotiation_->revoke_reconnect_credential(hdl);
        break;
      case "user_leave_transmission"_H:
        valid = signal_negotiation_->leave_transmission(hdl, j);
        break;
      case "disconnect_peer"_H:
        valid = signal_negotiation_->disconnect_peer(hdl, j);
        break;
      case "query_user_id_list"_H:
        valid = signal_negotiation_->query_user_id_list(hdl, j, state->source_address,
                                                dispatch);
        break;
      case "join_transmission"_H:
        valid = signal_negotiation_->join_transmission(hdl, j, state->source_address,
                                               dispatch);
        break;
      case "join_first_frame"_H:
        valid = signal_negotiation_->FirstFrame(hdl, j);
        break;
      case "join_receipt"_H:
        valid = signal_negotiation_->JoinReceipt(hdl, j);
        break;
      case "offer"_H:
        valid = signal_negotiation_->offer(hdl, j);
        break;
      case "answer"_H:
        valid = signal_negotiation_->answer(hdl, j);
        break;
      case "new_candidate"_H:
        valid = signal_negotiation_->new_candidate(hdl, j);
        break;
      case "new_candidate_mid"_H:
        valid = signal_negotiation_->new_candidate_mid(hdl, j);
        break;
      case "change_password"_H:
        valid = signal_negotiation_->change_password(hdl, j, state->source_address,
                                             dispatch);
        break;
      case "turn_credentials"_H:
        valid = signal_negotiation_->turn_credentials(hdl, j);
        break;
      case "client_info"_H:
        valid = signal_negotiation_->client_info(hdl, j);
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
          valid = true;
          SendMsg(hdl, resp);
        }
        break;
      }
      default:
        LOG_WARN("Unknown signaling message type");
        break;
    }
    if (valid) mark_activity();
  } catch (const json::exception&) {
    LOG_WARN("Invalid signaling fields on connection [{}]", state->id);
  }
}
