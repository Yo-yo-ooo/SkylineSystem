#pragma once
#ifndef _B_TREE_
#define _B_TREE_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>
#include <klib/klib.h>

template<typename KeyValue>
class BTree{
public:
    typedef struct BTreeNode{
        uint64_t KeyNum;
        bool IsLeaf;
        BTreeNode **Child;
        KeyValue *Keys;
    }BTreeNode;

    typedef struct BTreeStruct{
        BTreeNode *Root;
        uint64_t Dgree;
    }BTreeStruct;

    BTreeStruct *CreateTree(uint64_t Dgree){
        BTreeStruct *Tree = (BTreeStruct*)kmalloc(sizeof(BTreeStruct));
        Tree->Dgree = Dgree;
        BTreeNode *Node = BTree::CreateNode(Dgree,true);
        Tree->Root = Node;

        return Tree;
    }
    void DestoryNode(BTreeNode *Node){
        if(Node != nullptr){
            kfree(Node->Child);
            kfree(Node->Keys);
            kfree(Node);
        }
    }
    BTreeNode *CreateNode(uint64_t Dgree,bool IsLeaf){
        BTreeNode *Node = (BTreeNode*)kcalloc(1,sizeof(BTreeNode));
        if(Node == nullptr){
            //kerror("(BTreeNode)Create Node Failed");
            return nullptr;
        }
        Node->IsLeaf = IsLeaf;
        Node->Keys = (KeyValue*)kcalloc(1,(Dgree * 2 - 1) * sizeof(KeyValue));
        Node->Child = (BTreeNode**)kcalloc(1,(Dgree * 2) * sizeof(BTreeNode));
        Node->KeyNum = 0;
        return Node;
    }
    void Split(BTreeStruct *Tree, BTreeNode *ParentPtr, uint64_t Index){
        uint64_t Dgree = Tree->Dgree;
        BTreeNode *Y = ParentPtr->Child[Index];

        /* 新建节点z,将待分裂节点的后一半关键字放入新节点z */
        BTreeNode *Z = BTree::CreateNode(Dgree, Y->IsLeaf); // 创建节点，传入参  数为：节点的阶数+是否叶子节点
        Z->KeyNum = Dgree - 1; // 设置新节点z的关键字个数
        for (uint64_t K = Dgree; K < 2 * Dgree - 1; ++K)
            Z->Keys[K - Dgree] = Y->Keys[K];
        if (Y->IsLeaf == 0)   // 若待分裂节点为非叶子节点，则拥有的子树也需要复制到   新节点z
            for (uint64_t K = Dgree; K < 2 * Dgree; ++K )
                Z->Child[K - Dgree] = Y->Child[K];
        /* 将分裂出的关键字放入父节点x */
        for (uint64_t K = ParentPtr->KeyNum - 1; K >= Index; K--)   // 将后半关键字往后移动位   置，腾出空间，以便存放分裂节点的中间关键字
            ParentPtr->Keys[K + 1] = ParentPtr->Keys[K];
        ParentPtr->Keys[Index] = Y->Keys[Dgree - 1]; 
        ParentPtr->KeyNum += 1;
        Y->KeyNum = Dgree - 1;

        /* 将新建节点z作为子树插入到x节点 */
        for (uint64_t K = ParentPtr->KeyNum; K > Index; --K)    // 将父节点中部分子树往后移动一 个单位，以便插入新建节点z
            ParentPtr->Child[K + 1] = ParentPtr->Child[K];
        ParentPtr->Child[Index + 1] = Z;
    }
    void InsertKeyNNF(BTreeStruct *Tree,BTreeNode *ParentPtr, KeyValue Key){
        uint64_t Index = ParentPtr->KeyNum - 1;

        if(ParentPtr->IsLeaf == true){
            while (Index >= 0 && ParentPtr->Keys[Index] > Key)
            {
                ParentPtr->Keys[Index + 1] = ParentPtr->Keys[Index];
                Index--;
            }
            ParentPtr->Keys[Index + 1] = Key;
            ParentPtr->KeyNum += 1;
        }else{
            while (Index >= 0 && ParentPtr->Keys[Index] > Key)
                Index--;

            if (ParentPtr->Child[Index + 1]->KeyNum == (2 * Tree->Dgree - 1))
            {
                this->Split(Tree, ParentPtr, Index + 1);
                if (ParentPtr->Keys[Index + 1] < Key)   // 更新待探索的子节点索引，因为 分裂节点会向x节点插入新的关键字，所以要重新判断该关键字是否小于key
                    Index++;
            }

            this->InsertKeyNNF(Tree, ParentPtr->Child[Index + 1], Key);
        }
    }
    void InsertKey(BTreeStruct *Tree, KeyValue Key){
        BTreeNode *Root = Tree->Root;

        if(Root->KeyNum == 2 * Tree->Dgree - 1){
            BTreeNode *Node = this->CreateNode(Tree->Dgree,false);
            Tree->Root = Node;
            Node->Child[0] = Root;
            this->Split(Tree,Node,0);

            uint64_t i = 0;
            if(Node->Keys[0] < Key)
                ++i;
            this->InsertKeyNNF(Tree,Node->Child[i],Key);
        }else{
            this->InsertKeyNNF(Tree,Root,Key);
        }
    }

