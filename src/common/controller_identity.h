/*
 * @Author: DI JUNKUN
 * @Date: 2026-10-30
 * Copyright (c) 2026 by DI JUNKUN, All Rights Reserved.
 */

#ifndef _CONTROLLER_IDENTITY_H_
#define _CONTROLLER_IDENTITY_H_

#include <string>

inline bool IsTemporaryControllerRegistration(const std::string& id) {
  return id == "web" || id == "cli";
}

inline bool IsTemporaryControllerId(const std::string& id) {
  return id.rfind("web-", 0) == 0 || id.rfind("cli-", 0) == 0;
}

#endif
