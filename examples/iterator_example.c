#include <stdio.h>
#include <cisv/parser.h>

int main(void) {
    cisv_config cfg;
    cisv_config_init(&cfg);

    cisv_iterator_t *it = cisv_iterator_open("examples/sample.csv", &cfg);
    if (!it) {
        fprintf(stderr, "failed to open iterator\n");
        return 1;
    }

    const char **fields = NULL;
    const size_t *lengths = NULL;
    size_t field_count = 0;

    while (cisv_iterator_next(it, &fields, &lengths, &field_count) == CISV_ITER_OK) {
        for (size_t i = 0; i < field_count; i++) {
            printf("%.*s%s", (int)lengths[i], fields[i], (i + 1 < field_count) ? "," : "\n");
        }
    }

    cisv_iterator_close(it);
    return 0;
}
