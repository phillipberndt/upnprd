CFLAGS=-O3 -Wall -DTHREADS

LIBS=
ifneq ($(subst -DTHREADS,,$(CFLAGS)),$(CFLAGS))
LIBS+=-lpthread
endif

upnprd: upnprd.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f upnprd
