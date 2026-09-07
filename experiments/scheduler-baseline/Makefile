CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS = -lrt

cyclic: cyclic.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f cyclic
