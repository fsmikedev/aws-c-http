/*
 * Copyright 2010-2019 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <aws/http/private/strutil.h>

/* Lookup from '0' -> 0, 'f' -> 0xf, 'F' -> 0xF, etc
 * invalid characters have value 255 */
/* clang-format off */
static const uint8_t s_ascii_to_num_table[256] = {
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255,
    /* 0 - 9 */
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    255, 255, 255, 255, 255, 255, 255,
    /* A - F */
    0xA, 0xB, 0xC, 0xD, 0xE, 0xF,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255,
    /* a - f */
    0xa, 0xb, 0xc, 0xd, 0xe, 0xf,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};
/* clang-format on */

static int s_read_unsigned(struct aws_byte_cursor cursor, uint64_t *dst, uint8_t base) {
    uint64_t val = 0;
    *dst = 0;

    if (cursor.len == 0) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    /* read from left to right */
    for (size_t i = 0; i < cursor.len; ++i) {
        const uint8_t c = cursor.ptr[i];
        const uint8_t cval = s_ascii_to_num_table[c];
        if (cval >= base) {
            return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        }

        const uint64_t prev_val = val;

        val *= base;
        if (val < prev_val) {
            return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
        }

        val += cval;
        if (val < prev_val) {
            return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
        }
    }

    *dst = val;
    return AWS_OP_SUCCESS;
}

int aws_strutil_read_unsigned_num(struct aws_byte_cursor cursor, uint64_t *dst) {
    return s_read_unsigned(cursor, dst, 10 /*base*/);
}

int aws_strutil_read_unsigned_hex(struct aws_byte_cursor cursor, uint64_t *dst) {
    return s_read_unsigned(cursor, dst, 16 /*base*/);
}

static struct aws_byte_cursor s_trim(struct aws_byte_cursor cursor, const bool trim_table[256]) {
    /* trim leading whitespace */
    size_t i;
    for (i = 0; i < cursor.len; ++i) {
        const uint8_t c = cursor.ptr[i];
        if (!trim_table[c]) {
            break;
        }
    }
    cursor.ptr += i;
    cursor.len -= i;

    /* trim trailing whitespace */
    for (; cursor.len; --cursor.len) {
        const uint8_t c = cursor.ptr[cursor.len - 1];
        if (!trim_table[c]) {
            break;
        }
    }

    return cursor;
}

static const bool s_http_whitespace_table[256] = {
    [' '] = true,
    ['\t'] = true,
};

struct aws_byte_cursor aws_strutil_trim_http_whitespace(struct aws_byte_cursor cursor) {
    return s_trim(cursor, s_http_whitespace_table);
}
