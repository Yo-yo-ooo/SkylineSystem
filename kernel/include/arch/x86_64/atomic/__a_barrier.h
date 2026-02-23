/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_barrier.h                                      :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 17:51:15 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:28:34 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_BARRIER_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_BARRIER_H

# define __A_BARRIER	__a_barrier
# define _A_BARRIER		__a_barrier
# define A_BARRIER		__a_barrier

static inline __attribute__((always_inline)) void	__a_barrier(void)
{
	__asm__ volatile ("" : : : "memory");
}

/* extern inline __attribute__((always_inline)) void	a_barrier(void)
		__attribute__((weak, alias("__a_barrier"))); */

#endif
