all:
	gcc $(shell pkg-config --cflags hexchat-plugin) -fPIC -Wall -shared -o lua.so lua.c $(shell pkg-config --libs lua) 
