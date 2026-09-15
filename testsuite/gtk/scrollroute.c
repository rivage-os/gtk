/* scrollroute.c
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

#include <math.h>

#include <gtk/gtk.h>

#include "gtk/gtkscrollboundariesprivate.h"

typedef struct
{
  GtkWidget *widget;
  guint      axes;
  guint      chain_axes;
  double     scale[2];
  double     wheel_step[2];
  guint64    generation;
  gboolean  available;
} Receiver;

static GtkAdjustment *
get_adjustment (Receiver *receiver,
                guint axis)
{
  if (axis == 0)
    return gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (receiver->widget));

  return gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (receiver->widget));
}

static gboolean
query_receiver (GtkWidget              *widget,
                GtkScrollRouteUnit      unit,
                GtkScrollRouteGeometry *geometry)
{
  Receiver *receiver = g_object_get_data (G_OBJECT (widget), "receiver");

  if (!receiver->available)
    return FALSE;

  geometry->axes = receiver->axes;
  geometry->chain_axes = receiver->chain_axes;
  geometry->generation = receiver->generation;

  for (guint axis = 0; axis < 2; axis++)
    {
      geometry->adjustment[axis] = get_adjustment (receiver, axis);
      geometry->units_per_input[axis] = unit == GTK_SCROLL_ROUTE_WHEEL
                                      ? receiver->wheel_step[axis]
                                      : receiver->scale[axis];
    }

  return TRUE;
}

static void
receiver_init (Receiver *receiver)
{
  *receiver = (Receiver) {
    .widget = g_object_ref_sink (gtk_scrolled_window_new ()),
    .axes = GTK_SCROLL_ROUTE_BOTH,
    .chain_axes = GTK_SCROLL_ROUTE_BOTH,
    .scale = { 1, 1 },
    .wheel_step = { 10, 10 },
    .available = TRUE,
  };

  g_object_set_data (G_OBJECT (receiver->widget), "receiver", receiver);
  for (guint axis = 0; axis < 2; axis++)
    gtk_adjustment_configure (get_adjustment (receiver, axis), 0, 0, 100, 1, 10, 10);
}

static void
receiver_clear (Receiver *receiver)
{
  g_clear_object (&receiver->widget);
}

static void
assert_close (double actual,
              double expected)
{
  g_assert_cmpfloat_with_epsilon (actual, expected, 1e-9 * MAX (1, fabs (expected)));
}

static void
assert_conservation (const double input[2],
                     const GtkScrollRouteResult *result)
{
  for (guint axis = 0; axis < 2; axis++)
    {
      assert_close (result->consumed[axis] + result->remaining[axis], input[axis]);
      g_assert_cmpfloat (result->consumed[axis], >=, MIN (0, input[axis]));
      g_assert_cmpfloat (result->consumed[axis], <=, MAX (0, input[axis]));
    }
}

static void
test_consumption (void)
{
  const struct
  {
    double value, lower, upper, page, scale, delta;
    double next, consumed, remaining;
  } cases[] = {
    { 0, 0, 100, 10, 1, 0, 0, 0, 0 },
    { 0, 0, 100, 10, 1, -5, 0, 0, -5 },
    { 90, 0, 100, 10, 1, 5, 90, 0, 5 },
    { 85, 0, 100, 10, 1, 12, 90, 5, 7 },
    { 5, 0, 100, 10, 1, -12, 0, -5, -7 },
    { 12, 10, 100, 10, 2, -2, 10, -1, -1 },
    { 12, 10, 100, 10, -2, 2, 10, 1, 1 },
    { 10, 10, 10, 10, 1, 4, 10, 0, 4 },
    { 10, 10, 12, 20, 1, -4, 10, 0, -4 },
    { 10, 10, 9, 0, 1, 4, 10, 0, 4 },
    { 0, 0, 0.5, 0.25, 0.5, 1, 0.25, 0.5, 0.5 },
    { 0, 0, 100, 10, 1, 0.125, 0.125, 0.125, 0 },
    { 262615368.2578066, 0, 322043280, 619, 1, 0.058086696459313435, 262615368.3158933, 0.058086696459313435, 0 },
    { 0, 0, 100, 10, 2, G_MAXDOUBLE, 90, 45, G_MAXDOUBLE },
  };

  for (guint i = 0; i < G_N_ELEMENTS (cases); i++)
    {
      GtkScrollRouteConsumption result;

      g_assert_true (_gtk_scroll_route_consume (cases[i].value, cases[i].lower, cases[i].upper,
                                                cases[i].page, cases[i].scale, cases[i].delta,
                                                &result));
      assert_close (result.value, cases[i].next);
      assert_close (result.consumed, cases[i].consumed);

      if (cases[i].remaining == 0)
        g_assert_cmpfloat (result.remaining, ==, 0);
      else
        assert_close (result.remaining, cases[i].remaining);

      assert_close (result.consumed + result.remaining, cases[i].delta);
    }
}

static void
test_invalid_consumption (void)
{
  const double invalid[][6] = {
    { NAN, 0, 100, 10, 1, 1 },
    { 0, NAN, 100, 10, 1, 1 },
    { 0, 0, INFINITY, 10, 1, 1 },
    { 0, 0, 100, NAN, 1, 1 },
    { 0, 0, 100, -1, 1, 1 },
    { 0, 0, 100, 10, 0, 1 },
    { 0, 0, 100, 10, INFINITY, 1 },
    { 0, 0, 100, 10, 1, NAN },
    { 0, 0, 100, 10, 1, INFINITY },
    { -1, 0, 100, 10, 1, 1 },
    { 91, 0, 100, 10, 1, 1 },
    { 0, -G_MAXDOUBLE, G_MAXDOUBLE, 0, 1, 1 },
  };

  for (guint i = 0; i < G_N_ELEMENTS (invalid); i++)
    {
      GtkScrollRouteConsumption result;

      g_assert_false (_gtk_scroll_route_consume (invalid[i][0], invalid[i][1], invalid[i][2],
                                                 invalid[i][3], invalid[i][4], invalid[i][5],
                                                 &result));
      g_assert_cmpfloat (result.consumed, ==, 0);
    }
}

static void
test_split_axes (void)
{
  Receiver child, parent;
  GtkScrollRouteReceiver chain[2];
  GtkScrollRouteResult result;
  const double input[] = { 12, -20 };

  receiver_init (&child);
  receiver_init (&parent);

  gtk_adjustment_set_value (get_adjustment (&child, 0), 85);
  gtk_adjustment_set_value (get_adjustment (&child, 1), 10);
  gtk_adjustment_set_value (get_adjustment (&parent, 1), 50);

  parent.scale[0] = 2;
  parent.scale[1] = -2;
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };

  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);

  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  assert_close (gtk_adjustment_get_value (get_adjustment (&child, 0)), 90);
  assert_close (gtk_adjustment_get_value (get_adjustment (&child, 1)), 0);
  assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 0)), 14);
  assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 1)), 70);
  assert_close (result.remaining[0], 0);
  assert_close (result.remaining[1], 0);
  assert_conservation (input, &result);

  receiver_clear (&child);
  receiver_clear (&parent);
}

static void
test_barriers (void)
{
  for (guint axes = 0; axes <= GTK_SCROLL_ROUTE_BOTH; axes++)
    {
      for (guint chain_axes = 0; chain_axes <= GTK_SCROLL_ROUTE_BOTH; chain_axes++)
        {
          Receiver child, parent;
          GtkScrollRouteReceiver chain[2];
          GtkScrollRouteResult result;
          const double input[] = { 12, 20 };

          receiver_init (&child);
          receiver_init (&parent);

          child.axes = axes;
          child.chain_axes = chain_axes;

          /* Empty participating axes can still form barriers. */
          for (guint axis = 0; axis < 2; axis++)
            gtk_adjustment_configure (get_adjustment (&child, axis), 0, 0, 10, 1, 10, 10);

          chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
          chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };

          _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);

          g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
          g_assert_cmpuint (result.stopped_axes, ==, axes & ~chain_axes);

          for (guint axis = 0; axis < 2; axis++)
            {
              gboolean stopped = (result.stopped_axes & (1 << axis)) != 0;

              assert_close (gtk_adjustment_get_value (get_adjustment (&parent, axis)),
                            stopped ? 0 : input[axis]);
              assert_close (result.remaining[axis], stopped ? input[axis] : 0);
            }

          assert_conservation (input, &result);

          receiver_clear (&child);
          receiver_clear (&parent);
        }
    }
}

