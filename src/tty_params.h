#ifndef CHUART_TTY_PARAMS_H
#define CHUART_TTY_PARAMS_H

#include <stddef.h>

/*
 * Serial parameters as seen by the client, and (by default) mirrored onto the
 * module's UART.  Shared between the CUSE tty emulation, which decodes them
 * from the kernel termios the client writes, and the BLE layer, which pushes
 * them into the module through CH9140lib.
 */
struct tty_params {
	int baud;      /* 9600, 115200, ... */
	int databits;  /* 5..8 */
	int stopbits;  /* 1..2 */
	int parity;    /* 0 none, 1 odd, 2 even, 3 mark, 4 space */
	int flow;      /* hardware RTS/CTS enabled */
	int dtr;
	int rts;
};

int tty_params_equal(const struct tty_params *a, const struct tty_params *b);

/* "115200 8N1 flow DTR=0 RTS=0" - buf is always NUL terminated. */
void tty_params_format(const struct tty_params *p, char *buf, size_t size);

#endif /* CHUART_TTY_PARAMS_H */
