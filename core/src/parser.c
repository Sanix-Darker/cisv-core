#include "cisv/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>  // For INT_MAX
#include <sys/stat.h>
#include <errno.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#if defined(__AVX512F__) && defined(__AVX512BW__)
#include <immintrin.h>
#endif

#ifdef __AVX2__
#include <immintrin.h>
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__SSE2__) && !defined(__AVX2__) && !(defined(__AVX512F__) && defined(__AVX512BW__))
#include <emmintrin.h>
#endif

// Cache optimization constants
#define CACHE_LINE_SIZE 64
#define L1_SIZE (32 * 1024)
#define L2_SIZE (256 * 1024)
#define PREFETCH_DISTANCE 1024

// Parser states - keep minimal for branch prediction
#define S_NORMAL  0
#define S_QUOTED  1
#define S_ESCAPE  2

typedef struct cisv_parser {
    // Hot path data - first cache line
    const uint8_t *cur __attribute__((aligned(64)));
    const uint8_t *end;
    const uint8_t *field_start;
    uint8_t state;
    char delimiter;
    char quote;
    char escape;

    // Second cache line - callbacks and config
    cisv_field_cb fcb __attribute__((aligned(64)));
    cisv_row_cb rcb;
    void *user;
    bool trim;
    bool skip_empty_lines;
    int line_num;

    // Cold data - rarely accessed
    uint8_t *base __attribute__((aligned(64)));
    size_t size;
    int fd;
    cisv_error_cb ecb;
    char comment;
    int from_line;
    int to_line;
    size_t max_row_size;
    size_t memory_budget;
    bool skip_lines_with_error;
    bool has_row_controls;
    bool relaxed;
    bool had_error;
    void (*parse_impl)(struct cisv_parser *p);

    // Statistics
    size_t rows;
    size_t fields;
    size_t current_row_fields;
    size_t current_row_size;
    bool skip_current_row;
    bool row_is_comment;

    // Buffer for accumulating quoted field content
    uint8_t *quote_buffer __attribute__((aligned(64)));
    size_t quote_buffer_size;
    size_t quote_buffer_pos;
    const uint8_t *quoted_field_start;
    bool quoted_field_buffered;
    bool quoted_pending_quote;
    bool quoted_pending_escape;
    bool skip_leading_lf;

    // Buffer for streaming mode - holds partial unquoted fields across chunks
    uint8_t *stream_buffer;
    size_t stream_buffer_size;
    size_t stream_buffer_pos;
    bool streaming_mode;
} cisv_parser;

// Ultra-fast whitespace lookup table - O(1) direct index instead of bit extraction
// Covers: space (32), tab (9), CR (13), LF (10)
static const uint8_t ws_lookup[256] = {
    [' '] = 1, ['\t'] = 1, ['\r'] = 1, ['\n'] = 1
};

#define is_ws(c) (ws_lookup[(uint8_t)(c)])

// =============================================================================
// SWAR (SIMD Within A Register) - 1 Billion Row Challenge technique
// Processes 8 bytes at a time without SIMD instructions
// =============================================================================

// Check if any byte in a 64-bit word equals a target character
// Returns non-zero mask with high bit set for each matching byte
static inline uint64_t swar_has_byte(uint64_t word, uint8_t target) {
    uint64_t mask = target * 0x0101010101010101ULL;
    uint64_t xored = word ^ mask;
    // High bit set if any byte is zero (i.e., was a match)
    return (xored - 0x0101010101010101ULL) & ~xored & 0x8080808080808080ULL;
}

// Check if word contains delimiter, row ending, or quote.
static inline uint64_t swar_has_special(uint64_t word, char delim, char quote) {
    return swar_has_byte(word, delim) |
           swar_has_byte(word, '\n') |
           swar_has_byte(word, '\r') |
           swar_has_byte(word, quote);
}

// SIMD-accelerated whitespace trimming
#ifdef __AVX2__
// Skip leading whitespace using AVX2 - processes 32 bytes at a time
// PERF: __restrict hints allow better optimization by promising no aliasing
static inline const uint8_t *skip_ws_avx2(
    const uint8_t * __restrict start,
    const uint8_t * __restrict end
) {
    const __m256i space = _mm256_set1_epi8(' ');
    const __m256i tab = _mm256_set1_epi8('\t');
    const __m256i cr = _mm256_set1_epi8('\r');
    const __m256i nl = _mm256_set1_epi8('\n');

    while (start + 32 <= end) {
        __m256i chunk = _mm256_loadu_si256((const __m256i*)start);

        // Check for any of the 4 whitespace characters
        __m256i is_space = _mm256_cmpeq_epi8(chunk, space);
        __m256i is_tab = _mm256_cmpeq_epi8(chunk, tab);
        __m256i is_cr = _mm256_cmpeq_epi8(chunk, cr);
        __m256i is_nl = _mm256_cmpeq_epi8(chunk, nl);

        __m256i is_ws_vec = _mm256_or_si256(
            _mm256_or_si256(is_space, is_tab),
            _mm256_or_si256(is_cr, is_nl)
        );

        // Get mask of non-whitespace bytes
        uint32_t mask = ~_mm256_movemask_epi8(is_ws_vec);

        if (mask) {
            // Found non-whitespace byte, return position of first one
            return start + __builtin_ctz(mask);
        }
        start += 32;
    }

    // Scalar fallback for remainder
    while (start < end && is_ws(*start)) start++;
    return start;
}

// Find last non-whitespace using AVX2 - scans backwards
// PERF: __restrict hints allow better optimization by promising no aliasing
static inline const uint8_t *rskip_ws_avx2(
    const uint8_t * __restrict start,
    const uint8_t * __restrict end
) {
    const __m256i space = _mm256_set1_epi8(' ');
    const __m256i tab = _mm256_set1_epi8('\t');
    const __m256i cr = _mm256_set1_epi8('\r');
    const __m256i nl = _mm256_set1_epi8('\n');

    while (end - 32 >= start) {
        const uint8_t *check = end - 32;
        __m256i chunk = _mm256_loadu_si256((const __m256i*)check);

        __m256i is_space = _mm256_cmpeq_epi8(chunk, space);
        __m256i is_tab = _mm256_cmpeq_epi8(chunk, tab);
        __m256i is_cr = _mm256_cmpeq_epi8(chunk, cr);
        __m256i is_nl = _mm256_cmpeq_epi8(chunk, nl);

        __m256i is_ws_vec = _mm256_or_si256(
            _mm256_or_si256(is_space, is_tab),
            _mm256_or_si256(is_cr, is_nl)
        );

        uint32_t mask = ~_mm256_movemask_epi8(is_ws_vec);

        if (mask) {
            // Found non-whitespace, return position after last one
            return check + 32 - __builtin_clz(mask);
        }
        end -= 32;
    }

    // Scalar fallback
    while (start < end && is_ws(*(end - 1))) end--;
    return end;
}
#endif

void cisv_config_init(cisv_config *config) {
    memset(config, 0, sizeof(*config));
    config->delimiter = ',';
    config->quote = '"';
    config->from_line = 1;
}

static bool cisv_config_is_valid(const cisv_config *config) {
    if (!config) return false;

    if (config->delimiter == '\0' || config->quote == '\0') return false;
    if (config->delimiter == config->quote) return false;
    if (config->delimiter == '\n' || config->delimiter == '\r') return false;
    if (config->quote == '\n' || config->quote == '\r') return false;

    if (config->escape != '\0') {
        if (config->escape == '\n' || config->escape == '\r') return false;
        if (config->escape == config->delimiter) return false;
        if (config->escape == config->quote) return false;
    }

    if (config->comment == '\n' || config->comment == '\r') return false;
    if (config->from_line < 0 || config->to_line < 0) return false;
    int from_line = config->from_line > 0 ? config->from_line : 1;
    if (config->to_line != 0 && config->to_line < from_line) return false;

    return true;
}

static const char *cisv_getenv_first(const char *primary, const char *fallback) {
    const char *value = getenv(primary);
    if (value && *value) return value;
    value = getenv(fallback);
    return (value && *value) ? value : NULL;
}

