/*  lua-mhd -- Lua wrapper for libmicrohttpd

    Copyright (C) 2026 Предраг Јовановић
    SPDX-FileCopyrightText: 2026 Предраг Јовановић
    SPDX-License-Identifier: LGPL-3.0-or-later

    Look at the COPYING and COPYING.LESSER files for more information.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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

typedef struct LuaValue {
    int type;
    unsigned int length;
    union {
        char boolean;
        char *string;
        lua_Number number;
        void *udata;
    };
} LuaValue;

/*
--------------------------------------------------
Options table accepted as the first argument to
mhd.load / mhd.loadfile / mhd.start, in place of a
bare port number.
--------------------------------------------------
*/

typedef struct MhdOptions {
    int port;

    int thread_pool_size;
    int has_thread_pool_size;

    unsigned int connection_limit;
    int has_connection_limit;

    unsigned int connection_timeout;
    int has_connection_timeout;

    unsigned int per_ip_connection_limit;
    int has_per_ip_connection_limit;

    int single_thread;
    int debug;
    int ipv6;

    size_t max_body_size; /* 0 = unlimited */

    /* TLS: pointers alias into the options table's Lua strings and are
       only valid for the duration of the mhd.start/load/loadfile call. */
    int has_tls;
    const char *tls_key;
    const char *tls_cert;
    const char *tls_trust;        /* optional: CA / trust chain */
    const char *tls_key_password; /* optional */

    /* Unix domain socket, in place of a TCP port */
    const char *unix_socket;
    long unix_socket_mode; /* -1 = unset/don't chmod */
} MhdOptions;

typedef struct LuaMHDServer {
    struct MHD_Daemon *daemon;
    int argc;
    int port;
    char *script;
    size_t length;
    size_t max_body_size;
    char *unix_socket_path;

    LuaValue argv[];
} LuaMHDServer;

typedef struct Buffer {
    char *data;
    size_t length;
    size_t capacity;
} Buffer;

typedef struct Request {
    lua_State *state;
    struct MHD_Connection *connection;
    LuaMHDServer *server;
    Buffer buffer;
    size_t received; /* total body bytes seen so far (streaming or not) */
    int rejected;
    int streaming;     /* true if the worker registered an on_data handler */
    int req_table_ref; /* LUA_NOREF unless streaming */
    int handler_error; /* true if on_data raised a Lua error */
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

static int mhd_load_args(lua_State *L, LuaMHDServer *srv) {
    for (int i = 0; i < srv->argc; i++) {
        LuaValue *arg = &srv->argv[i];
        switch (arg->type) {
            /* clang-format off */
        case LUA_TNONE: case LUA_TNIL: lua_pushnil(L);                   break;
        case LUA_TBOOLEAN: lua_pushboolean(L, arg->boolean);             break;
        case LUA_TLIGHTUSERDATA: lua_pushlightuserdata(L, arg->udata);   break;
        case LUA_TNUMBER: lua_pushnumber(L, arg->number);                break;
        case LUA_TSTRING:  lua_pushlstring(L, arg->string, arg->length); break;
        default: return -1;
            /* clang-format on */
        }
    }

    return 0;
}

static lua_State *create_thread_state(LuaMHDServer *srv) {
    lua_State *L;

    if (!(L = luaL_newstate())) {
        fprintf(stderr, "[lua_mhd] luaL_newstate failed\n");
        return NULL;
    }

    luaL_openlibs(L);

    if (luaL_loadbuffer(L, srv->script, srv->length, "MHD Handler") != LUA_OK) {
        fprintf(
            stderr, "[lua_mhd] Failed to load script: %s\n", lua_tostring(L, -1)
        );

        lua_close(L);
        return NULL;
    }

    if (mhd_load_args(L, srv)) {
        fprintf(stderr, "[lua_mhd] Failed to load script arguments!\n");

        lua_close(L);
        return NULL;
    }

    if (lua_pcall(L, srv->argc, 1, 0) != LUA_OK) {
        fprintf(
            stderr,
            "[lua_mhd] Failed to pcall script: %s\n",
            lua_tostring(L, -1)
        );

        lua_close(L);
        return NULL;
    }

    /* The worker script may return either a bare handler function (the
       request body is fully buffered before it is called, as before), or
       a table { handle = fn, on_data = fn } where `on_data` is invoked
       once per incoming chunk so large bodies never need to be buffered
       in memory. */
    if (lua_isfunction(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "mhd_on_data");
        lua_setfield(L, LUA_REGISTRYINDEX, "mhd_handler");
    } else if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "handle");
        if (!lua_isfunction(L, -1)) {
            fprintf(stderr, "[lua_mhd] options.handle must be a function\n");
            lua_close(L);
            return NULL;
        }
        lua_setfield(L, LUA_REGISTRYINDEX, "mhd_handler");

        lua_getfield(L, -1, "on_data");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            lua_pushnil(L);
        }
        lua_setfield(L, LUA_REGISTRYINDEX, "mhd_on_data");

        lua_pop(L, 1); /* the options table itself */
    } else {
        fprintf(
            stderr,
            "[lua_mhd] Script must return a function or a table with a "
            "'handle' function\n"
        );
        lua_close(L);
        return NULL;
    }

    return L;
}

