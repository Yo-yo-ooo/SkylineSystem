//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

#ifndef _LIBMATH_H_
#define _LIBMATH_H_

#ifdef __cplusplus
extern "C" {
#endif

float ceilf(float x);
float  sqrtf(float x);
float  sinf(float x);
float  cosf(float x);
float  tanf(float x);
float  fabsf(float x);
float  atanf(float x);
float  atan2f(float y, float x);
float  floorf(float x);
float  fmodf(float a, float b);
float  acosf(float x);
float  logf(float x);
float  expf(float x);
float  powf(float x, float y);

double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double fabs(double x);
double atan(double x);
double atan2(double y, double x);
double floor(double x);
double fmod(double a, double b);
double acos(double x);
double log(double x);
double exp(double x);
double pow(double x, double y);
double ceil(double x);

#ifdef __cplusplus
}
#endif

#endif