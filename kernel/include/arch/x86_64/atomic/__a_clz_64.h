/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_clz_64.h                                       :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 18:03:10 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:27:29 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_CLZ_64_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_CLZ_64_H

# include <stdint.h>

# define __A_CLZ_64	__a_clz_64
# define _A_CLZ_64	__a_clz_64
# define A_CLZ_64	__a_clz_64

static inline __attribute__((always_inline)) int	__a_clz_64(uint64_t x)
{
	__asm__("bsr %1,%0 ; xor $63,%0" : "=r"(x) : "r"(x));
	return (x);
}

/* extern inline __attribute__((always_inline)) int	a_clz_64(uint64_t x)
		__attribute__((weak, alias("__a_clz_64"))); */

#endif
