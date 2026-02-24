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

/*
* SPDX-License-Identifier: GPL-2.0-only
* File: __a_inc.h
* add __a_inc64,__a_incu64
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

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

#define __A_INC64	__a_inc64
#define _A_INC64	__a_inc64
#define A_INC64		__a_inc64

static inline __attribute__((always_inline)) void	__a_inc64(volatile long long *p)
{
	__asm__	volatile (
			"lock ; incl %0"
			: "=m"(*p) : "m"(*p) : "memory" );
}


#define __A_INCU64	__a_incu64
#define _A_INCU64	__a_incu64
#define A_INCU64		__a_incu64

static inline __attribute__((always_inline)) void	__a_incu64(volatile unsigned long long *p)
{
	__asm__	volatile (
			"lock ; incl %0"
			: "=m"(*p) : "m"(*p) : "memory" );
}
/* extern inline __attribute__((always_inline)) void	a_inc(volatile int *p)
		__attribute__((weak, alias("__a_inc"))); */

#endif
