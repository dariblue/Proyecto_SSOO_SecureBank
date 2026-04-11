# ─────────────────────────────────────────────────────────
#  SecureBank — Makefile (Linux con POSIX mqueue + pthreads)
# ─────────────────────────────────────────────────────────
CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=gnu11
LDFLAGS = -pthread -lrt

# Ejecutables principales
TARGETS = banco usuario monitor init_cuentas

.PHONY: all clean

all: $(TARGETS)

banco: banco.c banco_comun.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

usuario: usuario.c banco_comun.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

monitor: monitor.c banco_comun.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

init_cuentas: init_cuentas.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGETS)
	rm -f cuentas.dat transacciones.log