static bool parse_positive_size_env(const char *value, size_t *out) {
    if (!value || !*value || !out) return false;

    errno = 0;
    char *end = NULL;
    unsigned long long number = strtoull(value, &end, 10);
    if (errno != 0 || end == value || number == 0) return false;

    while (*end == ' ' || *end == '\t') end++;

    unsigned long long multiplier = 1;
    if (*end != '\0') {
        if ((end[0] == 'k' || end[0] == 'K') && end[1] == '\0') {
            multiplier = 1024ULL;
        } else if ((end[0] == 'm' || end[0] == 'M') && end[1] == '\0') {
            multiplier = 1024ULL * 1024ULL;
        } else if ((end[0] == 'g' || end[0] == 'G') && end[1] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else if ((end[0] == 't' || end[0] == 'T') && end[1] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        } else if ((end[0] == 'k' || end[0] == 'K') &&
                   (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
            multiplier = 1024ULL;
        } else if ((end[0] == 'm' || end[0] == 'M') &&
                   (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
            multiplier = 1024ULL * 1024ULL;
        } else if ((end[0] == 'g' || end[0] == 'G') &&
                   (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else if ((end[0] == 't' || end[0] == 'T') &&
                   (end[1] == 'b' || end[1] == 'B') && end[2] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        } else if ((end[0] == 'k' || end[0] == 'K') &&
                   (end[1] == 'i' || end[1] == 'I') &&
                   (end[2] == 'b' || end[2] == 'B') && end[3] == '\0') {
            multiplier = 1024ULL;
        } else if ((end[0] == 'm' || end[0] == 'M') &&
                   (end[1] == 'i' || end[1] == 'I') &&
                   (end[2] == 'b' || end[2] == 'B') && end[3] == '\0') {
            multiplier = 1024ULL * 1024ULL;
        } else if ((end[0] == 'g' || end[0] == 'G') &&
                   (end[1] == 'i' || end[1] == 'I') &&
                   (end[2] == 'b' || end[2] == 'B') && end[3] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        } else if ((end[0] == 't' || end[0] == 'T') &&
                   (end[1] == 'i' || end[1] == 'I') &&
                   (end[2] == 'b' || end[2] == 'B') && end[3] == '\0') {
            multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        } else {
            return false;
        }
    }

    if (number > (unsigned long long)SIZE_MAX / multiplier) return false;
    *out = (size_t)(number * multiplier);
    return true;
}

static bool parse_positive_int_env(const char *value, int *out) {
    size_t parsed = 0;
    if (!parse_positive_size_env(value, &parsed) || parsed > (size_t)INT_MAX) {
        return false;
    }
    *out = (int)parsed;
    return true;
}

static int cisv_runtime_max_procs(void) {
    int parsed = 0;
    const char *value = cisv_getenv_first("CISV_MAX_PROCS", "GOMAXPROCS");
    if (parse_positive_int_env(value, &parsed)) {
        return parsed;
    }
    return 0;
}

static size_t cisv_runtime_memory_budget(void) {
    size_t parsed = 0;
    const char *value = cisv_getenv_first("CISV_MAX_MEMORY", "GOMEMLIMIT");
    if (parse_positive_size_env(value, &parsed)) {
        return parsed;
    }
    return 0;
}

static size_t cisv_runtime_max_row_size(void) {
    size_t parsed = 0;
    const char *value = getenv("CISV_MAX_ROW_SIZE");
    if (parse_positive_size_env(value, &parsed)) {
        return parsed;
    }
    return 0;
}

static size_t cisv_runtime_parallel_min_bytes(void) {
    size_t parsed = 0;
    const char *value = getenv("CISV_PARALLEL_MIN_BYTES");
    if (parse_positive_size_env(value, &parsed)) {
        return parsed;
    }
    return 0;
}

static size_t cisv_adaptive_row_limit(size_t memory_budget) {
    if (memory_budget == 0) return 0;

    size_t limit = memory_budget / 16;
    if (limit < 64 * 1024) {
        limit = 64 * 1024;
    }
    if (limit > 128 * 1024 * 1024) {
        limit = 128 * 1024 * 1024;
    }
    if (limit > memory_budget) {
        limit = memory_budget;
    }
    return limit;
}

static size_t cisv_buffer_limit_from_budget(size_t memory_budget) {
    const size_t max_buffer = 100 * 1024 * 1024;
    const size_t min_buffer = 64 * 1024;
    if (memory_budget == 0) return max_buffer;
    size_t limit = memory_budget / 4;
    if (limit < min_buffer) {
        limit = min_buffer;
    }
    if (limit > max_buffer) {
        limit = max_buffer;
    }
    return limit;
}

// Maximum quote buffer size to prevent DoS (100MB)
#define MAX_QUOTE_BUFFER_SIZE (100 * 1024 * 1024)
// Minimum buffer increment for efficiency (64KB)
#define MIN_BUFFER_INCREMENT (64 * 1024)
// Default maximum field size (1MB) - configurable
#define DEFAULT_MAX_FIELD_SIZE (1 * 1024 * 1024)

// Ensure quote buffer has enough space
// Optimized: 1.5x growth with 64KB minimum increment, cache-line aligned
static inline void parser_report_error(cisv_parser *p, int line, const char *msg);

static inline bool ensure_quote_buffer(cisv_parser *p, size_t needed) {
    size_t required = p->quote_buffer_pos + needed;
    if (__builtin_expect(required <= p->quote_buffer_size, 1)) {
        return true;  // Fast path: buffer has space
    }

    // Check for overflow using compiler builtin
    size_t new_size;
    if (__builtin_add_overflow(p->quote_buffer_pos, needed, &new_size)) {
        return false;
    }

    // 1.5x growth: reduces memory waste from ~50% to ~33%
    size_t grow_size = p->quote_buffer_size + (p->quote_buffer_size >> 1);
    if (grow_size < new_size) grow_size = new_size;
    if (grow_size < MIN_BUFFER_INCREMENT) grow_size = MIN_BUFFER_INCREMENT;
    new_size = grow_size;

    // Align to cache line for SIMD access
    new_size = (new_size + CACHE_LINE_SIZE - 1) & ~(size_t)(CACHE_LINE_SIZE - 1);

    // Enforce maximum buffer size to prevent DoS and configured memory budget.
    if (new_size > cisv_buffer_limit_from_budget(p->memory_budget)) return false;

    // cppcheck-suppress memleak
    // realloc ownership is transferred to tmp; on failure old pointer stays in p->quote_buffer.
    void *tmp = realloc(p->quote_buffer, new_size);
    if (__builtin_expect(!tmp, 0)) return false;
    p->quote_buffer = tmp;
    p->quote_buffer_size = new_size;
    return true;
}

// Append to quote buffer
static inline bool append_to_quote_buffer(cisv_parser *p, const uint8_t *data, size_t len) {
    if (!ensure_quote_buffer(p, len)) return false;
    memcpy(p->quote_buffer + p->quote_buffer_pos, data, len);
    p->quote_buffer_pos += len;
    return true;
}

// Ensure stream buffer has enough space (for streaming mode partial fields)
// SECURITY: Uses compiler built-ins for overflow-safe arithmetic
static inline bool ensure_stream_buffer(cisv_parser *p, size_t needed) {
    // Check required size with overflow protection
    size_t required;
    if (__builtin_add_overflow(p->stream_buffer_pos, needed, &required)) {
        parser_report_error(p, p->line_num + 1, "Stream buffer size overflow");
        return false;
    }

    if (required <= p->stream_buffer_size) {
        return true;  // Fast path: buffer has space
    }

    // 1.5x growth: reduces memory waste from ~50% to ~33%
    size_t grow_size = p->stream_buffer_size + (p->stream_buffer_size >> 1);
    size_t new_size = (grow_size > required) ? grow_size : required;

    // Enforce maximum buffer size to prevent DoS and configured memory budget.
    if (new_size > cisv_buffer_limit_from_budget(p->memory_budget)) {
        parser_report_error(p, p->line_num + 1, "Stream buffer exceeds maximum size");
        return false;
    }

    // cppcheck-suppress memleak
    // realloc ownership is transferred to tmp; on failure old pointer stays in p->stream_buffer.
    void *tmp = realloc(p->stream_buffer, new_size);
    if (!tmp) {
        // SECURITY: On realloc failure, old buffer is still valid
        // Report error but don't invalidate existing buffer
        parser_report_error(p, p->line_num + 1, "Stream buffer allocation failed");
        return false;
    }
    p->stream_buffer = tmp;
    p->stream_buffer_size = new_size;
    return true;
}

// Append to stream buffer
static inline bool append_to_stream_buffer(cisv_parser *p, const uint8_t *data, size_t len) {
    if (!ensure_stream_buffer(p, len)) return false;
    memcpy(p->stream_buffer + p->stream_buffer_pos, data, len);
    p->stream_buffer_pos += len;
    return true;
}

static inline void yield_row(cisv_parser *p);
static inline void yield_field(cisv_parser *p, const uint8_t *start, const uint8_t *end);

static inline void parser_report_error(cisv_parser *p, int line, const char *msg) {
    if (!p) return;
    if (!p->relaxed) {
        p->had_error = true;
    }
    if (p->skip_lines_with_error) {
        p->skip_current_row = true;
    }
    if (p->ecb) {
        p->ecb(p->user, line, msg);
    }
}

static inline bool row_in_emit_range(const cisv_parser *p) {
    int row_num = p->line_num + 1;
    return row_num >= p->from_line && (!p->to_line || row_num <= p->to_line);
}

static inline bool is_empty_physical_row(const cisv_parser *p, const uint8_t *field_end) {
    return p->skip_empty_lines &&
           p->current_row_fields == 0 &&
           p->field_start == field_end;
}

static inline void yield_line_end(cisv_parser *p, const uint8_t *field_end) {
    if (!is_empty_physical_row(p, field_end)) {
        yield_field(p, p->field_start, field_end);
    }
    yield_row(p);
}

static inline void mark_streaming_split_cr(cisv_parser *p, const uint8_t *after_cr) {
    if (p->streaming_mode && after_cr == p->end) {
        p->skip_leading_lf = true;
    }
}

// Inline hot-path functions
static inline void yield_field(cisv_parser *p, const uint8_t *start, const uint8_t *end) {
    bool emit_field = p->fcb && row_in_emit_range(p);

    // In streaming mode, check if we have buffered partial field data
    // SECURITY: Add NULL check for stream_buffer to prevent NULL dereference
    if (emit_field && p->streaming_mode && p->stream_buffer_pos > 0 && p->stream_buffer) {
        // Append current field data to stream buffer and yield from there
        size_t current_len = end - start;
        if (current_len > 0) {
            if (!append_to_stream_buffer(p, start, current_len)) {
                // Buffer overflow - report error and yield what we have from original pointers
                parser_report_error(p, p->line_num + 1, "Field exceeds maximum buffer size");
                p->stream_buffer_pos = 0;
                // Don't return - yield the original field data
            } else {
                start = p->stream_buffer;
                end = p->stream_buffer + p->stream_buffer_pos;
            }
        } else {
            start = p->stream_buffer;
            end = p->stream_buffer + p->stream_buffer_pos;
        }
    } else if (p->streaming_mode && p->stream_buffer_pos > 0) {
        p->stream_buffer_pos = 0;
    }

    if (__builtin_expect(p->trim, 0)) {
#ifdef __AVX2__
        // Use SIMD for fields larger than 64 bytes, scalar for smaller
        size_t len = end - start;
        if (__builtin_expect(len >= 64, 0)) {
            start = skip_ws_avx2(start, end);
            if (start < end) {
                end = rskip_ws_avx2(start, end);
            }
        } else {
            // Scalar path for small fields
            while (start < end && is_ws(*start)) start++;
            while (start < end && is_ws(*(end-1))) end--;
        }
#else
        // Trim leading whitespace - expect few iterations
        while (start < end && __builtin_expect(is_ws(*start), 0)) start++;
        // Trim trailing whitespace - expect few iterations
        while (start < end && __builtin_expect(is_ws(*(end-1)), 0)) end--;
#endif
    }

    size_t field_len = (size_t)(end - start);

    if (__builtin_expect(p->has_row_controls, 0)) {
        // Detect comment lines from the first unquoted field.
        if (p->current_row_fields == 0) {
            p->row_is_comment = (field_len > 0 && start[0] == (uint8_t)p->comment && p->comment != 0);
            if (p->row_is_comment) {
                p->skip_current_row = true;
            }
        }

        if (!p->skip_current_row && p->max_row_size > 0) {
            size_t next_size = p->current_row_size + field_len + 1;  // +1 delimiter/newline budget
            if (next_size < p->current_row_size || next_size > p->max_row_size) {
                parser_report_error(p, p->line_num + 1, "Row exceeds max_row_size");
                if (p->skip_lines_with_error) {
                    p->skip_current_row = true;
                }
            }
            p->current_row_size = next_size;
        }

        if (p->skip_current_row) {
            if (__builtin_expect(p->streaming_mode, 0)) {
                p->stream_buffer_pos = 0;
            }
            return;
        }
    }

    if (emit_field) {
        p->fcb(p->user, (const char*)start, end - start);
    }
    p->fields++;
    p->current_row_fields++;

    // Clear stream buffer after yielding
    if (__builtin_expect(p->streaming_mode, 0)) {
        p->stream_buffer_pos = 0;
    }
}

// Yield field from quote buffer
static inline void yield_quoted_field(cisv_parser *p) {
    const uint8_t *start = p->quote_buffer;
    const uint8_t *end = p->quote_buffer + p->quote_buffer_pos;
    bool emit_field = p->fcb && row_in_emit_range(p);

    if (__builtin_expect(p->trim, 0)) {
        // Trim leading whitespace - expect few iterations
        while (start < end && __builtin_expect(is_ws(*start), 0)) start++;
        // Trim trailing whitespace - expect few iterations
        while (start < end && __builtin_expect(is_ws(*(end-1)), 0)) end--;
    }

    size_t field_len = (size_t)(end - start);

    if (__builtin_expect(p->has_row_controls, 0)) {
        // Quoted first fields are never comment-line prefixes.
        if (p->current_row_fields == 0) {
            p->row_is_comment = false;
        }

        if (!p->skip_current_row && p->max_row_size > 0) {
            size_t next_size = p->current_row_size + field_len + 1;  // +1 delimiter/newline budget
            if (next_size < p->current_row_size || next_size > p->max_row_size) {
                parser_report_error(p, p->line_num + 1, "Row exceeds max_row_size");
                if (p->skip_lines_with_error) {
                    p->skip_current_row = true;
                }
            }
            p->current_row_size = next_size;
        }

        if (p->skip_current_row) {
            p->quote_buffer_pos = 0;
            p->quoted_field_start = NULL;
            p->quoted_field_buffered = false;
            p->quoted_pending_quote = false;
            p->quoted_pending_escape = false;
            return;
        }
    }

    if (emit_field) {
        p->fcb(p->user, (const char*)start, end - start);
    }
    p->fields++;
    p->current_row_fields++;

    p->quote_buffer_pos = 0;
    p->quoted_field_start = NULL;
    p->quoted_field_buffered = false;
    p->quoted_pending_quote = false;
    p->quoted_pending_escape = false;
}

static inline void yield_quoted_span(cisv_parser *p, const uint8_t *start, const uint8_t *end) {
    bool emit_field = p->fcb && row_in_emit_range(p);

    if (__builtin_expect(p->trim, 0)) {
        while (start < end && __builtin_expect(is_ws(*start), 0)) start++;
        while (start < end && __builtin_expect(is_ws(*(end - 1)), 0)) end--;
    }

    size_t field_len = (size_t)(end - start);

    if (__builtin_expect(p->has_row_controls, 0)) {
        if (p->current_row_fields == 0) {
            p->row_is_comment = false;
        }

        if (!p->skip_current_row && p->max_row_size > 0) {
            size_t next_size = p->current_row_size + field_len + 1;
            if (next_size < p->current_row_size || next_size > p->max_row_size) {
                parser_report_error(p, p->line_num + 1, "Row exceeds max_row_size");
                if (p->skip_lines_with_error) {
                    p->skip_current_row = true;
                }
            }
            p->current_row_size = next_size;
        }

        if (p->skip_current_row) {
            return;
        }
    }

    if (emit_field) {
        p->fcb(p->user, (const char *)start, field_len);
    }
    p->fields++;
    p->current_row_fields++;
}

static inline void consume_post_quoted_delimiter(cisv_parser *p) {
    while (p->cur < p->end && (*p->cur == ' ' || *p->cur == '\t')) {
        p->cur++;
    }

    if (p->cur < p->end && *p->cur == p->delimiter) {
        p->cur++;
    } else if (p->cur < p->end && *p->cur == '\n') {
        p->cur++;
        yield_row(p);
    } else if (p->cur < p->end && *p->cur == '\r' &&
               p->cur + 1 < p->end && *(p->cur + 1) == '\n') {
        p->cur += 2;
        yield_row(p);
    } else if (p->cur < p->end && *p->cur == '\r') {
        p->cur++;
        mark_streaming_split_cr(p, p->cur);
        yield_row(p);
    } else if (p->cur < p->end) {
        parser_report_error(p, p->line_num + 1, "Unexpected character after closing quote");
    }
    p->field_start = p->cur;
}

static inline bool append_current_quoted_segment(cisv_parser *p, const uint8_t *segment_start,
                                                 const uint8_t *segment_end) {
    if (segment_end <= segment_start) return true;
    if (!append_to_quote_buffer(p, segment_start, (size_t)(segment_end - segment_start))) {
        parser_report_error(p, p->line_num + 1, "Quoted field buffer allocation failed");
        return false;
    }
    p->quoted_field_buffered = true;
    return true;
}

static inline void consume_quoted_field(cisv_parser *p) {
    const uint8_t *segment_start = p->cur;

    if (p->quoted_pending_escape) {
        p->quoted_pending_escape = false;
        if (p->cur >= p->end) {
            return;
        }
        if (!append_to_quote_buffer(p, p->cur, 1)) {
            parser_report_error(p, p->line_num + 1, "Quoted field buffer allocation failed");
            p->cur = p->end;
            return;
        }
        p->quoted_field_buffered = true;
        p->cur++;
        segment_start = p->cur;
    }

    if (p->quoted_pending_quote) {
        p->quoted_pending_quote = false;
        p->quoted_pending_escape = false;
        if (p->cur < p->end && *p->cur == (uint8_t)p->quote) {
            if (!append_to_quote_buffer(p, p->cur, 1)) {
                parser_report_error(p, p->line_num + 1, "Quoted field buffer allocation failed");
                p->cur = p->end;
                return;
            }
            p->quoted_field_buffered = true;
            p->cur++;
            segment_start = p->cur;
        } else {
            yield_quoted_field(p);
            p->state = S_NORMAL;
            p->quoted_field_start = NULL;
            p->quoted_field_buffered = false;
            p->quoted_pending_quote = false;
            p->quoted_pending_escape = false;
            consume_post_quoted_delimiter(p);
            return;
        }
    }

    if (p->quoted_field_buffered && p->quote_buffer_pos > 0) {
        segment_start = p->cur;
    }

    while (p->cur < p->end) {
        size_t remaining = (size_t)(p->end - p->cur);
        const uint8_t *quote_ptr = memchr(p->cur, p->quote, remaining);
        const uint8_t *escape_ptr = NULL;
        const uint8_t *special = quote_ptr;

        if (p->escape != '\0') {
            escape_ptr = memchr(p->cur, p->escape, remaining);
            if (!special || (escape_ptr && escape_ptr < special)) {
                special = escape_ptr;
            }
        }

        if (!special) {
            if (p->streaming_mode || p->quoted_field_buffered) {
                if (!append_current_quoted_segment(p, segment_start, p->end)) {
                    p->cur = p->end;
                    return;
                }
            }
            p->cur = p->end;
            return;
        }

        if (p->escape != '\0' && special == escape_ptr) {
            if (!append_current_quoted_segment(p, segment_start, special)) {
                p->cur = p->end;
                return;
            }

            if (special + 1 >= p->end) {
                if (p->streaming_mode) {
                    p->quoted_pending_escape = true;
                    p->quoted_field_buffered = true;
                } else {
                    parser_report_error(p, p->line_num + 1, "Malformed escape at EOF");
                }
                p->cur = p->end;
                return;
            }

            if (!append_to_quote_buffer(p, special + 1, 1)) {
                parser_report_error(p, p->line_num + 1, "Quoted field buffer allocation failed");
                p->cur = p->end;
                return;
            }

            p->quoted_field_buffered = true;
            p->cur = special + 2;
            segment_start = p->cur;
            continue;
        }

        if (special + 1 < p->end && *(special + 1) == p->quote) {
            if (!append_current_quoted_segment(p, segment_start, special)) {
                p->cur = p->end;
                return;
            }
            if (!append_to_quote_buffer(p, special, 1)) {
                parser_report_error(p, p->line_num + 1, "Quoted field buffer allocation failed");
                p->cur = p->end;
                return;
            }
            p->quoted_field_buffered = true;
            p->cur = special + 2;
            segment_start = p->cur;
            continue;
        }

        if (p->streaming_mode && special + 1 >= p->end) {
            if (!append_current_quoted_segment(p, segment_start, special)) {
                p->cur = p->end;
                return;
            }
            p->quoted_pending_quote = true;
            p->quoted_pending_escape = false;
            p->quoted_field_buffered = true;
            p->cur = p->end;
            return;
        }

        if (p->quoted_field_buffered) {
            if (!append_current_quoted_segment(p, segment_start, special)) {
                p->cur = p->end;
                return;
            }
            yield_quoted_field(p);
        } else {
            yield_quoted_span(p, p->quoted_field_start, special);
        }

        p->state = S_NORMAL;
        p->cur = special + 1;
        p->quoted_field_start = NULL;
        p->quoted_field_buffered = false;
        p->quoted_pending_quote = false;
        p->quoted_pending_escape = false;
        consume_post_quoted_delimiter(p);
        return;
    }
}

// Flush buffered streaming data as a complete field at end-of-stream.
// We cannot call yield_field() directly here because it would re-append data
// to stream_buffer while streaming_mode is enabled.
static inline void yield_stream_buffer_field(cisv_parser *p) {
    if (p->stream_buffer_pos == 0) return;

    const uint8_t *start = p->stream_buffer;
    const uint8_t *end = p->stream_buffer + p->stream_buffer_pos;
    bool emit_field = p->fcb && row_in_emit_range(p);

    if (__builtin_expect(p->trim, 0)) {
        while (start < end && __builtin_expect(is_ws(*start), 0)) start++;
        while (start < end && __builtin_expect(is_ws(*(end-1)), 0)) end--;
    }

    if (start < end) {
        if (emit_field) {
            p->fcb(p->user, (const char*)start, end - start);
        }
        p->fields++;
        p->current_row_fields++;
    } else if (!p->skip_empty_lines) {
        if (emit_field) {
            p->fcb(p->user, (const char*)start, 0);
        }
        p->fields++;
        p->current_row_fields++;
    }

    p->stream_buffer_pos = 0;
}

static inline void yield_row(cisv_parser *p) {
    // SECURITY: Protect against line number overflow (2+ billion rows)
    if (__builtin_expect(p->line_num < INT_MAX, 1)) {
        p->line_num++;
    }
    // Drop rows marked as invalid when skip_lines_with_error is enabled.
    if (p->skip_current_row) {
        p->skip_current_row = false;
        p->current_row_fields = 0;
        p->current_row_size = 0;
        p->row_is_comment = false;
        return;
    }

    // Skip comment lines.
    if (p->row_is_comment) {
        p->current_row_fields = 0;
        p->current_row_size = 0;
        p->row_is_comment = false;
        return;
    }

    // Skip empty rows if skip_empty_lines is enabled
    if (p->skip_empty_lines && p->current_row_fields == 0) {
        p->current_row_size = 0;
        p->row_is_comment = false;
        return;
    }
    if (p->rcb && p->line_num >= p->from_line &&
        (!p->to_line || p->line_num <= p->to_line)) {
        p->rcb(p->user);
    }
    p->rows++;
    p->current_row_fields = 0;
    p->current_row_size = 0;
    p->row_is_comment = false;
}

// Forward declare all parse functions
#if defined(__AVX512F__) && defined(__AVX512BW__)
static void parse_avx512(cisv_parser *p);
#endif
#ifdef __AVX2__
static void parse_avx2(cisv_parser *p);
#endif
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
static void parse_neon(cisv_parser *p);
#endif
#if defined(__SSE2__) && !defined(__AVX2__) && !(defined(__AVX512F__) && defined(__AVX512BW__))
static void parse_sse2(cisv_parser *p);
#endif
// Scalar fallback for platforms without SIMD
static void parse_scalar(cisv_parser *p);

static inline void parse_dispatch(cisv_parser *p) {
    if (p->parse_impl) {
        p->parse_impl(p);
        return;
    }

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__AVX512F__) && defined(__AVX512BW__)
    if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
        p->parse_impl = parse_avx512;
        p->parse_impl(p);
        return;
    }
#endif
#ifdef __AVX2__
    if (__builtin_cpu_supports("avx2")) {
        p->parse_impl = parse_avx2;
        p->parse_impl(p);
        return;
    }
#endif
#if defined(__SSE2__) && !defined(__AVX2__) && !(defined(__AVX512F__) && defined(__AVX512BW__))
    p->parse_impl = parse_sse2;
    p->parse_impl(p);
    return;
#endif
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
    p->parse_impl = parse_neon;
    p->parse_impl(p);
    return;
#endif

    p->parse_impl = parse_scalar;
    p->parse_impl(p);
}

#if defined(__AVX512F__) && defined(__AVX512BW__)
// AVX-512 ultra-fast path
// PERF: __attribute__((hot)) tells compiler this is frequently called
__attribute__((hot))
static void parse_avx512(cisv_parser *p) {
    const __m512i delim_v = _mm512_set1_epi8(p->delimiter);
    const __m512i quote_v = _mm512_set1_epi8(p->quote);
    const __m512i nl_v = _mm512_set1_epi8('\n');
    const __m512i cr_v = _mm512_set1_epi8('\r');

    while (p->cur + 64 <= p->end) {
        const uint8_t *chunk_base = p->cur;
        _mm_prefetch(p->cur + PREFETCH_DISTANCE, _MM_HINT_T0);
        _mm_prefetch(p->cur + PREFETCH_DISTANCE + 64, _MM_HINT_T0);

        if (p->state == S_NORMAL) {
            __m512i chunk = _mm512_loadu_si512((__m512i*)p->cur);

            __mmask64 delim_mask = _mm512_cmpeq_epi8_mask(chunk, delim_v);
            __mmask64 quote_mask = _mm512_cmpeq_epi8_mask(chunk, quote_v);
            __mmask64 nl_mask = _mm512_cmpeq_epi8_mask(chunk, nl_v);
            __mmask64 cr_mask = _mm512_cmpeq_epi8_mask(chunk, cr_v);

            __mmask64 special = delim_mask | quote_mask | nl_mask | cr_mask;

            if (!special) {
                p->cur += 64;
                continue;
            }

            while (special) {
                int pos = __builtin_ctzll(special);
                const uint8_t *ptr = p->cur + pos;

                if (delim_mask & (1ULL << pos)) {
                    yield_field(p, p->field_start, ptr);
                    p->field_start = ptr + 1;
                } else if (nl_mask & (1ULL << pos)) {
                    const uint8_t *field_end = ptr;
                    if (field_end > p->field_start && *(field_end - 1) == '\r') {
                        field_end--;
                    }
                    yield_line_end(p, field_end);
                    p->field_start = ptr + 1;
                } else if (cr_mask & (1ULL << pos)) {
                    if (!(ptr + 1 < p->end && *(ptr + 1) == '\n')) {
                        yield_line_end(p, ptr);
                        mark_streaming_split_cr(p, ptr + 1);
                        p->field_start = ptr + 1;
                    }
                } else if (quote_mask & (1ULL << pos)) {
                    if (ptr == p->field_start) {
                        p->state = S_QUOTED;
                        p->cur = ptr + 1;
                        p->quote_buffer_pos = 0;
                        p->quoted_field_start = p->cur;
                        p->quoted_field_buffered = false;
                        p->quoted_pending_quote = false;
                        p->quoted_pending_escape = false;
                        consume_quoted_field(p);
                        break;
                    }
                }

                special &= special - 1;
            }

            if (p->state == S_NORMAL && p->cur == chunk_base) {
                p->cur += 64;
            }
        } else {
            consume_quoted_field(p);
        }
    }

    // Handle remainder
    while (p->cur < p->end) {
        uint8_t c = *p->cur++;

        if (p->state == S_NORMAL) {
            if (c == p->delimiter) {
                yield_field(p, p->field_start, p->cur - 1);
                p->field_start = p->cur;
            } else if (c == '\n') {
                const uint8_t *field_end = p->cur - 1;
                if (field_end > p->field_start && *(field_end - 1) == '\r') {
                    field_end--;
                }
                yield_line_end(p, field_end);
                p->field_start = p->cur;
            } else if (c == '\r') {
                yield_line_end(p, p->cur - 1);
                if (p->cur < p->end && *p->cur == '\n') {
                    p->cur++;
                } else if (p->cur == p->end) {
                    mark_streaming_split_cr(p, p->cur);
                }
                p->field_start = p->cur;
            } else if (c == p->quote && p->cur - 1 == p->field_start) {
                p->state = S_QUOTED;
                p->quote_buffer_pos = 0;
                p->quoted_field_start = p->cur;
                p->quoted_field_buffered = false;
                p->quoted_pending_quote = false;
                p->quoted_pending_escape = false;
                consume_quoted_field(p);
            }
        } else if (p->state == S_QUOTED) {
            p->cur--;
            consume_quoted_field(p);
        }
    }

    if (!p->streaming_mode && p->state == S_NORMAL) {
        if (p->field_start < p->end ||
            (p->field_start == p->end && p->current_row_fields > 0)) {
            yield_field(p, p->field_start, p->end);
        }
    } else if (!p->streaming_mode && p->state == S_QUOTED) {
        // SECURITY: Report unterminated quote at EOF
        parser_report_error(p, p->line_num + 1, "Unterminated quoted field at EOF");
        // Still yield the partial content so data isn't lost
        if (p->quoted_field_buffered && p->quote_buffer_pos > 0) {
            yield_quoted_field(p);
        } else if (p->quoted_field_start && p->quoted_field_start < p->end) {
            yield_quoted_span(p, p->quoted_field_start, p->end);
        }
    }

    if (!p->streaming_mode && p->current_row_fields > 0) {
        yield_row(p);
    }
}
#endif

#ifdef __AVX2__
// AVX2 fast path
// PERF: __attribute__((hot)) tells compiler this is frequently called
__attribute__((hot))
static void parse_avx2(cisv_parser *p) {
    const __m256i delim_v = _mm256_set1_epi8(p->delimiter);
    const __m256i quote_v = _mm256_set1_epi8(p->quote);
    const __m256i nl_v = _mm256_set1_epi8('\n');
    const __m256i cr_v = _mm256_set1_epi8('\r');

    while (p->cur + 32 <= p->end) {
        const uint8_t *chunk_base = p->cur;
        // Multiple prefetch lines for better cache utilization (matches AVX-512 pattern)
        _mm_prefetch(p->cur + PREFETCH_DISTANCE, _MM_HINT_T0);
        _mm_prefetch(p->cur + PREFETCH_DISTANCE + 64, _MM_HINT_T0);
        _mm_prefetch(p->cur + PREFETCH_DISTANCE + 128, _MM_HINT_T0);

        if (p->state == S_NORMAL) {
            __m256i chunk = _mm256_loadu_si256((__m256i*)p->cur);

            __m256i delim_cmp = _mm256_cmpeq_epi8(chunk, delim_v);
            __m256i quote_cmp = _mm256_cmpeq_epi8(chunk, quote_v);
            __m256i nl_cmp = _mm256_cmpeq_epi8(chunk, nl_v);
            __m256i cr_cmp = _mm256_cmpeq_epi8(chunk, cr_v);

            __m256i row_cmp = _mm256_or_si256(nl_cmp, cr_cmp);
            __m256i special = _mm256_or_si256(delim_cmp, _mm256_or_si256(quote_cmp, row_cmp));
            uint32_t mask = _mm256_movemask_epi8(special);

            if (!mask) {
                p->cur += 32;
                continue;
            }

            while (mask) {
                int pos = __builtin_ctz(mask);
                const uint8_t *ptr = p->cur + pos;
                uint8_t c = *ptr;

                if (c == p->delimiter) {
                    yield_field(p, p->field_start, ptr);
                    p->field_start = ptr + 1;
                } else if (c == '\n') {
                    const uint8_t *field_end = ptr;
                    if (field_end > p->field_start && *(field_end - 1) == '\r') {
                        field_end--;
                    }
                    yield_line_end(p, field_end);
                    p->field_start = ptr + 1;
                } else if (c == '\r') {
                    if (!(ptr + 1 < p->end && *(ptr + 1) == '\n')) {
                        yield_line_end(p, ptr);
                        mark_streaming_split_cr(p, ptr + 1);
                        p->field_start = ptr + 1;
                    }
                } else if (c == p->quote) {
                    if (ptr == p->field_start) {
                        p->state = S_QUOTED;
                        p->cur = ptr + 1;
                        p->quote_buffer_pos = 0;
                        p->quoted_field_start = p->cur;
                        p->quoted_field_buffered = false;
                        p->quoted_pending_quote = false;
                        p->quoted_pending_escape = false;
                        consume_quoted_field(p);
                        break;
                    }
                }

                mask &= mask - 1;
            }

            if (p->state == S_NORMAL && p->cur == chunk_base) {
                p->cur += 32;
            }
        } else {
            consume_quoted_field(p);
        }
    }

    // Handle remainder with scalar code
    while (p->cur < p->end) {
        uint8_t c = *p->cur++;

        if (p->state == S_NORMAL) {
            if (c == p->delimiter) {
                yield_field(p, p->field_start, p->cur - 1);
                p->field_start = p->cur;
            } else if (c == '\n') {
                const uint8_t *field_end = p->cur - 1;
                if (field_end > p->field_start && *(field_end - 1) == '\r') {
                    field_end--;
                }
                yield_line_end(p, field_end);
                p->field_start = p->cur;
            } else if (c == '\r') {
                yield_line_end(p, p->cur - 1);
                if (p->cur < p->end && *p->cur == '\n') {
                    p->cur++;
                } else if (p->cur == p->end) {
                    mark_streaming_split_cr(p, p->cur);
                }
                p->field_start = p->cur;
            } else if (c == p->quote && p->cur - 1 == p->field_start) {
                p->state = S_QUOTED;
                p->quote_buffer_pos = 0;
                p->quoted_field_start = p->cur;
                p->quoted_field_buffered = false;
                p->quoted_pending_quote = false;
                p->quoted_pending_escape = false;
                consume_quoted_field(p);
            }
        } else if (p->state == S_QUOTED) {
            p->cur--;
            consume_quoted_field(p);
        }
    }

    if (!p->streaming_mode && p->state == S_NORMAL) {
        if (p->field_start < p->end ||
            (p->field_start == p->end && p->current_row_fields > 0)) {
            yield_field(p, p->field_start, p->end);
        }
    } else if (!p->streaming_mode && p->state == S_QUOTED) {
        // SECURITY: Report unterminated quote at EOF
        parser_report_error(p, p->line_num + 1, "Unterminated quoted field at EOF");
        // Still yield the partial content so data isn't lost
        if (p->quoted_field_buffered && p->quote_buffer_pos > 0) {
            yield_quoted_field(p);
        } else if (p->quoted_field_start && p->quoted_field_start < p->end) {
            yield_quoted_span(p, p->quoted_field_start, p->end);
        }
    }

    if (!p->streaming_mode && p->current_row_fields > 0) {
        yield_row(p);
    }
}
#endif

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
// ARM NEON fast path for Apple Silicon, AWS Graviton, Raspberry Pi
// PERF: __attribute__((hot)) tells compiler this is frequently called
__attribute__((hot))
static void parse_neon(cisv_parser *p) {
    const uint8x16_t delim_v = vdupq_n_u8(p->delimiter);
    const uint8x16_t quote_v = vdupq_n_u8(p->quote);
    const uint8x16_t nl_v = vdupq_n_u8('\n');
    const uint8x16_t cr_v = vdupq_n_u8('\r');

    while (p->cur + 16 <= p->end) {
        const uint8_t *chunk_base = p->cur;
        // Prefetch future data
        __builtin_prefetch(p->cur + PREFETCH_DISTANCE, 0, 1);
        __builtin_prefetch(p->cur + PREFETCH_DISTANCE + 64, 0, 1);

        if (p->state == S_NORMAL) {
            uint8x16_t chunk = vld1q_u8(p->cur);

            // Compare for special characters
            uint8x16_t delim_cmp = vceqq_u8(chunk, delim_v);
            uint8x16_t quote_cmp = vceqq_u8(chunk, quote_v);
            uint8x16_t nl_cmp = vceqq_u8(chunk, nl_v);
            uint8x16_t cr_cmp = vceqq_u8(chunk, cr_v);

            // Combine masks
            uint8x16_t row_cmp = vorrq_u8(nl_cmp, cr_cmp);
            uint8x16_t special = vorrq_u8(delim_cmp, vorrq_u8(quote_cmp, row_cmp));

            // Check if any special chars found (using horizontal max)
            uint8x8_t special_low = vget_low_u8(special);
            uint8x8_t special_high = vget_high_u8(special);
            uint8x8_t special_max = vorr_u8(special_low, special_high);
            uint64_t has_special;
            vst1_u8((uint8_t*)&has_special, special_max);

            if (!has_special) {
                p->cur += 16;
                continue;
            }

            // Process bytes until we find a special character
            while (p->cur + 16 <= p->end) {
                uint8_t c = *p->cur;

                if (c == p->delimiter) {
                    yield_field(p, p->field_start, p->cur);
                    p->cur++;
                    p->field_start = p->cur;
                } else if (c == '\n') {
                    const uint8_t *field_end = p->cur;
                    if (field_end > p->field_start && *(field_end - 1) == '\r') {
                        field_end--;
                    }
                    yield_line_end(p, field_end);
                    p->cur++;
                    p->field_start = p->cur;
                } else if (c == '\r') {
                    yield_line_end(p, p->cur);
                    p->cur++;
                    if (p->cur < p->end && *p->cur == '\n') {
                        p->cur++;
                    } else if (p->cur == p->end) {
                        mark_streaming_split_cr(p, p->cur);
                    }
                    p->field_start = p->cur;
                } else if (c == p->quote && p->cur == p->field_start) {
                    p->state = S_QUOTED;
                    p->cur++;
                    p->quote_buffer_pos = 0;
                    p->quoted_field_start = p->cur;
                    p->quoted_field_buffered = false;
                    p->quoted_pending_quote = false;
                    p->quoted_pending_escape = false;
                    consume_quoted_field(p);
                    break;
                } else {
                    p->cur++;
                    if (p->cur >= p->field_start + 16) break;
                }
            }
            if (p->state == S_NORMAL && p->cur == chunk_base) {
                p->cur += 16;
            }
        } else {
            consume_quoted_field(p);
        }
    }

    // Handle remainder with scalar code
    while (p->cur < p->end) {
        uint8_t c = *p->cur++;

        if (p->state == S_NORMAL) {
            if (c == p->delimiter) {
                yield_field(p, p->field_start, p->cur - 1);
                p->field_start = p->cur;
            } else if (c == '\n') {
                const uint8_t *field_end = p->cur - 1;
                if (field_end > p->field_start && *(field_end - 1) == '\r') {
                    field_end--;
                }
                yield_line_end(p, field_end);
                p->field_start = p->cur;
            } else if (c == '\r') {
                yield_line_end(p, p->cur - 1);
                if (p->cur < p->end && *p->cur == '\n') {
                    p->cur++;
                } else if (p->cur == p->end) {
                    mark_streaming_split_cr(p, p->cur);
                }
                p->field_start = p->cur;
            } else if (c == p->quote && p->cur - 1 == p->field_start) {
                p->state = S_QUOTED;
                p->quote_buffer_pos = 0;
                p->quoted_field_start = p->cur;
                p->quoted_field_buffered = false;
                p->quoted_pending_quote = false;
                p->quoted_pending_escape = false;
                consume_quoted_field(p);
            }
        } else if (p->state == S_QUOTED) {
            p->cur--;
            consume_quoted_field(p);
        }
    }

    if (!p->streaming_mode && p->state == S_NORMAL) {
        if (p->field_start < p->end ||
            (p->field_start == p->end && p->current_row_fields > 0)) {
            yield_field(p, p->field_start, p->end);
        }
    } else if (!p->streaming_mode && p->state == S_QUOTED) {
        parser_report_error(p, p->line_num + 1, "Unterminated quoted field at EOF");
        if (p->quoted_field_buffered && p->quote_buffer_pos > 0) {
            yield_quoted_field(p);
        } else if (p->quoted_field_start && p->quoted_field_start < p->end) {
            yield_quoted_span(p, p->quoted_field_start, p->end);
        }
    }

    if (!p->streaming_mode && p->current_row_fields > 0) {
        yield_row(p);
    }
}
#endif

#if defined(__SSE2__) && !defined(__AVX2__) && !(defined(__AVX512F__) && defined(__AVX512BW__))
// SSE2 fast path for older x86-64 machines (16 bytes at a time)
// PERF: __attribute__((hot)) tells compiler this is frequently called
__attribute__((hot))
static void parse_sse2(cisv_parser *p) {
    const __m128i delim_v = _mm_set1_epi8(p->delimiter);
    const __m128i quote_v = _mm_set1_epi8(p->quote);
    const __m128i nl_v = _mm_set1_epi8('\n');
    const __m128i cr_v = _mm_set1_epi8('\r');

    while (p->cur + 16 <= p->end) {
        const uint8_t *chunk_base = p->cur;
        // Prefetch future data
        _mm_prefetch((const char*)(p->cur + PREFETCH_DISTANCE), _MM_HINT_T0);
        _mm_prefetch((const char*)(p->cur + PREFETCH_DISTANCE + 64), _MM_HINT_T0);

        if (p->state == S_NORMAL) {
            __m128i chunk = _mm_loadu_si128((const __m128i*)p->cur);

            __m128i delim_cmp = _mm_cmpeq_epi8(chunk, delim_v);
            __m128i quote_cmp = _mm_cmpeq_epi8(chunk, quote_v);
            __m128i nl_cmp = _mm_cmpeq_epi8(chunk, nl_v);
            __m128i cr_cmp = _mm_cmpeq_epi8(chunk, cr_v);

            __m128i row_cmp = _mm_or_si128(nl_cmp, cr_cmp);
            __m128i special = _mm_or_si128(delim_cmp, _mm_or_si128(quote_cmp, row_cmp));
            int mask = _mm_movemask_epi8(special);

            if (!mask) {
                p->cur += 16;
                continue;
            }

            while (mask) {
                int pos = __builtin_ctz(mask);
                const uint8_t *ptr = p->cur + pos;
                uint8_t c = *ptr;

                if (c == p->delimiter) {
                    yield_field(p, p->field_start, ptr);
                    p->field_start = ptr + 1;
                } else if (c == '\n') {
                    const uint8_t *field_end = ptr;
                    if (field_end > p->field_start && *(field_end - 1) == '\r') {
                        field_end--;
                    }
                    yield_line_end(p, field_end);
                    p->field_start = ptr + 1;
                } else if (c == '\r') {
                    if (!(ptr + 1 < p->end && *(ptr + 1) == '\n')) {
                        yield_line_end(p, ptr);
                        mark_streaming_split_cr(p, ptr + 1);
                        p->field_start = ptr + 1;
                    }
                } else if (c == p->quote) {
                    if (ptr == p->field_start) {
                        p->state = S_QUOTED;
                        p->cur = ptr + 1;
                        p->quote_buffer_pos = 0;
                        p->quoted_field_start = p->cur;
                        p->quoted_field_buffered = false;
                        p->quoted_pending_quote = false;
                        p->quoted_pending_escape = false;
                        consume_quoted_field(p);
                        break;
                    }
                }

                mask &= mask - 1;
            }

            if (p->state == S_NORMAL && p->cur == chunk_base) {
                p->cur += 16;
            }
        } else {
            consume_quoted_field(p);
        }
    }

    // Handle remainder with scalar code
    while (p->cur < p->end) {
        uint8_t c = *p->cur++;

        if (p->state == S_NORMAL) {
            if (c == p->delimiter) {
                yield_field(p, p->field_start, p->cur - 1);
                p->field_start = p->cur;
            } else if (c == '\n') {
                const uint8_t *field_end = p->cur - 1;
                if (field_end > p->field_start && *(field_end - 1) == '\r') {
                    field_end--;
                }
                yield_line_end(p, field_end);
                p->field_start = p->cur;
            } else if (c == '\r') {
                yield_line_end(p, p->cur - 1);
                if (p->cur < p->end && *p->cur == '\n') {
                    p->cur++;
                } else if (p->cur == p->end) {
                    mark_streaming_split_cr(p, p->cur);
                }
                p->field_start = p->cur;
            } else if (c == p->quote && p->cur - 1 == p->field_start) {
                p->state = S_QUOTED;
                p->quote_buffer_pos = 0;
                p->quoted_field_start = p->cur;
                p->quoted_field_buffered = false;
                p->quoted_pending_quote = false;
                p->quoted_pending_escape = false;
                consume_quoted_field(p);
            }
        } else if (p->state == S_QUOTED) {
            p->cur--;
            consume_quoted_field(p);
        }
    }

    if (!p->streaming_mode && p->state == S_NORMAL) {
        if (p->field_start < p->end ||
            (p->field_start == p->end && p->current_row_fields > 0)) {
            yield_field(p, p->field_start, p->end);
        }
    } else if (!p->streaming_mode && p->state == S_QUOTED) {
        parser_report_error(p, p->line_num + 1, "Unterminated quoted field at EOF");
        if (p->quoted_field_buffered && p->quote_buffer_pos > 0) {
            yield_quoted_field(p);
        } else if (p->quoted_field_start && p->quoted_field_start < p->end) {
            yield_quoted_span(p, p->quoted_field_start, p->end);
        }
    }

    if (!p->streaming_mode && p->current_row_fields > 0) {
        yield_row(p);
    }
}
#endif

// Scalar fallback using SWAR (SIMD Within A Register) - 1BRC technique
// Processes 8 bytes at a time without SIMD instructions (20-40% faster)
// Always compiled as fallback for all platforms
__attribute__((hot, unused))
static void parse_scalar(cisv_parser *p) {
    while (p->cur + 8 <= p->end) {
        if (p->state == S_NORMAL) {
            const uint8_t *chunk_base = p->cur;
            // Load 8 bytes using memcpy (compiler optimizes this)
            uint64_t word;
            memcpy(&word, p->cur, sizeof(word));

            // SWAR: Check all 8 bytes in parallel
            uint64_t special = swar_has_special(word, p->delimiter, p->quote);

            if (!special) {
                // Fast path: no special chars in 8-byte chunk
                p->cur += 8;
                continue;
            }

            // Process bytes until we find a special character
            while (p->cur < p->end && p->cur < p->field_start + 8) {
                uint8_t c = *p->cur;

                if (c == p->delimiter) {
                    yield_field(p, p->field_start, p->cur);
                    p->cur++;
                    p->field_start = p->cur;
                } else if (c == '\n') {
                    const uint8_t *field_end = p->cur;
                    if (field_end > p->field_start && *(field_end - 1) == '\r') {
                        field_end--;
                    }
                    yield_line_end(p, field_end);
                    p->cur++;
                    p->field_start = p->cur;
                } else if (c == '\r') {
                    yield_line_end(p, p->cur);
                    p->cur++;
                    if (p->cur < p->end && *p->cur == '\n') {
                        p->cur++;
                    } else if (p->cur == p->end) {
                        mark_streaming_split_cr(p, p->cur);
                    }
                    p->field_start = p->cur;
                } else if (c == p->quote && p->cur == p->field_start) {
                    p->state = S_QUOTED;
                    p->cur++;
                    p->quote_buffer_pos = 0;
                    p->quoted_field_start = p->cur;
                    p->quoted_field_buffered = false;
                    p->quoted_pending_quote = false;
                    p->quoted_pending_escape = false;
                    consume_quoted_field(p);
                    break;
                } else {
                    p->cur++;
                }
            }
            if (p->state == S_NORMAL && p->cur == chunk_base) {
                p->cur += 8;
            }
        } else {
            consume_quoted_field(p);
        }
    }

    // Handle remainder
    while (p->cur < p->end) {
        uint8_t c = *p->cur++;

        if (p->state == S_NORMAL) {
            if (c == p->delimiter) {
                yield_field(p, p->field_start, p->cur - 1);
                p->field_start = p->cur;
            } else if (c == '\n') {
                const uint8_t *field_end = p->cur - 1;
                if (field_end > p->field_start && *(field_end - 1) == '\r') {
                    field_end--;
                }
                yield_line_end(p, field_end);
                p->field_start = p->cur;
            } else if (c == '\r') {
                yield_line_end(p, p->cur - 1);
                if (p->cur < p->end && *p->cur == '\n') {
                    p->cur++;
                } else if (p->cur == p->end) {
                    mark_streaming_split_cr(p, p->cur);
                }
                p->field_start = p->cur;
            } else if (c == p->quote && p->cur - 1 == p->field_start) {
                p->state = S_QUOTED;
                p->quote_buffer_pos = 0;
                p->quoted_field_start = p->cur;
                p->quoted_field_buffered = false;
                p->quoted_pending_quote = false;
                p->quoted_pending_escape = false;
                consume_quoted_field(p);
            }
        } else if (p->state == S_QUOTED) {
            p->cur--;
            consume_quoted_field(p);
        }
    }

    if (!p->streaming_mode && p->state == S_NORMAL) {
        if (p->field_start < p->end ||
            (p->field_start == p->end && p->current_row_fields > 0)) {
            yield_field(p, p->field_start, p->end);
        }
    } else if (!p->streaming_mode && p->state == S_QUOTED) {
        // SECURITY: Report unterminated quote at EOF
        parser_report_error(p, p->line_num + 1, "Unterminated quoted field at EOF");
        // Still yield the partial content so data isn't lost
        if (p->quoted_field_buffered && p->quote_buffer_pos > 0) {
            yield_quoted_field(p);
        } else if (p->quoted_field_start && p->quoted_field_start < p->end) {
            yield_quoted_span(p, p->quoted_field_start, p->end);
        }
    }

    if (!p->streaming_mode && p->current_row_fields > 0) {
        yield_row(p);
    }
}

cisv_parser *cisv_parser_create_with_config(const cisv_config *config) {
    if (!cisv_config_is_valid(config)) return NULL;

    cisv_parser *p = calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->delimiter = config->delimiter;
    p->quote = config->quote;
    p->escape = config->escape;
    p->trim = config->trim;
    p->skip_empty_lines = config->skip_empty_lines;
    p->comment = config->comment;
    p->from_line = config->from_line > 0 ? config->from_line : 1;
    p->to_line = config->to_line;
    p->memory_budget = cisv_runtime_memory_budget();
    size_t env_row_limit = cisv_runtime_max_row_size();
    if (config->max_row_size > 0) {
        p->max_row_size = config->max_row_size;
    } else if (env_row_limit > 0) {
        p->max_row_size = env_row_limit;
    } else {
        p->max_row_size = cisv_adaptive_row_limit(p->memory_budget);
    }
    p->skip_lines_with_error = config->skip_lines_with_error;
    p->has_row_controls = (p->max_row_size > 0 || config->comment != 0 || config->skip_lines_with_error);
    p->relaxed = config->relaxed;
    p->had_error = false;

    p->fcb = config->field_cb;
    p->rcb = config->row_cb;
    p->ecb = config->error_cb;
    p->user = config->user;

    p->fd = -1;
    p->line_num = 0;
    p->had_error = false;
    p->current_row_fields = 0;
    p->current_row_size = 0;
    p->skip_current_row = false;
    p->row_is_comment = false;
    p->parse_impl = NULL;

    // Start at 64KB to avoid early reallocations and match MIN_BUFFER_INCREMENT.
    // These buffers are grown with realloc(), so use malloc-family allocation.
    p->quote_buffer_size = MIN_BUFFER_INCREMENT;
    p->quote_buffer = malloc(p->quote_buffer_size);
    if (!p->quote_buffer) {
        free(p);
        return NULL;
    }
    p->quote_buffer_pos = 0;
    p->quoted_pending_quote = false;
    p->quoted_pending_escape = false;
    p->skip_leading_lf = false;

    // Allocate stream buffer for streaming mode.
    p->stream_buffer_size = MIN_BUFFER_INCREMENT;
    p->stream_buffer = malloc(p->stream_buffer_size);
    if (!p->stream_buffer) {
        free(p->quote_buffer);
        free(p);
        return NULL;
    }
    p->stream_buffer_pos = 0;
    p->streaming_mode = false;

    return p;
}

cisv_parser *cisv_parser_create(cisv_field_cb fcb, cisv_row_cb rcb, void *user) {
    cisv_config config;
    cisv_config_init(&config);
    config.field_cb = fcb;
    config.row_cb = rcb;
    config.user = user;
    return cisv_parser_create_with_config(&config);
}

void cisv_parser_destroy(cisv_parser *p) {
    if (!p) return;

    if (p->base) munmap(p->base, p->size);
    if (p->fd >= 0) close(p->fd);
    if (p->quote_buffer) free(p->quote_buffer);
    if (p->stream_buffer) free(p->stream_buffer);
    free(p);
}

int cisv_parser_parse_file(cisv_parser *p, const char *path) {
    if (!p || !path) return -EINVAL;

    // If this parser instance was used previously, release old resources
    // before opening a new file.
    if (p->base) {
        munmap(p->base, p->size);
        p->base = NULL;
        p->size = 0;
    }
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }

    p->fd = open(path, O_RDONLY);
    if (p->fd < 0) return -errno;

    struct stat st;
    if (fstat(p->fd, &st) < 0) {
        close(p->fd);
        p->fd = -1;
        return -errno;
    }

    if (st.st_size == 0) {
        close(p->fd);
        p->fd = -1;
        return 0;
    }

    p->size = st.st_size;

    int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
    flags |= MAP_POPULATE;
#endif

    p->base = (uint8_t*)mmap(NULL, p->size, PROT_READ, flags, p->fd, 0);

    if (p->base == MAP_FAILED) {
        close(p->fd);
        p->fd = -1;
        return -errno;
    }

    madvise(p->base, p->size, MADV_SEQUENTIAL | MADV_WILLNEED);

    p->cur = p->base;
    p->end = p->base + p->size;
    p->field_start = p->cur;
    p->state = S_NORMAL;
    p->line_num = 0;
    p->current_row_fields = 0;
    p->quote_buffer_pos = 0;
    p->quoted_field_start = NULL;
    p->quoted_field_buffered = false;
    p->quoted_pending_quote = false;
    p->quoted_pending_escape = false;
    p->skip_leading_lf = false;
    p->stream_buffer_pos = 0;
    p->streaming_mode = false;
    p->current_row_size = 0;
    p->skip_current_row = false;
    p->row_is_comment = false;

    // Runtime ISA dispatch with scalar fallback.
    parse_dispatch(p);

    int result = p->had_error ? -EINVAL : 0;

    // Release file resources immediately after parse to avoid descriptor
    // retention when parser objects are reused.
    munmap(p->base, p->size);
    p->base = NULL;
    p->size = 0;
    close(p->fd);
    p->fd = -1;

    return result;
}

typedef struct {
    size_t rows;
} cisv_row_counter_t;

static inline bool ends_with_row_terminator(const uint8_t *data, size_t size) {
    return size > 0 && (data[size - 1] == '\n' || data[size - 1] == '\r');
}

static size_t count_newlines_fast(const uint8_t *data, size_t size, uint8_t quote_char, bool *needs_fallback) {
    size_t nl_count = 0;
    size_t cr_count = 0;
    size_t crlf_count = 0;
    size_t i = 0;
    bool previous_cr = false;
    bool saw_cr = false;

    *needs_fallback = false;

#if defined(__AVX512F__) && defined(__AVX512BW__)
    const __m512i nl_v = _mm512_set1_epi8('\n');
    const __m512i cr_v = _mm512_set1_epi8('\r');
    const __m512i quote_v = _mm512_set1_epi8((char)quote_char);

    while (i + 64 <= size) {
        __m512i chunk = _mm512_loadu_si512((const void *)(data + i));
        uint64_t quote_mask = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, quote_v);
        if (quote_mask) {
            *needs_fallback = true;
            return 0;
        }
        uint64_t cr_mask = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, cr_v);
        uint64_t nl_mask = (uint64_t)_mm512_cmpeq_epi8_mask(chunk, nl_v);
        nl_count += (size_t)__builtin_popcountll(nl_mask);
        if (cr_mask || previous_cr) {
            saw_cr = saw_cr || cr_mask != 0;
            cr_count += (size_t)__builtin_popcountll(cr_mask);
            crlf_count += (size_t)__builtin_popcountll((cr_mask << 1) & nl_mask);
            if (previous_cr && (nl_mask & 1ULL)) crlf_count++;
            previous_cr = (cr_mask & (1ULL << 63)) != 0;
        }
        i += 64;
    }
#elif defined(__AVX2__)
    const __m256i nl_v = _mm256_set1_epi8('\n');
    const __m256i cr_v = _mm256_set1_epi8('\r');
    const __m256i quote_v = _mm256_set1_epi8((char)quote_char);

    while (i + 32 <= size) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(data + i));
        __m256i quote_cmp = _mm256_cmpeq_epi8(chunk, quote_v);
        if (_mm256_movemask_epi8(quote_cmp) != 0) {
            *needs_fallback = true;
            return 0;
        }
        __m256i cr_cmp = _mm256_cmpeq_epi8(chunk, cr_v);
        __m256i nl_cmp = _mm256_cmpeq_epi8(chunk, nl_v);
        uint32_t cr_mask = (uint32_t)_mm256_movemask_epi8(cr_cmp);
        uint32_t nl_mask = (uint32_t)_mm256_movemask_epi8(nl_cmp);
        nl_count += (size_t)__builtin_popcount(nl_mask);
        if (cr_mask || previous_cr) {
            saw_cr = saw_cr || cr_mask != 0;
            cr_count += (size_t)__builtin_popcount(cr_mask);
            crlf_count += (size_t)__builtin_popcount((cr_mask << 1) & nl_mask);
            if (previous_cr && (nl_mask & 1U)) crlf_count++;
            previous_cr = (cr_mask & (1U << 31)) != 0;
        }
        i += 32;
    }