static void
test_ancestor_barrier (void)
{
  Receiver child, parent, outer;
  GtkScrollRouteReceiver chain[3];
  GtkScrollRouteResult result;
  const double input[] = { 12, 20 };

  receiver_init (&child);
  receiver_init (&parent);
  receiver_init (&outer);

  child.axes = GTK_SCROLL_ROUTE_X;
  parent.chain_axes = GTK_SCROLL_ROUTE_X;
  gtk_adjustment_set_value (get_adjustment (&child, 0), 85);
  gtk_adjustment_set_value (get_adjustment (&parent, 0), 90);
  gtk_adjustment_set_value (get_adjustment (&parent, 1), 85);
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };
  chain[2] = (GtkScrollRouteReceiver){ outer.widget, query_receiver };

  _gtk_scroll_route_dispatch (chain, 3, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);

  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  g_assert_cmpuint (result.stopped_axes, ==, GTK_SCROLL_ROUTE_Y);
  assert_close (result.remaining[0], 0);
  assert_close (result.remaining[1], 15);
  assert_close (gtk_adjustment_get_value (get_adjustment (&outer, 0)), 7);
  assert_close (gtk_adjustment_get_value (get_adjustment (&outer, 1)), 0);
  assert_conservation (input, &result);

  receiver_clear (&child);
  receiver_clear (&parent);
  receiver_clear (&outer);
}

