<!--
    lua-mhd -- Lua wrapper for libmicrohttpd

    Copyright (C) 2026 Предраг Јовановић
    SPDX-FileCopyrightText: 2026 Предраг Јовановић
    SPDX-License-Identifier: LGPL-3.0-or-later

    Look at the COPYING and COPYING.LESSER files for more information.
-->

# lua-mhd

Lua bindings for GNU libmicrohttpd, providing a simple way to build multithreaded HTTP servers in Lua.

## Features

* Built on top of GNU libmicrohttpd
* Each request is handled in a separate thread
* Each worker thread uses its own `lua_State`
* Supports loading worker scripts from source strings, files, or Lua callbacks
* Simple request/response API
* TLS/HTTPS, Unix domain sockets, and thread-pool tuning via an options table
* Static file serving, HTTP Basic/Digest authentication helpers
* Streaming request and response bodies
* Configurable request body size limits
* Compatible with LuaRocks

## Installation

Using LuaRocks:

```bash
luarocks install lua-mhd
```

## Usage

```lua
local mhd = require "lua-mhd"
```

### Starting a Server

Three functions are provided for creating a server:

```lua
local srv = mhd.load(options, sourceChunk, ...)
local srv = mhd.loadfile(options, path, ...)
local srv = mhd.start(options, callback, ...)
```

All functions start an HTTP server based on the given `options` table.

The additional arguments (`...`) are passed to every worker when it is created.

### Server Options

The `options` table replaces the plain port number and configures the
underlying `MHD_Daemon`.

