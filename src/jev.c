#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <sqlite3ext.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

SQLITE_EXTENSION_INIT1

#ifndef JEV_VERSION
#define JEV_VERSION "0.1.0"
#endif
#define JEV_DEFAULT_URL "https://api.typesafe.ai/v1/systemone"
#define JEV_DEFAULT_MODEL "jev-latest"
#define JEV_DEFAULT_BATCH_SIZE 40
#define JEV_DEFAULT_MAX_ROWS 500
#define JEV_DEFAULT_TIMEOUT 90

/* libcurl's easy ABI is stable. Loading it at runtime keeps the extension
 * buildable on machines that have curl installed but not its header package. */
typedef void CURL;
typedef int CURLcode;
typedef int CURLoption;
typedef int CURLINFO;
struct curl_slist {
  char *data;
  struct curl_slist *next;
};

enum {
  JEV_CURLE_OK = 0,
  JEV_CURLOPT_WRITEDATA = 10001,
  JEV_CURLOPT_URL = 10002,
  JEV_CURLOPT_WRITEFUNCTION = 20011,
  JEV_CURLOPT_TIMEOUT = 13,
  JEV_CURLOPT_POSTFIELDS = 10015,
  JEV_CURLOPT_USERAGENT = 10018,
  JEV_CURLOPT_HTTPHEADER = 10023,
  JEV_CURLOPT_POSTFIELDSIZE = 60,
  JEV_CURLOPT_NOSIGNAL = 99,
  JEV_CURLINFO_RESPONSE_CODE = 0x200002,
  JEV_CURL_GLOBAL_DEFAULT = 3
};

typedef struct {
  void *handle;
  CURLcode (*global_init)(long);
  void (*global_cleanup)(void);
  CURL *(*easy_init)(void);
  CURLcode (*easy_setopt)(CURL *, CURLoption, ...);
  CURLcode (*easy_perform)(CURL *);
  CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...);
  void (*easy_cleanup)(CURL *);
  const char *(*easy_strerror)(CURLcode);
  struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
  void (*slist_free_all)(struct curl_slist *);
} JevCurl;

typedef struct JevCacheEntry JevCacheEntry;
struct JevCacheEntry {
  char *key;
  char *answer;
  JevCacheEntry *next;
};

typedef struct {
  sqlite3 *db;
  JevCurl curl;
  char *api_key;
  char *api_url;
  char *model;
  int batch_size;
  int max_rows;
  long timeout;
  sqlite3_int64 requests;
  sqlite3_int64 input_tokens;
  sqlite3_int64 output_tokens;
  sqlite3_int64 rows_evaluated;
  sqlite3_int64 cache_hits;
  JevCacheEntry *cache;
} JevState;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} JevBuf;

static int buf_grow(JevBuf *b, size_t extra) {
  size_t need = b->len + extra + 1;
  size_t cap;
  char *p;
  if (need <= b->cap) return SQLITE_OK;
  cap = b->cap ? b->cap : 256;
  while (cap < need) {
    if (cap > SIZE_MAX / 2) return SQLITE_NOMEM;
    cap *= 2;
  }
  p = sqlite3_realloc64(b->data, cap);
  if (!p) return SQLITE_NOMEM;
  b->data = p;
  b->cap = cap;
  return SQLITE_OK;
}

static int buf_append_n(JevBuf *b, const char *s, size_t n) {
  int rc = buf_grow(b, n);
  if (rc != SQLITE_OK) return rc;
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
  return SQLITE_OK;
}

static int buf_append(JevBuf *b, const char *s) {
  return buf_append_n(b, s, strlen(s));
}

static int buf_printf(JevBuf *b, const char *fmt, ...) {
  va_list ap;
  char *s;
  int rc;
  va_start(ap, fmt);
  s = sqlite3_vmprintf(fmt, ap);
  va_end(ap);
  if (!s) return SQLITE_NOMEM;
  rc = buf_append(b, s);
  sqlite3_free(s);
  return rc;
}

static int buf_json_string(JevBuf *b, const unsigned char *s, int n) {
  static const char hex[] = "0123456789abcdef";
  int i;
  if (buf_append_n(b, "\"", 1) != SQLITE_OK) return SQLITE_NOMEM;
  for (i = 0; i < n; i++) {
    unsigned char c = s[i];
    if (c == '"' || c == '\\') {
      char esc[2] = {'\\', (char)c};
      if (buf_append_n(b, esc, 2) != SQLITE_OK) return SQLITE_NOMEM;
    } else if (c == '\b') {
      if (buf_append(b, "\\b") != SQLITE_OK) return SQLITE_NOMEM;
    } else if (c == '\f') {
      if (buf_append(b, "\\f") != SQLITE_OK) return SQLITE_NOMEM;
    } else if (c == '\n') {
      if (buf_append(b, "\\n") != SQLITE_OK) return SQLITE_NOMEM;
    } else if (c == '\r') {
      if (buf_append(b, "\\r") != SQLITE_OK) return SQLITE_NOMEM;
    } else if (c == '\t') {
      if (buf_append(b, "\\t") != SQLITE_OK) return SQLITE_NOMEM;
    } else if (c < 0x20) {
      char esc[6] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 15]};
      if (buf_append_n(b, esc, sizeof(esc)) != SQLITE_OK) return SQLITE_NOMEM;
    } else if (buf_append_n(b, (const char *)&c, 1) != SQLITE_OK) {
      return SQLITE_NOMEM;
    }
  }
  return buf_append_n(b, "\"", 1);
}

static int buf_json_cstr(JevBuf *b, const char *s) {
  return buf_json_string(b, (const unsigned char *)s, (int)strlen(s));
}

static void buf_reset(JevBuf *b) {
  sqlite3_free(b->data);
  memset(b, 0, sizeof(*b));
}

static size_t curl_write(char *ptr, size_t size, size_t nmemb, void *userdata) {
  JevBuf *b = userdata;
  size_t n = size * nmemb;
  return buf_append_n(b, ptr, n) == SQLITE_OK ? n : 0;
}

