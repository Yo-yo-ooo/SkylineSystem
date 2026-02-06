#pragma once

#ifdef __GNUC__
#define PACK( __Declaration__ ) __Declaration__ __attribute__((packed))
#define ReDefFunction(x) __attribute__((weak, alias(x)))
#define WeakFunction __attribute__((weak))
#endif

#ifdef _MSC_VER
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#define ReDefFunction(x) 
#endif

#define elif else if

#define GET_BIT(value,bit) ((value)&(1<<(bit)))    //读取指定位
#define CPL_BIT(value,bit) ((value)^=(1<<(bit)))   //取反指定位

#define SET0_BIT(value,bit) ((value)&=~(1<<(bit))) //把某个位置0
#define SET1_BIT(value,bit) ((value)|= (1<<(bit))) //把某个位置1

#define IGNORE_VALUE(VALUE) (void)VALUE

#define rm_mask(num, mask) ((num) & ((typeof(num))~(mask)))

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define max(a, b) ({ \
    __typeof__(a) ta = (a); \
    __typeof__(b) tb = (b); \
    ta < tb ? tb : ta; \
})

#define min(a, b) ({ \
    __typeof__(a) ta = (a); \
    __typeof__(b) tb = (b); \
    ta > tb ? tb : ta; \
})

#ifdef __cplusplus
template <class _Ty>
struct _if_remove_reference {
    using type                 = _Ty;
    using _Const_thru_ref_type = const _Ty;
};

template <class _Ty>
struct _if_remove_reference<_Ty&> {
    using type                 = _Ty;
    using _Const_thru_ref_type = const _Ty&;
};

template <class _Ty>
struct _if_remove_reference<_Ty&&> {
    using type                 = _Ty;
    using _Const_thru_ref_type = const _Ty&&;
};

template <class _Ty>
using _if_remove_reference_t = typename _if_remove_reference<_Ty>::type;

template <class _Ty>
[[nodiscard]]  constexpr _if_remove_reference_t<_Ty>&& _if_move(_Ty&& _Arg) noexcept {
    return static_cast<_if_remove_reference_t<_Ty>&&>(_Arg);
}

template<typename T>
void swap(T &a,T &b) noexcept
{
    T temp = _if_move(a);
    a = _if_move(b);
    b = _if_move(temp);
}
#else
#define swap(a, b) ({ \
    const auto __swap_tmp = (b); \
    b = a; \
    a = __swap_tmp; \
})
#endif