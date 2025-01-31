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

#include <aws/http/private/h1_connection.h>
#include <aws/http/private/h2_connection.h>
#include <aws/http/private/proxy_impl.h>

#include <aws/common/hash_table.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>

#if _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#endif

static struct aws_http_connection_system_vtable s_default_system_vtable = {
    .new_socket_channel = aws_client_bootstrap_new_socket_channel,
    .new_tls_socket_channel = aws_client_bootstrap_new_tls_socket_channel,
};

static const struct aws_http_connection_system_vtable *s_system_vtable_ptr = &s_default_system_vtable;

void aws_http_connection_set_system_vtable(const struct aws_http_connection_system_vtable *system_vtable) {
    s_system_vtable_ptr = system_vtable;
}

AWS_STATIC_STRING_FROM_LITERAL(s_alpn_protocol_http_1_1, "http/1.1");
AWS_STATIC_STRING_FROM_LITERAL(s_alpn_protocol_http_2, "h2");

struct aws_http_server {
    struct aws_allocator *alloc;
    struct aws_server_bootstrap *bootstrap;
    bool is_using_tls;
    size_t initial_window_size;
    void *user_data;
    aws_http_server_on_incoming_connection_fn *on_incoming_connection;
    aws_http_server_on_destroy_fn *on_destroy_complete;
    struct aws_socket *socket;

    /* Any thread may touch this data, but the lock must be held */
    struct {
        struct aws_mutex lock;
        bool is_shutting_down;
        struct aws_hash_table channel_to_connection_map;
    } synced_data;
};

void s_server_lock_synced_data(struct aws_http_server *server) {
    int err = aws_mutex_lock(&server->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

void s_server_unlock_synced_data(struct aws_http_server *server) {
    int err = aws_mutex_unlock(&server->synced_data.lock);
    AWS_ASSERT(!err);
    (void)err;
}

/* Determine the http-version, create appropriate type of connection, and insert it into the channel. */
static struct aws_http_connection *s_connection_new(
    struct aws_allocator *alloc,
    struct aws_channel *channel,
    bool is_server,
    bool is_using_tls,
    size_t initial_window_size) {

    struct aws_channel_slot *connection_slot = NULL;
    struct aws_http_connection *connection = NULL;

    /* Create slot for connection. */
    connection_slot = aws_channel_slot_new(channel);
    if (!connection_slot) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create slot in channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    int err = aws_channel_slot_insert_end(channel, connection_slot);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to insert slot into channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto error;
    }

    /* Determine HTTP version */
    enum aws_http_version version = AWS_HTTP_VERSION_1_1;

    if (is_using_tls) {
        /* Query TLS channel handler (immediately to left in the channel) for negotiated ALPN protocol */
        if (!connection_slot->adj_left || !connection_slot->adj_left->handler) {
            aws_raise_error(AWS_ERROR_INVALID_STATE);
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION, "static: Failed to find TLS handler in channel %p.", (void *)channel);
            goto error;
        }

        struct aws_channel_slot *tls_slot = connection_slot->adj_left;
        struct aws_channel_handler *tls_handler = tls_slot->handler;
        struct aws_byte_buf protocol = aws_tls_handler_protocol(tls_handler);
        if (protocol.len) {
            if (aws_string_eq_byte_buf(s_alpn_protocol_http_1_1, &protocol)) {
                version = AWS_HTTP_VERSION_1_1;
            } else if (aws_string_eq_byte_buf(s_alpn_protocol_http_2, &protocol)) {
                version = AWS_HTTP_VERSION_2;
            } else {
                AWS_LOGF_WARN(AWS_LS_HTTP_CONNECTION, "static: Unrecognized ALPN protocol. Assuming HTTP/1.1");
                AWS_LOGF_DEBUG(
                    AWS_LS_HTTP_CONNECTION, "static: Unrecognized ALPN protocol " PRInSTR, AWS_BYTE_BUF_PRI(protocol));

                version = AWS_HTTP_VERSION_1_1;
            }
        }
    }

    /* Create connection/handler */
    switch (version) {
        case AWS_HTTP_VERSION_1_1:
            if (is_server) {
                connection = aws_http_connection_new_http1_1_server(alloc, initial_window_size);
            } else {
                connection = aws_http_connection_new_http1_1_client(alloc, initial_window_size);
            }
            break;
        case AWS_HTTP_VERSION_2:
            AWS_FATAL_ASSERT(false && "H2 is not currently supported"); /* lol nice try */
            if (is_server) {
                connection = aws_http_connection_new_http2_server(alloc, initial_window_size);
            } else {
                connection = aws_http_connection_new_http2_client(alloc, initial_window_size);
            }
            break;
        default:
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "static: Unsupported version " PRInSTR,
                AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(version)));

            aws_raise_error(AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL);
            goto error;
    }

    if (!connection) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create " PRInSTR " %s connection object, error %d (%s).",
            AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(version)),
            is_server ? "server" : "client",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    /* Connect handler and slot */
    err = aws_channel_slot_set_handler(connection_slot, &connection->channel_handler);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to setting HTTP handler into slot on channel %p, error %d (%s).",
            (void *)channel,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    connection->channel_slot = connection_slot;

    /* Success! Acquire a hold on the channel to prevent its destruction until the user has
     * given the go-ahead via aws_http_connection_release() */
    aws_channel_acquire_hold(channel);

    return connection;

