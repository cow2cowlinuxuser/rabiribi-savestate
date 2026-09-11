#ifndef GHIDRA_TYPES_H
#define GHIDRA_TYPES_H

/* Ghidra C dialect so 09_functions_c files can be compiled for PPC. */

#include "host_win.h"
#include <stdbool.h>

typedef unsigned char undefined;
typedef unsigned char undefined1;
typedef unsigned short undefined2;
typedef unsigned int undefined4;
typedef unsigned long long undefined8;
typedef unsigned char byte;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef long long longlong;
typedef unsigned long long ulonglong;
typedef float float10;
typedef void code(void);

#endif
