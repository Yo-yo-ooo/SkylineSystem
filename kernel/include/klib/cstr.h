#pragma once

#include <stdint.h>
#include <stddef.h>
#include <klib/str.h>

#ifdef __cplusplus
const char* to_string(uint64_t value);

const char* to_string(int64_t value);

const char* to_string(int32_t value);
#ifdef __x86_64__
const char* to_string(double value, uint8_t places);

const char* to_string(double value);
#endif
const char* to_string(bool value);

const char* to_string(char value);

int64_t to_int(const char* string);

uint32_t ConvertStringToHex(const char* data);

const char* ConvertHexToString(uint64_t hex, uint8_t size);
const char* ConvertHexToString(uint64_t hex);
const char* ConvertHexToString(uint32_t hex);
const char* ConvertHexToString(uint16_t hex);
const char* ConvertHexToString(uint8_t hex);
unsigned long ConvertStringToLongHex(const char* data);

char* StrCombine(const char* a, const char* b);

#endif
