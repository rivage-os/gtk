/* GTK - The GIMP Toolkit
 * gtkscrollboundaries.c
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include "gtkscrollboundariesprivate.h"

#include "gtkadjustmentprivate.h"
#include "gtknative.h"
#include "gtksettingsprivate.h"
#include "gtkwidgetprivate.h"

#include <math.h>

#define MAX_OVERSHOOT_DISTANCE     100
#define DECELERATION_FRICTION      4
#define DECELERATION_STOP_VELOCITY 0.1
#define MAGIC_SCROLL_FACTOR        2.5

typedef struct
{
  gboolean invalidated;
} Dispatch;

typedef struct
{
  GtkAdjustment *adjustment;
  double         value;
  double         lower;
  double         upper;
  double         page_size;
  guint64        serial;
} AdjustmentState;

typedef struct
{
  GtkScrollRouteReceiver  receiver;
  GtkScrollRouteGeometry  geometry;
  GtkWidget              *parent;
  gulong                  parent_id;
  gulong                  destroy_id;
  gulong                  unmap_id;
} ReceiverState;

typedef struct
{
  GtkScrollRouteQuery     query;
  GtkWidget              *widget;
  GtkWidget              *parent;
  gboolean                had_parent;
  GtkScrollRouteGeometry  geometry;
  GtkAdjustment          *adjustment[2];
  guint64                 serial[2];
} ReceiverSnapshot;

struct _GtkScrollRouteSnapshot
{
  guint      ref_count;
  GPtrArray *receivers;
};

static void     scroll_input_release                      (GtkScrolledWindow *self,
                                                           double             vx,
                                                           double             vy,
                                                           gint64             timestamp);
static gboolean scroll_motion_adopt_adjustment_change     (GtkScrolledWindow *self,
                                                           GtkAdjustment     *adjustment);
static void     scroll_motion_reconcile_adjustment        (GtkScrolledWindow *self,
                                                           GtkAdjustment     *adjustment);
static gboolean scroll_overscroll_adopt_adjustment_change (GtkScrolledWindow *self,
                                                           GtkAdjustment     *adjustment);

static guint64 overscroll_sequence;

static GtkWidget *
scroll_route_get_parent (GtkWidget *widget)
{
  g_assert (GTK_IS_WIDGET (widget));

  if (GTK_IS_NATIVE (widget))
    return NULL;

  return gtk_widget_get_parent (widget);
}

/* No adjustment writes, including on invalid input. The subtraction-based
 * residual stays in input space, not adjustment or presentation space.
 */
gboolean
_gtk_scroll_route_consume (double                     value,
                           double                     lower,
                           double                     upper,
                           double                     page_size,
                           double                     units_per_input,
                           double                     delta,
                           GtkScrollRouteConsumption *result)
{
  double movement;
  double maximum;
  double next;
  double consumed;

  g_return_val_if_fail (result != NULL, FALSE);

  *result = (GtkScrollRouteConsumption) {value, 0, delta};

  if (!isfinite (value) || !isfinite (lower) || !isfinite (upper) ||
      !isfinite (page_size) || page_size < 0 ||
      !isfinite (units_per_input) || units_per_input == 0 || !isfinite (delta))
    return FALSE;

  maximum = MAX (lower, upper - page_size);
  if (!isfinite (maximum) || !isfinite (maximum - lower) ||
      value < lower || value > maximum)
    return FALSE;

  movement = units_per_input * delta;
  if ((movement >= 0 && movement <= maximum - value) ||
      (movement <= 0 && -movement <= value - lower))
    {
      /* Deriving the consumed input from next - value loses precision when a
       * small kinetic delta is applied to a large adjustment value. There is
       * no boundary remainder when the complete movement fits in the range.
       */
      next = CLAMP (value + units_per_input * delta, lower, maximum);
      *result = (GtkScrollRouteConsumption) {next, delta, 0};
      return TRUE;
    }

  /* Overflow of the requested movement simply reaches the appropriate edge. */
  next = movement < 0 ? lower : maximum;
  consumed = (next - value) / units_per_input;
  consumed = CLAMP (consumed, MIN (0, delta), MAX (0, delta));

  *result = (GtkScrollRouteConsumption) {next, consumed, delta - consumed};

  return TRUE;
}

static void
receiver_destroyed_or_unmapped (GtkWidget *widget,
                                Dispatch  *dispatch)
{
  dispatch->invalidated = TRUE;
}

static void
receiver_parent_changed (GtkWidget  *widget,
                         GParamSpec *pspec,
                         Dispatch   *dispatch)
{
  dispatch->invalidated = TRUE;
}

static void
clear_signal_handler (gulong   *handler_id,
                      gpointer  instance)
{
  /* g_object_run_dispose() may already have removed these handlers. */
  if (*handler_id != 0 && !g_signal_handler_is_connected (instance, *handler_id))
    *handler_id = 0;
  else
    g_clear_signal_handler (handler_id, instance);
}

static void
adjustment_state_free (gpointer data)
{
  AdjustmentState *state = data;

  g_clear_object (&state->adjustment);
  g_free (state);
}

static void
receiver_state_free (gpointer data)
{
  ReceiverState *state = data;

  clear_signal_handler (&state->parent_id, state->receiver.widget);
  clear_signal_handler (&state->destroy_id, state->receiver.widget);
  clear_signal_handler (&state->unmap_id, state->receiver.widget);
  g_clear_object (&state->parent);
  g_clear_object (&state->receiver.widget);
  g_free (state);
}

static void
receiver_snapshot_free (gpointer data)
{
  ReceiverSnapshot *receiver = data;

  g_clear_weak_pointer (&receiver->widget);
  g_clear_weak_pointer (&receiver->parent);
  g_clear_object (&receiver->adjustment[0]);
  g_clear_object (&receiver->adjustment[1]);
  g_free (receiver);
}

static GtkScrollRouteSnapshot *
scroll_route_snapshot_new (GPtrArray *states)
{
  GtkScrollRouteSnapshot *snapshot;

  g_assert (states != NULL);

  snapshot = g_new0 (GtkScrollRouteSnapshot, 1);
  snapshot->ref_count = 1;
  snapshot->receivers = g_ptr_array_new_with_free_func (receiver_snapshot_free);

  for (guint i = 0; i < states->len; i++)
    {
      ReceiverState *state = g_ptr_array_index (states, i);
      ReceiverSnapshot *receiver = g_new0 (ReceiverSnapshot, 1);

      receiver->query = state->receiver.query;
      g_set_weak_pointer (&receiver->widget, state->receiver.widget);
      g_set_weak_pointer (&receiver->parent, state->parent);
      receiver->had_parent = state->parent != NULL;
      receiver->geometry = state->geometry;

      for (guint axis = 0; axis < 2; axis++)
        {
          GtkAdjustment *adjustment = state->geometry.adjustment[axis];

          if ((state->geometry.axes & (1 << axis)) == 0)
            {
              receiver->geometry.adjustment[axis] = NULL;
              receiver->geometry.units_per_input[axis] = 0;
              continue;
            }

          receiver->adjustment[axis] = g_object_ref (adjustment);
          receiver->geometry.adjustment[axis] = receiver->adjustment[axis];
          receiver->serial[axis] = gtk_adjustment_get_change_serial (adjustment);
        }

      g_ptr_array_add (snapshot->receivers, receiver);
    }

  return snapshot;
}

GtkScrollRouteSnapshot *
_gtk_scroll_route_snapshot_ref (GtkScrollRouteSnapshot *snapshot)
{
  g_return_val_if_fail (snapshot != NULL, NULL);

  snapshot->ref_count++;

  return snapshot;
}

void
_gtk_scroll_route_snapshot_unref (GtkScrollRouteSnapshot *snapshot)
{
  g_return_if_fail (snapshot != NULL);
  g_return_if_fail (snapshot->ref_count > 0);

  snapshot->ref_count--;
  if (snapshot->ref_count > 0)
    return;

  g_clear_pointer (&snapshot->receivers, g_ptr_array_unref);
  g_free (snapshot);
}

gsize
_gtk_scroll_route_snapshot_get_n_receivers (GtkScrollRouteSnapshot *snapshot)
{
  g_return_val_if_fail (snapshot != NULL, 0);

  return snapshot->receivers->len;
}

GtkWidget *
_gtk_scroll_route_snapshot_get_receiver (GtkScrollRouteSnapshot *snapshot,
                                         gsize                   position)
{
  ReceiverSnapshot *receiver;

  g_return_val_if_fail (snapshot != NULL, NULL);
  g_return_val_if_fail (position < snapshot->receivers->len, NULL);

  receiver = g_ptr_array_index (snapshot->receivers, position);

  return receiver->widget;
}

GtkWidget *
_gtk_scroll_route_snapshot_get_parent (GtkScrollRouteSnapshot *snapshot,
                                       gsize                   position,
                                       gboolean               *had_parent)
{
  ReceiverSnapshot *receiver;

  g_return_val_if_fail (snapshot != NULL, NULL);
  g_return_val_if_fail (position < snapshot->receivers->len, NULL);

  receiver = g_ptr_array_index (snapshot->receivers, position);

  if (had_parent != NULL)
    *had_parent = receiver->had_parent;

  return receiver->parent;
}

const GtkScrollRouteGeometry *
_gtk_scroll_route_snapshot_get_geometry (GtkScrollRouteSnapshot *snapshot,
                                         gsize                   position)
{
  ReceiverSnapshot *receiver;

  g_return_val_if_fail (snapshot != NULL, NULL);
  g_return_val_if_fail (position < snapshot->receivers->len, NULL);

  receiver = g_ptr_array_index (snapshot->receivers, position);

  return &receiver->geometry;
}

guint64
_gtk_scroll_route_snapshot_get_adjustment_serial (GtkScrollRouteSnapshot *snapshot,
                                                  gsize                   position,
                                                  guint                   axis)
{
  ReceiverSnapshot *receiver;

  g_return_val_if_fail (snapshot != NULL, 0);
  g_return_val_if_fail (position < snapshot->receivers->len, 0);
  g_return_val_if_fail (axis < 2, 0);

  receiver = g_ptr_array_index (snapshot->receivers, position);

  return receiver->serial[axis];
}

void
_gtk_scroll_route_snapshot_reconcile_adjustment (GtkScrollRouteSnapshot *snapshot,
                                                 GtkAdjustment          *adjustment)
{
  guint64 serial;

  g_return_if_fail (snapshot != NULL);
  g_return_if_fail (GTK_IS_ADJUSTMENT (adjustment));

  serial = gtk_adjustment_get_change_serial (adjustment);

  for (guint i = 0; i < snapshot->receivers->len; i++)
    {
      ReceiverSnapshot *receiver = g_ptr_array_index (snapshot->receivers, i);

      for (guint axis = 0; axis < 2; axis++)
        {
          if (receiver->adjustment[axis] == adjustment)
            receiver->serial[axis] = serial;
        }
    }
}

