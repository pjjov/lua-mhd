/*  lua-mhd -- Lua wrapper for libmicrohttpd

    Copyright (C) 2026 Предраг Јовановић
    SPDX-FileCopyrightText: 2026 Предраг Јовановић
    SPDX-License-Identifier: LGPL-3.0-or-later

    Look at the COPYING and COPYING.LESSER files for more information.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <microhttpd.h>
#include <pthread.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#if MHD_VERSION >= 0x00097002
typedef enum MHD_Result MHD_Result_t;
#else
typedef int MHD_Result_t;
    #ifndef MHD_YES
        #define MHD_YES 1
        #define MHD_NO 0
    #endif
#endif

#define LUA_MHD_SERVER "lua_mhd.Server"

typedef struct LuaMHDServer {
    struct MHD_Daemon *daemon;
    int port;
    char *script;
    size_t length;
} LuaMHDServer;

typedef struct Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct Request {
    lua_State *state;
    struct MHD_Connection *connection;
    Buffer buffer;
} Request;

/*
---------------------------------------
Each worker thread creates a Lua state.
---------------------------------------
*/

static pthread_key_t tls_key;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;

static void tls_destructor(void *ptr) {
    if (ptr)
        lua_close((lua_State *)ptr);
}

static void tls_init(void) {
    pthread_key_t *key = &tls_key;
    pthread_key_create(key, tls_destructor);
}

static lua_State *create_thread_state(const char *script, size_t length) {
    lua_State *L;

    if (!(L = luaL_newstate())) {
        fprintf(stderr, "[lua_mhd] luaL_newstate failed\n");
        return NULL;
    }

    luaL_openlibs(L);

    if (luaL_loadbuffer(L, script, length, "MHD Handler") != LUA_OK) {
        fprintf(
            stderr, "[lua_mhd] Failed to load script: %s\n", lua_tostring(L, -1)
        );

        lua_close(L);
        return NULL;
    }

    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        fprintf(
            stderr,
            "[lua_mhd] Failed to pcall script: %s\n",
            lua_tostring(L, -1)
        );

        lua_close(L);
        return NULL;
    }

    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "[lua_mhd] Script must return a function\n");
        lua_close(L);
        return NULL;
    }

    lua_setfield(L, LUA_REGISTRYINDEX, "mhd_handler");
    return L;
}

static lua_State *get_thread_state(const char *script, size_t length) {
    pthread_once(&tls_key_once, tls_init);

    lua_State *L = (lua_State *)pthread_getspecific(tls_key);

    if (!L && (L = create_thread_state(script, length)))
        pthread_setspecific(tls_key, L);

    return L;
}

/*
------------------------------
libmicrohttpd request handling
------------------------------
*/

static void req_error(Request *req, const char *msg) {
    struct MHD_Response *res = MHD_create_response_from_buffer(
        strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT
    );

    MHD_queue_response(req->connection, MHD_HTTP_INTERNAL_SERVER_ERROR, res);
    MHD_destroy_response(res);
}

static Request *req_new(LuaMHDServer *srv, struct MHD_Connection *conn) {
    Request *req = calloc(1, sizeof(Request));
    req->state = get_thread_state(srv->script, srv->length);
    req->connection = conn;

    if (!req->state) {
        req_error(req, "Internal Server Error: Lua initialization failed");
        free(req);
        return NULL;
    }

    return req;
}

static void req_free(
    void *cls,
    struct MHD_Connection *connection,
    void **con_cls,
    enum MHD_RequestTerminationCode toe
) {
    (void)cls;
    (void)connection;
    (void)toe;
    if (*con_cls) {
        Request *req = *con_cls;
        free(req->buffer.data);
        free(req);
        *con_cls = NULL;
    }
}

static size_t buffer_append(Buffer *dst, const char *src, size_t len) {
    if (len == 0)
        return 0;

    if (dst->length + len > dst->capacity) {
        size_t cap = dst->capacity ? dst->capacity * 2 : 256;
        while (cap < dst->length + len)
            cap *= 2;

        void *newBuffer = realloc(dst->data, cap);

        if (!newBuffer)
            return 0;

        dst->data = newBuffer;
        dst->capacity = cap;
    }

    memcpy(dst->data + dst->length, src, len);
    dst->length += len;
    return 0;
}

static MHD_Result_t header_iter(
    void *cls, enum MHD_ValueKind kind, const char *key, const char *value
) {
    (void)kind;
    lua_State *L = (lua_State *)cls;
    lua_pushstring(L, key ? key : "");
    lua_pushstring(L, value ? value : "");
    lua_settable(L, -3);
    return MHD_YES;
}

static int req_push(
    Request *req, const char *url, const char *method, const char *version
) {
    lua_State *L = req->state;
    lua_getfield(L, LUA_REGISTRYINDEX, "mhd_handler");

    lua_newtable(L); /* request table */
    lua_pushstring(L, method ? method : "");
    lua_setfield(L, -2, "method");
    lua_pushstring(L, url ? url : "");
    lua_setfield(L, -2, "url");
    lua_pushstring(L, version ? version : "");
    lua_setfield(L, -2, "version");

    lua_newtable(L); /* headers */
    MHD_get_connection_values(req->connection, MHD_HEADER_KIND, header_iter, L);
    lua_setfield(L, -2, "headers");

    if (req->buffer.length > 0)
        lua_pushlstring(L, req->buffer.data, req->buffer.length);
    else
        lua_pushstring(L, "");

    lua_setfield(L, -2, "body");
    return MHD_YES;
}

static int req_handle(Request *req) {
    lua_State *L = req->state;

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "[lua_mhd] %s\n", err ? err : "(unknown)");

        req_error(
            req,
            "Internal Script Error: An error occured in the Lua handler "
            "function"
        );

        return -1;
    }

    return 0;
}

static void res_add_headers(Request *req, struct MHD_Response *res) {
    lua_State *L = req->state;
    lua_getfield(L, -1, "headers");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING
                && lua_type(L, -1) == LUA_TSTRING) {
                const char *key = lua_tostring(L, -2);
                const char *value = lua_tostring(L, -1);
                MHD_add_response_header(res, key, value);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
}

static enum MHD_Result res_send(Request *req) {
    lua_State *L = req->state;
    lua_getfield(L, -1, "code");

    if (!lua_isinteger(L, -1) && !lua_isnumber(L, -1)) {
        lua_pop(L, 1);
        return MHD_NO;
    }

    unsigned int code = (unsigned int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "body");
    size_t length = 0;
    const char *body = lua_tolstring(L, -1, &length);
    if (!body)
        body = "";

    struct MHD_Response *res = MHD_create_response_from_buffer(
        length, (void *)body, MHD_RESPMEM_MUST_COPY
    );
    lua_pop(L, 1);

    if (!res)
        return MHD_NO;

    res_add_headers(req, res);
    enum MHD_Result ret = MHD_queue_response(req->connection, code, res);
    MHD_destroy_response(res);
    return ret;
}

static MHD_Result_t access_handler(
    void *cls,
    struct MHD_Connection *connection,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **con_cls
) {
    LuaMHDServer *srv = (LuaMHDServer *)cls;
    Request *req = *con_cls;

    if (req == NULL) {
        req = req_new(srv, connection);
        *con_cls = req;
        return MHD_YES;
    }

    if (*upload_data_size > 0) {
        size_t recv = buffer_append(
            &req->buffer, upload_data, *upload_data_size
        );
        *upload_data_size -= recv;
        return MHD_YES;
    }

    req_push(req, url, method, version);

    if (req_handle(req))
        return MHD_YES;

    return res_send(req);
}
