#include "notification_store.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace {
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;
Statement Prepare(sqlite3* db, const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
    throw std::runtime_error("Announcement database operation failed");
  return Statement(stmt, sqlite3_finalize);
}
int Step(sqlite3_stmt* stmt) {
  const int result = sqlite3_step(stmt);
  if (result != SQLITE_ROW && result != SQLITE_DONE)
    throw std::runtime_error("Announcement database operation failed");
  return result;
}
void Bind(sqlite3_stmt* stmt, int index, const std::string& value) {
  if (sqlite3_bind_text(stmt, index, value.data(),
                        static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK)
    throw std::runtime_error("Announcement binding failed");
}
std::string Text(sqlite3_stmt* stmt, int column) {
  const auto* text = sqlite3_column_text(stmt, column);
  return text ? reinterpret_cast<const char*>(text) : "";
}
class Transaction {
 public:
  enum Mode { Read, Write };
  Transaction(sqlite3* db, Mode mode) : db_(db) {
    if (sqlite3_exec(db_, mode == Write ? "BEGIN IMMEDIATE" : "BEGIN",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error("Failed to begin notification transaction");
  }
  ~Transaction() {
    if (db_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
  }
  void Commit() {
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK)
      throw std::runtime_error("Failed to commit notification transaction");
    db_ = nullptr;
  }

 private:
  sqlite3* db_;
};
}  // namespace

NotificationStore::NotificationStore(const std::string& db_path,
                                     OpenMode mode) {
  const auto path = std::filesystem::path(db_path);
  if (mode == OpenMode::ReadWrite && !path.parent_path().empty())
    std::filesystem::create_directories(path.parent_path());
  const int flags = mode == OpenMode::ReadOnly
                        ? SQLITE_OPEN_READONLY
                        : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
  if (sqlite3_open_v2(db_path.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
    if (db_) sqlite3_close(db_);
    db_ = nullptr;
    throw std::runtime_error("Failed to open notification database");
  }
  try {
    sqlite3_busy_timeout(db_, 1000);
    if (mode == OpenMode::ReadWrite) {
      if (sqlite3_exec(db_, "PRAGMA journal_mode=WAL; PRAGMA secure_delete=ON;",
                       nullptr, nullptr, nullptr) != SQLITE_OK)
        throw std::runtime_error("Failed to configure notification database");
      InitializeSchema();
    }
  } catch (...) {
    sqlite3_close(db_);
    db_ = nullptr;
    throw;
  }
}

NotificationStore::~NotificationStore() {
  if (db_) sqlite3_close(db_);
}

void NotificationStore::SetReadDeadline(
    std::chrono::steady_clock::time_point deadline) {
  read_deadline_ = deadline;
  sqlite3_progress_handler(
      db_, 1000,
      [](void* data) -> int {
        return std::chrono::steady_clock::now() >=
               static_cast<NotificationStore*>(data)->read_deadline_;
      },
      this);
}

bool NotificationStore::ClearReadDeadline() {
  const bool expired = std::chrono::steady_clock::now() >= read_deadline_;
  sqlite3_progress_handler(db_, 0, nullptr, nullptr);
  read_deadline_ = (std::chrono::steady_clock::time_point::max)();
  return expired;
}

void NotificationStore::InitializeSchema() {
  Transaction transaction(db_, Transaction::Write);
  const char* sql =
      "CREATE TABLE IF NOT EXISTS announcements ("
      "id INTEGER PRIMARY KEY AUTOINCREMENT, revision INTEGER NOT NULL DEFAULT "
      "1,"
      "title TEXT NOT NULL, body TEXT NOT NULL, published INTEGER NOT NULL,"
      "updated_at INTEGER NOT NULL);"
      "CREATE INDEX IF NOT EXISTS idx_announcements_published "
      "ON announcements(published, updated_at DESC, id DESC);"
      // Preserve the previous catalog revision when upgrading. Keep a durable
      // counter so deleting rows cannot reuse an earlier catalog version.
      "CREATE TABLE IF NOT EXISTS announcement_catalog ("
      "id INTEGER PRIMARY KEY CHECK (id = 1), revision INTEGER NOT NULL);"
      "INSERT OR IGNORE INTO announcement_catalog(id, revision) "
      "SELECT 1, COALESCE(SUM(revision), 0) FROM announcements;"
      // Start at the revision already held by installed clients on upgrade.
      "CREATE TABLE IF NOT EXISTS announcement_public_catalog ("
      "id INTEGER PRIMARY KEY CHECK (id = 1), revision INTEGER NOT NULL);"
      "INSERT OR IGNORE INTO announcement_public_catalog SELECT id, revision "
      "FROM announcement_catalog WHERE id = 1;"
      "CREATE TABLE IF NOT EXISTS announcement_catalog_identity ("
      "id INTEGER PRIMARY KEY CHECK (id = 1), token TEXT NOT NULL);"
      "INSERT OR IGNORE INTO announcement_catalog_identity VALUES (1, lower(hex(randomblob(16))));"
      // Share the old sequence so a revision seen before a rollback or upgrade
      // always identifies the same public content.
      "DROP TRIGGER IF EXISTS announcements_catalog_insert;"
      "CREATE TRIGGER announcements_catalog_insert AFTER INSERT ON announcements BEGIN "
      "UPDATE announcement_catalog SET revision = revision + 1 WHERE id = 1;"
      "UPDATE announcement_public_catalog SET revision = "
      "(SELECT revision FROM announcement_catalog WHERE id = 1) WHERE NEW.published = 1; END;"
      "DROP TRIGGER IF EXISTS announcements_catalog_update;"
      "CREATE TRIGGER announcements_catalog_update AFTER UPDATE ON announcements BEGIN "
      "UPDATE announcement_catalog SET revision = revision + 1 WHERE id = 1;"
      "UPDATE announcement_public_catalog SET revision = "
      "(SELECT revision FROM announcement_catalog WHERE id = 1) "
      "WHERE OLD.published = 1 OR NEW.published = 1; END;"
      "DROP TRIGGER IF EXISTS announcements_catalog_delete;"
      "CREATE TRIGGER announcements_catalog_delete AFTER DELETE ON announcements BEGIN "
      "UPDATE announcement_catalog SET revision = revision + 1 WHERE id = 1;"
      "UPDATE announcement_public_catalog SET revision = "
      "(SELECT revision FROM announcement_catalog WHERE id = 1) WHERE OLD.published = 1; END;"
      // Remove obsolete device-reading data from the earlier implementation.
      "DROP TRIGGER IF EXISTS announcement_reads_identity_cleanup;"
      "DROP TRIGGER IF EXISTS announcement_reads_history_cleanup;"
      "DROP TABLE IF EXISTS announcement_reads;";
  if (sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
    throw std::runtime_error("Failed to initialize announcements");
  transaction.Commit();
}

nlohmann::json NotificationStore::List(bool include_drafts, int offset,
                                       bool summary_only) {
  std::lock_guard<std::mutex> lock(mutex_);
  Transaction snapshot(db_, Transaction::Read);
  auto generation = Prepare(
      db_, include_drafts ? "SELECT revision FROM announcement_catalog WHERE id = 1"
                          : "SELECT revision FROM announcement_public_catalog WHERE id = 1");
  if (Step(generation.get()) != SQLITE_ROW)
    throw std::runtime_error("Missing announcement catalog revision");
  const auto catalog_revision = sqlite3_column_int64(generation.get(), 0);
  // Only the two frequently requested public first pages are retained.
  auto& cached = summary_only ? public_summary_page_ : public_content_page_;
  if (!include_drafts && offset == 0 && !cached.is_null() &&
      cached.at("catalog_revision") == catalog_revision)
    return cached;
  auto count = Prepare(
      db_, "SELECT COUNT(*) FROM announcements WHERE (? OR published = 1)");
  sqlite3_bind_int(count.get(), 1, include_drafts);
  Step(count.get());
  const int total = sqlite3_column_int(count.get(), 0);
  const int limit = summary_only ? 200 : 20;
  offset = (std::max)(0, (std::min)(offset,
                                    total ? (total - 1) / limit * limit : 0));
  auto stmt = Prepare(
      db_, summary_only ? "SELECT id, revision FROM announcements WHERE (? OR "
                          "published = 1) ORDER BY id LIMIT ? OFFSET ?"
                        : "SELECT id, revision, title, body, published, "
                          "updated_at FROM announcements "
                          "WHERE (? OR published = 1) ORDER BY updated_at "
                          "DESC, id DESC LIMIT ? OFFSET ?");
  sqlite3_bind_int(stmt.get(), 1, include_drafts);
  sqlite3_bind_int(stmt.get(), 2, limit);
  sqlite3_bind_int(stmt.get(), 3, offset);
  auto items = nlohmann::json::array();
  while (Step(stmt.get()) == SQLITE_ROW) {
    nlohmann::json item = {{"id", sqlite3_column_int64(stmt.get(), 0)},
                           {"revision", sqlite3_column_int64(stmt.get(), 1)}};
    if (!summary_only) {
      item.update({{"title", Text(stmt.get(), 2)},
                   {"body", Text(stmt.get(), 3)},
                   {"published", sqlite3_column_int(stmt.get(), 4) != 0},
                   {"updated_at", sqlite3_column_int64(stmt.get(), 5)}});
    }
    items.push_back(std::move(item));
  }
  nlohmann::json page = {{"items", std::move(items)}, {"total", total},
                         {"offset", offset}, {"limit", limit},
                         {"summary_only", summary_only},
                         {"catalog_revision", catalog_revision}};
  if (!include_drafts && offset == 0) cached = page;
  return page;
}

NotificationStore::Catalog NotificationStore::PublicCatalog() {
  std::lock_guard<std::mutex> lock(mutex_);
  Transaction snapshot(db_, Transaction::Read);
  auto generation = Prepare(db_, "SELECT c.revision, i.token FROM announcement_public_catalog c "
                                 "JOIN announcement_catalog_identity i ON i.id = c.id WHERE c.id = 1");
  if (Step(generation.get()) != SQLITE_ROW)
    throw std::runtime_error("Missing announcement catalog revision");
  Catalog catalog{sqlite3_column_int64(generation.get(), 0), Text(generation.get(), 1), {}};
  auto rows = Prepare(db_, "SELECT id, revision, title, updated_at FROM announcements "
                           "WHERE published = 1 ORDER BY updated_at DESC, id DESC");
  int status = Step(rows.get());
  int offset = 0;
  do {
    auto items = nlohmann::json::array();
    while (status == SQLITE_ROW && items.size() < 32) {
      items.push_back({{"id", sqlite3_column_int64(rows.get(), 0)},
                       {"revision", sqlite3_column_int64(rows.get(), 1)},
                       {"title", Text(rows.get(), 2)},
                       {"updated_at", sqlite3_column_int64(rows.get(), 3)}});
      status = Step(rows.get());
    }
    catalog.pages.push_back(nlohmann::json{{"type", "announcement_catalog"},
        {"catalog_revision", catalog.revision}, {"offset", offset},
        {"last", status == SQLITE_DONE}, {"items", std::move(items)}}.dump());
    offset += 32;
  } while (status == SQLITE_ROW);
  return catalog;
}

nlohmann::json NotificationStore::PublicBody(int64_t id, int64_t revision) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto row = Prepare(db_, "SELECT body FROM announcements WHERE id = ? "
                          "AND revision = ? AND published = 1");
  sqlite3_bind_int64(row.get(), 1, id);
  sqlite3_bind_int64(row.get(), 2, revision);
  if (Step(row.get()) != SQLITE_ROW) return {{"error", "announcement_unavailable"}};
  return {{"body", Text(row.get(), 0)}};
}

NotificationStore::SaveResult NotificationStore::Save(
    int64_t id, int64_t revision, const std::string& title,
    const std::string& body, bool published) {
  std::lock_guard<std::mutex> lock(mutex_);
  Transaction transaction(db_, Transaction::Write);
  bool was_published = false;
  if (id != 0) {
    auto previous = Prepare(db_, "SELECT published, title, body FROM announcements "
                                 "WHERE id = ? AND revision = ?");
    sqlite3_bind_int64(previous.get(), 1, id);
    sqlite3_bind_int64(previous.get(), 2, revision);
    if (Step(previous.get()) != SQLITE_ROW)
      return {{{"ok", false}, {"error", "stale_announcement"}}, false};
    was_published = sqlite3_column_int(previous.get(), 0) != 0;
    if (was_published == published && Text(previous.get(), 1) == title &&
        Text(previous.get(), 2) == body)
      return {{{"ok", true}, {"id", id}, {"revision", revision}}, false};
  }
  auto stmt = Prepare(
      db_,
      id == 0 ? "INSERT INTO announcements(title, body, published, updated_at) "
                "VALUES(?, ?, ?, CAST(strftime('%s', 'now') AS INTEGER))"
              : "UPDATE announcements SET title = ?, body = ?, published = ?, "
                "updated_at = CAST(strftime('%s', 'now') AS INTEGER), revision "
                "= revision + 1 "
                "WHERE id = ? AND revision = ?");
  Bind(stmt.get(), 1, title);
  Bind(stmt.get(), 2, body);
  sqlite3_bind_int(stmt.get(), 3, published);
  if (id != 0) {
    sqlite3_bind_int64(stmt.get(), 4, id);
    sqlite3_bind_int64(stmt.get(), 5, revision);
  }
  Step(stmt.get());
  if (sqlite3_changes(db_) != 1)
    return {{{"ok", false}, {"error", "stale_announcement"}}, false};
  transaction.Commit();
  return {{{"ok", true}, {"id", id ? id : sqlite3_last_insert_rowid(db_)},
           {"revision", id ? revision + 1 : 1}}, was_published || published};
}

nlohmann::json NotificationStore::Delete(int64_t id, int64_t revision) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto stmt = Prepare(db_, "DELETE FROM announcements "
                           "WHERE id = ? AND revision = ? AND published = 0");
  sqlite3_bind_int64(stmt.get(), 1, id);
  sqlite3_bind_int64(stmt.get(), 2, revision);
  Step(stmt.get());
  if (sqlite3_changes(db_) != 1)
    return {{"ok", false}, {"error", "stale_announcement"}};
  return {{"ok", true}, {"id", id}};
}