guint
_gtk_scroll_route_snapshot_validate (GtkScrollRouteSnapshot *snapshot,
                                     GtkScrollRouteUnit      unit)
{
  guint valid = GTK_SCROLL_ROUTE_BOTH;

  g_return_val_if_fail (snapshot != NULL, 0);

  for (guint i = 0; i < snapshot->receivers->len; i++)
    {
      ReceiverSnapshot *receiver = g_ptr_array_index (snapshot->receivers, i);
      GtkWidget *widget;
      GtkWidget *parent;
      GtkScrollRouteGeometry geometry = {0};

      widget = receiver->widget;
      parent = receiver->parent;

      if (widget == NULL ||
          (receiver->had_parent && parent == NULL) ||
          gtk_widget_get_parent (widget) != parent ||
          !receiver->query (widget, unit, &geometry) ||
          gtk_widget_get_parent (widget) != parent ||
          geometry.generation != receiver->geometry.generation)
        return 0;

      for (guint axis = 0; axis < 2; axis++)
        {
          guint mask = 1 << axis;

          if (((geometry.axes ^ receiver->geometry.axes) & mask) != 0 ||
              ((geometry.stationary_axes ^ receiver->geometry.stationary_axes) & mask) != 0 ||
              ((geometry.chain_axes ^ receiver->geometry.chain_axes) & mask) != 0)
            valid &= ~mask;

          if ((receiver->geometry.axes & mask) != 0 &&
              (geometry.adjustment[axis] != receiver->adjustment[axis] ||
               geometry.units_per_input[axis] != receiver->geometry.units_per_input[axis] ||
               gtk_adjustment_get_change_serial (receiver->adjustment[axis]) != receiver->serial[axis]))
            valid &= ~mask;
        }
    }

  return valid;
}

static gboolean
dispatch_is_valid (GPtrArray          *receivers,
                   GHashTable         *adjustments,
                   GtkScrollRouteUnit  unit,
                   Dispatch           *dispatch)
{
  GHashTableIter iter;
  gpointer value;

  if (dispatch->invalidated)
    return FALSE;

  for (guint i = 0; i < receivers->len; i++)
    {
      ReceiverState *state = g_ptr_array_index (receivers, i);
      GtkScrollRouteGeometry geometry = {0};

      if (gtk_widget_get_parent (state->receiver.widget) != state->parent ||
          !state->receiver.query (state->receiver.widget, unit, &geometry) ||
          geometry.generation != state->geometry.generation ||
          geometry.axes != state->geometry.axes ||
          geometry.stationary_axes != state->geometry.stationary_axes ||
          geometry.chain_axes != state->geometry.chain_axes)
        return FALSE;

      for (guint axis = 0; axis < 2; axis++)
        {
          if ((geometry.axes & (1 << axis)) != 0 &&
              (geometry.adjustment[axis] != state->geometry.adjustment[axis] ||
               geometry.units_per_input[axis] != state->geometry.units_per_input[axis]))
            return FALSE;
        }
    }

  g_hash_table_iter_init (&iter, adjustments);
  while (g_hash_table_iter_next (&iter, NULL, &value))
    {
      AdjustmentState *state = value;

      if (gtk_adjustment_get_change_serial (state->adjustment) != state->serial ||
          gtk_adjustment_get_value (state->adjustment) != state->value ||
          gtk_adjustment_get_lower (state->adjustment) != state->lower ||
          gtk_adjustment_get_upper (state->adjustment) != state->upper ||
          gtk_adjustment_get_page_size (state->adjustment) != state->page_size)
        return FALSE;
    }

  return !dispatch->invalidated;
}

/* Synchronous preparation API. No raw-event propagation, gesture ownership,
 * effect selection, timeout, or tick callback belongs here. Callers supply the
 * eligible chain in innermost-to-outermost order and must not replay a sample
 * after INVALIDATED. Consumed reports intended writes even if application
 * callbacks subsequently move an adjustment elsewhere. Remaining includes
 * movement stopped at barriers; stopped_axes distinguishes it from unhandled
 * terminal movement. COMPLETE does not imply that every input unit was used.
 */
void
_gtk_scroll_route_dispatch (const GtkScrollRouteReceiver  *receivers,
                            gsize                          n_receivers,
                            GtkScrollRouteUnit             unit,
                            const double                   delta[2],
                            GtkScrollRouteResult          *result,
                            GtkScrollRouteSnapshot       **snapshot)
{
  GPtrArray *states = NULL;
  GHashTable *adjustments = NULL;
  GHashTable *visited_x = NULL;
  GHashTable *visited_y = NULL;
  Dispatch dispatch = {0};

  g_return_if_fail (receivers != NULL || n_receivers == 0);
  g_return_if_fail (delta != NULL);
  g_return_if_fail (result != NULL);

  if (snapshot != NULL)
    *snapshot = NULL;

  *result = (GtkScrollRouteResult) {
    .status = GTK_SCROLL_ROUTE_INVALID,
    .remaining = {delta[0], delta[1]},
    .terminal = {G_MAXSIZE, G_MAXSIZE},
  };

  if (!isfinite (delta[0]) || !isfinite (delta[1]) ||
      (unit != GTK_SCROLL_ROUTE_NORMALIZED && unit != GTK_SCROLL_ROUTE_WHEEL))
    goto out;

  states = g_ptr_array_new_with_free_func (receiver_state_free);
  adjustments = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, adjustment_state_free);
  visited_x = g_hash_table_new (g_direct_hash, g_direct_equal);
  visited_y = g_hash_table_new (g_direct_hash, g_direct_equal);

  /* Validate and retain the entire chain before the first observable write. */
  for (gsize i = 0; i < n_receivers; i++)
    {
      ReceiverState *state;

      if (!GTK_IS_WIDGET (receivers[i].widget) || receivers[i].query == NULL)
        goto out;

      state = g_new0 (ReceiverState, 1);
      state->receiver = receivers[i];
      g_object_ref (state->receiver.widget);
      state->parent = gtk_widget_get_parent (state->receiver.widget);

      if (state->parent != NULL)
        g_object_ref (state->parent);

      state->destroy_id =
        g_signal_connect (state->receiver.widget,
                          "destroy",
                          G_CALLBACK (receiver_destroyed_or_unmapped),
                          &dispatch);
      state->unmap_id =
        g_signal_connect (state->receiver.widget,
                          "unmap",
                          G_CALLBACK (receiver_destroyed_or_unmapped),
                          &dispatch);
      state->parent_id =
        g_signal_connect (state->receiver.widget,
                          "notify::parent",
                          G_CALLBACK (receiver_parent_changed),
                          &dispatch);

      g_ptr_array_add (states, state);

      if (!state->receiver.query (state->receiver.widget, unit, &state->geometry) ||
          (state->geometry.axes & ~GTK_SCROLL_ROUTE_BOTH) != 0 ||
          (state->geometry.stationary_axes & ~state->geometry.axes) != 0 ||
          (state->geometry.chain_axes & ~GTK_SCROLL_ROUTE_BOTH) != 0)
        goto out;

      for (guint axis = 0; axis < 2; axis++)
        {
          GtkAdjustment *adjustment = state->geometry.adjustment[axis];
          GtkScrollRouteConsumption consumption;
          AdjustmentState *adj_state;

          if ((state->geometry.axes & (1 << axis)) == 0)
            continue;

          if (!GTK_IS_ADJUSTMENT (adjustment))
            goto out;

          if (!(adj_state = g_hash_table_lookup (adjustments, adjustment)))
            {
              adj_state = g_new0 (AdjustmentState, 1);
              adj_state->adjustment = g_object_ref (adjustment);
              adj_state->serial = gtk_adjustment_get_change_serial (adjustment);
              adj_state->value = gtk_adjustment_get_value (adjustment);
              adj_state->lower = gtk_adjustment_get_lower (adjustment);
              adj_state->upper = gtk_adjustment_get_upper (adjustment);
              adj_state->page_size = gtk_adjustment_get_page_size (adjustment);

              g_hash_table_insert (adjustments, adjustment, adj_state);
            }

          if (!_gtk_scroll_route_consume (adj_state->value,
                                          adj_state->lower,
                                          adj_state->upper,
                                          adj_state->page_size,
                                          state->geometry.units_per_input[axis],
                                          0,
                                          &consumption))
            goto out;
        }
    }

  result->status = GTK_SCROLL_ROUTE_INVALIDATED;

  if (!dispatch_is_valid (states, adjustments, unit, &dispatch))
    goto out;

  for (guint i = 0; i < states->len; i++)
    {
      ReceiverState *state = g_ptr_array_index (states, i);

      for (guint axis = 0; axis < 2; axis++)
        {
          GHashTable *visited = axis == 0 ? visited_x : visited_y;
          GtkAdjustment *adjustment = state->geometry.adjustment[axis];
          GtkScrollRouteConsumption consumption;
          AdjustmentState *adj_state;
          guint mask = 1 << axis;

          if (result->remaining[axis] == 0 || (result->stopped_axes & mask) != 0 ||
              (state->geometry.axes & mask) == 0 ||
              !g_hash_table_add (visited, state->receiver.widget))
            continue;

          result->terminal[axis] = i;

          if ((state->geometry.stationary_axes & mask) == 0 &&
              g_hash_table_add (visited, adjustment))
            {
              adj_state = g_hash_table_lookup (adjustments, adjustment);

              if (!_gtk_scroll_route_consume (adj_state->value,
                                              adj_state->lower,
                                              adj_state->upper,
                                              adj_state->page_size,
                                              state->geometry.units_per_input[axis],
                                              result->remaining[axis],
                                              &consumption))
                goto out;

              result->remaining[axis] = consumption.remaining;
              result->consumed[axis] = delta[axis] - result->remaining[axis];

              if (consumption.value != adj_state->value)
                {
                  adj_state->value = consumption.value;

                  /* Exactly one mutation is ours. Any additional mutation,
                   * even one undone under frozen notifications, invalidates
                   * the sample instead of becoming scroll consumption.
                   */
                  adj_state->serial++;

                  gtk_adjustment_set_value (adjustment, consumption.value);

                  if (!dispatch_is_valid (states, adjustments, unit, &dispatch))
                    goto out;
                }
            }

          if (result->remaining[axis] != 0 && (state->geometry.chain_axes & mask) == 0)
            result->stopped_axes |= mask;
        }
    }

  result->status = GTK_SCROLL_ROUTE_COMPLETE;
  if (snapshot != NULL)
    *snapshot = scroll_route_snapshot_new (states);

out:
  g_clear_pointer (&visited_y, g_hash_table_unref);
  g_clear_pointer (&visited_x, g_hash_table_unref);
  g_clear_pointer (&adjustments, g_hash_table_unref);
  g_clear_pointer (&states, g_ptr_array_unref);
}
void
_gtk_scroll_motion_axis_clear (GtkScrollMotionAxis *motion)
{
  g_return_if_fail (motion != NULL);

  motion->motion_phase = GTK_SCROLL_MOTION_REST;
  motion->raw = 0;
  motion->offset = 0;
  motion->velocity = 0;
  motion->active = FALSE;
  motion->needs_frame = FALSE;
}

