#include "signal_negotiation.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "device_db_manager.h"
#include "transmission_manager.h"

int main() {
  int failures = 0;

  auto expect = [&failures](bool condition, const std::string& message) {
    if (!condition) {
      std::cerr << "FAIL: " << message << std::endl;
      ++failures;
    }
  };

  const auto db_path =
      std::filesystem::temp_directory_path() /
      ("crossdesk_signal_negotiation_test_" +
       std::to_string(std::chrono::steady_clock::now()
                          .time_since_epoch()
                          .count()) +
       ".db");

  {
    DeviceDBManager db(db_path.string());
    DeviceCredential offline_host = db.AddDevice("", "");
    expect(!offline_host.device_id.empty(), "test host device is registered");
    expect(!offline_host.password.empty(), "test host password is registered");

    auto transmission = std::make_shared<TransmissionManager>();
    auto turn_issuer = std::make_shared<TurnCredentialIssuer>(
        "test-turn-secret", "turn.example.com", 3478, 3600);
    SignalNegotiation negotiation(transmission, &db, turn_issuer);

    std::vector<json> sent_messages;
    auto requester_connection = std::make_shared<int>(1);
    websocketpp::connection_hdl requester_hdl(requester_connection);
    transmission->BindUserToWsHandle("C-controller", requester_hdl);
    negotiation.SetSendMsgCallback(
        [&](websocketpp::connection_hdl, json message) {
          sent_messages.push_back(message);
        });

    json request = {
        {"type", "join_transmission"},
        {"user_id", "C-controller"},
        {"transmission_id", offline_host.device_id + "@" +
                                offline_host.password},
    };

    negotiation.join_transmission(requester_hdl, request);

    expect(sent_messages.size() == 1,
           "offline host join sends one failure response to requester");
    if (!sent_messages.empty()) {
      expect(sent_messages[0].value("type", "") == "user_join_transmission",
             "offline host join response has user_join_transmission type");
      expect(sent_messages[0].value("status", "") == "failed",
             "offline host join response fails");
      expect(sent_messages[0].value("reason", "") == "Remote unavailable",
             "offline host join response reports remote unavailable");
    }

    sent_messages.clear();
    auto attacker_connection = std::make_shared<int>(2);
    websocketpp::connection_hdl attacker_hdl(attacker_connection);
    json login_with_wrong_password = {
        {"type", "login"},
        {"user_id", offline_host.device_id + "@attacker-password"},
    };

    negotiation.login_user(attacker_hdl, login_with_wrong_password);

    expect(sent_messages.size() == 1,
           "wrong device password login sends one response");
    if (!sent_messages.empty()) {
      expect(sent_messages[0].value("type", "") == "login",
             "wrong device password login response has login type");
      expect(sent_messages[0].value("status", "") == "fail",
             "wrong device password login fails");
      expect(sent_messages[0].value("reason", "") == "Authentication failed",
             "wrong device password login reports a generic reason");
    }
    expect(db.VerifyDevice(offline_host.device_id, offline_host.password) == 0,
           "wrong device password login does not replace original password");
    expect(db.VerifyDevice(offline_host.device_id, "attacker-password") != 0,
           "wrong device password is not accepted after failed login");

    sent_messages.clear();
    negotiation.change_password(
        attacker_hdl,
        {{"type", "change_password"}, {"new_password", "654321"}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("status", "") == "fail" &&
               sent_messages[0].value("reason", "") ==
                   "Missing or invalid request ID",
           "password change requires a request id");

    sent_messages.clear();
    negotiation.change_password(
        attacker_hdl,
        {{"type", "change_password"},
         {"request_id", "unauthenticated"},
         {"new_password", "654321"}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("status", "") == "fail" &&
               sent_messages[0].value("reason", "") == "Not authenticated",
           "unauthenticated connection cannot change a device password");
    expect(db.VerifyDevice(offline_host.device_id, offline_host.password) == 0,
           "unauthenticated password change leaves the password unchanged");

    sent_messages.clear();
    auto controller_connection = std::make_shared<int>(5);
    websocketpp::connection_hdl controller_hdl(controller_connection);
    transmission->BindUserToWsHandle("C-controller", controller_hdl);
    negotiation.change_password(
        controller_hdl,
        {{"type", "change_password"},
         {"request_id", "controller"},
         {"new_password", "654321"}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("status", "") == "fail",
           "temporary controller identity cannot change a device password");
    expect(db.VerifyDevice(offline_host.device_id, offline_host.password) == 0,
           "controller password change leaves the device password unchanged");

    sent_messages.clear();
    auto authenticated_connection = std::make_shared<int>(3);
    websocketpp::connection_hdl authenticated_hdl(authenticated_connection);
    json valid_login = {
        {"type", "login"},
        {"user_id", offline_host.device_id + "@" + offline_host.password},
    };
    negotiation.login_user(authenticated_hdl, valid_login);

    expect(sent_messages.size() == 1,
           "successful login sends one response");
    if (!sent_messages.empty()) {
      const json& response = sent_messages[0];
      expect(response.value("status", "") == "success",
             "valid device password login succeeds");
      expect(response.contains("turn") && response["turn"].is_object(),
             "successful login includes dynamic TURN credentials");
      if (response.contains("turn") && response["turn"].is_object()) {
        const json& turn = response["turn"];
        expect(turn.value("host", "") == "turn.example.com",
               "TURN response includes configured public host");
        expect(turn.value("port", 0) == 3478,
               "TURN response includes configured port");
        const std::string username = turn.value("username", "");
        expect(username.size() > offline_host.device_id.size() + 1 &&
                   username.substr(username.size() -
                                   offline_host.device_id.size() - 1) ==
                       ":" + offline_host.device_id,
               "TURN username is scoped to the authenticated device");
        expect(!turn.value("password", "").empty(),
               "TURN response includes a temporary password");
      }
    }

    sent_messages.clear();
    negotiation.turn_credentials(authenticated_hdl,
                                 {{"type", "turn_credentials"}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("status", "") == "success" &&
               sent_messages[0].contains("turn"),
           "authenticated client can explicitly refresh TURN credentials");

    sent_messages.clear();
    negotiation.change_password(
        authenticated_hdl,
        {{"type", "change_password"},
         {"request_id", "malformed"},
         {"new_password", "short"}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("status", "") == "fail",
           "authenticated client cannot set a malformed password");
    expect(db.VerifyDevice(offline_host.device_id, offline_host.password) == 0,
           "malformed password change leaves the password unchanged");

    sent_messages.clear();
    const std::string new_password = "654321";
    negotiation.change_password(
        authenticated_hdl,
        {{"type", "change_password"},
         {"request_id", "successful"},
         {"new_password", new_password}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("type", "") == "change_password" &&
               sent_messages[0].value("status", "") == "success" &&
               sent_messages[0].value("request_id", "") == "successful" &&
               sent_messages[0].value("user_id", "") == offline_host.device_id,
           "authenticated device can change its own password");
    expect(db.VerifyDevice(offline_host.device_id, offline_host.password) != 0,
           "old password is rejected after a successful password change");
    expect(db.VerifyDevice(offline_host.device_id, new_password) == 0,
           "new password is accepted after a successful password change");

    sent_messages.clear();
    auto relogin_connection = std::make_shared<int>(4);
    websocketpp::connection_hdl relogin_hdl(relogin_connection);
    negotiation.login_user(
        relogin_hdl,
        {{"type", "login"},
         {"user_id", offline_host.device_id + "@" + new_password}});
    expect(sent_messages.size() == 1 &&
               sent_messages[0].value("status", "") == "success",
           "device can log in again with its changed password");

  }

  std::filesystem::remove(db_path);

  if (failures > 0) {
    std::cerr << failures << " failure(s)" << std::endl;
    return 1;
  }

  return 0;
}
