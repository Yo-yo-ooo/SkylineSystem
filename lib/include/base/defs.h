#pragma once

#ifndef _DEFS_H_
#define _DEFS_H_

#ifdef __GNUC__
#define PACK( __Declaration__ ) __Declaration__ __attribute__((packed))
#define ReDefFunction(x) __attribute__((weak, alias(x)))
#define WeakFunction __attribute__((weak))
#endif

#ifdef _MSC_VER
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#define ReDefFunction(x) 
#endif

#endif