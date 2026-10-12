#include "tty_params.h"

#include <stdio.h>

int tty_params_equal(const struct tty_params *a, const struct tty_params *b)
{
	return a->baud == b->baud && a->databits == b->databits &&
	       a->stopbits == b->stopbits && a->parity == b->parity &&
	       a->flow == b->flow && a->dtr == b->dtr && a->rts == b->rts;
}

void tty_params_format(const struct tty_params *p, char *buf, size_t size)
{
	static const char parity_char[] = { 'N', 'O', 'E', 'M', 'S' };
	char parity = '?';

	if (p->parity >= 0 && p->parity <= 4)
		parity = parity_char[p->parity];

	snprintf(buf, size, "%d %d%c%d%s DTR=%d RTS=%d", p->baud, p->databits,
		 parity, p->stopbits, p->flow ? " flow" : "", p->dtr, p->rts);
}
