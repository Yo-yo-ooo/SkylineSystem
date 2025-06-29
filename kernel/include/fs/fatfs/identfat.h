#pragma once

#ifndef _IDENTFAT_H
#define _IDENTFAT_H

#include <stdint.h>
#include <stddef.h>

#include <partition/mbrgpt.h>
#include <partition/identfstype.h>

FS_TYPE IdentifyFat(uint32_t DriverID,uint32_t PartitionID,bool IsDebug);

#endif