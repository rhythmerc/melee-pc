#include <cstring>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <filesystem>
#include <vector>

#include "gpu.hpp"
#include "../internal.hpp"
#include "../io.hpp"
#include "../sqlite_utils.hpp"

#include <sqlite3.h>
#include <fmt/format.h>
#if defined(AURORA_CACHE_USE_ZSTD)
#include <zstd.h>
#endif
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace aurora::webgpu {
static Module Log("aurora::gpu::cache");

static sqlite3* db;
static sqlite3_stmt* load_stmt;
static sqlite3_stmt* store_stmt;
static bool cache_broken;
static std::mutex cache_mutex;
static std::vector<XXH128_hash_t> cache_keys_used;
/* This launch's number (aurora_meta.session, one more than the last), the
 * session each row was last used in (cache.last_session), and whether this
 * launch's prune has run. */
static int64_t cache_session;
static sqlite3_stmt* touch_stmt;
static bool cache_pruned;
static CacheStats cache_counters; /* guarded by cache_mutex */
#if defined(AURORA_CACHE_USE_ZSTD)
static std::vector<uint8_t> compress_buffer;
#endif

constexpr int CACHE_SCHEMA = 2;
// % of rows pruned to trigger a full VACUUM
constexpr uint64_t VacuumPrunePercentThreshold = 25;
/* A prune keeps rows used in any of this many recent launches. Pipelines
 * built lazily for render targets the startup warm-up doesn't cover (the XR
 * eyes' multiview, MSAA and clip variants) are only used once play reaches
 * them, often after the prune; pruning to one launch's keys threw them away
 * and every launch compiled them again, mid-fight. */
constexpr int64_t KeepSessions = 8;

static std::filesystem::path cache_path() {
  return io::fs_path_from_string(g_config.cachePath) / "dawn_cache.db";
}

static void init_abort() {
  cache_broken = true;
  sqlite3_close(db);
  db = nullptr;
}

static int check(int ret) {
  if (ret != SQLITE_OK) {
    Log.error("SQLite operation failed: {}", sqlite3_errmsg(db));
  }

  return ret;
}

static bool ensure_schema_up_to_date() {
  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open schema check transaction", sqlite3_errmsg(db));
    return false;
  }

  auto ret = sqlite::exec(db, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
  if (ret != SQLITE_OK) {
    Log.error("Failed to create schema table: {}", sqlite3_errmsg(db));
    return false;
  }

  bool match = false;
  auto cmd = fmt::format("SELECT * FROM aurora_schema WHERE value = {}", CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str(), [&match](int, char**, char**) { match = true; }, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to check schema table: {}", sqlite3_errmsg(db));
    return false;
  }

  if (match) {
    return true;
  }

  cmd = fmt::format(
      R"(DROP TABLE IF EXISTS cache;
CREATE TABLE cache (
  key BLOB PRIMARY KEY NOT NULL,
  value BLOB NOT NULL,
  size INTEGER NOT NULL,
  compressed INTEGER NOT NULL,
  last_session INTEGER NOT NULL DEFAULT 0
);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
      CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to update schema: {}", sqlite3_errmsg(db));
    return false;
  }

  tx.commit();
  return true;
}

static std::optional<uint64_t> select_uint64(const char* sql);

static bool cache_init_core() {
  Log.debug("SQLite version {}", sqlite3_libversion());

  std::string file = io::fs_path_to_string(cache_path());
  Log.debug("Using dawn cache at {}", file);
  auto ret = sqlite3_open(file.c_str(), &db);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open database: {}", sqlite3_errmsg(db));
    return false;
  }

  // WAL mode + NORMAL = no need for disk syncs, consistent but not durable is fine.
  ret = sqlite::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pragmas: {}", sqlite3_errmsg(db));
    return false;
  }

  if (!ensure_schema_up_to_date()) {
    Log.error("Failed to validate schema");
    return false;
  }

  /* Caches made before last_session existed get it in place (an error here
   * just means the column is there already), and this launch takes the next
   * session number. */
  sqlite::exec(db, "ALTER TABLE cache ADD COLUMN last_session INTEGER NOT NULL DEFAULT 0;");
  ret = sqlite::exec(db, "CREATE TABLE IF NOT EXISTS aurora_meta(name TEXT PRIMARY KEY NOT NULL, value INTEGER);"
                         "INSERT OR IGNORE INTO aurora_meta VALUES ('session', 0);"
                         "UPDATE aurora_meta SET value = value + 1 WHERE name = 'session';");
  if (ret != SQLITE_OK) {
    Log.error("Failed to advance the cache session: {}", sqlite3_errmsg(db));
    return false;
  }
  const auto session = select_uint64("SELECT value FROM aurora_meta WHERE name = 'session'");
  if (!session) {
    return false;
  }
  cache_session = static_cast<int64_t>(*session);

  ret = sqlite3_prepare_v3(db, "SELECT value, size, compressed FROM cache WHERE key = ?", -1, SQLITE_PREPARE_PERSISTENT,
                           &load_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db, "REPLACE INTO cache (key, value, size, compressed, last_session) VALUES (?, ?, ?, ?, ?)",
                           -1, SQLITE_PREPARE_PERSISTENT, &store_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db, "UPDATE cache SET last_session = ? WHERE key = ?", -1, SQLITE_PREPARE_PERSISTENT,
                           &touch_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  return true;
}