#elif defined(__SSE2__)
    const __m128i nl_v = _mm_set1_epi8('\n');
    const __m128i cr_v = _mm_set1_epi8('\r');
    const __m128i quote_v = _mm_set1_epi8((char)quote_char);

    while (i + 16 <= size) {
        __m128i chunk = _mm_loadu_si128((const __m128i *)(data + i));
        __m128i quote_cmp = _mm_cmpeq_epi8(chunk, quote_v);
        if (_mm_movemask_epi8(quote_cmp) != 0) {
            *needs_fallback = true;
            return 0;
        }
        __m128i cr_cmp = _mm_cmpeq_epi8(chunk, cr_v);
        __m128i nl_cmp = _mm_cmpeq_epi8(chunk, nl_v);
        uint32_t cr_mask = (uint32_t)_mm_movemask_epi8(cr_cmp);
        uint32_t nl_mask = (uint32_t)_mm_movemask_epi8(nl_cmp);
        nl_count += (size_t)__builtin_popcount(nl_mask);
        if (cr_mask || previous_cr) {
            saw_cr = saw_cr || cr_mask != 0;
            cr_count += (size_t)__builtin_popcount(cr_mask);
            crlf_count += (size_t)__builtin_popcount((cr_mask << 1) & nl_mask);
            if (previous_cr && (nl_mask & 1U)) crlf_count++;
            previous_cr = (cr_mask & (1U << 15)) != 0;
        }
        i += 16;
    }
