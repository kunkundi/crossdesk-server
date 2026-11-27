#include "signal_negotiation.h"
#include "controller_identity.h"
#include "session_recovery.h"

#include <openssl/sha.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

#include "log.h"

namespace {

void SetFailure(json& message, const std::string& code,
                const std::string& reason) {
  message["error_code"] = code;
  message["reason"] = reason;
  // Released MiniRTC clients only recognize these legacy join reasons.
  // Keep unknown IDs indistinguishable from wrong passwords. New clients use
  // error_code so cooldowns and busy responses are not mistaken for offline.
  if (message.value("type", "") == "user_join_transmission") {
    message["reason"] =
        code == "AUTHENTICATION_FAILED" || code == "PASSWORD_REQUIRED"
            ? "Incorrect password"
            : "Remote unavailable";
  }
}

bool GetStringField(const json& j, const char* key, std::string& value) {
  if (!j.contains(key) || !j[key].is_string()) {
    return false;
  }
  value = j[key].get<std::string>();
  return true;
}

bool ValidDeviceCredentials(const std::string& device_id,
                            const std::string& password) {
  // SQLite binds IDs as C strings. Reject embedded NULs to prevent multiple
  // limiter keys from authenticating against the same database device.
  return device_id.size() <= 128 && password.size() <= 256 &&
         device_id.find('\0') == std::string::npos &&
         password.find('\0') == std::string::npos;
}

bool CopyOptionalIceUfrag(const json& source, json& destination) {
  const auto field = source.find("ufrag");
  if (field == source.end()) {
    return true;
  }
  if (!field->is_string()) {
    return false;
  }

  // Match the client's bounds without rewriting the ICE generation tag.
  // Missing/empty tags remain compatible with older clients; the receiver
  // checks a non-empty tag against its remote SDP.
  const auto& ufrag = field->get_ref<const std::string&>();
  if (ufrag.size() > 256 || ufrag.find('\0') != std::string::npos ||
      ufrag.find_first_of(" \t\r\n") != std::string::npos) {
    return false;
  }
  destination["ufrag"] = ufrag;
  return true;
}

bool ShouldTrackClientInfo(const std::string& user_id) {
  return !user_id.empty() && !IsTemporaryControllerId(user_id) &&
         user_id.rfind("C-", 0) != 0;
}

std::string PasswordFingerprint(const std::string& password) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(password.data()),
         password.size(), digest);
  std::ostringstream stream;
  stream << std::hex << std::setfill('0');
  for (unsigned char byte : digest) {
    stream << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return stream.str();
}

}  // namespace

SignalNegotiation::SignalNegotiation(
    std::shared_ptr<TransmissionManager> transmission_manager,
    DeviceDBManager* device_db,
    std::shared_ptr<TurnCredentialIssuer> turn_credential_issuer,
    std::shared_ptr<IceServerConfigIssuer> ice_config_issuer,
    std::shared_ptr<DeviceAuthLimiter> device_auth_limiter,
    size_t source_credential_limit)
    : transmission_manager_(transmission_manager),
      device_db_manager_(device_db),
      turn_credential_issuer_(std::move(turn_credential_issuer)),
      ice_config_issuer_(std::move(ice_config_issuer)),
      device_auth_limiter_(device_auth_limiter
                               ? std::move(device_auth_limiter)
                               : std::make_shared<DeviceAuthLimiter>(
                                     DeviceAuthLimiter::Clock::now,
                                     DeviceAuthLimiter::kSourceFailures,
                                     DeviceAuthLimiter::kTargetFailures,
                                     DeviceAuthLimiter::Policy::Exponential)),
      credential_work_limiter_(DeviceAuthLimiter::Clock::now,
                               source_credential_limit) {}

SignalNegotiation::~SignalNegotiation() {}

bool SignalNegotiation::IsAuthorizedPeerSignal(
    websocketpp::connection_hdl hdl, const std::string& user_id,
    const std::string& remote_user_id, const std::string& transmission_id) const {
  if (hdl.expired() || user_id.empty() || remote_user_id.empty() ||
      transmission_id.empty() || user_id == remote_user_id ||
      transmission_manager_->GetUserId(hdl) != user_id) {
    return false;
  }

  const auto host_id =
      transmission_manager_->GetHostIdOfTransmission(transmission_id);
  if (host_id.empty() || (user_id != host_id && remote_user_id != host_id)) {
    return false;
  }

  return transmission_manager_->IsConnectionInTransmission(hdl,
                                                           transmission_id) &&
         !transmission_manager_->GetWsHandle(remote_user_id, transmission_id)
              .expired();
}

bool SignalNegotiation::AddTurnCredentials(json& message,
                                           const std::string& user_id) const {
  if (user_id.empty()) return false;
  if (ice_config_issuer_) {
    auto config = ice_config_issuer_->Issue(user_id);
    if (!config.contains("turn")) return false;
    message["turn"] = std::move(config["turn"]);
    return true;
  }
  if (!turn_credential_issuer_) return false;

  const TurnCredentials credentials = turn_credential_issuer_->Issue(user_id);
  message["turn"] = {{"host", credentials.host},
                     {"port", credentials.port},
                     {"username", credentials.username},
                     {"password", credentials.password},
                     {"expires_at", credentials.expires_at}};
  return true;
}

void SignalNegotiation::AddLoginIceConfig(json& message, const json& request,
                                          const std::string& user_id) const {
  if (ice_config_issuer_) message["ice_config_version"] = 1;
  const auto version = request.find("ice_config_version");
  if (!ice_config_issuer_ || version == request.end() ||
      !version->is_number_integer() || *version != 1)
    AddTurnCredentials(message, user_id);
}

void SignalNegotiation::AddConnectionIceConfig(
    json& message, const std::string& user_id) const {
  if (ice_config_issuer_) {
    message.update(ice_config_issuer_->Issue(user_id));
  } else {
    AddTurnCredentials(message, user_id);
  }
}

