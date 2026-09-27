/*
 * @Author: DI JUNKUN
 * @Date: 2025-09-08
 * Copyright (c) 2025 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _MAIN_H_
#define _MAIN_H_

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

#include "log/log.h"
#include "signal_server.h"

namespace {

uint16_t ParsePort(const std::string& port) {
  size_t parsed_len = 0;
  int value = 0;

  try {
    value = std::stoi(port, &parsed_len);
  } catch (const std::exception&) {
    throw std::invalid_argument("Invalid port: " + port);
  }

  if (parsed_len != port.size() || value < 1 || value > 65535) {
    throw std::out_of_range("Port must be an integer in range 1-65535: " +
                            port);
  }

  return static_cast<uint16_t>(value);
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string port = "9090";
  std::string log_dir = "/var/log/crossdesk";
  std::string certs_dir = "/var/lib/crossdesk/certs";
  std::string db_path = "/var/lib/crossdesk/db/crossdesk-server.db";

  if (argc > 1) {
    port = argv[1];
  }

  InitLogger(log_dir);

  try {
    SignalServer s(ParsePort(port), certs_dir, db_path);
    s.Run();
  } catch (std::exception& e) {
    std::cerr << "Fatal error: " << e.what() << std::endl;
    try {
      LOG_ERROR("Fatal error: {}", e.what());
    } catch (...) {
      // The logger itself may have failed to initialize its required file.
    }
    return 1;
  } catch (...) {
    std::cerr << "Unknown fatal error occurred" << std::endl;
    try {
      LOG_ERROR("Unknown fatal error occurred");
    } catch (...) {
    }
    return 1;
  }

  return 0;
}

#endif
