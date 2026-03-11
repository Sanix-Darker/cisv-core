#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cisv/parser.h"

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s generate-flat <path> <rows> <cols>\n"
            "  %s generate-json <path> <rows> <cols> <json-col>\n"
            "  %s parse <path>\n"
            "  %s count <path>\n"
            "  %s batch <path>\n"
            "  %s parallel <path> <threads>\n",
            prog, prog, prog, prog, prog, prog);
}

static int write_flat_dataset(const char *path, long rows, int cols) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    setvbuf(f, NULL, _IOFBF, 1 << 20);

    for (int c = 0; c < cols; c++) {
        fprintf(f, "h%d%c", c, c == cols - 1 ? '\n' : ',');
    }

    char *row = malloc((size_t)cols * 2 + 1);
    if (!row) {
        fclose(f);
        return -1;
    }

    char *p = row;
    for (int c = 0; c < cols; c++) {
        *p++ = '0';
        *p++ = (c == cols - 1) ? '\n' : ',';
    }
    size_t row_len = (size_t)(p - row);

    for (long i = 0; i < rows; i++) {
        if (fwrite(row, 1, row_len, f) != row_len) {
            free(row);
            fclose(f);
            return -1;
        }
    }

    free(row);
    return fclose(f);
}

static int write_json_dataset(const char *path, long rows, int cols, int json_col) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    setvbuf(f, NULL, _IOFBF, 1 << 20);

    for (int c = 0; c < cols; c++) {
        fprintf(f, "h%d%c", c, c == cols - 1 ? '\n' : ',');
    }

    for (long row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            if (col == json_col) {
                fprintf(f,
                        "\"{\n"
                        "  \"\"id\"\": %ld,\n"
                        "  \"\"name\"\": \"\"row-%ld\"\",\n"
                        "  \"\"items\"\": [\n"
                        "    {\"\"k\"\": \"\"a\"\", \"\"v\"\": 1},\n"
                        "    {\"\"k\"\": \"\"b\"\", \"\"v\"\": 2}\n"
                        "  ]\n"
                        "}\"",
                        row, row);
            } else {
                fputc('0', f);
            }
            fputc(col == cols - 1 ? '\n' : ',', f);
        }
    }

    return fclose(f);
}

static size_t parse_rows;
static size_t parse_fields;

static void bench_field_cb(void *user, const char *data, size_t len) {
    (void)user;
    (void)data;
    (void)len;
    parse_fields++;
}

static void bench_row_cb(void *user) {
    (void)user;
    parse_rows++;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "generate-flat") == 0) {
        if (argc != 5) {
            usage(argv[0]);
            return 1;
        }
        long rows = atol(argv[3]);
        int cols = atoi(argv[4]);
        return write_flat_dataset(argv[2], rows, cols) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "generate-json") == 0) {
        if (argc != 6) {
            usage(argv[0]);
            return 1;
        }
        long rows = atol(argv[3]);
        int cols = atoi(argv[4]);
        int json_col = atoi(argv[5]);
        return write_json_dataset(argv[2], rows, cols, json_col) == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "parse") == 0) {
        cisv_config config;
        cisv_config_init(&config);
        config.field_cb = bench_field_cb;
        config.row_cb = bench_row_cb;

        cisv_parser *parser = cisv_parser_create_with_config(&config);
        if (!parser) return 1;

        double t0 = now_seconds();
        int rc = cisv_parser_parse_file(parser, argv[2]);
        double t1 = now_seconds();

        printf("mode=parse rc=%d rows=%zu fields=%zu seconds=%.6f\n",
               rc, parse_rows, parse_fields, t1 - t0);
        cisv_parser_destroy(parser);
        return rc == 0 ? 0 : 1;
    }

    if (strcmp(argv[1], "count") == 0) {
        double t0 = now_seconds();
        size_t rows = cisv_parser_count_rows(argv[2]);
        double t1 = now_seconds();
        printf("mode=count rows=%zu seconds=%.6f\n", rows, t1 - t0);
        return 0;
    }

    if (strcmp(argv[1], "batch") == 0) {
        cisv_config config;
        cisv_config_init(&config);

        double t0 = now_seconds();
        cisv_result_t *result = cisv_parse_file_batch(argv[2], &config);
        double t1 = now_seconds();

        printf("mode=batch rows=%zu fields=%zu err=%d seconds=%.6f\n",
               result ? result->row_count : 0,
               result ? result->total_fields : 0,
               result ? result->error_code : -1,
               t1 - t0);
        cisv_result_free(result);
        return 0;
    }

    if (strcmp(argv[1], "parallel") == 0) {
        if (argc != 4) {
            usage(argv[0]);
            return 1;
        }

        int threads = atoi(argv[3]);
        cisv_config config;
        cisv_config_init(&config);

        double t0 = now_seconds();
        int result_count = 0;
        cisv_result_t **results = cisv_parse_file_parallel(argv[2], &config, threads, &result_count);
        double t1 = now_seconds();

        size_t rows = 0;
        size_t fields = 0;
        int err = 0;
        for (int i = 0; i < result_count; i++) {
            if (!results[i] || results[i]->error_code != 0) {
                err = 1;
                continue;
            }
            rows += results[i]->row_count;
            fields += results[i]->total_fields;
        }

        printf("mode=parallel rows=%zu fields=%zu err=%d chunks=%d seconds=%.6f\n",
               rows, fields, err, result_count, t1 - t0);
        cisv_results_free(results, result_count);
        return err ? 1 : 0;
    }

    usage(argv[0]);
    return 1;
}
