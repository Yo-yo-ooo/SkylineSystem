/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_fetch_add.h                                    :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/14 21:33:49 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:26:27 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_FETCH_ADD_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_FETCH_ADD_H

# define __A_FETCH_ADD	__a_fetch_add
# define _A_FETCH_ADD	__a_fetch_add
# define A_FETCH_ADD	__a_fetch_add

static inline __attribute__((always_inline)) int	__a_fetch_add(volatile int *p, int v)
{
	__asm__	volatile (
			"lock ; xadd %0, %1"
			: "=r"(v), "=m"(*p) : "0"(v) : "memory" );

	return (v);
}

#define __A_FETCH_ADDU64	__a_fetch_addu64
#define _A_FETCH_ADDU64	__a_fetch_addu64
#define A_FETCH_ADDU64	__a_fetch_addu64

static inline unsigned long long __a_fetch_addu64(volatile unsigned long long *p, unsigned long long val)
{
    __asm__ volatile (
            "lock ; xaddq %0, %1"
            : "=r"(val), "=m"(*p)
            :"0"(val) : "memory" );
    return (val); // 返回加法执行前的值
}

/* extern inline __attribute__((always_inline)) int	a_fetch_add(volatile int *p, int v)
		__attribute__((weak, alias("__a_fetch_add"))); */

#endif
