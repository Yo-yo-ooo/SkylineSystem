#pragma once
#ifndef _DESKTOP_H_
#define _DESKTOP_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>

#ifdef CONFIG_USE_DESKTOP_SUBSYS

#include <basicdraw/basicdraw.h>

class Desktop
{
private:
    BasicDraw bd;
public:
    Desktop(Framebuffer *fb) : bd(fb){}
    ~Desktop();
};



#endif


#endif