static lua_State *get_thread_state(LuaMHDServer *srv) {
    pthread_once(&tls_key_once, tls_init);

    lua_State *L = (lua_State *)pthread_getspecific(tls_key);

    if (!L && (L = create_thread_state(srv)))
        pthread_setspecific(tls_key, L);

    return L;
}

/*
------------------------------
libmicrohttpd request handling
------------------------------
*/

static enum MHD_Result req_simple_response(
    Request *req, unsigned int code, const char *msg
) {
    struct MHD_Response *res = MHD_create_response_from_buffer(
        strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT
    );

    enum MHD_Result ret = MHD_queue_response(req->connection, code, res);
    MHD_destroy_response(res);
    return ret;
}

static Request *req_new(LuaMHDServer *srv, struct MHD_Connection *conn) {
    Request *req = calloc(1, sizeof(Request));
    req->state = get_thread_state(srv);
    req->connection = conn;
    req->server = srv;
    req->req_table_ref = LUA_NOREF;

    if (!req->state) {
        req_simple_response(
            req,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "Internal Server Error: Lua initialization failed"
        );
        free(req);
        return NULL;
    }

    lua_getfield(req->state, LUA_REGISTRYINDEX, "mhd_on_data");
    req->streaming = lua_isfunction(req->state, -1);
    lua_pop(req->state, 1);

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
        if (req->state && req->req_table_ref != LUA_NOREF)
            luaL_unref(req->state, LUA_REGISTRYINDEX, req->req_table_ref);
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
    return len;
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

static int l_req_check_digest(lua_State *L) {
    Request *req = (Request *)lua_touserdata(L, lua_upvalueindex(1));
    const char *realm = luaL_checkstring(L, 1);
    const char *username = luaL_checkstring(L, 2);
    const char *password = luaL_checkstring(L, 3);

    unsigned int nonce_timeout = 0;
    enum MHD_DigestAuthMultiAlgo3 algo = MHD_DIGEST_AUTH_MULT_ALGO3_MD5;

    if (lua_istable(L, 4)) {
        lua_getfield(L, 4, "nonce_timeout");
        if (lua_isnumber(L, -1))
            nonce_timeout = (unsigned int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 4, "algorithm");
        if (lua_isstring(L, -1)) {
            const char *a = lua_tostring(L, -1);
            if (strcasecmp(a, "sha256") == 0)
                algo = MHD_DIGEST_AUTH_MULT_ALGO3_SHA256;
            else if (strcasecmp(a, "any") == 0)
                algo = MHD_DIGEST_AUTH_MULT_ALGO3_ANY_NON_SESSION;
        }
        lua_pop(L, 1);
    }

    enum MHD_DigestAuthResult r = MHD_digest_auth_check3(
        req->connection,
        realm,
        username,
        password,
        nonce_timeout,
        0,
        MHD_DIGEST_AUTH_MULT_QOP_AUTH,
        algo
    );

    if (r == MHD_DAUTH_OK)
        lua_pushboolean(L, 1);
    else if (r == MHD_DAUTH_NONCE_STALE)
        lua_pushliteral(L, "stale");
    else
        lua_pushboolean(L, 0);

    return 1;
}

/* Builds the request table (method/url/version/headers/query/auth helpers)
   on top of the Lua stack, leaving `body` as an empty string — callers fill
   that in themselves depending on whether the body is buffered or
   streamed. */
static void req_build_common(
    Request *req, const char *url, const char *method, const char *version
) {
    lua_State *L = req->state;

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

    lua_newtable(L); /* query params */
    MHD_get_connection_values(
        req->connection, MHD_GET_ARGUMENT_KIND, header_iter, L
    );
    lua_setfield(L, -2, "query");

    lua_pushstring(L, "");
    lua_setfield(L, -2, "body");

    /* HTTP Basic authentication, parsed for convenience. */
    struct MHD_BasicAuthInfo *basic = MHD_basic_auth_get_username_password3(
        req->connection
    );
    if (basic) {
        lua_newtable(L);
        lua_pushlstring(L, basic->username, basic->username_len);
        lua_setfield(L, -2, "username");
        if (basic->password)
            lua_pushlstring(L, basic->password, basic->password_len);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "password");
        lua_setfield(L, -2, "basic_auth");
        MHD_free(basic);
    }

    /* HTTP Digest authentication: the client-supplied username is exposed
       so the script can look up the matching password; verification then
       happens via req.check_digest(realm, username, password). */
    struct MHD_DigestAuthUsernameInfo *digest = MHD_digest_auth_get_username3(
        req->connection
    );
    if (digest) {
        if (digest->username)
            lua_pushlstring(L, digest->username, digest->username_len);
        else
            lua_pushnil(L);
        lua_setfield(L, -2, "digest_username");
        MHD_free(digest);
    }

    lua_pushlightuserdata(L, req);
    lua_pushcclosure(L, l_req_check_digest, 1);
    lua_setfield(L, -2, "check_digest");
}

