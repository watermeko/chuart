#ifndef CHUART_UI_H
#define CHUART_UI_H

#include <stddef.h>
#include <stdio.h>

/*
 * Terminal output helpers: colours and a small aligned-table printer, so the
 * command output is readable at a glance.  Colours are on by default in a
 * terminal and off when the output is redirected (NO_COLOR also disables them).
 */
void ui_init(void);

/* Style escapes for stdout and for stderr (logs); empty when colours are off. */
const char *ui_c(const char *sgr);
const char *ui_ec(const char *sgr);

#define UI_RESET  ui_c("0")
#define UI_BOLD   ui_c("1")
#define UI_DIM    ui_c("2")
#define UI_CYAN   ui_c("36")
#define UI_GREEN  ui_c("32")
#define UI_YELLOW ui_c("33")
#define UI_RED    ui_c("31")
#define UI_BLUE   ui_c("34")

/* RSSI colouring: strong green, weak red. */
const char *ui_rssi_color(int rssi);

struct ui_col {
	const char *header;
	int right_align;
};

struct ui_cell {
	const char *text;
	const char *sgr; /* NULL for plain */
};

/* Print a table with columns sized to their content, framed by dashed rules so
 * it stands apart from the surrounding messages. */
void ui_table(const struct ui_col *cols, size_t ncols, const struct ui_cell *cells,
	      size_t nrows);

/* "/home/you/x" -> "~/x" (writes into buf, returns buf). */
const char *ui_short_home(const char *path, char *buf, size_t size);

/* Human size for byte counts ("1.5 KiB"), for summaries. */
const char *ui_bytes(unsigned long long bytes, char *buf, size_t size);

#endif /* CHUART_UI_H */