void
_gtk_scroll_motion_axis_advance (GtkScrollMotionAxis *motion,
                                 gint64               timestamp,
                                 double               limit)
{
  double t, c1, c2, decay, offset, velocity;

  g_return_if_fail (motion != NULL);

  if (motion->motion_phase != GTK_SCROLL_MOTION_RETURN || timestamp <= motion->last_tick)
    return;

  motion->last_tick = timestamp;
  t = ((double) timestamp - motion->release_time) / G_USEC_PER_SEC;
  c1 = motion->initial_offset;
  c2 = motion->initial_velocity + 10 * c1;
  decay = exp (-10 * t);
  offset = (c1 + c2 * t) * decay;
  velocity = (c2 - 10 * (c1 + c2 * t)) * decay;

  if (!isfinite (offset) || !isfinite (velocity) ||
      offset * (c1 != 0 ? c1 : motion->initial_velocity) <= 0 ||
      (fabs (offset) < 0.1 && fabs (velocity) < 0.1))
    {
      _gtk_scroll_motion_axis_clear (motion);
      return;
    }

  motion->offset = CLAMP (offset, -limit * 0.8, limit * 0.8);
  motion->velocity = fabs (offset) > limit * 0.8 ? 0 : velocity;
  motion->active = motion->offset != 0;
  motion->needs_frame = TRUE;
}

gboolean
_gtk_scroll_motion_axis_handle (GtkScrollMotionAxis        *motion,
                                const GtkScrollMotionEvent *event,
                                double                      limit,
                                GtkScrollMotionReply       *reply)
{
  guint axis;
  double delta;
  double velocity;
  double old_offset;
  double consumed = 0;
  GtkPositionType side;

  g_return_val_if_fail (motion != NULL, FALSE);
  g_return_val_if_fail (event != NULL, FALSE);
  g_return_val_if_fail (reply != NULL, FALSE);

  axis = event->axes == GTK_SCROLL_ROUTE_X ? 0 : 1;
  delta = axis == 0 ? event->delta_x : event->delta_y;
  velocity = axis == 0 ? event->velocity_x : event->velocity_y;
  old_offset = motion->offset;
  side = axis == 0 ? event->side_x : event->side_y;

  *reply = (GtkScrollMotionReply) {0};

  if (event->phase == GTK_SCROLL_MOTION_CANCEL || !event->animations ||
      !(event->local_axes & (1 << axis)) || !isfinite (limit) || limit <= 0)
    {
      _gtk_scroll_motion_axis_clear (motion);
    }
  else if (event->phase == GTK_SCROLL_MOTION_BEGIN)
    {
      _gtk_scroll_motion_axis_advance (motion, event->timestamp, limit);
      motion->raw = -motion->offset / (0.5 * (1 - fabs (motion->offset) / limit));
      motion->velocity = 0;
      motion->motion_phase = GTK_SCROLL_MOTION_DIRECT;
      motion->last_tick = event->timestamp;
      motion->needs_frame = FALSE;
    }
  else if (event->phase == GTK_SCROLL_MOTION_TICK)
    {
      _gtk_scroll_motion_axis_advance (motion, event->timestamp, limit);
    }
  else if (event->phase == GTK_SCROLL_MOTION_RELEASE &&
           motion->motion_phase == GTK_SCROLL_MOTION_DIRECT)
    {
      double derivative = 0.5 / pow (1 + 0.5 * fabs (motion->raw) / limit, 2);

      if (motion->offset == 0 &&
          (!isfinite (velocity) || velocity == 0 ||
           (axis == 0 && ((side == GTK_POS_LEFT && velocity > 0) ||
                          (side == GTK_POS_RIGHT && velocity < 0))) ||
           (axis == 1 && ((side == GTK_POS_TOP && velocity > 0) ||
                          (side == GTK_POS_BOTTOM && velocity < 0)))))
        {
          _gtk_scroll_motion_axis_clear (motion);
        }
      else
        {
          motion->motion_phase = GTK_SCROLL_MOTION_RETURN;
          motion->initial_offset = motion->offset;
          motion->velocity = isfinite (velocity)
                           ? CLAMP (-velocity * derivative, -1000 * limit, 1000 * limit)
                           : 0;
          motion->initial_velocity = motion->velocity;
          motion->release_time = MAX (motion->last_tick, event->timestamp);
          motion->last_tick = motion->release_time;
          motion->needs_frame = TRUE;
        }
    }
  else if (motion->motion_phase == GTK_SCROLL_MOTION_DIRECT && isfinite (delta) && delta != 0)
    {
      if (event->phase == GTK_SCROLL_MOTION_UNWIND &&
          ((motion->raw < 0 && delta > 0) || (motion->raw > 0 && delta < 0)))
        {
          consumed = copysign (MIN (fabs (delta), fabs (motion->raw)), delta);
          motion->raw += consumed;
        }
      else if (event->phase == GTK_SCROLL_MOTION_PULL &&
               ((axis == 0 && ((delta < 0 && side == GTK_POS_LEFT) ||
                               (delta > 0 && side == GTK_POS_RIGHT))) ||
                (axis == 1 && ((delta < 0 && side == GTK_POS_TOP) ||
                               (delta > 0 && side == GTK_POS_BOTTOM)))) &&
               (motion->raw == 0 || (motion->raw > 0) == (delta > 0)))
        {
          consumed = delta;
          motion->raw = copysign (MIN (8 * limit, fabs (motion->raw) + fabs (delta)), delta);
        }

      motion->offset = -motion->raw * 0.5 / (1 + 0.5 * fabs (motion->raw) / limit);
      motion->last_tick = MAX (motion->last_tick, event->timestamp);
      motion->active = motion->offset != 0;
    }

  if (axis == 0)
    reply->consumed_x = consumed;
  else
    reply->consumed_y = consumed;

  reply->active_axes = motion->active ? 1 << axis : 0;
  reply->needs_frame = motion->needs_frame;

  return old_offset != motion->offset;
}

/* Enhanced routing is selected for the ancestor chain within this native.
 * This also enforces an outer containment barrier when the original child is
 * legacy.
 */
gboolean
_gtk_scrolled_window_boundary_chain_uses_routing (GtkWidget *widget)
{
  for (; widget != NULL; widget = scroll_route_get_parent (widget))
    {
      GtkScrolledWindow *self;
      GtkScrolledWindowBoundary *boundary;
      GtkWidget *child;
      gboolean explicit_policy;
      gboolean child_presents_overscroll;

      if (!GTK_IS_SCROLLED_WINDOW (widget))
        continue;

      self = GTK_SCROLLED_WINDOW (widget);
      boundary = _gtk_scrolled_window_get_boundary (self);
      child = _gtk_scrolled_window_get_scrollable_child (self);

      explicit_policy = boundary->overscroll_behavior_set[0] ||
                        boundary->overscroll_behavior_set[1];
      child_presents_overscroll = GTK_IS_SCROLLABLE (child) &&
                                  (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (child), GTK_ORIENTATION_HORIZONTAL) != GTK_OVERSCROLL_BEHAVIOR_NONE ||
                                   gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (child), GTK_ORIENTATION_VERTICAL) != GTK_OVERSCROLL_BEHAVIOR_NONE);

      if (explicit_policy || child_presents_overscroll)
        return TRUE;
    }

  return FALSE;
}

static gboolean
scrollable_get_geometry (GtkScrolledWindow                   *self,
                         GtkScrollable                       *scrollable,
                         GtkScrolledWindowScrollableGeometry *geometry)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkBorder border = {0};

  g_assert (GTK_IS_SCROLLED_WINDOW (self));
  g_assert (GTK_IS_SCROLLABLE (scrollable));

  if (_gtk_scrolled_window_get_scrollable_child (self) != GTK_WIDGET (scrollable))
    return FALSE;

  gtk_scrollable_get_border (scrollable, &border);

  *geometry = (GtkScrolledWindowScrollableGeometry) {
    .bounds = GRAPHENE_RECT_INIT (border.left,
                                  border.top,
                                  MAX (0, gtk_widget_get_width (GTK_WIDGET (scrollable)) - border.left - border.right),
                                  MAX (0, gtk_widget_get_height (GTK_WIDGET (scrollable)) - border.top - border.bottom)),
    .mapping_x = gtk_scrollable_get_scroll_factor (scrollable, GTK_ORIENTATION_HORIZONTAL),
    .mapping_y = gtk_scrollable_get_scroll_factor (scrollable, GTK_ORIENTATION_VERTICAL),
    .generation = priv->route_generation,
  };

  for (guint axis = 0; axis < 2; axis++)
    {
      GtkAdjustment *adjustment = axis == 0
                                ? gtk_scrollable_get_hadjustment (scrollable)
                                : gtk_scrollable_get_vadjustment (scrollable);
      double factor = axis == 0 ? geometry->mapping_x : geometry->mapping_y;
      gboolean has_scroll_range;

      if (adjustment == NULL || !isfinite (factor) || factor == 0)
        continue;

      has_scroll_range = (gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment)) > gtk_adjustment_get_lower (adjustment);

      if (has_scroll_range || priv->overscroll_behavior[axis] != GTK_OVERSCROLL_BEHAVIOR_AUTO)
        geometry->axes |= 1 << axis;
    }

  return _gtk_scrolled_window_get_scrollable_child (self) == GTK_WIDGET (scrollable);
}

static gboolean
scroll_route_query (GtkWidget *widget,
                    GtkScrollRouteUnit unit,
                    GtkScrollRouteGeometry *geometry)
{
  GtkScrolledWindow *self = GTK_SCROLLED_WINDOW (widget);
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowScrollableGeometry scrollable_geometry = {0};
  GtkWidget *child;
  guint64 generation = priv->route_generation;
  gboolean ret = FALSE;

  if ((child = _gtk_scrolled_window_get_scrollable_child (self)))
    g_object_ref (child);

  if (_gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL) == NULL ||
      _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL) == NULL)
    goto out;

  if (!GTK_IS_SCROLLABLE (child) ||
      !scrollable_get_geometry (self, GTK_SCROLLABLE (child), &scrollable_geometry))
    goto out;

  if (_gtk_scrolled_window_get_scrollable_child (self) != child || priv->route_generation != generation ||
      _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL) == NULL ||
      _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL) == NULL)
    goto out;

  *geometry = (GtkScrollRouteGeometry) {
    .adjustment = { _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL),
                    _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL) },
    .units_per_input = { scrollable_geometry.mapping_x, scrollable_geometry.mapping_y },
    .generation = generation,
    .axes = scrollable_geometry.axes,
  };

  for (guint axis = 0; axis < 2; axis++)
    {
      if (priv->overscroll_behavior[axis] == GTK_OVERSCROLL_BEHAVIOR_AUTO)
        geometry->chain_axes |= 1 << axis;

      if (unit == GTK_SCROLL_ROUTE_WHEEL)
        geometry->units_per_input[axis] = copysign (_gtk_scrolled_window_get_wheel_detent_scroll_step (self, axis),
                                                    geometry->units_per_input[axis]);

      /* A zero-page wheel receiver cannot consume detents, but remains a
       * policy barrier on its participating axes.
       */
      if (unit == GTK_SCROLL_ROUTE_WHEEL && geometry->units_per_input[axis] == 0)
        {
          geometry->stationary_axes |= geometry->axes & (1 << axis);
          geometry->units_per_input[axis] = 1;
        }
    }

  if (unit == GTK_SCROLL_ROUTE_NORMALIZED && priv->route_origin != NULL)
    {
      GtkWidget *target = child != NULL ? child : widget;
      graphene_matrix_t transform;
      double xx, yx, xy, yy, x0, y0;

      if (!gtk_widget_compute_transform (priv->route_origin, target, &transform) ||
          !graphene_matrix_to_2d (&transform, &xx, &yx, &xy, &yy, &x0, &y0) ||
          xy != 0 || yx != 0 || xx == 0 || yy == 0)
        goto out;

      geometry->units_per_input[0] *= xx;
      geometry->units_per_input[1] *= yy;
    }

  ret = TRUE;