/* Non-streaming path: build the request table (with the fully-buffered
   body) and push the handler function ready for a single lua_pcall. */
static int req_push(
    Request *req, const char *url, const char *method, const char *version
) {
    lua_State *L = req->state;
    lua_getfield(L, LUA_REGISTRYINDEX, "mhd_handler");

    req_build_common(req, url, method, version);

    if (req->buffer.length > 0) {
        lua_pushlstring(L, req->buffer.data, req->buffer.length);
        lua_setfield(L, -2, "body");
    }

    return MHD_YES;
}

/* Streaming path: build the request table once, up front (headers/query
   are already available before any body data arrives), and keep a
   registry reference to it so on_data() can be called repeatedly with it
   as chunks arrive, without rebuilding it each time. */
static void req_build_table(
    Request *req, const char *url, const char *method, const char *version
) {
    lua_State *L = req->state;
    req_build_common(req, url, method, version);
    req->req_table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Calls the worker's on_data(req, chunk) for one incoming chunk. Returns
   0 on success, -1 if the callback raised a Lua error. On error, no
   response is queued here — the caller must defer that until the body is
   fully drained, since queuing a response mid-upload can hang the
   connection with some libmicrohttpd versions. */
static int req_on_data(Request *req, const char *chunk, size_t len) {
    lua_State *L = req->state;

    lua_getfield(L, LUA_REGISTRYINDEX, "mhd_on_data");
    lua_rawgeti(L, LUA_REGISTRYINDEX, req->req_table_ref);
    lua_pushlstring(L, chunk, len);

    if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(
            stderr, "[lua_mhd] on_data error: %s\n", err ? err : "(unknown)"
        );
        lua_pop(L, 1);
        return -1;
    }

    return 0;
}

