#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include "cisv/parser.h"
#include "cisv/writer.h"
#include "cisv/transformer.h"
#include "cisv/operations.h"

static int test_count = 0;
static int pass_count = 0;

#define TEST(name) do { \
    test_count++; \
    printf("  Testing: %s... ", name); \
} while(0)

#define PASS() do { \
    pass_count++; \
    printf("PASS\n"); \
} while(0)

#define FAIL(msg) do { \
    printf("FAIL: %s\n", msg); \
} while(0)

// Test data
static int field_count = 0;
static int row_count = 0;
static int error_count = 0;
static char last_field[1024];

// Stored fields for multiline tests
#define MAX_STORED_FIELDS 64
static char stored_fields[MAX_STORED_FIELDS][4096];
static size_t stored_field_lens[MAX_STORED_FIELDS];
static int stored_field_count = 0;

static void test_field_cb(void *user, const char *data, size_t len) {
    (void)user;
    field_count++;
    if (len < sizeof(last_field)) {
        memcpy(last_field, data, len);
        last_field[len] = '\0';
    }
    if (stored_field_count < MAX_STORED_FIELDS && len < sizeof(stored_fields[0])) {
        memcpy(stored_fields[stored_field_count], data, len);
        stored_fields[stored_field_count][len] = '\0';
        stored_field_lens[stored_field_count] = len;
        stored_field_count++;
    }
}

static void test_row_cb(void *user) {
    (void)user;
    row_count++;
}

static void test_error_cb(void *user, int line, const char *msg) {
    (void)user;
    (void)line;
    (void)msg;
    error_count++;
}

static void reset_test_state(void) {
    field_count = 0;
    row_count = 0;
    error_count = 0;
    stored_field_count = 0;
    memset(last_field, 0, sizeof(last_field));
}

// Helper to create a temp file from a string
static const char* write_temp_csv(const char *content) {
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cisv_core_%d.csv", getpid());
    FILE *f = fopen(path, "w");
    if (!f) return NULL;
    fwrite(content, 1, strlen(content), f);
    fclose(f);
    return path;
}

static int write_temp_csv_named(const char *name, const char *content, char *path, size_t path_len) {
    if (!name || !content || !path || path_len == 0) return -1;
    if (snprintf(path, path_len, "/tmp/test_cisv_core_%d_%s.csv", getpid(), name) >= (int)path_len) {
        return -1;
    }
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t len = strlen(content);
    int write_ok = fwrite(content, 1, len, f) == len;
    int close_ok = fclose(f) == 0;
    int ok = write_ok && close_ok;
    if (!ok) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int read_tmpfile_string(FILE *f, char *buf, size_t buf_len) {
    if (!f || !buf || buf_len == 0) return -1;
    if (fflush(f) != 0 || fseek(f, 0, SEEK_SET) != 0) return -1;
    size_t n = fread(buf, 1, buf_len - 1, f);
    if (ferror(f)) return -1;
    buf[n] = '\0';
    return 0;
}

static char *save_env_value(const char *name) {
    const char *value = getenv(name);
    if (!value) return NULL;
    size_t len = strlen(value);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, value, len + 1);
    return copy;
}

static void restore_env_value(const char *name, char *value) {
    if (value) {
        setenv(name, value, 1);
        free(value);
    } else {
        unsetenv(name);
    }
}

static int count_open_fds(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) != -1) {
            count++;
        }
    }
    return count;
}

static const char *describe_field(const char *value) {
    static char preview[160];

    if (!value) {
        return "<null>";
    }

    snprintf(preview, sizeof(preview), "%.120s%s", value,
             strlen(value) > 120 ? "..." : "");
    return preview;
}

// Test: Config initialization
void test_config_init(void) {
    TEST("config initialization");

    cisv_config config;
    cisv_config_init(&config);

    if (config.delimiter == ',' &&
        config.quote == '"' &&
        config.from_line == 1) {
        PASS();
    } else {
        FAIL("default config values incorrect");
    }
}

// Test: Parser creation/destruction
void test_parser_lifecycle(void) {
    TEST("parser lifecycle");

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create parser");
        return;
    }

    cisv_parser_destroy(parser);
    PASS();
}

void test_invalid_parser_config_rejected(void) {
    TEST("invalid parser config rejected");

    cisv_config config;
    cisv_config_init(&config);

    int ok = 1;

    config.delimiter = '"';
    config.quote = '"';
    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (parser) {
        cisv_parser_destroy(parser);
        ok = 0;
    }

    cisv_config_init(&config);
    config.delimiter = '\n';
    parser = cisv_parser_create_with_config(&config);
    if (parser) {
        cisv_parser_destroy(parser);
        ok = 0;
    }

    cisv_config_init(&config);
    config.escape = '"';
    parser = cisv_parser_create_with_config(&config);
    if (parser) {
        cisv_parser_destroy(parser);
        ok = 0;
    }

    cisv_config_init(&config);
    config.to_line = 1;
    config.from_line = 2;
    parser = cisv_parser_create_with_config(&config);
    if (parser) {
        cisv_parser_destroy(parser);
        ok = 0;
    }

    if (ok) {
        PASS();
    } else {
        FAIL("accepted invalid parser config");
    }
}

void test_resource_env_row_limit_and_override(void) {
    TEST("resource env row limit and explicit override");

    char *old_row = save_env_value("CISV_MAX_ROW_SIZE");
    setenv("CISV_MAX_ROW_SIZE", "8", 1);

    const char *csv = "123456789,x\n";

    cisv_config config;
    cisv_config_init(&config);
    cisv_result_t *limited = cisv_parse_string_batch(csv, strlen(csv), &config);

    cisv_config_init(&config);
    config.max_row_size = 64;
    cisv_result_t *explicit_ok = cisv_parse_string_batch(csv, strlen(csv), &config);

    restore_env_value("CISV_MAX_ROW_SIZE", old_row);

    int ok = limited &&
             limited->error_code != 0 &&
             explicit_ok &&
             explicit_ok->error_code == 0 &&
             explicit_ok->row_count == 1 &&
             explicit_ok->total_fields == 2;

    cisv_result_free(limited);
    cisv_result_free(explicit_ok);

    if (ok) {
        PASS();
    } else {
        FAIL("env row limit did not apply or explicit max_row_size did not override it");
    }
}

void test_gomemlimit_adaptive_row_limit(void) {
    TEST("GOMEMLIMIT adaptive row limit and explicit override");

    char *old_cisv_memory = save_env_value("CISV_MAX_MEMORY");
    char *old_go_memory = save_env_value("GOMEMLIMIT");
    char *old_row = save_env_value("CISV_MAX_ROW_SIZE");
    unsetenv("CISV_MAX_MEMORY");
    unsetenv("CISV_MAX_ROW_SIZE");
    setenv("GOMEMLIMIT", "1MiB", 1);

    const size_t field_len = 70000;
    char *csv = malloc(field_len + 4);
    if (!csv) {
        restore_env_value("CISV_MAX_MEMORY", old_cisv_memory);
        restore_env_value("GOMEMLIMIT", old_go_memory);
        restore_env_value("CISV_MAX_ROW_SIZE", old_row);
        FAIL("failed to allocate test csv");
        return;
    }
    memset(csv, 'x', field_len);
    memcpy(csv + field_len, ",y\n", 4);

    cisv_config config;
    cisv_config_init(&config);
    cisv_result_t *limited = cisv_parse_string_batch(csv, strlen(csv), &config);

    cisv_config_init(&config);
    config.max_row_size = 100000;
    cisv_result_t *explicit_ok = cisv_parse_string_batch(csv, strlen(csv), &config);

    free(csv);
    restore_env_value("CISV_MAX_MEMORY", old_cisv_memory);
    restore_env_value("GOMEMLIMIT", old_go_memory);
    restore_env_value("CISV_MAX_ROW_SIZE", old_row);

    int ok = limited &&
             limited->error_code != 0 &&
             explicit_ok &&
             explicit_ok->error_code == 0 &&
             explicit_ok->row_count == 1 &&
             explicit_ok->total_fields == 2;

    cisv_result_free(limited);
    cisv_result_free(explicit_ok);

    if (ok) {
        PASS();
    } else {
        FAIL("GOMEMLIMIT adaptive row limit did not fail or explicit override did not pass");
    }
}

void test_resource_env_max_procs_clamps_parallel(void) {
    TEST("resource env max procs clamps parallel parse");

    char *old_cisv = save_env_value("CISV_MAX_PROCS");
    char *old_go = save_env_value("GOMAXPROCS");
    unsetenv("CISV_MAX_PROCS");
    setenv("GOMAXPROCS", "1", 1);

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cisv_env_procs_%d.csv", getpid());
    FILE *f = fopen(path, "w");
    if (!f) {
        restore_env_value("CISV_MAX_PROCS", old_cisv);
        restore_env_value("GOMAXPROCS", old_go);
        FAIL("failed to create temp file");
        return;
    }
    fputs("a,b\n", f);
    for (int i = 0; i < 5000; i++) {
        fprintf(f, "%d,%d\n", i, i + 1);
    }
    fclose(f);

    cisv_config config;
    cisv_config_init(&config);

    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(path, &config, 0, &result_count);

    restore_env_value("CISV_MAX_PROCS", old_cisv);
    restore_env_value("GOMAXPROCS", old_go);
    unlink(path);

    int ok = results && result_count == 1 && results[0] &&
             results[0]->error_code == 0 &&
             results[0]->row_count == 5001;

    cisv_results_free(results, result_count);

    if (ok) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected one parallel result, got count=%d", result_count);
        FAIL(buf);
    }
}

