#ifndef __CC_H__
#define __CC_H__

#include <klib/kprintf.h>

#define LWIP_NO_STDINT_H  0

#define U16_F "hu"
#define S16_F "d"
#define X16_F "hx"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "uz"



/* 选择小端模式 */
#define BYTE_ORDER LITTLE_ENDIAN

/* define compiler specific symbols */
#if defined (__ICCARM__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_USE_INCLUDES

#elif defined (__CC_ARM)

#define PACK_STRUCT_BEGIN __packed
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#elif defined (__GNUC__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__ ((__packed__))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#elif defined (__TASKING__)

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

#endif


#define LWIP_PLATFORM_ASSERT(x) do {kprintf(x);}while(0)
#define LWIP_PLATFORM_DIAG(x) LWIP_PLATFORM_ASSERT(x)
#define LWIP_PLATFORM_ASSERT(x) do {kprintf("Assertion \"%s\" failed at line %d in %s\n", \
                                     x, __LINE__, __FILE__);} while(0)

extern uint32_t sys_now(void);

 #endif /* __CC_H__ */