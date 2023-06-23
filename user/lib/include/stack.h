#define EmptyTOS    (-1)
#define MinStackSize (5)

template <typename ElementType>
class Stack
{
public:
    typedef struct SatckR{
        int Capacity;
        int TopOfStack;
        ElementType *Arry;
    }SatckRecord;
    int IsEmpty(SatckRecord *S);
    int IsFull(SatckRecord *S);
    SatckRecord *CreateStack(int MaxElements);
    void DisposeStack(SatckRecord *S);
    void MakeEmpty(SatckRecord *S);
    void Pop(SatckRecord *S);
    void Push(ElementType X,SatckRecord *S);
    ElementType Top(SatckRecord *S);
    ElementType TopAndPop(SatckRecord *S);
};