static int load_symbol(void *handle, void **out, const char *name, char **error) {
  const char *why;
  dlerror();
  *out = dlsym(handle, name);
  why = dlerror();
  if (why) {
    *error = sqlite3_mprintf("jev: missing libcurl symbol %s: %s", name, why);
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

static int curl_load(JevCurl *c, char **error) {
  static const char *names[] = {
    "libcurl.so.4", "libcurl.so", "libcurl.4.dylib", "libcurl.dylib", NULL
  };
  int i;
  memset(c, 0, sizeof(*c));
  for (i = 0; names[i] && !c->handle; i++) c->handle = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
  if (!c->handle) {
    *error = sqlite3_mprintf("jev: libcurl is required (%s)", dlerror());
    return SQLITE_ERROR;
  }
#define LOAD(name) do { \
  if (load_symbol(c->handle, (void **)&c->name, "curl_" #name, error) != SQLITE_OK) return SQLITE_ERROR; \
} while (0)
  LOAD(global_init);
  LOAD(global_cleanup);
  LOAD(easy_init);
  LOAD(easy_setopt);
  LOAD(easy_perform);
  LOAD(easy_getinfo);
  LOAD(easy_cleanup);
  LOAD(easy_strerror);
  LOAD(slist_append);
  LOAD(slist_free_all);
#undef LOAD
  if (c->global_init(JEV_CURL_GLOBAL_DEFAULT) != JEV_CURLE_OK) {
    *error = sqlite3_mprintf("jev: curl_global_init failed");
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

static char *strdup_sqlite(const char *s) {
  return s ? sqlite3_mprintf("%s", s) : NULL;
}

static const char *env_api_key(void) {
  const char *s = getenv("TYPESAFE_API_KEY");
  return s && *s ? s : NULL;
}

static void cache_clear(JevState *s) {
  JevCacheEntry *p = s->cache;
  while (p) {
    JevCacheEntry *next = p->next;
    sqlite3_free(p->key);
    sqlite3_free(p->answer);
    sqlite3_free(p);
    p = next;
  }
  s->cache = NULL;
}

static const char *cache_get(JevState *s, const char *key) {
  JevCacheEntry *p;
  for (p = s->cache; p; p = p->next) {
    if (strcmp(p->key, key) == 0) {
      s->cache_hits++;
      return p->answer;
    }
  }
  return NULL;
}

static int cache_put(JevState *s, const char *key, const char *answer) {
  JevCacheEntry *p = sqlite3_malloc64(sizeof(*p));
  if (!p) return SQLITE_NOMEM;
  memset(p, 0, sizeof(*p));
  p->key = strdup_sqlite(key);
  p->answer = strdup_sqlite(answer);
  if (!p->key || !p->answer) {
    sqlite3_free(p->key);
    sqlite3_free(p->answer);
    sqlite3_free(p);
    return SQLITE_NOMEM;
  }
  p->next = s->cache;
  s->cache = p;
  return SQLITE_OK;
}

static void state_free(void *p) {
  JevState *s = p;
  if (!s) return;
  cache_clear(s);
  sqlite3_free(s->api_key);
  sqlite3_free(s->api_url);
  sqlite3_free(s->model);
  if (s->curl.handle) {
    if (s->curl.global_cleanup) s->curl.global_cleanup();
    dlclose(s->curl.handle);
  }
  sqlite3_free(s);
}

static int json_is_valid(sqlite3 *db, const char *json) {
  sqlite3_stmt *stmt = NULL;
  int ok = 0;
  if (sqlite3_prepare_v2(db, "SELECT json_valid(?1)", -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) ok = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
}

static int json_type_is(sqlite3 *db, const char *json, const char *wanted) {
  sqlite3_stmt *stmt = NULL;
  int ok = 0;
  if (sqlite3_prepare_v2(db, "SELECT json_type(?1)", -1, &stmt, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *type = (const char *)sqlite3_column_text(stmt, 0);
    ok = type && strcmp(type, wanted) == 0;
  }
  sqlite3_finalize(stmt);
  return ok;
}

static int json_extract_text(sqlite3 *db, const char *json, const char *path, char **out) {
  sqlite3_stmt *stmt = NULL;
  int rc;
  *out = NULL;
  rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1, ?2)", -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
    *out = strdup_sqlite((const char *)sqlite3_column_text(stmt, 0));
    rc = *out ? SQLITE_OK : SQLITE_NOMEM;
  } else {
    rc = SQLITE_NOTFOUND;
  }
  sqlite3_finalize(stmt);
  return rc;
}

static int json_extract_double(sqlite3 *db, const char *json, const char *path, double *out) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1, ?2)", -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
    *out = sqlite3_column_double(stmt, 0);
    rc = SQLITE_OK;
  } else {
    rc = SQLITE_NOTFOUND;
  }
  sqlite3_finalize(stmt);
  return rc;
}

static int json_extract_int64(sqlite3 *db, const char *json, const char *path, sqlite3_int64 *out) {
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, "SELECT json_extract(?1, ?2)", -1, &stmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, path, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
    *out = sqlite3_column_int64(stmt, 0);
    rc = SQLITE_OK;
  } else {
    rc = SQLITE_NOTFOUND;
  }
  sqlite3_finalize(stmt);
  return rc;
}

static int http_post_once(JevState *s, const char *body, JevBuf *response,
                          long *status, char **error) {
  CURL *curl = NULL;
  struct curl_slist *headers = NULL;
  CURLcode rc;
  JevBuf auth = {0};
  int out = SQLITE_ERROR;
  if (!s->api_key || !*s->api_key) {
    *error = sqlite3_mprintf("jev: no API key; export TYPESAFE_API_KEY or call jev_config('api_key', ...)");
    return SQLITE_ERROR;
  }
  curl = s->curl.easy_init();
  if (!curl) {
    *error = sqlite3_mprintf("jev: curl_easy_init failed");
    return SQLITE_ERROR;
  }
  if (buf_append(&auth, "Authorization: Bearer ") != SQLITE_OK ||
      buf_append(&auth, s->api_key) != SQLITE_OK) {
    *error = sqlite3_mprintf("jev: out of memory");
    goto done;
  }
  headers = s->curl.slist_append(headers, "Content-Type: application/json");
  headers = s->curl.slist_append(headers, auth.data);
  if (!headers) {
    *error = sqlite3_mprintf("jev: could not allocate HTTP headers");
    goto done;
  }
  s->curl.easy_setopt(curl, JEV_CURLOPT_URL, s->api_url);
  s->curl.easy_setopt(curl, JEV_CURLOPT_HTTPHEADER, headers);
  s->curl.easy_setopt(curl, JEV_CURLOPT_POSTFIELDS, body);
  s->curl.easy_setopt(curl, JEV_CURLOPT_POSTFIELDSIZE, (long)strlen(body));
  s->curl.easy_setopt(curl, JEV_CURLOPT_TIMEOUT, s->timeout);
  s->curl.easy_setopt(curl, JEV_CURLOPT_NOSIGNAL, 1L);
  s->curl.easy_setopt(curl, JEV_CURLOPT_USERAGENT, "sqlite-jev/" JEV_VERSION);
  s->curl.easy_setopt(curl, JEV_CURLOPT_WRITEFUNCTION, curl_write);
  s->curl.easy_setopt(curl, JEV_CURLOPT_WRITEDATA, response);
  rc = s->curl.easy_perform(curl);
  if (rc != JEV_CURLE_OK) {
    *error = sqlite3_mprintf("jev: TypeSafe request failed: %s", s->curl.easy_strerror(rc));
    goto done;
  }
  s->curl.easy_getinfo(curl, JEV_CURLINFO_RESPONSE_CODE, status);
  out = SQLITE_OK;
done:
  if (headers) s->curl.slist_free_all(headers);
  if (curl) s->curl.easy_cleanup(curl);
  buf_reset(&auth);
  return out;
}

static int http_post(JevState *s, const char *body, char **response, char **error) {
  int attempt;
  long status = 0;
  *response = NULL;
  for (attempt = 0; attempt < 4; attempt++) {
    JevBuf b = {0};
    int rc = http_post_once(s, body, &b, &status, error);
    s->requests++;
    if (rc != SQLITE_OK) {
      buf_reset(&b);
      return rc;
    }
    if (status >= 200 && status < 300) {
      if (!json_is_valid(s->db, b.data ? b.data : "")) {
        *error = sqlite3_mprintf("jev: API returned invalid JSON");
        buf_reset(&b);
        return SQLITE_ERROR;
      }
      *response = b.data;
      return SQLITE_OK;
    }
    if (!(status == 429 || status == 529 || status >= 500) || attempt == 3) {
      char *message = NULL;
      if (b.data) json_extract_text(s->db, b.data, "$.error.message", &message);
      *error = sqlite3_mprintf("jev: TypeSafe API returned HTTP %ld%s%s", status,
                               message ? ": " : "", message ? message : "");
      sqlite3_free(message);
      buf_reset(&b);
      return SQLITE_ERROR;
    }
    buf_reset(&b);
    {
      struct timespec delay = {0, (long)(100000000L << attempt)};
      nanosleep(&delay, NULL);
    }
  }
  return SQLITE_ERROR;
}

static int append_state_json(JevState *s, JevBuf *b, sqlite3_value *value) {
  const char *text = (const char *)sqlite3_value_text(value);
  int n = sqlite3_value_bytes(value);
  if (!text) return buf_append(b, "null");
  if (sqlite3_value_subtype(value) == 74 || json_is_valid(s->db, text)) return buf_append_n(b, text, n);
  return buf_json_string(b, (const unsigned char *)text, n);
}

static int append_question(JevBuf *b, const char *kind, const char *instruction,
                           const char *criteria) {
  if (buf_append(b, "{\"type\":") != SQLITE_OK ||
      buf_json_cstr(b, kind) != SQLITE_OK ||
      buf_append(b, ",\"instructions\":") != SQLITE_OK ||
      buf_json_cstr(b, instruction) != SQLITE_OK) return SQLITE_NOMEM;
  if (criteria && *criteria) {
    if (buf_append(b, ",\"criteria\":") != SQLITE_OK || buf_append(b, criteria) != SQLITE_OK) return SQLITE_NOMEM;
  } else if (strcmp(kind, "noul") == 0) {
    if (buf_append(b, ",\"criteria\":{\"true\":\"The record satisfies the condition\",\"false\":\"The record does not satisfy the condition\"}") != SQLITE_OK) return SQLITE_NOMEM;
  }
  return buf_append(b, "}");
}

static int validate_question(JevState *s, const char *kind, const char *criteria, char **error) {
  if (strcmp(kind, "noul") != 0 && strcmp(kind, "choice") != 0 && strcmp(kind, "score") != 0) {
    *error = sqlite3_mprintf("jev: kind must be 'noul', 'choice', or 'score'");
    return SQLITE_ERROR;
  }
  if (strcmp(kind, "choice") == 0 && (!criteria || !json_type_is(s->db, criteria, "object"))) {
    *error = sqlite3_mprintf("jev: choice criteria must be a JSON object");
    return SQLITE_ERROR;
  }
  if (strcmp(kind, "score") == 0 && (!criteria || !json_type_is(s->db, criteria, "array"))) {
    *error = sqlite3_mprintf("jev: score criteria must be a JSON array");
    return SQLITE_ERROR;
  }
  if (criteria && !json_is_valid(s->db, criteria)) {
    *error = sqlite3_mprintf("jev: criteria is not valid JSON");
    return SQLITE_ERROR;
  }
  return SQLITE_OK;
}

static char *make_cache_key(JevState *s, const char *state, const char *question,
                            const char *kind, const char *criteria) {
  return sqlite3_mprintf("%s\x1f%s\x1f%s\x1f%s\x1f%s", s->model, kind, question,
                         criteria ? criteria : "", state);
}

static int eval_one(JevState *s, sqlite3_value *state, const char *question,
                    const char *kind, const char *criteria, char **answer, char **error) {
  JevBuf request = {0};
  char *response = NULL;
  char *cache_key = NULL;
  const char *cached;
  sqlite3_int64 tokens;
  int rc;
  *answer = NULL;
  if ((rc = validate_question(s, kind, criteria, error)) != SQLITE_OK) return rc;
  if (buf_append(&request, "{\"model\":") != SQLITE_OK ||
      buf_json_cstr(&request, s->model) != SQLITE_OK ||
      buf_append(&request, ",\"state\":") != SQLITE_OK ||
      append_state_json(s, &request, state) != SQLITE_OK) goto nomem;
  /* The request prefix includes model and state, which makes a compact cache key. */
  cache_key = make_cache_key(s, request.data, question, kind, criteria);
  if (!cache_key) goto nomem;
  cached = cache_get(s, cache_key);
  if (cached) {
    *answer = strdup_sqlite(cached);
    rc = *answer ? SQLITE_OK : SQLITE_NOMEM;
    goto done;
  }
  if (buf_append(&request, ",\"questions\":{\"answer\":") != SQLITE_OK ||
      append_question(&request, kind, question, criteria) != SQLITE_OK ||
      buf_append(&request, "}}") != SQLITE_OK) goto nomem;
  rc = http_post(s, request.data, &response, error);
  if (rc != SQLITE_OK) goto done;
  rc = json_extract_text(s->db, response, "$.answers.answer", answer);
  if (rc != SQLITE_OK) {
    *error = sqlite3_mprintf("jev: API response has no answer");
    rc = SQLITE_ERROR;
    goto done;
  }
  if (json_extract_int64(s->db, response, "$.usage.input_tokens", &tokens) == SQLITE_OK) s->input_tokens += tokens;
  if (json_extract_int64(s->db, response, "$.usage.output_tokens", &tokens) == SQLITE_OK) s->output_tokens += tokens;
  s->rows_evaluated++;
  cache_put(s, cache_key, *answer);
  goto done;
nomem:
  *error = sqlite3_mprintf("jev: out of memory");
  rc = SQLITE_NOMEM;
done:
  sqlite3_free(cache_key);
  sqlite3_free(response);
  buf_reset(&request);
  return rc;
}

static const char *value_text_or(sqlite3_value *v, const char *fallback) {
  const char *s = v && sqlite3_value_type(v) != SQLITE_NULL ? (const char *)sqlite3_value_text(v) : NULL;
  return s ? s : fallback;
}

static void result_error(sqlite3_context *ctx, char *error, int rc) {
  sqlite3_result_error(ctx, error ? error : "jev: operation failed", -1);
  if (rc == SQLITE_NOMEM) sqlite3_result_error_nomem(ctx);
  sqlite3_free(error);
}

static void fn_eval(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  const char *question = value_text_or(argv[1], "");
  const char *kind = argc > 2 ? value_text_or(argv[2], "noul") : "noul";
  const char *criteria = argc > 3 && sqlite3_value_type(argv[3]) != SQLITE_NULL
                           ? (const char *)sqlite3_value_text(argv[3]) : NULL;
  char *answer = NULL, *error = NULL;
  int rc;
  if (!question[0]) {
    sqlite3_result_error(ctx, "jev: question must not be empty", -1);
    return;
  }
  rc = eval_one(s, argv[0], question, kind, criteria, &answer, &error);
  if (rc != SQLITE_OK) {
    result_error(ctx, error, rc);
    return;
  }
  sqlite3_result_text(ctx, answer, -1, sqlite3_free);
  sqlite3_result_subtype(ctx, 74);
}

static void fn_prob(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  char *answer = NULL, *error = NULL;
  double value;
  int rc = eval_one(s, argv[0], value_text_or(argv[1], ""), "noul", NULL, &answer, &error);
  (void)argc;
  if (rc == SQLITE_OK) rc = json_extract_double(s->db, answer, "$.noul", &value);
  if (rc != SQLITE_OK) {
    sqlite3_free(answer);
    if (!error) error = sqlite3_mprintf("jev: noul answer is malformed");
    result_error(ctx, error, rc);
    return;
  }
  sqlite3_free(answer);
  sqlite3_result_double(ctx, value);
}

static void fn_jev(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  char *answer = NULL, *error = NULL;
  double value, threshold = argc > 2 ? sqlite3_value_double(argv[2]) : 0.5;
  int rc = eval_one(s, argv[0], value_text_or(argv[1], ""), "noul", NULL, &answer, &error);
  if (rc == SQLITE_OK) rc = json_extract_double(s->db, answer, "$.noul", &value);
  if (rc != SQLITE_OK) {
    sqlite3_free(answer);
    if (!error) error = sqlite3_mprintf("jev: noul answer is malformed");
    result_error(ctx, error, rc);
    return;
  }
  sqlite3_free(answer);
  sqlite3_result_int(ctx, value >= threshold);
}

static void fn_choice(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  char *answer = NULL, *choice = NULL, *error = NULL;
  int rc = eval_one(s, argv[0], value_text_or(argv[1], ""), "choice",
                    value_text_or(argv[2], NULL), &answer, &error);
  (void)argc;
  if (rc == SQLITE_OK) rc = json_extract_text(s->db, answer, "$.choice", &choice);
  sqlite3_free(answer);
  if (rc != SQLITE_OK) {
    if (!error) error = sqlite3_mprintf("jev: choice answer is malformed");
    result_error(ctx, error, rc);
    return;
  }
  sqlite3_result_text(ctx, choice, -1, sqlite3_free);
}

static void fn_score_common(sqlite3_context *ctx, int argc, sqlite3_value **argv, int normalize) {
  JevState *s = sqlite3_user_data(ctx);
  const char *criteria = value_text_or(argv[2], NULL);
  char *answer = NULL, *error = NULL;
  double score;
  int count = 0;
  int rc = eval_one(s, argv[0], value_text_or(argv[1], ""), "score", criteria, &answer, &error);
  (void)argc;
  if (rc == SQLITE_OK) rc = json_extract_double(s->db, answer, "$.score", &score);
  if (rc == SQLITE_OK && normalize) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(s->db, "SELECT json_array_length(?1)", -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, criteria, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (count < 2) rc = SQLITE_ERROR;
    else score /= (double)(count - 1);
  }
  sqlite3_free(answer);
  if (rc != SQLITE_OK) {
    if (!error) error = sqlite3_mprintf("jev: score answer is malformed");
    result_error(ctx, error, rc);
    return;
  }
  sqlite3_result_double(ctx, score);
}

static void fn_score(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  fn_score_common(ctx, argc, argv, 0);
}

static void fn_score_norm(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  fn_score_common(ctx, argc, argv, 1);
}

static void fn_confidence(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  const char *kind = value_text_or(argv[2], "");
  const char *criteria = value_text_or(argv[3], NULL);
  char *answer = NULL, *error = NULL;
  double confidence;
  int rc = eval_one(s, argv[0], value_text_or(argv[1], ""), kind, criteria, &answer, &error);
  (void)argc;
  if (rc == SQLITE_OK) rc = json_extract_double(s->db, answer, "$.confidence", &confidence);
  sqlite3_free(answer);
  if (rc != SQLITE_OK) {
    if (!error) error = sqlite3_mprintf("jev: answer has no confidence (nouls use distance from 0.5)");
    result_error(ctx, error, rc);
    return;
  }
  sqlite3_result_double(ctx, confidence);
}

static void fn_version(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  (void)argc; (void)argv;
  sqlite3_result_text(ctx, JEV_VERSION, -1, SQLITE_STATIC);
}

static void fn_cache_clear(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  (void)argc; (void)argv;
  cache_clear(s);
  sqlite3_result_null(ctx);
}

static void fn_stats(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  char *json = sqlite3_mprintf("{\"requests\":%lld,\"input_tokens\":%lld,\"output_tokens\":%lld,\"rows_evaluated\":%lld,\"cache_hits\":%lld}",
      s->requests, s->input_tokens, s->output_tokens, s->rows_evaluated, s->cache_hits);
  (void)argc; (void)argv;
  if (!json) sqlite3_result_error_nomem(ctx);
  else {
    sqlite3_result_text(ctx, json, -1, sqlite3_free);
    sqlite3_result_subtype(ctx, 74);
  }
}

static void fn_config(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  JevState *s = sqlite3_user_data(ctx);
  const char *name = value_text_or(argv[0], "");
  const char *value = argc > 1 ? value_text_or(argv[1], NULL) : NULL;
  char **slot = NULL;
  if (strcmp(name, "api_key") == 0) {
    if (argc == 1) { sqlite3_result_text(ctx, s->api_key ? "set" : "unset", -1, SQLITE_STATIC); return; }
    slot = &s->api_key;
  } else if (strcmp(name, "api_url") == 0) {
    slot = &s->api_url;
  } else if (strcmp(name, "model") == 0) {
    slot = &s->model;
  } else if (strcmp(name, "batch_size") == 0) {
    if (argc == 1) sqlite3_result_int(ctx, s->batch_size);
    else { int n = sqlite3_value_int(argv[1]); if (n < 1) sqlite3_result_error(ctx, "jev: batch_size must be positive", -1); else { s->batch_size = n; sqlite3_result_int(ctx, n); } }
    return;
  } else if (strcmp(name, "max_rows") == 0) {
    if (argc == 1) sqlite3_result_int(ctx, s->max_rows);
    else { int n = sqlite3_value_int(argv[1]); if (n < 1) sqlite3_result_error(ctx, "jev: max_rows must be positive", -1); else { s->max_rows = n; sqlite3_result_int(ctx, n); } }
    return;
  } else if (strcmp(name, "timeout") == 0) {
    if (argc == 1) sqlite3_result_int64(ctx, s->timeout);
    else { sqlite3_int64 n = sqlite3_value_int64(argv[1]); if (n < 1) sqlite3_result_error(ctx, "jev: timeout must be positive", -1); else { s->timeout = (long)n; sqlite3_result_int64(ctx, n); } }
    return;
  } else {
    sqlite3_result_error(ctx, "jev: unknown setting (api_key, api_url, model, batch_size, max_rows, timeout)", -1);
    return;
  }
  if (argc == 1) {
    sqlite3_result_text(ctx, *slot, -1, SQLITE_TRANSIENT);
    return;
  }
  if (!value || !*value) {
    sqlite3_result_error(ctx, "jev: setting value must not be empty", -1);
    return;
  }
  {
    char *copy = strdup_sqlite(value);
    if (!copy) { sqlite3_result_error_nomem(ctx); return; }
    sqlite3_free(*slot);
    *slot = copy;
    cache_clear(s);
    sqlite3_result_text(ctx, strcmp(name, "api_key") == 0 ? "set" : copy, -1, SQLITE_TRANSIENT);
  }
}

/* jev_rows(table, question, kind, criteria, columns) ----------------------- */
enum {
  COL_SOURCE_ROWID = 0, COL_ANSWER, COL_PROBABILITY, COL_CHOICE, COL_SCORE,
  COL_CONFIDENCE, COL_TABLE, COL_QUESTION, COL_KIND, COL_CRITERIA, COL_COLUMNS
};

typedef struct { sqlite3_vtab base; JevState *state; } JevVtab;

typedef struct {
  sqlite3_int64 source_rowid;
  char *answer;
  int has_probability;
  double probability;
  char *choice;
  int has_score;
  double score;
  int has_confidence;
  double confidence;
} JevRow;

typedef struct {
  sqlite3_vtab_cursor base;
  JevRow *rows;
  int count;
  int index;
} JevCursor;

static int vtab_connect(sqlite3 *db, void *aux, int argc, const char *const *argv,
                        sqlite3_vtab **out, char **error) {
  JevVtab *v;
  int rc;
  (void)argc; (void)argv; (void)error;
  rc = sqlite3_declare_vtab(db,
      "CREATE TABLE x(source_rowid INTEGER, answer TEXT, probability REAL, choice TEXT, score REAL, confidence REAL, "
      "table_name TEXT HIDDEN, question TEXT HIDDEN, kind TEXT HIDDEN, criteria TEXT HIDDEN, columns TEXT HIDDEN)");
  if (rc != SQLITE_OK) return rc;
  v = sqlite3_malloc64(sizeof(*v));
  if (!v) return SQLITE_NOMEM;
  memset(v, 0, sizeof(*v));
  v->state = aux;
  *out = &v->base;
  return SQLITE_OK;
}

static int vtab_disconnect(sqlite3_vtab *vtab) { sqlite3_free(vtab); return SQLITE_OK; }

static int vtab_best_index(sqlite3_vtab *vtab, sqlite3_index_info *info) {
  int i, col, next = 1, have_table = 0, have_question = 0;
  (void)vtab;
  /* Assign argv indexes in hidden-column order so xFilter can decode them. */
  for (col = COL_TABLE; col <= COL_COLUMNS; col++) {
    for (i = 0; i < info->nConstraint; i++) {
      if (info->aConstraint[i].iColumn != col || !info->aConstraint[i].usable ||
          info->aConstraint[i].op != SQLITE_INDEX_CONSTRAINT_EQ) continue;
      info->aConstraintUsage[i].argvIndex = next++;
      info->aConstraintUsage[i].omit = 1;
      info->idxNum |= 1 << (col - COL_TABLE);
      if (col == COL_TABLE) have_table = 1;
      if (col == COL_QUESTION) have_question = 1;
      break;
    }
  }
  if (!have_table || !have_question) {
    info->estimatedCost = 1e12;
    info->estimatedRows = 1000000000;
  } else {
    info->estimatedCost = 100000;
    info->estimatedRows = 100;
  }
  return SQLITE_OK;
}

static int vtab_open(sqlite3_vtab *vtab, sqlite3_vtab_cursor **out) {
  JevCursor *c = sqlite3_malloc64(sizeof(*c));
  (void)vtab;
  if (!c) return SQLITE_NOMEM;
  memset(c, 0, sizeof(*c));
  *out = &c->base;
  return SQLITE_OK;
}

static void cursor_reset(JevCursor *c) {
  int i;
  for (i = 0; i < c->count; i++) {
    sqlite3_free(c->rows[i].answer);
    sqlite3_free(c->rows[i].choice);
  }
  sqlite3_free(c->rows);
  c->rows = NULL;
  c->count = c->index = 0;
}

static int vtab_close(sqlite3_vtab_cursor *cursor) {
  JevCursor *c = (JevCursor *)cursor;
  cursor_reset(c);
  sqlite3_free(c);
  return SQLITE_OK;
}

static int valid_identifier(const char *s) {
  int i;
  if (!s || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return 0;
  for (i = 1; s[i]; i++) if (!(isalnum((unsigned char)s[i]) || s[i] == '_')) return 0;
  return 1;
}

static int append_identifier(JevBuf *b, const char *s) {
  const char *p;
  if (buf_append_n(b, "\"", 1) != SQLITE_OK) return SQLITE_NOMEM;
  for (p = s; *p; p++) {
    if (*p == '"' && buf_append_n(b, "\"", 1) != SQLITE_OK) return SQLITE_NOMEM;
    if (buf_append_n(b, p, 1) != SQLITE_OK) return SQLITE_NOMEM;
  }
  return buf_append_n(b, "\"", 1);
}

static int load_column_names(JevState *s, const char *table, const char *requested,
                             char ***names_out, int *count_out, char **error) {
  sqlite3_stmt *stmt = NULL;
  char **available = NULL, **names = NULL;
  int available_count = 0, count = 0, cap = 0, i, rc;
  *names_out = NULL; *count_out = 0;
  rc = sqlite3_prepare_v2(s->db, "SELECT name FROM pragma_table_xinfo(?1) WHERE hidden=0 ORDER BY cid", -1, &stmt, NULL);
  if (rc != SQLITE_OK) goto fail;
  sqlite3_bind_text(stmt, 1, table, -1, SQLITE_STATIC);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    char **next = sqlite3_realloc64(available, sizeof(char *) * (available_count + 1));
    if (!next) { rc = SQLITE_NOMEM; goto fail; }
    available = next;
    available[available_count] = strdup_sqlite((const char *)sqlite3_column_text(stmt, 0));
    if (!available[available_count++]) { rc = SQLITE_NOMEM; goto fail; }
  }
  if (rc != SQLITE_DONE) goto fail;
  sqlite3_finalize(stmt); stmt = NULL;
  if (available_count == 0) {
    *error = sqlite3_mprintf("jev: table or view '%s' does not exist", table);
    rc = SQLITE_ERROR; goto fail;
  }
  if (!requested) {
    names = available; count = available_count; available = NULL; available_count = 0;
  } else {
    if (!json_type_is(s->db, requested, "array")) {
      *error = sqlite3_mprintf("jev: columns must be a JSON array of column names"); rc = SQLITE_ERROR; goto fail;
    }
    rc = sqlite3_prepare_v2(s->db, "SELECT value FROM json_each(?1)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) goto fail;
    sqlite3_bind_text(stmt, 1, requested, -1, SQLITE_STATIC);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(stmt, 0);
      int found = 0;
      for (i = 0; i < available_count; i++) if (name && strcmp(name, available[i]) == 0) { found = 1; break; }
      if (!found) { *error = sqlite3_mprintf("jev: column '%s' does not exist in '%s'", name ? name : "", table); rc = SQLITE_ERROR; goto fail; }
      if (count == cap) {
        char **next;
        cap = cap ? cap * 2 : 8;
        next = sqlite3_realloc64(names, sizeof(char *) * cap);
        if (!next) { rc = SQLITE_NOMEM; goto fail; }
        names = next;
      }
      names[count] = strdup_sqlite(name);
      if (!names[count++]) { rc = SQLITE_NOMEM; goto fail; }
    }
    if (rc != SQLITE_DONE) goto fail;
    sqlite3_finalize(stmt); stmt = NULL;
    if (count == 0) { *error = sqlite3_mprintf("jev: columns must not be empty"); rc = SQLITE_ERROR; goto fail; }
  }
  for (i = 0; i < available_count; i++) sqlite3_free(available[i]);
  sqlite3_free(available);
  *names_out = names; *count_out = count;
  return SQLITE_OK;
fail:
  if (!*error) *error = sqlite3_mprintf("jev: could not inspect table '%s': %s", table, sqlite3_errmsg(s->db));
  sqlite3_finalize(stmt);
  for (i = 0; i < available_count; i++) sqlite3_free(available[i]);
  sqlite3_free(available);
  for (i = 0; i < count; i++) sqlite3_free(names[i]);
  sqlite3_free(names);
  return rc == SQLITE_NOMEM ? SQLITE_NOMEM : SQLITE_ERROR;
}

static int append_sql_value(JevBuf *b, sqlite3_stmt *stmt, int col) {
  int type = sqlite3_column_type(stmt, col);
  if (type == SQLITE_NULL) return buf_append(b, "null");
  if (type == SQLITE_INTEGER) return buf_printf(b, "%lld", sqlite3_column_int64(stmt, col));
  if (type == SQLITE_FLOAT) return buf_printf(b, "%.17g", sqlite3_column_double(stmt, col));
  if (type == SQLITE_TEXT) return buf_json_string(b, sqlite3_column_text(stmt, col), sqlite3_column_bytes(stmt, col));
  if (type == SQLITE_BLOB) {
    const unsigned char *p = sqlite3_column_blob(stmt, col);
    int n = sqlite3_column_bytes(stmt, col), i;
    static const char hex[] = "0123456789abcdef";
    if (buf_append(b, "\"hex:") != SQLITE_OK) return SQLITE_NOMEM;
    for (i = 0; i < n; i++) {
      char pair[2] = {hex[p[i] >> 4], hex[p[i] & 15]};
      if (buf_append_n(b, pair, 2) != SQLITE_OK) return SQLITE_NOMEM;
    }
    return buf_append_n(b, "\"", 1);
  }
  return SQLITE_ERROR;
}

typedef struct { sqlite3_int64 rowid; char *json; } InputRow;

static void free_input_rows(InputRow *rows, int count) {
  int i; for (i = 0; i < count; i++) sqlite3_free(rows[i].json); sqlite3_free(rows);
}

static int load_input_rows(JevState *s, const char *table, char **columns, int column_count,
                           InputRow **rows_out, int *count_out, char **error) {
  JevBuf sql = {0};
  sqlite3_stmt *stmt = NULL;
  InputRow *rows = NULL;
  int count = 0, cap = 0, i, rc;
  *rows_out = NULL; *count_out = 0;
  if (buf_append(&sql, "SELECT rowid") != SQLITE_OK) goto nomem;
  for (i = 0; i < column_count; i++) {
    if (buf_append(&sql, ",") != SQLITE_OK || append_identifier(&sql, columns[i]) != SQLITE_OK) goto nomem;
  }
  if (buf_append(&sql, " FROM ") != SQLITE_OK || append_identifier(&sql, table) != SQLITE_OK) goto nomem;
  rc = sqlite3_prepare_v2(s->db, sql.data, -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    *error = sqlite3_mprintf("jev: '%s' must be a rowid table: %s", table, sqlite3_errmsg(s->db));
    goto fail;
  }
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    JevBuf json = {0};
    if (count >= s->max_rows) {
      *error = sqlite3_mprintf("jev: table '%s' exceeds max_rows=%d; prefilter into a smaller table/view or raise jev_config('max_rows', ...)", table, s->max_rows);
      buf_reset(&json); rc = SQLITE_TOOBIG; goto fail;
    }
    if (count == cap) {
      InputRow *next;
      cap = cap ? cap * 2 : 32;
      next = sqlite3_realloc64(rows, sizeof(*rows) * cap);
      if (!next) { buf_reset(&json); goto nomem; }
      rows = next;
    }
    if (buf_append(&json, "{") != SQLITE_OK) { buf_reset(&json); goto nomem; }
    for (i = 0; i < column_count; i++) {
      if (i && buf_append(&json, ",") != SQLITE_OK) { buf_reset(&json); goto nomem; }
      if (buf_json_cstr(&json, columns[i]) != SQLITE_OK || buf_append(&json, ":") != SQLITE_OK ||
          append_sql_value(&json, stmt, i + 1) != SQLITE_OK) { buf_reset(&json); goto nomem; }
    }
    if (buf_append(&json, "}") != SQLITE_OK) { buf_reset(&json); goto nomem; }
    rows[count].rowid = sqlite3_column_int64(stmt, 0);
    rows[count].json = json.data;
    count++;
  }
  if (rc != SQLITE_DONE) goto fail;
  sqlite3_finalize(stmt); buf_reset(&sql);
  *rows_out = rows; *count_out = count;
  return SQLITE_OK;
nomem:
  rc = SQLITE_NOMEM;
  if (!*error) *error = sqlite3_mprintf("jev: out of memory");
fail:
  sqlite3_finalize(stmt); buf_reset(&sql); free_input_rows(rows, count);
  return rc;
}

