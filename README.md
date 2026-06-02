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
local srv = mhd.load(port, sourceChunk, ...)
local srv = mhd.loadfile(port, path, ...)
local srv = mhd.start(port, callback, ...)
```

All functions start an HTTP server listening on the specified port.

The additional arguments (`...`) are passed to every worker when it is created.

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

### Using `mhd.start`

`mhd.start` accepts a Lua callback that generates the worker script.

```lua
local srv = mhd.start(8080, function(...)
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

local srv = mhd.load(8080, source)
```

### Using `mhd.loadfile`

Load a worker script from a Lua file:

```lua
local srv = mhd.loadfile(8080, "worker.lua")
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
  8080,
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

local srv = mhd.start(8080, workerScript, config)
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
  body = "Request body"
}
```

### Fields

| Field     | Type   | Description           |
| --------- | ------ | --------------------- |
| `method`  | string | HTTP request method   |
| `url`     | string | Requested URL         |
| `version` | string | HTTP protocol version |
| `headers` | table  | Request headers       |
| `body`    | string | Request body          |

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

### Fields

| Field     | Type   | Description      |
| --------- | ------ | ---------------- |
| `code`    | number | HTTP status code |
| `body`    | string | Response body    |
| `headers` | table  | Response headers |

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
  8080,
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