static void
test_wheel_and_duplicates (void)
{
  Receiver child, shared, parent;
  GtkScrollRouteReceiver chain[4];
  GtkScrollRouteResult result;
  const double input[] = { 1, 0 };

  receiver_init (&child);
  receiver_init (&shared);
  receiver_init (&parent);
  gtk_adjustment_set_value (get_adjustment (&child, 0), 85);
  gtk_scrolled_window_set_hadjustment (GTK_SCROLLED_WINDOW (shared.widget),
                                       get_adjustment (&child, 0));

  /* A reversed mapping would consume again if the shared adjustment were revisited. */
  shared.wheel_step[0] = -100;
  parent.wheel_step[0] = 20;
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = chain[0];
  chain[2] = (GtkScrollRouteReceiver){ shared.widget, query_receiver };
  chain[3] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };

  _gtk_scroll_route_dispatch (chain, 4, GTK_SCROLL_ROUTE_WHEEL, input, &result, NULL);

  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  assert_close (gtk_adjustment_get_value (get_adjustment (&child, 0)), 90);
  assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 0)), 10);
  assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 1)), 0);
  assert_conservation (input, &result);

  /* A distinct receiver sharing the adjustment must still enforce its barrier. */
  gtk_adjustment_set_value (get_adjustment (&parent, 0), 0);
  shared.chain_axes = 0;
  _gtk_scroll_route_dispatch (chain, 4, GTK_SCROLL_ROUTE_WHEEL, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  g_assert_cmpuint (result.stopped_axes, ==, GTK_SCROLL_ROUTE_X);
  assert_close (result.remaining[0], 1);
  assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 0)), 0);

  receiver_clear (&child);
  receiver_clear (&shared);
  receiver_clear (&parent);
}