static char *row_cache_key(JevState *s, const InputRow *row, const char *question,
                           const char *kind, const char *criteria) {
  return make_cache_key(s, row->json, question, kind, criteria);
}

static int parse_answer(JevState *s, JevRow *row, const char *answer) {
  row->answer = strdup_sqlite(answer);
  if (!row->answer) return SQLITE_NOMEM;
  row->has_probability = json_extract_double(s->db, answer, "$.noul", &row->probability) == SQLITE_OK;
  json_extract_text(s->db, answer, "$.choice", &row->choice);
  row->has_score = json_extract_double(s->db, answer, "$.score", &row->score) == SQLITE_OK;
  row->has_confidence = json_extract_double(s->db, answer, "$.confidence", &row->confidence) == SQLITE_OK;
  return SQLITE_OK;
}

static int eval_batch(JevState *s, InputRow *inputs, int start, int count,
                      const char *question, const char *kind, const char *criteria,
                      JevRow *out, char **error) {
  JevBuf body = {0};
  char *response = NULL;
  sqlite3_int64 tokens;
  int i, rc;
  if (buf_append(&body, "{\"model\":") != SQLITE_OK || buf_json_cstr(&body, s->model) != SQLITE_OK ||
      buf_append(&body, ",\"state\":{\"rows\":[") != SQLITE_OK) goto nomem;
  for (i = 0; i < count; i++) {
    if ((i && buf_append(&body, ",") != SQLITE_OK) || buf_append(&body, inputs[start + i].json) != SQLITE_OK) goto nomem;
  }
  if (buf_append(&body, "]},\"questions\":{") != SQLITE_OK) goto nomem;
  for (i = 0; i < count; i++) {
    JevBuf instruction = {0};
    if (i && buf_append(&body, ",") != SQLITE_OK) { buf_reset(&instruction); goto nomem; }
    if (buf_printf(&body, "\"r%d\":", i) != SQLITE_OK) { buf_reset(&instruction); goto nomem; }
    if (strcmp(kind, "noul") == 0) {
      if (buf_printf(&instruction, "Does the record `rows[%d]` satisfy this condition: %s", i, question) != SQLITE_OK) { buf_reset(&instruction); goto nomem; }
    } else {
      if (buf_printf(&instruction, "For the record `rows[%d]`: %s", i, question) != SQLITE_OK) { buf_reset(&instruction); goto nomem; }
    }
    rc = append_question(&body, kind, instruction.data, criteria);
    buf_reset(&instruction);
    if (rc != SQLITE_OK) goto nomem;
  }
  if (buf_append(&body, "}}") != SQLITE_OK) goto nomem;
  rc = http_post(s, body.data, &response, error);
  if (rc != SQLITE_OK) goto done;
  for (i = 0; i < count; i++) {
    char path[64], *answer = NULL, *key;
    sqlite3_snprintf(sizeof(path), path, "$.answers.r%d", i);
    if (json_extract_text(s->db, response, path, &answer) != SQLITE_OK) {
      *error = sqlite3_mprintf("jev: API response has no answer for row %d", start + i);
      rc = SQLITE_ERROR; sqlite3_free(answer); goto done;
    }
    out[start + i].source_rowid = inputs[start + i].rowid;
    rc = parse_answer(s, &out[start + i], answer);
    key = row_cache_key(s, &inputs[start + i], question, kind, criteria);
    if (key) { cache_put(s, key, answer); sqlite3_free(key); }
    sqlite3_free(answer);
    if (rc != SQLITE_OK) goto nomem;
  }
  if (json_extract_int64(s->db, response, "$.usage.input_tokens", &tokens) == SQLITE_OK) s->input_tokens += tokens;
  if (json_extract_int64(s->db, response, "$.usage.output_tokens", &tokens) == SQLITE_OK) s->output_tokens += tokens;
  s->rows_evaluated += count;
  goto done;
nomem:
  if (!*error) *error = sqlite3_mprintf("jev: out of memory");
  rc = SQLITE_NOMEM;
done:
  sqlite3_free(response); buf_reset(&body); return rc;
}