void SignalNegotiation::RunCredentialWork(websocketpp::connection_hdl hdl,
                                          const std::string& source_address,
                                          json failure, CredentialWork work,
                                          const CredentialDispatch& dispatch) {
  const auto controller = transmission_manager_->GetVerifiedDeviceId(hdl);
  const auto source_key = json::array({controller.empty() ? "ip" : "controller",
      controller.empty() ? (source_address.empty() ? "unknown" : source_address) : controller}).dump();
  auto admit = [this, source_key] {
    return credential_work_limiter_.Admit(source_key);
  };
  if (dispatch) {
    auto expired = [this, hdl, failure]() mutable {
      SetFailure(failure, "CREDENTIAL_SERVICE_BUSY", "Credential service busy");
      failure["retry_after"] = 1;
      send_msg_(hdl, failure);
    };
    if (!dispatch(std::move(work), std::move(admit), expired)) expired();
    return;
  }
  const int delay = admit();
  if (delay > 0) {
    SetFailure(failure, "CREDENTIAL_RATE_LIMITED", "Too many credential requests");
    failure["retry_after"] = delay;
    send_msg_(hdl, failure);
  } else {
    work()();
  }
}

void SignalNegotiation::AuthenticateDevice(
    websocketpp::connection_hdl hdl, const std::string& source_address,
    const std::string& device_id, const std::string& password, json failure,
    DeviceAuthLimiter::Operation operation,
    const CredentialDispatch& dispatch, std::function<void()> on_verified,
    const std::string& bind_login_key) {
  SetFailure(failure, "AUTHENTICATION_FAILED", "Authentication failed");
  if (!ValidDeviceCredentials(device_id, password) ||
      IsTemporaryControllerId(device_id)) {
    send_msg_(hdl, failure);
    return;
  }
  // A claimed target ID (including a legacy login ID) never owns a failure
  // bucket. Only a key-verified connection can use a device's source budget;
  // all other clients share their transport IP, irrespective of C-/web/cli ID.
  const auto controller = transmission_manager_->GetVerifiedDeviceId(hdl);
  const auto principal = json::array({controller.empty() ? "ip" : "controller",
      controller.empty() ? (source_address.empty() ? "unknown" : source_address) : controller});
  const auto source_key = principal.dump();
  auto pair = principal;
  pair.push_back(device_id);
  const auto pair_key = pair.dump();
  const auto manual = device_db_manager_->PasswordRiskDelay(source_address, controller);
  const int retry_after = std::max(manual.value_or(1),
      device_auth_limiter_->CooldownRemaining(source_key, pair_key));
  if (!manual || retry_after > 0) {
    SetFailure(failure, manual ? "AUTHENTICATION_THROTTLED" : "CREDENTIAL_SERVICE_BUSY",
               manual ? "Too many authentication attempts" : "Credential service busy");
    failure["retry_after"] = retry_after;
    send_msg_(hdl, failure);
    return;
  }
  const auto identity = transmission_manager_->GetUserId(hdl);
  const auto record = device_db_manager_->ReadPassword(device_id);
  RunCredentialWork(
      hdl, source_address, failure,
      [this, hdl, source_address, device_id, password, record, identity,
       controller, source_key, pair_key, failure, bind_login_key, operation,
       on_verified = std::move(on_verified)]() -> std::function<void()> {
        std::optional<DevicePasswordRecord> verified;
        const auto manual = device_db_manager_->PasswordRiskDelay(source_address, controller);
        DeviceAuthLimiter::Result result{false, manual.value_or(1)};
        if (manual && *manual == 0)
          result = device_auth_limiter_->Verify(source_key, pair_key, [&] {
              verified = DevicePassword::Verify(record, password);
              return verified.has_value();
            }, operation);
        return [this, hdl, source_address, device_id, controller, record, identity, verified, result,
                unavailable = !manual,
                failure = failure, on_verified, bind_login_key]() mutable {
          if (hdl.expired() ||
              transmission_manager_->GetUserId(hdl) != identity)
            return;
          // The administrator may have added a rule while hashing was running.
          if (result.authenticated) {
            const auto current = device_db_manager_->PasswordRiskDelay(source_address, controller);
            if (!current || *current > 0) {
              result = {false, current.value_or(1)};
              unavailable = !current;
            }
          }
          if (result.authenticated && record && verified &&
              device_db_manager_->CommitPassword(device_id, *record,
                                                 *verified) &&
              (bind_login_key.empty() || device_db_manager_->BindLoginPublicKey(
                  device_id, *verified, bind_login_key))) {
            on_verified();
          } else {
            if (result.retry_after > 0) {
              SetFailure(failure, unavailable ? "CREDENTIAL_SERVICE_BUSY" : "AUTHENTICATION_THROTTLED",
                         unavailable ? "Credential service busy" : "Too many authentication attempts");
              failure["retry_after"] = result.retry_after;
            }
            send_msg_(hdl, failure);
          }
        };
      },
      dispatch);
}

