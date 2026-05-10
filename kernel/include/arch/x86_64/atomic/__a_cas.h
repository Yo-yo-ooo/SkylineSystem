/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_cas.h                                          :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/14 21:23:50 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:24:22 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_CAS_H

# define __A_CAS_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_CAS	__a_cas
# define _A_CAS		__a_cas
# define A_CAS		__a_cas

static inline __attribute__((always_inline)) int	__a_cas(volatile int *p, int t, int s)
{
	__asm__	volatile (
			"lock ; cmpxchg %3, %1"
			: "=a"(t), "=m"(*p) : "a"(t), "r"(s) : "memory" );

	return (t);
}

/* extern inline __attribute__((always_inline)) int	a_cas(volatile int *p, int t, int s)
		__attribute__((weak, alias("__a_cas"))); */

#endif
