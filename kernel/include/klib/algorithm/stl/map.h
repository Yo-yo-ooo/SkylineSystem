#pragma once
#ifndef _STL_MAP_H_
#define _STL_MAP_H_

#include <klib/algorithm/stl/rbtree.h>

namespace EasySTL{
    template<typename KeyType, typename ValType>
    class Map{
    public:
        void insert(KeyType key, ValType value){this->rbtree.Insert(key,value);}
        void erase(KeyType key){this->rbtree.Erase(key);}
        ValType find(KeyType key){return this->rbtree.Find(key);}
        ValType& operator[](const KeyType& key) {
            // 1. 尝试查找
            // 这里的 Find 最好返回一个指向值的指针或引用，如果没有则返回 nullptr
            ValType* existing = rbtree.FindPtr(key);
            
            if (existing == nullptr) {
                // 2. 如果不存在，直接插入默认值
                // 优秀的实现应该让 Insert 返回新插入节点的引用
                return rbtree.Insert(key, ValType{}); 
            }
            
            return *existing;
        }
    private:
        RBTree<KeyType,ValType> rbtree;
    };
}

#endif