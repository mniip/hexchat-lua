#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <glib.h>
#include <gmodule.h>

#ifndef G_OS_WIN32
#include <pwd.h>
#endif

#include <hexchat-plugin.h>


static char plugin_name[] = "lua";
static char plugin_description[] = "Lua scripting interface";
static char plugin_version[256] = "1.2-";

static char console_tab[] = ">>lua<<";
static char command_help[] = 
	"Usage: /lua load <filename>\n"
	"            unload <filename>\n"
	"            reload <filename>\n"
	"            exec <code>\n"
	"            inject <filename> <code>\n"
	"            reset\n"
	"            list\n"
	"            console";

static char registry_field[] = "plugin";

static hexchat_plugin *ph;

#if LUA_VERSION_NUM < 502
#define lua_rawlen lua_objlen
#define luaL_setfuncs(L, r, n) luaL_register(L, NULL, r)
#endif

#define ARRAY_RESIZE(A, N) ((A) = realloc((A), (N) * sizeof(*(A))))
#define ARRAY_GROW(A, N) ((N)++, ARRAY_RESIZE(A, N))
#define ARRAY_SHRINK(A, N) ((N)--, ARRAY_RESIZE(A, N))

typedef struct
{
	hexchat_hook *hook;
	lua_State *state;
	int ref;
}
hook_info;

typedef struct
{
	char *name;
	char *description;
	char *version;
	hexchat_plugin *handle;
	char *filename;
	lua_State *state;
	int traceback;
	int status;
	hook_info **hooks;
	size_t num_hooks;
	hook_info **unload_hooks;
	size_t num_unload_hooks;
}
script_info;

#define STATUS_ACTIVE 1
#define STATUS_DEFERRED_UNLOAD 2
#define STATUS_DEFERRED_RELOAD 4

static void check_deferred(script_info *info);

static inline script_info *get_info(lua_State *L)
{
	lua_getfield(L, LUA_REGISTRYINDEX, registry_field);
	script_info *info = lua_touserdata(L, -1);
	lua_pop(L, 1);
	return info;
}

static int api_hexchat_register(lua_State *L)
{
	script_info *info = get_info(L);
	if(info->name)
		return luaL_error(L, "script is already registered as '%s'", info->name);
	char const *name = luaL_checkstring(L, 1);
	char const *version = luaL_checkstring(L, 2);
	char const *description = luaL_checkstring(L, 3);
	info->name = g_strdup(name);
	info->description = g_strdup(description);
	info->version = g_strdup(version);
	info->handle = hexchat_plugingui_add(ph, info->filename, info->name, info->description, info->version, NULL);
	return 0;
}

static int api_hexchat_command(lua_State *L)
{
	hexchat_command(ph, luaL_checkstring(L, 1));
	return 0;
}

static int tostring(lua_State *L, int n)
{
	luaL_checkany(L, n);
	switch (lua_type(L, n))
	{
		case LUA_TNUMBER:
			lua_pushstring(L, lua_tostring(L, n));
			break;
		case LUA_TSTRING:
			lua_pushvalue(L, n);
			break;
		case LUA_TBOOLEAN:
			lua_pushstring(L, (lua_toboolean(L, n) ? "true" : "false"));
			break;
		case LUA_TNIL:
			lua_pushliteral(L, "nil");
			break;
		default:
			lua_pushfstring(L, "%s: %p", luaL_typename(L, n), lua_topointer(L, n));
			break;
	}
	return 1;
}

static int api_hexchat_print(lua_State *L)
{
	int args = lua_gettop(L);
	luaL_Buffer b;
	luaL_buffinit(L, &b);
	int i;
	for(i = 1; i <= args; i++)
	{
		if(i != 1)
			luaL_addstring(&b, " ");
		tostring(L, i);
		luaL_addvalue(&b);
	}
	luaL_pushresult(&b);
	hexchat_print(ph, lua_tostring(L, -1));
	return 0;
}

static int api_hexchat_emit_print(lua_State *L)
{
	hexchat_emit_print(ph, luaL_checkstring(L, 1), luaL_optstring(L, 2, NULL), luaL_optstring(L, 3, NULL), luaL_optstring(L, 4, NULL), luaL_optstring(L, 5, NULL), luaL_optstring(L, 6, NULL), NULL);
	return 0;
}

static int api_hexchat_emit_print_attrs(lua_State *L)
{
	hexchat_event_attrs *attrs = *(hexchat_event_attrs **)luaL_checkudata(L, 1, "attrs");
	hexchat_emit_print_attrs(ph, attrs, luaL_checkstring(L, 2), luaL_optstring(L, 3, NULL), luaL_optstring(L, 4, NULL), luaL_optstring(L, 5, NULL), luaL_optstring(L, 6, NULL), luaL_optstring(L, 7, NULL), NULL);
	return 0;
}