#endif

    while (i + 16 <= size) {
        uint64_t word0;
        uint64_t word1;
        memcpy(&word0, data + i, sizeof(word0));
        memcpy(&word1, data + i + 8, sizeof(word1));

        uint64_t quote_mask0 = swar_has_byte(word0, quote_char);
        uint64_t quote_mask1 = swar_has_byte(word1, quote_char);
        if (quote_mask0 | quote_mask1) {
            *needs_fallback = true;
            return 0;
        }

        uint64_t cr_mask0 = swar_has_byte(word0, '\r');
        uint64_t nl_mask0 = swar_has_byte(word0, '\n');
        nl_count += (size_t)__builtin_popcountll(nl_mask0);
        if (cr_mask0 || previous_cr) {
            saw_cr = saw_cr || cr_mask0 != 0;
            cr_count += (size_t)__builtin_popcountll(cr_mask0);
            crlf_count += (size_t)__builtin_popcountll((cr_mask0 << 8) & nl_mask0);
            if (previous_cr && (nl_mask0 & 0x80ULL)) crlf_count++;
            previous_cr = (cr_mask0 & 0x8000000000000000ULL) != 0;
        }

        uint64_t cr_mask1 = swar_has_byte(word1, '\r');
        uint64_t nl_mask1 = swar_has_byte(word1, '\n');
        nl_count += (size_t)__builtin_popcountll(nl_mask1);
        if (cr_mask1 || previous_cr) {
            saw_cr = saw_cr || cr_mask1 != 0;
            cr_count += (size_t)__builtin_popcountll(cr_mask1);
            crlf_count += (size_t)__builtin_popcountll((cr_mask1 << 8) & nl_mask1);
            if (previous_cr && (nl_mask1 & 0x80ULL)) crlf_count++;
            previous_cr = (cr_mask1 & 0x8000000000000000ULL) != 0;
        }
        i += 16;
    }

    for (; i < size; i++) {
        uint8_t c = data[i];
        if (c == quote_char) {
            *needs_fallback = true;
            return 0;
        }
        if (c == '\r') {
            cr_count++;
            saw_cr = true;
            previous_cr = true;
        } else if (c == '\n') {
            nl_count++;
            if (previous_cr) crlf_count++;
            previous_cr = false;
        } else {
            previous_cr = false;
        }
    }

    if (!saw_cr) {
        return nl_count + !ends_with_row_terminator(data, size);
    }
    return cr_count + nl_count - crlf_count + !ends_with_row_terminator(data, size);
}

