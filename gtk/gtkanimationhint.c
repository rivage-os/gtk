/* GTK - The GIMP Toolkit
 *
 * Copyright 2026 Christian Hergert <christian@sourceandstack.com>
 *
 * This library is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of the
 * License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "gtkanimationhintprivate.h"

#ifdef HAVE_SCHED_UCLAMP
#include <linux/sched.h>
#include <linux/sched/types.h>
#include <sys/syscall.h>
#include <unistd.h>

typedef struct
{
  struct sched_attr saved_attr;
  guint             count;
  gboolean          applied;
} GtkAnimationHint;

static GPrivate animation_hint = G_PRIVATE_INIT (g_free);
#endif

void
_gtk_push_animation_hint (void)
{
#ifdef HAVE_SCHED_UCLAMP
  GtkAnimationHint *hint = g_private_get (&animation_hint);
  struct sched_attr attr = { 0 };

  if (hint == NULL)
    {
      hint = g_new0 (GtkAnimationHint, 1);
      g_private_set (&animation_hint, hint);
    }

  if (hint->count++ != 0)
    return;

  hint->applied = FALSE;

  /* Older kernels and sandbox policies may not support this optional hint. */
  if (syscall (SYS_sched_getattr, 0, &attr, sizeof attr, 0) != 0)
    return;

  if (attr.size < SCHED_ATTR_SIZE_VER1)
    return;

  hint->saved_attr = attr;
  attr.size = sizeof attr;
  attr.sched_flags |= (SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN);

  /* Request maximum performance within the application's existing cap.
   * This biases placement and frequency; it does not change CPU affinity.
   */
  attr.sched_util_min = MIN (1024, attr.sched_util_max);

  hint->applied = syscall (SYS_sched_setattr, 0, &attr, 0) == 0;
#endif
}

void
_gtk_pop_animation_hint (void)
{
#ifdef HAVE_SCHED_UCLAMP
  GtkAnimationHint *hint = g_private_get (&animation_hint);
  struct sched_attr attr;

  g_assert (hint != NULL && hint->count > 0);

  if (--hint->count != 0 || !hint->applied)
    return;

  attr = hint->saved_attr;
  attr.size = sizeof attr;
  attr.sched_flags |= (SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN);
  syscall (SYS_sched_setattr, 0, &attr, 0);
  hint->applied = FALSE;
#endif
}
