#include "device_password.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace {
// An application format, with a hex salt in password_salt and hex digest here.
// Only known work factors are accepted, bounding CPU/memory for corrupt data.
constexpr char kPrefix[] = "argon2id$v=19$m=19456,t=2,p=1$";
constexpr size_t kSaltBytes = 16, kDigestBytes = 32;

std::string Hex(const unsigned char* data, size_t size) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    result += digits[data[i] >> 4];
    result += digits[data[i] & 15];
  }
  return result;
}

bool Unhex(const std::string& text, unsigned char* output, size_t size) {
  if (text.size() != size * 2) return false;
  auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < size; ++i) {
    const int hi = digit(text[i * 2]), lo = digit(text[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    output[i] = static_cast<unsigned char>((hi << 4) | lo);
  }
  return true;
}

using Kdf = std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)>;
using Context = std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)>;

std::optional<DevicePasswordRecord> Derive(
    const std::string& password,
    const std::array<unsigned char, kSaltBytes>& salt) {
  Kdf kdf(EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr), EVP_KDF_free);
  if (!kdf) return std::nullopt;
  Context context(EVP_KDF_CTX_new(kdf.get()), EVP_KDF_CTX_free);
  if (!context) return std::nullopt;
  uint32_t memory = 19456, iterations = 2, lanes = 1, threads = 1,
           version = 0x13;
  OSSL_PARAM params[] = {
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_PASSWORD,
                                        const_cast<char*>(password.data()),
                                        password.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                        const_cast<unsigned char*>(salt.data()),
                                        salt.size()),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_MEMCOST, &memory),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_LANES, &lanes),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &threads),
      OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ARGON2_VERSION, &version),
      OSSL_PARAM_construct_end()};
  std::array<unsigned char, kDigestBytes> digest{};
  if (EVP_KDF_derive(context.get(), digest.data(), digest.size(), params) != 1)
    return std::nullopt;
  DevicePasswordRecord result{Hex(salt.data(), salt.size()),
                              kPrefix + Hex(digest.data(), digest.size())};
  OPENSSL_cleanse(digest.data(), digest.size());
  return result;
}
}  // namespace

namespace DevicePassword {
void CheckSupport() {
  // Exercise the provider and parameters, not just algorithm discovery.
  if (!Hash("startup-check"))
    throw std::runtime_error(
        "Argon2id or the secure random source is unavailable");
}

bool ValidInput(const std::string& password) {
  return password.size() <= 256 && password.find('\0') == std::string::npos;
}

std::string Generate() {
  constexpr char alphabet[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  std::string result;
  constexpr size_t length = 6;
  while (result.size() < length) {
    std::array<unsigned char, 32> random{};
    if (RAND_priv_bytes(random.data(), random.size()) != 1) return {};
    for (auto byte : random) {
      // 248 is the largest multiple of 62 below 256.
      if (byte < 248 && result.size() < length) result += alphabet[byte % 62];
    }
    OPENSSL_cleanse(random.data(), random.size());
  }
  return result;
}

std::optional<DevicePasswordRecord> Hash(const std::string& password) {
  if (!ValidInput(password)) return std::nullopt;
  std::array<unsigned char, kSaltBytes> salt{};
  if (RAND_bytes(salt.data(), salt.size()) != 1) return std::nullopt;
  return Derive(password, salt);
}

bool IsCurrent(const DevicePasswordRecord& record) {
  std::array<unsigned char, kSaltBytes> salt{};
  std::array<unsigned char, kDigestBytes> digest{};
  return record.hash.rfind(kPrefix, 0) == 0 &&
         Unhex(record.salt, salt.data(), salt.size()) &&
         Unhex(record.hash.substr(sizeof(kPrefix) - 1), digest.data(),
               digest.size());
}

std::optional<DevicePasswordRecord> Verify(
    const std::optional<DevicePasswordRecord>& record,
    const std::string& password) {
  if (!ValidInput(password)) return std::nullopt;
  if (record && IsCurrent(*record)) {
    std::array<unsigned char, kSaltBytes> salt{};
    Unhex(record->salt, salt.data(), salt.size());
    auto computed = Derive(password, salt);
    if (computed && computed->hash.size() == record->hash.size() &&
        CRYPTO_memcmp(computed->hash.data(), record->hash.data(),
                      computed->hash.size()) == 0)
      return record;
    return std::nullopt;
  }

  // Do the same expensive work even when the legacy check fails or no ID
  // exists. Reuse this calculation on success instead of hashing a second time.
  auto replacement = Hash(password);
  std::array<unsigned char, 8> legacy_salt{};
  std::array<unsigned char, SHA256_DIGEST_LENGTH> expected{}, actual{};
  const bool legacy =
      record && Unhex(record->salt, legacy_salt.data(), legacy_salt.size()) &&
      Unhex(record->hash, expected.data(), expected.size());
  const auto input = (legacy ? record->salt : std::string(16, '0')) + password;
  SHA256(reinterpret_cast<const unsigned char*>(input.data()), input.size(),
         actual.data());
  const bool matches =
      CRYPTO_memcmp(expected.data(), actual.data(), actual.size()) == 0;
  return legacy && matches ? replacement : std::nullopt;
}
}  // namespace DevicePassword
