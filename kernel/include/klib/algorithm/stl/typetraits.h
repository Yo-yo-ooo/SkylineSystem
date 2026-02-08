#ifndef _TYPE_TRAITS_H_
#define _TYPE_TRAITS_H_

#include <pdef.h>

namespace EasySTL {

	
	struct _true_type { };

	struct _false_type { };

    _EXPORT_STD template <class _Ty>
    constexpr bool is_trivial_v = __is_trivial(_Ty);

    // 1. 基础模板：默认情况，不包含引用
    _EXPORT_STD template <class _Ty>
    struct remove_reference {
        using type                 = _Ty;
        using _Const_thru_ref_type = const _Ty;
    };

    _EXPORT_STD template <class _Ty>
    struct remove_reference<_Ty&> {
        using type                 = _Ty;
        using _Const_thru_ref_type = const _Ty&;
    };

    _EXPORT_STD template <class _Ty>
    struct remove_reference<_Ty&&> {
        using type                 = _Ty;
        using _Const_thru_ref_type = const _Ty&&;
    };

    template <class _Ty>
    using remove_reference_t = typename remove_reference<_Ty>::type;

    
    _EXPORT_STD template <class _Ty, _Ty _Val>
    struct integral_constant {
        static constexpr _Ty value = _Val;

        using value_type = _Ty;
        using type       = integral_constant;

        constexpr operator value_type() const noexcept {
            return value;
        }

        [[nodiscard]] constexpr value_type operator()() const noexcept {
            return value;
        }
    };

    _EXPORT_STD template <bool _Val>
    using bool_constant = integral_constant<bool, _Val>;

    _EXPORT_STD using true_type  = bool_constant<true>;
    _EXPORT_STD using false_type = bool_constant<false>;
    _EXPORT_STD template <bool _Test, class _Ty = void>
    struct enable_if {}; // no member "type" when !_Test

    template <class _Ty>
    struct enable_if<true, _Ty> { // type is _Ty for _Test
        using type = _Ty;
    };

    _EXPORT_STD template <bool _Test, class _Ty = void>
    using enable_if_t = typename enable_if<_Test, _Ty>::type;

    _EXPORT_STD template <class>
    constexpr bool is_lvalue_reference_v = false; // determine whether type argument is an lvalue reference

    template <class _Ty>
    constexpr bool is_lvalue_reference_v<_Ty&> = true;

    _EXPORT_STD  template <class _Ty>
    struct is_lvalue_reference : bool_constant<is_lvalue_reference_v<_Ty>> {};

    _EXPORT_STD template <class>
    constexpr bool is_rvalue_reference_v = false; // determine whether type argument is an rvalue reference

    template <class _Ty>
    constexpr bool is_rvalue_reference_v<_Ty&&> = true;

    _EXPORT_STD template <class _Ty>
    struct is_rvalue_reference : bool_constant<is_rvalue_reference_v<_Ty>> {};


    _EXPORT_STD template <class _Ty>
    [[nodiscard]]  constexpr _Ty&& forward(remove_reference_t<_Ty>& _Arg) noexcept {
        return static_cast<_Ty&&>(_Arg);
    }

    _EXPORT_STD template <class _Ty>
    [[nodiscard]]  constexpr _Ty&& forward(remove_reference_t<_Ty>&& _Arg) noexcept {
        static_assert(!is_lvalue_reference_v<_Ty>, "bad forward call");
        return static_cast<_Ty&&>(_Arg);
    }

    _EXPORT_STD template <class _Ty>
    [[nodiscard]]  constexpr remove_reference_t<_Ty>&& move(_Ty&& _Arg) noexcept {
        return static_cast<remove_reference_t<_Ty>&&>(_Arg);
    }


	/*

	** 萃取传入的T类型的类型特性

	*/

	template<class T>

	struct _type_traits

	{

		typedef _false_type		has_trivial_default_constructor;

		typedef _false_type		has_trivial_copy_constructor;

		typedef _false_type		has_trivial_assignment_operator;

		typedef _false_type		has_trivial_destructor;

		typedef _false_type		is_POD_type;

	};



	template<>