void test_iterator_resource_row_limit(void) {
    TEST("iterator resource row limit");

    char *old_row = save_env_value("CISV_MAX_ROW_SIZE");
    setenv("CISV_MAX_ROW_SIZE", "8", 1);

    const char *path = write_temp_csv("123456789,x\n");
    if (!path) {
        restore_env_value("CISV_MAX_ROW_SIZE", old_row);
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);
    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        restore_env_value("CISV_MAX_ROW_SIZE", old_row);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    int rc = cisv_iterator_next(it, &fields, &lengths, &field_count);
    cisv_iterator_close(it);

    config.max_row_size = 64;
    it = cisv_iterator_open(path, &config);
    int rc_override = CISV_ITER_ERROR;
    if (it) {
        rc_override = cisv_iterator_next(it, &fields, &lengths, &field_count);
        cisv_iterator_close(it);
    }

    unlink(path);
    restore_env_value("CISV_MAX_ROW_SIZE", old_row);

    if (rc == CISV_ITER_ERROR && rc_override == CISV_ITER_OK && field_count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected iterator limit error then override success, got rc=%d override=%d fields=%zu",
                 rc, rc_override, field_count);
        FAIL(buf);
    }
}

void test_iterator_custom_escape_values(void) {
    TEST("iterator custom escape preserves values");

    const char *path = write_temp_csv("\"a\\\"b\",c\n");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    int rc = cisv_iterator_next(it, &fields, &lengths, &field_count);
    char first_field[32] = {0};
    if (fields && field_count > 0) {
        snprintf(first_field, sizeof(first_field), "%s", fields[0]);
    }

    int success = (rc == CISV_ITER_OK &&
                   field_count == 2 &&
                   lengths[0] == 3 &&
                   strcmp(fields[0], "a\"b") == 0 &&
                   strcmp(fields[1], "c") == 0);

    cisv_iterator_close(it);
    unlink(path);

    if (success) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected escaped quote values, got rc=%d fields=%zu first='%s'",
                 rc, field_count, first_field);
        FAIL(buf);
    }
}

void test_iterator_custom_escape_eof_error(void) {
    TEST("iterator custom escape at EOF errors");

    const char *path = write_temp_csv("\"a\\");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    int rc = cisv_iterator_next(it, &fields, &lengths, &field_count);

    cisv_iterator_close(it);
    unlink(path);

    if (rc == CISV_ITER_ERROR) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected iterator error, got rc=%d fields=%zu", rc, field_count);
        FAIL(buf);
    }
}

void test_iterator_malformed_quote_errors(void) {
    TEST("iterator strict malformed quoted fields error");

    const char *path = write_temp_csv("\"a\"x,b\n");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    int rc = cisv_iterator_next(it, &fields, &lengths, &field_count);
    cisv_iterator_close(it);

    config.relaxed = true;
    it = cisv_iterator_open(path, &config);
    int relaxed_rc = CISV_ITER_ERROR;
    size_t relaxed_fields = 0;
    if (it) {
        relaxed_rc = cisv_iterator_next(it, &fields, &lengths, &relaxed_fields);
        cisv_iterator_close(it);
    }

    unlink(path);

    if (rc == CISV_ITER_ERROR &&
        relaxed_rc == CISV_ITER_OK &&
        relaxed_fields == 3) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected strict error and relaxed recovery, got rc=%d relaxed=%d relaxed_fields=%zu",
                 rc, relaxed_rc, relaxed_fields);
        FAIL(buf);
    }
}

void test_iterator_bare_quote_strict_and_relaxed(void) {
    TEST("iterator bare quote strict error and relaxed literal");

    const char *path = write_temp_csv("a\"b,c\n");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count_local = 0;
    int strict_rc = cisv_iterator_next(it, &fields, &lengths, &field_count_local);
    cisv_iterator_close(it);

    config.relaxed = true;
    it = cisv_iterator_open(path, &config);
    int relaxed_rc = CISV_ITER_ERROR;
    size_t relaxed_fields = 0;
    char first_field[16] = {0};
    if (it) {
        relaxed_rc = cisv_iterator_next(it, &fields, &lengths, &relaxed_fields);
        if (relaxed_rc == CISV_ITER_OK && relaxed_fields > 0 && lengths[0] < sizeof(first_field)) {
            memcpy(first_field, fields[0], lengths[0]);
            first_field[lengths[0]] = '\0';
        }
        cisv_iterator_close(it);
    }

    unlink(path);

    if (strict_rc == CISV_ITER_ERROR &&
        relaxed_rc == CISV_ITER_OK &&
        relaxed_fields == 2 &&
        strcmp(first_field, "a\"b") == 0) {
        PASS();
    } else {
        char buf[180];
        snprintf(buf, sizeof(buf), "expected strict error and relaxed [a\\\"b,c], got strict=%d relaxed=%d fields=%zu first='%s'",
                 strict_rc, relaxed_rc, relaxed_fields, first_field);
        FAIL(buf);
    }
}

void test_iterator_unterminated_quote_errors(void) {
    TEST("iterator strict unterminated quote errors");

    const char *path = write_temp_csv("\"unterminated");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    int rc = cisv_iterator_next(it, &fields, &lengths, &field_count);
    cisv_iterator_close(it);

    config.relaxed = true;
    it = cisv_iterator_open(path, &config);
    int relaxed_rc = CISV_ITER_ERROR;
    size_t relaxed_fields = 0;
    if (it) {
        relaxed_rc = cisv_iterator_next(it, &fields, &lengths, &relaxed_fields);
        cisv_iterator_close(it);
    }

    unlink(path);

    if (rc == CISV_ITER_ERROR &&
        relaxed_rc == CISV_ITER_OK &&
        relaxed_fields == 1) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected strict EOF error and relaxed recovery, got rc=%d relaxed=%d relaxed_fields=%zu",
                 rc, relaxed_rc, relaxed_fields);
        FAIL(buf);
    }
}

void test_iterator_cr_only_line_endings(void) {
    TEST("iterator supports CR-only line endings");

    const char *path = write_temp_csv("a,b\r\"line1\rline2\",c\r");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;

    int rc1 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int first_ok = (rc1 == CISV_ITER_OK &&
                    field_count == 2 &&
                    strcmp(fields[0], "a") == 0 &&
                    strcmp(fields[1], "b") == 0);

    int rc2 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int second_ok = (rc2 == CISV_ITER_OK &&
                     field_count == 2 &&
                     lengths[0] == strlen("line1\rline2") &&
                     memcmp(fields[0], "line1\rline2", lengths[0]) == 0 &&
                     strcmp(fields[1], "c") == 0);

    int rc3 = cisv_iterator_next(it, &fields, &lengths, &field_count);

    cisv_iterator_close(it);
    unlink(path);

    if (first_ok && second_ok && rc3 == CISV_ITER_EOF) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected two CR rows then EOF, got rc1=%d rc2=%d rc3=%d fields=%zu",
                 rc1, rc2, rc3, field_count);
        FAIL(buf);
    }
}

void test_iterator_row_controls(void) {
    TEST("iterator respects comments and line ranges");

    const char *path = write_temp_csv("  #skip\nid,msg\n1,\"hello \\\"quoted\\\"\"\n2,tail\n");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';
    config.comment = '#';
    config.trim = true;
    config.from_line = 1;
    config.to_line = 3;

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    int rc1 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int first_ok = (rc1 == CISV_ITER_OK &&
                    field_count == 2 &&
                    strcmp(fields[0], "id") == 0 &&
                    strcmp(fields[1], "msg") == 0);

    int rc2 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int second_ok = (rc2 == CISV_ITER_OK &&
                     field_count == 2 &&
                     lengths[1] == 14 &&
                     strcmp(fields[0], "1") == 0 &&
                     strcmp(fields[1], "hello \"quoted\"") == 0);

    int rc3 = cisv_iterator_next(it, &fields, &lengths, &field_count);

    cisv_iterator_close(it);
    unlink(path);

    if (first_ok && second_ok && rc3 == CISV_ITER_EOF) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected two filtered rows then EOF, got rc1=%d rc2=%d rc3=%d fields=%zu",
                 rc1, rc2, rc3, field_count);
        FAIL(buf);
    }
}

void test_iterator_skip_empty_and_quoted_comment_parity(void) {
    TEST("iterator skip_empty keeps quoted empty and quoted comment rows");

    const char *path = write_temp_csv("\n\"\"\n#skip\n\"#keep\"\n,,\n");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);
    config.comment = '#';
    config.skip_empty_lines = true;

    cisv_iterator_t *it = cisv_iterator_open(path, &config);
    if (!it) {
        unlink(path);
        FAIL("failed to open iterator");
        return;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;

    int rc1 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int first_ok = (rc1 == CISV_ITER_OK &&
                    field_count == 1 &&
                    lengths[0] == 0 &&
                    strcmp(fields[0], "") == 0);

    int rc2 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int second_ok = (rc2 == CISV_ITER_OK &&
                     field_count == 1 &&
                     strcmp(fields[0], "#keep") == 0);

    int rc3 = cisv_iterator_next(it, &fields, &lengths, &field_count);
    int third_ok = (rc3 == CISV_ITER_OK &&
                    field_count == 3 &&
                    lengths[0] == 0 &&
                    lengths[1] == 0 &&
                    lengths[2] == 0);

    int rc4 = cisv_iterator_next(it, &fields, &lengths, &field_count);

    cisv_iterator_close(it);
    unlink(path);

    if (first_ok && second_ok && third_ok && rc4 == CISV_ITER_EOF) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected quoted empty/comment and empty fields, got rc1=%d rc2=%d rc3=%d rc4=%d",
                 rc1, rc2, rc3, rc4);
        FAIL(buf);
    }
}