static int req_handle(Request *req) {
    lua_State *L = req->state;

    if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        fprintf(stderr, "[lua_mhd] %s\n", err ? err : "(unknown)");

        req_simple_response(
            req,
            MHD_HTTP_INTERNAL_SERVER_ERROR,
            "Internal Script Error: An error occured in the Lua handler "
            "function"
        );

        lua_pop(L, 1); /* pop the error message */
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

/* Finishes a response: queues it normally, or — if the handler set a
   `digest_challenge` table — turns it into an RFC 7616 challenge instead. */
static enum MHD_Result res_queue(
    Request *req, struct MHD_Response *res, unsigned int code
) {
    lua_State *L = req->state;
    enum MHD_Result ret;

    lua_getfield(L, -1, "digest_challenge");
    if (lua_istable(L, -1)) {
        int idx = lua_gettop(L);

        lua_getfield(L, idx, "realm");
        const char *realm = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";

        lua_getfield(L, idx, "opaque");
        const char *opaque = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;

        lua_getfield(L, idx, "stale");
        int stale = lua_toboolean(L, -1);

        lua_getfield(L, idx, "algorithm");
        enum MHD_DigestAuthMultiAlgo3 algo = MHD_DIGEST_AUTH_MULT_ALGO3_MD5;
        if (lua_isstring(L, -1)) {
            const char *a = lua_tostring(L, -1);
            if (strcasecmp(a, "sha256") == 0)
                algo = MHD_DIGEST_AUTH_MULT_ALGO3_SHA256;
            else if (strcasecmp(a, "any") == 0)
                algo = MHD_DIGEST_AUTH_MULT_ALGO3_ANY_NON_SESSION;
        }

        ret = MHD_queue_auth_required_response3(
            req->connection,
            realm,
            opaque,
            NULL,
            res,
            stale,
            MHD_DIGEST_AUTH_MULT_QOP_AUTH,
            algo,
            0,
            0
        );

        lua_pop(L, 5); /* algorithm, stale, opaque, realm, digest_challenge */
        MHD_destroy_response(res);
        return ret;
    }
    lua_pop(L, 1); /* digest_challenge (nil) */

    ret = MHD_queue_response(req->connection, code, res);
    MHD_destroy_response(res);
    return ret;
}

typedef struct StreamCtx {
    lua_State *L;
    int ref;
    char *pending;
    size_t pending_len;
    size_t pending_off;
    int done;
} StreamCtx;

static ssize_t stream_reader(void *cls, uint64_t pos, char *buf, size_t max) {
    (void)pos;
    StreamCtx *ctx = (StreamCtx *)cls;
    lua_State *L = ctx->L;

    if (ctx->pending_off >= ctx->pending_len) {
        free(ctx->pending);
        ctx->pending = NULL;
        ctx->pending_len = 0;
        ctx->pending_off = 0;

        if (ctx->done)
            return MHD_CONTENT_READER_END_OF_STREAM;

        lua_rawgeti(L, LUA_REGISTRYINDEX, ctx->ref);

        if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            fprintf(
                stderr, "[lua_mhd] stream error: %s\n", err ? err : "(unknown)"
            );
            lua_pop(L, 1);
            ctx->done = 1;
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        size_t len;
        const char *chunk = lua_tolstring(L, -1, &len);

        if (!chunk || len == 0) {
            lua_pop(L, 1);
            ctx->done = 1;
            return MHD_CONTENT_READER_END_OF_STREAM;
        }

        ctx->pending = malloc(len);
        if (!ctx->pending) {
            lua_pop(L, 1);
            ctx->done = 1;
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        memcpy(ctx->pending, chunk, len);
        ctx->pending_len = len;
        ctx->pending_off = 0;
        lua_pop(L, 1);
    }

    size_t avail = ctx->pending_len - ctx->pending_off;
    size_t n = avail < max ? avail : max;
    memcpy(buf, ctx->pending + ctx->pending_off, n);
    ctx->pending_off += n;
    return (ssize_t)n;
}

static void stream_free(void *cls) {
    StreamCtx *ctx = (StreamCtx *)cls;
    if (!ctx)
        return;
    luaL_unref(ctx->L, LUA_REGISTRYINDEX, ctx->ref);
    free(ctx->pending);
    free(ctx);
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

    /* Streaming responses: { code = 200, stream = function() ... end }
       lets the script produce the body incrementally instead of building
       the whole thing as one Lua string up front. The function is called
       repeatedly; returning nil/false/"" ends the response. */
    lua_getfield(L, -1, "stream");
    if (lua_isfunction(L, -1)) {
        StreamCtx *ctx = calloc(1, sizeof(StreamCtx));
        ctx->L = L;
        ctx->ref = luaL_ref(L, LUA_REGISTRYINDEX); /* pops the function */

        uint64_t size = MHD_SIZE_UNKNOWN;
        lua_getfield(L, -1, "stream_size");
        if (lua_isnumber(L, -1))
            size = (uint64_t)lua_tointeger(L, -1);
        lua_pop(L, 1);

        struct MHD_Response *res = MHD_create_response_from_callback(
            size, 8192, stream_reader, ctx, stream_free
        );

        if (!res) {
            stream_free(ctx);
            return MHD_NO;
        }

        res_add_headers(req, res);
        return res_queue(req, res, code);
    }
    lua_pop(L, 1); /* stream (nil or non-function) */

    /* Static file serving: { code = 200, file = "/path/to/file" } lets MHD
       stream the file directly (via sendfile where supported) instead of
       the script reading it into a Lua string. */
    lua_getfield(L, -1, "file");
    if (lua_isstring(L, -1)) {
        const char *path = lua_tostring(L, -1);
        int fd = open(path, O_RDONLY);
        lua_pop(L, 1); /* file */

        if (fd < 0)
            return req_simple_response(req, MHD_HTTP_NOT_FOUND, "Not Found");

        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd);
            return req_simple_response(req, MHD_HTTP_NOT_FOUND, "Not Found");
        }

        struct MHD_Response *res = MHD_create_response_from_fd64(
            (uint64_t)st.st_size, fd
        );

        if (!res) {
            close(fd);
            return MHD_NO;
        }

        res_add_headers(req, res);
        return res_queue(req, res, code);
    }
    lua_pop(L, 1); /* file (nil) */

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
    return res_queue(req, res, code);
}

