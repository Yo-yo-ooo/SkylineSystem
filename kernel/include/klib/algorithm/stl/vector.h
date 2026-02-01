
#include <klib/algorithm/stl/allocator.h>
#include <klib/algorithm/stl/iterator.h>
namespace EasySTL {
    
	template<class T>
    class vector
    {
    public:
        typedef T* iterator;
        typedef const T* const_iterator;
        iterator begin(){
            return _start;
        }
        iterator end(){
            return _finish;
        }
        const_iterator begin() const{
            return _start;
        }
        const_iterator end() const{
            return _finish;
        }
        //构造函数
        vector()
            :_start(nullptr)
                , _finish(nullptr)
                , _end_of_storage(nullptr)
            {}
        //传统写法
        //拷贝构造
        // v2(v1)
        //        vector(const vector<T>& v)
        //        {
        //          _start = new T[v.size()]; // v.capacity()也可以
        //          //memcpy(_start, v._start, sizeof(T)*v.size());
        //          for (size_t i = 0; i < v.size(); ++i)
        //          {
        //          _start[i] = v._start[i];
        //          }
        //          _finish = _start + v.size();
        //          _end_of_storage = _start + v.size();
        //        }
        // v2(v1)
        //        vector(const vector<T>& v)
        //            :_start(nullptr)
        //            , _finish(nullptr)
        //            , _end_of_storage(nullptr)
        //        {
        //            reserve(v.size());
        //            for (const auto& e : v)
        //            {
        //                push_back(e);
        //            }
        //        }
        //初始化为N个val
        vector(size_t n, const T& val = T())
            :_start(nullptr)
                , _finish(nullptr)
                , _end_of_storage(nullptr)
        {
            reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                push_back(val);
            }
        }
        //使用迭代器构造
        template <class InputIterator>
            vector(InputIterator first, InputIterator last)
            :_start(nullptr)
                , _finish(nullptr)
                , _end_of_storage(nullptr)
            {
                while(first != last)
                {
                    push_back(*first);
                    ++first;
                }
            }
        //现代写法
        void swap(vector<T>& v)
        {
            //std::swap(_start, v._start);
            T* tmp = v._start;
            v._start = _start;
            _start = tmp;
            //std::swap(_finish, v._finish);
            tmp = v._finish;
            v._finish = _finish;
            _finish = tmp;
            //std::swap(_end_of_storage, v._end_of_storage);
            tmp = v._end_of_storage;
            v._end_of_storage = _end_of_storage;
            _end_of_storage = tmp;
        }
        // v2(v1)
        vector(const vector<T>& v)
            :_start(nullptr)
                , _finish(nullptr)
                , _end_of_storage(nullptr)
            {
                vector<T> tmp(v.begin(), v.end());
                swap(tmp);
            }
        // v1 = v2
        vector<T>& operator=(vector<T> v)
        {
            swap(v);
            return *this;
        }
        ~vector()
        {
            delete[] _start;
            _start = _finish = _end_of_storage = nullptr;
        }
        size_t capacity() const
        {
            return _end_of_storage - _start;
        }
        const T& operator[](size_t pos) const
        {
            ASSERT(pos < size());
            return _start[pos];
        }
        T& operator[](size_t pos)
        {
            ASSERT(pos < size());
            return _start[pos];
        }
        size_t size() const
        {
            return _finish - _start;
        }
        //相关函数
        void reserve(size_t n)
        {
            if (n > capacity())
            {
                size_t sz = size();
                T* tmp = new T[n];
                if (_start)
                {
                    //memcpy(tmp, _start, sizeof(T)*sz);
                    for (size_t i = 0; i < sz; ++i)
                    {
                        tmp[i] = _start[i];
                    }
                    delete[] _start;
                }
                _start = tmp;
                _finish = _start + sz;
                _end_of_storage = _start + n;
            }
        }
        void resize(size_t n, const T& val = T())
        {
            if (n > capacity())
            {
                reserve(n);
            }
            if (n > size())
            {
                // 初始化填值
                while (_finish < _start + n)
                {
                    *_finish = val;
                    ++_finish;
                }
            }
            else
            {
                _finish = _start + n;
            }
        }
        void push_back(const T& x)
        {
            /*  if (_finish == _end_of_storage)
            {
                reserve(capacity() == 0 ? 4 : capacity() * 2);
            }
            *_finish = x;
            ++_finish;*/
            insert(end(), x);
        }
        void pop_back()
        {
            ASSERT(_finish > _start);
            --_finish;
        }
        iterator insert(iterator pos, const T& x)
        {
            ASSERT(pos >= _start);
            ASSERT(pos <= _finish);
            if (_finish == _end_of_storage)
            {
                size_t len = pos - _start;
                reserve(capacity() == 0 ? 4 : capacity() * 2);
                pos = _start + len;
            }
            // 挪动数据
            iterator end = _finish - 1;
            while (end >= pos)
            {
                *(end + 1) = *end;
                --end;
            }
            *pos = x;
            ++_finish;
            return pos;
        }
        // stl 规定erase返回删除位置下一个位置迭代器
        iterator erase(iterator pos)
        {
            ASSERT(pos >= _start);
            ASSERT(pos < _finish);
            iterator begin = pos + 1;
            while (begin < _finish)
            {
                *(begin - 1) = *begin;
                ++begin;
            }
            --_finish;
            //if (size() < capacity()/2)
            //{
            //  // 缩容 -- 以时间换空间
            //}
            return pos;
        }
        T& front()
        {
            ASSERT(size() > 0);
            return *_start;
        }
        T& back()
        {
            ASSERT(size() > 0);
            return *(_finish - 1);
        }
    private:
        iterator _start;
        iterator _finish;
        iterator _end_of_storage;
    };
}