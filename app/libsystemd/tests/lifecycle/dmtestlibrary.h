#ifndef DMTESTLIBRARY_H
#define DMTESTLIBRARY_H
#include <stdbool.h>
#include <stddef.h>
#include "dmtestlibrary_defs.h"
dmod_dmtestlibrary_api(1.0, bool, _active, (const char* unit));
dmod_dmtestlibrary_api(1.0, int, _path, (const char* unit, char* path, size_t capacity));
#endif