error:
    if (connection_slot) {
        if (!connection_slot->handler && connection) {
            aws_channel_handler_destroy(&connection->channel_handler);
        }

        aws_channel_slot_remove(connection_slot);
    }

    return NULL;
}

void aws_http_connection_close(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    connection->vtable->close(connection);
}

bool aws_http_connection_is_open(const struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    return connection->vtable->is_open(connection);
}

bool aws_http_connection_is_client(const struct aws_http_connection *connection) {
    return connection->client_data;
}

bool aws_http_connection_is_server(const struct aws_http_connection *connection) {
    return connection->server_data;
}

void aws_http_connection_update_window(struct aws_http_connection *connection, size_t increment_size) {
    AWS_ASSERT(connection);
    connection->vtable->update_window(connection, increment_size);
}

struct aws_channel *aws_http_connection_get_channel(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    return connection->channel_slot->channel;
}

void aws_http_connection_acquire(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    aws_atomic_fetch_add(&connection->refcount, 1);
}

void aws_http_connection_release(struct aws_http_connection *connection) {
    AWS_ASSERT(connection);
    size_t prev_refcount = aws_atomic_fetch_sub(&connection->refcount, 1);
    if (prev_refcount == 1) {
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Final connection refcount released, shut down if necessary.",
            (void *)connection);

        /* Channel might already be shut down, but make sure */
        aws_channel_shutdown(connection->channel_slot->channel, AWS_ERROR_SUCCESS);

        /* When the channel's refcount reaches 0, it destroys its slots/handlers, which will destroy the connection */
        aws_channel_release_hold(connection->channel_slot->channel);
    } else {
        AWS_ASSERT(prev_refcount != 0);
        AWS_LOGF_TRACE(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Connection refcount released, %zu remaining.",
            (void *)connection,
            prev_refcount - 1);
    }
}

/* At this point, the server bootstrapper has accepted an incoming connection from a client and set up a channel.
 * Now we need to create an aws_http_connection and insert it into the channel as a channel-handler.
 * Note: Be careful not to access server->socket until lock is acquired to avoid race conditions */
