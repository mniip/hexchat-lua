#include <stdlib.h>
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
	char *name;
	char *description;
	char *version;
	void *handle;
	char *filename;
	lua_State *state;
	hexchat_hook **hooks;
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

int api_hexchat_register(lua_State *L)
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

luaL_reg api_hexchat[] = {
	{"register", api_hexchat_register}
};

int luaopen_hexchat(lua_State *L)
{
	lua_newtable(L);
	luaL_register(L, NULL, api_hexchat);
	return 1;
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
	lua_pushlightuserdata(L, info);
	lua_setfield(L, LUA_REGISTRYINDEX, registry_field);
	luaopen_hexchat(L);
	lua_setglobal(L, "hexchat");
	lua_getglobal(L, "debug");
	lua_getfield(L, -1, "traceback");
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
		hexchat_unhook(ph, info->hooks[i]);
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

