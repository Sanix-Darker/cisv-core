#include "cisv/operations.h"
#include "cisv/writer.h"

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#define CISV_KEYSET_EMPTY SIZE_MAX

typedef struct {
    uint64_t hash;
    size_t offset;
    size_t len;
    size_t row_index;
    unsigned char used;
} cisv_key_entry_t;

typedef struct {
    cisv_key_entry_t *entries;
    size_t capacity;
    size_t count;
    char *arena;
    size_t arena_len;
    size_t arena_cap;
    size_t memory_used;
    size_t memory_limit;
} cisv_key_set_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cisv_key_builder_t;

typedef struct {
    char **fields;
    size_t *lengths;
    size_t field_count;
    size_t seq;
    size_t bytes_owned;
    int active;
} cisv_owned_row_t;

typedef struct {
    cisv_owned_row_t *rows;
    size_t count;
    size_t cap;
    size_t bytes_owned;
} cisv_row_store_t;

typedef struct {
    const cisv_rows_options_t *options;
    cisv_rows_stats_t *stats;
    char *error;
    size_t error_len;
    cisv_key_set_t accepted;
    cisv_key_set_t excluded;
    cisv_row_store_t row_store;
    cisv_owned_row_t header;
    int have_header;
    size_t *source_key_indexes;
    size_t source_key_count;
    size_t sequence;
    size_t memory_limit;
    cisv_writer *writer;
} cisv_rows_runtime_t;

static void rows_set_error(char *buf, size_t len, const char *fmt, ...) {
    if (!buf || len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, len, fmt, ap);
    va_end(ap);
}

static double rows_now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static size_t rows_peak_rss_bytes(void) {
#if defined(__unix__) || defined(__APPLE__)
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return (size_t)usage.ru_maxrss;
#else
    return (size_t)usage.ru_maxrss * 1024u;
#endif
#else
    return 0;
#endif
}

static size_t rows_file_size(const char *path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size < 0) return 0;
    return (size_t)st.st_size;
}

static uint64_t rows_hash_bytes(const char *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h ? h : 1;
}

static size_t rows_next_power_of_two(size_t n) {
    size_t p = 1;
    while (p < n && p <= SIZE_MAX / 2) p <<= 1;
    return p < n ? 0 : p;
}

static int rows_memory_limit_ok(size_t current, size_t add, size_t limit) {
    if (limit == 0) return 1;
    if (add > SIZE_MAX - current) return 0;
    return current + add <= limit;
}

static int key_set_init(cisv_key_set_t *set, size_t expected, size_t memory_limit) {
    memset(set, 0, sizeof(*set));
    set->memory_limit = memory_limit;

    size_t wanted = expected > 0 ? expected + (expected >> 1) + 16 : 1024;
    if (wanted < 1024) wanted = 1024;
    size_t cap = rows_next_power_of_two(wanted);
    if (cap == 0) return -1;

    size_t bytes;
    if (__builtin_mul_overflow(cap, sizeof(cisv_key_entry_t), &bytes)) return -1;
    if (!rows_memory_limit_ok(0, bytes, memory_limit)) return -1;

    set->entries = calloc(cap, sizeof(cisv_key_entry_t));
    if (!set->entries) return -1;
    set->capacity = cap;
    set->memory_used = bytes;
    return 0;
}

static void key_set_destroy(cisv_key_set_t *set) {
    if (!set) return;
    free(set->entries);
    free(set->arena);
    memset(set, 0, sizeof(*set));
}

static int key_entry_matches(const cisv_key_set_t *set,
                             const cisv_key_entry_t *entry,
                             const char *key,
                             size_t key_len,
                             uint64_t hash) {
    return entry->used &&
           entry->hash == hash &&
           entry->len == key_len &&
           memcmp(set->arena + entry->offset, key, key_len) == 0;
}

static int key_set_find_slot(const cisv_key_set_t *set,
                             const char *key,
                             size_t key_len,
                             uint64_t hash,
                             size_t *slot,
                             int *found) {
    if (!set || !set->entries || set->capacity == 0 || !slot || !found) return -1;
    size_t mask = set->capacity - 1;
    size_t idx = (size_t)hash & mask;
    for (size_t probe = 0; probe < set->capacity; probe++) {
        cisv_key_entry_t *entry = &set->entries[idx];
        if (!entry->used) {
            *slot = idx;
            *found = 0;
            return 0;
        }
        if (key_entry_matches(set, entry, key, key_len, hash)) {
            *slot = idx;
            *found = 1;
            return 0;
        }
        idx = (idx + 1) & mask;
    }
    return -1;
}