// Test: Parse simple CSV string
void test_parse_simple(void) {
    TEST("parse simple CSV");

    field_count = 0;
    row_count = 0;

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create parser");
        return;
    }

    const char *csv = "a,b,c\n1,2,3\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);

    cisv_parser_destroy(parser);

    if (field_count == 6 && row_count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 6 fields, 2 rows; got %d fields, %d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

// Test: Parse with custom delimiter
void test_parse_custom_delimiter(void) {
    TEST("parse with custom delimiter");

    field_count = 0;
    row_count = 0;

    cisv_config config;
    cisv_config_init(&config);
    config.delimiter = ';';
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create parser");
        return;
    }

    const char *csv = "a;b;c\n1;2;3\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);

    cisv_parser_destroy(parser);

    if (field_count == 6 && row_count == 2) {
        PASS();
    } else {
        FAIL("incorrect field/row count");
    }
}

// Test: Parse quoted fields
void test_parse_quoted(void) {
    TEST("parse quoted fields");

    field_count = 0;
    row_count = 0;
    memset(last_field, 0, sizeof(last_field));

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create parser");
        return;
    }

    const char *csv = "\"hello, world\",b\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);

    cisv_parser_destroy(parser);

    if (field_count == 2 && row_count == 1) {
        PASS();
    } else {
        FAIL("incorrect field/row count for quoted");
    }
}

void test_parse_quote_inside_unquoted_field(void) {
    TEST("quote inside unquoted field strict error and relaxed literal");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create parser");
        return;
    }

    const char *strict_csv = "a\"b,c\n";
    int strict_rc = cisv_parser_write(parser, (const uint8_t *)strict_csv, strlen(strict_csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);
    int strict_errors = error_count;

    reset_test_state();
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;
    config.relaxed = true;

    parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create relaxed parser");
        return;
    }

    const char *relaxed_csv = "a\"b\",c\n";
    int relaxed_rc = cisv_parser_write(parser, (const uint8_t *)relaxed_csv, strlen(relaxed_csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (strict_rc < 0 && strict_errors == 1 &&
        relaxed_rc == 0 &&
        field_count == 2 && row_count == 1 &&
        strcmp(stored_fields[0], "a\"b\"") == 0 &&
        strcmp(stored_fields[1], "c") == 0) {
        PASS();
    } else {
        char buf[220];
        snprintf(buf, sizeof(buf),
                 "expected strict error and relaxed [a\\\"b\\\", c], got strict_rc=%d strict_errors=%d relaxed_rc=%d fields=%d rows=%d",
                 strict_rc, strict_errors, relaxed_rc, field_count, row_count);
        FAIL(buf);
    }
}

void test_parse_backslash_escaped_json(void) {
    TEST("backslash escaped multiline JSON");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        FAIL("failed to create parser");
        return;
    }

    const char *csv =
        "id,payload\n"
        "1,\"{\\\n"
        "  \\\"name\\\": \\\"alice\\\",\\\n"
        "  \\\"nested\\\": {\\\"ok\\\": true}\\\n"
        "}\"" "\n";

    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2 &&
        strcmp(stored_fields[2], "1") == 0 &&
        strcmp(stored_fields[3], "{\n  \"name\": \"alice\",\n  \"nested\": {\"ok\": true}\n}") == 0) {
        PASS();
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf), "unexpected escaped JSON parse result: fields=%d rows=%d payload='%s'",
                 field_count, row_count,
                 describe_field(stored_field_count > 3 ? stored_fields[3] : NULL));
        FAIL(buf);
    }
}

// Test: Transformer uppercase
void test_transform_uppercase(void) {
    TEST("transform uppercase");

    cisv_transform_result_t result = cisv_transform_uppercase("hello", 5, NULL);

    if (result.len == 5 && strcmp(result.data, "HELLO") == 0) {
        PASS();
    } else {
        FAIL("uppercase transform failed");
    }

    cisv_transform_result_free(&result);
}

// Test: Transformer lowercase
void test_transform_lowercase(void) {
    TEST("transform lowercase");

    cisv_transform_result_t result = cisv_transform_lowercase("WORLD", 5, NULL);

    if (result.len == 5 && strcmp(result.data, "world") == 0) {
        PASS();
    } else {
        FAIL("lowercase transform failed");
    }

    cisv_transform_result_free(&result);
}

// Test: Transformer trim
void test_transform_trim(void) {
    TEST("transform trim");

    cisv_transform_result_t result = cisv_transform_trim("  hello  ", 9, NULL);

    if (result.len == 5 && strcmp(result.data, "hello") == 0) {
        PASS();
    } else {
        FAIL("trim transform failed");
    }

    cisv_transform_result_free(&result);
}

// Test: Transform pipeline
void test_transform_pipeline(void) {
    TEST("transform pipeline");

    cisv_transform_pipeline_t *pipeline = cisv_transform_pipeline_create(4);
    if (!pipeline) {
        FAIL("failed to create pipeline");
        return;
    }

    cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_UPPERCASE, NULL);
    cisv_transform_pipeline_add(pipeline, 1, TRANSFORM_LOWERCASE, NULL);

    cisv_transform_result_t r1 = cisv_transform_apply(pipeline, 0, "hello", 5);
    cisv_transform_result_t r2 = cisv_transform_apply(pipeline, 1, "WORLD", 5);

    int success = (strcmp(r1.data, "HELLO") == 0 && strcmp(r2.data, "world") == 0);

    cisv_transform_result_free(&r1);
    cisv_transform_result_free(&r2);
    cisv_transform_pipeline_destroy(pipeline);

    if (success) {
        PASS();
    } else {
        FAIL("pipeline transforms incorrect");
    }
}

void test_transform_rejects_unsupported_builtins(void) {
    TEST("transform rejects unsupported builtins");

    cisv_transform_pipeline_t *pipeline = cisv_transform_pipeline_create(2);
    if (!pipeline) {
        FAIL("failed to create pipeline");
        return;
    }

    int rc_none = cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_NONE, NULL);
    int rc_sha = cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_HASH_SHA256, NULL);
    int rc_md5 = cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_HASH_MD5, NULL);
    int rc_decode = cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_BASE64_DECODE, NULL);
    int rc_encrypt = cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_ENCRYPT_AES256, NULL);

    cisv_transform_result_t direct_sha = cisv_transform_hash_sha256("hello", 5, NULL);
    int success = (rc_none < 0 &&
                   rc_sha < 0 &&
                   rc_md5 < 0 &&
                   rc_decode < 0 &&
                   rc_encrypt < 0 &&
                   pipeline->count == 0 &&
                   direct_sha.data == NULL &&
                   direct_sha.len == 0 &&
                   direct_sha.needs_free == 0);

    cisv_transform_result_free(&direct_sha);
    cisv_transform_pipeline_destroy(pipeline);

    if (success) {
        PASS();
    } else {
        FAIL("unsupported transforms must fail instead of no-op/mock behavior");
    }
}

void test_transform_add_by_name_rejects_unsupported(void) {
    TEST("transform by name rejects unsupported builtins");

    cisv_transform_pipeline_t *pipeline = cisv_transform_pipeline_create(2);
    if (!pipeline) {
        FAIL("failed to create pipeline");
        return;
    }

    const char *headers[] = {"id", "value"};
    int header_rc = cisv_transform_pipeline_set_header(pipeline, headers, 2);
    int add_rc = cisv_transform_pipeline_add_by_name(pipeline, "value", TRANSFORM_HASH_SHA256, NULL);

    int success = (header_rc == 0 && add_rc < 0 && pipeline->count == 0);
    cisv_transform_pipeline_destroy(pipeline);

    if (success) {
        PASS();
    } else {
        FAIL("unsupported name-based transform was accepted");
    }
}

void test_transform_remove_field_rebuilds_index(void) {
    TEST("transform remove field rebuilds index");

    cisv_transform_pipeline_t *pipeline = cisv_transform_pipeline_create(2);
    if (!pipeline) {
        FAIL("failed to create pipeline");
        return;
    }

    int add0 = cisv_transform_pipeline_add(pipeline, 0, TRANSFORM_UPPERCASE, NULL);
    int add1 = cisv_transform_pipeline_add(pipeline, 1, TRANSFORM_LOWERCASE, NULL);
    cisv_transform_result_t before = cisv_transform_apply(pipeline, 0, "abc", 3);
    cisv_transform_result_free(&before);

    int removed = cisv_transform_pipeline_remove_field(pipeline, 0);
    cisv_transform_result_t field0 = cisv_transform_apply(pipeline, 0, "abc", 3);
    cisv_transform_result_t field1 = cisv_transform_apply(pipeline, 1, "XYZ", 3);

    int ok = (add0 == 0 && add1 == 0 && removed == 1 &&
              field0.len == 3 && memcmp(field0.data, "abc", 3) == 0 &&
              field1.len == 3 && memcmp(field1.data, "xyz", 3) == 0 &&
              pipeline->count == 1);

    cisv_transform_result_free(&field0);
    cisv_transform_result_free(&field1);
    cisv_transform_pipeline_destroy(pipeline);

    if (ok) {
        PASS();
    } else {
        FAIL("field transform removal did not preserve remaining transforms");
    }
}