bool SignalNegotiation::login_user(websocketpp::connection_hdl hdl,
                                   const json& j,
                                   const std::string& source_address,
                                   const CredentialDispatch& dispatch,
                                   std::function<void()> on_login) {
  // Login and connection cleanup run on the same application worker. Check
  // its binding before registration/authentication, not the network thread's
  // asynchronously updated authenticated flag. Pin the handle through binding.
  const auto connection = hdl.lock();
  if (!connection) return false;
  if (!transmission_manager_->GetUserId(hdl).empty()) {
    send_msg_(hdl, {{"type", "login"},
                    {"user_id", ""},
                    {"status", "fail"},
                    {"reason", "Already authenticated"}});
    return false;
  }

  if (j.contains("device_key"))
    return LoginWithKey(hdl, j, source_address, dispatch, std::move(on_login));

  std::string host_id_with_pwd;
  if (!GetStringField(j, "user_id", host_id_with_pwd)) {
    LOG_ERROR("login_user missing or invalid field: user_id");
    return false;
  }

  std::string host_id;
  std::string password;

  if (host_id_with_pwd.find("@") != std::string::npos) {
    host_id = host_id_with_pwd.substr(0, host_id_with_pwd.find("@"));
    password = host_id_with_pwd.substr(host_id_with_pwd.find("@") + 1);
  } else {
    host_id = host_id_with_pwd;
    password = "";
  }

  json failure = {{"type", "login"},
                  {"user_id", ""},
                  {"status", "fail"},
                  {"reason", "Authentication failed"}};
  if (!ValidDeviceCredentials(host_id, password)) {
    send_msg_(hdl, failure);
    return false;
  }

  const auto login_key = device_db_manager_->ReadLoginPublicKey(
      host_id.rfind("C-", 0) == 0 ? host_id.substr(2) : host_id);
  if (login_key && !login_key->empty()) {
    SetFailure(failure, "DEVICE_KEY_REQUIRED", "Device key authentication required");
    send_msg_(hdl, failure);
    return false;
  }

  auto completed = std::make_shared<bool>(false);
  auto finish = [this, hdl, j, host_id, completed, on_login](
                    const DeviceCredential& credential, bool registration) {
    *completed = CompleteLogin(hdl, j, host_id, credential, registration);
    if (*completed && on_login) on_login();
  };
  if (j.contains("reconnect_token")) {
    const auto version = j.find("reconnect_version");
    std::optional<int64_t> expires;
    if (version != j.end() && version->is_number_integer() && *version == 1 &&
        j["reconnect_token"].is_string() &&
        host_id_with_pwd.find('@') == std::string::npos) {
      expires = device_db_manager_->VerifyReconnectCredential(
          host_id, j["reconnect_token"].get<std::string>());
    }
    if (!expires) {
      failure["reconnect_rejected"] = true;
      send_msg_(hdl, failure);
      return false;
    }
    // High-entropy credentials use a bounded digest lookup, not the password
    // work queue or its per-IP quota. Never silently fall back to Argon2 here.
    *completed = CompleteLogin(hdl, j, host_id, {host_id, "", false}, false, expires);
    if (*completed && on_login) on_login();
  } else if (host_id.rfind("C-", 0) == 0) {
    finish({host_id, "", false}, false);
  } else if (host_id.empty() || IsTemporaryControllerRegistration(host_id)) {
    RegisterDevice(hdl, source_address, host_id, failure, dispatch, finish);
  } else {
    AuthenticateDevice(
        hdl, source_address, host_id, password, failure,
        DeviceAuthLimiter::Operation::DeviceLogin, dispatch,
        [finish, host_id] { finish({host_id, "", false}, false); });
  }
  return *completed;
}

void SignalNegotiation::RegisterDevice(
    websocketpp::connection_hdl hdl, const std::string& source,
    const std::string& kind, json failure, const CredentialDispatch& dispatch,
    std::function<void(const DeviceCredential&, bool)> finish,
    const std::string& login_public_key) {
  RunCredentialWork(hdl, source, failure,
      [this, hdl, kind, login_public_key, failure, finish]() -> std::function<void()> {
        const auto password = DevicePassword::Generate();
        const auto record = password.empty() ? std::nullopt : DevicePassword::Hash(password);
        return [this, hdl, kind, login_public_key, failure, finish, password, record] {
          if (hdl.expired() || !transmission_manager_->GetUserId(hdl).empty()) return;
          if (!record) {
            send_msg_(hdl, failure);
            return;
          }
          finish(device_db_manager_->RegisterDevice(kind, password, *record, login_public_key),
                 true);
        };
      }, dispatch);
}

void SignalNegotiation::AddLoginCapabilities(websocketpp::connection_hdl hdl,
    const json& request, json& reply) {
  if (request.contains("heartbeat_version") && request["heartbeat_version"] == 1) {
    const auto profile = request.find("heartbeat_profile");
    reply["heartbeat_seconds"] = profile != request.end() && *profile == "desktop" ? 60 : 20;
  }
  if (request.contains("join_delivery_version") && request["join_delivery_version"] == 1) {
    receipt_hosts_.insert(hdl);
    reply["join_delivery_version"] = 1;
  }
}

void SignalNegotiation::ForgetSignalingHandle(websocketpp::connection_hdl hdl) {
  receipt_hosts_.erase(hdl);
}

