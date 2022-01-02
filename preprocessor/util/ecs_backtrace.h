#ifndef __ECS_BACKTRACE_H__
#define __ECS_BACKTRACE_H__

/*============================================================================
 * Obtaining a stack backtrace
 *============================================================================*/

/*
  This file is part of Code_Saturne, a general-purpose CFD tool.

  Copyright (C) 1998-2022 EDF S.A.

  This program is free software; you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation; either version 2 of the License, or (at your option) any later
  version.

  This program is distributed in the hope that it will be useful, but WITHOUT
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
  details.

  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 51 Franklin
  Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

/*----------------------------------------------------------------------------*/

#include "ecs_def.h"

/* Standard C library headers */

#include <stdarg.h>

BEGIN_C_DECLS

/*============================================================================
 * Public types
 *============================================================================*/

/*============================================================================
 * Public function prototypes
 *============================================================================*/

/*
 * Print a backtrace.
 *
 * parameters:
 *   start_depth: <-- depth of backtrace at which to start printing
 *                    (0 for all, including backtrace print function)
 */

void
ecs_backtrace_print(int  start_depth);

/*----------------------------------------------------------------------------*/

END_C_DECLS

#endif /* __ECS_BACKTRACE_H__ */