void test_transform_remove_by_name(void) {
    TEST("transform remove by name");

    cisv_transform_pipeline_t *pipeline = cisv_transform_pipeline_create(2);
    if (!pipeline) {
        FAIL("failed to create pipeline");
        return;
    }

    const char *headers[] = {"id", "value"};
    int header_rc = cisv_transform_pipeline_set_header(pipeline, headers, 2);
    int add_rc = cisv_transform_pipeline_add_by_name(pipeline, "value", TRANSFORM_UPPERCASE, NULL);
    int removed = cisv_transform_pipeline_remove_by_name(pipeline, "value");
    cisv_transform_result_t result = cisv_transform_apply(pipeline, 1, "abc", 3);

    int ok = (header_rc == 0 && add_rc == 0 && removed == 1 &&
              result.len == 3 && memcmp(result.data, "abc", 3) == 0 &&
              pipeline->count == 0);

    cisv_transform_result_free(&result);
    cisv_transform_pipeline_destroy(pipeline);

    if (ok) {
        PASS();
    } else {
        FAIL("name transform removal failed");
    }
}

// Test: Writer basic
void test_writer_basic(void) {
    TEST("writer basic");

    FILE *tmp = tmpfile();
    if (!tmp) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_writer *writer = cisv_writer_create(tmp);
    if (!writer) {
        fclose(tmp);
        FAIL("failed to create writer");
        return;
    }

    cisv_writer_field_str(writer, "a");
    cisv_writer_field_str(writer, "b");
    cisv_writer_field_str(writer, "c");
    cisv_writer_row_end(writer);

    cisv_writer_field_int(writer, 1);
    cisv_writer_field_int(writer, 2);
    cisv_writer_field_int(writer, 3);
    cisv_writer_row_end(writer);

    cisv_writer_flush(writer);

    // Read back
    fseek(tmp, 0, SEEK_SET);
    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[len] = '\0';

    cisv_writer_destroy(writer);
    fclose(tmp);

    if (strcmp(buf, "a,b,c\n1,2,3\n") == 0) {
        PASS();
    } else {
        FAIL("writer output mismatch");
    }
}

// Test: Writer with quoting
void test_writer_quoting(void) {
    TEST("writer quoting");

    FILE *tmp = tmpfile();
    if (!tmp) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_writer *writer = cisv_writer_create(tmp);
    if (!writer) {
        fclose(tmp);
        FAIL("failed to create writer");
        return;
    }

    cisv_writer_field_str(writer, "hello, world");
    cisv_writer_field_str(writer, "normal");
    cisv_writer_row_end(writer);

    cisv_writer_flush(writer);

    fseek(tmp, 0, SEEK_SET);
    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[len] = '\0';

    cisv_writer_destroy(writer);
    fclose(tmp);

    if (strcmp(buf, "\"hello, world\",normal\n") == 0) {
        PASS();
    } else {
        char msg[300];
        snprintf(msg, sizeof(msg), "got: %s", buf);
        FAIL(msg);
    }
}

void test_writer_long_quoted_field_with_quotes(void) {
    TEST("writer long quoted field with embedded quotes");

    FILE *tmp = tmpfile();
    if (!tmp) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_writer *writer = cisv_writer_create(tmp);
    if (!writer) {
        fclose(tmp);
        FAIL("failed to create writer");
        return;
    }

    char field[96];
    memset(field, 'a', 80);
    field[1] = '"';
    field[40] = '"';

    char expected[256];
    size_t pos = 0;
    expected[pos++] = '"';
    for (size_t i = 0; i < 80; i++) {
        if (field[i] == '"') expected[pos++] = '"';
        expected[pos++] = field[i];
    }
    expected[pos++] = '"';
    expected[pos++] = '\n';
    expected[pos] = '\0';

    cisv_writer_field(writer, field, 80);
    cisv_writer_row_end(writer);
    cisv_writer_flush(writer);

    fseek(tmp, 0, SEEK_SET);
    char buf[256];
    size_t len = fread(buf, 1, sizeof(buf) - 1, tmp);
    buf[len] = '\0';

    cisv_writer_destroy(writer);
    fclose(tmp);

    if (strcmp(buf, expected) == 0) {
        PASS();
    } else {
        FAIL("long quoted writer output mismatch");
    }
}

void test_writer_large_quoted_field_direct_fallback(void) {
    TEST("writer large quoted field direct fallback");

    FILE *tmp = tmpfile();
    if (!tmp) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_writer_config config = {
        .delimiter = ',',
        .quote_char = '"',
        .always_quote = 0,
        .use_crlf = 0,
        .null_string = "",
        .buffer_size = 1024
    };
    cisv_writer *writer = cisv_writer_create_config(tmp, &config);
    if (!writer) {
        fclose(tmp);
        FAIL("failed to create writer");
        return;
    }

    const size_t field_len = 70000;
    char *field = malloc(field_len);
    if (!field) {
        cisv_writer_destroy(writer);
        fclose(tmp);
        FAIL("failed to allocate test field");
        return;
    }
    memset(field, 'a', field_len);
    field[3] = ',';
    field[1024] = '"';
    field[33333] = '"';
    field[field_len - 2] = '"';

    size_t expected_len = field_len + 2 + 3 + 1;
    char *expected = malloc(expected_len + 1);
    if (!expected) {
        free(field);
        cisv_writer_destroy(writer);
        fclose(tmp);
        FAIL("failed to allocate expected field");
        return;
    }
    size_t pos = 0;
    expected[pos++] = '"';
    for (size_t i = 0; i < field_len; i++) {
        if (field[i] == '"') expected[pos++] = '"';
        expected[pos++] = field[i];
    }
    expected[pos++] = '"';
    expected[pos++] = '\n';
    expected[pos] = '\0';

    int rc = cisv_writer_field(writer, field, field_len);
    if (rc == 0) rc = cisv_writer_row_end(writer);
    if (rc == 0) rc = cisv_writer_flush(writer);
    size_t bytes_written = cisv_writer_bytes_written(writer);

    char *buf = malloc(expected_len + 1);
    if (!buf) {
        free(expected);
        free(field);
        cisv_writer_destroy(writer);
        fclose(tmp);
        FAIL("failed to allocate output buffer");
        return;
    }

    fseek(tmp, 0, SEEK_SET);
    size_t len = fread(buf, 1, expected_len, tmp);
    buf[len] = '\0';

    cisv_writer_destroy(writer);
    fclose(tmp);

    int ok = (rc == 0 && len == expected_len && bytes_written == expected_len &&
              memcmp(buf, expected, expected_len) == 0);
    free(buf);
    free(expected);
    free(field);

    if (ok) {
        PASS();
    } else {
        FAIL("large quoted writer fallback output mismatch");
    }
}

void test_writer_config_validation(void) {
    TEST("writer config validation");

    FILE *tmp = tmpfile();
    if (!tmp) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_writer *writer = cisv_writer_create_config(tmp, NULL);
    int ok = writer != NULL;
    cisv_writer_destroy(writer);

    cisv_writer_config config = {
        .delimiter = ',',
        .quote_char = ',',
        .always_quote = 0,
        .use_crlf = 0,
        .null_string = "",
        .buffer_size = 0
    };

    writer = cisv_writer_create_config(tmp, &config);
    if (writer) {
        cisv_writer_destroy(writer);
        ok = 0;
    }

    config.delimiter = '\n';
    config.quote_char = '"';
    writer = cisv_writer_create_config(tmp, &config);
    if (writer) {
        cisv_writer_destroy(writer);
        ok = 0;
    }

    fclose(tmp);

    if (ok) {
        PASS();
    } else {
        FAIL("writer accepted invalid config or rejected NULL defaults");
    }
}

// Test: Base64 encode
void test_base64_encode(void) {
    TEST("base64 encode");

    cisv_transform_result_t result = cisv_transform_base64_encode("Hello", 5, NULL);

    if (strcmp(result.data, "SGVsbG8=") == 0) {
        PASS();
    } else {
        FAIL("base64 encode failed");
    }

    cisv_transform_result_free(&result);
}

// =============================================================================
// Multiline CSV Tests (Issue #108)
// =============================================================================

void test_parse_multiline_quoted(void) {
    TEST("parse multiline quoted field");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,b\n\"line1\nline2\",c\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2) {
        // Verify the multiline field content
        if (strcmp(stored_fields[2], "line1\nline2") == 0) {
            PASS();
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "multiline field mismatch: got '%s'",
                     describe_field(stored_fields[2]));
            FAIL(buf);
        }
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 4 fields, 2 rows; got %d fields, %d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_parse_multiline_row_count(void) {
    TEST("multiline row count via cisv_parser_count_rows");

    const char *csv = "h1,h2\n\"line1\nline2\nline3\",simple\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_parse_multiline_multiple_fields(void) {
    TEST("multiple multiline fields in same row");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,b,c\n\"x\ny\",middle,\"p\nq\nr\"\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 6 && row_count == 2) {
        if (strcmp(stored_fields[3], "x\ny") == 0 &&
            strcmp(stored_fields[4], "middle") == 0 &&
            strcmp(stored_fields[5], "p\nq\nr") == 0) {
            PASS();
        } else {
            FAIL("field content mismatch");
        }
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 6 fields, 2 rows; got %d fields, %d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_parse_multiline_escaped_quotes(void) {
    TEST("multiline with escaped quotes");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    // Field: she said "hi"\nthen left
    const char *csv = "a,b\n\"she said \"\"hi\"\"\nthen left\",ok\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2) {
        if (strcmp(stored_fields[2], "she said \"hi\"\nthen left") == 0) {
            PASS();
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "field mismatch: got '%s'",
                     describe_field(stored_fields[2]));
            FAIL(buf);
        }
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 4 fields, 2 rows; got %d fields, %d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_parse_multiline_crlf(void) {
    TEST("multiline with CRLF inside quotes");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,b\r\n\"line1\r\nline2\",c\r\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 4 fields, 2 rows; got %d fields, %d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_parse_cr_only_line_endings(void) {
    TEST("CR-only row endings with quoted CR data");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,b\r\"line1\rline2\",c\r";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2 &&
        strcmp(stored_fields[2], "line1\rline2") == 0) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected 4 fields, 2 rows and quoted CR data; got %d fields, %d rows, field='%s'",
                 field_count, row_count, describe_field(stored_fields[2]));
        FAIL(buf);
    }
}

