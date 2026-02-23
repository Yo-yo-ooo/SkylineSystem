/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_cas_p.h                                        :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/14 21:28:44 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:24:08 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_CAS_P_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_CAS_P_H

# define __A_CAS_P	__a_cas_p
# define _A_CAS_P	__a_cas_p
# define A_CAS_P	__a_cas_p

static __attribute__((always_inline)) void	*__a_cas_p(volatile void *p, void *t, void *s)
{
	__asm__("lock ; cmpxchg %3, %1"
		: "=a"(t), "=m"(*(void *volatile *)p)
		: "a"(t), "r"(s) : "memory" );
	return (t);
}

/* extern __attribute__((always_inline)) void	*a_cas_p(volatile void *p, void *t, void *s)
		__attribute__((weak, alias("__a_cas_p")));
 */
#endif
