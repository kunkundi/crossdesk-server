#include "log.h"

#include <atomic>
#include <spdlog/async.h>
#include <filesystem>
#include <regex>
#include <stdexcept>

#include "spdlog/sinks/daily_file_sink.h"

namespace {

std::string g_log_dir = "logs";
std::once_flag g_logger_once_flag;
std::shared_ptr<spdlog::logger> g_logger;
std::shared_ptr<spdlog::sinks::daily_file_sink_mt> g_file_sink;
std::atomic<bool> g_logger_created{false};

}  // namespace

void InitLogger(const std::string& log_dir) {
  if (g_logger_created.load()) {
    LOG_WARN(
        "InitLogger called after logger initialized. Ignoring log_dir: {}, "
        "using previous log_dir: {}",
        log_dir, g_log_dir);
    return;
  }

  g_log_dir = log_dir;
}

std::shared_ptr<spdlog::logger> get_logger() {
  std::call_once(g_logger_once_flag, []() {
    std::error_code ec;
    std::filesystem::create_directories(g_log_dir, ec);

    std::string filename = g_log_dir + "/" + LOGGER_NAME + ".log";

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    if (ec)
      throw spdlog::spdlog_ex("failed to create log directory: " + ec.message());
    // Append across restarts, rotate daily, and let age-based maintenance
    // remove closed files. A size/file-count cap would shorten retention.
    // File initialization must succeed: Compose does not persist stdout copies.
    g_file_sink = std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        filename, 0, 0, false, 0);
    sinks.push_back(g_file_sink);

    // Slow disks/stdout must not stall the network loop. The queue is bounded;
    // diagnostic overload drops oldest records instead of blocking signaling.
    spdlog::init_thread_pool(8192, 1);
    g_logger = std::make_shared<spdlog::async_logger>(
        LOGGER_NAME, sinks.begin(), sinks.end(), spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    g_logger->flush_on(spdlog::level::info);
    spdlog::register_logger(g_logger);
    g_logger_created.store(true);
  });

  return g_logger;
}

size_t CleanupExpiredLogs(int retention_days) {
  if (retention_days < 1 || retention_days > 3650)
    throw std::invalid_argument("Invalid log retention policy");
  get_logger();  // Publish the active file before considering old files.
  if (!g_file_sink)
    throw std::runtime_error("File logging is unavailable; retention cleanup skipped");
  namespace fs = std::filesystem;
  const auto cutoff = fs::file_time_type::clock::now() -
                      std::chrono::hours(int64_t{retention_days} * 24);
  const auto active = fs::path(g_file_sink->filename()).lexically_normal();
  static const std::regex server_name(
      R"(^crossdesk-server(_[0-9]{4}-[0-9]{2}-[0-9]{2}|-[0-9]{8}-[0-9]{6})(\.[0-9]+)?\.log$)");
  static const std::regex coturn_name(R"(^turn_[0-9]{4}-[0-9]{2}-[0-9]{2}\.log$)");
  size_t removed = 0;
  auto clean = [&](const fs::path& directory, const std::regex& pattern) {
    std::error_code ec;
    auto status = fs::symlink_status(directory, ec);
    if (ec == std::errc::no_such_file_or_directory) return;
    if (ec) throw fs::filesystem_error("Read log directory", directory, ec);
    if (!fs::is_directory(status) || fs::is_symlink(status)) return;
    for (const auto& entry : fs::directory_iterator(directory)) {
      // No recursion or symlink traversal; unrelated logs/backups are untouched.
      if (!std::regex_match(entry.path().filename().string(), pattern) ||
          entry.path().lexically_normal() == active ||
          !fs::is_regular_file(entry.symlink_status())) continue;
      if (entry.last_write_time() < cutoff && fs::remove(entry.path())) ++removed;
    }
  };
  clean(fs::path(g_log_dir), server_name);
  clean(fs::path(g_log_dir) / "coturn", coturn_name);
  return removed;
}
