/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_ctz_64.h                                       :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 17:57:52 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:27:43 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_CTZ_64_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_CTZ_64_H

# include <stdint.h>

# define __A_CTZ_64	__a_ctz_64
# define _A_CTZ_64	__a_ctz_64
# define A_CTZ_64	__a_ctz_64

static inline __attribute__((always_inline)) int	__a_ctz_64(uint64_t x)
{
	__asm__("bsf %1,%0" : "=r"(x) : "r"(x));
	return (x);
}

/* extern inline __attribute__((always_inline)) int	a_ctz_64(uint64_t x)
		__attribute__((weak, alias("__a_ctz_64"))); */

#endif