static void
test_dispatch_lifetime (void)
{
  GtkWidget *box;
  Receiver child, parent;
  GtkScrollRouteReceiver chain[3];
  GtkScrollRouteResult result;
  GtkWidget *child_weak;
  GtkWidget *parent_weak;
  GtkAdjustment *adjustment_weak;
  const double input[] = { 45, 0 };

  receiver_init (&child);
  receiver_init (&parent);
  box = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  child_weak = child.widget;
  parent_weak = parent.widget;
  adjustment_weak = get_adjustment (&child, 0);
  g_object_add_weak_pointer (G_OBJECT (child_weak), (gpointer *) &child_weak);
  g_object_add_weak_pointer (G_OBJECT (parent_weak), (gpointer *) &parent_weak);
  g_object_add_weak_pointer (G_OBJECT (adjustment_weak), (gpointer *) &adjustment_weak);
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = chain[0];
  chain[2] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };

  for (guint i = 0; i < 3; i++)
    {
      _gtk_scroll_route_dispatch (chain, 3, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
      g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
      assert_conservation (input, &result);
    }

  assert_close (gtk_adjustment_get_value (get_adjustment (&child, 0)), 90);
  assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 0)), 45);

  /* Exercise cleanup of partially prepared chains too. No callbacks may
   * reference the completed dispatch when widgets are subsequently changed.
   */
  parent.available = FALSE;
  _gtk_scroll_route_dispatch (chain, 3, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_INVALID);
  gtk_adjustment_set_value (get_adjustment (&child, 0), 10);
  gtk_box_append (GTK_BOX (box), child.widget);
  gtk_box_remove (GTK_BOX (box), child.widget);
  receiver_clear (&child);
  receiver_clear (&parent);
  g_assert_null (child_weak);
  g_assert_null (parent_weak);
  g_assert_null (adjustment_weak);
  g_object_unref (box);
}

static void
test_terminal_and_invalid (void)
{
  Receiver child, parent;
  GtkScrollRouteReceiver chain[2];
  GtkScrollRouteResult result;
  double input[] = { 100, -10 };

  receiver_init (&child);
  receiver_init (&parent);
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };

  _gtk_scroll_route_dispatch (NULL, 0, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  assert_close (result.remaining[0], 100);

  _gtk_scroll_route_dispatch (chain, 1, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  assert_close (result.remaining[0], 10);
  assert_close (result.remaining[1], -10);
  assert_conservation (input, &result);

  gtk_adjustment_set_value (get_adjustment (&child, 0), 0);
  parent.scale[1] = 0;
  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_INVALID);
  assert_close (gtk_adjustment_get_value (get_adjustment (&child, 0)), 0);
  assert_conservation (input, &result);

  input[0] = NAN;
  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_INVALID);
  assert_close (gtk_adjustment_get_value (get_adjustment (&child, 0)), 0);

  receiver_clear (&child);
  receiver_clear (&parent);
}

static void
test_terminal_receivers (void)
{
  Receiver child, parent;
  GtkScrollRouteReceiver chain[2];
  GtkScrollRouteResult result;
  double input[] = { -20, -10 };

  receiver_init (&child);
  receiver_init (&parent);
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };
  child.chain_axes = GTK_SCROLL_ROUTE_Y;

  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  g_assert_cmpuint (result.terminal[0], ==, 0);
  g_assert_cmpuint (result.terminal[1], ==, 1);
  assert_conservation (input, &result);

  parent.axes = GTK_SCROLL_ROUTE_X;
  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  g_assert_cmpuint (result.terminal[1], ==, 0);

  child.axes = GTK_SCROLL_ROUTE_X;
  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  g_assert_cmpuint (result.terminal[1], ==, G_MAXSIZE);

  input[0] = 0;
  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);
  g_assert_cmpuint (result.terminal[0], ==, G_MAXSIZE);
  receiver_clear (&child);
  receiver_clear (&parent);
}