static void s_server_bootstrap_on_accept_channel_setup(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_server *server = user_data;
    bool user_cb_invoked = false;
    struct aws_http_connection *connection = NULL;
    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "%p: Incoming connection failed with error code %d (%s)",
            (void *)server,
            error_code,
            aws_error_name(error_code));

        goto error;
    }
    /* Create connection */
    connection = s_connection_new(server->alloc, channel, true, server->is_using_tls, server->initial_window_size);
    if (!connection) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "%p: Failed to create connection object, error %d (%s).",
            (void *)server,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    int put_err = 0;
    /* BEGIN CRITICAL SECTION */
    s_server_lock_synced_data(server);
    if (server->synced_data.is_shutting_down) {
        error_code = AWS_ERROR_HTTP_CONNECTION_CLOSED;
    }
    if (!error_code) {
        put_err = aws_hash_table_put(&server->synced_data.channel_to_connection_map, channel, connection, NULL);
    }
    s_server_unlock_synced_data(server);
    /* END CRITICAL SECTION */
    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_ERROR_HTTP_SERVER_CLOSED,
            "id=%p: Incoming connection failed. The server is shutting down.",
            (void *)server);
        goto error;
    }

    if (put_err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "%p: %s:%d: Failed to store connection object, error %d (%s).",
            (void *)server,
            server->socket->local_endpoint.address,
            server->socket->local_endpoint.port,
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    /* Tell user of successful connection. */
    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: " PRInSTR " server connection established at %p %s:%d.",
        (void *)connection,
        AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(connection->http_version)),
        (void *)server,
        server->socket->local_endpoint.address,
        server->socket->local_endpoint.port);

    server->on_incoming_connection(server, connection, AWS_ERROR_SUCCESS, server->user_data);
    user_cb_invoked = true;

    /* If user failed to configure the server during callback, shut down the channel. */
    if (!connection->server_data->on_incoming_request) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Caller failed to invoke aws_http_connection_configure_server() during on_incoming_connection "
            "callback, closing connection.",
            (void *)connection);

        aws_raise_error(AWS_ERROR_HTTP_REACTION_REQUIRED);
        goto error;
    }
    return;

error:

    if (!error_code) {
        error_code = aws_last_error();
    }

    if (!user_cb_invoked) {
        server->on_incoming_connection(server, NULL, error_code, server->user_data);
    }

    if (channel) {
        aws_channel_shutdown(channel, error_code);
    }

    if (connection) {
        /* release the ref count for the user side */
        aws_http_connection_release(connection);
    }
}

/* clean the server memory up */
static void s_http_server_clean_up(struct aws_http_server *server) {
    if (!server) {
        return;
    }
    /* invoke the user callback */
    if (server->on_destroy_complete) {
        server->on_destroy_complete(server->user_data);
    }
    aws_hash_table_clean_up(&server->synced_data.channel_to_connection_map);
    aws_mutex_clean_up(&server->synced_data.lock);
    aws_mem_release(server->alloc, server);
}

/* At this point, the channel for a server connection has completed shutdown, but hasn't been destroyed yet. */
static void s_server_bootstrap_on_accept_channel_shutdown(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_server *server = user_data;

    /* Figure out which connection this was, and remove that entry from the map.
     * It won't be in the map if something went wrong while setting up the connection. */
    struct aws_hash_element map_elem;
    int was_present;

    /* BEGIN CRITICAL SECTION */
    s_server_lock_synced_data(server);
    int remove_err =
        aws_hash_table_remove(&server->synced_data.channel_to_connection_map, channel, &map_elem, &was_present);
    s_server_unlock_synced_data(server);
    /* END CRITICAL SECTION */

    if (!remove_err && was_present) {
        struct aws_http_connection *connection = map_elem.value;
        AWS_LOGF_INFO(AWS_LS_HTTP_CONNECTION, "id=%p: Server connection shut down.", (void *)connection);
        /* Tell user about shutdown */
        if (connection->server_data->on_shutdown) {
            connection->server_data->on_shutdown(connection, error_code, connection->user_data);
        }
    }
}

/* the server listener has finished the destroy process, no existing connections
 * finally safe to clean the server up */
static void s_server_bootstrap_on_server_listener_destroy(struct aws_server_bootstrap *bootstrap, void *user_data) {
    (void)bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_server *server = user_data;
    s_http_server_clean_up(server);
}

struct aws_http_server *aws_http_server_new(const struct aws_http_server_options *options) {
    aws_http_fatal_assert_library_initialized();

    struct aws_http_server *server = NULL;

