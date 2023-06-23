#include "../include/stack.h"
#include "../include/syscall.h"

void *malloc(size_t size){
    syscall(1,size,0,0,0,0,0);
}

template<typename ElementType>
int Stack<ElementType>::IsEmpty(SatckRecord *S)
{
    return S->TopOfStack == EmptyTOS;
}

template<typename ElementType>
int Stack<ElementType>::IsFull(SatckRecord *S)
{
    return S->TopOfStack == S->Capacity - 1;
}

template<typename ElementType>
typename Stack<ElementType>::SatckRecord* Stack<ElementType>::CreateStack(int MaxElements)
{
    if (MaxElements < MinStackSize) {
        return nullptr;
    }

    SatckRecord *S = malloc(sizeof(SatckRecord));
    if (S == nullptr) {
        return nullptr;
    }

    S->Arry = malloc(sizeof(ElementType) * MaxElements);
    if (S->Arry == nullptr) {
        free(S);
        return nullptr;
    }

    S->Capacity = MaxElements;
    MakeEmpty(S);

    return S;
}

template<typename ElementType>
void Stack<ElementType>::DisposeStack(SatckRecord *S)
{
    if (S != nullptr) {
        delete[]S->Arry;
        delete S;
    }
}

template<typename ElementType>
void Stack<ElementType>::MakeEmpty(SatckRecord *S)
{
    S->TopOfStack = EmptyTOS;
}

template<typename ElementType>
void Stack<ElementType>::Pop(SatckRecord *S)
{
    if (IsEmpty(S)) {
        return;
    }
    S->TopOfStack--;
}

template<typename ElementType>
void Stack<ElementType>::Push(ElementType X,SatckRecord *S)
{
    if (IsFull(S)) {
        return;
    }
    S->Arry[++S->TopOfStack] = X;
}

template<typename ElementType>
ElementType Stack<ElementType>::Top(SatckRecord *S)
{
    if (IsEmpty(S)) {
        return ElementType();
    }
    return S->Arry[S->TopOfStack];
}

template<typename ElementType>
ElementType Stack<ElementType>::TopAndPop(SatckRecord *S)
{
    if (IsEmpty(S)) {
        return ElementType();
    }
    return S->Arry[S->TopOfStack--];
}

// 显式实例化定义
template class Stack<int>;