/* Finishes a streamed request: fetches the handler and the request table
   built earlier by req_build_table, releases the registry ref, and calls
   the handler exactly like the non-streaming path does. */
static enum MHD_Result req_finish_streaming(Request *req) {
    lua_State *L = req->state;

    lua_getfield(L, LUA_REGISTRYINDEX, "mhd_handler");
    lua_rawgeti(L, LUA_REGISTRYINDEX, req->req_table_ref);

    luaL_unref(L, LUA_REGISTRYINDEX, req->req_table_ref);
    req->req_table_ref = LUA_NOREF;

    if (req_handle(req))
        return MHD_YES;

    enum MHD_Result ret = res_send(req);
    lua_pop(L, 1); /* pop the response table left by req_handle's pcall */
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

        if (req && req->streaming)
            req_build_table(req, url, method, version);

        return MHD_YES;
    }

    if (*upload_data_size > 0) {
        if (!req->rejected && srv->max_body_size > 0
            && req->received + *upload_data_size > srv->max_body_size)
            req->rejected = 1;

        if (req->rejected) {
            *upload_data_size = 0;
            return MHD_YES;
        }

        if (req->streaming) {
            if (req_on_data(req, upload_data, *upload_data_size)) {
                req->rejected = 1;
                req->handler_error = 1;
                *upload_data_size = 0;
                return MHD_YES;
            }
            req->received += *upload_data_size;
            *upload_data_size = 0;
            return MHD_YES;
        }

        size_t recv = buffer_append(
            &req->buffer, upload_data, *upload_data_size
        );
        req->received += recv;
        *upload_data_size -= recv;
        return MHD_YES;
    }

    if (req->rejected) {
        if (req->handler_error)
            return req_simple_response(
                req,
                MHD_HTTP_INTERNAL_SERVER_ERROR,
                "Internal Script Error: An error occured in the Lua "
                "on_data handler"
            );
        return req_simple_response(
            req, MHD_HTTP_CONTENT_TOO_LARGE, "Request body too large"
        );
    }

    if (req->streaming)
        return req_finish_streaming(req);

    req_push(req, url, method, version);

    if (req_handle(req))
        return MHD_YES;

    enum MHD_Result ret = res_send(req);
    lua_pop(req->state, 1); /* pop the response table left by req_handle */
    return ret;
}

/*
--------------------------------------------------------------------
The handler script must be loaded separately from the main Lua state
--------------------------------------------------------------------
*/

static void opt_number_field(
    lua_State *L, int idx, const char *name, int *has, long *out
) {
    lua_getfield(L, idx, name);
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1))
            luaL_error(L, "options.%s must be a number", name);
        *out = (long)lua_tonumber(L, -1);
        *has = 1;
    }
    lua_pop(L, 1);
}

static int opt_bool_field(lua_State *L, int idx, const char *name) {
    lua_getfield(L, idx, name);
    int truthy = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return truthy;
}

