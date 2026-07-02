# tools/sshtest/Makefile — host-side test harness for the Term SSH client.
# Pure cores (port/vt100.c, port/ssh.c) compile on the host unchanged.
CC      ?= cc
PORT     = ../../port
MONO     = ../../lib/monocypher
CFLAGS   = -std=c11 -Wall -Wextra -g -fsanitize=address,undefined -I$(PORT) -I$(MONO)

.PHONY: all vt crypto ssh clean
all: vt crypto

vt: vt_test
	./vt_test

crypto: crypto_test
	./crypto_test

vt_test: vt_test.c $(PORT)/vt100.c $(PORT)/vt100.h
	$(CC) $(CFLAGS) vt_test.c $(PORT)/vt100.c -o vt_test

crypto_test: crypto_test.c $(PORT)/sha256.c $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c
	$(CC) $(CFLAGS) crypto_test.c $(PORT)/sha256.c $(MONO)/monocypher.c $(MONO)/monocypher-ed25519.c -o crypto_test

clean:
	rm -f vt_test crypto_test ssh_test
