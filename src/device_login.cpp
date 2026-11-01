#include "signal_negotiation.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <memory>

#include "controller_identity.h"

namespace {
bool Hex(const std::string& text, size_t bytes) {
  return text.size() == bytes * 2 && std::all_of(text.begin(), text.end(),
      [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string Nonce() {
  std::array<unsigned char, 32> bytes{};
  if (RAND_bytes(bytes.data(), bytes.size()) != 1) return {};
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const auto byte : bytes) {
    result += digits[byte >> 4]; result += digits[byte & 15];
  }
  return result;
}
template <size_t N> std::array<unsigned char, N> Decode(const std::string& text) {
  std::array<unsigned char, N> bytes{};
  auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < N; ++i)
    bytes[i] = (digit(text[2 * i]) << 4) | digit(text[2 * i + 1]);
  return bytes;
}
bool Verify(const std::string& id, const std::string& public_key,
            const std::string& nonce, const std::string& signature) {
  if (!Hex(public_key, 32) || !Hex(signature, 64)) return false;
  const auto raw_key = Decode<32>(public_key);
  const auto raw_signature = Decode<64>(signature);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
      EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, raw_key.data(), raw_key.size()),
      EVP_PKEY_free);
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  const auto transcript = json::array({"crossdesk.device-login.v1", id, public_key, nonce}).dump();
  return key && context &&
      EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
      EVP_DigestVerify(context.get(), raw_signature.data(), raw_signature.size(),
          reinterpret_cast<const unsigned char*>(transcript.data()), transcript.size()) == 1;
}
}  // namespace

bool SignalNegotiation::LoginWithKey(websocketpp::connection_hdl hdl,
    const json& request, const std::string& source, const CredentialDispatch& dispatch,
    std::function<void()> on_login) {
  json failure{{"type", "login"}, {"user_id", ""}, {"status", "fail"},
               {"reason", "Authentication failed"}, {"error_code", "AUTHENTICATION_FAILED"}};
  auto reject = [&] { send_msg_(hdl, failure); return false; };
  try {
    const auto identity = request.at("user_id").get<std::string>();
    const auto at = identity.find('@');
    const auto id = identity.substr(0, at);
    const auto password = at == std::string::npos ? "" : identity.substr(at + 1);
    const auto& proof = request.at("device_key");
    if (id.size() > 128 || password.size() > 256 || identity.find('\0') != std::string::npos ||
        id.rfind("C-", 0) == 0 || IsTemporaryControllerId(id) ||
        IsTemporaryControllerRegistration(id) || !proof.is_object() || proof.size() > 4 ||
        !proof.at("version").is_number_integer() || proof.at("version") != 1)
      return reject();
    const auto public_key = proof.at("public_key").get<std::string>();
    if (!Hex(public_key, 32)) return reject();
    const auto now = std::chrono::steady_clock::now();

    if (!proof.contains("signature")) {
      if (proof.size() != 2 || at != std::string::npos) return reject();
      const int retry = key_login_limiter_.Admit(source);
      if (retry) {
        failure["error_code"] = "CREDENTIAL_RATE_LIMITED";
        failure["retry_after"] = retry;
        return reject();
      }
      if (now >= next_login_challenge_cleanup_) {
        for (auto it = login_challenges_.begin(); it != login_challenges_.end();) {
          if (it->first.expired() || now >= it->second.expires) it = login_challenges_.erase(it);
          else ++it;
        }
        next_login_challenge_cleanup_ = now + std::chrono::seconds(1);
      }
      if (login_challenges_.size() >= 16384 && !login_challenges_.count(hdl))
        return reject();
      auto nonce = Nonce();
      if (nonce.empty()) return reject();
      login_challenges_[hdl] = {id, public_key, nonce, now + std::chrono::seconds(10)};
      const auto key = device_db_manager_->ReadLoginPublicKey(id);
      send_msg_(hdl, {{"type", "login_challenge"}, {"user_id", id},
          {"device_key_version", 1}, {"nonce", nonce},
          {"password_required", !id.empty() && (!key || key->empty())}});
      return false;
    }

    auto it = login_challenges_.find(hdl);
    if (it == login_challenges_.end()) return reject();
    auto challenge = std::move(it->second);
    login_challenges_.erase(it);  // Consume before verification, including failures.
    if (now >= challenge.expires || id != challenge.id || public_key != challenge.public_key ||
        proof.at("nonce") != challenge.nonce ||
        !Verify(id, public_key, challenge.nonce, proof.at("signature").get<std::string>()))
      return reject();

    auto completed = std::make_shared<bool>(false);
    auto finish = [this, hdl, request, id, public_key, completed, on_login](
                      const DeviceCredential& credential, bool registration) {
      *completed = CompleteLogin(hdl, request, id, credential, registration,
                                 std::nullopt, public_key);
      if (*completed && on_login) on_login();
    };
    if (id.empty()) {
      if (!password.empty()) return reject();
      RegisterDevice(hdl, source, id, failure, dispatch, finish, public_key);
    } else {
      const auto bound = device_db_manager_->ReadLoginPublicKey(id);
      if (!bound) return reject();
      if (!bound->empty()) {
        if (*bound != public_key || !password.empty()) return reject();
        // No password hash, password failure counter or password worker queue.
        finish({id, "", false}, false);
      } else {
        // Only the first binding uses legacy credentials. Never replace a key.
        AuthenticateDevice(hdl, source, id, password, failure, dispatch,
            [id, finish] {
              finish({id, "", false}, false);
            }, public_key);
      }
    }
    return *completed;
  } catch (const std::exception&) {
    return reject();
  }
}
