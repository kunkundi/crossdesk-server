#include "admin_controller.h"
#include "log.h"

AdminHttpResponse AdminController::HandleDeviceData(
    const AdminHttpRequest& request) {
  const auto path = request.resource.substr(0, request.resource.find('?'));
  const auto token =
      AdminAuth::ExtractCookie(request.cookie, "cd_admin_session");
  if (path == "/api/admin/data-session") {
    if (request.method != "GET")
      return ErrorResponse(405, "method_not_allowed");
    const auto csrf = auth_->CsrfToken(token);
    if (!csrf) return ErrorResponse(401, "unauthorized");
    return JsonResponse(200, {{"csrf_token", *csrf}});
  }
  const auto action =
      path.substr(std::string("/api/admin/device-data/").size());
  if (action != "query" && action != "export" && action != "cleanup")
    return ErrorResponse(404, "not_found");
  if (request.method != "POST") return ErrorResponse(405, "method_not_allowed");
  if (!auth_->ValidateCsrf(token, request.csrf_token))
    return ErrorResponse(403, "invalid_csrf");
  if (request.content_type != "application/json" &&
      request.content_type != "application/json; charset=utf-8")
    return ErrorResponse(415, "json_required");
  if (request.body.size() > 4096)
    return ErrorResponse(413, "request_too_large");
  const auto body = nlohmann::json::parse(request.body, nullptr, false);
  if (!body.is_object()) return ErrorResponse(400, "invalid_json");
  for (const auto* key : {"device_id", "request_ref"})
    if (!body.contains(key) || !body[key].is_string())
      return ErrorResponse(400, "invalid_request");
  for (const auto* key :
       {"admin_password", "scope", "revision", "confirm_device_id"})
    if (body.contains(key) && !body[key].is_string())
      return ErrorResponse(400, "invalid_request");
  const auto id = body["device_id"].get<std::string>();
  const auto request_ref = body["request_ref"].get<std::string>();
  if (action != "query") {
    if (!body.contains("authorization_verified") ||
        !body["authorization_verified"].is_boolean() ||
        !body["authorization_verified"].get<bool>())
      return ErrorResponse(400, "verification_required");
    if (!auth_->Reauthenticate(token, body.value("admin_password", "")))
      return ErrorResponse(403, "reauthentication_failed");
  }
  if (action == "cleanup") {
    if (body.value("confirm_device_id", "") != id)
      return ErrorResponse(400, "confirmation_mismatch");
    // This handler and every signaling mutation run on the application writer.
    // Check live handles as well as persistent state, including controller
    // clones.
    if (transmission_ && (!transmission_->GetWsHandle(id).expired() ||
                          !transmission_->GetWsHandle("C-" + id).expired()))
      return ErrorResponse(409, "device_busy");
  }
  if (!db_) return ErrorResponse(503, "database_unavailable");
  try {
    auto result = db_->AdminDeviceData(id, action, body.value("scope", ""),
                                       body.value("revision", ""),
                                       auth_->Username(), request_ref);
    if (!result.value("ok", false)) {
      const auto error = result.value("error", "internal_error");
      return ErrorResponse(error == "invalid_request"    ? 400
                           : error == "device_not_found" ? 404
                                                         : 409,
                           error);
    }
    if (action == "cleanup") {
      if (presence_) presence_->ForgetOfflineDeviceData(id);
      if (device_data_cleanup_) device_data_cleanup_(id);
      InvalidateStatsCache();
    }
    auto response = JsonResponse(200, result);
    if (action == "export") {
      // Validated ID is restricted to ASCII letters/digits/hyphens/underscores.
      response.headers.push_back(
          {"Content-Disposition",
           "attachment; filename=\"crossdesk-device-" + id + ".json\""});
      // Internal confirmation/audit fields do not belong in user-facing
      // exports.
      auto data = result["data"];
      data.erase("revision");
      data.erase("cleanup_allowed");
      response.body = data.dump(2);
    }
    return response;
  } catch (const std::bad_alloc&) {
    throw;
  } catch (...) {
    // Never log the request body, admin password, or queried records.
    LOG_ERROR(
        "Admin device data operation failed; verify state before retrying");
    return ErrorResponse(500, "data_operation_failed");
  }
}
