/*
 * @Author: DI JUNKUN
 * @Date: 2025-06-26
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _SIGNAL_NEGOTIATION_H_
#define _SIGNAL_NEGOTIATION_H_

#include <deque>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>

#include "device_auth_limiter.h"
#include "device_db_manager.h"
#include "ice_server_config_issuer.h"
#include "transmission_manager.h"
#include "turn_credentials.h"

using nlohmann::json;
class SessionRecovery;

class SignalNegotiation {
 public:
  // Run computation on a credential worker; run its returned continuation on
  // the application worker after checking that the connection is still alive.
  using CredentialWork = std::function<std::function<void()>()>;
  // Admission is invoked only with a reserved execution slot, never on enqueue.
  // Expiry sends the original protocol's failure response for legacy clients.
  using CredentialDispatch = std::function<bool(
      CredentialWork, std::function<int()>, std::function<void()>)>;
  SignalNegotiation(
      std::shared_ptr<TransmissionManager> transmission_manager,
      DeviceDBManager* device_db,
      std::shared_ptr<TurnCredentialIssuer> turn_credential_issuer = nullptr,
      std::shared_ptr<IceServerConfigIssuer> ice_config_issuer = nullptr,
      std::shared_ptr<DeviceAuthLimiter> device_auth_limiter = nullptr,
      size_t source_credential_limit = 60);
  ~SignalNegotiation();

  void SetSendMsgCallback(
      std::function<void(websocketpp::connection_hdl, json)> send_msg) {
    send_msg_ = send_msg;
  }

  // Returns true for synchronous success. With a dispatcher, success is
  // reported by on_login after binding, never when work is merely queued.
  bool login_user(websocketpp::connection_hdl hdl, const json& j,
                  const std::string& source_address = "",
                  const CredentialDispatch& dispatch = {},
                  std::function<void()> on_login = {});
  bool leave_transmission(websocketpp::connection_hdl hdl, const json& j);
  bool disconnect_peer(websocketpp::connection_hdl hdl, const json& j);
  bool query_user_id_list(websocketpp::connection_hdl hdl, const json& j,
                          const std::string& source_address = "",
                          const CredentialDispatch& dispatch = {});
  bool join_transmission(websocketpp::connection_hdl hdl, const json& j,
                         const std::string& source_address = "",
                         const CredentialDispatch& dispatch = {});
  bool offer(websocketpp::connection_hdl hdl, const json& j);
  bool answer(websocketpp::connection_hdl hdl, const json& j);
  bool new_candidate(websocketpp::connection_hdl hdl, const json& j);
  bool new_candidate_mid(websocketpp::connection_hdl hdl, const json& j);
  bool change_password(websocketpp::connection_hdl hdl, const json& j,
                       const std::string& source_address = "",
                       const CredentialDispatch& dispatch = {});
  bool turn_credentials(websocketpp::connection_hdl hdl, const json& j);
  bool client_info(websocketpp::connection_hdl hdl, const json& j);
  bool revoke_reconnect_credential(websocketpp::connection_hdl hdl);
  void OnTemporaryControllerDisconnect(const std::string& user_id);
  void ForgetPasswordChangeResults(const std::string& device_id);
  void SetSessionRecovery(SessionRecovery* recovery) { recovery_ = recovery; }

 private:
  struct PasswordChangeResult {
    std::string password_fingerprint;
    json response;
  };

  bool IsAuthorizedPeerSignal(websocketpp::connection_hdl hdl,
                              const std::string& user_id,
                              const std::string& remote_user_id,
                              const std::string& transmission_id) const;
  bool AddTurnCredentials(json& message, const std::string& user_id) const;
  void AddLoginIceConfig(json& message, const json& request,
                         const std::string& user_id) const;
  void AddConnectionIceConfig(json& message, const std::string& user_id) const;
  void AuthenticateDevice(websocketpp::connection_hdl hdl,
                          const std::string& source_address,
                          const std::string& device_id,
                          const std::string& password, json failure,
                          const CredentialDispatch& dispatch,
                          std::function<void()> on_verified,
                          const std::string& bind_login_key = "");
  bool CompleteLogin(websocketpp::connection_hdl hdl, const json& request,
                     const std::string& host_id,
                     const DeviceCredential& credential, bool registration,
                     std::optional<int64_t> reconnect_expires = std::nullopt,
                     const std::string& verified_login_key = "");
  void RegisterDevice(websocketpp::connection_hdl hdl,
                       const std::string& source, const std::string& kind,
                       json failure, const CredentialDispatch& dispatch,
                       std::function<void(const DeviceCredential&, bool)> finish,
                       const std::string& login_public_key = "");
  bool LoginWithKey(websocketpp::connection_hdl hdl, const json& request,
                    const std::string& source, const CredentialDispatch& dispatch,
                    std::function<void()> on_login);
  void RunCredentialWork(websocketpp::connection_hdl hdl,
                         const std::string& source_address, json failure,
                         CredentialWork work,
                         const CredentialDispatch& dispatch);

  std::shared_ptr<TransmissionManager> transmission_manager_;
  DeviceDBManager* device_db_manager_;
  std::shared_ptr<TurnCredentialIssuer> turn_credential_issuer_;
  std::shared_ptr<IceServerConfigIssuer> ice_config_issuer_;
  std::shared_ptr<DeviceAuthLimiter> device_auth_limiter_;
  CredentialWorkLimiter credential_work_limiter_;
  struct LoginChallenge {
    std::string id, public_key, nonce;
    std::chrono::steady_clock::time_point expires;
  };
  // Application-worker owned; challenges belong to connections, never to an ID.
  std::map<websocketpp::connection_hdl, LoginChallenge,
           std::owner_less<websocketpp::connection_hdl>> login_challenges_;
  std::chrono::steady_clock::time_point next_login_challenge_cleanup_{};
  CredentialWorkLimiter key_login_limiter_{DeviceAuthLimiter::Clock::now, 120};
  std::function<void(websocketpp::connection_hdl, json)> send_msg_;
  std::mutex password_change_mutex_;
  std::unordered_map<std::string, PasswordChangeResult>
      password_change_results_;
  std::deque<std::string> password_change_result_order_;
  SessionRecovery* recovery_ = nullptr;
};

#endif
