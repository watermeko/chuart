#ifndef CHUART_CH9140LIB_H
#define CHUART_CH9140LIB_H

/*
 * Wrapper around the vendor header.
 *
 * CH9140lib.h is not warning-clean under -Wall -Wextra (it declares
 * `int CH9140BleGetBluetoothVer();`, a non-prototype declaration), so its
 * diagnostics are silenced here instead of weakening our own warning set.
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include "lib/CH9140lib.h"
#pragma GCC diagnostic pop

#endif /* CHUART_CH9140LIB_H */
