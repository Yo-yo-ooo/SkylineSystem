// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT

#include <stdint.h>


static __thread int __errno = 0;

int *_errno(){
    return &__errno;    
}