bool SignalNegotiation::CompleteLogin(websocketpp::connection_hdl hdl,
                                      const json& j, const std::string& host_id,
                                      const DeviceCredential& dev_cred,
                                      bool registration,
                                      std::optional<int64_t> reconnect_expires,
                                      const std::string& verified_login_key) {
  const auto connection = hdl.lock();
  if (!connection || !transmission_manager_->GetUserId(hdl).empty())
    return false;
  json failure = {{"type", "login"},
                  {"user_id", ""},
                  {"status", "fail"},
                  {"reason", "Authentication failed"}};
  std::string return_host_id;
  if (host_id.rfind("C-", 0) != 0) {
    std::string ret_host_id = dev_cred.device_id;
    std::string ret_password = dev_cred.password;
    bool update_password = dev_cred.update;

    if (ret_host_id.empty()) {
      send_msg_(hdl, failure);
      return false;
    }

    // Recheck at the binding boundary: queued legacy logins cannot cross a key
    // migration, and a deleted device or changed key cannot complete login.
    const auto login_key = device_db_manager_->ReadLoginPublicKey(ret_host_id);
    if (!login_key || *login_key != verified_login_key) {
      if (login_key && !login_key->empty())
        SetFailure(failure, "DEVICE_KEY_REQUIRED", "Device key authentication required");
      send_msg_(hdl, failure);
      return false;
    }

    if (!transmission_manager_->BindUserToWsHandle(ret_host_id, hdl,
          verified_login_key.empty() ? "" : ret_host_id)) {
      // Only remove credentials created by this request; an existing device
      // must survive a failed connection binding.
      if (registration && !device_db_manager_->RemoveDevice(ret_host_id)) {
        LOG_ERROR("Failed to remove unbound registration [{}]", ret_host_id);
      }
      send_msg_(hdl, failure);
      return false;
    }
    if (!IsTemporaryControllerId(ret_host_id))
      transmission_manager_->BindHostToTransmission(ret_host_id, ret_host_id);

    bool update_success = ret_host_id != "" && update_password;
    bool login_success =
        (ret_host_id != "" && ret_password == "") && !update_password;
    bool register_success =
        (ret_host_id != "" && ret_password != "") && (ret_host_id != host_id);

    if (register_success) {
      LOG_INFO("New client, assign id [{}] to it", ret_host_id);
      return_host_id = ret_host_id + "@" + ret_password;
    } else if (login_success) {
      LOG_INFO("Receive login request with id [{}]", ret_host_id);
      return_host_id = ret_host_id;
    } else if (update_success) {
      LOG_INFO("Client [{}] update password", ret_host_id);
      return_host_id = ret_host_id;
    }

    if (ShouldTrackClientInfo(ret_host_id) &&
        !device_db_manager_->UpdateDeviceClientInfo(ret_host_id, "", "")) {
      LOG_WARN("Failed to clear client information for [{}]", ret_host_id);
    }
    json message = {{"type", "login"},
                    {"user_id", return_host_id},
                    {"status", "success"}};
    if (!login_key->empty()) message["device_key_version"] = 1;
    const auto version = j.find("reconnect_version");
    if (reconnect_expires) {
      message["reconnected"] = true;
      message["reconnect_version"] = 1;
      message["reconnect_expires_at"] = *reconnect_expires;
    } else if (login_key->empty() && ShouldTrackClientInfo(ret_host_id) && version != j.end() &&
               version->is_number_integer() && *version == 1) {
      const auto credential = device_db_manager_->IssueReconnectCredential(ret_host_id);
      if (credential) {
        message["reconnect_version"] = 1;
        message["reconnect_token"] = credential->token;
        message["reconnect_expires_at"] = credential->expires_at;
      } else {
        LOG_WARN("Failed to issue reconnect credential for [{}]", ret_host_id);
      }
    }
    if (j.contains("announcement_sync_version") && j["announcement_sync_version"] == 2) {
      message["announcement_sync_version"] = 2;
      // The transport replaces the client's revision with the current public one.
      const auto id = j.find("catalog_id");
      if (id != j.end() && id->is_string() && id->get_ref<const std::string&>().size() == 32)
        message["catalog_id"] = *id;
      const auto rev = j.find("catalog_revision");
      message["catalog_revision"] = rev != j.end() && rev->is_number_integer() &&
          *rev >= 0 && *rev <= INT64_MAX ? *rev : json(-1);
    }
    AddLoginCapabilities(hdl, j, message);
    AddLoginIceConfig(message, j, ret_host_id);
    send_msg_(hdl, message);
  } else {
    const auto device_id = host_id.substr(2);
    const auto key = device_db_manager_->ReadLoginPublicKey(device_id);
    if ((!verified_login_key.empty() && (!key || *key != verified_login_key)) ||
        (verified_login_key.empty() && key && !key->empty())) {
      SetFailure(failure, "DEVICE_KEY_REQUIRED", "Device key authentication required");
      send_msg_(hdl, failure);
      return false;
    }
    if (!transmission_manager_->BindUserToWsHandle(host_id, hdl,
          verified_login_key.empty() ? "" : device_id)) {
      send_msg_(hdl, failure);
      return false;
    }
    transmission_manager_->BindHostToTransmission(host_id, host_id);
    LOG_INFO("Receive login request with id [{}]", host_id);

    json message = {
        {"type", "login"}, {"user_id", host_id}, {"status", "success"}};
    if (!verified_login_key.empty()) message["device_key_version"] = 1;
    AddLoginCapabilities(hdl, j, message);
    AddLoginIceConfig(message, j, host_id);
    send_msg_(hdl, message);
  }

  return true;
}

bool SignalNegotiation::revoke_reconnect_credential(websocketpp::connection_hdl hdl) {
  const auto id = transmission_manager_->GetUserId(hdl);
  const bool revoked = ShouldTrackClientInfo(id) &&
      device_db_manager_->RevokeReconnectCredential(id);
  json reply = {{"type", "revoke_reconnect_credential"}, {"user_id", id},
                {"status", revoked ? "success" : "fail"}};
  if (!revoked) reply["reason"] = "Unable to revoke reconnect credential";
  send_msg_(hdl, reply);
  return revoked;
}

bool SignalNegotiation::client_info(websocketpp::connection_hdl hdl,
                                    const json& j) {
  std::string version;
  std::string platform;
  if (!GetStringField(j, "version", version) || version.empty() ||
      version.size() > 64) {
    LOG_ERROR("client_info missing or invalid field: version");
    return false;
  }
  if (!GetStringField(j, "platform", platform) || platform.empty() ||
      platform.size() > 32) {
    LOG_ERROR("client_info missing or invalid field: platform");
    return false;
  }

  const std::string user_id = transmission_manager_->GetUserId(hdl);
  if (user_id.empty()) {
    LOG_WARN("Ignore client_info from unauthenticated connection");
    return false;
  }

  if (!ShouldTrackClientInfo(user_id)) {
    return true;
  }

  if (!device_db_manager_->UpdateDeviceClientInfo(user_id, version,
                                                   platform)) {
    LOG_ERROR("Failed to store client information for [{}]", user_id);
    return false;
  }

  LOG_INFO("Client [{}] reports version [{}] on [{}]", user_id, version,
           platform);
  return true;
}

bool SignalNegotiation::leave_transmission(websocketpp::connection_hdl hdl,
                                           const json& j) {
  std::string transmission_id;
  std::string user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("leave_transmission missing required fields");
    return false;
  }

  if (user_id.empty() || transmission_id.empty() ||
      transmission_manager_->GetUserId(hdl) != user_id) {
    LOG_WARN("Reject leave request with unauthenticated sender");
    return false;
  }

  if (!transmission_manager_->IsConnectionInTransmission(hdl, transmission_id)) {
    LOG_WARN("Reject leave request outside an authorized transmission");
    return false;
  }

  std::vector<websocketpp::connection_hdl> recipients;
  for (const auto& id :
       transmission_manager_->GetAllUserIdOfTransmission(transmission_id)) {
    if (id != user_id) {
      const auto recipient =
          transmission_manager_->GetWsHandle(id, transmission_id);
      if (!recipient.expired()) recipients.push_back(recipient);
    }
  }

  if (transmission_manager_->IsHostOfTransmission(user_id, transmission_id)) {
    transmission_manager_->ReleaseTransmission(transmission_id);
    LOG_INFO("Release transmission [{}] due to host leaves", transmission_id);
  } else if (!transmission_manager_->ReleaseGuestFromTransmission(
                 user_id, transmission_id)) {
    return false;
  }

  deliveries_.erase({transmission_id, user_id});
  LOG_INFO("[{}] leaves transmission [{}]", user_id.c_str(),
           transmission_id.c_str());

  json message = {{"type", "user_leave_transmission"},
                  {"transmission_id", transmission_id},
                  {"user_id", user_id}};

  for (const auto& recipient : recipients) {
    send_msg_(recipient, message);
  }

  return true;
}