static size_t count_rows_quote_aware(const uint8_t *data, size_t size,
                                     uint8_t delimiter, uint8_t quote_char,
                                     uint8_t escape_char, bool *ok) {
    size_t rows = 0;
    bool in_quote = false;
    bool at_field_start = true;
    bool after_quote = false;

    *ok = true;

    for (size_t i = 0; i < size; i++) {
        uint8_t c = data[i];

        if (in_quote) {
            if (escape_char != '\0' && c == escape_char) {
                if (i + 1 >= size) {
                    *ok = false;
                    return 0;
                }
                i++;
            } else if (c == quote_char) {
                if (i + 1 < size && data[i + 1] == quote_char) {
                    i++;
                } else {
                    in_quote = false;
                    after_quote = true;
                }
            }
            continue;
        }

        if (after_quote) {
            if (c == ' ' || c == '\t') {
                continue;
            }
            if (c == delimiter) {
                after_quote = false;
                at_field_start = true;
                continue;
            }
            if (c == '\n') {
                rows++;
                after_quote = false;
                at_field_start = true;
                continue;
            }
            if (c == '\r') {
                rows++;
                if (i + 1 < size && data[i + 1] == '\n') {
                    i++;
                }
                after_quote = false;
                at_field_start = true;
                continue;
            }

            *ok = false;
            return 0;
        }

        if (c == delimiter) {
            at_field_start = true;
        } else if (c == '\n') {
            rows++;
            at_field_start = true;
        } else if (c == '\r') {
            rows++;
            if (i + 1 < size && data[i + 1] == '\n') {
                i++;
            }
            at_field_start = true;
        } else if (c == quote_char && at_field_start) {
            in_quote = true;
            at_field_start = false;
        } else {
            at_field_start = false;
        }
    }

    if (in_quote) {
        *ok = false;
        return 0;
    }

    return rows + !ends_with_row_terminator(data, size);
}

typedef struct {
    size_t rows;
    size_t line_num;
    bool row_has_field;
    bool first_field_done;
    bool row_is_comment;
    bool trim;
    bool skip_empty_lines;
    uint8_t comment;
    size_t from_line;
    size_t to_line;
} cisv_semantic_counter_t;

static inline void count_finish_field(cisv_semantic_counter_t *counter,
                                      const uint8_t *data, size_t start,
                                      size_t end, bool quoted) {
    if (!counter->first_field_done) {
        if (!quoted && counter->comment != '\0') {
            while (counter->trim && start < end && is_ws(data[start])) start++;
            while (counter->trim && start < end && is_ws(data[end - 1])) end--;
            counter->row_is_comment = (start < end && data[start] == counter->comment);
        }
        counter->first_field_done = true;
    }
    counter->row_has_field = true;
}

static inline void count_finish_row(cisv_semantic_counter_t *counter, bool empty_physical_row) {
    counter->line_num++;
    if (!counter->row_is_comment &&
        !(counter->skip_empty_lines && empty_physical_row) &&
        counter->line_num >= counter->from_line &&
        (counter->to_line == 0 || counter->line_num <= counter->to_line)) {
        counter->rows++;
    }

    counter->row_has_field = false;
    counter->first_field_done = false;
    counter->row_is_comment = false;
}

static size_t count_rows_semantic(const uint8_t *data, size_t size,
                                  const cisv_config *config, bool *ok) {
    cisv_semantic_counter_t counter = {
        .trim = config->trim,
        .skip_empty_lines = config->skip_empty_lines,
        .comment = (uint8_t)config->comment,
        .from_line = config->from_line > 0 ? (size_t)config->from_line : 1,
        .to_line = config->to_line > 0 ? (size_t)config->to_line : 0,
    };
    uint8_t delimiter = (uint8_t)config->delimiter;
    uint8_t quote_char = (uint8_t)config->quote;
    uint8_t escape_char = (uint8_t)config->escape;
    bool in_quote = false;
    bool at_field_start = true;
    bool after_quote = false;
    bool current_field_quoted = false;
    size_t field_start = 0;

    *ok = true;

    for (size_t i = 0; i < size; i++) {
        uint8_t c = data[i];

        if (in_quote) {
            if (escape_char != '\0' && c == escape_char) {
                if (i + 1 >= size) {
                    *ok = false;
                    return 0;
                }
                i++;
            } else if (c == quote_char) {
                if (i + 1 < size && data[i + 1] == quote_char) {
                    i++;
                } else {
                    count_finish_field(&counter, data, field_start, i, true);
                    in_quote = false;
                    after_quote = true;
                }
            }
            continue;
        }

        if (after_quote) {
            if (c == ' ' || c == '\t') {
                continue;
            }
            if (c == delimiter) {
                after_quote = false;
                at_field_start = true;
                current_field_quoted = false;
                field_start = i + 1;
                continue;
            }
            if (c == '\n') {
                count_finish_row(&counter, false);
                after_quote = false;
                at_field_start = true;
                current_field_quoted = false;
                field_start = i + 1;
                continue;
            }
            if (c == '\r') {
                count_finish_row(&counter, false);
                if (i + 1 < size && data[i + 1] == '\n') {
                    i++;
                }
                after_quote = false;
                at_field_start = true;
                current_field_quoted = false;
                field_start = i + 1;
                continue;
            }

            *ok = false;
            return 0;
        }

        if (c == delimiter) {
            count_finish_field(&counter, data, field_start, i, current_field_quoted);
            at_field_start = true;
            current_field_quoted = false;
            field_start = i + 1;
        } else if (c == '\n') {
            size_t field_end = i;
            if (field_end > field_start && data[field_end - 1] == '\r') {
                field_end--;
            }
            bool empty_physical_row = !counter.row_has_field && field_start == field_end;
            if (!(config->skip_empty_lines && empty_physical_row)) {
                count_finish_field(&counter, data, field_start, field_end, current_field_quoted);
            }
            count_finish_row(&counter, empty_physical_row);
            at_field_start = true;
            current_field_quoted = false;
            field_start = i + 1;
        } else if (c == '\r') {
            size_t field_end = i;
            bool empty_physical_row = !counter.row_has_field && field_start == field_end;
            if (!(config->skip_empty_lines && empty_physical_row)) {
                count_finish_field(&counter, data, field_start, field_end, current_field_quoted);
            }
            if (i + 1 < size && data[i + 1] == '\n') {
                i++;
            }
            count_finish_row(&counter, empty_physical_row);
            at_field_start = true;
            current_field_quoted = false;
            field_start = i + 1;
        } else if (c == quote_char && at_field_start) {
            in_quote = true;
            at_field_start = false;
            current_field_quoted = true;
            field_start = i + 1;
        } else {
            at_field_start = false;
        }
    }

    if (in_quote) {
        *ok = false;
        return 0;
    }

    if (after_quote) {
        count_finish_row(&counter, false);
    } else if (size > 0 && (field_start < size || counter.row_has_field)) {
        count_finish_field(&counter, data, field_start, size, current_field_quoted);
        count_finish_row(&counter, false);
    }

    return counter.rows;
}

static void row_count_cb(void *user) {
    cisv_row_counter_t *counter = (cisv_row_counter_t *)user;
    counter->rows++;
}

// Quote-aware row counting via the main parser implementation so counting and
// parsing always share the same CSV semantics.
size_t cisv_parser_count_rows(const char *path) {
    cisv_config config;
    cisv_config_init(&config);
    return cisv_parser_count_rows_with_config(path, &config);
}

size_t cisv_parser_count_rows_with_config(const char *path, const cisv_config *config) {
    if (!path) return 0;

    cisv_config effective_config;
    if (config) {
        effective_config = *config;
    } else {
        cisv_config_init(&effective_config);
    }
    if (!cisv_config_is_valid(&effective_config)) return 0;

    bool simple_fast_count_allowed =
        !effective_config.skip_empty_lines &&
        effective_config.comment == 0 &&
        effective_config.from_line <= 1 &&
        effective_config.to_line == 0 &&
        effective_config.max_row_size == 0 &&
        !effective_config.skip_lines_with_error;
    bool semantic_count_allowed =
        effective_config.max_row_size == 0 &&
        !effective_config.skip_lines_with_error;

    int fd = semantic_count_allowed ? open(path, O_RDONLY) : -1;
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
            flags |= MAP_POPULATE;
#endif
            uint8_t *base = (uint8_t *)mmap(NULL, st.st_size, PROT_READ, flags, fd, 0);
            if (base != MAP_FAILED) {
                bool counted = false;
                size_t count = 0;
                if (simple_fast_count_allowed) {
                    bool needs_fallback = false;
                    count = count_newlines_fast(base, (size_t)st.st_size,
                                                (uint8_t)effective_config.quote, &needs_fallback);
                    if (!needs_fallback) {
                        counted = true;
                    } else {
                        count = count_rows_quote_aware(base, (size_t)st.st_size,
                                                       (uint8_t)effective_config.delimiter,
                                                       (uint8_t)effective_config.quote,
                                                       (uint8_t)effective_config.escape,
                                                       &counted);
                    }
                } else {
                    count = count_rows_semantic(base, (size_t)st.st_size,
                                                &effective_config, &counted);
                }
                munmap(base, st.st_size);
                close(fd);
                if (counted) {
                    return count;
                }
                fd = -1;
            }
        }
        if (fd >= 0) {
            close(fd);
        }
    }

    cisv_row_counter_t counter = {0};
    cisv_config count_config;
    count_config = effective_config;

    count_config.field_cb = NULL;
    count_config.row_cb = row_count_cb;
    count_config.error_cb = NULL;
    count_config.user = &counter;

    cisv_parser *parser = cisv_parser_create_with_config(&count_config);
    if (!parser) return 0;

    if (cisv_parser_parse_file(parser, path) < 0) {
        cisv_parser_destroy(parser);
        return 0;
    }

    cisv_parser_destroy(parser);
    return counter.rows;
}

int cisv_parser_write(cisv_parser *p, const uint8_t *chunk, size_t len) {
    if (!p || (!chunk && len > 0)) return -EINVAL;

    if (!p->streaming_mode) {
        p->had_error = false;
    }

    // Enable streaming mode - fields may span chunks
    p->streaming_mode = true;

    p->cur = chunk;
    p->end = chunk + len;
    p->field_start = p->cur;
    if (p->skip_leading_lf && p->cur < p->end) {
        if (*p->cur == '\n') {
            p->cur++;
        }
        p->skip_leading_lf = false;
        p->field_start = p->cur;
    }

    parse_dispatch(p);

    // After parsing, buffer any partial unquoted field for next chunk
    // (quoted fields are already handled by quote_buffer)
    if (p->state == S_NORMAL && p->field_start && p->field_start < p->cur) {
        // We have a partial field - buffer it for the next write() call
        size_t partial_len = p->cur - p->field_start;
        if (partial_len > 0) {
            if (!append_to_stream_buffer(p, p->field_start, partial_len)) {
                return -ENOMEM;
            }
        }
    }

    return p->had_error ? -EINVAL : 0;
}

