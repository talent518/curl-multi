CC := gcc
PKG_CONFIG := pkg-config
CFLAGS := -g $(shell $(PKG_CONFIG) --cflags libcurl)
LFLAGS := $(shell $(PKG_CONFIG) --libs libcurl) -Wl,-rpath,$(shell $(PKG_CONFIG) --variable=libdir libcurl)

all: curl-multi
	@echo -n

curl-multi: main.o
	@echo LD $@
	@$(CC) -o $@ $^ $(LFLAGS)

%.o: %.c
	@echo CC $@
	@$(CC) $(CFLAGS) -c -o $@ $<

clean:
	@LANG=en rm -vf *.o curl-multi
