#ifndef AWS_HTTP_H
#define AWS_HTTP_H

/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
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

#include <aws/http/exports.h>

#include <aws/io/io.h>

enum aws_http_errors {
    AWS_ERROR_HTTP_UNKNOWN = 0x0800,
    AWS_ERROR_HTTP_HEADER_NOT_FOUND,
    AWS_ERROR_HTTP_INVALID_HEADER_FIELD,
    AWS_ERROR_HTTP_INVALID_HEADER_NAME,
    AWS_ERROR_HTTP_INVALID_HEADER_VALUE,
    AWS_ERROR_HTTP_INVALID_METHOD,
    AWS_ERROR_HTTP_INVALID_PATH,
    AWS_ERROR_HTTP_INVALID_STATUS_CODE,
    AWS_ERROR_HTTP_MISSING_BODY_STREAM,
    AWS_ERROR_HTTP_INVALID_BODY_STREAM,
    AWS_ERROR_HTTP_CONNECTION_CLOSED,
    AWS_ERROR_HTTP_SWITCHED_PROTOCOLS,
    AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL,
    AWS_ERROR_HTTP_REACTION_REQUIRED,
    AWS_ERROR_HTTP_DATA_NOT_AVAILABLE,
    AWS_ERROR_HTTP_OUTGOING_STREAM_LENGTH_INCORRECT,
    AWS_ERROR_HTTP_CALLBACK_FAILURE,
    AWS_ERROR_HTTP_WEBSOCKET_UPGRADE_FAILURE,
    AWS_ERROR_HTTP_WEBSOCKET_CLOSE_FRAME_SENT,
    AWS_ERROR_HTTP_WEBSOCKET_IS_MIDCHANNEL_HANDLER,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_INVALID_STATE_FOR_ACQUIRE,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_VENDED_CONNECTION_UNDERFLOW,
    AWS_ERROR_HTTP_SERVER_CLOSED,
    AWS_ERROR_HTTP_PROXY_TLS_CONNECT_FAILED,
    AWS_ERROR_HTTP_CONNECTION_MANAGER_SHUTTING_DOWN,
    AWS_ERROR_HTTP_PROTOCOL_ERROR,
    AWS_ERROR_HTTP_STREAM_CLOSED,
    AWS_ERROR_HTTP_INVALID_FRAME_SIZE,

    AWS_ERROR_HTTP_END_RANGE = 0x0C00,
};

enum aws_http_log_subject {
    AWS_LS_HTTP_GENERAL = 0x800,
    AWS_LS_HTTP_CONNECTION,
    AWS_LS_HTTP_SERVER,
    AWS_LS_HTTP_STREAM,
    AWS_LS_HTTP_CONNECTION_MANAGER,
    AWS_LS_HTTP_WEBSOCKET,
    AWS_LS_HTTP_WEBSOCKET_SETUP,
};

enum aws_http_version {
    AWS_HTTP_VERSION_UNKNOWN, /* Invalid version. */
    AWS_HTTP_VERSION_1_0,
    AWS_HTTP_VERSION_1_1,
    AWS_HTTP_VERSION_2,
    AWS_HTTP_VERSION_COUNT,
};

AWS_EXTERN_C_BEGIN

/**
 * Initializes internal datastructures used by aws-c-http.
 * Must be called before using any functionality in aws-c-http.
 */
AWS_HTTP_API
void aws_http_library_init(struct aws_allocator *alloc);

/**
 * Clean up internal datastructures used by aws-c-http.
 * Must not be called until application is done using functionality in aws-c-http.
 */
AWS_HTTP_API
void aws_http_library_clean_up(void);

/**
 * Returns the description of common status codes.
 * Ex: 404 -> "Not Found"
 * An empty string is returned if the status code is not recognized.
 */
AWS_HTTP_API
const char *aws_http_status_text(int status_code);

/**
 * Shortcuts for common HTTP request methods
 */
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_get;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_head;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_post;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_put;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_delete;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_connect;
AWS_HTTP_API
extern const struct aws_byte_cursor aws_http_method_options;

AWS_EXTERN_C_END

#endif /* AWS_HTTP_H */