static int key_set_rehash(cisv_key_set_t *set, size_t new_capacity) {
    size_t bytes;
    if (__builtin_mul_overflow(new_capacity, sizeof(cisv_key_entry_t), &bytes)) return -1;
    size_t old_bytes = set->capacity * sizeof(cisv_key_entry_t);
    if (bytes > old_bytes && !rows_memory_limit_ok(set->memory_used, bytes - old_bytes, set->memory_limit)) {
        return -2;
    }

    cisv_key_entry_t *new_entries = calloc(new_capacity, sizeof(cisv_key_entry_t));
    if (!new_entries) return -1;

    cisv_key_entry_t *old_entries = set->entries;
    size_t old_capacity = set->capacity;
    set->entries = new_entries;
    set->capacity = new_capacity;
    set->count = 0;
    set->memory_used = set->memory_used - old_bytes + bytes;

    for (size_t i = 0; i < old_capacity; i++) {
        if (!old_entries[i].used) continue;
        size_t mask = new_capacity - 1;
        size_t idx = (size_t)old_entries[i].hash & mask;
        while (new_entries[idx].used) idx = (idx + 1) & mask;
        new_entries[idx] = old_entries[i];
        set->count++;
    }

    free(old_entries);
    return 0;
}

static int key_set_grow_if_needed(cisv_key_set_t *set) {
    if ((set->count + 1) * 10 < set->capacity * 7) return 0;
    if (set->capacity > SIZE_MAX / 2) return -1;
    return key_set_rehash(set, set->capacity << 1);
}

static int key_set_reserve_arena(cisv_key_set_t *set, size_t add) {
    if (add == 0) return 0;
    if (add > SIZE_MAX - set->arena_len) return -1;
    size_t required = set->arena_len + add;
    if (required <= set->arena_cap) return 0;

    size_t new_cap = set->arena_cap ? set->arena_cap + (set->arena_cap >> 1) : 4096;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap <<= 1;
    }

    if (!rows_memory_limit_ok(set->memory_used, new_cap - set->arena_cap, set->memory_limit)) {
        return -2;
    }
    char *new_data = realloc(set->arena, new_cap);
    if (!new_data) return -1;
    set->arena = new_data;
    set->memory_used += new_cap - set->arena_cap;
    set->arena_cap = new_cap;
    return 0;
}

static int key_set_lookup(cisv_key_set_t *set,
                          const char *key,
                          size_t key_len,
                          uint64_t hash,
                          size_t *row_index) {
    size_t slot = 0;
    int found = 0;
    if (key_set_find_slot(set, key, key_len, hash, &slot, &found) != 0) return 0;
    if (!found) return 0;
    if (row_index) *row_index = set->entries[slot].row_index;
    return 1;
}

static int key_set_insert_or_assign(cisv_key_set_t *set,
                                    const char *key,
                                    size_t key_len,
                                    uint64_t hash,
                                    size_t row_index,
                                    int *inserted,
                                    size_t *old_row_index) {
    if (inserted) *inserted = 0;
    if (old_row_index) *old_row_index = CISV_KEYSET_EMPTY;

    int grow_rc = key_set_grow_if_needed(set);
    if (grow_rc != 0) return grow_rc;

    size_t slot = 0;
    int found = 0;
    if (key_set_find_slot(set, key, key_len, hash, &slot, &found) != 0) return -1;
    if (found) {
        if (old_row_index) *old_row_index = set->entries[slot].row_index;
        set->entries[slot].row_index = row_index;
        return 0;
    }

    int reserve_rc = key_set_reserve_arena(set, key_len);
    if (reserve_rc != 0) return reserve_rc;
    size_t offset = set->arena_len;
    if (key_len > 0) memcpy(set->arena + offset, key, key_len);
    set->arena_len += key_len;

    set->entries[slot].used = 1;
    set->entries[slot].hash = hash;
    set->entries[slot].offset = offset;
    set->entries[slot].len = key_len;
    set->entries[slot].row_index = row_index;
    set->count++;
    if (inserted) *inserted = 1;
    return 0;
}

static void key_builder_destroy(cisv_key_builder_t *builder) {
    if (!builder) return;
    free(builder->data);
    memset(builder, 0, sizeof(*builder));
}

static int key_builder_reserve(cisv_key_builder_t *builder, size_t add) {
    if (add > SIZE_MAX - builder->len) return -1;
    size_t required = builder->len + add;
    if (required <= builder->cap) return 0;
    size_t new_cap = builder->cap ? builder->cap * 2 : 64;
    while (new_cap < required) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = required;
            break;
        }
        new_cap *= 2;
    }
    char *new_data = realloc(builder->data, new_cap);
    if (!new_data) return -1;
    builder->data = new_data;
    builder->cap = new_cap;
    return 0;
}

static int key_builder_append(cisv_key_builder_t *builder, const void *data, size_t len) {
    if (key_builder_reserve(builder, len) != 0) return -1;
    if (len > 0) memcpy(builder->data + builder->len, data, len);
    builder->len += len;
    return 0;
}

