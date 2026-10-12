#define _GNU_SOURCE
#include "ui.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_out_color;
static int g_err_color;
static int g_initialised;

void ui_init(void)
{
	if (g_initialised)
		return;
	g_initialised = 1;

	/* NO_COLOR (any value) and dumb terminals switch colours off; otherwise
	 * they follow the stream: a terminal gets them, a pipe or a file does
	 * not, so redirected tables and JSON stay clean. */
	if (getenv("NO_COLOR")) {
		g_out_color = g_err_color = 0;
		return;
	}
	const char *term = getenv("TERM");

	if (term && strcmp(term, "dumb") == 0) {
		g_out_color = g_err_color = 0;
		return;
	}
	g_out_color = isatty(STDOUT_FILENO);
	g_err_color = isatty(STDERR_FILENO);
}

/*
 * Escape sequences must outlive the call: callers store them in table cells, so
 * each distinct code gets its own permanent slot (a single shared buffer would
 * make every cell end up with whichever style was requested last).
 */
static const char *style(int enabled, const char *sgr)
{
	static struct {
		const char *code;
		char esc[16];
	} cache[16];
	static size_t used;

	if (!enabled || !sgr || !*sgr)
		return "";
	for (size_t i = 0; i < used; i++)
		if (strcmp(cache[i].code, sgr) == 0)
			return cache[i].esc;
	if (used == sizeof(cache) / sizeof(cache[0]))
		return "";
	cache[used].code = sgr;
	snprintf(cache[used].esc, sizeof(cache[used].esc), "\033[%sm", sgr);
	return cache[used++].esc;
}

const char *ui_c(const char *sgr)
{
	ui_init();
	return style(g_out_color, sgr);
}

const char *ui_ec(const char *sgr)
{
	ui_init();
	return style(g_err_color, sgr);
}

const char *ui_rssi_color(int rssi)
{
	if (rssi >= -60)
		return UI_GREEN;
	if (rssi >= -80)
		return UI_YELLOW;
	return UI_RED;
}

static void styled(const char *sgr, const char *text)
{
	if (sgr && *sgr)
		printf("%s%s%s", sgr, text, UI_RESET);
	else
		fputs(text, stdout);
}

void ui_table(const struct ui_col *cols, size_t ncols, const struct ui_cell *cells,
	      size_t nrows)
{
	size_t *width;

	if (ncols == 0)
		return;
	width = calloc(ncols, sizeof(*width));
	if (!width)
		return;

	for (size_t c = 0; c < ncols; c++)
		width[c] = strlen(cols[c].header);
	for (size_t r = 0; r < nrows; r++)
		for (size_t c = 0; c < ncols; c++) {
			const char *text = cells[r * ncols + c].text;
			size_t len = text ? strlen(text) : 0;

			if (len > width[c])
				width[c] = len;
		}

	size_t rule = ncols - 1;

	for (size_t c = 0; c < ncols; c++)
		rule += width[c];
	for (size_t i = 0; i < rule; i++)
		putchar('-');
	printf("\n");

	for (size_t c = 0; c < ncols; c++) {
		if (c)
			fputs("  ", stdout);
		printf("%s", UI_BOLD);
		if (c + 1 < ncols)
			printf("%-*s", (int)width[c], cols[c].header);
		else
			fputs(cols[c].header, stdout);
		fputs(UI_RESET, stdout);
	}
	putchar('\n');

	for (size_t r = 0; r < nrows; r++) {
		for (size_t c = 0; c < ncols; c++) {
			const struct ui_cell *cell = &cells[r * ncols + c];
			const char *text = cell->text ? cell->text : "";
			int pad = (int)(width[c] - strlen(text));

			if (c)
				fputs("  ", stdout);
			if (cols[c].right_align) {
				printf("%*s", pad, "");
				styled(cell->sgr, text);
			} else {
				styled(cell->sgr, text);
				printf("%*s", pad, "");
			}
		}
		putchar('\n');
	}
	for (size_t i = 0; i < rule; i++)
		putchar('-');
	putchar('\n');
	free(width);
}

const char *ui_short_home(const char *path, char *buf, size_t size)
{
	const char *home = getenv("HOME");

	if (!path)
		return "";
	if (home && *home && strncmp(path, home, strlen(home)) == 0)
		snprintf(buf, size, "~%s", path + strlen(home));
	else
		snprintf(buf, size, "%s", path);
	return buf;
}

const char *ui_bytes(unsigned long long bytes, char *buf, size_t size)
{
	static const char *const units[] = { "B", "KiB", "MiB", "GiB" };
	double value = (double)bytes;
	size_t unit = 0;

	while (value >= 1024.0 && unit + 1 < sizeof(units) / sizeof(units[0])) {
		value /= 1024.0;
		unit++;
	}
	if (unit == 0)
		snprintf(buf, size, "%llu B", bytes);
	else
		snprintf(buf, size, "%.1f %s", value, units[unit]);
	return buf;
}
