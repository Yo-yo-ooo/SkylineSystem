/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_or.h                                           :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/14 21:50:27 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:26:56 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_OR_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_OR_H

# define __A_OR		__a_or
# define _A_OR		__a_or
# define A_OR		__a_or

static __attribute__((always_inline)) void	__a_or(volatile int *p, int v)
{
	__asm__	volatile (
			"lock ; or %1, %0"
			: "=m"(*p) : "r"(v) : "memory" );
}

/* extern __attribute__((always_inline)) void	a_or(volatile int *p, int v)
		__attribute__((weak, alias("__a_or"))); */

#endif