static int evaluate_rows(JevState *s, InputRow *inputs, int input_count,
                         const char *question, const char *kind, const char *criteria,
                         JevRow **rows_out, char **error) {
  JevRow *rows;
  int i, rc = SQLITE_OK;
  rows = sqlite3_malloc64(sizeof(*rows) * (input_count ? input_count : 1));
  if (!rows) return SQLITE_NOMEM;
  memset(rows, 0, sizeof(*rows) * (input_count ? input_count : 1));
  for (i = 0; i < input_count; ) {
    int j, pending_count = 0;
    InputRow *pending = sqlite3_malloc64(sizeof(*pending) * s->batch_size);
    int *positions = sqlite3_malloc64(sizeof(*positions) * s->batch_size);
    if (!pending || !positions) { sqlite3_free(pending); sqlite3_free(positions); rc = SQLITE_NOMEM; goto fail; }
    for (j = 0; j < s->batch_size && i < input_count; j++, i++) {
      char *key = row_cache_key(s, &inputs[i], question, kind, criteria);
      const char *cached = key ? cache_get(s, key) : NULL;
      if (cached) {
        rows[i].source_rowid = inputs[i].rowid;
        rc = parse_answer(s, &rows[i], cached);
      } else {
        pending[pending_count] = inputs[i];
        positions[pending_count++] = i;
      }
      sqlite3_free(key);
      if (rc != SQLITE_OK) { sqlite3_free(pending); sqlite3_free(positions); goto fail; }
    }
    if (pending_count) {
      JevRow *tmp = sqlite3_malloc64(sizeof(*tmp) * pending_count);
      if (!tmp) { sqlite3_free(pending); sqlite3_free(positions); rc = SQLITE_NOMEM; goto fail; }
      memset(tmp, 0, sizeof(*tmp) * pending_count);
      rc = eval_batch(s, pending, 0, pending_count, question, kind, criteria, tmp, error);
      if (rc == SQLITE_OK) {
        for (j = 0; j < pending_count; j++) rows[positions[j]] = tmp[j];
      }
      sqlite3_free(tmp);
      if (rc != SQLITE_OK) { sqlite3_free(pending); sqlite3_free(positions); goto fail; }
    }
    sqlite3_free(pending); sqlite3_free(positions);
  }
  *rows_out = rows;
  return SQLITE_OK;
fail:
  for (i = 0; i < input_count; i++) { sqlite3_free(rows[i].answer); sqlite3_free(rows[i].choice); }
  sqlite3_free(rows);
  if (rc == SQLITE_NOMEM && !*error) *error = sqlite3_mprintf("jev: out of memory");
  return rc;
}

