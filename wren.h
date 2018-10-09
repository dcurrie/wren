/** @file wren.h
*
* @brief Wren configuration and API
*
* @par
* @copyright Copyright © 2018 Doug Currie, Londonderry, NH, USA
*/

#ifndef WREN_H
#define WREN_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>

/* ############## Configuration ############## */

/** @def WREN_UNALIGNED_ACCESS_OK
* @brief non-zero enables reads and writes of multi-byte values from/to unaligned addresses.
* Default to safe mode: no unaligned accesses.
*/
#ifndef WREN_UNALIGNED_ACCESS_OK
#define WREN_UNALIGNED_ACCESS_OK (0)
#endif

/** @def WREN_STANDALONE
* @brief non-zero enables main() with command line config and read-eval-print loop.
* Default to standalone; set to 0 if you want to use Wren as a library.
*/
#ifndef WREN_STANDALONE
#define WREN_STANDALONE (1)
#endif


/* ################## Types ################## */

/** Type of a Wren-language value. Must be capable of holding a pointer or an integer
*/
typedef intptr_t wValue;
/* and how to printf it
*/
#define PRVAL "ld"

/** Type of the unsigned version of a Wren-language value.
*/
typedef uintptr_t wUvalu; 

/** Size of a Wren-language value; needed for preprocessor so sizeof() not sufficient.
*/
#if (INTPTR_MAX == INT64_MAX)
#define SIZEOF_WVALUE (8)
#else
#if (INTPTR_MAX == INT32_MAX)
#define SIZEOF_WVALUE (4)
#else
#error "Cannot determine wValue size."
#endif
#endif

// TODO: wIndex


/* ################### API ################### */

typedef wValue (*apply_t)(); // the type of C functions for CCALL and wren_bind_c_function()

void wren_initialize (void);

void wren_bind_c_function (const char *name, apply_t fn, const uint8_t arity);

void wren_load_file (FILE *fp);

void wren_read_eval_print_loop (void);

#ifdef __cplusplus
}
#endif

#endif /* WREN_H */