out:
  g_clear_object (&child);
  return ret;
}

gboolean
_gtk_scrolled_window_boundary_query (GtkScrolledWindow      *self,
                                     GtkScrollRouteUnit      unit,
                                     GtkScrollRouteGeometry *geometry)
{
  g_return_val_if_fail (GTK_IS_SCROLLED_WINDOW (self), FALSE);
  g_return_val_if_fail (geometry != NULL, FALSE);

  return scroll_route_query (GTK_WIDGET (self), unit, geometry);
}

static gboolean
scroll_motion_query (GtkScrolledWindow      *origin,
                     GtkWidget              *widget,
                     GtkScrollRouteGeometry *geometry)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (GTK_SCROLLED_WINDOW (widget));
  GtkScrolledWindowBoundary *origin_priv = _gtk_scrolled_window_get_boundary (origin);
  GtkWidget *previous = priv->route_origin;
  gboolean result;

  priv->route_origin = GTK_WIDGET (origin);
  result = scroll_route_query (widget, origin_priv->input.unit, geometry);
  priv->route_origin = previous;

  return result;
}

static guint
scroll_motion_validate (GtkScrolledWindow *self)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  guint valid = GTK_SCROLL_ROUTE_BOTH;
  GtkScrollRouteSnapshot *snapshot;
  gsize n_receivers;

  if (priv->motion_snapshot == NULL)
    return 0;

  snapshot = _gtk_scroll_route_snapshot_ref (priv->motion_snapshot);
  n_receivers = _gtk_scroll_route_snapshot_get_n_receivers (snapshot);

  for (guint i = 0; i < n_receivers; i++)
    {
      GtkWidget *widget;
      GtkWidget *parent;
      const GtkScrollRouteGeometry *saved;
      GtkScrollRouteGeometry geometry;
      gboolean had_parent;

      widget = _gtk_scroll_route_snapshot_get_receiver (snapshot, i);
      parent = _gtk_scroll_route_snapshot_get_parent (snapshot, i, &had_parent);
      saved = _gtk_scroll_route_snapshot_get_geometry (snapshot, i);

      if (widget == NULL || (had_parent && parent == NULL) ||
          gtk_widget_get_parent (widget) != parent ||
          (widget != GTK_WIDGET (self) && !gtk_widget_is_ancestor (GTK_WIDGET (self), widget)) ||
          !scroll_motion_query (self, widget, &geometry) ||
          gtk_widget_get_parent (widget) != parent ||
          geometry.generation != saved->generation)
        {
          valid = 0;
          goto out;
        }

      for (guint axis = 0; axis < 2; axis++)
        {
          guint mask = 1 << axis;

          if (((geometry.axes ^ saved->axes) & mask) != 0 ||
              ((geometry.stationary_axes ^ saved->stationary_axes) & mask) != 0 ||
              ((geometry.chain_axes ^ saved->chain_axes) & mask) != 0)
            {
              valid &= ~mask;
              continue;
            }

          if ((saved->axes & mask) != 0 &&
              (geometry.adjustment[axis] != saved->adjustment[axis] ||
               geometry.units_per_input[axis] != saved->units_per_input[axis] ||
               gtk_adjustment_get_change_serial (geometry.adjustment[axis]) != _gtk_scroll_route_snapshot_get_adjustment_serial (snapshot, i, axis)))
            valid &= ~mask;
        }
    }

out:
  _gtk_scroll_route_snapshot_unref (snapshot);

  return valid;
}

static gboolean
scroll_motion_adopt_adjustment_change (GtkScrolledWindow *self,
                                       GtkAdjustment     *adjustment)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrollRouteSnapshot *snapshot;
  GtkScrollable *child;
  guint axis;
  gsize n_receivers;
  gboolean ret = FALSE;

  if (priv->motion_snapshot == NULL)
    return FALSE;

  if (adjustment == _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL))
    axis = GTK_ORIENTATION_HORIZONTAL;
  else if (adjustment == _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL))
    axis = GTK_ORIENTATION_VERTICAL;
  else
    return FALSE;

  if (_gtk_scrolled_window_get_scrollable_child (self) == NULL ||
      !GTK_IS_SCROLLABLE (_gtk_scrolled_window_get_scrollable_child (self)))
    return FALSE;

  child = GTK_SCROLLABLE (_gtk_scrolled_window_get_scrollable_child (self));

  if ((axis == GTK_ORIENTATION_HORIZONTAL &&
       gtk_scrollable_get_hadjustment (child) != adjustment) ||
      (axis == GTK_ORIENTATION_VERTICAL &&
       gtk_scrollable_get_vadjustment (child) != adjustment))
    return FALSE;

  snapshot = _gtk_scroll_route_snapshot_ref (priv->motion_snapshot);
  n_receivers = _gtk_scroll_route_snapshot_get_n_receivers (snapshot);

  for (guint i = 0; i < n_receivers; i++)
    {
      GtkWidget *widget;
      GtkWidget *parent;
      const GtkScrollRouteGeometry *saved;
      GtkScrollRouteGeometry geometry;
      gboolean had_parent;

      widget = _gtk_scroll_route_snapshot_get_receiver (snapshot, i);
      parent = _gtk_scroll_route_snapshot_get_parent (snapshot, i, &had_parent);
      saved = _gtk_scroll_route_snapshot_get_geometry (snapshot, i);

      if (widget == NULL || (had_parent && parent == NULL) ||
          gtk_widget_get_parent (widget) != parent ||
          (widget != GTK_WIDGET (self) && !gtk_widget_is_ancestor (GTK_WIDGET (self), widget)) ||
          !scroll_motion_query (self, widget, &geometry) ||
          gtk_widget_get_parent (widget) != parent ||
          geometry.generation != saved->generation)
        goto out;

      for (guint candidate_axis = 0; candidate_axis < 2; candidate_axis++)
        {
          guint mask = 1 << candidate_axis;

          if (((geometry.axes ^ saved->axes) & mask) != 0 ||
              ((geometry.stationary_axes ^ saved->stationary_axes) & mask) != 0 ||
              ((geometry.chain_axes ^ saved->chain_axes) & mask) != 0)
            goto out;

          if ((saved->axes & mask) != 0)
            {
              gboolean adopting_serial =
                candidate_axis == axis &&
                geometry.adjustment[candidate_axis] == adjustment &&
                saved->adjustment[candidate_axis] == adjustment;

              if (geometry.adjustment[candidate_axis] != saved->adjustment[candidate_axis] ||
                  geometry.units_per_input[candidate_axis] != saved->units_per_input[candidate_axis] ||
                  (!adopting_serial &&
                   gtk_adjustment_get_change_serial (geometry.adjustment[candidate_axis]) != _gtk_scroll_route_snapshot_get_adjustment_serial (snapshot, i, candidate_axis)))
                goto out;
            }
        }
    }

  _gtk_scroll_route_snapshot_reconcile_adjustment (priv->motion_snapshot, adjustment);
  ret = TRUE;

out:
  _gtk_scroll_route_snapshot_unref (snapshot);
  return ret;
}

static gboolean
scroll_overscroll_geometry_matches (const GtkScrolledWindowScrollableGeometry *a,
                                    const GtkScrolledWindowScrollableGeometry *b,
                                    guint                                      axis)
{
  return a->generation == b->generation &&
         ((a->axes ^ b->axes) & (1 << axis)) == 0 &&
         (axis == 0 ? (a->mapping_x == b->mapping_x &&
                       a->bounds.origin.x == b->bounds.origin.x &&
                       a->bounds.size.width == b->bounds.size.width)
                    : (a->mapping_y == b->mapping_y &&
                       a->bounds.origin.y == b->bounds.origin.y &&
                       a->bounds.size.height == b->bounds.size.height));
}

static void
scroll_motion_notify (GtkScrolledWindow *self,
                      GtkScrollable     *child)
{
  GtkScrollableInterface *iface;
  GtkScrolledWindow *hold_self;
  GtkScrollable *hold_child;

  if (child == NULL)
    return;

  hold_self = g_object_ref (self);
  hold_child = g_object_ref (child);

  gtk_widget_queue_allocate (GTK_WIDGET (self));
  gtk_widget_queue_draw (GTK_WIDGET (child));

  iface = GTK_SCROLLABLE_GET_IFACE (child);
  if (iface->overscroll_changed != NULL)
    iface->overscroll_changed (child);

  g_object_unref (hold_child);
  g_object_unref (hold_self);
}

static void
scroll_overscroll_handle_motion (GtkScrolledWindow                *self,
                                 GtkScrolledWindowOverscrollOwner *owner,
                                 guint                             axis,
                                 const GtkScrollMotionEvent       *event,
                                 GtkScrollMotionReply             *reply)
{
  double extent = axis == 0 ? owner->geometry.bounds.size.width : owner->geometry.bounds.size.height;
  double limit = MIN (MAX_OVERSHOOT_DISTANCE, extent / 4);

  if (_gtk_scroll_motion_axis_handle (&owner->motion, event, limit, reply))
    scroll_motion_notify (self, owner->child);
}

/* Each receiver retains only its own child. Session ids associate ancestor
 * owners with the originating controller without retaining the widget chain.
 */
static void
scroll_overscroll_detach_owner (GtkScrolledWindowOverscrollOwner *owner)
{
  g_clear_object (&owner->origin_signals);
  g_clear_object (&owner->child);

  *owner = (GtkScrolledWindowOverscrollOwner) {0};
}

static gboolean
scroll_overscroll_owner_is_at_edge (GtkScrolledWindow *self,
                                    guint              axis)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowOverscrollOwner *owner;
  GtkAdjustment *adjustment;
  GtkPositionType side;
  double mapping;
  double direction;
  double edge;

  g_assert (axis < 2);

  owner = &priv->overscroll_axis[axis];
  g_assert (owner->child != NULL);

  if (axis == 0)
    {
      adjustment = _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL);
      side = owner->event.side_x;
      mapping = owner->geometry.mapping_x;
    }
  else
    {
      adjustment = _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL);
      side = owner->event.side_y;
      mapping = owner->geometry.mapping_y;
    }

  direction = (side == GTK_POS_LEFT || side == GTK_POS_TOP ? -1 : 1) * mapping;
  edge = direction < 0
           ? gtk_adjustment_get_lower (adjustment)
           : MAX (gtk_adjustment_get_lower (adjustment),
                  gtk_adjustment_get_upper (adjustment) - gtk_adjustment_get_page_size (adjustment));

  return direction != 0 && gtk_adjustment_get_value (adjustment) == edge;
}

