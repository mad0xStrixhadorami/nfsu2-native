// Force-included (-include) for reused nfsmw sources when building for Linux.
#pragma once
#include <math.h>
#include <string.h>
#include <stdlib.h>
#ifndef M_TWOPI
#define M_TWOPI (2.0 * M_PI)
#endif
// EA's debug link to a developer PC ("bFunk"). Not used natively; defined as a no-op.
void bFunkCallASync(const char *server_name, int function_num, const void *data, int size);

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