static void parse_options(lua_State *L, int idx, MhdOptions *opts) {
    long tmp;
    memset(opts, 0, sizeof(*opts));
    opts->unix_socket_mode = -1;

    luaL_checktype(L, idx, LUA_TTABLE);

    lua_getfield(L, idx, "unix_socket");
    if (lua_isstring(L, -1))
        opts->unix_socket = lua_tostring(L, -1);
    else if (!lua_isnil(L, -1))
        luaL_error(L, "options.unix_socket must be a string");
    lua_pop(L, 1);

    lua_getfield(L, idx, "port");
    if (!lua_isnil(L, -1)) {
        if (!lua_isinteger(L, -1) && !lua_isnumber(L, -1))
            luaL_error(L, "options.port must be a number");
        opts->port = (int)lua_tointeger(L, -1);
    } else if (!opts->unix_socket) {
        luaL_error(
            L, "options.port is required unless options.unix_socket is given"
        );
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "unix_socket_mode");
    if (lua_isnumber(L, -1))
        opts->unix_socket_mode = (long)lua_tointeger(L, -1);
    lua_pop(L, 1);

    opt_number_field(
        L, idx, "thread_pool_size", &opts->has_thread_pool_size, &tmp
    );
    if (opts->has_thread_pool_size)
        opts->thread_pool_size = (int)tmp;

    opt_number_field(
        L, idx, "connection_limit", &opts->has_connection_limit, &tmp
    );
    if (opts->has_connection_limit)
        opts->connection_limit = (unsigned int)tmp;

    opt_number_field(
        L, idx, "connection_timeout", &opts->has_connection_timeout, &tmp
    );
    if (opts->has_connection_timeout)
        opts->connection_timeout = (unsigned int)tmp;

    opt_number_field(
        L,
        idx,
        "per_ip_connection_limit",
        &opts->has_per_ip_connection_limit,
        &tmp
    );
    if (opts->has_per_ip_connection_limit)
        opts->per_ip_connection_limit = (unsigned int)tmp;

    int has_max_body = 0;
    opt_number_field(L, idx, "max_body_size", &has_max_body, &tmp);
    if (has_max_body) {
        if (tmp < 0)
            luaL_error(L, "options.max_body_size must not be negative");
        opts->max_body_size = (size_t)tmp;
    }

    opts->single_thread = opt_bool_field(L, idx, "single_thread");
    opts->debug = opt_bool_field(L, idx, "debug");
    opts->ipv6 = opt_bool_field(L, idx, "ipv6");

    if (opts->has_thread_pool_size && opts->single_thread)
        luaL_error(
            L,
            "options.thread_pool_size and options.single_thread are "
            "mutually exclusive"
        );

    /* TLS */
    lua_getfield(L, idx, "tls");
    if (lua_istable(L, -1)) {
        int tlsIdx = lua_gettop(L);

        lua_getfield(L, tlsIdx, "key");
        if (lua_isstring(L, -1))
            opts->tls_key = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, tlsIdx, "cert");
        if (lua_isstring(L, -1))
            opts->tls_cert = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, tlsIdx, "trust");
        if (lua_isstring(L, -1))
            opts->tls_trust = lua_tostring(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, tlsIdx, "key_password");
        if (lua_isstring(L, -1))
            opts->tls_key_password = lua_tostring(L, -1);
        lua_pop(L, 1);

        if (!opts->tls_key || !opts->tls_cert)
            luaL_error(
                L, "options.tls requires both 'key' and 'cert' PEM strings"
            );

        opts->has_tls = 1;
    } else if (!lua_isnil(L, -1)) {
        luaL_error(L, "options.tls must be a table");
    }
    lua_pop(L, 1); /* tls */
}

static int create_unix_socket(const char *path, long mode) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    unlink(path); /* ignore errors: the path may simply not exist yet */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    if (mode >= 0)
        chmod(path, (mode_t)mode);

    if (listen(fd, SOMAXCONN) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }

    return fd;
}

static struct MHD_Daemon *mhd_start(MhdOptions *opts, void *user) {
    unsigned int flags = MHD_USE_INTERNAL_POLLING_THREAD;

    if (opts->ipv6)
        flags |= MHD_USE_IPv6;
    if (opts->debug)
        flags |= MHD_USE_DEBUG;
    if (opts->has_tls)
        flags |= MHD_USE_TLS;

    /* Thread-per-connection is our default threading model, since worker
       state is kept in thread-local storage. A thread pool or a single
       internal thread are also compatible with that model, but cannot be
       combined with MHD_USE_THREAD_PER_CONNECTION. */
    if (!opts->has_thread_pool_size && !opts->single_thread)
        flags |= MHD_USE_THREAD_PER_CONNECTION;

    int listen_fd = -1;
    if (opts->unix_socket) {
        listen_fd = create_unix_socket(
            opts->unix_socket, opts->unix_socket_mode
        );
        if (listen_fd < 0)
            return NULL;
    }

    struct MHD_OptionItem items[12];
    int n = 0;

    items[n].option = MHD_OPTION_NOTIFY_COMPLETED;
    items[n].value = (intptr_t)req_free;
    items[n].ptr_value = NULL;
    n++;

    if (opts->has_thread_pool_size) {
        items[n].option = MHD_OPTION_THREAD_POOL_SIZE;
        items[n].value = (intptr_t)opts->thread_pool_size;
        items[n].ptr_value = NULL;
        n++;
    }