void cisv_parser_end(cisv_parser *p) {
    if (!p) return;

    if (p->streaming_mode) {
        bool finalized_pending_quote = false;
        if (p->state == S_QUOTED && p->quoted_pending_escape) {
            p->quoted_pending_escape = false;
            parser_report_error(p, p->line_num + 1, "Malformed escape at EOF");
            if (p->quote_buffer_pos > 0) {
                yield_quoted_field(p);
            }
            p->state = S_NORMAL;
            p->quoted_field_start = NULL;
            p->quoted_field_buffered = false;
            finalized_pending_quote = true;
        } else if (p->state == S_QUOTED && p->quoted_pending_quote) {
            p->quoted_pending_quote = false;
            p->quoted_pending_escape = false;
            yield_quoted_field(p);
            p->state = S_NORMAL;
            p->quoted_field_start = NULL;
            p->quoted_field_buffered = false;
            finalized_pending_quote = true;
        }
        if (p->state == S_NORMAL && p->stream_buffer_pos > 0) {
            yield_stream_buffer_field(p);
        } else if (p->state == S_NORMAL && p->current_row_fields > 0 && !finalized_pending_quote) {
            yield_field(p, p->cur, p->cur);
        } else if (p->state == S_QUOTED && p->quote_buffer_pos > 0) {
            parser_report_error(p, p->line_num + 1, "Unterminated quoted field at EOF");
            yield_quoted_field(p);
        }
        if (p->current_row_fields > 0) {
            yield_row(p);
        }
        p->skip_leading_lf = false;
        p->streaming_mode = false;
        return;
    }

    if (p->state == S_NORMAL && p->field_start &&
        (p->field_start < p->cur ||
         (p->field_start == p->cur && p->current_row_fields > 0))) {
        yield_field(p, p->field_start, p->cur);
    } else if (p->state == S_QUOTED && p->quote_buffer_pos > 0) {
        yield_quoted_field(p);
    }
    if (p->current_row_fields > 0) {
        yield_row(p);
    }
}

int cisv_parser_get_line_number(const cisv_parser *p) {
    return p ? p->line_num : 0;
}

// =============================================================================
// Parallel Chunk Processing Implementation (1 Billion Row Challenge technique)
// =============================================================================

cisv_mmap_file_t *cisv_mmap_open(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    if (st.st_size == 0) {
        close(fd);
        errno = EINVAL;
        return NULL;
    }

    int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
    flags |= MAP_POPULATE;
#endif

    uint8_t *data = (uint8_t*)mmap(NULL, st.st_size, PROT_READ, flags, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    // Advise kernel for sequential access
    madvise(data, st.st_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    cisv_mmap_file_t *file = malloc(sizeof(cisv_mmap_file_t));
    if (!file) {
        munmap(data, st.st_size);
        close(fd);
        return NULL;
    }

    file->data = data;
    file->size = st.st_size;
    file->fd = fd;

    return file;
}

void cisv_mmap_close(cisv_mmap_file_t *file) {
    if (!file) return;
    if (file->data) munmap(file->data, file->size);
    if (file->fd >= 0) close(file->fd);
    free(file);
}

static size_t count_chunk_rows_quote_aware(const uint8_t *start,
                                           const uint8_t *end,
                                           char quote_char,
                                           char escape_char) {
    size_t rows = 0;
    bool in_quote = false;

    for (const uint8_t *p = start; p < end; p++) {
        uint8_t c = *p;
        if (in_quote) {
            if (escape_char != '\0' && c == (uint8_t)escape_char) {
                if (p + 1 < end) {
                    p++;
                }
            } else if (c == (uint8_t)quote_char) {
                if (p + 1 < end && *(p + 1) == (uint8_t)quote_char) {
                    p++;
                } else {
                    in_quote = false;
                }
            }
        } else {
            if (c == '\n') {
                rows++;
            } else if (c == '\r') {
                rows++;
                if (p + 1 < end && *(p + 1) == '\n') {
                    p++;
                }
            } else if (c == (uint8_t)quote_char) {
                in_quote = true;
            }
        }
    }

    return rows;
}

static cisv_chunk_t *split_chunks_with_quote(
    const cisv_mmap_file_t *file,
    int num_chunks,
    int *chunk_count,
    char quote_char,
    char escape_char
) {
    if (!file || !file->data || num_chunks <= 0 || !chunk_count) {
        return NULL;
    }

    // Clamp to reasonable chunk count
    if (num_chunks > 256) num_chunks = 256;

    // Calculate approximate chunk size
    size_t chunk_size = file->size / num_chunks;
    if (chunk_size < 4096) {
        // File too small for requested chunks
        num_chunks = 1;
        chunk_size = file->size;
    }

    cisv_chunk_t *chunks = calloc(num_chunks, sizeof(cisv_chunk_t));
    if (!chunks) return NULL;

    const uint8_t *data = file->data;
    const uint8_t *end = file->data + file->size;
    const uint8_t *chunk_start = data;
    int actual_chunks = 0;
    size_t next_target = chunk_size;
    bool in_quote = false;
    const uint8_t *scan = data;

    while (actual_chunks < num_chunks - 1 && scan < end) {
        uint8_t c = *scan;
        if (c == (uint8_t)quote_char) {
            if (in_quote && scan + 1 < end && *(scan + 1) == (uint8_t)quote_char) {
                scan += 2;
                continue;
            }
            in_quote = !in_quote;
        } else if (in_quote && escape_char != '\0' && c == (uint8_t)escape_char) {
            if (scan + 1 < end) {
                scan += 2;
                continue;
            }
        } else if (!in_quote && (c == '\n' || c == '\r')) {
            const uint8_t *row_end = scan + 1;
            if (c == '\r' && row_end < end && *row_end == '\n') {
                row_end++;
            }
            if ((size_t)(row_end - data) >= next_target) {
                const uint8_t *target_end = row_end;
                size_t row_count = count_chunk_rows_quote_aware(chunk_start, target_end,
                                                                quote_char, escape_char);

                chunks[actual_chunks].start = chunk_start;
                chunks[actual_chunks].end = target_end;
                chunks[actual_chunks].row_count = row_count;
                chunks[actual_chunks].chunk_index = actual_chunks;

                chunk_start = target_end;
                actual_chunks++;
                next_target += chunk_size;
            }
            scan = row_end;
            continue;
        }

        scan++;
    }

    while (chunk_start < end && actual_chunks < num_chunks) {
        const uint8_t *target_end = end;

        // Count rows in this chunk using quote-aware scalar loop.
        // chunk_start is always at a row boundary, so initial state is outside quotes.
        size_t row_count = count_chunk_rows_quote_aware(chunk_start, target_end,
                                                        quote_char, escape_char);

        chunks[actual_chunks].start = chunk_start;
        chunks[actual_chunks].end = target_end;
        chunks[actual_chunks].row_count = row_count;
        chunks[actual_chunks].chunk_index = actual_chunks;

        chunk_start = target_end;
        actual_chunks++;
    }

    *chunk_count = actual_chunks;
    return chunks;
}

cisv_chunk_t *cisv_split_chunks(
    const cisv_mmap_file_t *file,
    int num_chunks,
    int *chunk_count
) {
    return split_chunks_with_quote(file, num_chunks, chunk_count, '"', '\0');
}

int cisv_parse_chunk(cisv_parser *p, const cisv_chunk_t *chunk) {
    if (!p || !chunk || !chunk->start) return -1;

    // Reset parser state for new chunk
    p->cur = chunk->start;
    p->end = chunk->end;
    p->field_start = p->cur;
    p->state = S_NORMAL;
    p->had_error = false;
    p->quote_buffer_pos = 0;
    p->quoted_field_start = NULL;
    p->quoted_field_buffered = false;
    p->quoted_pending_quote = false;
    p->quoted_pending_escape = false;
    p->skip_leading_lf = false;
    p->current_row_fields = 0;
    p->current_row_size = 0;
    p->skip_current_row = false;
    p->row_is_comment = false;

    parse_dispatch(p);

    return p->had_error ? -EINVAL : 0;
}

// =============================================================================
// Batch Parsing Implementation
// High-performance API that returns all data at once (no callbacks)
// =============================================================================

// Initial allocation sizes for batch parsing
#define BATCH_INITIAL_ROWS 1024
#define BATCH_INITIAL_FIELDS 8192
#define BATCH_INITIAL_DATA (1024 * 1024)  // 1MB initial data buffer

// Internal collector for batch parsing
typedef struct {
    cisv_result_t *result;
    size_t current_row_start;  // Index in all_fields where current row starts
    size_t current_row_data_start;
} BatchCollector;

// Ensure result has capacity for more rows
static inline bool batch_ensure_rows(cisv_result_t *r, size_t needed) {
    size_t required;
    if (__builtin_add_overflow(r->row_count, needed, &required)) return false;
    if (required <= r->row_capacity) return true;

    // 1.5x growth: reduces memory waste from ~50% to ~33%
    size_t new_cap = r->row_capacity + (r->row_capacity >> 1);
    if (new_cap < required) new_cap = required;

    size_t alloc_size;
    if (__builtin_mul_overflow(new_cap, sizeof(cisv_row_t), &alloc_size)) return false;
    size_t budget = cisv_runtime_memory_budget();
    if (budget > 0 && alloc_size > budget) return false;

    cisv_row_t *new_rows = realloc(r->rows, alloc_size);
    if (!new_rows) return false;

    r->rows = new_rows;
    r->row_capacity = new_cap;
    return true;
}

// Ensure result has capacity for more fields
static inline bool batch_ensure_fields(cisv_result_t *r, size_t needed) {
    size_t required;
    if (__builtin_add_overflow(r->total_fields, needed, &required)) return false;
    if (required <= r->fields_capacity) return true;

    // 1.5x growth: reduces memory waste from ~50% to ~33%
    size_t new_cap = r->fields_capacity + (r->fields_capacity >> 1);
    if (new_cap < required) new_cap = required;

    size_t fields_bytes;
    size_t lengths_bytes;
    if (__builtin_mul_overflow(new_cap, sizeof(char*), &fields_bytes)) return false;
    if (__builtin_mul_overflow(new_cap, sizeof(size_t), &lengths_bytes)) return false;
    size_t budget = cisv_runtime_memory_budget();
    if (budget > 0 && (fields_bytes > budget || lengths_bytes > budget ||
                       fields_bytes > budget - lengths_bytes)) {
        return false;
    }

    char **new_fields = realloc(r->all_fields, fields_bytes);
    if (!new_fields) return false;

    size_t *new_lengths = realloc(r->all_lengths, lengths_bytes);
    if (!new_lengths) {
        // Preserve the successful realloc result so ownership is not lost
        // if the second growth step fails.
        r->all_fields = new_fields;
        return false;
    }

    r->all_fields = new_fields;
    r->all_lengths = new_lengths;
    r->fields_capacity = new_cap;
    return true;
}

// Ensure result has capacity for more field data
// NOTE: all_fields[] stores offsets (not pointers) during parsing to avoid
// O(n) pointer fixup on reallocation. Offsets are converted to pointers
// in batch_result_finalize() after parsing completes.
static inline bool batch_ensure_data(cisv_result_t *r, size_t needed) {
    size_t required;
    if (__builtin_add_overflow(r->field_data_size, needed, &required)) return false;
    if (required <= r->field_data_capacity) return true;

    // 1.5x growth: reduces memory waste from ~50% to ~33%
    size_t new_cap = r->field_data_capacity + (r->field_data_capacity >> 1);
    if (new_cap < required) new_cap = required;

    // Round up to 64-byte alignment for cache efficiency
    if (new_cap > SIZE_MAX - 63) return false;
    new_cap = (new_cap + 63) & ~(size_t)63;
    size_t budget = cisv_runtime_memory_budget();
    if (budget > 0 && new_cap > budget) return false;

    char *new_data = realloc(r->field_data, new_cap);
    if (!new_data) return false;

    // No pointer fixup needed - we store offsets, not pointers
    r->field_data = new_data;
    r->field_data_capacity = new_cap;
    return true;
}

// Batch field callback - accumulates fields into result
// NOTE: stores field offset (not pointer) to avoid O(n) fixup on realloc
static void batch_field_cb(void *user, const char *data, size_t len) {
    BatchCollector *bc = (BatchCollector *)user;
    cisv_result_t *r = bc->result;

    // Ensure we have space
    if (!batch_ensure_fields(r, 1)) {
        r->error_code = -1;
        snprintf(r->error_message, sizeof(r->error_message), "Out of memory (fields)");
        return;
    }

    if (!batch_ensure_data(r, len + 1)) {
        r->error_code = -1;
        snprintf(r->error_message, sizeof(r->error_message), "Out of memory (data)");
        return;
    }

    // Store current offset before copying (field_data may reallocate)
    size_t offset = r->field_data_size;

    // Copy field data (null-terminated for convenience)
    char *dest = r->field_data + offset;
    memcpy(dest, data, len);
    dest[len] = '\0';

    // Record field offset (converted to pointer in batch_result_finalize)
    r->all_fields[r->total_fields] = (char*)(uintptr_t)offset;
    r->all_lengths[r->total_fields] = len;
    r->total_fields++;
    r->field_data_size += len + 1;
}

// Batch row callback - stores field start index (pointers set up at end)
// We store the start index in the fields pointer and convert to actual
// pointers after parsing is complete (to handle reallocation during parsing)
static void batch_row_cb(void *user) {
    BatchCollector *bc = (BatchCollector *)user;
    cisv_result_t *r = bc->result;

    if (!batch_ensure_rows(r, 1)) {
        r->error_code = -1;
        snprintf(r->error_message, sizeof(r->error_message), "Out of memory (rows)");
        return;
    }

    // Store field count and start index (not actual pointers - those are set later)
    size_t field_count = r->total_fields - bc->current_row_start;
    cisv_row_t *row = &r->rows[r->row_count];

    // Store start index as intptr_t (will be converted to pointer later)
    row->fields = (char **)(intptr_t)bc->current_row_start;
    row->field_lengths = NULL;  // Will be set during finalization
    row->field_count = field_count;

    r->row_count++;
    bc->current_row_start = r->total_fields;
    bc->current_row_data_start = r->field_data_size;
}

// Finalize result by converting stored indices/offsets to actual pointers
// Must be called after all parsing is complete
static void batch_result_finalize(cisv_result_t *r) {
    // First, convert all field offsets to actual pointers
    // This is O(n) but only done once at the end, not on every realloc
    for (size_t i = 0; i < r->total_fields; i++) {
        size_t offset = (size_t)(uintptr_t)r->all_fields[i];
        r->all_fields[i] = r->field_data + offset;
    }

    // Now convert row field indices to actual pointers
    for (size_t i = 0; i < r->row_count; i++) {
        size_t start_index = (size_t)(intptr_t)r->rows[i].fields;
        r->rows[i].fields = &r->all_fields[start_index];
        r->rows[i].field_lengths = &r->all_lengths[start_index];
    }
}

// Batch error callback
static void batch_error_cb(void *user, int line, const char *msg) {
    BatchCollector *bc = (BatchCollector *)user;
    cisv_result_t *r = bc->result;

    r->total_fields = bc->current_row_start;
    r->field_data_size = bc->current_row_data_start;

    if (r->error_code == 0) {
        r->error_code = -1;
        snprintf(r->error_message, sizeof(r->error_message),
                 "Parse error at line %d: %s", line, msg ? msg : "unknown error");
    }
}

// Maximum initial allocation for pre-sized buffers (500MB)
#define BATCH_MAX_INITIAL_ALLOC (500 * 1024 * 1024)

// Allocate and initialize a new result structure with size hints
// file_size_hint: estimated file size (0 to use defaults)
static cisv_result_t *batch_result_create_with_hint(size_t file_size_hint) {
    cisv_result_t *r = calloc(1, sizeof(cisv_result_t));
    if (!r) return NULL;

    // Estimate buffer sizes based on file size
    // Heuristics: ~100 bytes/row average, ~8 fields/row average
    size_t row_cap = BATCH_INITIAL_ROWS;
    size_t field_cap = BATCH_INITIAL_FIELDS;
    size_t data_cap = BATCH_INITIAL_DATA;

    if (file_size_hint > 0) {
        // Estimate rows: assume ~100 bytes per row on average
        size_t est_rows = file_size_hint / 100;
        if (est_rows > row_cap && est_rows * sizeof(cisv_row_t) < BATCH_MAX_INITIAL_ALLOC) {
            row_cap = est_rows;
        }

        // Estimate fields: assume ~8 fields per row
        size_t est_fields = est_rows * 8;
        if (est_fields > field_cap && est_fields * sizeof(char*) < BATCH_MAX_INITIAL_ALLOC) {
            field_cap = est_fields;
        }

        // Field data needs at least file_size bytes (strings + null terminators)
        // Add 10% overhead for null terminators
        size_t est_data = file_size_hint + (file_size_hint / 10);
        if (est_data > data_cap && est_data < BATCH_MAX_INITIAL_ALLOC) {
            data_cap = est_data;
        }

        // Align data capacity to 64 bytes for cache efficiency
        data_cap = (data_cap + 63) & ~(size_t)63;
    }

    size_t memory_budget = cisv_runtime_memory_budget();
    if (memory_budget > 0) {
        size_t row_budget = memory_budget / 8;
        size_t field_budget = memory_budget / 4;
        size_t data_budget = memory_budget / 2;

        size_t max_rows = row_budget / sizeof(cisv_row_t);
        if (max_rows > 0 && row_cap > max_rows) row_cap = max_rows;
        if (row_cap < 128) row_cap = 128;

        size_t field_slot_size = sizeof(char*) + sizeof(size_t);
        size_t max_fields = field_slot_size ? field_budget / field_slot_size : 0;
        if (max_fields > 0 && field_cap > max_fields) field_cap = max_fields;
        if (field_cap < 512) field_cap = 512;

        if (data_budget > 0 && data_cap > data_budget) data_cap = data_budget;
        if (data_cap < 64 * 1024) data_cap = 64 * 1024;
        data_cap = (data_cap + 63) & ~(size_t)63;
    }

    size_t rows_bytes;
    size_t fields_bytes;
    size_t lengths_bytes;
    if (__builtin_mul_overflow(row_cap, sizeof(cisv_row_t), &rows_bytes) ||
        __builtin_mul_overflow(field_cap, sizeof(char*), &fields_bytes) ||
        __builtin_mul_overflow(field_cap, sizeof(size_t), &lengths_bytes)) {
        free(r);
        return NULL;
    }

    // Allocate buffers with estimated sizes
    r->rows = malloc(rows_bytes);
    r->all_fields = malloc(fields_bytes);
    r->all_lengths = malloc(lengths_bytes);
    r->field_data = malloc(data_cap);

    if (!r->rows || !r->all_fields || !r->all_lengths || !r->field_data) {
        free(r->rows);
        free(r->all_fields);
        free(r->all_lengths);
        free(r->field_data);
        free(r);
        return NULL;
    }

    r->row_capacity = row_cap;
    r->fields_capacity = field_cap;
    r->field_data_capacity = data_cap;

    return r;
}

// Allocate and initialize a new result structure with default sizes
static cisv_result_t *batch_result_create(void) {
    return batch_result_create_with_hint(0);
}

static cisv_result_t *batch_result_create_error(int code, const char *message) {
    cisv_result_t *r = calloc(1, sizeof(cisv_result_t));
    if (!r) return NULL;
    r->error_code = code;
    snprintf(r->error_message, sizeof(r->error_message), "%s", message ? message : "Parse error");
    return r;
}

void cisv_result_free(cisv_result_t *result) {
    if (!result) return;

    // Note: row->fields and row->field_lengths point into all_fields/all_lengths
    // so we don't need to free them separately
    free(result->rows);
    free(result->all_fields);
    free(result->all_lengths);
    free(result->field_data);
    free(result);
}

cisv_result_t *cisv_parse_file_batch(const char *path, const cisv_config *config) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    // Get file size for buffer pre-sizing (reduces reallocations)
    size_t file_size_hint = 0;
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        file_size_hint = (size_t)st.st_size;
    }

    cisv_result_t *result = batch_result_create_with_hint(file_size_hint);
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }

    BatchCollector bc = {
        .result = result,
        .current_row_start = 0,
        .current_row_data_start = 0
    };

    // Create config with batch callbacks
    cisv_config batch_config;
    if (config) {
        batch_config = *config;
    } else {
        cisv_config_init(&batch_config);
    }
    batch_config.field_cb = batch_field_cb;
    batch_config.row_cb = batch_row_cb;
    batch_config.error_cb = batch_error_cb;
    batch_config.user = &bc;

    cisv_parser *parser = cisv_parser_create_with_config(&batch_config);
    if (!parser) {
        cisv_result_free(result);
        errno = ENOMEM;
        return NULL;
    }

    int parse_result = cisv_parser_parse_file(parser, path);
    cisv_parser_destroy(parser);

    if (parse_result < 0) {
        result->error_code = parse_result;
        snprintf(result->error_message, sizeof(result->error_message),
                 "Failed to parse file: %s", strerror(-parse_result));
    }

    // Convert stored indices to actual pointers now that parsing is complete
    batch_result_finalize(result);

    return result;
}