void test_parse_multiline_empty_lines(void) {
    TEST("consecutive newlines inside quotes");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,b\n\"line1\n\n\nline4\",c\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2) {
        if (strcmp(stored_fields[2], "line1\n\n\nline4") == 0) {
            PASS();
        } else {
            char buf[256];
            snprintf(buf, sizeof(buf), "field mismatch: got '%s'",
                     describe_field(stored_fields[2]));
            FAIL(buf);
        }
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 4 fields, 2 rows; got %d fields, %d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_parse_multiline_issue108(void) {
    TEST("issue #108: many multiline fields");

    // Reproduce issue: 1 header + 1 data row with many multiline quoted fields
    // The old bug would count raw \n chars and return 57 instead of 2
    const char *csv =
        "\"h1\",\"h2\",\"h3\",\"h4\",\"h5\"\n"
        "\"a\nb\nc\nd\ne\nf\ng\nh\ni\nj\","
        "\"k\nl\nm\nn\no\np\nq\nr\ns\nt\","
        "\"u\nv\nw\nx\ny\nz\n1\n2\n3\n4\","
        "\"5\n6\n7\n8\n9\n0\na\nb\nc\nd\","
        "\"e\nf\ng\nh\ni\nj\nk\nl\nm\nn\"\n";

    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu (old bug returned 57)", count);
        FAIL(buf);
    }
}

void test_count_rows_with_config_custom_quote(void) {
    TEST("count_rows_with_config custom quote char");

    // Use ' as quote char instead of "
    const char *csv = "h1,h2\n'line1\nline2',simple\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.quote = '\'';

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_with_escape_config(void) {
    TEST("count_rows_with_config escape char");

    const char *csv =
        "id,payload\n"
        "1,\"{\\\n"
        "  \\\"name\\\": \\\"alice\\\",\\\n"
        "  \\\"nested\\\": {\\\"ok\\\": true}\\\n"
        "}\"\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_with_row_controls(void) {
    TEST("count_rows_with_config respects row controls");

    const char *csv = "#comment\n\nh1,h2\n1,2\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.comment = '#';
    config.skip_empty_lines = true;

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_with_line_range_controls(void) {
    TEST("count_rows_with_config respects line ranges");

    const char *csv = "a\nb\nc\nd\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.from_line = 2;
    config.to_line = 3;

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_skip_empty_preserves_empty_fields(void) {
    TEST("count_rows skip_empty keeps rows with empty fields");

    const char *csv = "\n,,\n\"\"\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.skip_empty_lines = true;

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_trimmed_comment_and_quoted_comment(void) {
    TEST("count_rows trims comments but keeps quoted comment field");

    const char *csv = "  #skip\n\"#keep\"\nvalue\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.comment = '#';
    config.trim = true;

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_final_row_without_newline(void) {
    TEST("count_rows final row without trailing newline");

    const char *path = write_temp_csv("a,b\n1,2");
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_cr_only_line_endings(void) {
    TEST("count_rows supports CR-only line endings");

    const char *path = write_temp_csv("a,b\r\"line1\rline2\",c\r");
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 2) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 2, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_mixed_line_endings_no_quotes(void) {
    TEST("count_rows fast path supports mixed LF CRLF CR endings");

    const char *path = write_temp_csv("a,b\r\n1,2\n3,4\rlast,row");
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 4) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 4, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_empty_file(void) {
    TEST("count_rows empty file is zero");

    const char *path = write_temp_csv("");
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 0, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_invalid_config_rejected(void) {
    TEST("count_rows_with_config rejects invalid config");

    const char *path = write_temp_csv("a,b\n1,2\n");
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.delimiter = '"';
    config.quote = '"';

    size_t count = cisv_parser_count_rows_with_config(path, &config);
    unlink(path);

    if (count == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected invalid config to return 0, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_malformed_quote_returns_zero(void) {
    TEST("count_rows quote-aware path rejects malformed quote");

    const char *path = write_temp_csv("a,b\n\"x\"z,1\n");
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t count = cisv_parser_count_rows(path);
    unlink(path);

    if (count == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected malformed count to return 0, got %zu", count);
        FAIL(buf);
    }
}

void test_count_rows_bare_quote_strict_and_relaxed(void) {
    TEST("count_rows bare quote strict error and relaxed count");

    const char *path = write_temp_csv("a,b\n1\"x,2\n");
    if (!path) { FAIL("failed to create temp file"); return; }

    size_t strict_count = cisv_parser_count_rows(path);

    cisv_config config;
    cisv_config_init(&config);
    config.relaxed = true;
    size_t relaxed_count = cisv_parser_count_rows_with_config(path, &config);

    unlink(path);

    if (strict_count == 0 && relaxed_count == 2) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected strict=0 relaxed=2, got strict=%zu relaxed=%zu",
                 strict_count, relaxed_count);
        FAIL(buf);
    }
}

void test_parser_reuse_no_fd_leak(void) {
    TEST("parser reuse does not leak file descriptors");

    const char *csv = "a,b\n1,2\n";
    const char *path = write_temp_csv(csv);
    if (!path) { FAIL("failed to create temp file"); return; }

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { unlink(path); FAIL("failed to create parser"); return; }

    int before = count_open_fds();
    int ok = 1;
    for (int i = 0; i < 25; i++) {
        if (cisv_parser_parse_file(parser, path) < 0) {
            ok = 0;
            break;
        }
    }
    int after = count_open_fds();

    cisv_parser_destroy(parser);
    unlink(path);

    if (ok && after == before) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "fd leak detected: before=%d after=%d", before, after);
        FAIL(buf);
    }
}

void test_streaming_chunk_boundaries(void) {
    TEST("streaming parse across chunk boundaries");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *chunk1 = "a,b";
    const char *chunk2 = "\n1,2";

    cisv_parser_write(parser, (const uint8_t *)chunk1, strlen(chunk1));
    cisv_parser_write(parser, (const uint8_t *)chunk2, strlen(chunk2));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2 &&
        strcmp(stored_fields[0], "a") == 0 &&
        strcmp(stored_fields[1], "b") == 0 &&
        strcmp(stored_fields[2], "1") == 0 &&
        strcmp(stored_fields[3], "2") == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected 4 fields/2 rows, got %d/%d", field_count, row_count);
        FAIL(buf);
    }
}

void test_streaming_split_crlf_boundaries(void) {
    TEST("streaming parse coalesces split CRLF");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    cisv_parser_write(parser, (const uint8_t *)"a,b\r", 4);
    cisv_parser_write(parser, (const uint8_t *)"\n1,2\r", 5);
    cisv_parser_write(parser, (const uint8_t *)"\n", 1);
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2 &&
        strcmp(stored_fields[0], "a") == 0 &&
        strcmp(stored_fields[1], "b") == 0 &&
        strcmp(stored_fields[2], "1") == 0 &&
        strcmp(stored_fields[3], "2") == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected split CRLF as 2 rows, got %d fields/%d rows",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_streaming_rfc_quote_across_chunk_boundary(void) {
    TEST("streaming RFC quote escape across chunk boundary");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *chunk1 = "\"a\"";
    const char *chunk2 = "\"b\",c\n";
    int rc1 = cisv_parser_write(parser, (const uint8_t *)chunk1, strlen(chunk1));
    int rc2 = cisv_parser_write(parser, (const uint8_t *)chunk2, strlen(chunk2));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (rc1 == 0 && rc2 == 0 &&
        error_count == 0 &&
        field_count == 2 &&
        row_count == 1 &&
        strcmp(stored_fields[0], "a\"b") == 0 &&
        strcmp(stored_fields[1], "c") == 0) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected escaped quote across chunks, got fields=%d rows=%d errors=%d first='%s'",
                 field_count, row_count, error_count,
                 stored_field_count > 0 ? stored_fields[0] : "");
        FAIL(buf);
    }
}

void test_streaming_custom_escape_across_chunk_boundary(void) {
    TEST("streaming custom escape across chunk boundary");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *chunk1 = "\"a\\";
    const char *chunk2 = "\"b\",c\n";
    int rc1 = cisv_parser_write(parser, (const uint8_t *)chunk1, strlen(chunk1));
    int rc2 = cisv_parser_write(parser, (const uint8_t *)chunk2, strlen(chunk2));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (rc1 == 0 && rc2 == 0 &&
        error_count == 0 &&
        field_count == 2 &&
        row_count == 1 &&
        strcmp(stored_fields[0], "a\"b") == 0 &&
        strcmp(stored_fields[1], "c") == 0) {
        PASS();
    } else {
        char buf[192];
        snprintf(buf, sizeof(buf), "expected custom escaped quote across chunks, got fields=%d rows=%d errors=%d first='%s'",
                 field_count, row_count, error_count,
                 stored_field_count > 0 ? stored_fields[0] : "");
        FAIL(buf);
    }
}

void test_streaming_custom_escape_eof_error(void) {
    TEST("streaming custom escape at EOF reports error");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *chunk = "\"a\\";
    int rc = cisv_parser_write(parser, (const uint8_t *)chunk, strlen(chunk));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (rc == 0 &&
        error_count == 1 &&
        field_count == 1 &&
        row_count == 1 &&
        strcmp(stored_fields[0], "a") == 0) {
        PASS();
    } else {
        char buf[192];
        snprintf(buf, sizeof(buf), "expected malformed escape EOF error with partial field, got fields=%d rows=%d errors=%d first='%s'",
                 field_count, row_count, error_count,
                 stored_field_count > 0 ? stored_fields[0] : "");
        FAIL(buf);
    }
}

void test_skip_empty_preserves_empty_fields(void) {
    TEST("skip_empty_lines preserves empty fields");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.skip_empty_lines = true;
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,,c\n\n1,,3\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 6 && row_count == 2 &&
        strcmp(stored_fields[0], "a") == 0 &&
        strcmp(stored_fields[1], "") == 0 &&
        strcmp(stored_fields[2], "c") == 0 &&
        strcmp(stored_fields[3], "1") == 0 &&
        strcmp(stored_fields[4], "") == 0 &&
        strcmp(stored_fields[5], "3") == 0) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected 6 fields/2 rows with empty fields preserved, got %d/%d",
                 field_count, row_count);
        FAIL(buf);
    }
}

