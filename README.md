# HexChat Lua Interface
## Scripts
Just like many other language plugins, the Lua plugin provides the commands `/load`, `/unload`, and `/reload` which operate on Lua scripts. You can forcibly load a Lua script with any extension by using `/lua load`, `/lua unload` or `/lua reload` instead. All files ending in `.lua` and `.luac` in the addons directory will be automatically loaded on startup.

Every script gets its own isolated state. The state is initialized with the Lua standard library, and has a global table called `hexchat` providing the API, which is described below.

On top of that, the interactive interpreter available through `/lua exec` and `/lua console` has an isolated state as well.

## Commands
### `/load <filename>` and `/lua load <filename>`
Loads a script with the given filename. `/load` will only load files ending in `.lua` and `.luac`.

### `/unload <filename>` and `/lua unload <filename>`
Unloads a script with the given filename.

### `/reload <filename>` and `/lua reload <filename>`
Reloads a script with the given filename.

### `/lua list`
Lists loaded Lua scripts.

### `/lua exec <code>`
Executes given code in the interpreter.

### `/lua console`
Opens an interactive console. Messages to that tab are intercepted and interpreted as code.

### `/lua inject <filename> <code>`
Executes given code in the context of a given script (which has to be loaded).

### `/lua reload`
Reloads the interpreter (but not the scripts).

## API
The HexChat API is accessible through the `hexchat` table.

### General functions

#### `hexchat.register(name, version, description)`
Upon initialization every script should introduce itself by calling `hexchat.register`. Failure to do so will result in the script being unloaded immediately.

#### `hexchat.command(cmd)`
Executes the string the command `cmd` in the current context, as if `/cmd` was typed by the user.

#### `hexchat.print(...)`
Prints zero or more values to the current tab. This function also replaces the global `print` function.

#### `hexchat.emit_print(event, ...)`
Emits a text event (can be found in Settings->Text Events) into the current tab. `...` are the strings that are passed as arguments to the event. At the moment due to internal limitations, only 5 arguments are passed as there aren't any text events with more arguments.

#### `hexchat.send_modes(targets, mode[, max])`
Sets multiple modes in the current context, possibly grouping them together up to the server limit. `targets` needs to be an array of strings, `mode` needs to be a string of 2 characters: `+` or `-` followed by the mode letter. `max` is the number of modes on one line, if omitted the serverside limit is used.

#### `hexchat.nickcmp(a, b)`
Compares 2 strings case-insensitively, in accordance with current server's casemapping. Returns a negative number if `a` is less than `b`, zero if they are equal, and a positive number if `a` is more than `b`.

#### `hexchat.strip(string[, keep_colors[, keep_attrs]])`
Removes color codes from the given string. If `keep_colors` is a truthy value, colors are not removed. If `keep_attrs` is a truthy value, attributes such as bold or underline are not removed. Returns the resulting string.

#### `hexchat.get_info(id)`
Returns information about the current context. `id` is a string determining the information you want. It can be one of the following (case sensitive):
* `away` - Away reason or nil if you are not away.
* `channel` - Current channel name.
* `charset` - Character set used in the current context.
* `configdir` - HexChat config directory, e.g. `/home/user/.config/hexchat`.
* `event_text <name>` - Text event format string for `<name>`.
* `host` - Real hostname of the server you connected to.
* `inputbox` - The input box contents, what the user has typed.
* `libdirfs` - Library directory. e.g. `/usr/lib/hexchat`. The same directory is used for autoloading plugins.
* `modes` - Channel modes, or nil if not known.
* `network` - Current network name, or nil if not known.
* `nick` - Your current nickname.
* `nickserv` - NickServ password for this network or nil.
* `server` - Current server name (what the server claims to be). nil if you are not connected.
* `topic` - Current channel topic.
* `version` - HexChat version number.
* `win_status` - Window status: `active`, `hidden` or `normal`.
* `win_ptr` - A light userdata pointer to the native window. GtkWindow * on unix, HWND on windows.
* `gtkwin_ptr` - Light userdata pointer to a GtkWindow.

#### `hexchat.iterate(list)`
Iterate through the list `list`. To be used with generic for-loops in the following fashion:

     for chan in hexchat.iterate"channels" do
         print(chan.server .. ": " .. chan.channel)
     end

### Preferences
You can access HexChat's settings via the `hexchat.prefs` pseudo-table, see `/set` for a list of keys. Note that you cannot modify the table. Instead, you should use `hexchat.command"/set -quiet <key> <value>"`

### Hooks
Some hooks are executed in a priority order, and hooks executed earlier can prevent later hooks from being invoked. The following constants determine priorities of such hooks and are passed to the hooking function:

* `hexchat.PRI_HIGHEST` - The highest priority.
* `hexchat.PRI_HIGH`
* `hexchat.PRI_NORM` - The default priority.
* `hexchat.PRI_LOW`
* `hexchat.PRI_LOWEST` - The lowest priority.

The following constants determine whether to pass the event on after the hook has finished. One of these has to be returned from the callback:

* `hexchat.EAT_NONE` - Let other hooks see the event.
* `hexchat.EAT_HEXCHAT` - Let other hooks see the event, but prevent HexChat itself from seeing it.
* `hexchat.EAT_PLUGIN` - Don't let remaining hooks see the event, but let HexChat know about it.
* `hexchat.EAT_ALL` - Consume this event completely, don't let anyone else know about it.

All hooking functions return an object which can be later used to remove the hook, but the hooks are also removed automatically when the script is unloaded or reloaded.

Unlike the C and Python APIs, there isn't a userdata value passed to the hooks. Instead you should use upvalues, closures, and/or anonymous functions.

#### `hexchat.hook_command(command, callback[, help[, priority]])`
Hooks the function `callback` to be executed whenever `/command` is entered. `help` is the helptext for the `/help` command. Returns a hook object. The callback receives an array of words, and an array of word_eols as arguments.

If `command` is nil, then non-command text is hooked instead, including `/say`.

#### `hexchat.hook_print(event, callback[, priority])`
Hooks the function `callback` to be executed whenever the text event `event` is to be printed. Returns a hook object. The callback receives the array of event's arguments as its only argument.

There are also a few extra events you can hook using this function:
* `Open Context` - Emitted when a new context is created.
* `Close Context` - Emitted when a context is closed.
* `Focus Tab` - Emitted when a tab is brought to the front.
* `Focus Window` - Emitted when a toplevel window is focused, or the main tab-window is focused by the window manager.
* `DCC Chat Text` - Emitted when some text from a DCC Chat arrives. It provides these elements in the word list:
  * Address
  * Port
  * Nick
  * Message
* `Key Press` - Emitted when some keys are pressed in the input box. It provides these elements in the word list:
  * Key Value
  * Modifier bitfield (Shift, CapsLock, Alt, etc)
  * String version of the key
  * Length of the string (may be 0 for unprintable keys)


#### `hexchat.hook_server(command, callback[, priority])`
Hooks the function `callback` to be executed whenever `command` is received from the server. Returns a hook object. The callback receives an array of words, and an array of word_eols as arguments.

If `command` is nil, then the callback is called for every received line.

#### `hexchat.hook_timer(interval, callback)`
Hooks the function `callback` to be executed after `inverval` milliseconds. Returns a hook object. If the callback returns a truthy value, it is scheduled to happen after the same preiod of time.

#### `hexchat.hook_unload(callback)`
Hooks the function `callback` to be executed when the current script is unloaded. Returns a hook object.

#### `hook:unhook()` and `hexchat.unhook(hook)`
Removes the given hook. A hook can only be removed once.

### Contexts
A context corresponds to a HexChat window or tab. Some of the functions in `hexchat.*` will do something in the current tab. Using contexts you can perform such actions in other tabs instead.

#### `hexchat.get_context()`
Returns a context object for the current context.

#### `hexchat.find_context(server_name, channel_name)`
Finds a context object for a tab on the given channel of the given channel. If `server_name` is nil, it searches for the given channel or query across all servers. If `channel_name` is nil, finds the frontmost tab of the given server. If both are `nil`, returns current context. In any case, if the specified tab was not found, the function returns nil.

#### `ctx:set()` and `hexchat.set_context(ctx)`
Makes `ctx` the "current" context. All `hexchat.*` will be using this context. This setting only persists within one event. Next time any of the callbacks is called, the current context will be set to the actual one.

#### `ctx:find_context(server_name, channel_name)`
Identical to `hexchat.find_context`, except the defaults are based on the current context.

#### `ctx:print(...)`
Prints zero or more values in the given context.

#### `ctx:emit_print(event, ...)`
Emits a text event into the given context. See `hexchat.emit_print`.

#### `ctx:command(cmd)`
Executes the command `/cmd` in the given context. See `hexchat.command`.

#### `ctx:nickcmp(a, b)`
Compares 2 strings using casemapping from the given context. See `hexchat.nickcmp`.

#### `ctx:get_info(id)`
Returns information about the given context. See `hexchat.get_info`.

### Plugin preferences
To persistently store your script's settings, you can use the `hexchat.pluginprefs` pseudo-table. The values inside will persist across script reloads, HexChat restarts, and reboots. Currently, you can only store and read strings and numbers associated to string keys, and iterate through the table with `pairs()`.

### Attributes
Attributes correspond to extra metadata for messages, such as server-time (currently the only supported attribute). Some functions have attributes-enhanced versions.

#### `hexchat.attrs()`
Returns a new attributes object. It has only one field: `server_time_utc`.

#### `hexchat.emit_print_attrs(attrs, event, ...)` and `ctx:emit_print_attrs(attrs, event, ...)`
Analogous to `hexchat.emit_print` and `ctx:emit_print` respectively, but passes an extra attributes argument.

#### `hexchat.hook_print_attrs(event, callback[, priority])`
Identical to `hexchat.hook_print`, except that the callback receives an additional second argument with an attributes object and that the aforementioned extra events cannot be hooked.

#### `hexchat.hook_server_attrs(command, callback[, priority])`
Identical to `hexchat.hook_server`, except that the callback receives an additional third argument with an attributes object.


