#pragma once

#ifdef __cplusplus
    template<typename T, typename U>
    auto max(T a, U b) -> decltype(a + b) {
        return a < b ? b : a;
    }
    template <typename T,typename U>
    auto min(T a,U b) -> decltype(a + b){
        return a < b ? a : b;
    }
    template <typename T>
    auto swap(T a,T b) -> decltype(a){
        T tmp = a;
        a = b;
        b = tmp;
    }
#else
    #define max(a,b) (((a) > (b)) ? (a) : (b))
    #define min(a,b) (((a) < (b)) ? (a) : (b))
    #define swap(type,a,b){ \
        type tmp = a;        \
        a = b; \ 
        b = tmp; \
    } \
#endif