    if (opts->has_connection_limit) {
        items[n].option = MHD_OPTION_CONNECTION_LIMIT;
        items[n].value = (intptr_t)opts->connection_limit;
        items[n].ptr_value = NULL;
        n++;
    }

    if (opts->has_connection_timeout) {
        items[n].option = MHD_OPTION_CONNECTION_TIMEOUT;
        items[n].value = (intptr_t)opts->connection_timeout;
        items[n].ptr_value = NULL;
        n++;
    }

    if (opts->has_per_ip_connection_limit) {
        items[n].option = MHD_OPTION_PER_IP_CONNECTION_LIMIT;
        items[n].value = (intptr_t)opts->per_ip_connection_limit;
        items[n].ptr_value = NULL;
        n++;
    }

    if (opts->has_tls) {
        items[n].option = MHD_OPTION_HTTPS_MEM_KEY;
        items[n].value = 0;
        items[n].ptr_value = (void *)opts->tls_key;
        n++;

        items[n].option = MHD_OPTION_HTTPS_MEM_CERT;
        items[n].value = 0;
        items[n].ptr_value = (void *)opts->tls_cert;
        n++;

        if (opts->tls_trust) {
            items[n].option = MHD_OPTION_HTTPS_MEM_TRUST;
            items[n].value = 0;
            items[n].ptr_value = (void *)opts->tls_trust;
            n++;
        }

        if (opts->tls_key_password) {
            items[n].option = MHD_OPTION_HTTPS_KEY_PASSWORD;
            items[n].value = 0;
            items[n].ptr_value = (void *)opts->tls_key_password;
            n++;
        }
    }

    if (listen_fd >= 0) {
        items[n].option = MHD_OPTION_LISTEN_SOCKET;
        items[n].value = (intptr_t)listen_fd;
        items[n].ptr_value = NULL;
        n++;
    }

    items[n].option = MHD_OPTION_END;
    items[n].value = 0;
    items[n].ptr_value = NULL;
    n++;

    struct MHD_Daemon *daemon = MHD_start_daemon(
        flags,
        (uint16_t)opts->port,
        NULL,
        NULL,
        access_handler,
        user,
        MHD_OPTION_ARRAY,
        items,
        MHD_OPTION_END
    );

    if (!daemon && listen_fd >= 0) {
        close(listen_fd);
        unlink(opts->unix_socket);
    }

    return daemon;
}

static int mhd_save_args(lua_State *L, LuaMHDServer *srv, int idx, int argc) {
    for (int i = 0; i < argc; i++) {
        LuaValue *out = &srv->argv[i];
        switch (lua_type(L, idx + i)) {
        case LUA_TNONE:
        case LUA_TNIL:
            out->type = LUA_TNIL;
            break;
        case LUA_TBOOLEAN:
            out->type = LUA_TBOOLEAN;
            out->boolean = lua_toboolean(L, idx + i);
            break;
        case LUA_TLIGHTUSERDATA:
            out->type = LUA_TLIGHTUSERDATA;
            out->udata = lua_touserdata(L, idx + i);
            break;
        case LUA_TNUMBER:
            out->type = LUA_TNUMBER;
            out->number = lua_tonumber(L, idx + i);
            break;
        case LUA_TSTRING: {
            size_t length;
            const char *string = lua_tolstring(L, idx + i, &length);

            if (length > UINT_MAX)
                return -1;

            out->type = LUA_TSTRING;
            out->length = length;
            out->string = strdup(string);
            break;
        }
        case LUA_TTABLE:
        case LUA_TFUNCTION:
        case LUA_TUSERDATA:
        case LUA_TTHREAD:
        default:
            return -1;
        }
    }

    return 0;
}

static void mhd_free(LuaMHDServer *srv) {
    if (srv->daemon) {
        MHD_stop_daemon(srv->daemon);
        srv->daemon = NULL;
    }

    if (srv->unix_socket_path) {
        unlink(srv->unix_socket_path);
        free(srv->unix_socket_path);
        srv->unix_socket_path = NULL;
    }

    for (int i = 0; i < srv->argc; i++) {
        if (srv->argv[i].type == LUA_TSTRING) {
            srv->argv[i].type = LUA_TNIL;
            free(srv->argv[i].string);
        }
    }

    free(srv->script);
    srv->script = NULL;
}