static std::optional<uint64_t> select_uint64(const char* sql) {
  sqlite3_stmt* stmt = nullptr;
  auto ret = sqlite3_prepare_v3(db, sql, -1, 0, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement '{}': {}", sql, sqlite3_errmsg(db));
    return std::nullopt;
  }

  std::optional<uint64_t> result;
  ret = sqlite3_step(stmt);
  if (ret == SQLITE_ROW) {
    result = static_cast<uint64_t>(sqlite3_column_int64(stmt, 0));
  } else if (ret != SQLITE_DONE) {
    Log.error("Failed to execute statement '{}': {}", sql, sqlite3_errmsg(db));
  }

  sqlite3_finalize(stmt);
  return result;
}

static bool fill_used_key_table(const std::vector<XXH128_hash_t>& usedKeys) {
  auto ret = sqlite::exec(db,
                          "CREATE TEMP TABLE IF NOT EXISTS cache_keys_used ("
                          "key BLOB PRIMARY KEY NOT NULL"
                          ");"
                          "DELETE FROM cache_keys_used;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare dawn cache prune key table: {}", sqlite3_errmsg(db));
    return false;
  }

  sqlite3_stmt* stmt = nullptr;
  ret = sqlite3_prepare_v3(db, "INSERT OR IGNORE INTO cache_keys_used (key) VALUES (?)", -1, 0, &stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare dawn cache prune key insert: {}", sqlite3_errmsg(db));
    return false;
  }

  for (const auto& keyHash : usedKeys) {
    ret = sqlite3_bind_blob(stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT);
    if (ret != SQLITE_OK) {
      Log.error("Failed to bind dawn cache prune key: {}", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return false;
    }

    ret = sqlite3_step(stmt);
    if (ret != SQLITE_DONE) {
      Log.error("Failed to insert dawn cache prune key: {}", sqlite3_errmsg(db));
      sqlite3_finalize(stmt);
      return false;
    }

    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
  }

  sqlite3_finalize(stmt);
  return true;
}

