#pragma once

#include <stdint.h>
#include <stddef.h>

const char* to_string(uint64_t value);

const char* to_string(int64_t value);

const char* to_string(int value);

const char* to_string(double value, uint8_t places);

const char* to_string(double value);

const char* to_string(bool value);

const char* to_string(char value);

int64_t to_int(const char* string);

unsigned int ConvertStringToHex(const char* data);

const char* ConvertHexToString(uint64_t hex, uint8_t size);
const char* ConvertHexToString(uint64_t hex);
const char* ConvertHexToString(uint32_t hex);
const char* ConvertHexToString(uint16_t hex);
const char* ConvertHexToString(uint8_t hex);
unsigned long ConvertStringToLongHex(const char* data);

int strcmp(const char *cs, const char *ct);
char *strtok(char *str, const char *delim);
char* strchr(char* str, int c);
char *strcpy(char *strDest, const char *strSrc);
int strncmp(const char* a, const char* b, size_t n);
char *strncpy(char *dest, const char *src, size_t n);