static int vtab_filter(sqlite3_vtab_cursor *cursor, int idx_num, const char *idx_str,
                       int argc, sqlite3_value **argv) {
  JevCursor *c = (JevCursor *)cursor;
  JevVtab *v = (JevVtab *)cursor->pVtab;
  const char *args[5] = {NULL, NULL, "noul", NULL, NULL};
  char **columns = NULL, *error = NULL;
  InputRow *inputs = NULL;
  int column_count = 0, input_count = 0, arg = 0, bit, i, rc;
  (void)idx_str; (void)argc;
  cursor_reset(c);
  for (bit = 0; bit < 5; bit++) if (idx_num & (1 << bit)) args[bit] = value_text_or(argv[arg++], bit == 2 ? "noul" : NULL);
  if (!args[0] || !args[1]) {
    v->base.zErrMsg = sqlite3_mprintf("jev_rows requires table_name and question");
    return SQLITE_CONSTRAINT;
  }
  if (!valid_identifier(args[0])) {
    v->base.zErrMsg = sqlite3_mprintf("jev: table_name must be a simple SQLite identifier");
    return SQLITE_ERROR;
  }
  rc = validate_question(v->state, args[2], args[3], &error);
  if (rc != SQLITE_OK) goto done;
  rc = load_column_names(v->state, args[0], args[4], &columns, &column_count, &error);
  if (rc != SQLITE_OK) goto done;
  rc = load_input_rows(v->state, args[0], columns, column_count, &inputs, &input_count, &error);
  if (rc != SQLITE_OK) goto done;
  rc = evaluate_rows(v->state, inputs, input_count, args[1], args[2], args[3], &c->rows, &error);
  if (rc == SQLITE_OK) c->count = input_count;
done:
  for (i = 0; i < column_count; i++) sqlite3_free(columns[i]);
  sqlite3_free(columns);
  free_input_rows(inputs, input_count);
  if (rc != SQLITE_OK) {
    v->base.zErrMsg = error ? error : sqlite3_mprintf("jev: evaluation failed");
    if (rc == SQLITE_NOMEM) return SQLITE_NOMEM;
    return SQLITE_ERROR;
  }
  sqlite3_free(error);
  return SQLITE_OK;
}

