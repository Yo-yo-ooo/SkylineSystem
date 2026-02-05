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
        ValType& operator[](const KeyType& key){
            EasySTL::pair<ValType,bool> val = this->rbtree.Find(key,false);
            bool TF = val.second;
            if(TF == false){
                this->rbtree.Insert(key,ValType{});
                return *((ValType*)&this->rbtree.Find(key,false).first);
            }
            return val.first;
        }
    private:
        RBTree<KeyType,ValType> rbtree;
    };
}

#endif