static int key_builder_append_size(cisv_key_builder_t *builder, size_t value) {
    unsigned char bytes[8];
    uint64_t v = (uint64_t)value;
    for (size_t i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)(v & 0xffu);
        v >>= 8;
    }
    return key_builder_append(builder, bytes, sizeof(bytes));
}

static int build_row_key(const char **fields,
                         const size_t *lengths,
                         size_t field_count,
                         const size_t *indexes,
                         size_t index_count,
                         cisv_key_builder_t *builder,
                         const char **key,
                         size_t *key_len,
                         int *empty_key) {
    if (!fields || !lengths || !indexes || index_count == 0 || !key || !key_len || !empty_key) return -1;
    *empty_key = 1;

    if (index_count == 1) {
        size_t idx = indexes[0];
        if (idx >= field_count) return -2;
        *key = fields[idx] ? fields[idx] : "";
        *key_len = lengths[idx];
        *empty_key = (*key_len == 0);
        return 0;
    }

    builder->len = 0;
    for (size_t i = 0; i < index_count; i++) {
        size_t idx = indexes[i];
        if (idx >= field_count) return -2;
        size_t len = lengths[idx];
        if (len > 0) *empty_key = 0;
        if (key_builder_append_size(builder, len) != 0) return -1;
        if (key_builder_append(builder, fields[idx], len) != 0) return -1;
    }

    *key = builder->data;
    *key_len = builder->len;
    return 0;
}

static void owned_row_destroy(cisv_owned_row_t *row) {
    if (!row) return;
    if (row->fields) {
        for (size_t i = 0; i < row->field_count; i++) free(row->fields[i]);
    }
    free(row->fields);
    free(row->lengths);
    memset(row, 0, sizeof(*row));
}

static int owned_row_copy(cisv_owned_row_t *out,
                          const char **fields,
                          const size_t *lengths,
                          size_t field_count,
                          size_t seq) {
    memset(out, 0, sizeof(*out));
    out->fields = calloc(field_count, sizeof(char *));
    out->lengths = calloc(field_count, sizeof(size_t));
    if (!out->fields || !out->lengths) {
        owned_row_destroy(out);
        return -1;
    }

    out->field_count = field_count;
    out->seq = seq;
    out->active = 1;
    out->bytes_owned = field_count * (sizeof(char *) + sizeof(size_t));

    for (size_t i = 0; i < field_count; i++) {
        size_t len = lengths[i];
        out->fields[i] = malloc(len + 1);
        if (!out->fields[i]) {
            owned_row_destroy(out);
            return -1;
        }
        if (len > 0 && fields[i]) memcpy(out->fields[i], fields[i], len);
        out->fields[i][len] = '\0';
        out->lengths[i] = len;
        out->bytes_owned += len + 1;
    }
    return 0;
}

static int row_store_append(cisv_row_store_t *store, cisv_owned_row_t *row, size_t *index) {
    if (store->count == store->cap) {
        size_t new_cap = store->cap ? store->cap + (store->cap >> 1) : 1024;
        if (new_cap <= store->cap) return -1;
        cisv_owned_row_t *new_rows = realloc(store->rows, new_cap * sizeof(cisv_owned_row_t));
        if (!new_rows) return -1;
        memset(new_rows + store->cap, 0, (new_cap - store->cap) * sizeof(cisv_owned_row_t));
        store->rows = new_rows;
        store->cap = new_cap;
    }
    store->rows[store->count] = *row;
    store->bytes_owned += row->bytes_owned;
    if (index) *index = store->count;
    store->count++;
    memset(row, 0, sizeof(*row));
    return 0;
}

static void row_store_deactivate(cisv_row_store_t *store, size_t index) {
    if (!store || index >= store->count || !store->rows[index].active) return;
    store->rows[index].active = 0;
    store->bytes_owned -= store->rows[index].bytes_owned;
    owned_row_destroy(&store->rows[index]);
}

static void row_store_destroy(cisv_row_store_t *store) {
    if (!store) return;
    for (size_t i = 0; i < store->count; i++) owned_row_destroy(&store->rows[i]);
    free(store->rows);
    memset(store, 0, sizeof(*store));
}

static int write_row(cisv_rows_runtime_t *rt,
                     const char **fields,
                     const size_t *lengths,
                     size_t field_count) {
    for (size_t i = 0; i < field_count; i++) {
        if (cisv_writer_field(rt->writer, fields[i], lengths[i]) < 0) return -1;
    }
    return cisv_writer_row_end(rt->writer);
}

static int write_owned_row(cisv_rows_runtime_t *rt, const cisv_owned_row_t *row) {
    return write_row(rt, (const char **)row->fields, row->lengths, row->field_count);
}

