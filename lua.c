#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <hexchat-plugin.h>


char plugin_name[] = "lua";
char plugin_description[] = "Lua scripting interface";
char plugin_version[256] = "0.0-";

char registry_field[] = "plugin";

hexchat_plugin *ph;

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
	void *handle;
	char *filename;
	lua_State *state;
	int traceback;
	hook_info **hooks;
	size_t num_hooks;
}
script_info;

inline script_info *get_info(lua_State *L)
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
	info->name = strdup(name);
	info->description = strdup(description);
	info->version = strdup(version);
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
	size_t n = lua_objlen(L, 1);
	char const *mode = luaL_checkstring(L, 2);
	if(strlen(mode) != 2)
		return luaL_argerror(L, 2, "expected sign followed by a mode letter");
	int modes = luaL_optint(L, 3, 0);
	const char *targets[n];
	int i;
	for(i = 0; i < n; i++)
	{
		lua_rawgeti(L, 1, i + 1);
		if(lua_type(L, -1) != LUA_TSTRING)
			return luaL_argerror(L, 1, "expected an array of strings");
		targets[i] = lua_tostring(L, -1);
		lua_pop(L, 1);
	}
	hexchat_send_modes(ph, targets, n, modes, mode[0], mode[1]);
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
	int leave_attrs = lua_toboolean(L, 2);
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
	hexchat_unhook(ph, hook->hook);
	free(hook);
}

static int unregister_hook(hook_info *hook)
{
	script_info *info = get_info(hook->state);
	int i;
	for(i = 0; i < info->num_hooks; i++)
		if(info->hooks[i] == hook)
		{
			free_hook(hook);
			int j;
			for(j = info->num_hooks - 1; j > i; j--)
				info->hooks[j - 1] = info->hooks[j];
			ARRAY_SHRINK(info->hooks, info->num_hooks);
			return 1;
		}
	return 0;
}