void test_batch_line_range_grouping(void) {
    TEST("batch parser line range field grouping");

    cisv_config config;
    cisv_config_init(&config);
    config.from_line = 2;
    config.to_line = 2;

    const char *csv = "h1,h2\n1,2\n3,4\n";
    cisv_result_t *result = cisv_parse_string_batch(csv, strlen(csv), &config);
    if (!result) {
        FAIL("failed to parse batch string");
        return;
    }

    int ok = result->error_code == 0 &&
             result->row_count == 1 &&
             result->total_fields == 2 &&
             result->rows[0].field_count == 2 &&
             strcmp(result->rows[0].fields[0], "1") == 0 &&
             strcmp(result->rows[0].fields[1], "2") == 0;

    cisv_result_free(result);

    if (ok) {
        PASS();
    } else {
        FAIL("batch line range included fields from skipped rows");
    }
}

void test_trailing_empty_field_at_eof(void) {
    TEST("trailing empty field at EOF");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "a,";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    cisv_result_t *batch = cisv_parse_string_batch(csv, strlen(csv), &config);

    int ok = field_count == 2 && row_count == 1 &&
             strcmp(stored_fields[0], "a") == 0 &&
             strcmp(stored_fields[1], "") == 0 &&
             batch && batch->error_code == 0 &&
             batch->row_count == 1 &&
             batch->total_fields == 2 &&
             strcmp(batch->rows[0].fields[0], "a") == 0 &&
             strcmp(batch->rows[0].fields[1], "") == 0;

    cisv_result_free(batch);

    if (ok) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected trailing empty field, got callback fields=%d rows=%d",
                 field_count, row_count);
        FAIL(buf);
    }
}

static int batch_simple_lf_edges_ok(const cisv_result_t *result) {
    return result &&
           result->error_code == 0 &&
           result->row_count == 4 &&
           result->total_fields == 9 &&
           result->rows[0].field_count == 3 &&
           strcmp(result->rows[0].fields[0], "a") == 0 &&
           strcmp(result->rows[0].fields[1], "") == 0 &&
           strcmp(result->rows[0].fields[2], "c") == 0 &&
           result->rows[1].field_count == 1 &&
           strcmp(result->rows[1].fields[0], "") == 0 &&
           result->rows[2].field_count == 3 &&
           strcmp(result->rows[2].fields[0], "1") == 0 &&
           strcmp(result->rows[2].fields[1], "2") == 0 &&
           strcmp(result->rows[2].fields[2], "") == 0 &&
           result->rows[3].field_count == 2 &&
           strcmp(result->rows[3].fields[0], "last") == 0 &&
           strcmp(result->rows[3].fields[1], "row") == 0;
}

void test_batch_simple_lf_materializer_edges(void) {
    TEST("batch simple LF materializer preserves edge fields");

    const char *csv = "a,,c\n\n1,2,\nlast,row";
    cisv_result_t *string_result = cisv_parse_string_batch(csv, strlen(csv), NULL);

    const char *path = write_temp_csv(csv);
    cisv_result_t *file_result = path ? cisv_parse_file_batch(path, NULL) : NULL;

    int ok = batch_simple_lf_edges_ok(string_result) &&
             batch_simple_lf_edges_ok(file_result);

    cisv_result_free(string_result);
    cisv_result_free(file_result);
    if (path) unlink(path);

    if (ok) {
        PASS();
    } else {
        FAIL("simple LF batch materializer did not preserve empty fields/rows");
    }
}

void test_quoted_field_closes_at_eof(void) {
    TEST("quoted field closes at EOF");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "\"say \"\"hello\"\"\",\"test\"";
    int rc = cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (rc == 0 &&
        error_count == 0 &&
        field_count == 2 &&
        row_count == 1 &&
        strcmp(stored_fields[0], "say \"hello\"") == 0 &&
        strcmp(stored_fields[1], "test") == 0) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected quoted EOF row, got fields=%d rows=%d errors=%d first='%s'",
                 field_count, row_count, error_count,
                 stored_field_count > 0 ? stored_fields[0] : "");
        FAIL(buf);
    }
}

void test_malformed_quote_errors(void) {
    TEST("malformed quoted fields report errors");
    reset_test_state();

    const char *csv = "\"unterminated";
    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;
    config.error_cb = test_error_cb;

    cisv_result_t *batch = cisv_parse_string_batch(csv, strlen(csv), &config);

    const char *path = write_temp_csv("\"a\"x,b\n");
    if (!path) {
        cisv_result_free(batch);
        FAIL("failed to create temp file");
        return;
    }

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        cisv_result_free(batch);
        unlink(path);
        FAIL("failed to create parser");
        return;
    }

    int rc = cisv_parser_parse_file(parser, path);
    cisv_parser_destroy(parser);
    unlink(path);

    int batch_error_code = batch ? batch->error_code : 0;
    int ok = batch &&
             batch_error_code != 0 &&
             rc < 0 &&
             error_count >= 1;

    cisv_result_free(batch);

    if (ok) {
        PASS();
    } else {
        char buf[160];
        snprintf(buf, sizeof(buf), "expected strict errors, got batch=%d rc=%d errors=%d",
                 batch_error_code, rc, error_count);
        FAIL(buf);
    }
}

void test_parse_comment_lines(void) {
    TEST("parse with comment line prefix");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.comment = '#';
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    const char *csv = "#meta\na,b\n#ignore this line\n1,2\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2 &&
        strcmp(stored_fields[0], "a") == 0 &&
        strcmp(stored_fields[1], "b") == 0 &&
        strcmp(stored_fields[2], "1") == 0 &&
        strcmp(stored_fields[3], "2") == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected comments skipped, got fields=%d rows=%d", field_count, row_count);
        FAIL(buf);
    }
}

void test_max_row_size_skip_error_lines(void) {
    TEST("max_row_size with skip_lines_with_error");
    reset_test_state();

    cisv_config config;
    cisv_config_init(&config);
    config.max_row_size = 10;
    config.skip_lines_with_error = true;
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) { FAIL("failed to create parser"); return; }

    // Middle row is intentionally oversized and should be skipped.
    const char *csv = "a,b\nverylongfield,2\n1,2\n";
    cisv_parser_write(parser, (const uint8_t *)csv, strlen(csv));
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    if (field_count == 4 && row_count == 2 &&
        strcmp(stored_fields[0], "a") == 0 &&
        strcmp(stored_fields[1], "b") == 0 &&
        strcmp(stored_fields[2], "1") == 0 &&
        strcmp(stored_fields[3], "2") == 0) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected oversized row skipped, got fields=%d rows=%d", field_count, row_count);
        FAIL(buf);
    }
}

void test_batch_skip_error_rolls_back_fields(void) {
    TEST("batch skip_lines_with_error rolls back malformed row fields");

    const char *csv = "a,b\n\"bad\"x,y\n1,2\n";
    cisv_config config;
    cisv_config_init(&config);
    config.skip_lines_with_error = true;

    cisv_result_t *result = cisv_parse_string_batch(csv, strlen(csv), &config);
    if (!result) {
        FAIL("batch parse returned null");
        return;
    }

    int ok = result->error_code != 0 &&
             result->row_count == 2 &&
             result->total_fields == 4 &&
             result->rows[0].field_count == 2 &&
             result->rows[1].field_count == 2 &&
             strcmp(result->rows[0].fields[0], "a") == 0 &&
             strcmp(result->rows[0].fields[1], "b") == 0 &&
             strcmp(result->rows[1].fields[0], "1") == 0 &&
             strcmp(result->rows[1].fields[1], "2") == 0;

    cisv_result_free(result);

    if (ok) {
        PASS();
    } else {
        FAIL("expected only header and final row after malformed row rollback");
    }
}