bool SignalNegotiation::disconnect_peer(websocketpp::connection_hdl hdl,
                                         const json& j) {
  std::string transmission_id;
  std::string user_id;
  std::string remote_user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "user_id", user_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !IsAuthorizedPeerSignal(hdl, user_id, remote_user_id, transmission_id) ||
      !transmission_manager_->IsHostOfTransmission(user_id, transmission_id)) {
    LOG_WARN("Reject unauthorized peer disconnect request");
    return false;
  }

  const auto recipient =
      transmission_manager_->GetWsHandle(remote_user_id, transmission_id);
  if (!transmission_manager_->ReleaseGuestFromTransmission(remote_user_id,
                                                          transmission_id)) {
    return false;
  }
  // Preserve the host and other guests. Existing clients already understand
  // this notification as closure of their connection to the named peer.
  send_msg_(recipient, {{"type", "user_leave_transmission"},
                        {"transmission_id", transmission_id},
                        {"user_id", user_id}});
  LOG_INFO("[{}] disconnected [{}] from transmission [{}]", user_id,
           remote_user_id, transmission_id);
  return true;
}

bool SignalNegotiation::query_user_id_list(websocketpp::connection_hdl hdl,
                                           const json& j,
                                           const std::string& source_address,
                                           const CredentialDispatch& dispatch) {
  std::string transmission_id_pwd;
  if (!GetStringField(j, "transmission_id", transmission_id_pwd)) {
    LOG_ERROR("query_user_id_list missing or invalid field: transmission_id");
    return false;
  }

  std::string transmission_id;
  std::string password;

  if (transmission_id_pwd.find("@") != std::string::npos) {
    transmission_id =
        transmission_id_pwd.substr(0, transmission_id_pwd.find("@"));
    password = transmission_id_pwd.substr(transmission_id_pwd.find("@") + 1);
  } else {
    transmission_id = transmission_id_pwd;
    password = "";
  }

  json failure = {{"type", "user_id_list"},
                  {"transmission_id", transmission_id},
                  {"user_id_list", json::array()},
                  {"status", "failed"}};
  if (hdl.expired() || transmission_manager_->GetUserId(hdl).empty()) {
    failure["reason"] = "Not authenticated";
    send_msg_(hdl, failure);
    return true;
  }
  AuthenticateDevice(
      hdl, source_address, transmission_id, password, failure,
      DeviceAuthLimiter::Operation::MemberQuery, dispatch,
      [this, hdl, transmission_id] {
        std::vector<std::string> user_id_list =
            transmission_manager_->GetAllUserIdOfTransmission(transmission_id);

        json message = {{"type", "user_id_list"},
                        {"transmission_id", transmission_id},
                        {"user_id_list", user_id_list},
                        {"status", "success"}};

        send_msg_(hdl, message);
      });

  return true;
}

bool SignalNegotiation::join_transmission(websocketpp::connection_hdl hdl,
                                          const json& j,
                                          const std::string& source_address,
                                          const CredentialDispatch& dispatch) {
  std::string transmission_id_pwd;
  if (!GetStringField(j, "transmission_id", transmission_id_pwd)) {
    LOG_ERROR("join_transmission missing or invalid field: transmission_id");
    return false;
  }

  std::string transmission_id;
  std::string password;

  if (transmission_id_pwd.find("@") != std::string::npos) {
    transmission_id =
        transmission_id_pwd.substr(0, transmission_id_pwd.find("@"));
    password = transmission_id_pwd.substr(transmission_id_pwd.find("@") + 1);
  } else {
    transmission_id = transmission_id_pwd;
    password = "";
  }

  std::string user_id;
  if (!GetStringField(j, "user_id", user_id) || user_id.empty()) {
    LOG_ERROR("join_transmission missing or invalid field: user_id");
    return false;
  }

  json failure = {{"type", "user_join_transmission"},
                  {"transmission_id", transmission_id},
                  {"status", "failed"}};
  if (hdl.expired() || transmission_manager_->GetUserId(hdl) != user_id) {
    SetFailure(failure, "NOT_AUTHENTICATED", "Not authenticated");
    send_msg_(hdl, failure);
    return true;
  }
  std::string request_id;
  if (j.contains("request_id") && (!GetStringField(j, "request_id", request_id) ||
      request_id.empty() || request_id.size() > 64)) return false;
  if (!request_id.empty()) failure["request_id"] = request_id;
  const auto previous = deliveries_.find({transmission_id, user_id});
  if (previous != deliveries_.end() && !request_id.empty()) {
    const auto& delivery = previous->second;
    const auto host = transmission_manager_->GetWsHandle(transmission_id);
    if (delivery.request_id == request_id && !hdl.owner_before(delivery.guest) &&
        !delivery.guest.owner_before(hdl) && !host.owner_before(delivery.host) &&
        !delivery.host.owner_before(host) &&
        (delivery.status == Delivery::Status::TimedOut ||
         transmission_manager_->IsConnectionInTransmission(hdl, transmission_id))) {
      if (delivery.status == Delivery::Status::TimedOut) {
        failure["error_code"] = "REMOTE_UNRESPONSIVE";
        failure["reason"] = "Remote did not respond";
        send_msg_(hdl, failure);
      } else if (delivery.status != Delivery::Status::Waiting) {
        send_msg_(hdl, {{"type", "join_delivery"}, {"request_id", request_id},
            {"transmission_id", transmission_id},
            {"status", delivery.status == Delivery::Status::Received ? "received" : "checking"}});
      }
      return true;
    }
  }
  if (password.empty() && ValidDeviceCredentials(transmission_id, password)) {
    // Older desktops probe with ID@ to open their password dialog. This is
    // not a password guess: never read the target record, spend a credential
    // worker slot, or consume a failure budget for it.
    SetFailure(failure, "PASSWORD_REQUIRED", "Password required");
    send_msg_(hdl, failure);
    return true;
  }
  AuthenticateDevice(
      hdl, source_address, transmission_id, password, failure,
      DeviceAuthLimiter::Operation::RemoteConnection, dispatch,
      [this, hdl, transmission_id, user_id, request_id] {
        std::string host_id =
            transmission_manager_->GetHostIdOfTransmission(transmission_id);
        websocketpp::connection_hdl host_hdl =
            transmission_manager_->GetWsHandle(host_id);

        if (host_id.empty() || host_hdl.expired()) {
          LOG_WARN("Remote [{}] is unavailable, cannot join transmission",
                   transmission_id.c_str());
          json message = {{"type", "user_join_transmission"},
                          {"transmission_id", transmission_id},
                          {"status", "failed"}};
          SetFailure(message, "REMOTE_UNAVAILABLE", "Remote unavailable");
          send_msg_(hdl, message);
          return;
        }

        const DeliveryKey key{transmission_id, user_id};
        if (!transmission_manager_->BindGuestToTransmission(
                user_id, transmission_id, hdl)) {
          LOG_WARN("Reject connection request without a valid guest binding");
          return;
        }

        // Also confirm an existing legacy routing entry restored at startup;
        // binding that entry does not emit a new session callback.
        std::string generation;
        device_db_manager_->StartRemoteControlSession(transmission_id, host_id,
                                                      user_id, &generation);

        json message = {{"type", "user_join_transmission"},
                        {"transmission_id", transmission_id},
                        {"user_id", user_id},
                        {"status", "success"}};

        deliveries_.erase(key);
        if (!request_id.empty()) {
          const auto id = std::to_string(++delivery_sequence_);
          deliveries_.emplace(key, Delivery{hdl, host_hdl, request_id, id,
                                             std::chrono::steady_clock::now()});
          message["delivery_id"] = id;
        }
        if (!generation.empty()) message["route_generation"] = generation;
        if (recovery_) recovery_->Issue(transmission_id, host_id, user_id, hdl);
        AddConnectionIceConfig(message, host_id);
        send_msg_(host_hdl, message);
      });

  return true;
}