static bool cache_init() {
  if (cache_broken) {
    return false;
  }

  if (db) {
    return true;
  }

  if (!cache_init_core()) {
    Log.error("SQLite DB init failed");
    init_abort();
    return false;
  }

  Log.debug("SQLite cache init succeeded");

  return true;
}

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void*) {
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return 0;
  }

  sqlite::Transaction tx(db, Log);
  if (!tx) {
    Log.error("Failed to open load transaction");
    return 0;
  }

  const auto keyHash = XXH128(key, keySize, 0);
  check(sqlite3_bind_blob(load_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));

  const auto ret = sqlite3_step(load_stmt);
  size_t foundSize;
  bool loadSucceeded = false;
  if (ret == SQLITE_ROW) {
    // Hit
    const auto foundPtr = sqlite3_column_blob(load_stmt, 0);
    foundSize = sqlite3_column_int64(load_stmt, 1);
    const bool compressed = sqlite3_column_int(load_stmt, 2) != 0;

    if (value && valueSize == foundSize) {
      loadSucceeded = true;
      if (compressed) {
#if defined(AURORA_CACHE_USE_ZSTD)
        const auto compSize = sqlite3_column_bytes(load_stmt, 0);
        const auto zstdRet = ZSTD_decompress(value, valueSize, foundPtr, compSize);
        if (ZSTD_isError(zstdRet)) {
          Log.error("zstd decompression error: {}", ZSTD_getErrorName(zstdRet));
          foundSize = 0;
          loadSucceeded = false;
        } else if (zstdRet != foundSize) {
          Log.error("zstd decompression size mismatch: expected {}, got {}", foundSize, zstdRet);
          foundSize = 0;
          loadSucceeded = false;
        }
#else
        Log.error("Cache entry is zstd-compressed but zstd support is disabled");
        foundSize = 0;
        loadSucceeded = false;
#endif
      } else {
        if (foundSize != 0 && !foundPtr) {
          Log.error("Cache entry is missing raw value data");
          foundSize = 0;
          loadSucceeded = false;
        } else if (foundSize != 0) {
          std::memcpy(value, foundPtr, foundSize);
        }
      }
    }
  } else if (ret == SQLITE_DONE) {
    // Miss
    foundSize = 0;
    ++cache_counters.misses;
  } else {
    Log.error("Looking up cache key failed: {}", sqlite3_errmsg(db));
    return 0;
  }

  check(sqlite3_reset(load_stmt));

  if (loadSucceeded && cache_pruned) {
    /* Used after this launch's prune: mark it here, as the prune does for
     * the keys before it. */
    check(sqlite3_bind_int64(touch_stmt, 1, cache_session));
    check(sqlite3_bind_blob(touch_stmt, 2, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
    if (sqlite3_step(touch_stmt) != SQLITE_DONE) {
      Log.error("Failed to mark cache key used: {}", sqlite3_errmsg(db));
    }
    check(sqlite3_reset(touch_stmt));
    tx.commit();
  }

  if (loadSucceeded) {
    cache_keys_used.push_back(keyHash);
    /* Dawn asks for the size first (value == nullptr), then for the data;
     * only the second call is a hit. */
    ++cache_counters.hits;
    cache_counters.hitBytes += foundSize;
  }

  return foundSize;
}

void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void*) {
  std::lock_guard lock(cache_mutex);

  if (!cache_init()) {
    return;
  }

  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open store transaction");
    return;
  }

  const void* storedValue = value;
  sqlite3_uint64 storedValueSize = valueSize;
  int compressed = 0;
#if defined(AURORA_CACHE_USE_ZSTD)
  const auto bound = ZSTD_compressBound(valueSize);
  if (ZSTD_isError(bound)) {
    Log.error("Failed to calculate ZSTD_compressBound: {}", ZSTD_getErrorName(bound));
    return;
  }

  if (compress_buffer.size() < bound) {
    compress_buffer.resize(bound);
  }

  const auto compressRet = ZSTD_compress(compress_buffer.data(), compress_buffer.size(), value, valueSize, 0);
  if (ZSTD_isError(compressRet)) {
    Log.error("ZSTD compression error: {}", ZSTD_getErrorName(compressRet));
    return;
  }

  if (compressRet < valueSize) {
    storedValue = compress_buffer.data();
    storedValueSize = compressRet;
    compressed = 1;
  }