    void Merge(BTreeStruct *Tree, BTreeNode *Node, uint64_t Index){
        BTreeNode *Left = Node->Child[Index];   // 合并的左子节点
        BTreeNode *Right = Node->Child[Index + 1];  // 合并的右子节点

        /* 将父节点关键字以及右子节点都存入左子节点中 */
        Left->Keys[Tree->Dgree - 1] = Node->Keys[Index];
        for (uint64_t i = 0; i < Right->KeyNum; ++i)    // 移动right中的关键字
        {
            Left->Keys[Tree->Dgree + i] = Right->Keys[i];
        }
        if (Left->IsLeaf == false)    // 移动right中的子节点指针
        {
            for (uint64_t i = 0; i <= Right->KeyNum; ++i)
            {
                Left->Child[Tree->Dgree + i] = Right->Child[i];
            }
        }
        Left->KeyNum += Right->KeyNum + 1;

        this->DestoryNode(Right);  // 释放右子节点

        for (uint64_t i = Index + 1; i < Node->KeyNum; ++i) // 更新父节点关键字和指向子 节点的指针位置
        {
            Node->Keys[i - 1] = Node->Keys[i];
            Node->Child[i] = Node->Child[i + 1];
        }
        Node->Child[Node->KeyNum + 1] = nullptr;
        Node->KeyNum -= 1;

        if (Node->KeyNum == 0) // 如果父节点个数为0,这种情况只可能是父节点为根节点
        {
            Tree->Root = Left;
            this->DestoryNode(Node);
        }
    }

    BTreeNode* Find(BTreeStruct *Tree,KeyValue Key){
        BTreeNode *Ptr = Tree->Root;
Again:
        if(Ptr->IsLeaf != false){
            uint64_t Index = 0;
            while (Index < Ptr->KeyNum && Key > Ptr->Keys[Index])    
                Index++;
            if(Ptr->Keys[Index] != Key && Ptr->Child[Index]->KeyNum >= Tree->Dgree){
                    Ptr = Ptr->Child[Index];
                    goto Again;
            }else if(Ptr->Keys[Index] != Key && Ptr->Child[Index + 1]->KeyNum >= Tree->Dgree){
                Ptr = Ptr->Child[Index + 1];
                goto Again;
            }else if(Ptr->Keys[Index] == Key){
                return Ptr;
            }
        }else
            for(uint64_t i = 0;i < Ptr->KeyNum;i++)
                if(Ptr->Keys[i] == Key)
                    return Ptr;
        return nullptr;
    }

