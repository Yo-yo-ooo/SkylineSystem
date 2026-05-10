/*
* SPDX-License-Identifier: GPL-2.0-only
* File: string.h
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#pragma once
#ifndef SKYLINE_LIB_STRING_H
#define SKYLINE_LIB_STRING_H

#include <stdint.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C"{
#endif

uint8_t strcmp(const char *cs, const char *ct);
char *strtok(char *str, const char *delim);
char* strchr(const char* str, int32_t c);
char *strcpy(char *strDest, const char *strSrc);
int32_t strncmp(const char* a, const char* b, size_t n);
char *strncpy(char *dest, const char *src, size_t n);
char* strcat(char* dest, const char* source);
size_t strlen(const char*__restrict__  str);
int32_t atoi(char *str);

void *memcpy(void *str1, const void *str2, size_t n);
void *memset(void *str, int c, size_t n);
void *memmove(void *str1, const void *str2, size_t n);


#ifdef __cplusplus
}
#endif

#endif