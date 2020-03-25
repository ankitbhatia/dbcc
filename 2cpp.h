#ifndef _2CPP_H
#define _2CPP_H

#ifdef __cplusplus
extern "C" {
#endif

#include "can.h"
#include "2c.h"
#include <stdbool.h>

// typedef struct {
// 	bool use_time_stamps;
// 	bool use_doubles_for_encoding;
// 	bool generate_print, generate_pack, generate_unpack;
// 	bool generate_asserts;
// } dbc2cpp_options_t;

int dbc2cpp(dbc_t *dbc, FILE *c, FILE *h, const char *name, dbc2c_options_t *copts);

#ifdef __cplusplus
}
#endif

#endif
