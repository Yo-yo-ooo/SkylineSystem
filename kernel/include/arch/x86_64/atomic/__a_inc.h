/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_inc.h                                          :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 17:09:53 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:26:37 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_INC_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_INC_H

# define __A_INC	__a_inc
# define _A_INC		__a_inc
# define A_INC		__a_inc

static inline __attribute__((always_inline)) void	__a_inc(volatile int *p)
{
	__asm__	volatile (
			"lock ; incl %0"
			: "=m"(*p) : "m"(*p) : "memory" );
}

/* extern inline __attribute__((always_inline)) void	a_inc(volatile int *p)
		__attribute__((weak, alias("__a_inc"))); */

#endif