static int mhd_wrap(
    lua_State *L, int idx, MhdOptions *opts, char *script, size_t length
) {
    int argc = lua_gettop(L) - (idx - 1);
    size_t size = sizeof(LuaMHDServer) + argc * sizeof(LuaValue);

    LuaMHDServer *srv = lua_newuserdata(L, size);
    memset(srv, 0, size);
    srv->port = opts->port;
    srv->script = script;
    srv->length = length;
    srv->max_body_size = opts->max_body_size;
    srv->unix_socket_path = opts->unix_socket ? strdup(opts->unix_socket)
                                              : NULL;
    srv->daemon = mhd_start(opts, srv);

    luaL_getmetatable(L, LUA_MHD_SERVER);
    lua_setmetatable(L, -2);

    if (!srv->daemon || mhd_save_args(L, srv, idx, argc)) {
        mhd_free(srv);
        if (opts->unix_socket)
            return luaL_error(
                L,
                "MHD_start_daemon failed on unix socket '%s'",
                opts->unix_socket
            );
        return luaL_error(L, "MHD_start_daemon failed on port %d", srv->port);
    }

    return 1;
}

static int l_mhd_load(lua_State *L) {
    size_t length;
    MhdOptions opts;
    parse_options(L, 1, &opts);
    const char *script = luaL_checklstring(L, 2, &length);
    return mhd_wrap(L, 3, &opts, strdup(script), length);
}

static int l_mhd_loadfile(lua_State *L) {
    MhdOptions opts;
    parse_options(L, 1, &opts);
    const char *path = luaL_checkstring(L, 2);
    FILE *f;
    char *buf;

    if (!(f = fopen(path, "rb")))
        return luaL_error(L, "Unable to open file at '%s'.", path);

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (!(buf = malloc(size))) {
        fclose(f);
        return luaL_error(L, "Out of memory!");
    }

    size_t len = fread(buf, 1, size, f);
    fclose(f);
    return mhd_wrap(L, 3, &opts, buf, len);
}

static int func_writer(lua_State *L, const void *p, size_t sz, void *ud) {
    (void)L;
    Buffer *buf = (Buffer *)ud;
    buffer_append(buf, p, sz);
    return 0;
}

static int l_mhd_start(lua_State *L) {
    if (!lua_isfunction(L, 2))
        return luaL_error(L, "Expected a function to start the MHD server!");

    MhdOptions opts;
    parse_options(L, 1, &opts);

    Buffer buf;
    buf.data = NULL;
    buf.length = 0;
    buf.capacity = 0;

    lua_pushvalue(L, 2);
    int rc = lua_dump(L, func_writer, &buf, 0);
    lua_pop(L, 1);

    if (rc != 0) {
        free(buf.data);
        return luaL_error(L, "Failed to dump worker function (error %d)", rc);
    }

    return mhd_wrap(L, 3, &opts, buf.data, buf.length);
}

static int server_stop(lua_State *L) {
    LuaMHDServer *srv = luaL_checkudata(L, 1, LUA_MHD_SERVER);

    if (srv->daemon) {
        MHD_stop_daemon(srv->daemon);
        srv->daemon = NULL;
    }

    if (srv->unix_socket_path) {
        unlink(srv->unix_socket_path);
        free(srv->unix_socket_path);
        srv->unix_socket_path = NULL;
    }

    return 0;
}

static int server_gc(lua_State *L) {
    LuaMHDServer *srv = luaL_checkudata(L, 1, LUA_MHD_SERVER);
    mhd_free(srv);
    return 0;
}

static int server_tostring(lua_State *L) {
    LuaMHDServer *srv = luaL_checkudata(L, 1, LUA_MHD_SERVER);
    lua_pushfstring(
        L,
        "lua_mhd.Server(port=%d, running=%s)",
        srv->port,
        srv->daemon ? "true" : "false"
    );
    return 1;
}

static const luaL_Reg serverMethods[] = {
    { "stop", server_stop },
    { "__gc", server_gc },
    { "__tostring", server_tostring },
    { NULL, NULL },
};

static const luaL_Reg mhdMethods[] = {
    { "start", l_mhd_start },
    { "load", l_mhd_load },
    { "loadfile", l_mhd_loadfile },
    { NULL, NULL },
};

int luaopen_mhd(lua_State *L) {
    luaL_newmetatable(L, LUA_MHD_SERVER);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, serverMethods, 0);
    lua_pop(L, 1);

    luaL_newlib(L, mhdMethods);
    char version[9];
    snprintf(version, sizeof(version), "%08x", (unsigned int)MHD_VERSION);
    lua_pushstring(L, version);
    lua_setfield(L, -2, "version");

    return 1;
}