static gboolean
scroll_overscroll_owner_delta_is_outward (GtkScrolledWindowOverscrollOwner *owner,
                                          guint                             axis,
                                          double                            delta)
{
  GtkPositionType side;

  g_assert (axis < 2);

  side = axis == 0 ? owner->event.side_x : owner->event.side_y;
  delta *= owner->scale;

  return axis == 0
           ? ((side == GTK_POS_LEFT && delta < 0) ||
              (side == GTK_POS_RIGHT && delta > 0))
           : ((side == GTK_POS_TOP && delta < 0) ||
              (side == GTK_POS_BOTTOM && delta > 0));
}

void
_gtk_scrolled_window_boundary_cancel (GtkScrolledWindow *self,
                                      guint              axes)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindow *hold_self;
  GtkScrolledWindowOverscrollOwner cancelled[2] = {0};

  hold_self = g_object_ref (self);

  if (axes == GTK_SCROLL_ROUTE_BOTH)
    {
      priv->touch_release_pending = FALSE;
      if (!priv->advancing_motion)
        g_clear_pointer (&priv->motion_snapshot, _gtk_scroll_route_snapshot_unref);
    }

  for (guint axis = 0; axis < 2; axis++)
    {
      if (axes & (1 << axis))
        priv->motion_velocity[axis] = 0;
    }

  /* Detach all selected records before entering any application handler. */
  for (guint axis = 0; axis < 2; axis++)
    {
      if (axes & (1 << axis))
        {
          cancelled[axis] = priv->overscroll_axis[axis];
          priv->overscroll_axis[axis] = (GtkScrolledWindowOverscrollOwner) {0};
        }
    }

  for (guint axis = 0; axis < 2; axis++)
    {
      GtkScrolledWindowOverscrollOwner *owner = &cancelled[axis];
      GtkScrollMotionReply reply;

      if (owner->child == NULL)
        continue;

      g_clear_object (&owner->origin_signals);

      owner->event.phase = GTK_SCROLL_MOTION_CANCEL;
      owner->event.timestamp = g_get_monotonic_time ();

      scroll_overscroll_handle_motion (self, owner, axis, &owner->event, &reply);
      scroll_overscroll_detach_owner (owner);
    }

  if (priv->overscroll_tick_id != 0 &&
      !priv->overscroll_axis[0].motion.needs_frame &&
      !priv->overscroll_axis[1].motion.needs_frame &&
      priv->motion_velocity[0] == 0 && priv->motion_velocity[1] == 0 && !priv->advancing_motion)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), priv->overscroll_tick_id);
      priv->overscroll_tick_id = 0;
    }

  g_object_unref (hold_self);
}

static void
scroll_overscroll_origin_gone (GtkWidget         *origin,
                               GtkScrolledWindow *self)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);

  for (guint axis = 0; axis < 2; axis++)
    {
      GtkScrolledWindowOverscrollOwner *owner = &priv->overscroll_axis[axis];
      GObject *target = NULL;

      if (owner->origin_signals != NULL)
        target = g_signal_group_dup_target (owner->origin_signals);

      if (target == G_OBJECT (origin))
        _gtk_scrolled_window_boundary_cancel (self, 1 << axis);

      g_clear_object (&target);
    }
}

static void
scroll_overscroll_origin_parent_changed (GtkWidget         *origin,
                                         GParamSpec        *pspec,
                                         GtkScrolledWindow *self)
{
  scroll_overscroll_origin_gone (origin, self);
}

static gboolean
scroll_overscroll_send (GtkScrolledWindow      *self,
                        guint                   axis,
                        GtkScrollMotionCommand  phase,
                        double                  delta,
                        gint64                  timestamp,
                        GtkScrollMotionReply   *reply)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowOverscrollOwner *owner = &priv->overscroll_axis[axis];
  GtkScrollable *child = NULL;
  GtkScrolledWindowScrollableGeometry geometry;
  GtkScrollMotionEvent event;
  guint64 sequence;
  GtkAdjustment *adjustment = NULL;
  GObject *origin = NULL;
  graphene_matrix_t transform;
  double xx, xy, yx, yy, x0, y0;
  gboolean ret = FALSE;

  *reply = (GtkScrollMotionReply) {0};
  if (owner->child == NULL)
    goto out;

  origin = g_signal_group_dup_target (owner->origin_signals);
  if (origin == NULL ||
      (origin != G_OBJECT (self) && !gtk_widget_is_ancestor (GTK_WIDGET (origin), GTK_WIDGET (self))) ||
      !gtk_widget_compute_transform (GTK_WIDGET (origin), GTK_WIDGET (owner->child), &transform) ||
      !graphene_matrix_to_2d (&transform, &xx, &yx, &xy, &yy, &x0, &y0) ||
      xy != 0 || yx != 0 || (axis == 0 ? xx : yy) != owner->scale)
    {
      _gtk_scrolled_window_boundary_cancel (self, 1 << axis);
      goto out;
    }

  child = g_object_ref (owner->child);
  sequence = owner->event.sequence;

  if (!scrollable_get_geometry (self, child, &geometry))
    goto out;

  if (owner->child != child || owner->event.sequence != sequence)
    goto out;

  if (geometry.generation != owner->event.generation)
    {
      _gtk_scrolled_window_boundary_cancel (self, GTK_SCROLL_ROUTE_BOTH);
      goto out;
    }

  adjustment = g_object_ref (axis == 0 ? _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL)
                                       : _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL));

  if (priv->overscroll_behavior[axis] == GTK_OVERSCROLL_BEHAVIOR_NONE ||
      !scroll_overscroll_geometry_matches (&geometry, &owner->geometry, axis) ||
      gtk_adjustment_get_change_serial (adjustment) != owner->adjustment_serial ||
      _gtk_scrolled_window_get_scrollable_child (self) != GTK_WIDGET (child) ||
      owner->generation != priv->route_generation ||
      geometry.generation != owner->event.generation ||
      !(geometry.axes & (1 << axis)))
    {
      _gtk_scrolled_window_boundary_cancel (self, 1 << axis);
      goto out;
    }

  if (phase == GTK_SCROLL_MOTION_TICK)
    {
      if (timestamp <= owner->motion.last_tick)
        {
          reply->active_axes = owner->motion.active ? 1 << axis : 0;
          reply->needs_frame = owner->motion.needs_frame;
          ret = TRUE;
          goto out;
        }
    }

  event = owner->event;
  event.phase = phase;
  event.timestamp = timestamp;
  event.delta_x = axis == 0 ? delta * owner->scale : 0;
  event.delta_y = axis == 1 ? delta * owner->scale : 0;

  g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)),
                "gtk-enable-animations", &event.animations,
                NULL);

  scroll_overscroll_handle_motion (self, owner, axis, &event, reply);
  if (owner->child != child || owner->event.sequence != sequence ||
      owner->generation != priv->route_generation || _gtk_scrolled_window_get_scrollable_child (self) != GTK_WIDGET (child))
    goto out;

  if (phase == GTK_SCROLL_MOTION_PULL || phase == GTK_SCROLL_MOTION_UNWIND)
    {
      double before = owner->notification_distance;
      double consumed = axis == 0 ? reply->consumed_x : reply->consumed_y;
      double mapping = axis == 0 ? geometry.mapping_x : geometry.mapping_y;

      owner->notification_distance = CLAMP (before + consumed * mapping,
                                            -MAX_OVERSHOOT_DISTANCE,
                                            MAX_OVERSHOOT_DISTANCE);

      if (fabs (before) < MAX_OVERSHOOT_DISTANCE &&
          fabs (owner->notification_distance) == MAX_OVERSHOOT_DISTANCE)
        {
          GtkPositionType side;

          if (axis == 0)
            side = owner->notification_distance < 0 ? GTK_POS_LEFT : GTK_POS_RIGHT;
          else
            side = owner->notification_distance < 0 ? GTK_POS_TOP : GTK_POS_BOTTOM;

          if (axis == 0 && gtk_widget_get_direction (GTK_WIDGET (self)) == GTK_TEXT_DIR_RTL)
            side = side == GTK_POS_LEFT ? GTK_POS_RIGHT : GTK_POS_LEFT;

          _gtk_scrolled_window_emit_edge_overshot (self, side);

          if (owner->child != child || owner->event.sequence != sequence)
            goto out;
        }
    }

  if (!scrollable_get_geometry (self, child, &geometry))
    goto out;

  if (owner->child != child || owner->event.sequence != sequence)
    goto out;

  if ((origin != G_OBJECT (self) &&
       !gtk_widget_is_ancestor (GTK_WIDGET (origin), GTK_WIDGET (self))) ||
      !gtk_widget_compute_transform (GTK_WIDGET (origin), GTK_WIDGET (child), &transform) ||
      !graphene_matrix_to_2d (&transform, &xx, &yx, &xy, &yy, &x0, &y0) ||
      xy != 0 || yx != 0 || (axis == 0 ? xx : yy) != owner->scale ||
      priv->overscroll_behavior[axis] == GTK_OVERSCROLL_BEHAVIOR_NONE ||
      !scroll_overscroll_geometry_matches (&geometry, &owner->geometry, axis) ||
      gtk_adjustment_get_change_serial (adjustment) != owner->adjustment_serial)
    {
      _gtk_scrolled_window_boundary_cancel (self, geometry.generation != owner->event.generation ? GTK_SCROLL_ROUTE_BOTH : 1 << axis);
      goto out;
    }

  ret = TRUE;

out:
  g_clear_object (&origin);
  g_clear_object (&adjustment);
  g_clear_object (&child);

  return ret;
}