void test_parallel_custom_quote_chunk_split(void) {
    TEST("parallel parse uses custom quote in chunk splitting");

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cisv_parallel_quote_%d.csv", getpid());
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("failed to create temp file");
        return;
    }

    fprintf(f, "id|text|flag\n");
    for (int i = 0; i < 1200; i++) {
        if (i == 600) {
            fprintf(f, "%d|'line1\nline2'|ok\n", i);
        } else {
            fprintf(f, "%d|'value_%d'|ok\n", i, i);
        }
    }
    fclose(f);

    cisv_config config;
    cisv_config_init(&config);
    config.delimiter = '|';
    config.quote = '\'';

    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(path, &config, 2, &result_count);
    if (!results || result_count <= 0) {
        unlink(path);
        FAIL("parallel parse failed");
        return;
    }

    size_t total_rows = 0;
    size_t total_fields = 0;
    int found_multiline = 0;
    int had_error = 0;

    for (int i = 0; i < result_count; i++) {
        cisv_result_t *r = results[i];
        if (!r) {
            had_error = 1;
            continue;
        }
        if (r->error_code != 0) {
            had_error = 1;
        }
        total_rows += r->row_count;
        total_fields += r->total_fields;

        for (size_t row_idx = 0; row_idx < r->row_count; row_idx++) {
            cisv_row_t *row = &r->rows[row_idx];
            for (size_t col = 0; col < row->field_count; col++) {
                if (row->field_lengths[col] == 11 &&
                    memcmp(row->fields[col], "line1\nline2", 11) == 0) {
                    found_multiline = 1;
                }
            }
        }
    }

    cisv_results_free(results, result_count);
    unlink(path);

    // header + 1200 data rows
    if (!had_error && total_rows == 1201 && total_fields == 3603 && found_multiline) {
        PASS();
    } else {
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "expected rows=1201 fields=3603 multiline=1, got rows=%zu fields=%zu multiline=%d error=%d",
                 total_rows, total_fields, found_multiline, had_error);
        FAIL(buf);
    }
}

void test_parallel_custom_escape_chunk_split(void) {
    TEST("parallel parse uses custom escape in chunk splitting");

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cisv_parallel_escape_%d.csv", getpid());
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("failed to create temp file");
        return;
    }

    fprintf(f, "id,text,flag\n");
    fprintf(f, "0,\"");
    for (int i = 0; i < 4500; i++) {
        fputc('a', f);
    }
    fprintf(f, "\\\"");
    for (int i = 0; i < 500; i++) {
        fputc('b', f);
    }
    fputc('\n', f);
    for (int i = 0; i < 300; i++) {
        fputc('c', f);
    }
    fprintf(f, "\",ok\n");

    for (int i = 1; i <= 260; i++) {
        fprintf(f, "%d,value,ok\n", i);
    }
    fclose(f);

    cisv_config config;
    cisv_config_init(&config);
    config.escape = '\\';

    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(path, &config, 2, &result_count);
    if (!results || result_count <= 0) {
        unlink(path);
        FAIL("parallel parse failed");
        return;
    }

    size_t total_rows = 0;
    size_t total_fields = 0;
    int found_multiline = 0;
    int had_error = 0;

    for (int i = 0; i < result_count; i++) {
        cisv_result_t *r = results[i];
        if (!r) {
            had_error = 1;
            continue;
        }
        if (r->error_code != 0) {
            had_error = 1;
        }
        total_rows += r->row_count;
        total_fields += r->total_fields;

        for (size_t row_idx = 0; row_idx < r->row_count; row_idx++) {
            cisv_row_t *row = &r->rows[row_idx];
            for (size_t col = 0; col < row->field_count; col++) {
                if (row->field_lengths[col] == 5302 &&
                    memchr(row->fields[col], '\n', row->field_lengths[col]) &&
                    memchr(row->fields[col], '"', row->field_lengths[col])) {
                    found_multiline = 1;
                }
            }
        }
    }

    cisv_results_free(results, result_count);
    unlink(path);

    if (!had_error && total_rows == 262 && total_fields == 786 && found_multiline) {
        PASS();
    } else {
        char buf[220];
        snprintf(buf, sizeof(buf),
                 "expected rows=262 fields=786 multiline=1, got rows=%zu fields=%zu multiline=%d error=%d",
                 total_rows, total_fields, found_multiline, had_error);
        FAIL(buf);
    }
}

void test_parallel_cr_only_chunk_split(void) {
    TEST("parallel parse splits CR-only row endings");

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cisv_parallel_cr_%d.csv", getpid());
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("failed to create temp file");
        return;
    }

    for (int i = 0; i < 5000; i++) {
        fprintf(f, "%d,%d\r", i, i + 1);
    }
    fclose(f);

    cisv_config config;
    cisv_config_init(&config);

    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(path, &config, 4, &result_count);
    if (!results || result_count <= 0) {
        unlink(path);
        FAIL("parallel parse failed");
        return;
    }

    size_t total_rows = 0;
    size_t total_fields = 0;
    int had_error = 0;

    for (int i = 0; i < result_count; i++) {
        cisv_result_t *r = results[i];
        if (!r || r->error_code != 0) {
            had_error = 1;
            continue;
        }
        total_rows += r->row_count;
        total_fields += r->total_fields;
    }

    cisv_results_free(results, result_count);
    unlink(path);

    if (!had_error && result_count > 1 && total_rows == 5000 && total_fields == 10000) {
        PASS();
    } else {
        char buf[180];
        snprintf(buf, sizeof(buf),
                 "expected split CR rows=5000 fields=10000 chunks>1, got rows=%zu fields=%zu chunks=%d error=%d",
                 total_rows, total_fields, result_count, had_error);
        FAIL(buf);
    }
}

void test_parallel_malformed_quote_errors(void) {
    TEST("parallel parse surfaces malformed quote errors");

    const char *path = write_temp_csv("a,b\n\"a\"x,b\n");
    if (!path) {
        FAIL("failed to create temp file");
        return;
    }

    cisv_config config;
    cisv_config_init(&config);

    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(path, &config, 2, &result_count);
    unlink(path);

    int saw_error = 0;
    int had_results = results != NULL;
    if (results) {
        for (int i = 0; i < result_count; i++) {
            if (!results[i] || results[i]->error_code != 0) {
                saw_error = 1;
            }
        }
    }

    cisv_results_free(results, result_count);

    if (had_results && result_count > 0 && saw_error) {
        PASS();
    } else {
        char buf[128];
        snprintf(buf, sizeof(buf), "expected parallel error result, got had_results=%d count=%d saw_error=%d",
                 had_results, result_count, saw_error);
        FAIL(buf);
    }
}

void test_wide_multiline_json_stress(void) {
    TEST("wide multiline JSON stress");

    char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cisv_wide_json_%d.csv", getpid());
    FILE *f = fopen(path, "w");
    if (!f) {
        FAIL("failed to create temp file");
        return;
    }

    for (int col = 0; col < 128; col++) {
        fprintf(f, "h%d%s", col, col == 127 ? "\n" : ",");
    }

    for (int row = 0; row < 256; row++) {
        for (int col = 0; col < 128; col++) {
            if (col == 64) {
                fprintf(f,
                        "\"{\n"
                        "  \"\"id\"\": %d,\n"
                        "  \"\"name\"\": \"\"row-%d\"\",\n"
                        "  \"\"items\"\": [\n"
                        "    {\"\"k\"\": \"\"a\"\", \"\"v\"\": 1},\n"
                        "    {\"\"k\"\": \"\"b\"\", \"\"v\"\": 2}\n"
                        "  ]\n"
                        "}\"",
                        row, row);
            } else {
                fprintf(f, "%d", row + col);
            }
            fputc(col == 127 ? '\n' : ',', f);
        }
    }
    fclose(f);

    reset_test_state();
    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = test_field_cb;
    config.row_cb = test_row_cb;

    cisv_parser *parser = cisv_parser_create_with_config(&config);
    if (!parser) {
        unlink(path);
        FAIL("failed to create parser");
        return;
    }

    int rc = cisv_parser_parse_file(parser, path);
    cisv_parser_destroy(parser);
    size_t counted_rows = cisv_parser_count_rows(path);

    int result_count = 0;
    cisv_result_t **results = cisv_parse_file_parallel(path, &config, 4, &result_count);
    cisv_result_t *batch = cisv_parse_file_batch(path, &config);

    size_t parallel_rows = 0;
    size_t parallel_fields = 0;
    int parallel_ok = 1;
    int found_multiline = 0;

    if (!results || result_count <= 0) {
        parallel_ok = 0;
    } else {
        for (int i = 0; i < result_count; i++) {
            if (!results[i] || results[i]->error_code != 0) {
                parallel_ok = 0;
                continue;
            }
            parallel_rows += results[i]->row_count;
            parallel_fields += results[i]->total_fields;
            for (size_t r = 0; r < results[i]->row_count; r++) {
                cisv_row_t *row = &results[i]->rows[r];
                for (size_t c = 0; c < row->field_count; c++) {
                    if (row->field_lengths[c] > 32 &&
                        strstr(row->fields[c], "\"name\": \"row-0\"") != NULL &&
                        strstr(row->fields[c], "\"items\": [") != NULL) {
                        found_multiline = 1;
                    }
                }
            }
        }
    }

    if (results) {
        cisv_results_free(results, result_count);
    }
    unlink(path);

    if (rc == 0 &&
        field_count == 257 * 128 &&
        row_count == 257 &&
        counted_rows == 257 &&
        batch && batch->error_code == 0 &&
        batch->row_count == 257 &&
        batch->total_fields == 257 * 128 &&
        parallel_ok &&
        parallel_rows == 257 &&
        parallel_fields == 257 * 128 &&
        found_multiline) {
        PASS();
    } else {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "callback rows=%d fields=%d rc=%d counted_rows=%zu batch_rows=%zu batch_fields=%zu parallel_rows=%zu parallel_fields=%zu parallel_ok=%d multiline=%d",
                 row_count, field_count, rc,
                 counted_rows,
                 batch ? batch->row_count : 0,
                 batch ? batch->total_fields : 0,
                 parallel_rows, parallel_fields, parallel_ok, found_multiline);
        FAIL(buf);
    }

    cisv_result_free(batch);
}

