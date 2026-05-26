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
