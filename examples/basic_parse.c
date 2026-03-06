#include <stdio.h>
#include <cisv/parser.h>

static void on_field(void *user, const char *data, size_t len) {
    (void)user;
    printf("[%.*s] ", (int)len, data);
}

static void on_row(void *user) {
    (void)user;
    printf("\n");
}

int main(void) {
    cisv_config cfg;
    cisv_config_init(&cfg);
    cfg.field_cb = on_field;
    cfg.row_cb = on_row;

    cisv_parser *p = cisv_parser_create_with_config(&cfg);
    if (!p) {
        fprintf(stderr, "failed to create parser\n");
        return 1;
    }

    if (cisv_parser_parse_file(p, "examples/sample.csv") != 0) {
        fprintf(stderr, "failed to parse file\n");
        cisv_parser_destroy(p);
        return 1;
    }

    cisv_parser_destroy(p);
    return 0;
}