    void BNodeDeleteKey(BTreeStruct *Tree, BTreeNode *Node, KeyValue Key){
        if (Node == nullptr)
            return;

        uint64_t Index = 0;
        // 找到节点中第一个大于等于key的关键字
        while (Index < Node->KeyNum && Key > Node->Keys[Index])    
            Index++;

        if (Index < Node->KeyNum && Node->Keys[Index] == Key) {    // 在该节点中找 到等于key的关键字, 考虑具体的删除操作

            if (Node->IsLeaf == true)    // 该节点为叶子节点
            {
                for (uint64_t i = Index; i < Node->KeyNum - 1; ++i)
                {
                    Node->Keys[i] = Node->Keys[i + 1];
                }
                Node->Keys[Node->KeyNum - 1] = 0;
                Node->KeyNum -= 1;

                if (Node->KeyNum == 0)
                {
                    free(Node);
                    Tree->root = nullptr;
                }
            } else if (Node->Child[Index]->KeyNum >= Tree->Dgree) {    // 借用左   子节点的关键字进行覆盖
                BTreeNode * Left = Node->Child[Index];
                Node->Keys[Index] = Left->Keys[Left->KeyNum - 1];

                this->BNodeDeleteKey(Tree, Left, Left->Keys[Left->KeyNum - 1]);  // 递归删除子节点中用来覆盖上层节点的关键字
            } else if (Node->Child[Index + 1]->KeyNum >= Tree->Dgree) {    // 借   用左子节点的关键字进行覆盖
                BTreeNode *Right = Node->Child[Index + 1];
                Node->Keys[Index] = Right->Keys[0];

                this->BNodeDeleteKey(Tree, Right, Right->Keys[0]);    // 递归删   除子节点中用来覆盖上层节点的关键字
            } else {
                this->Merge(Tree, Node, Index);
                this->BNodeDeleteKey(Tree, Node->Child[Index], Key);
            }

        } else {    // 继续往children[Index]子节点找

            BTreeNode *Child = Node->Child[Index];
            if (Child == nullptr) {
                kerror("(BTree 0x%p)Cannot find Key : %d\n", (uint64_t)this,Key);
                return;
            }

            /* 找关键字所在的节点过程中，遇到关键字个数==Dgree-1的节点，进行丰满处  理 */
            if (Child->KeyNum == Tree->Dgree - 1) {
                BTreeNode *Left = nullptr, *Right = nullptr;
                if (Index > 0)
                    Left = Node->Child[Index - 1];
                if (Index < Node->KeyNum)
                    Right = Node->Child[Index + 1];

                if ((Left && Left->KeyNum >= Tree->Dgree) || (Right && Right->KeyNum     >= Tree->Dgree)) {
                    uint64_t Select = 0;
                    if (Left && Right)
                        Select = (Right->KeyNum > Left->KeyNum) ? 1 : 0;

                    if (Select) {   // 选右子树
                        /* 将父节点node的第一个大于key的关键字移给child */
                        Child->Keys[Child->KeyNum] = Node->Keys[Index];
                        Child->KeyNum += 1;
                        Child->Child[Child->KeyNum] = Right->Child[0];

                        /* 将child右兄弟节点的第一个关键字移给父节点node */
                        Node->Keys[Index] = Right->Keys[0];

                        /* 更新child右兄弟节点中关键字和子节点指针的位置 */
                        for (uint64_t i = 0; i < Right->KeyNum - 1; ++i) {
                            Right->Keys[i] = Right->Keys[i + 1];
                            Right->Child[i] = Right->Child[i + 1];
                        }
                        Right->Keys[Right->KeyNum - 1] = 0;
                        Right->Child[Right->KeyNum - 1] = Right->Child   [Right->KeyNum];
                        Right->Child[Right->KeyNum] = nullptr;
                        Right->KeyNum -= 1;

                    } else {    // 选左子树

                        Child->KeyNum++;
                        Child->Child[Child->KeyNum] = Child->Child   [Child->KeyNum - 1];
                        for (uint64_t i = Child->KeyNum - 2; i >= 0; --i) {
                            Child->Keys[i + 1] = Child->Keys[i];
                            Child->Child[i + 1] = Child->Child[i];
                        }
                        Child->Keys[0] = Node->Keys[Index - 1];
                        Child->Child[0] = Left->Child[Left->KeyNum];

                        Node->Keys[Index - 1] = Left->Keys[Left->KeyNum - 1];

                        Left->Keys[Left->KeyNum - 1] = 0;
                        Left->Child[Left->KeyNum] = nullptr;
                        Left->KeyNum -= 1;
                    }
                } else {
                    if (Left && Left->KeyNum == Tree->Dgree - 1) {
                        this->Merge(Tree, Node, Index - 1);
                        Child = Left;
                    } else if (Right && Right->KeyNum == Tree->Dgree - 1) {
                        this->Merge(Tree, Node, Index);
                    }
                }
            }
            this->BNodeDeleteKey(Tree, Child, Key);
        }
    }

    void DeleteKey(BTreeStruct *Tree, KeyValue Key){
        if(Tree->Root != nullptr)
            this->BNodeDeleteKey(Tree,Tree->Root,Key);
    }


    void PrintTree(BTreeStruct *Tree, BTreeNode *Node, uint64_t Layer){
        BTreeNode *Ptr = Node;
        if(Ptr){
            kinfoln("\n(Btree 0x%p) Layer=%x,KeyNum=%x,IsLeaf=%s",
                (uint64_t)this,Layer,Ptr->KeyNum,Ptr->IsLeaf == true ? "true" : "false");
            for(uint64_t i = 0; i < Node->KeyNum;i++)
                kinfoln("\n(Btree 0x%p) %x",
                    (uint64_t)this,Layer,Ptr->Keys[i]);
            Layer++;
            for(uint64_t i = 0;i <= Ptr->KeyNum;i++)
                if(Ptr->Child[i])
                    this->PrintTree(Tree,Ptr->Child[i],Layer);
        }else
            kerror("(BTree: 0x%p) Is empty",(uint64_t)this);
    }

    void InsertKey(KeyValue Key){this->InsertKey(this->PBTree,Key);}
    void DeleteKey(KeyValue Key){this->DeleteKey(this->PBTree,Key);}
    void PrintTree(BTreeNode *Node, uint64_t Layer){this->PrintTree(this->PBTree,Node,Layer);}
    BTreeNode* Find(KeyValue Key){this->Find(this->PBTree,Key);}
    void CreateTree(uint64_t Dgree, bool _NOTHIING_)
    {(void)_NOTHIING_;this->PBTree = this->CreateTree(Dgree);}

private:
    BTreeStruct *PBTree;
};


#endif