static int vtab_next(sqlite3_vtab_cursor *cursor) { ((JevCursor *)cursor)->index++; return SQLITE_OK; }
static int vtab_eof(sqlite3_vtab_cursor *cursor) { JevCursor *c = (JevCursor *)cursor; return c->index >= c->count; }

static int vtab_column(sqlite3_vtab_cursor *cursor, sqlite3_context *ctx, int col) {
  JevCursor *c = (JevCursor *)cursor;
  JevRow *r = &c->rows[c->index];
  switch (col) {
    case COL_SOURCE_ROWID: sqlite3_result_int64(ctx, r->source_rowid); break;
    case COL_ANSWER: sqlite3_result_text(ctx, r->answer, -1, SQLITE_TRANSIENT); sqlite3_result_subtype(ctx, 74); break;
    case COL_PROBABILITY: if (r->has_probability) sqlite3_result_double(ctx, r->probability); else sqlite3_result_null(ctx); break;
    case COL_CHOICE: if (r->choice) sqlite3_result_text(ctx, r->choice, -1, SQLITE_TRANSIENT); else sqlite3_result_null(ctx); break;
    case COL_SCORE: if (r->has_score) sqlite3_result_double(ctx, r->score); else sqlite3_result_null(ctx); break;
    case COL_CONFIDENCE: if (r->has_confidence) sqlite3_result_double(ctx, r->confidence); else sqlite3_result_null(ctx); break;
    default: sqlite3_result_null(ctx);
  }
  return SQLITE_OK;
}

