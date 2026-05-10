/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_or_64.h                                        :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/14 22:02:52 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:26:47 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_OR_64_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_OR_64_H

# include <stdint.h>

# define __A_OR_64	__a_or_64
# define _A_OR_64	__a_or_64
# define A_OR_64	__a_or_64

static inline __attribute__((always_inline)) void	__a_or_64(volatile uint64_t *p, uint64_t v)
{
	__asm__	volatile (
			"lock ; or %1, %0"
			: "=m"(*p) : "r"(v) : "memory" );
}

/* extern inline __attribute__((always_inline)) void	a_or_64(volatile uint64_t *p, uint64_t v)
		__attribute__((weak, alias("__a_or_64"))); */

#endif