cisv_result_t *cisv_parse_string_batch(const char *data, size_t len, const cisv_config *config) {
    if (!data) {
        errno = EINVAL;
        return NULL;
    }

    cisv_result_t *result = batch_result_create();
    if (!result) {
        errno = ENOMEM;
        return NULL;
    }

    BatchCollector bc = {
        .result = result,
        .current_row_start = 0,
        .current_row_data_start = 0
    };

    // Create config with batch callbacks
    cisv_config batch_config;
    if (config) {
        batch_config = *config;
    } else {
        cisv_config_init(&batch_config);
    }
    batch_config.field_cb = batch_field_cb;
    batch_config.row_cb = batch_row_cb;
    batch_config.error_cb = batch_error_cb;
    batch_config.user = &bc;

    cisv_parser *parser = cisv_parser_create_with_config(&batch_config);
    if (!parser) {
        cisv_result_free(result);
        errno = ENOMEM;
        return NULL;
    }

    cisv_parser_write(parser, (const uint8_t *)data, len);
    cisv_parser_end(parser);
    cisv_parser_destroy(parser);

    // Convert stored indices to actual pointers now that parsing is complete
    batch_result_finalize(result);

    return result;
}

// =============================================================================
// Parallel Batch Parsing Implementation
// =============================================================================

#include <pthread.h>

// Thread argument for parallel parsing
typedef struct {
    const cisv_chunk_t *chunk;
    const cisv_config *config;
    cisv_result_t *result;
    size_t chunk_size_hint;
} ParallelParseArg;

// Thread function for parallel parsing
static void *parallel_parse_thread(void *arg) {
    ParallelParseArg *parg = (ParallelParseArg *)arg;

    cisv_result_t *result = batch_result_create_with_hint(parg->chunk_size_hint);
    if (!result) {
        parg->result = batch_result_create_error(-ENOMEM, "Out of memory (result)");
        return NULL;
    }

    BatchCollector bc = {
        .result = result,
        .current_row_start = 0,
        .current_row_data_start = 0
    };

    // Create config with batch callbacks
    cisv_config batch_config;
    if (parg->config) {
        batch_config = *parg->config;
    } else {
        cisv_config_init(&batch_config);
    }
    batch_config.field_cb = batch_field_cb;
    batch_config.row_cb = batch_row_cb;
    batch_config.error_cb = batch_error_cb;
    batch_config.user = &bc;

    cisv_parser *parser = cisv_parser_create_with_config(&batch_config);
    if (!parser) {
        result->error_code = -ENOMEM;
        snprintf(result->error_message, sizeof(result->error_message), "Out of memory (parser)");
        parg->result = result;
        return NULL;
    }

    int parse_result = cisv_parse_chunk(parser, parg->chunk);
    cisv_parser_destroy(parser);
    if (parse_result < 0 && result->error_code == 0) {
        result->error_code = parse_result;
        snprintf(result->error_message, sizeof(result->error_message), "Parse error in chunk");
    }

    // Convert stored indices to actual pointers now that parsing is complete
    batch_result_finalize(result);

    parg->result = result;
    return NULL;
}

// Get number of available CPUs
static int get_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    if (count > 0) return (int)count;
#endif
    return 4;  // Default fallback
}