static int rows_memory_check(cisv_rows_runtime_t *rt) {
    if (!rt->memory_limit) return 1;
    size_t used = rt->accepted.memory_used;
    if (used > SIZE_MAX - rt->excluded.memory_used) return 0;
    used += rt->excluded.memory_used;
    if (used > SIZE_MAX - rt->row_store.bytes_owned) return 0;
    used += rt->row_store.bytes_owned;
    return used <= rt->memory_limit;
}

static int header_index_for_name(const char **fields,
                                 const size_t *lengths,
                                 size_t field_count,
                                 const char *name) {
    if (!name) return -1;
    size_t name_len = strlen(name);
    for (size_t i = 0; i < field_count; i++) {
        if (lengths[i] == name_len && memcmp(fields[i], name, name_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static cisv_rows_status_t resolve_key_indexes(const char **names,
                                              size_t name_count,
                                              const size_t *indexes,
                                              size_t index_count,
                                              int use_indexes,
                                              const char **header_fields,
                                              const size_t *header_lengths,
                                              size_t header_field_count,
                                              int no_header,
                                              size_t **out_indexes,
                                              size_t *out_count,
                                              char *error,
                                              size_t error_len) {
    if (!out_indexes || !out_count) return CISV_ROWS_USAGE_ERROR;
    *out_indexes = NULL;
    *out_count = 0;

    if (use_indexes) {
        if (index_count == 0 || !indexes) {
            rows_set_error(error, error_len, "at least one key index is required");
            return CISV_ROWS_USAGE_ERROR;
        }
        size_t *copy = calloc(index_count, sizeof(size_t));
        if (!copy) return CISV_ROWS_IO_ERROR;
        memcpy(copy, indexes, index_count * sizeof(size_t));
        *out_indexes = copy;
        *out_count = index_count;
        return CISV_ROWS_OK;
    }

    if (no_header) {
        rows_set_error(error, error_len, "named keys require a header; use --key-index with --no-header");
        return CISV_ROWS_USAGE_ERROR;
    }
    if (!names || name_count == 0) {
        rows_set_error(error, error_len, "at least one key column is required");
        return CISV_ROWS_USAGE_ERROR;
    }

    size_t *resolved = calloc(name_count, sizeof(size_t));
    if (!resolved) return CISV_ROWS_IO_ERROR;
    for (size_t i = 0; i < name_count; i++) {
        int idx = header_index_for_name(header_fields, header_lengths, header_field_count, names[i]);
        if (idx < 0) {
            rows_set_error(error, error_len, "missing key column: %s", names[i]);
            free(resolved);
            return CISV_ROWS_MISSING_KEY;
        }
        resolved[i] = (size_t)idx;
    }

    *out_indexes = resolved;
    *out_count = name_count;
    return CISV_ROWS_OK;
}

static int headers_equal(const cisv_owned_row_t *header,
                         const char **fields,
                         const size_t *lengths,
                         size_t field_count) {
    if (!header || header->field_count != field_count) return 0;
    for (size_t i = 0; i < field_count; i++) {
        if (header->lengths[i] != lengths[i]) return 0;
        if (memcmp(header->fields[i], fields[i], lengths[i]) != 0) return 0;
    }
    return 1;
}

static cisv_rows_status_t load_exclude_keys(cisv_rows_runtime_t *rt) {
    const cisv_rows_options_t *opt = rt->options;
    if (!opt->exclude_file) return CISV_ROWS_OK;

    cisv_iterator_t *it = cisv_iterator_open(opt->exclude_file, &opt->csv_config);
    if (!it) {
        rows_set_error(rt->error, rt->error_len, "failed to open exclude keys file: %s", opt->exclude_file);
        return CISV_ROWS_IO_ERROR;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    size_t *exclude_indexes = NULL;
    size_t exclude_index_count = 0;
    cisv_key_builder_t builder = {0};
    int rc;

    if (!opt->no_header) {
        rc = cisv_iterator_next(it, &fields, &lengths, &field_count);
        if (rc == CISV_ITER_EOF) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            rows_set_error(rt->error, rt->error_len, "exclude keys file is empty: %s", opt->exclude_file);
            return CISV_ROWS_MISSING_KEY;
        }
        if (rc == CISV_ITER_ERROR) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            rows_set_error(rt->error, rt->error_len, "parse error in exclude keys file: %s", opt->exclude_file);
            return CISV_ROWS_PARSE_ERROR;
        }
        cisv_rows_status_t status = resolve_key_indexes(
            opt->exclude_key_columns,
            opt->exclude_key_column_count,
            opt->exclude_key_indexes,
            opt->exclude_key_index_count,
            opt->use_exclude_key_indexes,
            fields,
            lengths,
            field_count,
            opt->no_header,
            &exclude_indexes,
            &exclude_index_count,
            rt->error,
            rt->error_len
        );
        if (status != CISV_ROWS_OK) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return status;
        }
    } else {
        cisv_rows_status_t status = resolve_key_indexes(
            opt->exclude_key_columns,
            opt->exclude_key_column_count,
            opt->exclude_key_indexes,
            opt->exclude_key_index_count,
            opt->use_exclude_key_indexes,
            NULL,
            NULL,
            0,
            opt->no_header,
            &exclude_indexes,
            &exclude_index_count,
            rt->error,
            rt->error_len
        );
        if (status != CISV_ROWS_OK) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return status;
        }
    }

    while ((rc = cisv_iterator_next(it, &fields, &lengths, &field_count)) == CISV_ITER_OK) {
        const char *key = NULL;
        size_t key_len = 0;
        int empty_key = 0;
        int key_rc = build_row_key(fields, lengths, field_count, exclude_indexes,
                                   exclude_index_count, &builder, &key, &key_len, &empty_key);
        if (key_rc == -2) {
            rt->stats->malformed_rows++;
            if (opt->csv_config.skip_lines_with_error) continue;
            free(exclude_indexes);
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            rows_set_error(rt->error, rt->error_len, "missing exclude key field in %s", opt->exclude_file);
            return CISV_ROWS_PARSE_ERROR;
        }
        if (key_rc != 0) {
            free(exclude_indexes);
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return CISV_ROWS_IO_ERROR;
        }
        if (opt->drop_empty_key && empty_key) continue;

        uint64_t hash = rows_hash_bytes(key, key_len);
        int inserted = 0;
        int insert_rc = key_set_insert_or_assign(&rt->excluded, key, key_len, hash,
                                                 CISV_KEYSET_EMPTY, &inserted, NULL);
        (void)inserted;
        if (insert_rc == -2) {
            free(exclude_indexes);
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return CISV_ROWS_MEMORY_LIMIT;
        }
        if (insert_rc != 0) {
            free(exclude_indexes);
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return CISV_ROWS_IO_ERROR;
        }
    }

    free(exclude_indexes);
    cisv_iterator_close(it);
    key_builder_destroy(&builder);

    if (rc == CISV_ITER_ERROR) {
        rows_set_error(rt->error, rt->error_len, "parse error in exclude keys file: %s", opt->exclude_file);
        return CISV_ROWS_PARSE_ERROR;
    }
    if (!rows_memory_check(rt)) return CISV_ROWS_MEMORY_LIMIT;
    return CISV_ROWS_OK;
}

static cisv_rows_status_t process_source_row(cisv_rows_runtime_t *rt,
                                             const char **fields,
                                             const size_t *lengths,
                                             size_t field_count,
                                             cisv_key_builder_t *builder) {
    const cisv_rows_options_t *opt = rt->options;
    rt->stats->input_rows++;

    if (opt->mode == CISV_ROWS_CAT) {
        if (write_row(rt, fields, lengths, field_count) != 0) {
            rows_set_error(rt->error, rt->error_len, "failed to write output row");
            return CISV_ROWS_IO_ERROR;
        }
        rt->stats->output_rows++;
        rt->sequence++;
        return CISV_ROWS_OK;
    }

    const char *key = NULL;
    size_t key_len = 0;
    int empty_key = 0;
    int key_rc = build_row_key(fields, lengths, field_count,
                               rt->source_key_indexes, rt->source_key_count,
                               builder, &key, &key_len, &empty_key);
    if (key_rc == -2) {
        rt->stats->malformed_rows++;
        if (opt->csv_config.skip_lines_with_error) {
            rt->sequence++;
            return CISV_ROWS_OK;
        }
        rows_set_error(rt->error, rt->error_len, "row is missing a key field");
        return CISV_ROWS_PARSE_ERROR;
    }
    if (key_rc != 0) return CISV_ROWS_IO_ERROR;

    if (opt->drop_empty_key && empty_key) {
        rt->stats->empty_key_rows++;
        rt->sequence++;
        return CISV_ROWS_OK;
    }

    uint64_t hash = rows_hash_bytes(key, key_len);
    if (opt->exclude_file && key_set_lookup(&rt->excluded, key, key_len, hash, NULL)) {
        rt->stats->excluded_rows++;
        rt->sequence++;
        return CISV_ROWS_OK;
    }

    if (opt->mode == CISV_ROWS_FILTER_EXCLUDE) {
        if (write_row(rt, fields, lengths, field_count) != 0) {
            rows_set_error(rt->error, rt->error_len, "failed to write output row");
            return CISV_ROWS_IO_ERROR;
        }
        rt->stats->output_rows++;
        rt->sequence++;
        return CISV_ROWS_OK;
    }

    if (opt->keep == CISV_KEEP_FIRST) {
        if (key_set_lookup(&rt->accepted, key, key_len, hash, NULL)) {
            rt->stats->duplicate_rows++;
            rt->sequence++;
            return CISV_ROWS_OK;
        }

        int inserted = 0;
        int insert_rc = key_set_insert_or_assign(&rt->accepted, key, key_len, hash,
                                                 CISV_KEYSET_EMPTY, &inserted, NULL);
        if (insert_rc == -2) return CISV_ROWS_MEMORY_LIMIT;
        if (insert_rc != 0) return CISV_ROWS_IO_ERROR;
        if (!rows_memory_check(rt)) return CISV_ROWS_MEMORY_LIMIT;

        if (write_row(rt, fields, lengths, field_count) != 0) {
            rows_set_error(rt->error, rt->error_len, "failed to write output row");
            return CISV_ROWS_IO_ERROR;
        }
        rt->stats->output_rows++;
        rt->sequence++;
        return CISV_ROWS_OK;
    }

    cisv_owned_row_t owned = {0};
    if (owned_row_copy(&owned, fields, lengths, field_count, rt->sequence) != 0) {
        return CISV_ROWS_IO_ERROR;
    }

    size_t new_row_index = 0;
    if (row_store_append(&rt->row_store, &owned, &new_row_index) != 0) {
        owned_row_destroy(&owned);
        return CISV_ROWS_IO_ERROR;
    }

    int inserted = 0;
    size_t old_row_index = CISV_KEYSET_EMPTY;
    int insert_rc = key_set_insert_or_assign(&rt->accepted, key, key_len, hash,
                                             new_row_index, &inserted, &old_row_index);
    if (insert_rc == -2) return CISV_ROWS_MEMORY_LIMIT;
    if (insert_rc != 0) return CISV_ROWS_IO_ERROR;
    if (!inserted && old_row_index != CISV_KEYSET_EMPTY) {
        rt->stats->duplicate_rows++;
        row_store_deactivate(&rt->row_store, old_row_index);
    }
    if (!rows_memory_check(rt)) return CISV_ROWS_MEMORY_LIMIT;

    rt->sequence++;
    return CISV_ROWS_OK;
}

static cisv_rows_status_t prepare_source_header(cisv_rows_runtime_t *rt,
                                                const char **fields,
                                                const size_t *lengths,
                                                size_t field_count) {
    const cisv_rows_options_t *opt = rt->options;

    if (!rt->have_header) {
        if (owned_row_copy(&rt->header, fields, lengths, field_count, 0) != 0) {
            return CISV_ROWS_IO_ERROR;
        }
        rt->have_header = 1;

        if (opt->mode != CISV_ROWS_CAT) {
            cisv_rows_status_t status = resolve_key_indexes(
                opt->key_columns,
                opt->key_column_count,
                opt->key_indexes,
                opt->key_index_count,
                opt->use_key_indexes,
                fields,
                lengths,
                field_count,
                opt->no_header,
                &rt->source_key_indexes,
                &rt->source_key_count,
                rt->error,
                rt->error_len
            );
            if (status != CISV_ROWS_OK) return status;
        }

        if (write_row(rt, fields, lengths, field_count) != 0) {
            rows_set_error(rt->error, rt->error_len, "failed to write CSV header");
            return CISV_ROWS_IO_ERROR;
        }
        return CISV_ROWS_OK;
    }

    if (!headers_equal(&rt->header, fields, lengths, field_count)) {
        rt->stats->header_mismatch_rows++;
        if (!opt->ignore_header_mismatch) {
            rows_set_error(rt->error, rt->error_len, "header mismatch");
            return CISV_ROWS_HEADER_MISMATCH;
        }
    }
    return CISV_ROWS_OK;
}

static cisv_rows_status_t process_source_file(cisv_rows_runtime_t *rt, const char *path) {
    const cisv_rows_options_t *opt = rt->options;
    cisv_iterator_t *it = cisv_iterator_open(path, &opt->csv_config);
    if (!it) {
        rows_set_error(rt->error, rt->error_len, "failed to open input file: %s", path);
        return CISV_ROWS_IO_ERROR;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;
    cisv_key_builder_t builder = {0};
    int rc;

    if (!opt->no_header) {
        rc = cisv_iterator_next(it, &fields, &lengths, &field_count);
        if (rc == CISV_ITER_EOF) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return CISV_ROWS_OK;
        }
        if (rc == CISV_ITER_ERROR) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            rows_set_error(rt->error, rt->error_len, "parse error while reading header: %s", path);
            return CISV_ROWS_PARSE_ERROR;
        }
        cisv_rows_status_t status = prepare_source_header(rt, fields, lengths, field_count);
        if (status != CISV_ROWS_OK) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return status;
        }
    } else if (opt->mode != CISV_ROWS_CAT && !rt->source_key_indexes) {
        cisv_rows_status_t status = resolve_key_indexes(
            opt->key_columns,
            opt->key_column_count,
            opt->key_indexes,
            opt->key_index_count,
            opt->use_key_indexes,
            NULL,
            NULL,
            0,
            opt->no_header,
            &rt->source_key_indexes,
            &rt->source_key_count,
            rt->error,
            rt->error_len
        );
        if (status != CISV_ROWS_OK) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return status;
        }
    }

    while ((rc = cisv_iterator_next(it, &fields, &lengths, &field_count)) == CISV_ITER_OK) {
        cisv_rows_status_t status = process_source_row(rt, fields, lengths, field_count, &builder);
        if (status != CISV_ROWS_OK) {
            cisv_iterator_close(it);
            key_builder_destroy(&builder);
            return status;
        }
    }

    cisv_iterator_close(it);
    key_builder_destroy(&builder);

    if (rc == CISV_ITER_ERROR) {
        rt->stats->malformed_rows++;
        rows_set_error(rt->error, rt->error_len, "parse error while reading input file: %s", path);
        return CISV_ROWS_PARSE_ERROR;
    }
    return CISV_ROWS_OK;
}

