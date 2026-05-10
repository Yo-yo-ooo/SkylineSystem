/* ************************************************************************** */
/*                                                                            */
/*                                                        :::      ::::::::   */
/*   __a_spin.h                                         :+:      :+:    :+:   */
/*                                                    +:+ +:+         +:+     */
/*   By: lgoddijn <lgoddijn@student.codam.nl >      +#+  +:+       +#+        */
/*                                                +#+#+#+#+#+   +#+           */
/*   Created: 2024/11/17 17:53:26 by lgoddijn          #+#    #+#             */
/*   Updated: 2024/11/17 20:28:17 by lgoddijn         ###   ########.fr       */
/*                                                                            */
/* ************************************************************************** */

#ifndef __A_SPIN_H

# ifndef __INTERNAL_ATOMIC_EXTEND
#  error "Include `atomic_arch.h` instead."
# endif

# define __A_SPIN_H

# define __A_SPIN	__a_spin
# define _A_SPIN	__a_spin
# define A_SPIN		__a_spin

static inline __attribute__((always_inline)) void	__a_spin(void)
{
	__asm__ volatile ("pause" : : : "memory");
}

/* extern inline __attribute__((always_inline)) void	a_spin(void)
		__attribute__((weak, alias("__a_spin"))); */

#endif
