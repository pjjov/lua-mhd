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

typedef struct LuaMHDServer {
    struct MHD_Daemon *daemon;
    int argc;
    int port;
    char *script;
    size_t length;

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

    if (!lua_isfunction(L, -1)) {
        fprintf(stderr, "[lua_mhd] Script must return a function\n");
        lua_close(L);
        return NULL;
    }

    lua_setfield(L, LUA_REGISTRYINDEX, "mhd_handler");
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

static void req_error(Request *req, const char *msg) {
    struct MHD_Response *res = MHD_create_response_from_buffer(
        strlen(msg), (void *)msg, MHD_RESPMEM_PERSISTENT
    );

    MHD_queue_response(req->connection, MHD_HTTP_INTERNAL_SERVER_ERROR, res);
    MHD_destroy_response(res);
}

static Request *req_new(LuaMHDServer *srv, struct MHD_Connection *conn) {
    Request *req = calloc(1, sizeof(Request));
    req->state = get_thread_state(srv);
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

/*
--------------------------------------------------------------------
The handler script must be loaded separately from the main Lua state
--------------------------------------------------------------------
*/

static struct MHD_Daemon *mhd_start(int port, void *user) {
    return MHD_start_daemon(
        MHD_USE_THREAD_PER_CONNECTION | MHD_USE_INTERNAL_POLLING_THREAD,
        (uint16_t)port,
        NULL,
        NULL,
        access_handler,
        user,
        MHD_OPTION_NOTIFY_COMPLETED,
        req_free,
        NULL,
        MHD_OPTION_END
    );
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
    lua_State *L, int idx, int port, char *script, size_t length
) {
    int argc = lua_gettop(L) - (idx - 1);
    size_t size = sizeof(LuaMHDServer) + argc * sizeof(LuaValue);

    LuaMHDServer *srv = lua_newuserdata(L, size);
    memset(srv, 0, size);
    srv->port = port;
    srv->script = script;
    srv->length = length;
    srv->daemon = mhd_start(port, srv);

    luaL_getmetatable(L, LUA_MHD_SERVER);
    lua_setmetatable(L, -2);

    if (!srv->daemon || mhd_save_args(L, srv, idx, argc)) {
        mhd_free(srv);
        return luaL_error(L, "MHD_start_daemon failed on port %d", srv->port);
    }

    return 1;
}

static int l_mhd_load(lua_State *L) {
    size_t length;
    int port = (int)luaL_checkinteger(L, 1);
    const char *script = luaL_checklstring(L, 2, &length);
    return mhd_wrap(L, 3, port, strdup(script), length);
}

static int l_mhd_loadfile(lua_State *L) {
    int port = (int)luaL_checkinteger(L, 1);
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
    return mhd_wrap(L, 3, port, buf, len);
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

    int port = (int)luaL_checkinteger(L, 1);

    Buffer buf;
    buf.data = NULL;
    buf.length = 0;
    buf.capacity = 0;

    lua_pushvalue(L, 2);
    int rc = lua_dump(L, func_writer, &buf, 0);
    lua_pop(L, 1);

    return mhd_wrap(L, 3, port, buf.data, buf.length);
}

static int server_stop(lua_State *L) {
    LuaMHDServer *srv = luaL_checkudata(L, 1, LUA_MHD_SERVER);

    if (srv->daemon) {
        MHD_stop_daemon(srv->daemon);
        srv->daemon = NULL;
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
    lua_pushfstring(L, "%08x", (unsigned int)MHD_VERSION);
    lua_setfield(L, -2, "version");

    return 1;
}