| Field                      | Type    | Default | Description                                                              |
| -------------------------- | ------- | ------- | ------------------------------------------------------------------------ |
| `port`                     | number  | —       | TCP port to listen on. Required unless `unix_socket` is given.          |
| `unix_socket`               | string  | none    | Path to a Unix domain socket to listen on instead of a TCP port.        |
| `unix_socket_mode`          | number  | none    | `chmod` mode applied to the socket file (e.g. `tonumber("660", 8)`).     |
| `thread_pool_size`         | number  | none    | Use a fixed-size internal thread pool instead of one thread per connection. |
| `single_thread`            | boolean | `false` | Run the daemon on a single internal thread. Mutually exclusive with `thread_pool_size`. |
| `connection_limit`         | number  | none    | Maximum number of concurrent connections.                                |
| `connection_timeout`       | number  | none    | Idle connection timeout, in seconds.                                     |
| `per_ip_connection_limit`  | number  | none    | Maximum number of concurrent connections per client IP.                  |
| `max_body_size`             | number  | none    | Maximum request body size, in bytes. Larger bodies get a `413`.         |
| `debug`                    | boolean | `false` | Enable libmicrohttpd's internal debug/verbose logging.                   |
| `ipv6`                     | boolean | `false` | Listen on IPv6 in addition to IPv4.                                      |
| `tls`                       | table   | none    | Enables HTTPS. See [TLS / HTTPS](#tls--https) below.                    |

By default (no `thread_pool_size` or `single_thread` given), the daemon uses
one thread per connection, which matches the [threading model](#threading-model)
described below. Setting `thread_pool_size` switches to a fixed pool of
worker threads instead, which is generally more efficient under high
connection churn while remaining compatible with the thread-local `lua_State`
model. `single_thread` runs everything, including request handling, on a
single internal thread — useful for simple or low-traffic servers.

Example:

```lua
local srv = mhd.start({
  port = 8080,
  thread_pool_size = 4,
  connection_timeout = 30,
  connection_limit = 1000,
  max_body_size = 10 * 1024 * 1024, -- 10 MiB
}, function()
  return function(req)
    return { code = 200, body = "Hello, world!" }
  end
end)
```

### TLS / HTTPS

Pass a `tls` table with PEM-encoded `key` and `cert` strings to serve HTTPS
instead of plain HTTP:

```lua
local function readFile(path)
  local f = assert(io.open(path, "rb"))
  local data = f:read("a")
  f:close()
  return data
end

local srv = mhd.start({
  port = 8443,
  tls = {
    key = readFile("key.pem"),
    cert = readFile("cert.pem"),
  },
}, function()
  return function(req)
    return { code = 200, body = "Hello over HTTPS!" }
  end
end)
```

#### `tls` fields

| Field          | Type   | Description                                                        |
| -------------- | ------ | ------------------------------------------------------------------ |
| `key`          | string | **Required.** Private key, PEM-encoded.                            |
| `cert`         | string | **Required.** Certificate, PEM-encoded.                             |
| `trust`        | string | Optional CA/trust chain, PEM-encoded, for verifying client certs.   |
| `key_password` | string | Optional password if `key` is encrypted.                            |

### Unix Domain Sockets

Set `unix_socket` instead of `port` to listen on a Unix domain socket —
useful when running behind a reverse proxy like nginx:

```lua
local srv = mhd.start({
  unix_socket = "/run/myapp/server.sock",
  unix_socket_mode = tonumber("660", 8),
}, function()
  return function(req)
    return { code = 200, body = "Hello via Unix socket!" }
  end
end)
```

The socket file is created (replacing any stale file at the same path) when
the server starts, and removed automatically when the server is stopped,
garbage collected, or fails to start.

### Worker Initialization

Each incoming request is processed by a dedicated worker thread running in its own `lua_State`.

The worker script must return a request handler function:

```lua
function workerScript(...)
  -- Worker initialization
  local config = ...

  return function(req)
    return {
      code = 200,
      body = "Hello, world!"
    }
  end
end
```

Alternatively, it may return a table with a `handle` function and an
optional `on_data` function, to stream the request body instead of
buffering it — see [Streaming Requests](#streaming-requests).

### Using `mhd.start`

`mhd.start` accepts a Lua callback that generates the worker script.

```lua
local srv = mhd.start({ port = 8080 }, function(...)
  return function(req)
    return {
      code = 200,
      body = "Hello from lua-mhd"
    }
  end
end)
```

#### Important

The callback passed to `mhd.start` is serialized as Lua bytecode before being transferred to worker states.

Because of this:

* The callback **must not contain upvalues**
* Only serializable Lua functions can be used
* Any required state should be passed through the variadic arguments (`...`)

### Using `mhd.load`

Load a worker script from a Lua source string:

```lua
local source = [[
return function(req)
  return {
    code = 200,
    body = "Loaded from source"
  }.
end
]]

local srv = mhd.load({ port = 8080 }, source)
```

### Using `mhd.loadfile`

Load a worker script from a Lua file:

```lua
local srv = mhd.loadfile({ port = 8080 }, "worker.lua")
```

## Passing Parameters to Workers

Additional arguments supplied to `load`, `loadfile`, or `start` are passed to each worker when it is created.

Supported argument types:

* `nil`
* `boolean`
* `number`
* `string`
* `lightuserdata`

Example:

```lua
local srv = mhd.start(
  { port = 8080 },
  workerScript,
  "production",
  true,
  42
)
```

### Passing Tables

Tables cannot be transferred directly between worker states.

If you need to pass structured data, serialize it to a string first:

```lua
local json = require "cjson"

local config = json.encode({
  database = "app.db",
  timeout = 30
})

local srv = mhd.start({ port = 8080 }, workerScript, config)
```

The worker can then deserialize the string during initialization.

## Request Object

Incoming requests are provided as a Lua table:

```lua
{
  method = "GET",
  url = "/index.html",
  version = "HTTP/1.1",
  headers = {
    ["Content-Type"] = "text/plain"
  },
  query = {
    q = "search term"
  },
  body = "Request body",
  basic_auth = {
    username = "alice",
    password = "secret"
  },
  digest_username = "alice",
  check_digest = function(realm, username, password) ... end
}
```

### Fields

| Field             | Type     | Description                                                        |
| ----------------- | -------- | ------------------------------------------------------------------ |
| `method`          | string   | HTTP request method                                                 |
| `url`             | string   | Requested URL                                                       |
| `version`         | string   | HTTP protocol version                                                |
| `headers`         | table    | Request headers                                                     |
| `query`           | table    | Parsed query string parameters                                      |
| `body`            | string   | Request body                                                        |
| `basic_auth`      | table    | Present if the client sent an `Authorization: Basic` header. See below. |
| `digest_username` | string   | Present if the client sent an `Authorization: Digest` header.       |
| `check_digest`    | function | Verifies a Digest Authentication response. See below.               |

Note: if a query parameter is repeated (e.g. `?tag=a&tag=b`), only the last
value is kept in the `query` table.

## Authentication

### HTTP Basic Authentication

If the client sent an `Authorization: Basic` header, it is parsed
automatically and made available as `req.basic_auth`:

```lua
return function(req)
  local auth = req.basic_auth
  if auth and auth.username == "alice" and auth.password == "secret" then
    return { code = 200, body = "welcome, alice" }
  end

  return {
    code = 401,
    body = "Access denied",
    headers = { ["WWW-Authenticate"] = 'Basic realm="my-realm"' }
  }
end
```

`auth.password` may be `nil` if the client didn't send one.

### HTTP Digest Authentication

Digest authentication needs cooperation from libmicrohttpd itself (to
generate and validate nonces), so it works a little differently. The
client's username is exposed as `req.digest_username`, letting the script
look up the corresponding password (e.g. from a database); the response is
then verified with `req.check_digest(realm, username, password)`:

```lua
local function lookupPassword(username)
  -- e.g. a database lookup; here, a fixed "user database"
  local passwords = { alice = "secret" }
  return passwords[username]
end

return function(req)
  local username = req.digest_username
  local password = username and lookupPassword(username)

  if password then
    local ok = req.check_digest("my-realm", username, password)
    if ok == true then
      return { code = 200, body = "welcome, " .. username }
    end
  end

  return {
    code = 401,
    body = "Access denied",
    digest_challenge = { realm = "my-realm" }
  }
end
```

`req.check_digest(realm, username, password, [opts])` returns:

* `true` — authentication succeeded.
* `false` — authentication failed (wrong password, malformed header, etc).
* `"stale"` — the credentials were previously valid but the nonce has
  expired; ask the client to retry (it will, transparently, using the same
  username/password).

`opts` is an optional table with:

| Field           | Type   | Default | Description                                      |
| --------------- | ------ | ------- | ------------------------------------------------- |
| `algorithm`     | string | `"MD5"` | `"MD5"`, `"SHA256"`, or `"any"` to accept either. |
| `nonce_timeout` | number | daemon default | How long (seconds) a nonce remains valid. |

`digest_challenge` on a **response** table (see below) turns that response
into a proper `WWW-Authenticate: Digest ...` challenge instead of sending
it as-is:

| Field       | Type    | Description                                             |
| ----------- | ------- | -------------------------------------------------------- |
| `realm`     | string  | Realm presented to the client.                            |
| `opaque`    | string  | Optional opaque value echoed back by the client.          |
| `stale`     | boolean | Set when re-challenging after a `"stale"` result, so the client retries automatically. |
| `algorithm` | string  | `"MD5"` (default), `"SHA256"`, or `"any"`.                |

## Response Object

Request handlers should return a response table:

```lua
{
  code = 200,
  body = "<html>Hello</html>",
  headers = {
    ["Content-Type"] = "text/html"
  }
}
```

Instead of `body`, a response can set `file` to have libmicrohttpd stream a
file from disk directly (using `sendfile` where supported), which avoids
reading the whole file into memory yourself:

```lua
return function(req)
  return {
    code = 200,
    file = "/var/www/static/logo.png",
    headers = { ["Content-Type"] = "image/png" }
  }
end
```

If the file cannot be opened (missing, permissions, not a regular file), a
`404 Not Found` is sent automatically instead, and `code`/`headers` are
ignored for that response.

### Fields

| Field              | Type   | Description                                                        |
| ------------------ | ------ | ------------------------------------------------------------------- |
| `code`             | number | HTTP status code                                                     |
| `body`             | string | Response body. Ignored if `file` or `stream` is set.                 |
| `file`             | string | Path to a file to serve as the response body, streamed by libmicrohttpd. |
| `stream`           | function | Produces the response body incrementally. See [Streaming Responses](#streaming-responses). |
| `stream_size`      | number | Total response size in bytes, if known in advance. Only used with `stream`. |
| `headers`          | table  | Response headers                                                     |
| `digest_challenge` | table  | Turns this response into an HTTP Digest Authentication challenge. See [Authentication](#authentication). |

Note: range requests (`Range:` header, partial content) aren't handled
specially for `file` responses — the whole file is sent every time.

## Streaming Responses

Instead of `body`, a response can set `stream` to a function that produces
the body incrementally. `lua-mhd` calls it repeatedly, sending each chunk
to the client as it's produced, instead of requiring the whole body to be
built as one Lua string up front:

```lua
return function(req)
  local n = 0

  return {
    code = 200,
    headers = { ["Content-Type"] = "text/plain" },
    stream = function()
      n = n + 1
      if n > 5 then
        return nil -- ends the response
      end
      return "chunk " .. n .. "\n"
    end
  }
end
```

The function is called with no arguments and should return the next chunk
as a string. Returning `nil`, `false`, or an empty string ends the
response. If the function raises an error, the response ends there and the
error is logged to stderr — by that point headers (and possibly some body
bytes) have already been sent to the client, so the error can't be turned
into a clean HTTP error response anymore; keep any validation that can fail
outside the `stream` function where possible.

If the total size is known ahead of time, set `stream_size` (in bytes) so
`lua-mhd` can send a proper `Content-Length` header instead of
`Transfer-Encoding: chunked`:

```lua
return {
  code = 200,
  stream_size = fileSize,
  stream = function() ... end
}
```

## Streaming Requests

By default, the request body is fully read into memory (available as
`req.body`) before the handler function is called. For large uploads, a
worker can instead return a table with `on_data`, which is called once per
chunk **as it arrives**, so the body is never buffered in memory:

```lua
function workerScript(...)
  return {
    on_data = function(req, chunk)
      -- called once per incoming chunk, in order, before `handle` runs
      req.total = (req.total or 0) + #chunk
    end,

    handle = function(req)
      -- req.body is NOT populated when on_data is used
      return { code = 200, body = "received " .. (req.total or 0) .. " bytes" }
    end
  }
end
```

`req` is the same table across every `on_data` call and the final `handle`
call for a given request, so it's a convenient place to accumulate state
(write chunks to a file, hash them, parse them incrementally, etc). If
`on_data` raises an error, the upload is stopped, `handle` is never called,
and a `500 Internal Server Error` is sent once the connection has finished
draining.

`options.max_body_size` (see [Server Options](#server-options)) is
enforced the same way for streamed requests as for buffered ones — it
tracks total bytes received rather than buffer size, so it works whether
or not `on_data` is used.

## Complete Example

```lua
local mhd = require "lua-mhd"

local function worker(greeting)
  return function(req)
    return {
      code = 200,
      body = greeting,
      headers = {
        ["Content-Type"] = "text/plain"
      }
    }
  end
end

local srv = mhd.start(
  { port = 8080 },
  worker,
  "Hello, world!"
)

print("Server listening on port 8080")

io.read('l') -- basic graceful shutdown

srv:stop()
```

## Stopping the Server

```lua
srv:stop()
```

This stops the HTTP server and all worker threads.

## Threading Model

`lua-mhd` uses a multithreaded architecture:

* Each request is handled in a separate thread.
* Each thread owns its own independent `lua_State`.
* Lua objects are not shared between workers.
* Worker initialization code runs once when the worker state is created.
* The returned request handler is then used to process incoming requests.

Because workers are isolated, communication between them must be done through external mechanisms such as databases, sockets, shared memory, or serialized messages.

## License

See [COPYING](./COPYING) and [COPYING.LESSER](./COPYING.LESSER) files for more information.