	struct _type_traits<bool>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<char>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<unsigned char>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<signed char>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<wchar_t>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<short>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<unsigned short>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<int>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<unsigned int>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<long>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<unsigned long>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<long long>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<unsigned long long>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<float>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<double>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<long double>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};


    //指针是POD类型
	template<class T>

	struct _type_traits<T*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<class T>

	struct _type_traits<const T*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<char*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<unsigned char*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<signed char*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<const char*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<const unsigned char*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

	template<>

	struct _type_traits<const signed char*>

	{

		typedef _true_type		has_trivial_default_constructor;

		typedef _true_type		has_trivial_copy_constructor;

		typedef _true_type		has_trivial_assignment_operator;

		typedef _true_type		has_trivial_destructor;

		typedef _true_type		is_POD_type;

	};

    
    template <class _FloatingType>
    struct _Floating_type_traits;

    template <>
    struct _Floating_type_traits<float> {
        static constexpr int32_t _Mantissa_bits           = 24; // FLT_MANT_DIG
        static constexpr int32_t _Exponent_bits           = 8; // sizeof(float) * CHAR_BIT - FLT_MANT_DIG
        static constexpr int32_t _Maximum_binary_exponent = 127; // FLT_MAX_EXP - 1
        static constexpr int32_t _Minimum_binary_exponent = -126; // FLT_MIN_EXP - 1
        static constexpr int32_t _Exponent_bias           = 127;
        static constexpr int32_t _Sign_shift              = 31; // _Exponent_bits + _Mantissa_bits - 1
        static constexpr int32_t _Exponent_shift          = 23; // _Mantissa_bits - 1

        using _Uint_type = uint32_t;

        static constexpr uint32_t _Exponent_mask             = 0x000000FFu; // (1u << _Exponent_bits) - 1
        static constexpr uint32_t _Normal_mantissa_mask      = 0x00FFFFFFu; // (1u << _Mantissa_bits) - 1
        static constexpr uint32_t _Denormal_mantissa_mask    = 0x007FFFFFu; // (1u << (_Mantissa_bits - 1)) - 1
        static constexpr uint32_t _Special_nan_mantissa_mask = 0x00400000u; // 1u << (_Mantissa_bits - 2)
        static constexpr uint32_t _Shifted_sign_mask         = 0x80000000u; // 1u << _Sign_shift
        static constexpr uint32_t _Shifted_exponent_mask     = 0x7F800000u; // _Exponent_mask << _Exponent_shift

        static constexpr float _Minimum_value = 0x1.000000p-126f; // FLT_MIN
        static constexpr float _Maximum_value = 0x1.FFFFFEp+127f; // FLT_MAX
    };

    template <>
    struct _Floating_type_traits<double> {
        static constexpr int32_t _Mantissa_bits           = 53; // DBL_MANT_DIG
        static constexpr int32_t _Exponent_bits           = 11; // sizeof(double) * CHAR_BIT - DBL_MANT_DIG
        static constexpr int32_t _Maximum_binary_exponent = 1023; // DBL_MAX_EXP - 1
        static constexpr int32_t _Minimum_binary_exponent = -1022; // DBL_MIN_EXP - 1
        static constexpr int32_t _Exponent_bias           = 1023;
        static constexpr int32_t _Sign_shift              = 63; // _Exponent_bits + _Mantissa_bits - 1
        static constexpr int32_t _Exponent_shift          = 52; // _Mantissa_bits - 1

        using _Uint_type = uint64_t;

        static constexpr uint64_t _Exponent_mask             = 0x00000000000007FFu; // (1ULL << _Exponent_bits) - 1
        static constexpr uint64_t _Normal_mantissa_mask      = 0x001FFFFFFFFFFFFFu; // (1ULL << _Mantissa_bits) - 1
        static constexpr uint64_t _Denormal_mantissa_mask    = 0x000FFFFFFFFFFFFFu; // (1ULL << (_Mantissa_bits - 1)) - 1
        static constexpr uint64_t _Special_nan_mantissa_mask = 0x0008000000000000u; // 1ULL << (_Mantissa_bits - 2)
        static constexpr uint64_t _Shifted_sign_mask         = 0x8000000000000000u; // 1ULL << _Sign_shift
        static constexpr uint64_t _Shifted_exponent_mask     = 0x7FF0000000000000u; // _Exponent_mask << _Exponent_shift

        static constexpr double _Minimum_value = 0x1.0000000000000p-1022; // DBL_MIN
        static constexpr double _Maximum_value = 0x1.FFFFFFFFFFFFFp+1023; // DBL_MAX
    };

    template <>
    struct _Floating_type_traits<long double> : _Floating_type_traits<double> {};


    template <class _Ty>
    [[nodiscard]] constexpr _Ty* addressof(_Ty& _Val) noexcept {
        return __builtin_addressof(_Val);
    }
}

#endif