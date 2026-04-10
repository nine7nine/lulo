CC ?= gcc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra
NC_CFLAGS := $(shell $(PKG_CONFIG) --cflags notcurses)
NC_LIBS := $(shell $(PKG_CONFIG) --libs notcurses)

.PHONY: all clean

all: lulo nc_input_probe

lulo: lulo.c lulo_model.c lulo_model.h lulo_proc.c lulo_proc.h lulo_dizk.c lulo_dizk.h
	$(CC) $(CFLAGS) $(NC_CFLAGS) -o $@ lulo.c lulo_model.c lulo_proc.c lulo_dizk.c $(NC_LIBS) -lm

nc_input_probe: nc_input_probe.c
	$(CC) $(CFLAGS) $(NC_CFLAGS) -o $@ nc_input_probe.c $(NC_LIBS)

clean:
	rm -f lulo nc_input_probe
