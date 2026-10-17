// Linux/GCC replacement for nfsmw's src/types.h. Put this directory first on the include path
// and build the reused nfsmw sources with -DEA_PLATFORM_WIN32 (their PC code path).
#ifndef _TYPES_H_
#define _TYPES_H_

#include <cstddef>
#include <cstdint>

typedef unsigned char Bool;
typedef int BOOL;

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef unsigned int type_operator_new;

typedef uint32_t u32;
typedef int32_t i32;
typedef uint16_t u16;
typedef int16_t i16;
typedef uint8_t u8;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef float f32;
typedef double f64;

#ifndef ATTRIBUTE_ALIGN
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))
#endif
#define ALIGN_16 ATTRIBUTE_ALIGN(16)
#define EA_PACKED __attribute__((packed))
#ifndef AT_ADDRESS
#define AT_ADDRESS(xyz)
#endif
#define __declspec(x)

#define ONLINE_SUPPORT (0)
#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#endif
