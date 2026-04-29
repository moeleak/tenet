#include "bot_memory.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error_size > 0) {
        snprintf(error, error_size, "%s", message != NULL ? message : "未知错误");
    }
}

static void set_sqlite_error(bot_memory_t *memory, char *error, size_t error_size, const char *prefix)
{
    if (error_size > 0) {
        snprintf(error, error_size, "%s: %s", prefix, sqlite3_errmsg(memory->db));
    }
}

static int exec_sql(bot_memory_t *memory, const char *sql, char *error, size_t error_size)
{
    char *errmsg = NULL;

    if (sqlite3_exec(memory->db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        if (error_size > 0) {
            snprintf(error, error_size, "SQLite: %s", errmsg != NULL ? errmsg : sqlite3_errmsg(memory->db));
        }
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

static int try_load_extension(sqlite3 *db, const char *path)
{
    char *errmsg = NULL;
    int rc;

    if (path == NULL || *path == '\0') {
        return -1;
    }
    rc = sqlite3_load_extension(db, path, NULL, &errmsg);
    sqlite3_free(errmsg);
    return rc == SQLITE_OK ? 0 : -1;
}

static int load_vss_extensions(bot_memory_t *memory,
                               const bot_config_t *config,
                               char *error,
                               size_t error_size)
{
    const char *vector_names[] = {"vector0", "libvector0", NULL};
    const char *vss_names[] = {"vss0", "libvss0", NULL};
    int loaded_vss = 0;
    int i;

    sqlite3_enable_load_extension(memory->db, 1);
    if (config->vector_extension_path[0] != '\0') {
        if (try_load_extension(memory->db, config->vector_extension_path) != 0) {
            snprintf(error, error_size, "加载 vector0 扩展失败: %s", config->vector_extension_path);
            return -1;
        }
    } else {
        for (i = 0; vector_names[i] != NULL; i++) {
            if (try_load_extension(memory->db, vector_names[i]) == 0) {
                break;
            }
        }
    }
    if (config->vss_extension_path[0] != '\0') {
        loaded_vss = try_load_extension(memory->db, config->vss_extension_path) == 0;
    } else {
        for (i = 0; vss_names[i] != NULL; i++) {
            if (try_load_extension(memory->db, vss_names[i]) == 0) {
                loaded_vss = 1;
                break;
            }
        }
    }
    if (!loaded_vss) {
        if (config->vss_extension_path[0] != '\0') {
            snprintf(error, error_size, "加载 vss0 扩展失败: %s", config->vss_extension_path);
            return -1;
        }
        return 0;
    }
    return 1;
}

static int init_schema(bot_memory_t *memory, char *error, size_t error_size)
{
    const char *sql =
        "PRAGMA journal_mode=WAL;"
        "CREATE TABLE IF NOT EXISTS metadata("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS messages("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  speaker TEXT NOT NULL,"
        "  role TEXT NOT NULL,"
        "  target_user TEXT NOT NULL DEFAULT '',"
        "  content TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS memory_items("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  scope TEXT NOT NULL,"
        "  username TEXT NOT NULL DEFAULT '',"
        "  content TEXT NOT NULL,"
        "  embedding_json TEXT NOT NULL DEFAULT '',"
        "  embedding_dim INTEGER NOT NULL DEFAULT 0,"
        "  source_message_id INTEGER,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS summaries("
        "  scope TEXT NOT NULL,"
        "  username TEXT NOT NULL DEFAULT '',"
        "  content TEXT NOT NULL DEFAULT '',"
        "  updated_message_id INTEGER NOT NULL DEFAULT 0,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  PRIMARY KEY(scope, username)"
        ");"
        "CREATE TABLE IF NOT EXISTS seen_messages("
        "  fingerprint TEXT PRIMARY KEY,"
        "  seen_at INTEGER NOT NULL"
        ");";

    return exec_sql(memory, sql, error, error_size);
}

static void migrate_schema(bot_memory_t *memory)
{
    char *errmsg = NULL;

    if (sqlite3_exec(memory->db, "ALTER TABLE memory_items ADD COLUMN embedding_json TEXT NOT NULL DEFAULT ''", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        errmsg = NULL;
    }
    if (sqlite3_exec(memory->db, "ALTER TABLE memory_items ADD COLUMN embedding_dim INTEGER NOT NULL DEFAULT 0", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
    }
}

static int load_vector_dim(bot_memory_t *memory)
{
    sqlite3_stmt *stmt = NULL;
    int dim = 0;

    if (sqlite3_prepare_v2(memory->db,
                           "SELECT value FROM metadata WHERE key='embedding_dim'",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        dim = atoi((const char *)sqlite3_column_text(stmt, 0));
    }
    sqlite3_finalize(stmt);
    return dim;
}

static int ensure_vss_table(bot_memory_t *memory, size_t embedding_dim, char *error, size_t error_size)
{
    char sql[256];
    sqlite3_stmt *stmt = NULL;

    if (embedding_dim == 0 || embedding_dim > 100000) {
        set_error(error, error_size, "embedding 维度无效");
        return -1;
    }
    if (memory->vector_dim > 0 && memory->vector_dim != (int)embedding_dim) {
        snprintf(error, error_size, "embedding 维度变化: 数据库=%d 当前=%zu",
                 memory->vector_dim, embedding_dim);
        return -1;
    }
    if (memory->vector_dim != (int)embedding_dim) {
        if (sqlite3_prepare_v2(memory->db,
                               "INSERT OR REPLACE INTO metadata(key, value) VALUES('embedding_dim', ?)",
                               -1, &stmt, NULL) != SQLITE_OK) {
            set_sqlite_error(memory, error, error_size, "保存 embedding 维度失败");
            return -1;
        }
        sqlite3_bind_int(stmt, 1, (int)embedding_dim);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            set_sqlite_error(memory, error, error_size, "保存 embedding 维度失败");
            return -1;
        }
        sqlite3_finalize(stmt);
        memory->vector_dim = (int)embedding_dim;
    }
    if (!memory->vss_enabled) {
        return 0;
    }
    snprintf(sql, sizeof(sql),
             "CREATE VIRTUAL TABLE IF NOT EXISTS memory_vectors USING vss0(embedding(%zu))",
             embedding_dim);
    return exec_sql(memory, sql, error, error_size);
}

int bot_memory_open(bot_memory_t *memory,
                    const bot_config_t *config,
                    char *error,
                    size_t error_size)
{
    int vss_rc;

    memset(memory, 0, sizeof(*memory));
    if (config->reset_memory) {
        char sidecar[1024];

        (void)unlink(config->memory_db_path);
        snprintf(sidecar, sizeof(sidecar), "%s-wal", config->memory_db_path);
        (void)unlink(sidecar);
        snprintf(sidecar, sizeof(sidecar), "%s-shm", config->memory_db_path);
        (void)unlink(sidecar);
    }
    if (sqlite3_open(config->memory_db_path, &memory->db) != SQLITE_OK) {
        set_sqlite_error(memory, error, error_size, "打开 SQLite 记忆库失败");
        bot_memory_close(memory);
        return -1;
    }
    vss_rc = load_vss_extensions(memory, config, error, error_size);
    if (vss_rc < 0) {
        bot_memory_close(memory);
        return -1;
    }
    memory->vss_enabled = vss_rc > 0;
    if (init_schema(memory, error, error_size) != 0) {
        bot_memory_close(memory);
        return -1;
    }
    migrate_schema(memory);
    memory->vector_dim = load_vector_dim(memory);
    if (memory->vector_dim > 0) {
        if (ensure_vss_table(memory, (size_t)memory->vector_dim, error, error_size) != 0) {
            bot_memory_close(memory);
            return -1;
        }
    }
    return 0;
}

void bot_memory_close(bot_memory_t *memory)
{
    if (memory->db != NULL) {
        sqlite3_close(memory->db);
    }
    memory->db = NULL;
    memory->vector_dim = 0;
    memory->vss_enabled = 0;
}

int bot_memory_is_seen(bot_memory_t *memory, const char *fingerprint)
{
    sqlite3_stmt *stmt = NULL;
    int seen = 0;

    if (sqlite3_prepare_v2(memory->db,
                           "SELECT 1 FROM seen_messages WHERE fingerprint=?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, fingerprint, -1, SQLITE_TRANSIENT);
    seen = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return seen;
}

int bot_memory_mark_seen(bot_memory_t *memory, const char *fingerprint)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (sqlite3_prepare_v2(memory->db,
                           "INSERT OR IGNORE INTO seen_messages(fingerprint, seen_at) VALUES(?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, fingerprint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)time(NULL));
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int insert_message(bot_memory_t *memory,
                          const char *speaker,
                          const char *role,
                          const char *target_user,
                          const char *content)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (sqlite3_prepare_v2(memory->db,
                           "INSERT INTO messages(speaker, role, target_user, content, created_at) "
                           "VALUES(?, ?, ?, ?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, speaker, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, target_user != NULL ? target_user : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int bot_memory_store_exchange(bot_memory_t *memory,
                              const char *sender,
                              const char *question,
                              const char *answer,
                              char *error,
                              size_t error_size)
{
    if (exec_sql(memory, "BEGIN", error, error_size) != 0) {
        return -1;
    }
    if (insert_message(memory, sender, "user", sender, question) != 0 ||
        insert_message(memory, "tenet-bot", "assistant", sender, answer) != 0) {
        (void)exec_sql(memory, "ROLLBACK", NULL, 0);
        set_sqlite_error(memory, error, error_size, "保存问答失败");
        return -1;
    }
    if (exec_sql(memory, "COMMIT", error, error_size) != 0) {
        return -1;
    }
    return 0;
}

int bot_memory_add_item(bot_memory_t *memory,
                        const char *scope,
                        const char *username,
                        const char *content,
                        const char *embedding_json,
                        size_t embedding_dim,
                        char *error,
                        size_t error_size)
{
    sqlite3_stmt *stmt = NULL;
    sqlite3_int64 rowid;

    if (ensure_vss_table(memory, embedding_dim, error, error_size) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(memory->db,
                           "INSERT INTO memory_items(scope, username, content, embedding_json, embedding_dim, created_at) "
                           "VALUES(?, ?, ?, ?, ?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(memory, error, error_size, "写入记忆条目失败");
        return -1;
    }
    sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, content, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, embedding_json != NULL ? embedding_json : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, (int)embedding_dim);
    sqlite3_bind_int64(stmt, 6, (sqlite3_int64)time(NULL));
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        set_sqlite_error(memory, error, error_size, "写入记忆条目失败");
        return -1;
    }
    sqlite3_finalize(stmt);
    rowid = sqlite3_last_insert_rowid(memory->db);

    if (!memory->vss_enabled) {
        return 0;
    }
    if (sqlite3_prepare_v2(memory->db,
                           "INSERT INTO memory_vectors(rowid, embedding) VALUES(?, ?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(memory, error, error_size, "写入记忆向量失败");
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, rowid);
    sqlite3_bind_text(stmt, 2, embedding_json, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        set_sqlite_error(memory, error, error_size, "写入记忆向量失败");
        return -1;
    }
    sqlite3_finalize(stmt);
    return 0;
}

static int append_item_by_id(bot_memory_t *memory,
                             sqlite3_int64 rowid,
                             const char *scope,
                             const char *username,
                             bot_str_t *out,
                             int *count)
{
    sqlite3_stmt *stmt = NULL;
    int matched = 0;

    if (sqlite3_prepare_v2(memory->db,
                           "SELECT content FROM memory_items WHERE id=? AND scope=? "
                           "AND (?='' OR username=?)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, rowid);
    sqlite3_bind_text(stmt, 2, scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        if (content != NULL && *content != '\0') {
            if (*count == 0) {
                if (bot_str_append(out, "\n相关长期记忆:\n") != 0) {
                    sqlite3_finalize(stmt);
                    return -1;
                }
            }
            if (bot_str_appendf(out, "- %s\n", content) != 0) {
                sqlite3_finalize(stmt);
                return -1;
            }
            (*count)++;
            matched = 1;
        }
    }
    sqlite3_finalize(stmt);
    return matched;
}

static int parse_vector_json(const char *json, double **values_out, size_t *count_out)
{
    const char *cursor = json;
    double *values = NULL;
    size_t count = 0;
    size_t cap = 0;

    *values_out = NULL;
    *count_out = 0;
    while (*cursor != '\0' && *cursor != '[') {
        cursor++;
    }
    if (*cursor != '[') {
        return -1;
    }
    cursor++;
    for (;;) {
        char *endptr;
        double value;

        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            cursor++;
        }
        if (*cursor == ']') {
            break;
        }
        value = strtod(cursor, &endptr);
        if (endptr == cursor) {
            free(values);
            return -1;
        }
        if (count == cap) {
            double *next;
            cap = cap > 0 ? cap * 2 : 512;
            next = realloc(values, cap * sizeof(values[0]));
            if (next == NULL) {
                free(values);
                return -1;
            }
            values = next;
        }
        values[count++] = value;
        cursor = endptr;
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
            cursor++;
        }
        if (*cursor == ',') {
            cursor++;
            continue;
        }
        if (*cursor == ']') {
            break;
        }
        free(values);
        return -1;
    }
    if (count == 0) {
        free(values);
        return -1;
    }
    *values_out = values;
    *count_out = count;
    return 0;
}

typedef struct fallback_hit {
    double score;
    char *content;
} fallback_hit_t;

static double cosine_score(const double *left, const double *right, size_t count)
{
    double dot = 0.0;
    double left_norm = 0.0;
    double right_norm = 0.0;
    size_t i;

    for (i = 0; i < count; i++) {
        dot += left[i] * right[i];
        left_norm += left[i] * left[i];
        right_norm += right[i] * right[i];
    }
    if (left_norm <= 0.0 || right_norm <= 0.0) {
        return -1.0;
    }
    return dot / (sqrt(left_norm) * sqrt(right_norm));
}

static void fallback_hits_free(fallback_hit_t *hits, int count)
{
    int i;

    for (i = 0; i < count; i++) {
        free(hits[i].content);
    }
    free(hits);
}

static int fallback_hits_insert(fallback_hit_t *hits, int top_k, double score, const char *content)
{
    int pos;
    int last;

    if (top_k <= 0 || content == NULL || *content == '\0') {
        return 0;
    }
    for (pos = 0; pos < top_k; pos++) {
        if (hits[pos].content == NULL || score > hits[pos].score) {
            break;
        }
    }
    if (pos >= top_k) {
        return 0;
    }
    free(hits[top_k - 1].content);
    last = top_k - 1;
    while (last > pos) {
        hits[last] = hits[last - 1];
        last--;
    }
    hits[pos].score = score;
    hits[pos].content = bot_strdup_safe(content);
    return hits[pos].content != NULL ? 0 : -1;
}

static int bot_memory_search_scan(bot_memory_t *memory,
                                  const char *scope,
                                  const char *username,
                                  const char *embedding_json,
                                  int top_k,
                                  bot_str_t *out,
                                  char *error,
                                  size_t error_size)
{
    sqlite3_stmt *stmt = NULL;
    double *query = NULL;
    size_t query_count = 0;
    fallback_hit_t *hits;
    int i;
    int appended = 0;

    if (parse_vector_json(embedding_json, &query, &query_count) != 0) {
        set_error(error, error_size, "解析查询向量失败");
        return -1;
    }
    hits = calloc((size_t)top_k, sizeof(hits[0]));
    if (hits == NULL) {
        free(query);
        set_error(error, error_size, "分配检索结果失败");
        return -1;
    }
    if (sqlite3_prepare_v2(memory->db,
                           "SELECT content, embedding_json FROM memory_items "
                           "WHERE scope=? AND (?='' OR username=?) AND embedding_dim=? AND embedding_json<>''",
                           -1, &stmt, NULL) != SQLITE_OK) {
        free(query);
        fallback_hits_free(hits, top_k);
        set_sqlite_error(memory, error, error_size, "准备扫描检索失败");
        return -1;
    }
    sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, (int)query_count);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        const char *stored_json = (const char *)sqlite3_column_text(stmt, 1);
        double *stored = NULL;
        size_t stored_count = 0;

        if (stored_json != NULL && parse_vector_json(stored_json, &stored, &stored_count) == 0 &&
            stored_count == query_count) {
            double score = cosine_score(query, stored, query_count);
            if (fallback_hits_insert(hits, top_k, score, content != NULL ? content : "") != 0) {
                free(stored);
                sqlite3_finalize(stmt);
                free(query);
                fallback_hits_free(hits, top_k);
                set_error(error, error_size, "保存扫描检索结果失败");
                return -1;
            }
        }
        free(stored);
    }
    sqlite3_finalize(stmt);
    free(query);

    for (i = 0; i < top_k; i++) {
        if (hits[i].content == NULL) {
            continue;
        }
        if (!appended) {
            if (bot_str_append(out, "\n相关长期记忆:\n") != 0) {
                fallback_hits_free(hits, top_k);
                return -1;
            }
            appended = 1;
        }
        if (bot_str_appendf(out, "- %s\n", hits[i].content) != 0) {
            fallback_hits_free(hits, top_k);
            return -1;
        }
    }
    fallback_hits_free(hits, top_k);
    return 0;
}

int bot_memory_search(bot_memory_t *memory,
                      const char *scope,
                      const char *username,
                      const char *embedding_json,
                      int top_k,
                      bot_str_t *out,
                      char *error,
                      size_t error_size)
{
    sqlite3_stmt *stmt = NULL;
    int wanted;
    int count = 0;

    if (top_k <= 0 || memory->vector_dim <= 0 || embedding_json == NULL || *embedding_json == '\0') {
        return 0;
    }
    if (!memory->vss_enabled) {
        return bot_memory_search_scan(memory, scope, username, embedding_json, top_k, out, error, error_size);
    }
    wanted = top_k * 8 + 20;
    if (sqlite3_prepare_v2(memory->db,
                           "SELECT rowid, distance FROM memory_vectors "
                           "WHERE vss_search(embedding, ?) LIMIT ?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        set_sqlite_error(memory, error, error_size, "准备向量检索失败");
        return -1;
    }
    sqlite3_bind_text(stmt, 1, embedding_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, wanted);
    while (sqlite3_step(stmt) == SQLITE_ROW && count < top_k) {
        sqlite3_int64 rowid = sqlite3_column_int64(stmt, 0);
        int matched = append_item_by_id(memory, rowid, scope, username, out, &count);
        if (matched < 0) {
            sqlite3_finalize(stmt);
            set_sqlite_error(memory, error, error_size, "读取检索记忆失败");
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}


int bot_memory_append_summary(bot_memory_t *memory,
                              const char *scope,
                              const char *username,
                              bot_str_t *out)
{
    sqlite3_stmt *stmt = NULL;
    int rc = 0;

    if (sqlite3_prepare_v2(memory->db,
                           "SELECT content FROM summaries WHERE scope=? AND username=?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *content = (const char *)sqlite3_column_text(stmt, 0);
        if (content != NULL && *content != '\0') {
            rc = bot_str_appendf(out, "%s摘要:\n%s\n", strcmp(scope, "global") == 0 ? "公共" : "用户", content);
        }
    }
    sqlite3_finalize(stmt);
    return rc;
}

int bot_memory_append_recent_context(bot_memory_t *memory,
                                     const char *username,
                                     int limit,
                                     bot_str_t *out)
{
    sqlite3_stmt *stmt = NULL;
    int appended_header = 0;

    if (limit <= 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(memory->db,
                           "SELECT speaker, role, content FROM ("
                           "  SELECT id, speaker, role, content FROM messages "
                           "  WHERE target_user=? OR role='user' "
                           "  ORDER BY id DESC LIMIT ?"
                           ") ORDER BY id ASC",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *speaker = (const char *)sqlite3_column_text(stmt, 0);
        const char *role = (const char *)sqlite3_column_text(stmt, 1);
        const char *content = (const char *)sqlite3_column_text(stmt, 2);

        if (!appended_header) {
            if (bot_str_append(out, "\n最近上下文:\n") != 0) {
                sqlite3_finalize(stmt);
                return -1;
            }
            appended_header = 1;
        }
        if (bot_str_appendf(out, "%s %s: %s\n",
                            strcmp(role != NULL ? role : "", "assistant") == 0 ? "bot" : "user",
                            speaker != NULL ? speaker : "",
                            content != NULL ? content : "") != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

static long long max_message_id(bot_memory_t *memory)
{
    sqlite3_stmt *stmt = NULL;
    long long id = 0;

    if (sqlite3_prepare_v2(memory->db, "SELECT COALESCE(MAX(id), 0) FROM messages", -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

static long long summary_updated_id(bot_memory_t *memory, const char *scope, const char *username)
{
    sqlite3_stmt *stmt = NULL;
    long long id = 0;

    if (sqlite3_prepare_v2(memory->db,
                           "SELECT updated_message_id FROM summaries WHERE scope=? AND username=?",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        id = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return id;
}

int bot_memory_summary_due(bot_memory_t *memory,
                           const char *scope,
                           const char *username,
                           int threshold)
{
    long long max_id = max_message_id(memory);
    long long updated = summary_updated_id(memory, scope, username);

    return max_id - updated >= threshold;
}

int bot_memory_collect_summary_source(bot_memory_t *memory,
                                      const char *scope,
                                      const char *username,
                                      int limit,
                                      bot_str_t *out,
                                      long long *max_message_id_out)
{
    sqlite3_stmt *stmt = NULL;
    long long updated = summary_updated_id(memory, scope, username);
    const char *sql_global =
        "SELECT id, speaker, role, content FROM messages WHERE id>? ORDER BY id DESC LIMIT ?";
    const char *sql_user =
        "SELECT id, speaker, role, content FROM messages WHERE id>? AND target_user=? ORDER BY id DESC LIMIT ?";

    *max_message_id_out = max_message_id(memory);
    if (sqlite3_prepare_v2(memory->db,
                           strcmp(scope, "global") == 0 ? sql_global : sql_user,
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, updated);
    if (strcmp(scope, "global") == 0) {
        sqlite3_bind_int(stmt, 2, limit);
    } else {
        sqlite3_bind_text(stmt, 2, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, limit);
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *speaker = (const char *)sqlite3_column_text(stmt, 1);
        const char *role = (const char *)sqlite3_column_text(stmt, 2);
        const char *content = (const char *)sqlite3_column_text(stmt, 3);
        if (bot_str_appendf(out, "%s %s: %s\n",
                            strcmp(role != NULL ? role : "", "assistant") == 0 ? "bot" : "user",
                            speaker != NULL ? speaker : "",
                            content != NULL ? content : "") != 0) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

int bot_memory_save_summary(bot_memory_t *memory,
                            const char *scope,
                            const char *username,
                            const char *summary,
                            long long updated_message_id)
{
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (sqlite3_prepare_v2(memory->db,
                           "INSERT INTO summaries(scope, username, content, updated_message_id, updated_at) "
                           "VALUES(?, ?, ?, ?, ?) "
                           "ON CONFLICT(scope, username) DO UPDATE SET "
                           "content=excluded.content, updated_message_id=excluded.updated_message_id, "
                           "updated_at=excluded.updated_at",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, username != NULL ? username : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, summary, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, updated_message_id);
    sqlite3_bind_int64(stmt, 5, (sqlite3_int64)time(NULL));
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}
