#!/usr/bin/make -f

all : obfuskey eventcap

obfuskey : src/main.c src/keycodes.c src/keycodes.h
	gcc src/main.c src/keycodes.c -o obfuskey -lm $(shell pkg-config --cflags --libs libevdev) $(shell pkg-config --cflags --libs libsodium) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

eventcap : src/eventcap.c
	gcc src/eventcap.c -o eventcap $(CPPFLAGS) $(CFLAGS) $(LDFLAGS)

clean :
	rm -f obfuskey eventcap