static cisv_rows_status_t flush_keep_last_rows(cisv_rows_runtime_t *rt) {
    for (size_t i = 0; i < rt->row_store.count; i++) {
        if (!rt->row_store.rows[i].active) continue;
        if (write_owned_row(rt, &rt->row_store.rows[i]) != 0) {
            rows_set_error(rt->error, rt->error_len, "failed to write output row");
            return CISV_ROWS_IO_ERROR;
        }
        rt->stats->output_rows++;
    }
    return CISV_ROWS_OK;
}

void cisv_rows_options_init(cisv_rows_options_t *options) {
    if (!options) return;
    memset(options, 0, sizeof(*options));
    cisv_config_init(&options->csv_config);
    options->mode = CISV_ROWS_MERGE;
    options->keep = CISV_KEEP_FIRST;
    options->output = stdout;
}

void cisv_rows_stats_init(cisv_rows_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    memcpy(stats->mode, "in-memory", sizeof("in-memory"));
    memcpy(stats->keep, "first", sizeof("first"));
}

const char *cisv_rows_status_name(cisv_rows_status_t status) {
    switch (status) {
        case CISV_ROWS_OK: return "success";
        case CISV_ROWS_USAGE_ERROR: return "usage error";
        case CISV_ROWS_PARSE_ERROR: return "CSV parse error";
        case CISV_ROWS_MISSING_KEY: return "missing key column";
        case CISV_ROWS_HEADER_MISMATCH: return "header mismatch";
        case CISV_ROWS_IO_ERROR: return "I/O error";
        case CISV_ROWS_MEMORY_LIMIT: return "memory limit exceeded";
        case CISV_ROWS_EXTERNAL_ERROR: return "external mode error";
        default: return "unknown error";
    }
}