static int vtab_rowid(sqlite3_vtab_cursor *cursor, sqlite3_int64 *rowid) {
  JevCursor *c = (JevCursor *)cursor;
  *rowid = c->index;
  return SQLITE_OK;
}

static sqlite3_module JevModule = {
  3, NULL, vtab_connect, vtab_best_index, vtab_disconnect, vtab_disconnect,
  vtab_open, vtab_close, vtab_filter, vtab_next, vtab_eof, vtab_column, vtab_rowid,
  NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL
};

static int register_function(sqlite3 *db, const char *name, int nargs, void *state,
                             void (*fn)(sqlite3_context *, int, sqlite3_value **)) {
  return sqlite3_create_function_v2(db, name, nargs, SQLITE_UTF8 | SQLITE_RESULT_SUBTYPE,
                                    state, fn, NULL, NULL, NULL);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_jev_init(sqlite3 *db, char **error, const sqlite3_api_routines *api) {
  JevState *s;
  const char *key;
  int rc;
  SQLITE_EXTENSION_INIT2(api);
  s = sqlite3_malloc64(sizeof(*s));
  if (!s) return SQLITE_NOMEM;
  memset(s, 0, sizeof(*s));
  s->db = db;
  s->api_url = strdup_sqlite(JEV_DEFAULT_URL);
  s->model = strdup_sqlite(JEV_DEFAULT_MODEL);
  s->batch_size = JEV_DEFAULT_BATCH_SIZE;
  s->max_rows = JEV_DEFAULT_MAX_ROWS;
  s->timeout = JEV_DEFAULT_TIMEOUT;
  key = env_api_key();
  if (key) s->api_key = strdup_sqlite(key);
  if (!s->api_url || !s->model || (key && !s->api_key)) { state_free(s); return SQLITE_NOMEM; }
  rc = curl_load(&s->curl, error);
  if (rc != SQLITE_OK) { state_free(s); return rc; }
#define REG(name, n, fn) do { rc = register_function(db, name, n, s, fn); if (rc != SQLITE_OK) goto fail; } while (0)
  REG("jev_eval", 2, fn_eval); REG("jev_eval", 3, fn_eval); REG("jev_eval", 4, fn_eval);
  REG("jev_prob", 2, fn_prob);
  REG("jev", 2, fn_jev); REG("jev", 3, fn_jev);
  REG("jev_choice", 3, fn_choice);
  REG("jev_score", 3, fn_score); REG("jev_score_norm", 3, fn_score_norm);
  REG("jev_confidence", 4, fn_confidence);
  REG("jev_version", 0, fn_version); REG("jev_stats", 0, fn_stats);
  REG("jev_cache_clear", 0, fn_cache_clear);
  REG("jev_config", 1, fn_config); REG("jev_config", 2, fn_config);
#undef REG
  rc = sqlite3_create_module_v2(db, "jev_rows", &JevModule, s, state_free);
  if (rc != SQLITE_OK) goto fail_no_free;
  return SQLITE_OK;
fail:
  state_free(s);
fail_no_free:
  return rc;
}
