#pragma once
#ifndef _PAIR_H_
#define _PAIR_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>


namespace std_{
    template <class T1, class T2>
    struct pair
    {
        typedef T1 first_type;
        typedef T2 second_type;

        T1 first;
        T2 second;

        //默认构造函数
        pair()
            :first(T1()), second(T2()) {}  //T1()用0初始化 T2()用0初始化

        //构造函数
        pair(const T1& a, const T2& b)
            :first(a), second(b) {}

        //拷贝构造函数
        template<class U, class V>
        pair(const pair<U, V>& p)
            : first(p.first), second(p.second) {
            ;
        }//{创建调用了copy构造函数}
    };


    template<class T1, class T2>
    bool operator==(const pair<T1, T2>& s1, const pair<T1, T2>& s2)
    {
        return s1.first == s2.first && s1.second == s2.second;
    }

    template<class T1, class T2>
    bool operator>(const pair<T1, T2>& s1, const pair<T1, T2>& s2)
    {
        return (s1.first > s2.first) || (!(s1.first < s2.first) && s1.second > s2.second);
    }

    template<class T1, class T2>
    bool operator<(const pair<T1, T2>& s1, const pair<T1, T2>& s2)
    {
        return (s1.first < s2.first) || (!(s1.first > s2.first) && s1.second < s2.second);
    }

    template <typename T1, typename T2>
    pair<T1, T2> make_pair(T1 x, T2 y) {
        return pair<T1, T2>(x, y);
    }
}


#endif