bool SignalNegotiation::ConfirmDelivery(const DeliveryKey& key,
    websocketpp::connection_hdl host, const std::string& delivery_id) {
  auto it = deliveries_.find(key);
  if (it == deliveries_.end()) return false;
  auto& d = it->second;
  if (d.status == Delivery::Status::Received || d.status == Delivery::Status::TimedOut ||
      host.owner_before(d.host) || d.host.owner_before(host) ||
      (!delivery_id.empty() && delivery_id != d.delivery_id) ||
      !transmission_manager_->IsConnectionInTransmission(d.guest, key.first) ||
      !transmission_manager_->IsConnectionInTransmission(host, key.first)) return false;
  d.status = Delivery::Status::Received;
  ++receipts_;
  if (receipt_times_ms_.size() < 8192)
    receipt_times_ms_.push_back(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - d.started).count());
  send_msg_(d.guest, {{"type", "join_delivery"}, {"status", "received"},
                      {"request_id", d.request_id}, {"transmission_id", key.first}});
  return true;
}

bool SignalNegotiation::JoinReceipt(websocketpp::connection_hdl host, const json& j) {
  std::string tx, guest, id;
  if (GetStringField(j, "transmission_id", tx) && GetStringField(j, "user_id", guest) &&
      GetStringField(j, "delivery_id", id) && !id.empty())
    return ConfirmDelivery({tx, guest}, host, id);
  return false;
}

bool SignalNegotiation::FirstFrame(websocketpp::connection_hdl guest, const json& j) {
  std::string tx, request;
  const auto ms = j.find("elapsed_ms");
  if (!GetStringField(j, "transmission_id", tx) || !GetStringField(j, "request_id", request) ||
      ms == j.end() || !ms->is_number_integer() || *ms < 0 || *ms > 3600000) return false;
  auto it = deliveries_.find({tx, transmission_manager_->GetUserId(guest)});
  if (it == deliveries_.end()) return false;
  auto& d = it->second;
  if (d.guest.owner_before(guest) || guest.owner_before(d.guest) || d.request_id != request ||
      d.status != Delivery::Status::Received || d.first_frame ||
      !transmission_manager_->IsConnectionInTransmission(guest, tx)) return false;
  d.first_frame = true;
  ++first_frames_;
  if (frame_times_ms_.size() < 8192) frame_times_ms_.push_back(ms->get<int64_t>());
  return true;
}

void SignalNegotiation::LogDeliveryDiagnostics() {
  const auto p95 = [](std::vector<int64_t>& samples) {
    if (samples.empty()) return int64_t{-1};
    const auto rank = (samples.size() * 95 + 99) / 100 - 1;
    std::nth_element(samples.begin(), samples.begin() + rank, samples.end());
    return samples[rank];
  };
  LOG_INFO("Join delivery: forwarded_total={} received_total={} unresponsive_total={} "
           "first_frame_total={} receipt_samples={} receipt_p95_ms={} "
           "first_frame_samples={} join_to_first_frame_p95_ms={}", delivery_sequence_,
           receipts_, delivery_timeouts_, first_frames_, receipt_times_ms_.size(),
           p95(receipt_times_ms_), frame_times_ms_.size(), p95(frame_times_ms_));
  receipt_times_ms_.clear();
  frame_times_ms_.clear();
}