static const char *rows_command_name(cisv_rows_mode_t mode) {
    switch (mode) {
        case CISV_ROWS_CAT: return "cat rows";
        case CISV_ROWS_DEDUP: return "dedup";
        case CISV_ROWS_FILTER_EXCLUDE: return "filter exclude";
        case CISV_ROWS_MERGE: return "merge rows";
        default: return "unknown";
    }
}

static cisv_rows_status_t validate_options(const cisv_rows_options_t *opt,
                                           char *error,
                                           size_t error_len) {
    if (!opt || !opt->output) {
        rows_set_error(error, error_len, "missing operation options");
        return CISV_ROWS_USAGE_ERROR;
    }
    if (!opt->input_files || opt->input_file_count == 0) {
        rows_set_error(error, error_len, "at least one input file is required");
        return CISV_ROWS_USAGE_ERROR;
    }
    if (opt->external) {
        rows_set_error(error, error_len, "external mode is not available in this build");
        return CISV_ROWS_EXTERNAL_ERROR;
    }
    if (opt->mode != CISV_ROWS_CAT &&
        !opt->use_key_indexes &&
        (!opt->key_columns || opt->key_column_count == 0)) {
        rows_set_error(error, error_len, "at least one key is required");
        return CISV_ROWS_USAGE_ERROR;
    }
    if ((opt->mode == CISV_ROWS_FILTER_EXCLUDE || opt->mode == CISV_ROWS_MERGE) &&
        !opt->exclude_file) {
        rows_set_error(error, error_len, "exclude keys file is required");
        return CISV_ROWS_USAGE_ERROR;
    }
    if (opt->exclude_file &&
        !opt->use_exclude_key_indexes &&
        (!opt->exclude_key_columns || opt->exclude_key_column_count == 0)) {
        rows_set_error(error, error_len, "exclude key column is required");
        return CISV_ROWS_USAGE_ERROR;
    }
    return CISV_ROWS_OK;
}

