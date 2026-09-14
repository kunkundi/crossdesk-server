#include "log.h"

#include <atomic>
#include <spdlog/async.h>
#include <filesystem>

namespace {

std::string g_log_dir = "logs";
std::once_flag g_logger_once_flag;
std::shared_ptr<spdlog::logger> g_logger;
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

    auto now = std::chrono::system_clock::now() + std::chrono::hours(8);
    auto now_time = std::chrono::system_clock::to_time_t(now);

    std::tm tm_info;
#ifdef _WIN32
    gmtime_s(&tm_info, &now_time);
#else
    gmtime_r(&now_time, &tm_info);
#endif

    std::stringstream ss;
    ss << LOGGER_NAME;
    ss << std::put_time(&tm_info, "-%Y%m%d-%H%M%S.log");

    std::string filename = g_log_dir + "/" + ss.str();

    std::vector<spdlog::sink_ptr> sinks;
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

    try {
      if (ec) {
        throw spdlog::spdlog_ex("failed to create log directory: " +
                                ec.message());
      }
      sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          filename, 5 * 1024 * 1024, 3));
    } catch (const std::exception& e) {
      std::cerr << "Warning: file logging disabled: " << e.what() << std::endl;
    }

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