void SignalNegotiation::TickJoinDeliveries(
    const std::function<void(websocketpp::connection_hdl)>& probe) {
  using namespace std::chrono;
  const auto now = steady_clock::now();
  for (auto it = deliveries_.begin(); it != deliveries_.end();) {
    const auto& [tx, guest] = it->first;
    auto& d = it->second;
    const auto age = now - d.started;
    if (d.guest.expired() || (d.status == Delivery::Status::TimedOut && age >= seconds(60))) {
      it = deliveries_.erase(it);
      continue;
    }
    if (age < seconds(3) || d.status == Delivery::Status::TimedOut) { ++it; continue; }
    const bool bound = transmission_manager_->IsConnectionInTransmission(d.guest, tx);
    if (d.status == Delivery::Status::Received) {
      // Keep the idempotency key for the lifetime of the current route.
      if (!bound) it = deliveries_.erase(it);
      else ++it;
      continue;
    }
    if (transmission_manager_->GetUserId(d.guest) != guest) {
      it = deliveries_.erase(it);
      continue;
    }
    if (age >= seconds(10) || !bound || d.host.expired()) {
      if (bound) transmission_manager_->ReleaseGuestFromTransmission(guest, tx);
      send_msg_(d.guest, {{"type", "user_join_transmission"}, {"status", "failed"},
          {"transmission_id", tx}, {"request_id", d.request_id},
          {"error_code", "REMOTE_UNRESPONSIVE"}, {"reason", "Remote did not respond"}});
      ++delivery_timeouts_;
      d.status = Delivery::Status::TimedOut;
      ++it;
      continue;
    }
    if (d.status == Delivery::Status::Waiting) {
      d.status = Delivery::Status::Checking;
      probe(d.host);
      send_msg_(d.guest, {{"type", "join_delivery"}, {"status", "checking"},
          {"request_id", d.request_id}, {"transmission_id", tx}});
    }
    ++it;
  }
}

bool SignalNegotiation::offer(websocketpp::connection_hdl hdl, const json& j) {
  std::string transmission_id;
  std::string remote_user_id;
  std::string user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("offer missing required fields");
    return false;
  }

  if (!IsAuthorizedPeerSignal(hdl, user_id, remote_user_id, transmission_id)) {
    LOG_WARN("Reject offer outside an authorized transmission");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id, transmission_id);

  std::string sdp;
  if (GetStringField(j, "sdp", sdp)) {
    json message = {
        {"type", "offer"},
        {"transmission_id", transmission_id},
        {"remote_user_id", user_id},
        {"sdp", sdp},
    };
    const auto guest = transmission_manager_->IsHostOfTransmission(user_id, transmission_id)
                           ? remote_user_id : user_id;
    const auto generation =
        device_db_manager_->SessionRouteGeneration(transmission_id, guest);
    if (!generation.empty()) message["route_generation"] = generation;
    if (!sdp.empty() && !receipt_hosts_.count(hdl))
      ConfirmDelivery({transmission_id, remote_user_id}, hdl, {});
    AddConnectionIceConfig(message, remote_user_id);
    LOG_INFO("[{}] send offer to [{}]", user_id, remote_user_id);
    send_msg_(destination_hdl, message);

  } else {
    LOG_ERROR("Invalid offer msg");
  }

  return true;
}

bool SignalNegotiation::session_route(websocketpp::connection_hdl hdl, const json& j) {
  const auto user = transmission_manager_->GetUserId(hdl);
  std::string tx, peer, generation, route;
  if (user.empty() || !GetStringField(j, "transmission_id", tx) ||
      !GetStringField(j, "remote_user_id", peer) ||
      !GetStringField(j, "route_generation", generation) ||
      !GetStringField(j, "route", route) || user == peer ||
      tx.size() > 128 || peer.size() > 128) return false;
  // If the join has a live socket grant, reject reports from superseded sockets.
  const auto bound = transmission_manager_->GetWsHandle(user, tx);
  if (!bound.expired() && (hdl.owner_before(bound) || bound.owner_before(hdl))) return false;
  device_db_manager_->UpdateSessionRoute(tx, user, peer, generation, route);
  return true;
}

bool SignalNegotiation::answer(websocketpp::connection_hdl hdl, const json& j) {
  std::string transmission_id;
  std::string remote_user_id;
  std::string user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !GetStringField(j, "user_id", user_id)) {
    LOG_ERROR("answer missing required fields");
    return false;
  }

  if (!IsAuthorizedPeerSignal(hdl, user_id, remote_user_id, transmission_id)) {
    LOG_WARN("Reject answer outside an authorized transmission");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id, transmission_id);

  std::string sdp;
  if (GetStringField(j, "sdp", sdp)) {
    json message = {{"type", "answer"},
                    {"sdp", sdp},
                    {"remote_user_id", user_id},
                    {"transmission_id", transmission_id}};
    LOG_INFO("[{}] send answer to [{}]", user_id, remote_user_id);
    send_msg_(destination_hdl, message);
  } else {
    LOG_ERROR("Invalid answer msg");
  }

  return true;
}

bool SignalNegotiation::new_candidate(websocketpp::connection_hdl hdl,
                                      const json& j) {
  std::string transmission_id;
  std::string candidate;
  std::string user_id;
  std::string remote_user_id;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "sdp", candidate) ||
      !GetStringField(j, "user_id", user_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id)) {
    LOG_ERROR("new_candidate missing required fields");
    return false;
  }

  if (!IsAuthorizedPeerSignal(hdl, user_id, remote_user_id, transmission_id)) {
    LOG_WARN("Reject candidate outside an authorized transmission");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id, transmission_id);

  // LOG_INFO("send candidate [{}]", candidate.c_str());
  json message = {{"type", "new_candidate"},
                  {"sdp", candidate},
                  {"remote_user_id", user_id},
                  {"transmission_id", transmission_id}};
  if (!CopyOptionalIceUfrag(j, message)) {
    LOG_WARN("new_candidate has an invalid ICE username fragment");
    return false;
  }
  send_msg_(destination_hdl, message);

  return true;
}

