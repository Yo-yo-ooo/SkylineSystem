#include <klib/algorithm/ftree.h>

namespace FTree
{
    FTreeStruct* Init(){
        FTreeStruct* FileTreeNode;
        FileTreeNode->Elements = nullptr;
        FileTreeNode->IsFirst = true;
        FileTreeNode->Next = nullptr;
        return FileTreeNode;
    }

    FTreeStruct* Find(void *Element,FTreeStruct *Tree){
        if(Tree == nullptr) return nullptr;
        if(Element == nullptr) return nullptr;
        FTreeStruct *BufTree = Tree;
        for(uint64_t i = 0;i < SLAB::GetSize(BufTree->Next);i++){
            if(BufTree == nullptr) return nullptr;
            if(_memcmp(Element,(void*)BufTree->Elements,
                SLAB::GetSize(Element)) == 0){
                return BufTree;
            }else{
                BufTree = (FTreeStruct *)BufTree->Next;
            }
        }
    }

    FTreeStruct *Insert(FTreeStruct *Tree){
        
    }

} // namespace FTree
