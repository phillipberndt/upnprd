CFLAGS=-O3 -Wall

upnprd: upnprd.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f upnprd
