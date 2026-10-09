/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-09
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _DEVICE_PASSWORD_H_
#define _DEVICE_PASSWORD_H_

#include <optional>
#include <string>

// Stored in the existing salt/hash columns. The hash identifies the algorithm,
// version and work factors; unrecognized formats never fall back to SHA-256.
struct DevicePasswordRecord {
  std::string salt;
  std::string hash;
  bool operator==(const DevicePasswordRecord& other) const {
    return salt == other.salt && hash == other.hash;
  }
};

namespace DevicePassword {
void CheckSupport();
std::string Generate();
std::optional<DevicePasswordRecord> Hash(const std::string& password);
// One Argon2id calculation for current, legacy, missing and malformed records.
// A successful legacy check returns a replacement record for atomic migration.
std::optional<DevicePasswordRecord> Verify(
    const std::optional<DevicePasswordRecord>& record,
    const std::string& password);
bool ValidInput(const std::string& password);
bool IsCurrent(const DevicePasswordRecord& record);
}  // namespace DevicePassword

#endif