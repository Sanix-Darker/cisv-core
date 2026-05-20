#ifndef CISV_OPERATIONS_H
#define CISV_OPERATIONS_H

#include <stddef.h>
#include <stdio.h>

#include "parser.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CISV_ROWS_CAT = 0,
    CISV_ROWS_DEDUP = 1,
    CISV_ROWS_FILTER_EXCLUDE = 2,
    CISV_ROWS_MERGE = 3
} cisv_rows_mode_t;

typedef enum {
    CISV_KEEP_FIRST = 0,
    CISV_KEEP_LAST = 1
} cisv_keep_policy_t;

typedef enum {
    CISV_ROWS_OK = 0,
    CISV_ROWS_USAGE_ERROR = 2,
    CISV_ROWS_PARSE_ERROR = 3,
    CISV_ROWS_MISSING_KEY = 4,
    CISV_ROWS_HEADER_MISMATCH = 5,
    CISV_ROWS_IO_ERROR = 6,
    CISV_ROWS_MEMORY_LIMIT = 7,
    CISV_ROWS_EXTERNAL_ERROR = 8
} cisv_rows_status_t;

typedef struct {
    cisv_rows_mode_t mode;

    const char **input_files;
    size_t input_file_count;

    const char **key_columns;
    size_t key_column_count;
    const size_t *key_indexes;
    size_t key_index_count;
    int use_key_indexes;

    const char *exclude_file;
    const char **exclude_key_columns;
    size_t exclude_key_column_count;
    const size_t *exclude_key_indexes;
    size_t exclude_key_index_count;
    int use_exclude_key_indexes;

    cisv_keep_policy_t keep;
    int no_header;
    int drop_empty_key;
    int ignore_header_mismatch;
    int external;
    const char *tmp_dir;
    size_t memory_limit;

    FILE *output;
    cisv_config csv_config;
} cisv_rows_options_t;

typedef struct {
    char command[32];
    char mode[16];
    char keep[8];

    size_t input_files;
    size_t input_rows;
    size_t output_rows;
    size_t duplicate_rows;
    size_t excluded_rows;
    size_t malformed_rows;
    size_t empty_key_rows;
    size_t header_mismatch_rows;

    size_t bytes_read;
    size_t bytes_written;
    size_t temp_bytes_read;
    size_t temp_bytes_written;
    size_t peak_rss_bytes;

    double elapsed_seconds;
} cisv_rows_stats_t;

void cisv_rows_options_init(cisv_rows_options_t *options);
void cisv_rows_stats_init(cisv_rows_stats_t *stats);

cisv_rows_status_t cisv_rows_execute(const cisv_rows_options_t *options,
                                     cisv_rows_stats_t *stats,
                                     char *error,
                                     size_t error_len);

const char *cisv_rows_status_name(cisv_rows_status_t status);

#ifdef __cplusplus
}
#endif

#endif
