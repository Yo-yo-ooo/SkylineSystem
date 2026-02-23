/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_crash.h                                        :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 17:55:56 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:28:53 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_CRASH_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_CRASH_H

# define __A_CRASH	__a_crash
# define _A_CRASH	__a_crash
# define A_CRASH	__a_crash

static __attribute__((always_inline)) void	__a_crash(void)
{
	__asm__ volatile ("hlt" : : : "memory");
}

/* extern __attribute__((always_inline)) void	a_crash(void)
		__attribute__((weak, alias("__a_crash"))); */

#endif