    if (!options || options->self_size == 0 || !options->allocator || !options->bootstrap || !options->socket_options ||
        !options->on_incoming_connection || !options->endpoint) {

        AWS_LOGF_ERROR(AWS_LS_HTTP_SERVER, "static: Invalid options, cannot create server.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        /* nothing to clean up */
        return NULL;
    }

    server = aws_mem_calloc(options->allocator, 1, sizeof(struct aws_http_server));
    if (!server) {
        /* nothing to clean up */
        return NULL;
    }

    server->alloc = options->allocator;
    server->bootstrap = options->bootstrap;
    server->is_using_tls = options->tls_options != NULL;
    server->initial_window_size = options->initial_window_size;
    server->user_data = options->server_user_data;
    server->on_incoming_connection = options->on_incoming_connection;
    server->on_destroy_complete = options->on_destroy_complete;

    int err = aws_mutex_init(&server->synced_data.lock);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER, "static: Failed to initialize mutex, error %d (%s).", err, aws_error_name(err));
        goto mutex_error;
    }
    err = aws_hash_table_init(
        &server->synced_data.channel_to_connection_map, server->alloc, 16, aws_hash_ptr, aws_ptr_eq, NULL, NULL);
    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "static: Cannot create server, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));
        goto hash_table_error;
    }
    /* Protect against callbacks firing before server->socket is set */
    s_server_lock_synced_data(server);
    if (options->tls_options) {
        server->is_using_tls = true;

        server->socket = aws_server_bootstrap_new_tls_socket_listener(
            options->bootstrap,
            options->endpoint,
            options->socket_options,
            options->tls_options,
            s_server_bootstrap_on_accept_channel_setup,
            s_server_bootstrap_on_accept_channel_shutdown,
            s_server_bootstrap_on_server_listener_destroy,
            server);
    } else {
        server->socket = aws_server_bootstrap_new_socket_listener(
            options->bootstrap,
            options->endpoint,
            options->socket_options,
            s_server_bootstrap_on_accept_channel_setup,
            s_server_bootstrap_on_accept_channel_shutdown,
            s_server_bootstrap_on_server_listener_destroy,
            server);
    }
    s_server_unlock_synced_data(server);

    if (!server->socket) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_SERVER,
            "static: Failed creating new socket listener, error %d (%s). Cannot create server.",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto socket_error;
    }

    AWS_LOGF_INFO(
        AWS_LS_HTTP_SERVER,
        "%p %s:%d: Server setup complete, listening for incoming connections.",
        (void *)server,
        server->socket->local_endpoint.address,
        server->socket->local_endpoint.port);

    return server;

socket_error:
    aws_hash_table_clean_up(&server->synced_data.channel_to_connection_map);
hash_table_error:
    aws_mutex_clean_up(&server->synced_data.lock);
mutex_error:
    aws_mem_release(server->alloc, server);
    return NULL;
}

void aws_http_server_release(struct aws_http_server *server) {
    if (!server) {
        return;
    }
    bool already_shutting_down = false;
    /* BEGIN CRITICAL SECTION */
    s_server_lock_synced_data(server);
    if (server->synced_data.is_shutting_down) {
        already_shutting_down = true;
    } else {
        server->synced_data.is_shutting_down = true;
    }
    if (!already_shutting_down) {
        /* shutdown all existing channels */
        for (struct aws_hash_iter iter = aws_hash_iter_begin(&server->synced_data.channel_to_connection_map);
             !aws_hash_iter_done(&iter);
             aws_hash_iter_next(&iter)) {
            struct aws_channel *channel = (struct aws_channel *)iter.element.key;
            aws_channel_shutdown(channel, AWS_ERROR_HTTP_CONNECTION_CLOSED);
        }
    }
    s_server_unlock_synced_data(server);
    /* END CRITICAL SECTION */

    if (already_shutting_down) {
        /* The service is already shutting down, not shutting it down again */
        AWS_LOGF_TRACE(AWS_LS_HTTP_SERVER, "id=%p: The server is already shutting down", (void *)server);
        return;
    }

    /* stop listening, clean up the socket, after all existing connections finish shutting down, the
     * s_server_bootstrap_on_server_listener_destroy will be invoked, clean up of the server will be there */
    AWS_LOGF_INFO(
        AWS_LS_HTTP_SERVER,
        "%p %s:%d: Shutting down the server.",
        (void *)server,
        server->socket->local_endpoint.address,
        server->socket->local_endpoint.port);

    aws_server_bootstrap_destroy_socket_listener(server->bootstrap, server->socket);

    /* wait for connections to finish shutting down
     * clean up will be called from eventloop */
}

