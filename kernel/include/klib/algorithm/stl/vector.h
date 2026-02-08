
#include <klib/algorithm/stl/allocator.h>
#include <klib/algorithm/stl/iterator.h>
namespace EasySTL {
    template <
        typename T,
        size_t DefaultCapacity = 128,
        void* (*VMalloc)(u64 size) = kmalloc,
        void (*VFree)(void* ptr) = kfree,
        void* (*VRealloc)(void* ptr, u64 size) = krealloc,
        void* (*VMemcpy)(void* dest, const void* src, uint64_t n) = __memcpy,
        void (*VMemset)(void* dest, uint8_t Val, uint64_t size) = _memset
    >
    class vector {
    private:
        T* List;
        size_t Position{ 0 }, Capacity{ 0 };

        void push_back_check_realloc() {
            if (this->Position > this->Capacity) {
                List = (T*)VRealloc(List, this->Capacity * 2 * sizeof(T));
                this->Capacity *= 2;
            }
        }
    public:
        vector() {
            this->Capacity = DefaultCapacity;
            List = (T*)VMalloc(DefaultCapacity * sizeof(T));
        }
        vector(size_t Size) {
            this->Capacity = Size % sizeof(T) ? Size / sizeof(T) + 1 : Size / sizeof(T);
            List = (T*)VMalloc(this->Capacity * sizeof(T));
        }/* 

        void push_back(T Data) {
            this->push_back_check_realloc();
            kinfoln("%X",EasySTL::addressof(this->List[this->Position]));
            kinfoln("%X",EasySTL::addressof(Data));
            VMemcpy(
                EasySTL::addressof(this->List[this->Position]),
                EasySTL::addressof(Data),
                sizeof(T)
            );
            this->Position++;
            kinfoln("Memcpy OK!");
            return;
        } */

        void push_back(T& Data) {
            this->push_back_check_realloc();
            kinfoln("%X",EasySTL::addressof(this->List[this->Position]));
            kinfoln("%X",EasySTL::addressof(Data));
            kinfoln("Sizeof T:%d",sizeof(T));
            VMemcpy(
                (void*)&this->List[this->Position],
                &Data,
                sizeof(T)
            );
            this->Position++;
            kinfoln("Memcpy OK!");
            return;
        }

        T& operator[](size_t Index) {
            return *((T*)&this->List[Index]);
        }
    };
}