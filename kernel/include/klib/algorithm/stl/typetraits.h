#ifndef _TYPE_TRAITS_H_
#define _TYPE_TRAITS_H_

namespace EasySTL {

	
	struct _true_type { };

	struct _false_type { };

    template <class _Ty>
    constexpr bool is_trivial_v = __is_trivial(_Ty);

    // 1. 基础模板：默认情况，不包含引用
    template <class _Ty>
    struct remove_reference {
        using type                 = _Ty;
        using _Const_thru_ref_type = const _Ty;
    };

    template <class _Ty>
    struct remove_reference<_Ty&> {
        using type                 = _Ty;
        using _Const_thru_ref_type = const _Ty&;
    };

    template <class _Ty>
    struct remove_reference<_Ty&&> {
        using type                 = _Ty;
        using _Const_thru_ref_type = const _Ty&&;
    };

    template <class _Ty>
    using remove_reference_t = typename remove_reference<_Ty>::type;

    
    template <class _Ty, _Ty _Val>
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

    template <bool _Val>
    using bool_constant = integral_constant<bool, _Val>;

    using true_type  = bool_constant<true>;
    using false_type = bool_constant<false>;

    template <class>
    constexpr bool is_lvalue_reference_v = false; // determine whether type argument is an lvalue reference

    template <class _Ty>
    constexpr bool is_lvalue_reference_v<_Ty&> = true;

    template <class _Ty>
    struct is_lvalue_reference : bool_constant<is_lvalue_reference_v<_Ty>> {};

    template <class>
    constexpr bool is_rvalue_reference_v = false; // determine whether type argument is an rvalue reference

    template <class _Ty>
    constexpr bool is_rvalue_reference_v<_Ty&&> = true;

    template <class _Ty>
    struct is_rvalue_reference : bool_constant<is_rvalue_reference_v<_Ty>> {};


    template <class _Ty>
    [[nodiscard]]  constexpr _Ty&& forward(remove_reference_t<_Ty>& _Arg) noexcept {
        return static_cast<_Ty&&>(_Arg);
    }

    template <class _Ty>
    [[nodiscard]]  constexpr _Ty&& forward(remove_reference_t<_Ty>&& _Arg) noexcept {
        static_assert(!is_lvalue_reference_v<_Ty>, "bad forward call");
        return static_cast<_Ty&&>(_Arg);
    }

    template <class _Ty>
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



}

#endif