gboolean
_gtk_scrolled_window_advance_overscroll (GtkScrolledWindow *self,
                                         gint64             timestamp)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  gboolean needs_frame = FALSE;

  g_return_val_if_fail (GTK_IS_SCROLLED_WINDOW (self), FALSE);

  g_object_ref (self);

  if (priv->advancing_motion)
    {
      g_object_unref (self);
      return TRUE;
    }

  priv->advancing_motion = TRUE;

  if (priv->touch_release_pending)
    scroll_input_release (self, 0, 0, priv->input.timestamp);

  if (priv->motion_snapshot != NULL)
    {
      guint valid = scroll_motion_validate (self);

      for (guint axis = 0; axis < 2; axis++)
        {
          if (!(valid & (1 << axis)))
            priv->motion_velocity[axis] = 0;
        }
    }

  if (timestamp > priv->motion_time &&
      (priv->motion_velocity[0] != 0 || priv->motion_velocity[1] != 0))
    {
      GtkScrolledWindowScrollInput input = priv->input;
      double delta[2];
      double elapsed = (timestamp - priv->motion_time) / (double) G_USEC_PER_SEC;

      input.inertia = TRUE;
      input.interval_start = priv->motion_time;
      input.timestamp = timestamp;
      input.session = priv->motion_session;

      for (guint axis = 0; axis < 2; axis++)
        {
          double velocity = priv->motion_velocity[axis];
          double duration = fabs (velocity) > DECELERATION_STOP_VELOCITY
                          ? MIN (elapsed, log (fabs (velocity) / DECELERATION_STOP_VELOCITY) / DECELERATION_FRICTION)
                          : 0;

          input.velocity[axis] = velocity;
          delta[axis] = velocity * -expm1 (-DECELERATION_FRICTION * duration) / DECELERATION_FRICTION;
          priv->motion_velocity[axis] = duration < elapsed ? 0 : velocity * exp (-DECELERATION_FRICTION * duration);
        }

      priv->motion_time = timestamp;

      _gtk_scrolled_window_boundary_route_input_full (self, input.unit, delta[0], delta[1], NULL, &input);
    }

  for (guint axis = 0; axis < 2; axis++)
    {
      GtkScrolledWindowOverscrollOwner *owner = &priv->overscroll_axis[axis];
      GtkScrollMotionReply reply;

      if (owner->motion.needs_frame &&
          scroll_overscroll_send (self, axis, GTK_SCROLL_MOTION_TICK, 0, timestamp, &reply))
        needs_frame |= reply.needs_frame;

      if (owner->child != NULL && owner->released &&
          !owner->motion.active && !owner->motion.needs_frame)
        scroll_overscroll_detach_owner (owner);
    }

  needs_frame |= priv->motion_velocity[0] != 0 || priv->motion_velocity[1] != 0;
  priv->advancing_motion = FALSE;

  if (!needs_frame && priv->overscroll_tick_id != 0)
    {
      gtk_widget_remove_tick_callback (GTK_WIDGET (self), priv->overscroll_tick_id);
      priv->overscroll_tick_id = 0;
    }

  if (priv->motion_velocity[0] == 0 && priv->motion_velocity[1] == 0)
    g_clear_pointer (&priv->motion_snapshot, _gtk_scroll_route_snapshot_unref);
  g_object_unref (self);

  return needs_frame;
}

static gboolean
scroll_overscroll_tick (GtkWidget     *widget,
                        GdkFrameClock *clock,
                        gpointer       data)
{
  GtkScrolledWindow *self = GTK_SCROLLED_WINDOW (widget);
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  gboolean needs_frame;

  needs_frame = _gtk_scrolled_window_advance_overscroll (self, gdk_frame_clock_get_frame_time (clock));

  if (!needs_frame)
    priv->overscroll_tick_id = 0;

  return needs_frame;
}

static void
scroll_overscroll_stop_notify (gpointer data)
{
  gtk_widget_pop_animation_hint (GTK_WIDGET (data));
}

void
_gtk_scrolled_window_boundary_schedule (GtkScrolledWindow *self)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);

  if (priv->overscroll_tick_id == 0 &&
      (priv->overscroll_axis[0].motion.needs_frame ||
       priv->overscroll_axis[1].motion.needs_frame ||
       priv->motion_velocity[0] != 0 || priv->motion_velocity[1] != 0 ||
       priv->touch_release_pending))
    {
      gtk_widget_push_animation_hint (GTK_WIDGET (self));
      priv->overscroll_tick_id = gtk_widget_add_tick_callback (GTK_WIDGET (self),
                                                               scroll_overscroll_tick, self,
                                                               scroll_overscroll_stop_notify);
    }
}

void
_gtk_scrolled_window_boundary_release (GtkScrolledWindow      *self,
                                       GtkScrollMotionCommand  phase)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GPtrArray *chain = g_ptr_array_new_with_free_func (g_object_unref);
  guint64 session = priv->input_session;

  if (session == 0)
    goto out;

  for (GtkWidget *widget = GTK_WIDGET (self); widget; widget = scroll_route_get_parent (widget))
    {
      if (GTK_IS_SCROLLED_WINDOW (widget))
        g_ptr_array_add (chain, g_object_ref (widget));
    }

  for (guint i = 0; i < chain->len; i++)
    {
      GtkScrolledWindow *receiver = chain->pdata[i];
      GtkScrolledWindowBoundary *receiver_priv = _gtk_scrolled_window_get_boundary (receiver);

      for (guint axis = 0; axis < 2; axis++)
        {
          GtkScrolledWindowOverscrollOwner *owner = &receiver_priv->overscroll_axis[axis];
          GtkScrollMotionReply reply;

          if (owner->session != session)
            continue;

          if (phase == GTK_SCROLL_MOTION_CANCEL)
            {
              _gtk_scrolled_window_boundary_cancel (receiver, 1 << axis);
            }
          else if (!owner->released)
            {
              priv->motion_velocity[axis] = 0;

              owner->released = TRUE;
              owner->event.velocity_x = axis == 0 ? priv->input.velocity[0] * owner->scale : 0;
              owner->event.velocity_y = axis == 1 ? priv->input.velocity[1] * owner->scale : 0;
              scroll_overscroll_send (receiver, axis, phase, 0, priv->input.timestamp, &reply);

              _gtk_scrolled_window_boundary_schedule (receiver);
            }
        }
    }

out:
  g_ptr_array_unref (chain);
}

static guint64
scroll_overscroll_next_sequence (void)
{
  /* Keep sequence numbers ordered with monotonic timestamps. */
  overscroll_sequence = MAX (overscroll_sequence + 1, (guint64) g_get_monotonic_time ());
  return overscroll_sequence;
}

/* A pull belongs to the terminal scroller, even when its presentation moves
 * the next event from a descendant scroller onto an ancestor (or back).
 * Transfer the input session without resetting the terminal presentation.
 */
static gboolean
scroll_overscroll_transfer_origin (GtkScrolledWindow                  *self,
                                   GtkScrolledWindow                  *origin,
                                   guint                               axis,
                                   const GtkScrolledWindowScrollInput *input)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowOverscrollOwner *owner = &priv->overscroll_axis[axis];
  GtkScrolledWindowScrollableGeometry geometry;
  GtkScrollRouteGeometry route;
  GtkScrollMotionReply reply;
  GtkScrollable *child = NULL;
  GObject *old_origin = NULL;
  guint64 sequence;
  double mapping;
  double scale;
  gboolean ret = FALSE;

  g_assert (owner->child != NULL);
  g_assert (input->continuous && !input->inertia);

  sequence = owner->event.sequence;
  child = g_object_ref (owner->child);
  old_origin = g_signal_group_dup_target (owner->origin_signals);

  if (old_origin == NULL ||
      (old_origin != G_OBJECT (origin) &&
       !gtk_widget_is_ancestor (GTK_WIDGET (old_origin), GTK_WIDGET (origin)) &&
       !gtk_widget_is_ancestor (GTK_WIDGET (origin), GTK_WIDGET (old_origin))) ||
      owner->event.source != input->source ||
      owner->generation != priv->route_generation ||
      owner->event.generation != priv->route_generation ||
      !scrollable_get_geometry (self, child, &geometry) ||
      owner->child != child || owner->event.sequence != sequence ||
      !scroll_overscroll_geometry_matches (&geometry, &owner->geometry, axis) ||
      !(geometry.axes & (1 << axis)) ||
      !scroll_motion_query (origin, GTK_WIDGET (self), &route) ||
      owner->child != child || owner->event.sequence != sequence ||
      !(route.axes & (1 << axis)) ||
      route.adjustment[axis] != _gtk_scrolled_window_get_adjustment (self, axis) ||
      gtk_adjustment_get_change_serial (route.adjustment[axis]) != owner->adjustment_serial)
    goto out;

  mapping = axis == 0 ? geometry.mapping_x : geometry.mapping_y;
  scale = route.units_per_input[axis] / mapping;
  if (!isfinite (scale) || scale == 0)
    goto out;

  owner->session = input->session;
  owner->scale = scale;
  owner->event.sequence = scroll_overscroll_next_sequence ();
  owner->event.timestamp = input->timestamp;
  owner->event.velocity_x = owner->event.velocity_y = 0;
  owner->released = FALSE;
  g_signal_group_set_target (owner->origin_signals, origin);

  ret = scroll_overscroll_send (self, axis, GTK_SCROLL_MOTION_BEGIN, 0,
                                input->timestamp, &reply);

out:
  g_clear_object (&old_origin);
  g_clear_object (&child);
  return ret;
}