cisv_result_t **cisv_parse_file_parallel(const char *path, const cisv_config *config,
                                          int num_threads, int *result_count) {
    if (!path || !result_count) {
        errno = EINVAL;
        return NULL;
    }

    *result_count = 0;

    // Auto-detect thread count
    if (num_threads <= 0) {
        num_threads = get_cpu_count();
    }

    // Limit to reasonable maximum
    if (num_threads > 64) num_threads = 64;

    int proc_cap = cisv_runtime_max_procs();
    if (proc_cap > 0 && num_threads > proc_cap) {
        num_threads = proc_cap;
    }
    if (num_threads < 1) num_threads = 1;

    // Memory-map the file
    cisv_mmap_file_t *mmap_file = cisv_mmap_open(path);
    if (!mmap_file) {
        return NULL;
    }

    size_t parallel_min_bytes = cisv_runtime_parallel_min_bytes();
    if (parallel_min_bytes > 0 && mmap_file->size < parallel_min_bytes) {
        num_threads = 1;
    }

    size_t memory_budget = cisv_runtime_memory_budget();
    if (memory_budget > 0) {
        size_t min_per_thread = 256 * 1024;
        size_t memory_thread_cap = memory_budget / min_per_thread;
        if (memory_thread_cap == 0) memory_thread_cap = 1;
        if ((size_t)num_threads > memory_thread_cap) {
            num_threads = (int)memory_thread_cap;
        }
    }

    // Split into chunks
    int chunk_count;
    char quote_char = '"';
    char escape_char = '\0';
    if (config && config->quote != '\0') {
        quote_char = config->quote;
    }
    if (config && config->escape != '\0') {
        escape_char = config->escape;
    }
    cisv_chunk_t *chunks = split_chunks_with_quote(mmap_file, num_threads, &chunk_count,
                                                   quote_char, escape_char);
    if (!chunks || chunk_count == 0) {
        cisv_mmap_close(mmap_file);
        return NULL;
    }

    // Fast path: single chunk does not benefit from thread orchestration.
    if (chunk_count == 1) {
        cisv_result_t **results = calloc(1, sizeof(cisv_result_t *));
        if (!results) {
            free(chunks);
            cisv_mmap_close(mmap_file);
            errno = ENOMEM;
            return NULL;
        }

        ParallelParseArg arg = {
            .chunk = &chunks[0],
            .config = config,
            .result = NULL,
            .chunk_size_hint = (size_t)(chunks[0].end - chunks[0].start),
        };

        parallel_parse_thread(&arg);
        results[0] = arg.result;
        if (!results[0]) {
            results[0] = batch_result_create_error(-ENOMEM, "Worker failed without a result");
        }

        free(chunks);
        cisv_mmap_close(mmap_file);
        *result_count = 1;
        return results;
    }

    // Allocate thread arguments and results array
    ParallelParseArg *args = calloc(chunk_count, sizeof(ParallelParseArg));
    pthread_t *threads = calloc(chunk_count, sizeof(pthread_t));
    cisv_result_t **results = calloc(chunk_count, sizeof(cisv_result_t *));

    if (!args || !threads || !results) {
        free(args);
        free(threads);
        free(results);
        free(chunks);
        cisv_mmap_close(mmap_file);
        errno = ENOMEM;
        return NULL;
    }

    // Launch threads
    for (int i = 0; i < chunk_count; i++) {
        args[i].chunk = &chunks[i];
        args[i].config = config;
        args[i].result = NULL;
        args[i].chunk_size_hint = (size_t)(chunks[i].end - chunks[i].start);

        if (pthread_create(&threads[i], NULL, parallel_parse_thread, &args[i]) != 0) {
            // Thread creation failed - wait for already-launched threads
            for (int j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
                if (args[j].result) {
                    cisv_result_free(args[j].result);
                }
            }
            free(args);
            free(threads);
            free(results);
            free(chunks);
            cisv_mmap_close(mmap_file);
            return NULL;
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < chunk_count; i++) {
        pthread_join(threads[i], NULL);
        results[i] = args[i].result;
        if (!results[i]) {
            results[i] = batch_result_create_error(-ENOMEM, "Worker failed without a result");
        }
    }

    free(args);
    free(threads);
    free(chunks);
    cisv_mmap_close(mmap_file);

    *result_count = chunk_count;
    return results;
}

void cisv_results_free(cisv_result_t **results, int count) {
    if (!results) return;

    for (int i = 0; i < count; i++) {
        cisv_result_free(results[i]);
    }
    free(results);
}

// =============================================================================
// Row-by-Row Iterator Implementation (fgetcsv-style)
// Forward-only iteration with minimal memory footprint
// =============================================================================

// Initial allocation sizes for iterator
#define ITER_INITIAL_FIELDS 32
#define ITER_INITIAL_DATA 4096

struct cisv_iterator {
    // File access (mmap)
    int fd;
    uint8_t *data;
    size_t file_size;

    // Position tracking
    const uint8_t *pos;        // Current position
    const uint8_t *end;        // End of file

    // Row buffer (reused, grows as needed)
    char **fields;             // Pointers to field data
    size_t *lengths;           // Field lengths
    size_t field_count;        // Fields in current row
    size_t field_capacity;     // Allocated slots

    // Field data storage (contiguous)
    char *field_data;          // String storage
    size_t field_data_len;     // Current usage
    size_t field_data_cap;     // Capacity

    // Parser state
    int state;                 // S_NORMAL, S_QUOTED
    char delimiter;
    char quote;
    char escape;
    bool relaxed;
    bool trim;
    bool skip_empty_lines;
    char comment;
    size_t from_line;
    size_t to_line;
    size_t line_num;
    bool first_field_quoted;
    size_t max_row_size;
    size_t current_row_size;

    // Quote buffer for escaped quotes
    uint8_t *quote_buffer;
    size_t quote_buffer_pos;
    size_t quote_buffer_size;

    // Status
    bool eof;
    int error_code;
};

#define ITER_ROW_SKIP 1

static inline void iter_clear_row(cisv_iterator_t *it) {
    it->field_count = 0;
    it->field_data_len = 0;
    it->current_row_size = 0;
    it->quote_buffer_pos = 0;
    it->first_field_quoted = false;
}

static inline void iter_set_empty_output(const char ***fields,
                                         const size_t **lengths,
                                         size_t *field_count) {
    if (fields) *fields = NULL;
    if (lengths) *lengths = NULL;
    if (field_count) *field_count = 0;
}

// Ensure iterator has capacity for more fields
static inline bool iter_ensure_fields(cisv_iterator_t *it, size_t needed) {
    size_t required;
    if (__builtin_add_overflow(it->field_count, needed, &required)) return false;
    if (required <= it->field_capacity) return true;

    size_t new_cap = it->field_capacity + (it->field_capacity >> 1);
    if (new_cap < required) new_cap = required;

    size_t fields_bytes;
    size_t lengths_bytes;
    if (__builtin_mul_overflow(new_cap, sizeof(char*), &fields_bytes)) return false;
    if (__builtin_mul_overflow(new_cap, sizeof(size_t), &lengths_bytes)) return false;
    size_t budget = cisv_runtime_memory_budget();
    if (budget > 0 && (fields_bytes > budget || lengths_bytes > budget ||
                       fields_bytes > budget - lengths_bytes)) {
        return false;
    }

    char **new_fields = realloc(it->fields, fields_bytes);
    if (!new_fields) return false;

    size_t *new_lengths = realloc(it->lengths, lengths_bytes);
    if (!new_lengths) {
        it->fields = new_fields;  // Keep the successful realloc
        return false;
    }

    it->fields = new_fields;
    it->lengths = new_lengths;
    it->field_capacity = new_cap;
    return true;
}

// Ensure iterator has capacity for more field data
static inline bool iter_ensure_data(cisv_iterator_t *it, size_t needed) {
    size_t with_nul;
    size_t required;
    if (__builtin_add_overflow(needed, 1, &with_nul)) return false;
    if (__builtin_add_overflow(it->field_data_len, with_nul, &required)) return false;
    if (required <= it->field_data_cap) return true;

    size_t new_cap = it->field_data_cap + (it->field_data_cap >> 1);
    if (new_cap < required) new_cap = required;
    if (new_cap > SIZE_MAX - 63) return false;
    new_cap = (new_cap + 63) & ~(size_t)63;  // Align to 64 bytes
    size_t budget = cisv_runtime_memory_budget();
    if (budget > 0 && new_cap > budget) return false;

    char *new_data = realloc(it->field_data, new_cap);
    if (!new_data) return false;

    // Update field pointers if data moved
    if (new_data != it->field_data) {
        ptrdiff_t offset = new_data - it->field_data;
        for (size_t i = 0; i < it->field_count; i++) {
            it->fields[i] += offset;
        }
    }

    it->field_data = new_data;
    it->field_data_cap = new_cap;
    return true;
}

// Ensure quote buffer has space
static inline bool iter_ensure_quote_buffer(cisv_iterator_t *it, size_t needed) {
    size_t required;
    if (__builtin_add_overflow(it->quote_buffer_pos, needed, &required)) return false;
    if (required <= it->quote_buffer_size) return true;

    size_t new_size = it->quote_buffer_size + (it->quote_buffer_size >> 1);
    if (new_size < required) new_size = required;
    if (new_size > cisv_buffer_limit_from_budget(cisv_runtime_memory_budget())) return false;

    uint8_t *new_buf = realloc(it->quote_buffer, new_size);
    if (!new_buf) return false;

    it->quote_buffer = new_buf;
    it->quote_buffer_size = new_size;
    return true;
}

// Add a field to current row
static inline bool iter_add_field(cisv_iterator_t *it, const uint8_t *start, size_t len) {
    if (it->max_row_size > 0) {
        size_t with_delimiter;
        size_t next_size;
        if (__builtin_add_overflow(len, 1, &with_delimiter) ||
            __builtin_add_overflow(it->current_row_size, with_delimiter, &next_size) ||
            next_size > it->max_row_size) {
            it->error_code = EOVERFLOW;
            return false;
        }
        it->current_row_size = next_size;
    }

    if (!iter_ensure_fields(it, 1)) {
        it->error_code = ENOMEM;
        return false;
    }
    if (!iter_ensure_data(it, len)) {
        it->error_code = ENOMEM;
        return false;
    }

    // Copy field data
    char *field_ptr = it->field_data + it->field_data_len;
    memcpy(field_ptr, start, len);
    field_ptr[len] = '\0';  // Null-terminate

    it->fields[it->field_count] = field_ptr;
    it->lengths[it->field_count] = len;
    it->field_count++;
    it->field_data_len += len + 1;

    return true;
}

// Add a field from quote buffer (for fields with escaped quotes)
static inline bool iter_add_quoted_field(cisv_iterator_t *it) {
    bool result = iter_add_field(it, it->quote_buffer, it->quote_buffer_pos);
    it->quote_buffer_pos = 0;
    return result;
}

cisv_iterator_t *cisv_iterator_open(const char *path, const cisv_config *config) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (config && !cisv_config_is_valid(config)) {
        errno = EINVAL;
        return NULL;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return NULL;
    }

    // Handle empty file
    if (st.st_size == 0) {
        close(fd);
        errno = 0;  // Not an error, just empty
        // Return iterator in EOF state
        cisv_iterator_t *it = calloc(1, sizeof(cisv_iterator_t));
        if (!it) {
            errno = ENOMEM;
            return NULL;
        }
        it->fd = -1;
        it->eof = true;
        it->delimiter = config ? config->delimiter : ',';
        it->quote = config ? config->quote : '"';
        it->escape = config ? config->escape : '\0';
        it->relaxed = config ? config->relaxed : false;
        it->trim = config ? config->trim : false;
        it->skip_empty_lines = config ? config->skip_empty_lines : false;
        it->comment = config ? config->comment : '\0';
        it->from_line = config && config->from_line > 0 ? (size_t)config->from_line : 1;
        it->to_line = config && config->to_line > 0 ? (size_t)config->to_line : 0;
        size_t memory_budget = cisv_runtime_memory_budget();
        size_t env_row_limit = cisv_runtime_max_row_size();
        it->max_row_size = config && config->max_row_size > 0
            ? config->max_row_size
            : (env_row_limit > 0 ? env_row_limit : cisv_adaptive_row_limit(memory_budget));
        return it;
    }

    int flags = MAP_PRIVATE;
#ifdef MAP_POPULATE
    flags |= MAP_POPULATE;
#endif

    uint8_t *data = (uint8_t*)mmap(NULL, st.st_size, PROT_READ, flags, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    // Advise kernel for sequential access
    madvise(data, st.st_size, MADV_SEQUENTIAL | MADV_WILLNEED);

    cisv_iterator_t *it = calloc(1, sizeof(cisv_iterator_t));
    if (!it) {
        munmap(data, st.st_size);
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    it->fd = fd;
    it->data = data;
    it->file_size = st.st_size;
    it->pos = data;
    it->end = data + st.st_size;
    it->state = S_NORMAL;

    // Config
    if (config) {
        it->delimiter = config->delimiter;
        it->quote = config->quote;
        it->escape = config->escape;
        it->relaxed = config->relaxed;
        it->trim = config->trim;
        it->skip_empty_lines = config->skip_empty_lines;
        it->comment = config->comment;
        it->from_line = config->from_line > 0 ? (size_t)config->from_line : 1;
        it->to_line = config->to_line > 0 ? (size_t)config->to_line : 0;
        size_t memory_budget = cisv_runtime_memory_budget();
        size_t env_row_limit = cisv_runtime_max_row_size();
        it->max_row_size = config->max_row_size > 0
            ? config->max_row_size
            : (env_row_limit > 0 ? env_row_limit : cisv_adaptive_row_limit(memory_budget));
    } else {
        it->delimiter = ',';
        it->quote = '"';
        it->escape = '\0';
        it->relaxed = false;
        it->trim = false;
        it->skip_empty_lines = false;
        it->comment = '\0';
        it->from_line = 1;
        it->to_line = 0;
        it->max_row_size = cisv_adaptive_row_limit(cisv_runtime_memory_budget());
    }

    // Allocate initial buffers
    it->field_capacity = ITER_INITIAL_FIELDS;
    it->fields = malloc(it->field_capacity * sizeof(char*));
    it->lengths = malloc(it->field_capacity * sizeof(size_t));
    it->field_data_cap = ITER_INITIAL_DATA;
    it->field_data = malloc(it->field_data_cap);
    it->quote_buffer_size = 4096;
    it->quote_buffer = malloc(it->quote_buffer_size);

    if (!it->fields || !it->lengths || !it->field_data || !it->quote_buffer) {
        cisv_iterator_close(it);
        errno = ENOMEM;
        return NULL;
    }

    return it;
}

// Trim whitespace from field (modifies start/end pointers)
static inline void iter_trim_field(const uint8_t **start, const uint8_t **end) {
    while (*start < *end && is_ws(**start)) (*start)++;
    while (*start < *end && is_ws(*(*end - 1))) (*end)--;
}

static inline int iter_finish_row(cisv_iterator_t *it,
                                  const char ***fields,
                                  const size_t **lengths,
                                  size_t *field_count) {
    it->line_num++;

    if (it->to_line != 0 && it->line_num > it->to_line) {
        it->eof = true;
        iter_set_empty_output(fields, lengths, field_count);
        return CISV_ITER_EOF;
    }

    bool empty_row = it->field_count == 1 &&
                     it->lengths[0] == 0 &&
                     !it->first_field_quoted;
    bool comment_row = it->comment != '\0' &&
                       !it->first_field_quoted &&
                       it->field_count > 0 &&
                       it->lengths[0] > 0 &&
                       it->fields[0][0] == it->comment;

    if ((it->skip_empty_lines && empty_row) ||
        comment_row ||
        it->line_num < it->from_line) {
        iter_clear_row(it);
        return ITER_ROW_SKIP;
    }

    if (fields) *fields = (const char **)it->fields;
    if (lengths) *lengths = it->lengths;
    if (field_count) *field_count = it->field_count;
    it->first_field_quoted = false;
    return CISV_ITER_OK;
}

int cisv_iterator_next(cisv_iterator_t *it,
                       const char ***fields,
                       const size_t **lengths,
                       size_t *field_count) {
    if (!it || it->eof) {
        iter_set_empty_output(fields, lengths, field_count);
        return CISV_ITER_EOF;
    }

    // Reset row state
    iter_clear_row(it);

restart_row:
    if (it->pos >= it->end) {
        it->eof = true;
        iter_set_empty_output(fields, lengths, field_count);
        return CISV_ITER_EOF;
    }

    const uint8_t *field_start = it->pos;
    it->state = S_NORMAL;

    // Parse until end of row
    while (it->pos < it->end) {
        uint8_t c = *it->pos;

        if (it->state == S_NORMAL) {
            if (c == it->delimiter) {
                // End of field
                const uint8_t *field_end = it->pos;
                if (it->trim) {
                    iter_trim_field(&field_start, &field_end);
                }
                if (!iter_add_field(it, field_start, field_end - field_start)) {
                    if (it->error_code == 0) it->error_code = ENOMEM;
                    return CISV_ITER_ERROR;
                }
                it->pos++;
                field_start = it->pos;
            } else if (c == '\n') {
                // End of row
                const uint8_t *field_end = it->pos;
                // Handle CRLF if a legacy path reaches LF after CR.
                if (field_end > field_start && *(field_end - 1) == '\r') {
                    field_end--;
                }
                if (it->trim) {
                    iter_trim_field(&field_start, &field_end);
                }
                if (!iter_add_field(it, field_start, field_end - field_start)) {
                    if (it->error_code == 0) it->error_code = ENOMEM;
                    return CISV_ITER_ERROR;
                }
                it->pos++;

                int finished = iter_finish_row(it, fields, lengths, field_count);
                if (finished == ITER_ROW_SKIP) {
                    goto restart_row;
                }
                return finished;

            } else if (c == '\r') {
                const uint8_t *field_end = it->pos;
                if (it->trim) {
                    iter_trim_field(&field_start, &field_end);
                }
                if (!iter_add_field(it, field_start, field_end - field_start)) {
                    if (it->error_code == 0) it->error_code = ENOMEM;
                    return CISV_ITER_ERROR;
                }
                it->pos++;
                if (it->pos < it->end && *it->pos == '\n') {
                    it->pos++;
                }

                int finished = iter_finish_row(it, fields, lengths, field_count);
                if (finished == ITER_ROW_SKIP) {
                    goto restart_row;
                }
                return finished;

            } else if (c == it->quote && it->pos == field_start) {
                // Start of quoted field
                if (it->field_count == 0) {
                    it->first_field_quoted = true;
                }
                it->state = S_QUOTED;
                it->quote_buffer_pos = 0;
                it->pos++;
            } else {
                it->pos++;
            }
        } else {
            // S_QUOTED state
            if (it->escape != '\0' && c == it->escape) {
                if (it->pos + 1 >= it->end) {
                    it->error_code = EINVAL;
                    return CISV_ITER_ERROR;
                }
                if (!iter_ensure_quote_buffer(it, 1)) {
                    if (it->error_code == 0) it->error_code = ENOMEM;
                    return CISV_ITER_ERROR;
                }
                it->quote_buffer[it->quote_buffer_pos++] = *(it->pos + 1);
                it->pos += 2;
            } else if (c == it->quote) {
                // Check for escaped quote
                if (it->pos + 1 < it->end && *(it->pos + 1) == it->quote) {
                    // Escaped quote - add one quote to buffer
                    if (!iter_ensure_quote_buffer(it, 1)) {
                        if (it->error_code == 0) it->error_code = ENOMEM;
                        return CISV_ITER_ERROR;
                    }
                    it->quote_buffer[it->quote_buffer_pos++] = it->quote;
                    it->pos += 2;
                } else {
                    // End of quoted field
                    if (it->trim) {
                        // Trim the quote buffer content
                        const uint8_t *qstart = it->quote_buffer;
                        const uint8_t *qend = it->quote_buffer + it->quote_buffer_pos;
                        iter_trim_field(&qstart, &qend);
                        it->quote_buffer_pos = qend - qstart;
                        if (qstart != it->quote_buffer) {
                            memmove(it->quote_buffer, qstart, it->quote_buffer_pos);
                        }
                    }
                    if (!iter_add_quoted_field(it)) {
                        if (it->error_code == 0) it->error_code = ENOMEM;
                        return CISV_ITER_ERROR;
                    }
                    it->state = S_NORMAL;
                    it->pos++;

                    while (it->pos < it->end && (*it->pos == ' ' || *it->pos == '\t')) {
                        it->pos++;
                    }

                    // Skip delimiter or newline after closing quote
                    if (it->pos < it->end) {
                        if (*it->pos == it->delimiter) {
                            it->pos++;
                            field_start = it->pos;
                        } else if (*it->pos == '\n') {
                            it->pos++;

                            int finished = iter_finish_row(it, fields, lengths, field_count);
                            if (finished == ITER_ROW_SKIP) {
                                goto restart_row;
                            }
                            return finished;
                        } else if (*it->pos == '\r' && it->pos + 1 < it->end && *(it->pos + 1) == '\n') {
                            it->pos += 2;

                            int finished = iter_finish_row(it, fields, lengths, field_count);
                            if (finished == ITER_ROW_SKIP) {
                                goto restart_row;
                            }
                            return finished;
                        } else if (*it->pos == '\r') {
                            it->pos++;

                            int finished = iter_finish_row(it, fields, lengths, field_count);
                            if (finished == ITER_ROW_SKIP) {
                                goto restart_row;
                            }
                            return finished;
                        } else if (!it->relaxed) {
                            it->error_code = EINVAL;
                            return CISV_ITER_ERROR;
                        }
                    }
                    field_start = it->pos;
                }
            } else {
                // Regular character in quoted field
                if (!iter_ensure_quote_buffer(it, 1)) {
                    if (it->error_code == 0) it->error_code = ENOMEM;
                    return CISV_ITER_ERROR;
                }
                it->quote_buffer[it->quote_buffer_pos++] = c;
                it->pos++;
            }
        }
    }

    // End of file - handle last field if any
    if (field_start < it->end || it->quote_buffer_pos > 0) {
        if (it->state == S_QUOTED) {
            if (!it->relaxed) {
                it->error_code = EINVAL;
                return CISV_ITER_ERROR;
            }
            // Unterminated quote - yield what we have
            if (it->trim) {
                const uint8_t *qstart = it->quote_buffer;
                const uint8_t *qend = it->quote_buffer + it->quote_buffer_pos;
                iter_trim_field(&qstart, &qend);
                it->quote_buffer_pos = qend - qstart;
                if (qstart != it->quote_buffer) {
                    memmove(it->quote_buffer, qstart, it->quote_buffer_pos);
                }
            }
            if (!iter_add_quoted_field(it)) {
                if (it->error_code == 0) it->error_code = ENOMEM;
                return CISV_ITER_ERROR;
            }
        } else {
            const uint8_t *field_end = it->end;
            // Handle trailing CR
            if (field_end > field_start && *(field_end - 1) == '\r') {
                field_end--;
            }
            if (it->trim) {
                iter_trim_field(&field_start, &field_end);
            }
            if (!iter_add_field(it, field_start, field_end - field_start)) {
                if (it->error_code == 0) it->error_code = ENOMEM;
                return CISV_ITER_ERROR;
            }
        }
    }

    if (it->field_count > 0) {
        int finished = iter_finish_row(it, fields, lengths, field_count);
        it->eof = true;
        if (finished == ITER_ROW_SKIP) {
            iter_set_empty_output(fields, lengths, field_count);
            return CISV_ITER_EOF;
        }
        return finished;
    }

    it->eof = true;
    iter_set_empty_output(fields, lengths, field_count);
    return CISV_ITER_EOF;
}

void cisv_iterator_close(cisv_iterator_t *it) {
    if (!it) return;

    if (it->data && it->file_size > 0) {
        munmap(it->data, it->file_size);
    }
    if (it->fd >= 0) {
        close(it->fd);
    }

    free(it->fields);
    free(it->lengths);
    free(it->field_data);
    free(it->quote_buffer);
    free(it);
}
