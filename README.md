# cisv-core

![License](https://img.shields.io/badge/license-MIT-blue)

Core C library for CISV with SIMD optimizations (AVX-512/AVX2 + scalar fallback).

## Features

- SIMD-accelerated CSV parsing
- Streaming parser and iterator API
- Fast row counting API
- Configurable delimiter, quote, escape, comments, trimming
- Transform hooks and parallel chunk support

## Performance

Typical throughput (machine dependent):

| Test | Throughput |
|------|------------|
| Simple CSV | 165-290 MB/s |
| Quoted fields | 175-189 MB/s |
| Large fields | 153-169 MB/s |

## Installation

```bash
git clone https://github.com/Sanix-Darker/cisv-core
cd cisv-core
make -C core all
```

## C API

### Basic example

```c
cisv_config cfg;
cisv_config_init(&cfg);
cisv_parser *p = cisv_parser_create_with_config(&cfg);
cisv_parser_parse_file(p, "data.csv");
cisv_parser_destroy(p);
```

### Detailed example (iterator + early exit)

```c
cisv_config cfg;
cisv_config_init(&cfg);
cisv_iterator_t *it = cisv_iterator_open("large.csv", &cfg);
const char **fields;
const size_t *lengths;
size_t field_count;
while (cisv_iterator_next(it, &fields, &lengths, &field_count) == CISV_ITER_OK) {
    if (field_count > 0 && lengths[0] == 4) break;
}
cisv_iterator_close(it);
```

More runnable examples: [`examples/`](./examples)

## Benchmarks

```bash
docker build -t cisv-core-bench -f core/benchmarks/Dockerfile .
# or run workflow: .github/workflows/benchmark.yml
```

## Repository Map

- Core: https://github.com/Sanix-Darker/cisv-core
- CLI: https://github.com/Sanix-Darker/cisv-cli
- PHP binding: https://github.com/Sanix-Darker/cisv-php
- Node.js binding: https://github.com/Sanix-Darker/cisv-nodejs
- Python binding: https://github.com/Sanix-Darker/cisv-python