#endif

  const auto keyHash = XXH128(key, keySize, 0);
  check(sqlite3_bind_blob64(store_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
  check(
      sqlite3_bind_blob64(store_stmt, 2, storedValue, storedValueSize, compressed ? SQLITE_STATIC : SQLITE_TRANSIENT));
  check(sqlite3_bind_int64(store_stmt, 3, static_cast<sqlite3_int64>(valueSize)));
  check(sqlite3_bind_int(store_stmt, 4, compressed));
  check(sqlite3_bind_int64(store_stmt, 5, cache_session));

  const auto ret = sqlite3_step(store_stmt);
  if (ret != SQLITE_DONE) {
    // Error or something
    Log.error("Failed to insert row: {}", sqlite3_errmsg(db));
    return;
  }

  check(sqlite3_reset(store_stmt));
  check(sqlite3_bind_null(store_stmt, 2));
  check(sqlite3_bind_null(store_stmt, 4));

  tx.commit();
  cache_keys_used.push_back(keyHash);
  ++cache_counters.stores;
  cache_counters.storeBytes += valueSize;
}

CacheStats cache_stats() {
  std::lock_guard lock(cache_mutex);
  return cache_counters;
}

void cache_prune() {
  std::lock_guard lock(cache_mutex);
  if (!cache_init()) {
    return;
  }
  cache_pruned = true;
  if (cache_keys_used.empty()) {
    Log.warn("Skipping Dawn cache prune because no cache keys were used");
    return;
  }

  uint64_t totalRows = 0;
  sqlite3_int64 deletedRows = 0;
  {
    sqlite::Transaction tx(db, Log, true);
    if (!tx) {
      Log.error("Failed to open dawn cache prune transaction");
      return;
    }

    if (!fill_used_key_table(cache_keys_used)) {
      return;
    }

    const auto totalRowsResult = select_uint64("SELECT COUNT(*) FROM cache");
    if (!totalRowsResult) {
      return;
    }
    totalRows = *totalRowsResult;

    auto ret = sqlite::exec(db, fmt::format("UPDATE cache SET last_session = {} "
                                            "WHERE key IN (SELECT key FROM cache_keys_used)",
                                            cache_session)
                                    .c_str());
    if (ret != SQLITE_OK) {
      Log.error("Failed to mark used dawn cache rows: {}", sqlite3_errmsg(db));
      return;
    }
    ret = sqlite::exec(db,
                       fmt::format("DELETE FROM cache WHERE last_session <= {}", cache_session - KeepSessions).c_str());
    if (ret != SQLITE_OK) {
      Log.error("Failed to prune dawn cache rows: {}", sqlite3_errmsg(db));
      return;
    }
    deletedRows = sqlite3_changes(db);

    tx.commit();
  }

  if (deletedRows == 0) {
    Log.debug("Dawn cache prune completed; no stale entries found");
    return;
  }

  Log.info("Pruned {} stale Dawn cache entries", deletedRows);

  // VACUUM if we removed at least 25% of the rows
  if (totalRows != 0 && static_cast<uint64_t>(deletedRows) * 100ull >= totalRows * VacuumPrunePercentThreshold) {
    if (const auto ret = sqlite::exec(db, "VACUUM;"); ret != SQLITE_OK) {
      Log.warn("Failed to vacuum dawn cache after pruning: {}", sqlite3_errmsg(db));
      return;
    }
  }

  if (const auto ret = sqlite::exec(db, "PRAGMA wal_checkpoint(TRUNCATE);"); ret != SQLITE_OK) {
    Log.warn("Failed to checkpoint dawn cache WAL: {}", sqlite3_errmsg(db));
  }
}

void cache_shutdown() {
#if defined(AURORA_CACHE_USE_ZSTD)
  compress_buffer.clear();
#endif
  cache_keys_used.clear();
  cache_counters = {};
  if (load_stmt != nullptr) {
    check(sqlite3_finalize(load_stmt));
    load_stmt = nullptr;
  }
  if (store_stmt != nullptr) {
    check(sqlite3_finalize(store_stmt));
    store_stmt = nullptr;
  }
  if (touch_stmt != nullptr) {
    check(sqlite3_finalize(touch_stmt));
    touch_stmt = nullptr;
  }
  cache_pruned = false;
  if (db != nullptr) {
    check(sqlite3_close(db));
    db = nullptr;
  }
}

} // namespace aurora::webgpu