static void
scroll_overscroll_pull (GtkScrolledWindow                  *self,
                        GtkScrolledWindow                  *origin,
                        guint                               axis,
                        double                              delta,
                        const GtkScrolledWindowScrollInput *input)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowBoundary *origin_priv = _gtk_scrolled_window_get_boundary (origin);
  GtkScrolledWindowOverscrollOwner *owner = &priv->overscroll_axis[axis];
  GtkScrollRouteGeometry route;
  GtkScrolledWindowScrollableGeometry geometry;
  GtkScrollMotionReply reply;
  GtkScrollable *child = NULL;
  guint64 generation = priv->route_generation;
  double mapping;

  GtkScrolledWindowScrollInput crossing = *input;

  if (input->inertia)
    {
      double velocity = input->velocity[axis];
      double elapsed = (input->timestamp - input->interval_start) / (double) G_USEC_PER_SEC;
      double duration = fabs (velocity) > DECELERATION_STOP_VELOCITY ? MIN (elapsed, log (fabs (velocity) / DECELERATION_STOP_VELOCITY) / DECELERATION_FRICTION) : 0;
      double distance = velocity * -expm1 (-DECELERATION_FRICTION * duration) / DECELERATION_FRICTION;
      double consumed = distance - delta;
      double remaining_velocity = velocity - DECELERATION_FRICTION * consumed;

      crossing.timestamp = input->interval_start +
                           (gint64) llround (-log (remaining_velocity / velocity) /
                                             DECELERATION_FRICTION * G_USEC_PER_SEC);
      crossing.velocity[axis] = remaining_velocity;
      origin_priv->motion_velocity[axis] = 0;
      input = &crossing;
    }

  if (!GTK_IS_SCROLLABLE (_gtk_scrolled_window_get_scrollable_child (self)) || delta == 0 ||
      (input->source == GTK_SCROLL_INPUT_TOUCHSCREEN &&
       !gtk_scrolled_window_get_kinetic_scrolling (self)))
    goto out;

  child = g_object_ref (GTK_SCROLLABLE (_gtk_scrolled_window_get_scrollable_child (self)));

  if (!scroll_route_query (GTK_WIDGET (self), GTK_SCROLL_ROUTE_NORMALIZED, &route))
    goto out;

  if (!scrollable_get_geometry (self, child, &geometry))
    goto out;

  if (_gtk_scrolled_window_get_scrollable_child (self) != GTK_WIDGET (child) || generation != priv->route_generation ||
      priv->overscroll_behavior[axis] == GTK_OVERSCROLL_BEHAVIOR_NONE ||
      !(geometry.axes & (1 << axis)))
    goto out;

  mapping = axis == 0 ? geometry.mapping_x : geometry.mapping_y;
  if (!isfinite (mapping) || mapping == 0)
    goto out;

  if (owner->child == NULL || owner->released || owner->session != origin_priv->input_session)
    {
      _gtk_scrolled_window_boundary_cancel (self, 1 << axis);

      if (_gtk_scrolled_window_get_scrollable_child (self) != GTK_WIDGET (child) || generation != priv->route_generation)
        goto out;

      *owner = (GtkScrolledWindowOverscrollOwner) {
        .child = g_object_ref (child),
        .session = origin_priv->input_session,
        .generation = generation,
        .geometry = geometry,
        .adjustment_serial = gtk_adjustment_get_change_serial (route.adjustment[axis]),
        .scale = route.units_per_input[axis] / mapping,
        .event = {
            .source = input->source,
            .sequence = scroll_overscroll_next_sequence (),
            .axes = 1 << axis,
            .local_axes = priv->overscroll_behavior[axis] != GTK_OVERSCROLL_BEHAVIOR_NONE
                              ? 1 << axis
                              : 0,
            .chain_axes = route.chain_axes,
            .generation = geometry.generation,
            .side_x = delta * route.units_per_input[axis] / mapping < 0 ? GTK_POS_LEFT : GTK_POS_RIGHT,
            .side_y = delta * route.units_per_input[axis] / mapping < 0 ? GTK_POS_TOP : GTK_POS_BOTTOM,
        },
      };

      owner->origin_signals = g_signal_group_new (GTK_TYPE_WIDGET);
      g_signal_group_connect_object (owner->origin_signals, "notify::parent",
                                     G_CALLBACK (scroll_overscroll_origin_parent_changed), self, 0);
      g_signal_group_connect_object (owner->origin_signals, "unmap",
                                     G_CALLBACK (scroll_overscroll_origin_gone), self, 0);
      g_signal_group_connect_object (owner->origin_signals, "destroy",
                                     G_CALLBACK (scroll_overscroll_origin_gone), self, 0);
      g_signal_group_set_target (owner->origin_signals, origin);

      if (!scroll_overscroll_send (self, axis, GTK_SCROLL_MOTION_BEGIN, 0, input->timestamp, &reply))
        goto out;
    }

  if (input->inertia)
    {
      guint64 sequence = owner->event.sequence;

      owner->released = TRUE;
      owner->event.velocity_x = axis == 0 ? input->velocity[axis] * owner->scale : 0;
      owner->event.velocity_y = axis == 1 ? input->velocity[axis] * owner->scale : 0;

      if (scroll_overscroll_send (self, axis, GTK_SCROLL_MOTION_RELEASE, 0, input->timestamp, &reply) &&
          owner->event.sequence == sequence)
        scroll_overscroll_send (self, axis, GTK_SCROLL_MOTION_TICK, 0, origin_priv->motion_time, &reply);
    }
  else
    {
      scroll_overscroll_send (self, axis, GTK_SCROLL_MOTION_PULL, delta, input->timestamp, &reply);
    }

  _gtk_scrolled_window_boundary_schedule (self);

out:
  g_clear_object (&child);
}

gboolean
_gtk_scrolled_window_boundary_route_input_full (GtkScrolledWindow                  *self,
                                                GtkScrollRouteUnit                  unit,
                                                double                              dx,
                                                double                              dy,
                                                GdkEvent                           *event,
                                                const GtkScrolledWindowScrollInput *input)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GArray *receivers;
  GPtrArray *holds;
  GtkScrollRouteResult result;
  GtkScrollRouteSnapshot *snapshot = NULL;
  double delta[2] = {dx, dy};
  gboolean eligible = unit == GTK_SCROLL_ROUTE_NORMALIZED && input->continuous;

  if (priv->route_depth != 0)
    return GDK_EVENT_STOP;

  if (event != NULL && priv->routed_event == event)
    return GDK_EVENT_STOP;

  receivers = g_array_new (FALSE, FALSE, sizeof (GtkScrollRouteReceiver));
  holds = g_ptr_array_new_with_free_func (g_object_unref);

  for (GtkWidget *widget = GTK_WIDGET (self); widget; widget = scroll_route_get_parent (widget))
    {
      GtkScrollRouteReceiver receiver;
      GtkScrolledWindowBoundary *receiver_priv;

      if (!GTK_IS_SCROLLED_WINDOW (widget))
        continue;

      receiver_priv = _gtk_scrolled_window_get_boundary (GTK_SCROLLED_WINDOW (widget));
      if (receiver_priv->route_depth != 0)
        break;

      receiver = (GtkScrollRouteReceiver) {widget, scroll_route_query};
      g_array_append_val (receivers, receiver);
      g_ptr_array_add (holds, g_object_ref (widget));
      g_clear_pointer (&receiver_priv->routed_event, gdk_event_unref);

      if (event != NULL)
        receiver_priv->routed_event = gdk_event_ref (event);

      if (!input->inertia)
        {
          receiver_priv->motion_velocity[0] = receiver_priv->motion_velocity[1] = 0;
          _gtk_scrolled_window_clear_legacy_scrolling (GTK_SCROLLED_WINDOW (widget), TRUE);
        }
      else
        {
          _gtk_scrolled_window_clear_legacy_scrolling (GTK_SCROLLED_WINDOW (widget), FALSE);
        }
    }

  for (guint i = 0; i < holds->len; i++)
    {
      GtkScrolledWindowBoundary *receiver_priv = _gtk_scrolled_window_get_boundary (holds->pdata[i]);

      receiver_priv->route_depth++;
      receiver_priv->route_origin = GTK_WIDGET (self);
    }

  /* An ancestor's latched axis must unwind before any descendant writes.
   * Unconsumed outward pressure stays with that owner for this sample.
   */
  for (guint i = 0; i < holds->len; i++)
    {
      GtkScrolledWindow *receiver = holds->pdata[i];
      GtkScrolledWindowBoundary *receiver_priv = _gtk_scrolled_window_get_boundary (receiver);

      for (guint axis = 0; axis < 2; axis++)
        {
          GtkScrolledWindowOverscrollOwner *owner = &receiver_priv->overscroll_axis[axis];
          GtkScrollMotionReply reply;
          double scale;

          if (owner->child == NULL)
            continue;

          /* A release with no movement must not cancel another session's pull. */
          if (delta[axis] == 0)
            continue;

          if (input->inertia && owner->released)
            continue;

          if (eligible && !input->inertia &&
              (owner->session != input->session || owner->released) &&
              owner->event.source == input->source)
            scroll_overscroll_transfer_origin (receiver, self, axis, input);

          if (owner->child == NULL)
            continue;

          if (!scroll_overscroll_owner_is_at_edge (receiver, axis))
            {
              /* Keep the presentation steady while outward input traverses
               * newly published range. Once it reaches the edge, any
               * remainder resumes pulling the same owner.
               */
              if (eligible && !input->inertia && !owner->released &&
                  owner->session == input->session)
                {
                  if (delta[axis] == 0 ||
                      scroll_overscroll_owner_delta_is_outward (owner, axis, delta[axis]))
                    continue;
                }
              else
                {
                  _gtk_scrolled_window_boundary_cancel (receiver, 1 << axis);
                  continue;
                }
            }

          if (!eligible || owner->session != input->session || owner->released)
            {
              _gtk_scrolled_window_boundary_cancel (receiver, 1 << axis);
              continue;
            }

          scale = owner->scale;

          if (!scroll_overscroll_send (receiver, axis, GTK_SCROLL_MOTION_UNWIND, delta[axis], input->timestamp, &reply))
            {
              delta[axis] = 0;
              continue;
            }

          delta[axis] -= (axis == 0 ? reply.consumed_x : reply.consumed_y) / scale;

          if (owner->motion.active)
            {
              scroll_overscroll_send (receiver, axis, GTK_SCROLL_MOTION_PULL, delta[axis], input->timestamp, &reply);
              delta[axis] = 0;
            }
          else
            {
              _gtk_scrolled_window_boundary_cancel (receiver, 1 << axis);
            }
        }
    }

  _gtk_scroll_route_dispatch ((GtkScrollRouteReceiver *)receivers->data, receivers->len,
                              unit, delta, &result, &snapshot);

  if (result.status == GTK_SCROLL_ROUTE_COMPLETE && unit == GTK_SCROLL_ROUTE_NORMALIZED)
    {
      g_clear_pointer (&priv->motion_snapshot, _gtk_scroll_route_snapshot_unref);
      priv->motion_snapshot = g_steal_pointer (&snapshot);
    }
  else
    {
      g_clear_pointer (&priv->motion_snapshot, _gtk_scroll_route_snapshot_unref);
    }

  if (result.status == GTK_SCROLL_ROUTE_COMPLETE)
    {
      for (guint axis = 0; axis < 2; axis++)
        {
          if (fabs (result.remaining[axis]) > 1e-9 * MAX (1, fabs (delta[axis])) &&
              result.terminal[axis] < holds->len)
            {
              if (eligible)
                scroll_overscroll_pull (holds->pdata[result.terminal[axis]], self, axis, result.remaining[axis], input);
              else if (input->inertia)
                priv->motion_velocity[axis] = 0;
            }
        }
    }

  if (result.status != GTK_SCROLL_ROUTE_COMPLETE)
    _gtk_scrolled_window_boundary_release (self, GTK_SCROLL_MOTION_CANCEL);

  for (guint i = 0; i < holds->len; i++)
    {
      GtkScrolledWindowBoundary *receiver_priv = _gtk_scrolled_window_get_boundary (holds->pdata[i]);

      receiver_priv->route_depth--;
      receiver_priv->route_origin = NULL;
    }

  /* Even invalidation may follow a successful write. Never replay this raw
   * vector through ancestor controllers after attempting internal dispatch.
   */
  g_ptr_array_unref (holds);
  g_array_unref (receivers);
  g_clear_pointer (&snapshot, _gtk_scroll_route_snapshot_unref);

  return GDK_EVENT_STOP;
}

gboolean
_gtk_scrolled_window_boundary_route_controller (GtkScrolledWindow        *self,
                                                GtkEventControllerScroll *scroll,
                                                double                    dx,
                                                double                    dy,
                                                gboolean                  continuous)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowScrollInput input = priv->input;
  GtkScrollRouteUnit unit;

  input.timestamp = g_get_monotonic_time ();
  input.continuous = continuous;

  if (gtk_event_controller_get_current_event_state (GTK_EVENT_CONTROLLER (scroll)) & GDK_SHIFT_MASK)
    {
      double tmp = dx;

      dx = dy;
      dy = tmp;
    }

  if (gtk_event_controller_scroll_get_unit (scroll) == GDK_SCROLL_UNIT_WHEEL)
    {
      unit = GTK_SCROLL_ROUTE_WHEEL;
    }
  else
    {
      unit = GTK_SCROLL_ROUTE_NORMALIZED;
      dx *= MAGIC_SCROLL_FACTOR;
      dy *= MAGIC_SCROLL_FACTOR;
    }

  input.unit = unit;
  priv->input.unit = unit;

  if (unit == GTK_SCROLL_ROUTE_WHEEL)
    priv->input.continuous = FALSE;

  return _gtk_scrolled_window_boundary_route_input_full (self, unit, dx, dy,
                                                         gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (scroll)),
                                                         &input);
}

