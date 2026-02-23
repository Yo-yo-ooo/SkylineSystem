/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_dec.h                                          :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 17:13:51 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:25:59 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_DEC_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_DEC_H

# define __A_DEC	__a_dec
# define _A_DEC		__a_dec
# define A_DEC		__a_dec

static __attribute__((always_inline)) void	__a_dec(volatile int *p)
{
	__asm__	volatile (
			"lock ; decl %0"
			: "=m"(*p) : "m"(*p) : "memory" );
}

/* extern __attribute__((always_inline)) void	a_dec(volatile int *p)
		__attribute__((weak, alias("__a_dec"))); */

#endif
