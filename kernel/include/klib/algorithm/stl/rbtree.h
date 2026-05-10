#pragma once
#ifndef _RED_BLACK_TREE_H_
#define _RED_BLACK_TREE_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>
#include <klib/algorithm/stl/rbt_.h>
namespace EasySTL{
template<typename KeyType, typename ValType>
class RBTree {
    // 内部使用的键值对类型
    struct Pair {
        KeyType key;
        ValType value;

        // 重载 < 运算符，这样可以直接使用 std::less
        bool operator<(const Pair& other) const {return key < other.key;}
        bool operator<(const KeyType& k) const { return key < k; }
    };

public:
    ValType& Insert(const KeyType& key, const ValType& value) {
        auto p = rbt.insert({ key, value });
        return const_cast<ValType&>(p->data.value);
    }

    void Erase(KeyType key) {
        // 创建一个临时 Pair 对象用于查找
        Pair tmp{ key, ValType{} };
        rbt.erase(tmp);
    }

    ValType* FindPtr(const KeyType& key) {
        Pair tmp{ key, ValType{} }; // 建议底层 rbt 支持 key 查找，避免构造 ValType
        auto p = rbt.lower_bound(tmp);
        
        if (p && !(p->data < tmp) && !(tmp < p->data)) {
            return &(p->data.value);
        }
        return nullptr;
    }

    EasySTL::pair<ValType,bool> Find(KeyType key,bool x){
        // 创建一个临时 Pair 对象用于查找
        Pair tmp{ key, ValType{} };
        // 使用 lower_bound 找到第一个不小于 key 的节点
        auto p = rbt.lower_bound(tmp);

        // 检查是否找到完全匹配的节点
        if (p && !(p->data < tmp) && !(tmp < p->data)) {
            return EasySTL::make_pair<ValType,bool>(p->data.value,true);
        }

        return EasySTL::make_pair<ValType,bool>(ValType{},false);
    }
private:
    // 使用默认的 std::less，因为 Pair 已经重载了 < 运算符
    rb_tree<Pair> rbt;
};

}
#endif