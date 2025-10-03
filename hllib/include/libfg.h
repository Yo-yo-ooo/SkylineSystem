#ifndef __LIB_FUNCGEN_H__
#define __LIB_FUNCGEN_H__

#define map1(func, t) (func(t))
#define map2(func, t, ...) (func(t)), map1(func, __VA_ARGS__)
#define map3(func, t, ...) (func(t)), map2(func, __VA_ARGS__)
#define map4(func, t, ...) (func(t)), map3(func, __VA_ARGS__)
#define map5(func, t, ...) (func(t)), map4(func, __VA_ARGS__)
#define map6(func, t, ...) (func(t)), map5(func, __VA_ARGS__)
#define map7(func, t, ...) (func(t)), map6(func, __VA_ARGS__)
#define map8(func, t, ...) (func(t)), map7(func, __VA_ARGS__)
#define map9(func, t, ...) (func(t)), map8(func, __VA_ARGS__)

#define map(nr, ...) map##nr(__VA_ARGS__)
#endif