static int api_command_closure(char *word[], char *word_eol[], void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	lua_rawgeti(L, LUA_REGISTRYINDEX, get_info(L)->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i;
	lua_newtable(L);
	for(i = 1; *word_eol[i]; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	lua_newtable(L);
	for(i = 1; *word_eol[i]; i++)
	{
		lua_pushstring(L, word_eol[i]);
		lua_rawseti(L, -2, i);
	}
	if(lua_pcall(L, 2, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error in command hook: %s", error ? error : "(non-string error)");
		return HEXCHAT_EAT_NONE;
	}
	return lua_tointeger(L, -1);
}

static int api_hexchat_hook_command(lua_State *L)
{
	char const *command = luaL_optstring(L, 1, "");
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	char const *help = luaL_optstring(L, 3, NULL);
	int pri = luaL_optint(L, 4, HEXCHAT_PRI_NORM);
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
	lua_rawgeti(L, LUA_REGISTRYINDEX, get_info(L)->traceback);
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
	if(lua_pcall(L, 1, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error in print hook: %s", error ? error : "(non-string error)");
		return HEXCHAT_EAT_NONE;
	}
	return lua_tointeger(L, -1);
}

static int api_hexchat_hook_print(lua_State *L)
{
	char const *command = luaL_checkstring(L, 1);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optint(L, 3, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_print(ph, command, pri, api_print_closure, info);
	hook_info **u = lua_newuserdata(L, sizeof(hook_info *));
	*u = info;
	luaL_newmetatable(L, "hook");
	lua_setmetatable(L, -2);
	register_hook(info);
	return 1;
}

static int api_print_attrs_closure(char *word[], hexchat_event_attrs *attrs, void *udata)
{
	hook_info *info = udata;
	lua_State *L = info->state;
	lua_rawgeti(L, LUA_REGISTRYINDEX, get_info(L)->traceback);
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
	*u = attrs;
	luaL_newmetatable(L, "attrs");
	lua_setmetatable(L, -2);
	if(lua_pcall(L, 2, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error in print hook: %s", error ? error : "(non-string error)");
		return HEXCHAT_EAT_NONE;
	}
	return lua_tointeger(L, -1);
}

static int api_hexchat_hook_print_attrs(lua_State *L)
{
	char const *command = luaL_checkstring(L, 1);
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optint(L, 3, HEXCHAT_PRI_NORM);
	hook_info *info = malloc(sizeof(hook_info));
	info->state = L;
	info->ref = ref;
	info->hook = hexchat_hook_print_attrs(ph, command, pri, api_print_attrs_closure, info);
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
	lua_rawgeti(L, LUA_REGISTRYINDEX, get_info(L)->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i;
	lua_newtable(L);
	for(i = 1; *word_eol[i]; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	lua_newtable(L);
	for(i = 1; *word_eol[i]; i++)
	{
		lua_pushstring(L, word_eol[i]);
		lua_rawseti(L, -2, i);
	}
	if(lua_pcall(L, 2, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error in server hook: %s", error ? error : "(non-string error)");
		return HEXCHAT_EAT_NONE;
	}
	return lua_tointeger(L, -1);
}

static int api_hexchat_hook_server(lua_State *L)
{
	char const *command = luaL_optstring(L, 1, "RAW LINE");
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optint(L, 3, HEXCHAT_PRI_NORM);
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
	lua_rawgeti(L, LUA_REGISTRYINDEX, get_info(L)->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	int i;
	lua_newtable(L);
	for(i = 1; *word_eol[i]; i++)
	{
		lua_pushstring(L, word[i]);
		lua_rawseti(L, -2, i);
	}
	lua_newtable(L);
	for(i = 1; *word_eol[i]; i++)
	{
		lua_pushstring(L, word_eol[i]);
		lua_rawseti(L, -2, i);
	}
	hexchat_event_attrs **u = lua_newuserdata(L, sizeof(hexchat_event_attrs *));
	*u = attrs;
	luaL_newmetatable(L, "attrs");
	lua_setmetatable(L, -2);
	if(lua_pcall(L, 3, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error in server hook: %s", error ? error : "(non-string error)");
		return HEXCHAT_EAT_NONE;
	}
	return lua_tointeger(L, -1);
}

static int api_hexchat_hook_server_attrs(lua_State *L)
{
	char const *command = luaL_optstring(L, 1, "RAW LINE");
	lua_pushvalue(L, 2);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);
	int pri = luaL_optint(L, 3, HEXCHAT_PRI_NORM);
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
	lua_rawgeti(L, LUA_REGISTRYINDEX, get_info(L)->traceback);
	int base = lua_gettop(L);
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->ref);
	if(lua_pcall(L, 0, 1, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error in timer hook: %s", error ? error : "(non-string error)");
		return 0;
	}
	return lua_toboolean(L, -1);
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

static int api_hexchat_unhook(lua_State *L)
{
	hook_info *info = *(hook_info **)luaL_checkudata(L, 1, "hook");
	unregister_hook(info);
	return 0;
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
	hexchat_set_context(ph, context);
	return 0;
}

int wrap_context_closure(lua_State *L)
{
	hexchat_context *context = *(hexchat_context **)luaL_checkudata(L, 1, "context");
	lua_pushvalue(L, lua_upvalueindex(1));
	lua_replace(L, 1);
	hexchat_context *old = hexchat_get_context(ph);
	hexchat_set_context(ph, context);
	lua_call(L, lua_gettop(L) - 1, LUA_MULTRET);
	hexchat_set_context(ph, old);
	return lua_gettop(L);
}

inline void wrap_context(lua_State *L, char const *field, lua_CFunction func)
{
	lua_pushcfunction(L, func);
	lua_pushcclosure(L, wrap_context_closure, 1);
	lua_setfield(L, -2, field);
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
	return 0;
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

static int api_hexchat_pluginprefs_meta_index(lua_State *L)
{
	char const *key = luaL_checkstring(L, 2);
	char str[512];
	if(hexchat_pluginpref_get_str(ph, key, str))
	{
		lua_pushstring(L, str);
		return 1;
	}
	int r = hexchat_pluginpref_get_int(ph, key);
	if(r != -1)
	{
		lua_pushnumber(L, r);
		return 1;
	}
	lua_pushnil(L);
	return 1;
}

static int api_hexchat_pluginprefs_meta_newindex(lua_State *L)
{
	char const *key = luaL_checkstring(L, 2);
	switch(lua_type(L, 3))
	{
		case LUA_TSTRING:
			hexchat_pluginpref_set_str(ph, key, lua_tostring(L, 3));
			return 0;
		case LUA_TNUMBER:
			hexchat_pluginpref_set_int(ph, key, lua_tointeger(L, 3));
			return 0;
		case LUA_TNIL: case LUA_TNONE:
			hexchat_pluginpref_delete(ph, key);
			return 0;
		default:
			return luaL_argerror(L, 3, "expected string, number, or nil");
	}
}

static int api_hexchat_pluginprefs_meta_pairs_closure(lua_State *L)
{
	char *dest = lua_touserdata(L, lua_upvalueindex(1));
	if(dest && *dest)
	{
		char *key = dest;
		dest = strchr(dest, ',');
		if(dest)
			*(dest++) = 0;
		lua_pushlightuserdata(L, dest);
		lua_replace(L, lua_upvalueindex(1));
		lua_pushstring(L, key);
		char str[512];
		if(hexchat_pluginpref_get_str(ph, key, str))
		{
			lua_pushstring(L, str);
			return 2;
		}
		int r = hexchat_pluginpref_get_int(ph, key);
		if(r != -1)
		{
			lua_pushnumber(L, r);
			return 2;
		}
		lua_pushnil(L);
		return 2;
	}
	else
	{
		free(lua_touserdata(L, lua_upvalueindex(2)));
		return 0;
	}
}

static int api_hexchat_pluginprefs_meta_pairs(lua_State *L)
{
	char *dest = malloc(4096);
	if(!hexchat_pluginpref_list(ph, dest))
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

luaL_reg api_hexchat[] = {
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
	{"unhook", api_hexchat_unhook},
	{"get_context", api_hexchat_get_context},
	{"find_context", api_hexchat_find_context},
	{"set_context", api_hexchat_set_context},
	{"attrs", api_hexchat_attrs}
};

luaL_reg api_hexchat_prefs_meta[] = {
	{"__index", api_hexchat_prefs_meta_index},
	{"__newindex", api_hexchat_prefs_meta_newindex}
};

luaL_reg api_hexchat_pluginprefs_meta[] = {
	{"__index", api_hexchat_pluginprefs_meta_index},
	{"__newindex", api_hexchat_pluginprefs_meta_newindex},
	{"__pairs", api_hexchat_pluginprefs_meta_pairs}
};

luaL_reg api_hook_meta_index[] = {
	{"unhook", api_hexchat_unhook}
};

luaL_reg api_attrs_meta[] = {
	{"__index", api_attrs_meta_index},
	{"__newindex", api_attrs_meta_newindex},
	{"__gc", api_attrs_meta_gc}
};

int luaopen_hexchat(lua_State *L)
{
	lua_newtable(L);
	luaL_register(L, NULL, api_hexchat);

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
	luaL_register(L, NULL, api_hexchat_prefs_meta);
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "prefs");

	lua_newtable(L);
	lua_newtable(L);
	luaL_register(L, NULL, api_hexchat_pluginprefs_meta);
	lua_setmetatable(L, -2);
	lua_setfield(L, -2, "pluginprefs");

	luaL_newmetatable(L, "hook");
	lua_newtable(L);
	luaL_register(L, NULL, api_hook_meta_index);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, "context");
	lua_newtable(L);
	wrap_context(L, "set", api_hexchat_set_context);
	wrap_context(L, "find_context", api_hexchat_find_context);
	wrap_context(L, "print", api_hexchat_print);
	wrap_context(L, "emit_print", api_hexchat_emit_print);
	wrap_context(L, "emit_print_attrs", api_hexchat_emit_print_attrs);
	wrap_context(L, "command", api_hexchat_command);
	wrap_context(L, "get_info", api_hexchat_get_info);
	lua_setfield(L, -2, "__index");
	lua_pop(L, 1);

	luaL_newmetatable(L, "attrs");
	luaL_register(L, NULL, api_attrs_meta);
	lua_pop(L, 1);

	return 1;
}

int pairs_closure(lua_State *L)
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

void patch_pairs(lua_State *L)
{
	lua_getglobal(L, "pairs");
	lua_pushcclosure(L, pairs_closure, 1);
	lua_setglobal(L, "pairs");
}

script_info **scripts = NULL;
size_t num_scripts = 0;

static char *expand_buffer = NULL;
static char const *expand_path(char const *path)
{
	if(path[0] != '/')
	{
		if(path[0] == '~')
		{
			char const *home = getenv("HOME");
			expand_buffer = realloc(expand_buffer, strlen(home) + 1 + strlen(path + 1) + 1);
			strcpy(expand_buffer, home);
			strcat(expand_buffer, "/");
			strcat(expand_buffer, path + 1);
			return expand_buffer;
		}
		else
		{
			char const *configdir = hexchat_get_info(ph, "configdir");
			char const *addons = "addons";
			expand_buffer = realloc(expand_buffer, strlen(configdir) + 1 + strlen(addons) + 1 + strlen(path) + 1);
			strcpy(expand_buffer, configdir);
			strcat(expand_buffer, "/");
			strcat(expand_buffer, addons);
			strcat(expand_buffer, "/");
			strcat(expand_buffer, path);
			return expand_buffer;
		}
	}
	else
		return path;
}

static int is_lua_file(char const *file)
{
	char const *ext1 = ".lua";
	char const *ext2 = ".luac";
	return !strcasecmp(file + strlen(file) - strlen(ext1), ext1) || !strcasecmp(file + strlen(file) - strlen(ext2), ext2);
}

static script_info *create_script(char const *file)
{
	script_info *info = malloc(sizeof(script_info));
	info->name = info->description = info->version = NULL;
	info->hooks = NULL;
	info->num_hooks = 0;
	info->filename = strdup(expand_path(file));
	lua_State *L = luaL_newstate();
	info->state = L;
	if(!L)
	{
		free(info->filename);
		free(info);
		return NULL;
	}
	luaL_openlibs(L);
	if(LUA_VERSION_NUM < 502)
		patch_pairs(L);
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
	info->traceback = luaL_ref(L, LUA_REGISTRYINDEX);
	lua_pushlightuserdata(L, info);
	lua_setfield(L, LUA_REGISTRYINDEX, registry_field);
	luaopen_hexchat(L);
	lua_setglobal(L, "hexchat");
	lua_rawgeti(L, LUA_REGISTRYINDEX, info->traceback);
	int base = lua_gettop(L);
	if(luaL_loadfile(L, info->filename))
	{
		hexchat_printf(ph, "\00320Lua syntax error: %s", luaL_optstring(L, -1, ""));
		lua_close(L);
		free(info->filename);
		free(info);
		return NULL;
	}
	if(lua_pcall(L, 0, 0, base))
	{
		char const *error = lua_tostring(L, -1);
		hexchat_printf(ph, "\00320Lua error: %s", error ? error : "(non-string error)");
		lua_close(L);
		free(info->filename);
		free(info);
		return 0;
	}
	if(!info->name)
	{
		hexchat_printf(ph, "\00320Lua script didn't register with hexchat.register");
		lua_close(L);
		free(info->filename);
		free(info);
		return 0;
	}
	info->handle = hexchat_plugingui_add(ph, info->filename, info->name, info->description, info->version, NULL);
	return info;
}

static void destroy_script(script_info *info)
{
	int i;
	for(i = 0; i < info->num_hooks; i++)
		free_hook(info->hooks[i]);
	lua_State *L = info->state;
	lua_close(L);
	free(info->filename);
	free(info->name);
	free(info->description);
	free(info->version);
	hexchat_plugingui_remove(ph, info->handle);
	free(info);
}

static int load_script(char const *file)
{
	if(is_lua_file(file))
	{
		script_info *info = create_script(file);
		if(info)
		{
			ARRAY_GROW(scripts, num_scripts);
			scripts[num_scripts - 1] = info;
		}
		return 1;
	}
	return 0;
}


static int unload_script(char const *filename)
{
	int i;
	char const *expanded = expand_path(filename);
	for(i = 0; i < num_scripts; i++)
		if(!strcmp(scripts[i]->filename, expanded))
		{
			destroy_script(scripts[i]);
			int j;
			for(j = num_scripts - 1; j > i; j--)
				scripts[j - 1] = scripts[j];
			ARRAY_SHRINK(scripts, num_scripts);
			return 1;
		}
	return 0;
}

static void cleanup_scripts()
{
	int i;
	for(i = 0; i < num_scripts; i++)
		destroy_script(scripts[i]);
	num_scripts = 0;
	ARRAY_RESIZE(scripts, num_scripts);
	free(expand_buffer);
}

static void autoload_scripts()
{
}

static int command_load(char *word[], char *word_eol[], void *userdata)
{
	if(load_script(word[2]))
		return HEXCHAT_EAT_ALL;
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
	if(unload_script(word[2]))
	{
		load_script(word[2]);
		return HEXCHAT_EAT_ALL;
	}
	else
		return HEXCHAT_EAT_NONE;
}

int hexchat_plugin_init(hexchat_plugin *plugin_handle, char **name, char **description, char **version, char *arg)
{
	strcat(plugin_version, strchr(LUA_VERSION, ' ') + 1);

	*name = plugin_name;
	*description = plugin_description;
	*version = plugin_version;

	ph = plugin_handle;

	hexchat_hook_command(ph, "LOAD", HEXCHAT_PRI_NORM, command_load, NULL, NULL);
	hexchat_hook_command(ph, "UNLOAD", HEXCHAT_PRI_NORM, command_unload, NULL, NULL);
	hexchat_hook_command(ph, "RELOAD", HEXCHAT_PRI_NORM, command_reload, NULL, NULL);
	//hexchat_hook_command(ph, "lua", HEXCHAT_PRI_NORM, command_lua, NULL, NULL);

	hexchat_printf(ph, "%s version %s loaded.\n", plugin_name, plugin_version);

	if(!arg)
		autoload_scripts();
	return 1;
}

int hexchat_plugin_deinit(hexchat_plugin *plugin_handle)
{
	cleanup_scripts();
	return 1;
}

