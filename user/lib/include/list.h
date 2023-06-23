template<typename ElementType>
class List
{
public:
    typedef struct Node{
        ElementType Element;
        Node *Next;
    }Node;
    Node *MakeEmpty(Node *L);
    int IsEmpty(Node *L);
    int IsLast(Node *p,Node *L);
    Node *Find(ElementType X,Node *L);
    Node *FindPrevious(ElementType X,Node *L);
    Node *Header(Node *L);
    Node *First(Node *L);
    Node *Advance(Node *L);
    void Delete(ElementType X, Node *L);
    void Insert(ElementType X, Node *L,Node *p);
    void DeleteList(Node *L);
    ElementType Retrieve(Node *p);
};


