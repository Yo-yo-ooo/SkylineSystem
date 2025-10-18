#pragma once
#ifndef x86_MEMCPY_H
#define x86_MEMCPY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__


#else
#error "You include x86_64 ARCH file,but your ARCH not x86_64"
#endif

#endif