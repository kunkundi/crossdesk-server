#include "notification_service.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace {
NotificationResult ErrorResponse(int status, const std::string& error) {
  return {status, {{"ok", false}, {"error", error}}};
}
}  // namespace

bool NotificationService::IsAdminRoute(const std::string& resource) {
  return resource.substr(0, resource.find('?')) == "/api/admin/announcements";
}

bool NotificationService::IsClientMessage(const std::string& type) {
  return type == "announcements_list";
}

NotificationResult NotificationService::HandleAdminRequest(
    const std::string& method, const std::string& resource,
    const std::string& request_body) {
  if (!IsAdminRoute(resource)) return ErrorResponse(404, "not_found");
  if (method == "GET") {
    int offset = 0;
    const auto query = resource.find('?');
    if (query != std::string::npos) {
      const auto value = resource.substr(query + 1);
      if (value.rfind("offset=", 0) != 0)
        return ErrorResponse(400, "invalid_request");
      try {
        size_t consumed = 0;
        offset = std::stoi(value.substr(7), &consumed);
        if (consumed != value.size() - 7 || offset < 0)
          return ErrorResponse(400, "invalid_request");
      } catch (...) {
        return ErrorResponse(400, "invalid_request");
      }
    }
    return {200, store_.List(true, offset)};
  }
  if (method != "POST") return ErrorResponse(405, "method_not_allowed");
  if (request_body.size() > 16 * 1024)
    return ErrorResponse(413, "request_too_large");
  const auto body = nlohmann::json::parse(request_body, nullptr, false);
  if (!body.is_object()) return ErrorResponse(400, "invalid_json");
  if (body.contains("action") &&
      (!body["action"].is_string() ||
       (body["action"] != "save" && body["action"] != "delete")))
    return ErrorResponse(400, "invalid_request");
  for (const auto* key : {"id", "revision"})
    if (!body.contains(key) || !body[key].is_number_integer() ||
        body[key] < 0 || body[key] > 2147483647)
      return ErrorResponse(400, "invalid_request");
  if (body.value("action", "save") == "delete") {
    if (body["id"] == 0 || body["revision"] == 0)
      return ErrorResponse(400, "invalid_request");
    const auto result = store_.Delete(body["id"], body["revision"]);
    if (!result.value("ok", false))
      return ErrorResponse(409, "stale_announcement");
    if (broadcast_) broadcast_({{"type", "announcements_changed"}});
    return {200, result};
  }
  for (const auto* key : {"title", "body"}) {
    if (!body.contains(key) || !body[key].is_string())
      return ErrorResponse(400, "invalid_request");
    const auto value = body[key].get<std::string>();
    if (value.empty() || value.find('\0') != std::string::npos ||
        std::all_of(value.begin(), value.end(),
                    [](unsigned char c) { return std::isspace(c); }) ||
        value.size() > (std::string(key) == "title" ? 240u : 8000u))
      return ErrorResponse(400, "invalid_announcement_text");
  }
  if (!body.contains("published") || !body["published"].is_boolean() ||
      (body["id"] == 0 ? body["revision"] != 0 : body["revision"] == 0))
    return ErrorResponse(400, "invalid_request");
  const auto result = store_.Save(body["id"], body["revision"], body["title"],
                                  body["body"], body["published"]);
  if (!result.value("ok", false))
    return ErrorResponse(409, "stale_announcement");
  if (broadcast_) broadcast_({{"type", "announcements_changed"}});
  return {200, result};
}

std::optional<nlohmann::json> NotificationService::HandleClientMessage(
    const nlohmann::json& message) {
  if (!message.is_object() || !message.contains("type") ||
      !message["type"].is_string() || !IsClientMessage(message["type"]))
    return std::nullopt;
  if (!message.contains("request_id") || !message["request_id"].is_string() ||
      message["request_id"].get_ref<const std::string&>().size() > 80)
    return std::nullopt;
  if (!message.contains("offset") || !message["offset"].is_number_integer() ||
      message["offset"] < 0 || message["offset"] > 2147483647)
    return std::nullopt;
  if (message.contains("summary_only") && !message["summary_only"].is_boolean())
    return std::nullopt;
  nlohmann::json response = {{"type", "announcements"},
                             {"request_id", message["request_id"]}};
  try {
    response.update(store_.List(false, message["offset"],
                                message.value("summary_only", false)));
  } catch (const std::runtime_error&) {
    response["error"] = "announcement_operation_failed";
  }
  return response;
}
