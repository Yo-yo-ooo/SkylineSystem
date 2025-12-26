#pragma once
#ifndef _HLLIB_SOFT_FLOAT_POINT_H
#define _HLLIB_SOFT_FLOAT_POINT_H

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <common/floatx.hpp>

using fp64 = flx::floatx<11, 52>;
using fp32 = flx::floatx<8, 23>;

#endif