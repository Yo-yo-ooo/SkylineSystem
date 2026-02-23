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

/* extern inline __attribute__((always_inline)) int	a_fetch_add(volatile int *p, int v)
		__attribute__((weak, alias("__a_fetch_add"))); */

#endif