static size_t estimate_source_rows(const cisv_rows_options_t *opt) {
    size_t bytes = 0;
    for (size_t i = 0; i < opt->input_file_count; i++) {
        size_t sz = rows_file_size(opt->input_files[i]);
        if (sz > SIZE_MAX - bytes) return SIZE_MAX / 64;
        bytes += sz;
    }
    size_t estimate = bytes / 128;
    return estimate > 0 ? estimate : 1024;
}

static size_t estimate_exclude_rows(const cisv_rows_options_t *opt) {
    if (!opt->exclude_file) return 1024;
    size_t bytes = rows_file_size(opt->exclude_file);
    size_t estimate = bytes / 16;
    return estimate > 0 ? estimate : 1024;
}

cisv_rows_status_t cisv_rows_execute(const cisv_rows_options_t *options,
                                     cisv_rows_stats_t *stats,
                                     char *error,
                                     size_t error_len) {
    cisv_rows_status_t status = validate_options(options, error, error_len);
    if (status != CISV_ROWS_OK) return status;

    cisv_rows_stats_t local_stats;
    if (!stats) stats = &local_stats;
    cisv_rows_stats_init(stats);
    snprintf(stats->command, sizeof(stats->command), "%s", rows_command_name(options->mode));
    snprintf(stats->mode, sizeof(stats->mode), "%s", options->external ? "external" : "in-memory");
    snprintf(stats->keep, sizeof(stats->keep), "%s", options->keep == CISV_KEEP_LAST ? "last" : "first");
    stats->input_files = options->input_file_count;

    for (size_t i = 0; i < options->input_file_count; i++) {
        size_t sz = rows_file_size(options->input_files[i]);
        if (sz > SIZE_MAX - stats->bytes_read) stats->bytes_read = SIZE_MAX;
        else stats->bytes_read += sz;
    }
    if (options->exclude_file) {
        size_t sz = rows_file_size(options->exclude_file);
        if (sz > SIZE_MAX - stats->bytes_read) stats->bytes_read = SIZE_MAX;
        else stats->bytes_read += sz;
    }

    cisv_rows_runtime_t rt;
    memset(&rt, 0, sizeof(rt));
    rt.options = options;
    rt.stats = stats;
    rt.error = error;
    rt.error_len = error_len;
    rt.memory_limit = options->memory_limit;

    double start = rows_now_seconds();

    if (key_set_init(&rt.accepted, estimate_source_rows(options), options->memory_limit) != 0 ||
        key_set_init(&rt.excluded, estimate_exclude_rows(options), options->memory_limit) != 0) {
        status = CISV_ROWS_MEMORY_LIMIT;
        goto done;
    }

    cisv_writer_config writer_config = {
        .delimiter = options->csv_config.delimiter ? options->csv_config.delimiter : ',',
        .quote_char = options->csv_config.quote ? options->csv_config.quote : '"',
        .always_quote = 0,
        .use_crlf = 0,
        .null_string = "",
        .buffer_size = 1 << 20
    };
    rt.writer = cisv_writer_create_config(options->output, &writer_config);
    if (!rt.writer) {
        status = CISV_ROWS_IO_ERROR;
        rows_set_error(error, error_len, "failed to create CSV writer");
        goto done;
    }

    status = load_exclude_keys(&rt);
    if (status != CISV_ROWS_OK) goto done;

    for (size_t i = 0; i < options->input_file_count; i++) {
        status = process_source_file(&rt, options->input_files[i]);
        if (status != CISV_ROWS_OK) goto done;
    }

    if (options->keep == CISV_KEEP_LAST &&
        (options->mode == CISV_ROWS_DEDUP || options->mode == CISV_ROWS_MERGE)) {
        status = flush_keep_last_rows(&rt);
        if (status != CISV_ROWS_OK) goto done;
    }

    if (cisv_writer_flush(rt.writer) != 0) {
        status = CISV_ROWS_IO_ERROR;
        rows_set_error(error, error_len, "failed to flush CSV writer");
        goto done;
    }
    stats->bytes_written = cisv_writer_bytes_written(rt.writer);

done:
    if (rt.writer) {
        if (status == CISV_ROWS_OK && stats->bytes_written == 0) {
            stats->bytes_written = cisv_writer_bytes_written(rt.writer);
        }
        cisv_writer_destroy(rt.writer);
    }
    stats->elapsed_seconds = rows_now_seconds() - start;
    stats->peak_rss_bytes = rows_peak_rss_bytes();

    free(rt.source_key_indexes);
    owned_row_destroy(&rt.header);
    row_store_destroy(&rt.row_store);
    key_set_destroy(&rt.accepted);
    key_set_destroy(&rt.excluded);

    if (status != CISV_ROWS_OK && error && error_len > 0 && error[0] == '\0') {
        rows_set_error(error, error_len, "%s", cisv_rows_status_name(status));
    }
    return status;
}