static void
test_snapshot_validation (void)
{
  Receiver child, parent;
  GtkScrollRouteReceiver chain[2];
  GtkScrollRouteResult result;
  GtkScrollRouteSnapshot *snapshot = NULL;
  GtkWidget *receiver;
  GtkWidget *parent_widget;
  const GtkScrollRouteGeometry *geometry;
  const double input[] = { 20, 20 };
  gboolean had_parent;

  receiver_init (&child);
  receiver_init (&parent);
  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };

  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, &snapshot);

  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_COMPLETE);
  g_assert_nonnull (snapshot);
  g_assert_cmpuint (_gtk_scroll_route_snapshot_get_n_receivers (snapshot), ==, 2);
  g_assert_cmpuint (_gtk_scroll_route_snapshot_validate (snapshot, GTK_SCROLL_ROUTE_NORMALIZED),
                    ==, GTK_SCROLL_ROUTE_BOTH);

  receiver = _gtk_scroll_route_snapshot_get_receiver (snapshot, 0);
  g_assert_true (receiver == child.widget);

  parent_widget = _gtk_scroll_route_snapshot_get_parent (snapshot, 0, &had_parent);
  g_assert_false (had_parent);
  g_assert_null (parent_widget);

  geometry = _gtk_scroll_route_snapshot_get_geometry (snapshot, 0);
  g_assert_true (geometry->adjustment[0] == get_adjustment (&child, 0));
  g_assert_true (geometry->adjustment[1] == get_adjustment (&child, 1));

  gtk_adjustment_set_value (get_adjustment (&child, 0), 10);
  g_assert_cmpuint (_gtk_scroll_route_snapshot_validate (snapshot, GTK_SCROLL_ROUTE_NORMALIZED),
                    ==, GTK_SCROLL_ROUTE_Y);

  _gtk_scroll_route_snapshot_reconcile_adjustment (snapshot, get_adjustment (&child, 0));
  g_assert_cmpuint (_gtk_scroll_route_snapshot_validate (snapshot, GTK_SCROLL_ROUTE_NORMALIZED),
                    ==, GTK_SCROLL_ROUTE_BOTH);

  child.generation++;
  g_assert_cmpuint (_gtk_scroll_route_snapshot_validate (snapshot, GTK_SCROLL_ROUTE_NORMALIZED),
                    ==, 0);

  _gtk_scroll_route_snapshot_unref (snapshot);
  receiver_clear (&child);
  receiver_clear (&parent);
}

typedef enum
{
  CHANGE_RANGE,
  CHANGE_RANGE_FROZEN,
  CHANGE_RANGE_RESTORED,
  CHANGE_VALUE,
  CHANGE_VALUE_RESTORED,
  CHANGE_OTHER_AXIS,
  CHANGE_REPLACE,
  CHANGE_CHILD,
  CHANGE_PARENT,
  CHANGE_DISPOSE,
  CHANGE_GENERATION,
  CHANGE_REMOVE_ANCESTOR,
} Change;

typedef struct
{
  Receiver *child;
  Receiver *parent;
  GtkWidget *box;
  Change change;
  gboolean changed;
} Mutation;