bool SignalNegotiation::new_candidate_mid(websocketpp::connection_hdl hdl,
                                          const json& j) {
  std::string transmission_id;
  std::string user_id;
  std::string remote_user_id;
  std::string candidate;
  std::string mid;
  if (!GetStringField(j, "transmission_id", transmission_id) ||
      !GetStringField(j, "user_id", user_id) ||
      !GetStringField(j, "remote_user_id", remote_user_id) ||
      !GetStringField(j, "candidate", candidate) ||
      !GetStringField(j, "mid", mid)) {
    LOG_ERROR("new_candidate_mid missing required fields");
    return false;
  }

  if (!IsAuthorizedPeerSignal(hdl, user_id, remote_user_id, transmission_id)) {
    LOG_WARN("Reject candidate outside an authorized transmission");
    return false;
  }

  websocketpp::connection_hdl destination_hdl =
      transmission_manager_->GetWsHandle(remote_user_id, transmission_id);

  // LOG_INFO("send candidate [{}]", candidate.c_str());
  json message = {{"type", "new_candidate_mid"},
                  {"remote_user_id", user_id},
                  {"transmission_id", transmission_id},
                  {"candidate", candidate},
                  {"mid", mid}};
  if (!CopyOptionalIceUfrag(j, message)) {
    LOG_WARN("new_candidate_mid has an invalid ICE username fragment");
    return false;
  }
  send_msg_(destination_hdl, message);

  return true;
}

bool SignalNegotiation::change_password(websocketpp::connection_hdl hdl,
                                        const json& j,
                                        const std::string& source_address,
                                        const CredentialDispatch& dispatch) {
  json message = {{"type", "change_password"}, {"status", "fail"}};
  std::string request_id;
  if (!GetStringField(j, "request_id", request_id) || request_id.empty() ||
      request_id.size() > 128 || request_id.find('\0') != std::string::npos) {
    message["reason"] = "Missing or invalid request ID";
    send_msg_(hdl, message);
    return true;
  }
  message["request_id"] = request_id;
  const std::string user_id = transmission_manager_->GetUserId(hdl);
  message["user_id"] = user_id;
  std::string new_password;
  if (user_id.empty()) {
    message["reason"] = "Not authenticated";
  } else if (user_id.rfind("C-", 0) == 0 || IsTemporaryControllerId(user_id)) {
    message["reason"] = "Password changes are not allowed for this identity";
  } else if (!GetStringField(j, "new_password", new_password) ||
             new_password.size() != 6 ||
             !DevicePassword::ValidInput(new_password)) {
    message["reason"] = "Password must contain exactly 6 characters";
  } else {
    const auto cache_key = user_id + "\n" + request_id;
    const auto fingerprint = PasswordFingerprint(new_password);
    {
      std::lock_guard<std::mutex> lock(password_change_mutex_);
      const auto cached = password_change_results_.find(cache_key);
      if (cached != password_change_results_.end()) {
        if (cached->second.password_fingerprint == fingerprint)
          message = cached->second.response;
        else
          message["reason"] = "Request ID was already used";
        send_msg_(hdl, message);
        return true;
      }
    }
    const auto record = device_db_manager_->ReadPassword(user_id);
    RunCredentialWork(
        hdl, source_address, message,
        [this, hdl, user_id, new_password, record, message, cache_key,
         fingerprint]() -> std::function<void()> {
          const auto replacement = DevicePassword::Hash(new_password);
          return [this, hdl, user_id, record, replacement, message = message,
                  cache_key, fingerprint]() mutable {
            if (hdl.expired() ||
                transmission_manager_->GetUserId(hdl) != user_id)
              return;
            std::lock_guard<std::mutex> lock(password_change_mutex_);
            const auto cached = password_change_results_.find(cache_key);
            if (cached != password_change_results_.end()) {
              if (cached->second.password_fingerprint == fingerprint)
                message = cached->second.response;
              else
                message["reason"] = "Request ID was already used";
            } else if (!record || !replacement ||
                       !device_db_manager_->CommitPassword(user_id, *record,
                                                           *replacement)) {
              message["reason"] = "Failed to update password";
            } else {
              message["status"] = "success";
              password_change_results_.emplace(
                  cache_key, PasswordChangeResult{fingerprint, message});
              password_change_result_order_.push_back(cache_key);
              while (password_change_result_order_.size() > 4096) {
                password_change_results_.erase(
                    password_change_result_order_.front());
                password_change_result_order_.pop_front();
              }
              LOG_INFO("Authenticated client [{}] changed its device password",
                       user_id);
            }
            send_msg_(hdl, message);
          };
        },
        dispatch);
    return true;
  }
  send_msg_(hdl, message);
  return true;
}

void SignalNegotiation::ForgetPasswordChangeResults(const std::string& device_id) {
  std::lock_guard<std::mutex> lock(password_change_mutex_);
  const auto prefix = device_id + "\n";
  for (auto it = password_change_result_order_.begin();
       it != password_change_result_order_.end();) {
    if (it->rfind(prefix, 0) == 0) {
      password_change_results_.erase(*it);
      it = password_change_result_order_.erase(it);
    } else ++it;
  }
}

bool SignalNegotiation::turn_credentials(websocketpp::connection_hdl hdl,
                                         const json& j) {
  (void)j;
  json message = {{"type", "turn_credentials"}};
  const std::string user_id = transmission_manager_->GetUserId(hdl);
  if (user_id.empty()) {
    message["status"] = "fail";
    message["reason"] = "Not authenticated";
  } else if (AddTurnCredentials(message, user_id)) {
    message["status"] = "success";
  } else {
    message["status"] = "fail";
    message["reason"] = "TURN credentials are not configured";
  }
  send_msg_(hdl, message);
  return true;
}

void SignalNegotiation::OnTemporaryControllerDisconnect(
    const std::string& user_id) {
  // Extract pure user_id (remove password part if exists)
  std::string pure_user_id = user_id;
  size_t at_pos = user_id.find("@");
  if (at_pos != std::string::npos) {
    pure_user_id = user_id.substr(0, at_pos);
  }

  // Remove the temporary identity, presence and associations together after
  // session release/logout. Daily activity survives the WebSocket disconnect.
  if (IsTemporaryControllerId(pure_user_id)) {
    if (!device_db_manager_->RemoveDevice(pure_user_id)) {
      LOG_WARN("Failed to remove temporary controller [{}] from database",
               pure_user_id);
    }
  }
}