/* At this point, the channel bootstrapper has established a connection to the server and set up a channel.
 * Now we need to create the aws_http_connection and insert it into the channel as a channel-handler. */
static void s_client_bootstrap_on_channel_setup(
    struct aws_client_bootstrap *channel_bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)channel_bootstrap;
    AWS_ASSERT(user_data);
    struct aws_http_client_bootstrap *http_bootstrap = user_data;

    /* Contract for setup callbacks is: channel is NULL if error_code is non-zero. */
    AWS_FATAL_ASSERT((error_code != 0) == (channel == NULL));

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Client connection failed with error %d (%s).",
            error_code,
            aws_error_name(error_code));

        /* Immediately tell user of failed connection.
         * No channel exists, so there will be no channel_shutdown callback. */
        http_bootstrap->on_setup(NULL, error_code, http_bootstrap->user_data);

        /* Clean up the http_bootstrap, it has no more work to do. */
        aws_mem_release(http_bootstrap->alloc, http_bootstrap);
        return;
    }

    AWS_LOGF_TRACE(AWS_LS_HTTP_CONNECTION, "static: Socket connected, creating client connection object.");

    http_bootstrap->connection = s_connection_new(
        http_bootstrap->alloc, channel, false, http_bootstrap->is_using_tls, http_bootstrap->initial_window_size);
    if (!http_bootstrap->connection) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create the client connection object, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    http_bootstrap->connection->proxy_request_transform = http_bootstrap->proxy_request_transform;
    http_bootstrap->connection->user_data = http_bootstrap->user_data;

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: " PRInSTR " client connection established.",
        (void *)http_bootstrap->connection,
        AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(http_bootstrap->connection->http_version)));

    /* Tell user of successful connection.
     * Then clear the on_setup callback so that we know it's been called */
    http_bootstrap->on_setup(http_bootstrap->connection, AWS_ERROR_SUCCESS, http_bootstrap->user_data);
    http_bootstrap->on_setup = NULL;

    return;

error:
    /* Something went wrong. Invoke channel shutdown. Then wait for channel shutdown to complete
     * before informing the user that setup failed and cleaning up the http_bootstrap.*/
    aws_channel_shutdown(channel, aws_last_error());
}

/* At this point, the channel for a client connection has completed its shutdown */
static void s_client_bootstrap_on_channel_shutdown(
    struct aws_client_bootstrap *channel_bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)channel_bootstrap;
    (void)channel;

    AWS_ASSERT(user_data);
    struct aws_http_client_bootstrap *http_bootstrap = user_data;

    /* If on_setup hasn't been called yet, inform user of failed setup.
     * If on_setup was already called, inform user that it's shut down now. */
    if (http_bootstrap->on_setup) {
        /* make super duper sure that failed setup receives a non-zero error_code */
        if (error_code == 0) {
            error_code = AWS_ERROR_UNKNOWN;
        }

        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Client setup failed with error %d (%s).",
            error_code,
            aws_error_name(error_code));

        http_bootstrap->on_setup(NULL, error_code, http_bootstrap->user_data);

    } else if (http_bootstrap->on_shutdown) {
        AWS_LOGF_INFO(
            AWS_LS_HTTP_CONNECTION,
            "%p: Client shutdown completed with error %d (%s).",
            (void *)http_bootstrap->connection,
            error_code,
            aws_error_name(error_code));

        http_bootstrap->on_shutdown(http_bootstrap->connection, error_code, http_bootstrap->user_data);
    }

    /* Clean up bootstrapper */
    aws_mem_release(http_bootstrap->alloc, http_bootstrap);
}