static int api_hexchat_send_modes(lua_State *L)
{
	luaL_checktype(L, 1, LUA_TTABLE);
	size_t n = lua_rawlen(L, 1);
	char const *mode = luaL_checkstring(L, 2);
	if(strlen(mode) != 2)
		return luaL_argerror(L, 2, "expected sign followed by a mode letter");
	int modes = luaL_optinteger(L, 3, 0);
	char const **targets = malloc(n * sizeof(char const *));
	size_t i;
	for(i = 0; i < n; i++)
	{
		lua_rawgeti(L, 1, i + 1);
		if(lua_type(L, -1) != LUA_TSTRING)
		{
			free(targets);
			return luaL_argerror(L, 1, "expected an array of strings");
		}
		targets[i] = lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	hexchat_send_modes(ph, targets, n, modes, mode[0], mode[1]);
	free(targets);
	return 0;
}

static int api_hexchat_nickcmp(lua_State *L)
{
	lua_pushnumber(L, hexchat_nickcmp(ph, luaL_checkstring(L, 1), luaL_checkstring(L, 2)));
	return 1;
}

static int api_hexchat_strip(lua_State *L)
{
	size_t len;
	luaL_checktype(L, 1, LUA_TSTRING);
	char const *text = lua_tolstring(L, 1, &len);
	int leave_colors = lua_toboolean(L, 2);
	int leave_attrs = lua_toboolean(L, 3);
	char *result = hexchat_strip(ph, text, len, (leave_colors ? 0 : 1) | (leave_attrs ? 0 : 2));
	if(result)
	{
		lua_pushstring(L, result);
		hexchat_free(ph, result);
		return 1;
	}
	return 0;
}

static void register_hook(hook_info *hook)
{
	script_info *info = get_info(hook->state);
	ARRAY_GROW(info->hooks, info->num_hooks);
	info->hooks[info->num_hooks - 1] = hook;
}

static void free_hook(hook_info *hook)
{
	lua_State *L = hook->state;
	luaL_unref(L, LUA_REGISTRYINDEX, hook->ref);
	if(hook->hook)
		hexchat_unhook(ph, hook->hook);
	free(hook);
}

static int unregister_hook(hook_info *hook)
{
	script_info *info = get_info(hook->state);
	size_t i;
	for(i = 0; i < info->num_hooks; i++)
		if(info->hooks[i] == hook)
		{
			free_hook(hook);
			size_t j;
			for(j = i; j < info->num_hooks - 1; j++)
				info->hooks[j] = info->hooks[j + 1];
			ARRAY_SHRINK(info->hooks, info->num_hooks);
			return 1;
		}
	for(i = 0; i < info->num_unload_hooks; i++)
		if(info->unload_hooks[i] == hook)
		{
			free_hook(hook);
			size_t j;
			for(j = i; j < info->num_unload_hooks - 1; j++)
				info->unload_hooks[j] = info->unload_hooks[j + 1];
			ARRAY_SHRINK(info->unload_hooks, info->num_unload_hooks);
			return 1;
		}
	return 0;
}

static int api_command_closure(char *word[], char *word_eol[], void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	script_info *script = get_info(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, script->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i;
	lua_newtable(L);
	for(i = 1; i < 32 && *word_eol[i]; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	lua_newtable(L);
	for(i = 1; i < 32 && *word_eol[i]; i++)
	{
		lua_pushstring(L, word_eol[i]);
		lua_rawseti(L, -2, i);
	}
	script->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 2, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error in command hook: %s", error ? error : "(non-string error)");
		check_deferred(script);
		return HEXCHAT_EAT_NONE;
	}
	int ret = lua_tointeger(L, -1);
	lua_pop(L, 2);
	check_deferred(script);
	return ret;
}

static int api_hexchat_hook_command(lua_State *L)
{
	char const *command = luaL_optstring(L, 1, "");
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	char const *help = luaL_optstring(L, 3, NULL);
	int pri = luaL_optinteger(L, 4, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_command(ph, command, pri, api_command_closure, help, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static int api_print_closure(char *word[], void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	script_info *script = get_info(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, script->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i, j;
	for(j = 31; j >= 1; j--)
		if(*word[j])
			break;
	lua_newtable(L);
	for(i = 1; i <= j; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	script->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 1, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error in print hook: %s", error ? error : "(non-string error)");
		check_deferred(script);
		return HEXCHAT_EAT_NONE;
	}
	int ret = lua_tointeger(L, -1);
	lua_pop(L, 2);
	check_deferred(script);
	return ret;
}

static int api_hexchat_hook_print(lua_State *L)
{
	char const *event = luaL_checkstring(L, 1);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optinteger(L, 3, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_print(ph, event, pri, api_print_closure, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static hexchat_event_attrs *event_attrs_copy(const hexchat_event_attrs *attrs)
{
	hexchat_event_attrs *copy = hexchat_event_attrs_create(ph);
	copy->server_time_utc = attrs->server_time_utc;
	return copy;
}

static int api_print_attrs_closure(char *word[], hexchat_event_attrs *attrs, void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	script_info *script = get_info(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, script->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i, j;
	for(j = 31; j >= 1; j--)
		if(*word[j])
			break;
	lua_newtable(L);
	for(i = 1; i <= j; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	hexchat_event_attrs **u = lua_newuserdata(L, sizeof(hexchat_event_attrs *));
	*u = event_attrs_copy(attrs);
	luaL_newmetatable(L, "attrs");
	lua_setmetatable(L, -2);
	script->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 2, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error in print_attrs hook: %s", error ? error : "(non-string error)");
		check_deferred(script);
		return HEXCHAT_EAT_NONE;
	}
	int ret = lua_tointeger(L, -1);
	lua_pop(L, 2);
	check_deferred(script);
	return ret;
}

static int api_hexchat_hook_print_attrs(lua_State *L)
{
	char const *event = luaL_checkstring(L, 1);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optinteger(L, 3, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_print_attrs(ph, event, pri, api_print_attrs_closure, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static int api_server_closure(char *word[], char *word_eol[], void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	script_info *script = get_info(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, script->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i;
	lua_newtable(L);
	for(i = 1; i < 32 && *word_eol[i]; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	lua_newtable(L);
	for(i = 1; i < 32 && *word_eol[i]; i++)
	{
		lua_pushstring(L, word_eol[i]);
		lua_rawseti(L, -2, i);
	}
	script->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 2, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error in server hook: %s", error ? error : "(non-string error)");
		check_deferred(script);
		return HEXCHAT_EAT_NONE;
	}
	int ret = lua_tointeger(L, -1);
	lua_pop(L, 2);
	check_deferred(script);
	return ret;
}

static int api_hexchat_hook_server(lua_State *L)
{
	char const *command = luaL_optstring(L, 1, "RAW LINE");
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optinteger(L, 3, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_server(ph, command, pri, api_server_closure, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static int api_server_attrs_closure(char *word[], char *word_eol[], hexchat_event_attrs *attrs, void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	script_info *script = get_info(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, script->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i;
	lua_newtable(L);
	for(i = 1; i < 32 && *word_eol[i]; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	lua_newtable(L);
	for(i = 1; i < 32 && *word_eol[i]; i++)
	{
		lua_pushstring(L, word_eol[i]);
		lua_rawseti(L, -2, i);
	}
	hexchat_event_attrs **u = lua_newuserdata(L, sizeof(hexchat_event_attrs *));
	*u = event_attrs_copy(attrs);
	luaL_newmetatable(L, "attrs");
	lua_setmetatable(L, -2);
	script->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 3, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error in server_attrs hook: %s", error ? error : "(non-string error)");
		return HEXCHAT_EAT_NONE;
		check_deferred(script);
	}
	int ret = lua_tointeger(L, -1);
	lua_pop(L, 2);
	check_deferred(script);
	return ret;
}

static int api_hexchat_hook_server_attrs(lua_State *L)
{
	char const *command = luaL_optstring(L, 1, "RAW LINE");
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optinteger(L, 3, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_server_attrs(ph, command, pri, api_server_attrs_closure, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static int api_timer_closure(void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	script_info *script = get_info(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, script->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	script->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 0, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error in timer hook: %s", error ? error : "(non-string error)");
		check_deferred(script);
		return 0;
	}
	int ret = lua_toboolean(L, -1);
	lua_pop(L, 2);
	check_deferred(script);
	return ret;
}

static int api_hexchat_hook_timer(lua_State *L)
{
	int timeout = luaL_checknumber(L, 1);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_timer(ph, timeout, api_timer_closure, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static int api_hexchat_hook_unload(lua_State *L)
{
	lua_pushvalue(L, 1);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = NULL;
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	script_info *script = get_info(info->state);
	ARRAY_GROW(script->unload_hooks, script->num_unload_hooks);
	script->unload_hooks[script->num_unload_hooks - 1] = info;
	return 1;
}

static int api_hexchat_unhook(lua_State *L)
{
	hook_info **info = (hook_info **)luaL_checkudata(L, 1, "hook");
	if(*info)
	{
		unregister_hook(*info);
		*info = 0;
		return 0;
	}
	else
	{
		tostring(L, 1);
		return luaL_error(L, "hook %s is already unhooked", lua_tostring(L, -1));
	}
}

static int api_hexchat_find_context(lua_State *L)
{
	char const *server = luaL_optstring(L, 1, NULL);
	char const *channel = luaL_optstring(L, 2, NULL);
	hexchat_context *context = hexchat_find_context(ph, server, channel);
	if(context)
	{
		hexchat_context **u = lua_newuserdata(L, sizeof(hexchat_context *));
		*u = context;
		luaL_newmetatable(L, "context");
		lua_setmetatable(L, -2);
		return 1;
	}
	else
	{
		lua_pushnil(L);
		return 1;
	}
}

static int api_hexchat_get_context(lua_State *L)
{
	hexchat_context *context = hexchat_get_context(ph);
	hexchat_context **u = lua_newuserdata(L, sizeof(hexchat_context *));
	*u = context;
	luaL_newmetatable(L, "context");
	lua_setmetatable(L, -2);
	return 1;
}

static int api_hexchat_set_context(lua_State *L)
{
	hexchat_context *context = *(hexchat_context **)luaL_checkudata(L, 1, "context");
	int success = hexchat_set_context(ph, context);
	lua_pushboolean(L, success);
	return 1;
}

static int wrap_context_closure(lua_State *L)
{
	hexchat_context *context = *(hexchat_context **)luaL_checkudata(L, 1, "context");
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_replace(L, 1);
	hexchat_context *old = hexchat_get_context(ph);
	if(!hexchat_set_context(ph, context))
		return luaL_error(L, "could not switch into context");
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	hexchat_set_context(ph, old);
	return lua_gettop(L);
}

static inline void wrap_context(lua_State *L, char const *field, lua_CFunction func)
{
	lua_pushcfunction(L, func);
	lua_pushcclosure(L, wrap_context_closure, 1);
	lua_setfield(L, -2, field);
}

static int api_hexchat_context_meta_eq(lua_State *L)
{
	hexchat_context *this = *(hexchat_context **)luaL_checkudata(L, 1, "context");
	hexchat_context *that = *(hexchat_context **)luaL_checkudata(L, 2, "context");
	lua_pushboolean(L, this == that);
	return 1;
}

static int api_hexchat_get_info(lua_State *L)
{
	char const *key = luaL_checkstring(L, 1);
	char const *data = hexchat_get_info(ph, key);
	if(data)
	{
		if(!strcmp(key, "gtkwin_ptr") || !strcmp(key, "win_ptr"))
			lua_pushlightuserdata(L, (void *)data);
		else
			lua_pushstring(L, data);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int api_hexchat_attrs(lua_State *L)
{
	hexchat_event_attrs *attrs = hexchat_event_attrs_create(ph);
	hexchat_event_attrs **u = lua_newuserdata(L, sizeof(hexchat_event_attrs *));
	*u = attrs;
	luaL_newmetatable(L, "attrs");
	lua_setmetatable(L, -2);
	return 1;
}

static int api_iterate_closure(lua_State *L)
{
	hexchat_list *list = *(hexchat_list **)luaL_checkudata(L, lua_upvalueindex(1), "list");
	if(hexchat_list_next(ph, list))
	{
		lua_pushvalue(L, lua_upvalueindex(1));
		return 1;
	}
	else
		return 0;
}

static int api_hexchat_iterate(lua_State *L)
{
	char const *name = luaL_checkstring(L, 1);
	hexchat_list *list = hexchat_list_get(ph, name);
	if(list)
	{
		hexchat_list **u = lua_newuserdata(L, sizeof(hexchat_list *));
		*u = list;
		luaL_newmetatable(L, "list");
		lua_setmetatable(L, -2);
		lua_pushcclosure(L, api_iterate_closure, 1);
		return 1;
	}
	else
		return luaL_argerror(L, 1, "invalid list name");
}

static int api_hexchat_prefs_meta_index(lua_State *L)
{
	char const *key = luaL_checkstring(L, 2);
	char const *string;
	int number;
	int ret = hexchat_get_prefs(ph, key, &string, &number);
	switch(ret)
	{
		case 0:
			lua_pushnil(L);
			return 1;
		case 1:
			lua_pushstring(L, string);
			return 1;
		case 2:
			lua_pushnumber(L, number);
			return 1;
		case 3:
			lua_pushboolean(L, number);
			return 1;
		default:
			return 0;
	}
}

static int api_hexchat_prefs_meta_newindex(lua_State *L)
{
	return luaL_error(L, "hexchat.prefs is read-only");
}

static inline int list_marshal(lua_State *L, const char *key, hexchat_list *list)
{
	char const *str = hexchat_list_str(ph, list, key);
	if(str)
	{
		if(!strcmp(key, "context"))
		{
			hexchat_context **u = lua_newuserdata(L, sizeof(hexchat_context *));
			*u = (hexchat_context *)str;
			luaL_newmetatable(L, "context");
			lua_setmetatable(L, -2);
			return 1;
		}
		lua_pushstring(L, str);
		return 1;
	}
	int number = hexchat_list_int(ph, list, key);
	if(number != -1)
	{
		lua_pushinteger(L, number);
		return 1;
	}
	if (list != NULL)
	{
	  	time_t tm = hexchat_list_time(ph, list, key);
		if(tm != -1)
		{
			lua_pushinteger(L, tm);
			return 1;
		}
	}

	lua_pushnil(L);
	return 1;
}

static int api_hexchat_props_meta_index(lua_State *L)
{
  	char const *key = luaL_checkstring(L, 2);
	return list_marshal(L, key, NULL);
}

static int api_hexchat_props_meta_newindex(lua_State *L)
{
	return luaL_error(L, "hexchat.props is read-only");
}

static int api_hexchat_pluginprefs_meta_index(lua_State *L)
{
	script_info *script = get_info(L);
	if(!script->name)
		return luaL_error(L, "cannot use hexchat.pluginprefs before registering with hexchat.register");
	char const *key = luaL_checkstring(L, 2);
	hexchat_plugin *h = script->handle;
	int r = hexchat_pluginpref_get_int(h, key);
	if(r != -1)
	{
		lua_pushnumber(L, r);
		return 1;
	}
	char str[512];
	if(hexchat_pluginpref_get_str(h, key, str))
	{
		lua_pushstring(L, str);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int api_hexchat_pluginprefs_meta_newindex(lua_State *L)
{
	script_info *script = get_info(L);
	if(!script->name)
		return luaL_error(L, "cannot use hexchat.pluginprefs before registering with hexchat.register");
	char const *key = luaL_checkstring(L, 2);
	hexchat_plugin *h = script->handle;
	switch(lua_type(L, 3))
	{
		case LUA_TSTRING:
			hexchat_pluginpref_set_str(h, key, lua_tostring(L, 3));
			return 0;
		case LUA_TNUMBER:
			hexchat_pluginpref_set_int(h, key, lua_tointeger(L, 3));
			return 0;
		case LUA_TNIL: case LUA_TNONE:
			hexchat_pluginpref_delete(h, key);
			return 0;
		default:
			return luaL_argerror(L, 3, "expected string, number, or nil");
	}
}

static int api_hexchat_pluginprefs_meta_pairs_closure(lua_State *L)
{
	char *dest = lua_touserdata(L, lua_upvalueindex(1));
	hexchat_plugin *h = get_info(L)->handle;
	if(dest && *dest)
	{
		char *key = dest;
		dest = strchr(dest, ',');
		if(dest)
			*(dest++) = 0;
		lua_pushlightuserdata(L, dest);
		lua_replace(L, lua_upvalueindex(1));
		lua_pushstring(L, key);
		int r = hexchat_pluginpref_get_int(h, key);
		if(r != -1)
		{
			lua_pushnumber(L, r);
			return 2;
		}
		char str[512];
		if(hexchat_pluginpref_get_str(h, key, str))
		{
			lua_pushstring(L, str);
			return 2;
		}
		lua_pushnil(L);
		return 2;
	}
	else
		return 0;
}

static int api_hexchat_pluginprefs_meta_pairs(lua_State *L)
{
	script_info *script = get_info(L);
	if(!script->name)
		return luaL_error(L, "cannot use hexchat.pluginprefs before registering with hexchat.register");
	char *dest = lua_newuserdata(L, 4096);
	hexchat_plugin *h = script->handle;
	if(!hexchat_pluginpref_list(h, dest))
		strcpy(dest, "");
	lua_pushlightuserdata(L, dest);
	lua_pushlightuserdata(L, dest);
	lua_pushcclosure(L, api_hexchat_pluginprefs_meta_pairs_closure, 2);
	return 1;
}

static int api_attrs_meta_index(lua_State *L)
{
	hexchat_event_attrs *attrs = *(hexchat_event_attrs **)luaL_checkudata(L, 1, "attrs");
	char const *key = luaL_checkstring(L, 2);
	if(!strcmp(key, "server_time_utc"))
	{
		lua_pushnumber(L, attrs->server_time_utc);
		return 1;
	}
	else
	{
		lua_pushnil(L);
		return 1;
	}
}

static int api_attrs_meta_newindex(lua_State *L)
{
	hexchat_event_attrs *attrs = *(hexchat_event_attrs **)luaL_checkudata(L, 1, "attrs");
	char const *key = luaL_checkstring(L, 2);
	if(!strcmp(key, "server_time_utc"))
	{
		attrs->server_time_utc = luaL_checknumber(L, 3);
		return 0;
	}
	else
		return 0;
}

static int api_attrs_meta_gc(lua_State *L)
{
	hexchat_event_attrs *attrs = *(hexchat_event_attrs **)luaL_checkudata(L, 1, "attrs");
	hexchat_event_attrs_free(ph, attrs);
	return 0;
}

static int api_list_meta_index(lua_State *L)
{
	hexchat_list *list = *(hexchat_list **)luaL_checkudata(L, 1, "list");
	char const *key = luaL_checkstring(L, 2);
	return list_marshal(L, key, list);
}

static int api_list_meta_newindex(lua_State *L)
{
	return luaL_error(L, "hexchat.iterate list is read-only");
}

static int api_list_meta_gc(lua_State *L)
{
	hexchat_list *list = *(hexchat_list **)luaL_checkudata(L, 1, "list");
	hexchat_list_free(ph, list);
	return 0;
}

static luaL_Reg api_hexchat[] = {
	{"register", api_hexchat_register},
	{"command", api_hexchat_command},
	{"print", api_hexchat_print},
	{"emit_print", api_hexchat_emit_print},
	{"emit_print_attrs", api_hexchat_emit_print_attrs},
	{"send_modes", api_hexchat_send_modes},
	{"nickcmp", api_hexchat_nickcmp},
	{"strip", api_hexchat_strip},
	{"get_info", api_hexchat_get_info},
	{"hook_command", api_hexchat_hook_command},
	{"hook_print", api_hexchat_hook_print},
	{"hook_print_attrs", api_hexchat_hook_print_attrs},
	{"hook_server", api_hexchat_hook_server},
	{"hook_server_attrs", api_hexchat_hook_server_attrs},
	{"hook_timer", api_hexchat_hook_timer},
	{"hook_unload", api_hexchat_hook_unload},
	{"unhook", api_hexchat_unhook},
	{"get_context", api_hexchat_get_context},
	{"find_context", api_hexchat_find_context},
	{"set_context", api_hexchat_set_context},
	{"attrs", api_hexchat_attrs},
	{"iterate", api_hexchat_iterate},
	{NULL, NULL}
};

static luaL_Reg api_hexchat_props_meta[] = {
	{"__index", api_hexchat_props_meta_index},
	{"__newindex", api_hexchat_props_meta_newindex},
	{NULL, NULL}
};

static luaL_Reg api_hexchat_prefs_meta[] = {
	{"__index", api_hexchat_prefs_meta_index},
	{"__newindex", api_hexchat_prefs_meta_newindex},
	{NULL, NULL}
};

static luaL_Reg api_hexchat_pluginprefs_meta[] = {
	{"__index", api_hexchat_pluginprefs_meta_index},
	{"__newindex", api_hexchat_pluginprefs_meta_newindex},
	{"__pairs", api_hexchat_pluginprefs_meta_pairs},
	{NULL, NULL}
};

static luaL_Reg api_hook_meta_index[] = {
	{"unhook", api_hexchat_unhook},
	{NULL, NULL}
};

static luaL_Reg api_attrs_meta[] = {
	{"__index", api_attrs_meta_index},
	{"__newindex", api_attrs_meta_newindex},
	{"__gc", api_attrs_meta_gc},
	{NULL, NULL}
};

static luaL_Reg api_list_meta[] = {
	{"__index", api_list_meta_index},
	{"__newindex", api_list_meta_newindex},
	{"__gc", api_list_meta_gc},
	{NULL, NULL}
};

static int luaopen_hexchat(lua_State *L)
{
	lua_newtable(L);
	luaL_setfuncs(L, api_hexchat, 0);

	lua_pushnumber(L, HEXCHAT_PRI_HIGHEST); lua_setfield(L, -2, "PRI_HIGHEST");
	lua_pushnumber(L, HEXCHAT_PRI_HIGH); lua_setfield(L, -2, "PRI_HIGH");
	lua_pushnumber(L, HEXCHAT_PRI_NORM); lua_setfield(L, -2, "PRI_NORM");
	lua_pushnumber(L, HEXCHAT_PRI_LOW); lua_setfield(L, -2, "PRI_LOW");
	lua_pushnumber(L, HEXCHAT_PRI_LOWEST); lua_setfield(L, -2, "PRI_LOWEST");
	lua_pushnumber(L, HEXCHAT_EAT_NONE); lua_setfield(L, -2, "EAT_NONE");
	lua_pushnumber(L, HEXCHAT_EAT_HEXCHAT); lua_setfield(L, -2, "EAT_HEXCHAT");
	lua_pushnumber(L, HEXCHAT_EAT_PLUGIN); lua_setfield(L, -2, "EAT_PLUGIN");
	lua_pushnumber(L, HEXCHAT_EAT_ALL); lua_setfield(L, -2, "EAT_ALL");

	lua_newtable(L);
	lua_newtable(L);
	luaL_setfuncs(L, api_hexchat_prefs_meta, 0);
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "prefs");

	lua_newtable(L);
	lua_newtable(L);
	luaL_setfuncs(L, api_hexchat_props_meta, 0);
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "props");

	lua_newtable(L);
	lua_newtable(L);
	luaL_setfuncs(L, api_hexchat_pluginprefs_meta, 0);
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "pluginprefs");

	luaL_newmetatable(L, "hook");
	lua_newtable(L);
	luaL_setfuncs(L, api_hook_meta_index, 0);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, "context");
	lua_newtable(L);
	lua_pushcfunction(L, api_hexchat_set_context);
	lua_setfield(L, -2, "set");
	wrap_context(L, "find_context", api_hexchat_find_context);
	wrap_context(L, "print", api_hexchat_print);
	wrap_context(L, "emit_print", api_hexchat_emit_print);
	wrap_context(L, "emit_print_attrs", api_hexchat_emit_print_attrs);
	wrap_context(L, "command", api_hexchat_command);
	wrap_context(L, "nickcmp", api_hexchat_nickcmp);
	wrap_context(L, "get_info", api_hexchat_get_info);
	wrap_context(L, "iterate", api_hexchat_iterate);
	lua_setfield(L, -2, "__index");
	lua_pushcfunction(L, api_hexchat_context_meta_eq);
	lua_setfield(L, -2, "__eq");
	lua_pop(L, 1);


	luaL_newmetatable(L, "attrs");
	luaL_setfuncs(L, api_attrs_meta, 0);
	lua_pop(L, 1);

	luaL_newmetatable(L, "list");
	luaL_setfuncs(L, api_list_meta, 0);
	lua_pop(L, 1);

	return 1;
}

static int pairs_closure(lua_State *L)
{
	lua_settop(L, 1);
	if(luaL_getmetafield(L, 1, "__pairs"))
	{
		lua_insert(L, 1);
		lua_call(L, 1, LUA_MULTRET);
		return lua_gettop(L);
	}
	else
	{
		lua_pushvalue(L, lua_upvalueindex(1));
		lua_insert(L, 1);
		lua_call(L, 1, LUA_MULTRET);
		return lua_gettop(L);
	}
}

static void patch_pairs(lua_State *L)
{
	lua_getglobal(L, "pairs");
	lua_pushcclosure(L, pairs_closure, 1);
	lua_setglobal(L, "pairs");
}

static void patch_clibs(lua_State *L)
{
	lua_pushnil(L);
	while(lua_next(L, LUA_REGISTRYINDEX))
	{
		if(lua_type(L, -2) == LUA_TLIGHTUSERDATA && lua_type(L, -1) == LUA_TTABLE)
		{
			lua_setfield(L, LUA_REGISTRYINDEX, "_CLIBS");
			break;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
}

script_info **scripts = NULL;
size_t num_scripts = 0;

static char *expand_buffer = NULL;
static char const *expand_path(char const *path)
{
	if(g_path_is_absolute(path))
		return path;
#ifndef G_OS_WIN32
	if(path[0] == '~')
	{
		if(!path[1] || path[1] == '/')
		{
			if(expand_buffer)
				g_free(expand_buffer);
			expand_buffer = g_build_filename(g_get_home_dir(), path + 1, NULL);
			return expand_buffer;
		}
		else
		{
			char *user = g_strdup(path + 1);
			char *slash_pos = strchr(user, '/');
			if(slash_pos)
				*slash_pos = 0;
			struct passwd *pw = getpwnam(user);
			g_free(user);
			if(pw)
			{
				slash_pos = strchr(path, '/');
				if(!slash_pos)
					return pw->pw_dir;
				if(expand_buffer)
					g_free(expand_buffer);
				expand_buffer = g_strconcat(pw->pw_dir, slash_pos, NULL);
				return expand_buffer;
			}
			else
			{
				return path;
			}
		}
	}
	else
#endif
	{
		if(expand_buffer)
			g_free(expand_buffer);
		expand_buffer = g_build_filename(hexchat_get_info(ph, "configdir"), "addons", path, NULL);
		return expand_buffer;
	}
}

static int is_lua_file(char const *file)
{
	char const *ext1 = ".lua";
	char const *ext2 = ".luac";
	return (strlen(file) >= strlen(ext1) && !strcmp(file + strlen(file) - strlen(ext1), ext1)) || (strlen(file) >= strlen(ext2) && !strcmp(file + strlen(file) - strlen(ext2), ext2));
}

static void prepare_state(lua_State *L, script_info *info)
{
	luaL_openlibs(L);
	if(LUA_VERSION_NUM < 502)
		patch_pairs(L);
	if(LUA_VERSION_NUM > 502)
		patch_clibs(L);
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	info->traceback = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_pop(L, 1);
	lua_pushlightuserdata(L, info);
	lua_setfield(L, LUA_REGISTRYINDEX, registry_field);
	luaopen_hexchat(L);
	lua_setglobal(L, "hexchat");
	lua_getglobal(L, "hexchat");
	lua_getfield(L, -1, "print");
	lua_setglobal(L, "print");
	lua_pop(L, 1);
}

static script_info *create_script(char const *file)
{
	script_info *info = malloc(sizeof(script_info));
	info->name = info->description = info->version = NULL;
	info->handle = NULL;
	info->status = 0;
	info->hooks = NULL;
	info->num_hooks = 0;
	info->unload_hooks = NULL;
	info->num_unload_hooks = 0;
	info->filename = g_strdup(expand_path(file));
	lua_State *L = luaL_newstate();
	info->state = L;
	if(!L)
	{
		g_free(info->filename);
		free(info);
		return NULL;
	}
	prepare_state(L, info);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->traceback);
	int base = lua_gettop(L);
	char *filename_fs = g_filename_from_utf8(info->filename, -1, NULL, NULL, NULL);
	if(!filename_fs)
	{
		hexchat_printf(ph, "Invalid filename: %s", info->filename);
		lua_close(L);
		g_free(info->filename);
		free(info);
		return NULL;
	}
	if(luaL_loadfile(L, filename_fs))
	{
		g_free(filename_fs);
		hexchat_printf(ph, "Lua syntax error: %s", luaL_optstring(L, -1, ""));
		lua_close(L);
		g_free(info->filename);
		free(info);
		return NULL;
	}
	g_free(filename_fs);
	info->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 0, 0, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "Lua error: %s", error ? error : "(non-string error)");
		size_t i;
		for(i = 0; i < info->num_hooks; i++)
			free_hook(info->hooks[i]);
		for(i = 0; i < info->num_unload_hooks; i++)
			free_hook(info->unload_hooks[i]);
		lua_close(L);
		if(info->name)
		{
			g_free(info->name);
			g_free(info->description);
			g_free(info->version);
			hexchat_plugingui_remove(ph, info->handle);
		}
		g_free(info->filename);
		free(info);
		return 0;
	}
	lua_pop(L, 1);
	if(!info->name)
	{
		hexchat_printf(ph, "Lua script didn't register with hexchat.register");
		size_t i;
		for(i = 0; i < info->num_hooks; i++)
			free_hook(info->hooks[i]);
		for(i = 0; i < info->num_unload_hooks; i++)
			free_hook(info->unload_hooks[i]);
		lua_close(L);
		g_free(info->filename);
		free(info);
		return 0;
	}
	return info;
}

static void destroy_script(script_info *info)
{
	size_t i;
	for(i = 0; i < info->num_hooks; i++)
		free_hook(info->hooks[i]);
	lua_State *L = info->state;
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->traceback);
	int base = lua_gettop(L);
	for(i = 0; i < info->num_unload_hooks; i++)
	{
		hook_info *hook = info->unload_hooks[i];
		lua_rawgeti(L, LUA_REGISTRYINDEX, hook->ref);
		if(lua_pcall(L, 0, 0, base))
		{
			char const *error = lua_tostring(L, -1);
			lua_pop(L, 2);
			hexchat_printf(ph, "Lua error in unload hook: %s", error ? error : "(non-string error)");
		}
		free_hook(hook);
	}
	lua_close(L);
	g_free(info->filename);
	g_free(info->name);
	g_free(info->description);
	g_free(info->version);
	hexchat_plugingui_remove(ph, info->handle);
	free(info);
}

static void load_script(char const *file)
{
	script_info *info = create_script(file);
	if(info)
	{
		ARRAY_GROW(scripts, num_scripts);
		scripts[num_scripts - 1] = info;
		check_deferred(info);
	}
}

static int unload_script(char const *filename)
{
	size_t i;
	char const *expanded = expand_path(filename);
	for(i = 0; i < num_scripts; i++)
		if(!strcmp(scripts[i]->filename, expanded))
		{
			if(scripts[i]->status & STATUS_ACTIVE)
				scripts[i]->status |= STATUS_DEFERRED_UNLOAD;
			else
			{
				destroy_script(scripts[i]);
				size_t j;
				for(j = i; j < num_scripts - 1; j++)
					scripts[j] = scripts[j + 1];
				ARRAY_SHRINK(scripts, num_scripts);
			}
			return 1;
		}
	return 0;
}

static int reload_script(char const *filename)
{
	size_t i;
	char const *expanded = expand_path(filename);
	for(i = 0; i < num_scripts; i++)
		if(!strcmp(scripts[i]->filename, expanded))
		{
			if(scripts[i]->status & STATUS_ACTIVE)
				scripts[i]->status |= STATUS_DEFERRED_RELOAD;
			else
			{
				destroy_script(scripts[i]);
				size_t j;
				for(j = i; j < num_scripts - 1; j++)
					scripts[j] = scripts[j + 1];
				ARRAY_SHRINK(scripts, num_scripts);
				load_script(filename);
			}
			return 1;
		}
	return 0;
}

static void autoload_scripts(void)
{
	char *path = g_build_filename(hexchat_get_info(ph, "configdir"), "addons", NULL);
	GDir *dir = g_dir_open(path, 0, NULL);
	if(dir)
	{
		char const *filename;
		while((filename = g_dir_read_name(dir)))
			if(is_lua_file(filename))
				load_script(filename);
		g_dir_close(dir);
	}
	g_free(path);
}

script_info *interp = NULL;
static void create_interpreter(void)
{
	interp = malloc(sizeof(script_info));
	interp->name = "lua interpreter";
	interp->description = "";
	interp->version = "";
	interp->handle = ph;
	interp->status = 0;
	interp->hooks = NULL;
	interp->num_hooks = 0;
	interp->unload_hooks = NULL;
	interp->num_unload_hooks = 0;
	interp->filename = "";
	lua_State *L = luaL_newstate();
	interp->state = L;
	if(!L)
	{
		free(interp);
		interp = NULL;
		return;
	}
	prepare_state(L, interp);
}

static void destroy_interpreter(void)
{
	if(interp)
	{
		size_t i;
		for(i = 0; i < interp->num_hooks; i++)
			free_hook(interp->hooks[i]);
		lua_State *L = interp->state;
		lua_rawgeti(L, LUA_REGISTRYINDEX, interp->traceback);
		int base = lua_gettop(L);
		for(i = 0; i < interp->num_unload_hooks; i++)
		{
			hook_info *hook = interp->unload_hooks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, hook->ref);
			if(lua_pcall(L, 0, 0, base))
			{
				char const *error = lua_tostring(L, -1);
				lua_pop(L, 2);
				hexchat_printf(ph, "Lua error in unload hook: %s", error ? error : "(non-string error)");
			}
			free_hook(hook);
		}
		lua_close(L);
		free(interp);
		interp = NULL;
	}
}

static void inject_string(script_info *info, char const *line)
{
	lua_State *L = info->state;
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->traceback);
	int base = lua_gettop(L);
	if(luaL_loadbuffer(L, line, strlen(line), "@interpreter"))
	{
		hexchat_printf(ph, "Lua syntax error: %s", luaL_optstring(L, -1, ""));
		lua_pop(L, 2);
		return;
	}
	info->status |= STATUS_ACTIVE;
	if(lua_pcall(L, 0, LUA_MULTRET, base))
	{
		char const *error = lua_tostring(L, -1);
		lua_pop(L, 2);
		hexchat_printf(ph, "Lua error: %s", error ? error : "(non-string error)");
		return;
	}
	int top = lua_gettop(L);
	if(top > base)
	{
		luaL_Buffer b;
		luaL_buffinit(L, &b);
		int i;
		for(i = base + 1; i <= top; i++)
		{
			if(i != base + 1)
				luaL_addstring(&b, " ");
			tostring(L, i);
			luaL_addvalue(&b);
		}
		luaL_pushresult(&b);
		hexchat_print(ph, lua_tostring(L, -1));
		lua_pop(L, top - base + 1);
	}
	lua_pop(L, 1);
	check_deferred(info);
}

static int command_load(char *word[], char *word_eol[], void *userdata)
{
	if(is_lua_file(word[2]))
	{
		load_script(word[2]);
		return HEXCHAT_EAT_ALL;
	}
	else
		return HEXCHAT_EAT_NONE;
}

static int command_unload(char *word[], char *word_eol[], void *userdata)
{
	if(unload_script(word[2]))
		return HEXCHAT_EAT_ALL;
	else
		return HEXCHAT_EAT_NONE;
}

static int command_reload(char *word[], char *word_eol[], void *userdata)
{
	if(reload_script(word[2]))
		return HEXCHAT_EAT_ALL;
	else
		return HEXCHAT_EAT_NONE;
}

static int command_console_exec(char *word[], char *word_eol[], void *userdata)
{
	char const *channel = hexchat_get_info(ph, "channel");
	if(channel && !strcmp(channel, console_tab))
	{
		if(interp)
		{
			hexchat_printf(ph, "> %s", word_eol[1]);
			inject_string(interp, word_eol[1]);
		}
		return HEXCHAT_EAT_ALL;
	}
	return HEXCHAT_EAT_NONE;
}

void check_deferred(script_info *info)
{
	info->status &= ~STATUS_ACTIVE;
	if(info->status & STATUS_DEFERRED_UNLOAD)
	{
		size_t i;
		for(i = 0; i < num_scripts; i++)
			if(scripts[i] == info)
			{
				destroy_script(info);
				size_t j;
				for(j = i; j < num_scripts - 1; j++)
					scripts[j] = scripts[j + 1];
				ARRAY_SHRINK(scripts, num_scripts);
			}
	}
	else if(info->status & STATUS_DEFERRED_RELOAD)
	{
		if(info == interp)
		{
			destroy_interpreter();
			create_interpreter();
		}
		else
		{
			size_t i;
			for(i = 0; i < num_scripts; i++)
				if(scripts[i] == info)
				{
					char *filename = g_strdup(info->filename);
					destroy_script(info);
					size_t j;
					for(j = i; j < num_scripts - 1; j++)
						scripts[j] = scripts[j + 1];
					ARRAY_SHRINK(scripts, num_scripts);
					load_script(filename);
					g_free(filename);
					break;
				}
		}
	}
}

static int command_lua(char *word[], char *word_eol[], void *userdata)
{
	if(!strcmp(word[2], "load"))
	{
		load_script(word[3]);
	}
	else if(!strcmp(word[2], "unload"))
	{
		if(!unload_script(word[3]))
			hexchat_printf(ph, "Could not find a script by the name '%s'", word[3]);
	}
	else if(!strcmp(word[2], "reload"))
	{
		if(!reload_script(word[3]))
			hexchat_printf(ph, "Could not find a script by the name '%s'", word[3]);
	}
	else if(!strcmp(word[2], "exec"))
	{
		if(interp)
			inject_string(interp, word_eol[3]);
	}
	else if(!strcmp(word[2], "inject"))
	{
		char const *expanded = expand_path(word[3]);
		size_t i;
		int found = 0;
		for(i = 0; i < num_scripts; i++)
			if(!strcmp(scripts[i]->filename, expanded))
			{
				inject_string(scripts[i], word_eol[4]);
				found = 1;
				break;
			}
		if(!found)
			hexchat_printf(ph, "Could not find a script by the name '%s'", word[3]);
	}
	else if(!strcmp(word[2], "reset"))
	{
		if(interp->status & STATUS_ACTIVE)
			interp->status |= STATUS_DEFERRED_RELOAD;
		else
		{
			destroy_interpreter();
			create_interpreter();
		}
	}
	else if(!strcmp(word[2], "list"))
	{
		size_t i;
		for(i = 0; i < num_scripts; i++)
			hexchat_printf(ph, "%s %s: %s (%s)", scripts[i]->name, scripts[i]->version, scripts[i]->description, scripts[i]->filename);
		if(interp)
			hexchat_printf(ph, "%s %s", interp->name, plugin_version);
	}
	else if(!strcmp(word[2], "console"))
	{
		hexchat_commandf(ph, "query %s", console_tab);
	}
	else
	{
		hexchat_command(ph, "help lua");
	}
	return HEXCHAT_EAT_ALL;
}

G_MODULE_EXPORT int hexchat_plugin_init(hexchat_plugin *plugin_handle, char **name, char **description, char **version, char *arg)
{
	strcat(plugin_version, strchr(LUA_VERSION, ' ') + 1);

	*name = plugin_name;
	*description = plugin_description;
	*version = plugin_version;

	ph = plugin_handle;

	hexchat_hook_command(ph, "", HEXCHAT_PRI_NORM, command_console_exec, NULL, NULL);
	hexchat_hook_command(ph, "LOAD", HEXCHAT_PRI_NORM, command_load, NULL, NULL);
	hexchat_hook_command(ph, "UNLOAD", HEXCHAT_PRI_NORM, command_unload, NULL, NULL);
	hexchat_hook_command(ph, "RELOAD", HEXCHAT_PRI_NORM, command_reload, NULL, NULL);
	hexchat_hook_command(ph, "lua", HEXCHAT_PRI_NORM, command_lua, command_help, NULL);

	hexchat_printf(ph, "%s version %s loaded.\n", plugin_name, plugin_version);

	create_interpreter();

	if(!arg)
		autoload_scripts();
	return 1;
}

G_MODULE_EXPORT int hexchat_plugin_deinit(hexchat_plugin *plugin_handle)
{
	size_t i;
	int found = 0;
	for(i = 0; i < num_scripts; i++)
		if(scripts[i]->status & STATUS_ACTIVE)
		{
			found = 1;
			break;
		}
	if(!found && interp && interp->status & STATUS_ACTIVE)
		found = 1;
	if(found)
	{
		hexchat_print(ph, "\00304Cannot unload the lua plugin while there are active states");
		return 0;
	}
	destroy_interpreter();
	for(i = 0; i < num_scripts; i++)
		destroy_script(scripts[i]);
	num_scripts = 0;
	ARRAY_RESIZE(scripts, num_scripts);
	if(expand_buffer)
		g_free(expand_buffer);
	return 1;
}

