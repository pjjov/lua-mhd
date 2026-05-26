package = "lua-mhd"
version = "1.0-1"

source = {
   url = "git://github.com/pjjov/lua-mhd"
}

description = {
   summary = "Lua wrapper for libmicrohttpd",
   detailed = [[
Lua wrapper for GNU libmicrohttpd that uses multiple Lua
states for handling requests across multiple worker threads.
]],
   homepage = "https://github.com/pjjov/lua-mhd",
   license = "LGPL-3.0-or-later"
}

dependencies = {
   "lua >= 5.1"
}

build = {
   type = "builtin",

   external_dependencies = {
      MICROHTTPD = {
         header = "microhttpd.h",
         library = "microhttpd"
      }
   },

   modules = {
      mhd = {
         sources = {
            "lua-mhd.c"
         },

         libraries = {
            "microhttpd",
            "pthread"
         }
      }
   }
}