static void
scroll_input_begin (GtkScrolledWindow *self,
                    GtkScrollInputSource source)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  static guint64 next_session;
  guint64 previous_session = priv->input_session;
  GPtrArray *chain = g_ptr_array_new_with_free_func (g_object_unref);

  priv->motion_velocity[0] = priv->motion_velocity[1] = 0;
  g_clear_pointer (&priv->motion_snapshot, _gtk_scroll_route_snapshot_unref);

  if (priv->route_depth != 0)
    priv->route_generation++;

  priv->input_session = ++next_session;
  priv->input = (GtkScrolledWindowScrollInput) {
    .source = source,
    .session = priv->input_session,
    .timestamp = g_get_monotonic_time (),
    .unit = source == GTK_SCROLL_INPUT_WHEEL ? GTK_SCROLL_ROUTE_WHEEL : GTK_SCROLL_ROUTE_NORMALIZED,
    .continuous = source == GTK_SCROLL_INPUT_TOUCHPAD || source == GTK_SCROLL_INPUT_TOUCHSCREEN,
  };
  priv->release_pending = FALSE;
  priv->touch_release_pending = FALSE;

  for (GtkWidget *widget = GTK_WIDGET (self); widget; widget = scroll_route_get_parent (widget))
    {
      if (GTK_IS_SCROLLED_WINDOW (widget))
        g_ptr_array_add (chain, g_object_ref (widget));
    }

  for (guint i = 0; i < chain->len; i++)
    {
      GtkScrolledWindow *receiver = chain->pdata[i];
      GtkScrolledWindowBoundary *receiver_priv = _gtk_scrolled_window_get_boundary (receiver);

      for (guint axis = 0; axis < 2; axis++)
        {
          GtkScrolledWindowOverscrollOwner *owner = &receiver_priv->overscroll_axis[axis];

          if (owner->child != NULL && owner->session == previous_session &&
              (!owner->released || owner->event.source != source))
            _gtk_scrolled_window_boundary_cancel (receiver, 1 << axis);
        }
    }

  g_ptr_array_unref (chain);
}

static void
scroll_input_release (GtkScrolledWindow *self,
                      double             vx,
                      double             vy,
                      gint64             timestamp)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  guint64 session = priv->input_session;

  g_object_ref (self);

  if (priv->motion_snapshot == NULL && priv->input.unit == GTK_SCROLL_ROUTE_NORMALIZED)
    {
      GtkScrolledWindowScrollInput input = priv->input;

      input.timestamp = timestamp;

      _gtk_scrolled_window_boundary_route_input_full (self, input.unit, 0, 0, NULL, &input);

      if (session != priv->input_session)
        goto cleanup;
    }

  priv->input.velocity[0] = isfinite (vx) ? vx : 0;
  priv->input.velocity[1] = isfinite (vy) ? vy : 0;
  priv->input.timestamp = timestamp;
  priv->motion_velocity[0] = priv->input.velocity[0];
  priv->motion_velocity[1] = priv->input.velocity[1];
  priv->motion_time = timestamp;
  priv->motion_session = priv->input_session;

  _gtk_scrolled_window_boundary_release (self, GTK_SCROLL_MOTION_RELEASE);

  if (session != priv->input_session)
    goto cleanup;

  priv->release_pending = FALSE;
  priv->touch_release_pending = FALSE;
  priv->touch_claimed = FALSE;

  if (priv->motion_snapshot == NULL)
    priv->motion_velocity[0] = priv->motion_velocity[1] = 0;

  _gtk_scrolled_window_boundary_schedule (self);

cleanup:
  g_object_unref (self);
}

void
_gtk_scrolled_window_boundary_begin_input (GtkScrolledWindow    *self,
                                           GtkScrollInputSource  source,
                                           gint64                timestamp)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);

  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (self));

  scroll_input_begin (self, source);

  priv->input.timestamp = timestamp;
}

void
_gtk_scrolled_window_begin_input (GtkScrolledWindow    *self,
                                  GtkScrollInputSource  source,
                                  gint64                timestamp)
{
  _gtk_scrolled_window_boundary_begin_input (self, source, timestamp);
}

void
_gtk_scrolled_window_boundary_update_input (GtkScrolledWindow *self,
                                            double             dx,
                                            double             dy,
                                            gint64             timestamp)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowScrollInput input;

  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (self));

  input = priv->input;
  input.timestamp = timestamp;

  _gtk_scrolled_window_boundary_route_input_full (self, input.unit, dx, dy, NULL, &input);
}

void
_gtk_scrolled_window_update_input (GtkScrolledWindow *self,
                                   double             dx,
                                   double             dy,
                                   gint64             timestamp)
{
  _gtk_scrolled_window_boundary_update_input (self, dx, dy, timestamp);
}

void
_gtk_scrolled_window_boundary_input_release (GtkScrolledWindow *self,
                                             double             vx,
                                             double             vy,
                                             gint64             timestamp)
{
  g_return_if_fail (GTK_IS_SCROLLED_WINDOW (self));

  scroll_input_release (self, vx, vy, timestamp);
}

void
_gtk_scrolled_window_release_input (GtkScrolledWindow *self,
                                    double             vx,
                                    double             vy,
                                    gint64             timestamp)
{
  _gtk_scrolled_window_boundary_input_release (self, vx, vy, timestamp);
}

gboolean
_gtk_scrolled_window_get_overscroll (GtkScrolledWindow *self,
                                     GtkScrollable     *scrollable,
                                     double            *offset_x,
                                     double            *offset_y)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowScrollableGeometry geometry;
  double x = 0;
  double y = 0;

  g_return_val_if_fail (GTK_IS_SCROLLED_WINDOW (self), FALSE);
  g_return_val_if_fail (GTK_IS_SCROLLABLE (scrollable), FALSE);

  if (_gtk_scrolled_window_get_scrollable_child (self) == GTK_WIDGET (scrollable) &&
      scrollable_get_geometry (self, scrollable, &geometry))
    {
      for (guint axis = 0; axis < 2; axis++)
        {
          GtkScrolledWindowOverscrollOwner *motion = &priv->overscroll_axis[axis];
          GtkAdjustment *adjustment = axis == 0
                                    ? gtk_scrollable_get_hadjustment (scrollable)
                                    : gtk_scrollable_get_vadjustment (scrollable);

          if (motion->motion.active && (geometry.axes & (1 << axis)) &&
              motion->child == scrollable && adjustment != NULL &&
              motion->generation == priv->route_generation &&
              motion->adjustment_serial == gtk_adjustment_get_change_serial (adjustment))
            {
              if (axis == 0)
                x = motion->motion.offset;
              else
                y = motion->motion.offset;
            }
        }
    }

  if (offset_x != NULL)
    *offset_x = x;

  if (offset_y != NULL)
    *offset_y = y;

  return x != 0 || y != 0;
}

static void
scroll_motion_reconcile_adjustment (GtkScrolledWindow *self,
                                    GtkAdjustment     *adjustment)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  guint64 serial = gtk_adjustment_get_change_serial (adjustment);

  if (priv->overscroll_axis[0].child != NULL &&
      _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL) == adjustment)
    priv->overscroll_axis[0].adjustment_serial = serial;

  if (priv->overscroll_axis[1].child != NULL &&
      _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL) == adjustment)
    priv->overscroll_axis[1].adjustment_serial = serial;

  if (priv->motion_snapshot != NULL)
    _gtk_scroll_route_snapshot_reconcile_adjustment (priv->motion_snapshot, adjustment);
}

static gboolean
scroll_overscroll_adopt_adjustment_change (GtkScrolledWindow *self,
                                           GtkAdjustment     *adjustment)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  GtkScrolledWindowOverscrollOwner *owner;
  GtkScrolledWindowScrollableGeometry geometry;
  GtkScrollable *child;
  guint axis;

  if (adjustment == _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL))
    axis = GTK_ORIENTATION_HORIZONTAL;
  else if (adjustment == _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_VERTICAL))
    axis = GTK_ORIENTATION_VERTICAL;
  else
    return FALSE;

  owner = &priv->overscroll_axis[axis];

  if (owner->child == NULL ||
      _gtk_scrolled_window_get_scrollable_child (self) == NULL ||
      owner->generation != priv->route_generation ||
      owner->event.generation != priv->route_generation ||
      !GTK_IS_SCROLLABLE (_gtk_scrolled_window_get_scrollable_child (self)) ||
      owner->child != GTK_SCROLLABLE (_gtk_scrolled_window_get_scrollable_child (self)))
    return FALSE;

  child = owner->child;

  if ((axis == GTK_ORIENTATION_HORIZONTAL &&
       gtk_scrollable_get_hadjustment (child) != adjustment) ||
      (axis == GTK_ORIENTATION_VERTICAL &&
       gtk_scrollable_get_vadjustment (child) != adjustment))
    return FALSE;

  if (!scrollable_get_geometry (self, child, &geometry) ||
      _gtk_scrolled_window_get_scrollable_child (self) != GTK_WIDGET (child) ||
      owner->child != child ||
      geometry.generation != owner->event.generation ||
      priv->overscroll_behavior[axis] == GTK_OVERSCROLL_BEHAVIOR_NONE ||
      !scroll_overscroll_geometry_matches (&geometry, &owner->geometry, axis) ||
      !(geometry.axes & (1 << axis)))
    return FALSE;

  owner->adjustment_serial = gtk_adjustment_get_change_serial (adjustment);

  if (priv->motion_snapshot != NULL)
    _gtk_scroll_route_snapshot_reconcile_adjustment (priv->motion_snapshot, adjustment);

  return TRUE;
}

gboolean
_gtk_scrolled_window_boundary_changed_adjustment (GtkScrolledWindow *self,
                                                  GtkAdjustment     *adjustment,
                                                  gboolean          *adopted)
{
  GtkScrolledWindowBoundary *priv = _gtk_scrolled_window_get_boundary (self);
  gboolean internal_adjustment_update;
  gboolean adopted_adjustment_update = FALSE;

  g_return_val_if_fail (GTK_IS_SCROLLED_WINDOW (self), FALSE);
  g_return_val_if_fail (GTK_IS_ADJUSTMENT (adjustment), FALSE);

  internal_adjustment_update = priv->route_depth > 0 || priv->allocating_child;

  if (internal_adjustment_update)
    {
      scroll_motion_reconcile_adjustment (self, adjustment);
    }
  else if (!(adopted_adjustment_update = scroll_overscroll_adopt_adjustment_change (self, adjustment) ||
             scroll_motion_adopt_adjustment_change (self, adjustment)))
    {
      guint axis = adjustment == _gtk_scrolled_window_get_adjustment (self, GTK_ORIENTATION_HORIZONTAL) ? 0 : 1;

      _gtk_scrolled_window_boundary_cancel (self, 1 << axis);
    }

  if (adopted != NULL)
    *adopted = adopted_adjustment_update;

  return internal_adjustment_update;
}