static void
mutate_during_dispatch (GtkAdjustment *adjustment,
                        Mutation *mutation)
{
  if (mutation->changed)
    return;

  mutation->changed = TRUE;

  switch (mutation->change)
    {
    case CHANGE_RANGE:
      gtk_adjustment_set_upper (adjustment, 200);
      break;

    case CHANGE_RANGE_FROZEN:
      g_object_freeze_notify (G_OBJECT (adjustment));
      gtk_adjustment_set_upper (adjustment, 200);
      break;

    case CHANGE_RANGE_RESTORED:
      g_object_freeze_notify (G_OBJECT (adjustment));
      gtk_adjustment_set_upper (adjustment, 200);
      gtk_adjustment_set_upper (adjustment, 100);
      break;

    case CHANGE_VALUE:
      gtk_adjustment_set_value (adjustment, 20);
      break;

    case CHANGE_VALUE_RESTORED:
      g_object_freeze_notify (G_OBJECT (adjustment));
      gtk_adjustment_set_value (adjustment, 20);
      gtk_adjustment_set_value (adjustment, 90);
      break;

    case CHANGE_OTHER_AXIS:
      gtk_adjustment_set_value (get_adjustment (mutation->parent, 1), 30);
      break;

    case CHANGE_REPLACE:
      gtk_scrolled_window_set_hadjustment (GTK_SCROLLED_WINDOW (mutation->child->widget), NULL);
      break;

    case CHANGE_CHILD:
      gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (mutation->child->widget),
                                     gtk_drawing_area_new ());
      mutation->child->generation++;
      break;

    case CHANGE_PARENT:
      gtk_box_append (GTK_BOX (mutation->box), mutation->child->widget);
      break;

    case CHANGE_DISPOSE:
      g_object_run_dispose (G_OBJECT (mutation->child->widget));
      mutation->child->available = FALSE;
      break;

    case CHANGE_GENERATION:
      mutation->child->generation++;
      break;

    case CHANGE_REMOVE_ANCESTOR:
      mutation->parent->available = FALSE;
      g_clear_object (&mutation->parent->widget);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
test_invalidation (gconstpointer data)
{
  GtkAdjustment *adjustment;
  GtkWidget *box;
  Receiver child, parent;
  GtkScrollRouteReceiver chain[2];
  GtkScrollRouteResult result;
  const double input[] = { 20, 20 };
  Mutation mutation;
  gulong signal_id;

  receiver_init (&child);
  receiver_init (&parent);

  box = g_object_ref_sink (gtk_box_new (GTK_ORIENTATION_VERTICAL, 0));
  adjustment = g_object_ref (get_adjustment (&child, 0));

  gtk_adjustment_set_value (adjustment, 85);

  chain[0] = (GtkScrollRouteReceiver){ child.widget, query_receiver };
  chain[1] = (GtkScrollRouteReceiver){ parent.widget, query_receiver };
  mutation = (Mutation){ &child, &parent, box, GPOINTER_TO_INT (data), FALSE };
  signal_id = g_signal_connect (adjustment,
                                "value-changed",
                                G_CALLBACK (mutate_during_dispatch),
                                &mutation);

  _gtk_scroll_route_dispatch (chain, 2, GTK_SCROLL_ROUTE_NORMALIZED, input, &result, NULL);

  g_assert_cmpint (result.status, ==, GTK_SCROLL_ROUTE_INVALIDATED);
  assert_close (result.consumed[0], 5);
  assert_close (result.remaining[0], 15);
  assert_close (result.consumed[1], 0);
  assert_conservation (input, &result);

  if (parent.widget != NULL)
    assert_close (gtk_adjustment_get_value (get_adjustment (&parent, 0)), 0);

  g_clear_signal_handler (&signal_id, adjustment);

  if (mutation.change == CHANGE_RANGE_FROZEN ||
      mutation.change == CHANGE_RANGE_RESTORED ||
      mutation.change == CHANGE_VALUE_RESTORED)
    g_object_thaw_notify (G_OBJECT (adjustment));

  if (mutation.change == CHANGE_PARENT)
    gtk_box_remove (GTK_BOX (box), child.widget);

  receiver_clear (&child);
  receiver_clear (&parent);
  g_object_unref (adjustment);
  g_object_unref (box);
}

int
main (int   argc,
      char *argv[])
{
  gtk_test_init (&argc, &argv, NULL);

  g_test_add_func ("/scrollroute/consume", test_consumption);
  g_test_add_func ("/scrollroute/invalid-consumption", test_invalid_consumption);
  g_test_add_func ("/scrollroute/split-axes", test_split_axes);
  g_test_add_func ("/scrollroute/barriers", test_barriers);
  g_test_add_func ("/scrollroute/ancestor-barrier", test_ancestor_barrier);
  g_test_add_func ("/scrollroute/wheel-and-duplicates", test_wheel_and_duplicates);
  g_test_add_func ("/scrollroute/dispatch-lifetime", test_dispatch_lifetime);
  g_test_add_func ("/scrollroute/terminal-receivers", test_terminal_receivers);
  g_test_add_func ("/scrollroute/terminal-and-invalid", test_terminal_and_invalid);
  g_test_add_func ("/scrollroute/snapshot-validation", test_snapshot_validation);

  g_test_add_data_func ("/scrollroute/invalidation/range",
                        GINT_TO_POINTER (CHANGE_RANGE),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/frozen-range",
                        GINT_TO_POINTER (CHANGE_RANGE_FROZEN),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/restored-range",
                        GINT_TO_POINTER (CHANGE_RANGE_RESTORED),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/restored-value",
                        GINT_TO_POINTER (CHANGE_VALUE_RESTORED),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/value",
                        GINT_TO_POINTER (CHANGE_VALUE),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/other-axis",
                        GINT_TO_POINTER (CHANGE_OTHER_AXIS),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/replace",
                        GINT_TO_POINTER (CHANGE_REPLACE),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/child",
                        GINT_TO_POINTER (CHANGE_CHILD),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/reparent",
                        GINT_TO_POINTER (CHANGE_PARENT),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/dispose",
                        GINT_TO_POINTER (CHANGE_DISPOSE),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/generation",
                        GINT_TO_POINTER (CHANGE_GENERATION),
                        test_invalidation);
  g_test_add_data_func ("/scrollroute/invalidation/remove-ancestor",
                        GINT_TO_POINTER (CHANGE_REMOVE_ANCESTOR),
                        test_invalidation);

  return g_test_run ();
}
