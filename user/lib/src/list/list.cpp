#include "../include/list.h"

template<typename ElementType>
typename List<ElementType>::Node* List<ElementType>::MakeEmpty(Node *L)
{
    Node *p = L->Next;
    while (p != nullptr) {
        Node *next = p->Next;
        delete p;
        p = next;
    }
    L->Next = nullptr;
    return L;
}

template<typename ElementType>
int List<ElementType>::IsEmpty(Node *L)
{
    return L->Next == nullptr;
}

template<typename ElementType>
int List<ElementType>::IsLast(Node *p,Node *L)
{
    return p->Next == nullptr;
}

template<typename ElementType>
typename List<ElementType>::Node* List<ElementType>::Find(ElementType X,Node *L)
{
    Node *p = L->Next;
    while (p != nullptr && p->Element != X) {
        p = p->Next;
    }
    return p;
}

template<typename ElementType>
typename List<ElementType>::Node* List<ElementType>::FindPrevious(ElementType X,Node *L)
{
    Node *p = L;
    while (p->Next != nullptr && p->Next->Element != X) {
        p = p->Next;
    }
    return p;
}

template<typename ElementType>
typename List<ElementType>::Node* List<ElementType>::Header(Node *L)
{
    return L;
}

template<typename ElementType>
typename List<ElementType>::Node* List<ElementType>::First(Node *L)
{
    return L->Next;
}

template<typename ElementType>
typename List<ElementType>::Node* List<ElementType>::Advance(Node *L)
{
    return L->Next;
}

template<typename ElementType>
void List<ElementType>::Delete(ElementType X, Node *L)
{
    Node *p = FindPrevious(X, L);
    if (p->Next != nullptr) {
        Node *temp = p->Next;
        p->Next = temp->Next;
        delete temp;
    }
}

template<typename ElementType>
void List<ElementType>::Insert(ElementType X, Node *L, Node *p)
{
    Node *temp = new Node;
    temp->Element = X;
    temp->Next = p->Next;
    p->Next = temp;
}

template<typename ElementType>
void List<ElementType>::DeleteList(Node *L)
{
    Node *p = L->Next;
    while (p != nullptr) {
        Node *next = p->Next;
        delete p;
        p = next;
    }
    L->Next = nullptr;
}

template<typename ElementType>
ElementType List<ElementType>::Retrieve(Node *p)
{
    return p->Element;
}

// 显式实例化定义
template class List<int>;