int aws_http_client_connect_internal(
    const struct aws_http_client_connection_options *options,
    aws_http_proxy_request_transform_fn *proxy_request_transform) {

    AWS_FATAL_ASSERT(options->proxy_options == NULL);

    struct aws_http_client_bootstrap *http_bootstrap = NULL;
    struct aws_string *host_name = NULL;
    int err = 0;

    if (!options || options->self_size == 0 || !options->allocator || !options->bootstrap ||
        options->host_name.len == 0 || !options->socket_options || !options->on_setup) {

        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "static: Invalid options, cannot create client connection.");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto error;
    }

    /* bootstrap_new() functions requires a null-terminated c-str */
    host_name = aws_string_new_from_array(options->allocator, options->host_name.ptr, options->host_name.len);
    if (!host_name) {
        goto error;
    }

    http_bootstrap = aws_mem_calloc(options->allocator, 1, sizeof(struct aws_http_client_bootstrap));
    if (!http_bootstrap) {
        goto error;
    }

    http_bootstrap->alloc = options->allocator;
    http_bootstrap->is_using_tls = options->tls_options != NULL;
    http_bootstrap->initial_window_size = options->initial_window_size;
    http_bootstrap->user_data = options->user_data;
    http_bootstrap->on_setup = options->on_setup;
    http_bootstrap->on_shutdown = options->on_shutdown;
    http_bootstrap->proxy_request_transform = proxy_request_transform;

    AWS_LOGF_TRACE(
        AWS_LS_HTTP_CONNECTION,
        "static: attempting to initialize a new client channel to %s:%d",
        aws_string_c_str(host_name),
        (int)options->port);

    if (options->tls_options) {
        err = s_system_vtable_ptr->new_tls_socket_channel(
            options->bootstrap,
            aws_string_c_str(host_name),
            options->port,
            options->socket_options,
            options->tls_options,
            s_client_bootstrap_on_channel_setup,
            s_client_bootstrap_on_channel_shutdown,
            http_bootstrap);
    } else {
        err = s_system_vtable_ptr->new_socket_channel(
            options->bootstrap,
            aws_string_c_str(host_name),
            options->port,
            options->socket_options,
            s_client_bootstrap_on_channel_setup,
            s_client_bootstrap_on_channel_shutdown,
            http_bootstrap);
    }

    if (err) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to initiate socket channel for new client connection, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        goto error;
    }

    aws_string_destroy(host_name);
    return AWS_OP_SUCCESS;

error:
    if (http_bootstrap) {
        aws_mem_release(http_bootstrap->alloc, http_bootstrap);
    }

    if (host_name) {
        aws_string_destroy(host_name);
    }

    return AWS_OP_ERR;
}

int aws_http_client_connect(const struct aws_http_client_connection_options *options) {
    aws_http_fatal_assert_library_initialized();

    if (options->proxy_options != NULL) {
        return aws_http_client_connect_via_proxy(options);
    } else {
        return aws_http_client_connect_internal(options, NULL);
    }
}

enum aws_http_version aws_http_connection_get_version(const struct aws_http_connection *connection) {
    return connection->http_version;
}

int aws_http_connection_configure_server(
    struct aws_http_connection *connection,
    const struct aws_http_server_connection_options *options) {

    if (!connection || !options || !options->on_incoming_request) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "id=%p: Invalid server configuration options.", (void *)connection);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (!connection->server_data) {
        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION,
            "id=%p: Server-only function invoked on client, ignoring call.",
            (void *)connection);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }
    if (connection->server_data->on_incoming_request) {
        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION, "id=%p: Connection is already configured, ignoring call.", (void *)connection);
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    connection->user_data = options->connection_user_data;
    connection->server_data->on_incoming_request = options->on_incoming_request;
    connection->server_data->on_shutdown = options->on_shutdown;

    return AWS_OP_SUCCESS;
}