void test_rows_merge_dedup_exclude_first(void) {
    TEST("rows merge excludes deleted keys and keeps first row");

    char newest[256], middle[256], oldest[256], deleted[256];
    if (write_temp_csv_named("newest", "Id,Name\n2,new\n3,keep\n4,del\n", newest, sizeof(newest)) != 0 ||
        write_temp_csv_named("middle", "Id,Name\n1,mid1\n2,dup\n4,del2\n", middle, sizeof(middle)) != 0 ||
        write_temp_csv_named("oldest", "Id,Name\n1,dupold\n5,old\n", oldest, sizeof(oldest)) != 0 ||
        write_temp_csv_named("deleted", "Id\n4\n", deleted, sizeof(deleted)) != 0) {
        FAIL("failed to create temporary merge fixtures");
        return;
    }

    FILE *out = tmpfile();
    if (!out) {
        unlink(newest); unlink(middle); unlink(oldest); unlink(deleted);
        FAIL("tmpfile failed");
        return;
    }

    const char *inputs[] = {newest, middle, oldest};
    const char *keys[] = {"Id"};
    cisv_rows_options_t options;
    cisv_rows_options_init(&options);
    options.mode = CISV_ROWS_MERGE;
    options.input_files = inputs;
    options.input_file_count = 3;
    options.key_columns = keys;
    options.key_column_count = 1;
    options.exclude_file = deleted;
    options.exclude_key_columns = keys;
    options.exclude_key_column_count = 1;
    options.output = out;

    cisv_rows_stats_t stats;
    char error[256] = {0};
    cisv_rows_status_t status = cisv_rows_execute(&options, &stats, error, sizeof(error));

    char buf[512];
    int read_ok = read_tmpfile_string(out, buf, sizeof(buf)) == 0;
    fclose(out);
    unlink(newest); unlink(middle); unlink(oldest); unlink(deleted);

    const char *expected = "Id,Name\n2,new\n3,keep\n1,mid1\n5,old\n";
    if (status == CISV_ROWS_OK &&
        read_ok &&
        strcmp(buf, expected) == 0 &&
        stats.input_rows == 8 &&
        stats.output_rows == 4 &&
        stats.duplicate_rows == 2 &&
        stats.excluded_rows == 2) {
        PASS();
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "status=%d error=%s input=%zu output=%zu dup=%zu excl=%zu got=%s",
                 status, error, stats.input_rows, stats.output_rows,
                 stats.duplicate_rows, stats.excluded_rows, read_ok ? buf : "<read failed>");
        FAIL(msg);
    }
}

void test_rows_dedup_keep_last_order(void) {
    TEST("rows dedup keep last emits final occurrence order");

    char input[256];
    if (write_temp_csv_named("dedup_last", "Id,V\n1,a\n2,b\n1,c\n", input, sizeof(input)) != 0) {
        FAIL("failed to create dedup fixture");
        return;
    }

    FILE *out = tmpfile();
    if (!out) {
        unlink(input);
        FAIL("tmpfile failed");
        return;
    }

    const char *inputs[] = {input};
    const char *keys[] = {"Id"};
    cisv_rows_options_t options;
    cisv_rows_options_init(&options);
    options.mode = CISV_ROWS_DEDUP;
    options.input_files = inputs;
    options.input_file_count = 1;
    options.key_columns = keys;
    options.key_column_count = 1;
    options.keep = CISV_KEEP_LAST;
    options.output = out;

    cisv_rows_stats_t stats;
    char error[256] = {0};
    cisv_rows_status_t status = cisv_rows_execute(&options, &stats, error, sizeof(error));

    char buf[256];
    int read_ok = read_tmpfile_string(out, buf, sizeof(buf)) == 0;
    fclose(out);
    unlink(input);

    const char *expected = "Id,V\n2,b\n1,c\n";
    if (status == CISV_ROWS_OK &&
        read_ok &&
        strcmp(buf, expected) == 0 &&
        stats.input_rows == 3 &&
        stats.output_rows == 2 &&
        stats.duplicate_rows == 1) {
        PASS();
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "status=%d error=%s got=%s", status, error, read_ok ? buf : "<read failed>");
        FAIL(msg);
    }
}

void test_rows_dedup_composite_keys_are_structured(void) {
    TEST("rows dedup composite keys avoid ambiguous concatenation");

    char input[256];
    if (write_temp_csv_named("dedup_composite", "A,B,V\n1,23,x\n12,3,y\n1,23,z\n", input, sizeof(input)) != 0) {
        FAIL("failed to create composite fixture");
        return;
    }

    FILE *out = tmpfile();
    if (!out) {
        unlink(input);
        FAIL("tmpfile failed");
        return;
    }

    const char *inputs[] = {input};
    const char *keys[] = {"A", "B"};
    cisv_rows_options_t options;
    cisv_rows_options_init(&options);
    options.mode = CISV_ROWS_DEDUP;
    options.input_files = inputs;
    options.input_file_count = 1;
    options.key_columns = keys;
    options.key_column_count = 2;
    options.output = out;

    cisv_rows_stats_t stats;
    char error[256] = {0};
    cisv_rows_status_t status = cisv_rows_execute(&options, &stats, error, sizeof(error));

    char buf[256];
    int read_ok = read_tmpfile_string(out, buf, sizeof(buf)) == 0;
    fclose(out);
    unlink(input);

    const char *expected = "A,B,V\n1,23,x\n12,3,y\n";
    if (status == CISV_ROWS_OK &&
        read_ok &&
        strcmp(buf, expected) == 0 &&
        stats.input_rows == 3 &&
        stats.output_rows == 2 &&
        stats.duplicate_rows == 1) {
        PASS();
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "status=%d error=%s got=%s", status, error, read_ok ? buf : "<read failed>");
        FAIL(msg);
    }
}

int main(void) {
    printf("CISV Core Library Tests\n");
    printf("========================\n\n");

    // Parser tests
    printf("Parser Tests:\n");
    test_config_init();
    test_parser_lifecycle();
    test_invalid_parser_config_rejected();
    test_resource_env_row_limit_and_override();
    test_gomemlimit_adaptive_row_limit();
    test_resource_env_max_procs_clamps_parallel();
    test_iterator_resource_row_limit();
    test_iterator_custom_escape_values();
    test_iterator_custom_escape_eof_error();
    test_iterator_malformed_quote_errors();
    test_iterator_bare_quote_strict_and_relaxed();
    test_iterator_unterminated_quote_errors();
    test_iterator_cr_only_line_endings();
    test_iterator_row_controls();
    test_iterator_skip_empty_and_quoted_comment_parity();
    test_parse_simple();
    test_parse_custom_delimiter();
    test_parse_quoted();
    test_parse_quote_inside_unquoted_field();
    test_parse_backslash_escaped_json();

    // Transformer tests
    printf("\nTransformer Tests:\n");
    test_transform_uppercase();
    test_transform_lowercase();
    test_transform_trim();
    test_transform_pipeline();
    test_transform_rejects_unsupported_builtins();
    test_transform_add_by_name_rejects_unsupported();
    test_transform_remove_field_rebuilds_index();
    test_transform_remove_by_name();
    test_base64_encode();

    // Writer tests
    printf("\nWriter Tests:\n");
    test_writer_basic();
    test_writer_quoting();
    test_writer_long_quoted_field_with_quotes();
    test_writer_large_quoted_field_direct_fallback();
    test_writer_config_validation();

    // Multiline tests (issue #108)
    printf("\nMultiline Tests (Issue #108):\n");
    test_parse_multiline_quoted();
    test_parse_multiline_row_count();
    test_parse_multiline_multiple_fields();
    test_parse_multiline_escaped_quotes();
    test_parse_multiline_crlf();
    test_parse_cr_only_line_endings();
    test_parse_multiline_empty_lines();
    test_parse_multiline_issue108();
    test_count_rows_with_config_custom_quote();
    test_count_rows_with_escape_config();
    test_count_rows_with_row_controls();
    test_count_rows_with_line_range_controls();
    test_count_rows_skip_empty_preserves_empty_fields();
    test_count_rows_trimmed_comment_and_quoted_comment();
    test_count_rows_final_row_without_newline();
    test_count_rows_cr_only_line_endings();
    test_count_rows_mixed_line_endings_no_quotes();
    test_count_rows_empty_file();
    test_count_rows_invalid_config_rejected();
    test_count_rows_malformed_quote_returns_zero();
    test_count_rows_bare_quote_strict_and_relaxed();
    test_parser_reuse_no_fd_leak();
    test_streaming_chunk_boundaries();
    test_streaming_split_crlf_boundaries();
    test_streaming_rfc_quote_across_chunk_boundary();
    test_streaming_custom_escape_across_chunk_boundary();
    test_streaming_custom_escape_eof_error();
    test_skip_empty_preserves_empty_fields();
    test_batch_line_range_grouping();
    test_trailing_empty_field_at_eof();
    test_batch_simple_lf_materializer_edges();
    test_quoted_field_closes_at_eof();
    test_malformed_quote_errors();
    test_parse_comment_lines();
    test_max_row_size_skip_error_lines();
    test_batch_skip_error_rolls_back_fields();
    test_parallel_custom_quote_chunk_split();
    test_parallel_custom_escape_chunk_split();
    test_parallel_cr_only_chunk_split();
    test_parallel_malformed_quote_errors();
    test_wide_multiline_json_stress();

    // Row operation tests
    printf("\nRow Operation Tests:\n");
    test_rows_merge_dedup_exclude_first();
    test_rows_dedup_keep_last_order();
    test_rows_dedup_composite_keys_are_structured();

    // Summary
    printf("\n========================\n");
    printf("Results: %d/%d tests passed\n", pass_count, test_count);

    return (pass_count == test_count) ? 0 : 1;
}
