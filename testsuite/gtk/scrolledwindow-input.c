/* scrolledwindow-input.c
 *
 * Copyright 2026 Christian Hergert
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <gtk/gtk.h>

#include "gtk/deprecated/gtkiconview.h"
#include "gtk/deprecated/gtktreeview.h"
#include "gtk/gtkcolumnviewprivate.h"
#include "gtk/gtklistbaseprivate.h"
#include "gtk/gtkscrollboundariesprivate.h"
#include "gtk/gtkscrolledwindowprivate.h"

#include "scrollable-fixture.h"

static GtkWidget *
create_window_for_scroller (GtkScrolledWindow *scroller,
                            GtkWidget         *child)
{
  GtkWidget *window;

  window = gtk_window_new ();
  gtk_window_set_child (GTK_WINDOW (window), GTK_WIDGET (scroller));
  gtk_scrolled_window_set_policy (scroller, GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  gtk_scrolled_window_set_child (scroller, child);

  gtk_widget_realize (window);
  gtk_widget_measure (window, GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);
  gtk_widget_measure (window, GTK_ORIENTATION_VERTICAL, 200, NULL, NULL, NULL, NULL);
  gtk_widget_allocate (window, 200, 200, -1, NULL);

  return window;
}

static void
allocate_test_window (GtkWidget *window,
                      int        width,
                      int        height)
{
  gtk_widget_measure (window, GTK_ORIENTATION_HORIZONTAL, -1, NULL, NULL, NULL, NULL);
  gtk_widget_measure (window, GTK_ORIENTATION_VERTICAL, width, NULL, NULL, NULL, NULL);
  gtk_widget_allocate (window, width, height, -1, NULL);
}

static void
configure_at_end (GtkScrolledWindow *scroller)
{
  GtkAdjustment *hadj;
  GtkAdjustment *vadj;

  hadj = gtk_scrolled_window_get_hadjustment (scroller);
  vadj = gtk_scrolled_window_get_vadjustment (scroller);

  gtk_adjustment_configure (hadj, 90, 0, 100, 1, 10, 10);
  gtk_adjustment_configure (vadj, 90, 0, 100, 1, 10, 10);
}

static void
set_value_at_end (GtkAdjustment *adjustment)
{
  gtk_adjustment_set_value (adjustment,
                            gtk_adjustment_get_upper (adjustment) -
                            gtk_adjustment_get_page_size (adjustment));
}

static void
setup_list_item (GtkSignalListItemFactory *factory,
                 GtkListItem              *list_item)
{
  gtk_list_item_set_child (list_item, gtk_label_new (NULL));
}

static void
bind_list_item (GtkSignalListItemFactory *factory,
                GtkListItem              *list_item)
{
  GtkStringObject *item;
  GtkWidget *label;

  item = gtk_list_item_get_item (list_item);
  label = gtk_list_item_get_child (list_item);

  gtk_label_set_label (GTK_LABEL (label), gtk_string_object_get_string (item));
}

static GtkWidget *
create_column_view (guint n_items)
{
  GtkStringList *strings;
  GtkListItemFactory *factory;
  GtkSelectionModel *selection;
  GtkColumnViewColumn *column;
  GtkWidget *view;

  strings = gtk_string_list_new (NULL);
  for (guint i = 0; i < n_items; i++)
    {
      char *text;

      text = g_strdup_printf ("Item %u", i);
      gtk_string_list_append (strings, text);
      g_free (text);
    }

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (setup_list_item), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (bind_list_item), NULL);

  selection = GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (strings)));
  view = gtk_column_view_new (selection);
  column = gtk_column_view_column_new ("Item", factory);
  gtk_column_view_column_set_fixed_width (column, 320);
  gtk_column_view_append_column (GTK_COLUMN_VIEW (view), column);
  g_object_unref (column);

  return view;
}

static GtkWidget *
create_list_view (guint n_items)
{
  GtkStringList *strings;
  GtkListItemFactory *factory;
  GtkSelectionModel *selection;
  GtkWidget *view;

  strings = gtk_string_list_new (NULL);
  for (guint i = 0; i < n_items; i++)
    {
      char *text;

      text = g_strdup_printf ("Item %u", i);
      gtk_string_list_append (strings, text);
      g_free (text);
    }

  factory = gtk_signal_list_item_factory_new ();
  g_signal_connect (factory, "setup", G_CALLBACK (setup_list_item), NULL);
  g_signal_connect (factory, "bind", G_CALLBACK (bind_list_item), NULL);

  selection = GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (strings)));
  view = gtk_list_view_new (selection, factory);

  return view;
}

static GtkWidget *
find_visible_list_child (GtkListView      *listview,
                         GtkWidget        *target,
                         graphene_point_t *point)
{
  int target_height;

  target_height = gtk_widget_get_height (target);

  for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (listview));
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      if (!gtk_widget_get_child_visible (child))
        continue;

      if (!gtk_widget_compute_point (child, target,
                                     &GRAPHENE_POINT_INIT (0, 0), point))
        continue;

      if (point->y >= 0 && point->y < target_height)
        return child;
    }

  return NULL;
}

static GtkPropagationPhase
get_scrolled_window_drag_phase (GtkScrolledWindow *scroller)
{
  GListModel *controllers;
  guint n_controllers;

  controllers = gtk_widget_observe_controllers (GTK_WIDGET (scroller));
  n_controllers = g_list_model_get_n_items (controllers);

  for (guint i = 0; i < n_controllers; i++)
    {
      GtkEventController *controller;

      controller = g_list_model_get_item (controllers, i);
      if (G_OBJECT_TYPE (controller) == GTK_TYPE_GESTURE_DRAG)
        {
          GtkPropagationPhase phase;

          phase = gtk_event_controller_get_propagation_phase (controller);
          g_object_unref (controller);
          g_object_unref (controllers);
          return phase;
        }

      g_object_unref (controller);
    }

  g_object_unref (controllers);
  g_assert_not_reached ();
}

static gboolean
pull_overscroll (GtkScrolledWindow    *scroller,
                 GtkScrollInputSource  source)
{
  GtkWidget *child;
  double x = 0;
  double y = 0;

  child = gtk_scrolled_window_get_child (scroller);

  _gtk_scrolled_window_begin_input (scroller, source, 1000000);
  _gtk_scrolled_window_update_input (scroller, 20, 30, 1010000);

  return gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y);
}

static void
test_overscroll_getter (void)
{
  GtkScrolledWindow *scroller;
  FixtureScrollable *child;
  GtkWidget *window;
  double x = 123;
  double y = 456;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, GTK_WIDGET (child));
  configure_at_end (scroller);

  g_assert_false (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y));
  g_assert_cmpfloat (x, ==, 0);
  g_assert_cmpfloat (y, ==, 0);

  g_assert_true (pull_overscroll (scroller, GTK_SCROLL_INPUT_TOUCHPAD));
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y));
  g_assert_cmpfloat (x, !=, 0);
  g_assert_cmpfloat (y, !=, 0);
  g_assert_cmpuint (child->overscroll_changes, >, 0);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_adjustment_range_expands_during_pull (void)
{
  GtkScrolledWindow *scroller;
  FixtureScrollable *child;
  GtkAdjustment *adjustment;
  GtkWidget *window;
  double before;
  double unwound;
  double x;
  double y;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, GTK_WIDGET (child));
  configure_at_end (scroller);
  adjustment = gtk_scrolled_window_get_vadjustment (scroller);

  _gtk_scrolled_window_begin_input (scroller, GTK_SCROLL_INPUT_TOUCHPAD, 1000000);
  _gtk_scrolled_window_update_input (scroller, 0, 30, 1010000);
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &before));

  gtk_adjustment_configure (adjustment,
                            gtk_adjustment_get_value (adjustment),
                            gtk_adjustment_get_lower (adjustment),
                            gtk_adjustment_get_upper (adjustment) + 10,
                            gtk_adjustment_get_step_increment (adjustment),
                            gtk_adjustment_get_page_increment (adjustment),
                            gtk_adjustment_get_page_size (adjustment));

  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y));
  g_assert_cmpfloat_with_epsilon (y, before, 0.001);

  _gtk_scrolled_window_update_input (scroller, 0, -10, 1020000);
  g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), ==, 90);
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &unwound));
  g_assert_cmpfloat (unwound, >, before);

  _gtk_scrolled_window_update_input (scroller, 0, 20, 1030000);
  g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), >, 90);
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y));
  g_assert_cmpfloat_with_epsilon (y, unwound, 0.001);

  _gtk_scrolled_window_update_input (scroller, 0, 200, 1040000);
  g_assert_cmpfloat (gtk_adjustment_get_value (adjustment), ==, 100);
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y));
  g_assert_cmpfloat (y, <, unwound);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_detached_getter (void)
{
  FixtureScrollable *child;
  double x = 123;
  double y = 456;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  g_object_ref_sink (child);

  g_assert_false (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), &x, &y));
  g_assert_cmpfloat (x, ==, 0);
  g_assert_cmpfloat (y, ==, 0);

  g_object_unref (child);
}

static void
test_policy_api (void)
{
  GtkScrolledWindow *scroller;

  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (scroller);

  gtk_scrolled_window_set_overscroll_behavior (scroller,
                                               GTK_ORIENTATION_HORIZONTAL,
                                               GTK_OVERSCROLL_BEHAVIOR_CONTAIN);
  gtk_scrolled_window_set_overscroll_behavior (scroller,
                                               GTK_ORIENTATION_VERTICAL,
                                               GTK_OVERSCROLL_BEHAVIOR_NONE);

  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_CONTAIN);
  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);

  g_object_unref (scroller);
}

static void
test_child_default_precedence (void)
{
  GtkScrolledWindow *scroller;
  FixtureScrollable *child;
  GtkWidget *text_view;

  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (scroller);
  child = g_object_new (fixture_scrollable_get_type (), NULL);
  text_view = gtk_text_view_new ();

  gtk_scrolled_window_set_overscroll_behavior (scroller,
                                               GTK_ORIENTATION_HORIZONTAL,
                                               GTK_OVERSCROLL_BEHAVIOR_CONTAIN);
  gtk_scrolled_window_set_child (scroller, GTK_WIDGET (child));

  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_CONTAIN);
  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_AUTO);

  gtk_scrolled_window_set_child (scroller, text_view);
  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_CONTAIN);
  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_AUTO);

  gtk_scrolled_window_set_child (scroller, NULL);
  g_object_unref (scroller);
}

static void
test_continuous_only_presentation (void)
{
  GtkScrolledWindow *scroller;
  FixtureScrollable *child;
  GtkWidget *window;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, GTK_WIDGET (child));
  configure_at_end (scroller);

  g_assert_false (pull_overscroll (scroller, GTK_SCROLL_INPUT_WHEEL));

  _gtk_scrolled_window_begin_input (scroller, GTK_SCROLL_INPUT_KEYBOARD, 2000000);
  _gtk_scrolled_window_update_input (scroller, 20, 30, 2010000);
  g_assert_false (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), NULL, NULL));

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_none_contains_without_presentation (void)
{
  GtkScrolledWindow *scroller;
  FixtureScrollable *child;
  GtkWidget *window;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  gtk_scrolled_window_set_overscroll_behavior (scroller,
                                               GTK_ORIENTATION_HORIZONTAL,
                                               GTK_OVERSCROLL_BEHAVIOR_NONE);
  gtk_scrolled_window_set_overscroll_behavior (scroller,
                                               GTK_ORIENTATION_VERTICAL,
                                               GTK_OVERSCROLL_BEHAVIOR_NONE);
  window = create_window_for_scroller (scroller, GTK_WIDGET (child));
  configure_at_end (scroller);

  g_assert_false (pull_overscroll (scroller, GTK_SCROLL_INPUT_TOUCHPAD));

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_empty_contained_axis_stops_parent (void)
{
  const GtkOverscrollBehavior behaviors[] = {
    GTK_OVERSCROLL_BEHAVIOR_CONTAIN,
    GTK_OVERSCROLL_BEHAVIOR_NONE,
  };

  for (guint i = 0; i < G_N_ELEMENTS (behaviors); i++)
    {
      GtkScrolledWindow *outer;
      GtkScrolledWindow *inner;
      FixtureScrollable *child;
      GtkWidget *window;
      GtkAdjustment *outer_vadjustment;

      child = g_object_new (fixture_scrollable_get_type (), NULL);
      inner = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
      outer = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());

      gtk_scrolled_window_set_policy (inner, GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
      gtk_scrolled_window_set_overscroll_behavior (inner,
                                                   GTK_ORIENTATION_VERTICAL,
                                                   behaviors[i]);
      gtk_scrolled_window_set_child (inner, GTK_WIDGET (child));
      gtk_widget_set_size_request (GTK_WIDGET (inner), 200, 400);

      window = create_window_for_scroller (outer, GTK_WIDGET (inner));
      outer_vadjustment = gtk_scrolled_window_get_vadjustment (outer);

      gtk_adjustment_configure (gtk_scrolled_window_get_vadjustment (inner),
                                0, 0, 200, 1, 10, 200);
      gtk_adjustment_configure (outer_vadjustment, 0, 0, 100, 1, 10, 10);

      _gtk_scrolled_window_begin_input (inner, GTK_SCROLL_INPUT_WHEEL, 1000000);
      _gtk_scrolled_window_update_input (inner, 0, 1, 1010000);

      g_assert_cmpfloat (gtk_adjustment_get_value (outer_vadjustment), ==, 0);

      gtk_window_destroy (GTK_WINDOW (window));
    }
}

static void
test_native_boundary_stops_routing (void)
{
  GtkScrolledWindow *outer;
  GtkScrolledWindow *inner;
  FixtureScrollable *child;
  GtkWidget *anchor;
  GtkWidget *popover;
  GtkWidget *window;
  GtkAdjustment *outer_vadjustment;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  inner = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  outer = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  anchor = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  popover = gtk_popover_new ();

  gtk_scrolled_window_set_overscroll_behavior (outer,
                                               GTK_ORIENTATION_VERTICAL,
                                               GTK_OVERSCROLL_BEHAVIOR_CONTAIN);
  gtk_scrolled_window_set_policy (inner, GTK_POLICY_EXTERNAL, GTK_POLICY_EXTERNAL);
  gtk_popover_set_child (GTK_POPOVER (popover), GTK_WIDGET (inner));
  gtk_widget_set_parent (popover, anchor);

  g_assert_false (_gtk_scrolled_window_boundary_chain_uses_routing (GTK_WIDGET (inner)));

  gtk_scrolled_window_set_child (inner, GTK_WIDGET (child));
  window = create_window_for_scroller (outer, anchor);
  gtk_popover_popup (GTK_POPOVER (popover));
  allocate_test_window (window, 200, 200);

  outer_vadjustment = gtk_scrolled_window_get_vadjustment (outer);
  gtk_adjustment_configure (outer_vadjustment, 20, 0, 100, 1, 10, 10);
  configure_at_end (inner);

  _gtk_scrolled_window_begin_input (inner, GTK_SCROLL_INPUT_WHEEL, 1000000);
  _gtk_scrolled_window_update_input (inner, 0, 1, 1010000);

  g_assert_cmpfloat (gtk_adjustment_get_value (outer_vadjustment), ==, 20);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_release_returns_to_rest (void)
{
  GtkScrolledWindow *scroller;
  FixtureScrollable *child;
  GtkWidget *window;
  gint64 timestamp = 1100000;

  child = g_object_new (fixture_scrollable_get_type (), NULL);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, GTK_WIDGET (child));
  configure_at_end (scroller);

  g_assert_true (pull_overscroll (scroller, GTK_SCROLL_INPUT_TOUCHPAD));
  _gtk_scrolled_window_release_input (scroller, 0, 0, timestamp);

  while (_gtk_scrolled_window_advance_overscroll (scroller, timestamp))
    timestamp += 100000;

  g_assert_false (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (child), NULL, NULL));

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_viewport_wrapper (void)
{
  GtkScrolledWindow *scroller;
  GtkWidget *content;
  GtkWidget *viewport;
  GtkWidget *window;
  graphene_point_t before;
  graphene_point_t after;
  double x = 0;
  double y = 0;

  content = gtk_button_new ();
  gtk_widget_set_size_request (content, 400, 400);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, content);
  viewport = gtk_scrolled_window_get_child (scroller);
  g_assert_true (GTK_IS_VIEWPORT (viewport));
  configure_at_end (scroller);
  gtk_widget_allocate (window, 200, 200, -1, NULL);
  set_value_at_end (gtk_scrolled_window_get_hadjustment (scroller));
  set_value_at_end (gtk_scrolled_window_get_vadjustment (scroller));
  allocate_test_window (window, 200, 200);
  g_assert_true (gtk_widget_compute_point (content, GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &before));

  g_assert_true (pull_overscroll (scroller, GTK_SCROLL_INPUT_TOUCHPAD));
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (viewport), &x, &y));
  allocate_test_window (window, 200, 200);
  g_assert_true (gtk_widget_compute_point (content, GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &after));
  g_assert_cmpfloat_with_epsilon (after.x - before.x, x, 0.001);
  g_assert_cmpfloat_with_epsilon (after.y - before.y, y, 0.001);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_text_view_content_moves (void)
{
  GtkScrolledWindow *scroller;
  GtkWidget *overlay;
  GtkWidget *text_view;
  GtkWidget *window;
  GString *text;
  graphene_point_t before;
  graphene_point_t after;
  double x = 0;
  double y = 0;

  text_view = gtk_text_view_new ();
  overlay = gtk_label_new ("overlay");
  text = g_string_new (NULL);

  for (guint i = 0; i < 100; i++)
    g_string_append (text, "A line of text that wraps to the available width.\n");

  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (text_view)),
                            text->str,
                            text->len);
  g_string_free (text, TRUE);

  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (text_view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_add_overlay (GTK_TEXT_VIEW (text_view), overlay, 50, 50);

  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, text_view);
  g_assert_true (gtk_widget_compute_point (overlay, GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &before));

  _gtk_scrolled_window_begin_input (scroller, GTK_SCROLL_INPUT_TOUCHPAD, 1000000);
  _gtk_scrolled_window_update_input (scroller, -30, -40, 1010000);
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (text_view), &x, &y));
  g_assert_cmpfloat (x, ==, 0);
  g_assert_cmpfloat (y, >, 0);

  allocate_test_window (window, 200, 200);
  g_assert_true (gtk_widget_compute_point (overlay, GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &after));
  g_assert_cmpfloat_with_epsilon (after.x - before.x, 0, 0.001);
  g_assert_cmpfloat_with_epsilon (after.y - before.y, y, 0.001);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_text_view_bottom_range_change_keeps_overscroll (void)
{
  GtkScrolledWindow *scroller;
  GtkCssProvider *provider;
  GtkWidget *text_view;
  GtkWidget *window;
  GString *text;
  double before_x = 0;
  double before_y = 0;
  double after_x = 0;
  double after_y = 0;

  text_view = gtk_text_view_new ();
  gtk_widget_add_css_class (text_view, "overscroll-range-test");
  text = g_string_new (NULL);
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider,
                                     "textview.overscroll-range-test { line-height: 1.0; }");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  for (guint i = 0; i < 100; i++)
    g_string_append (text, "A line of text that wraps to the available width.\n");

  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (text_view)),
                            text->str,
                            text->len);
  g_string_free (text, TRUE);

  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (text_view), GTK_WRAP_WORD_CHAR);

  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, text_view);

  set_value_at_end (gtk_scrolled_window_get_vadjustment (scroller));
  allocate_test_window (window, 200, 200);

  _gtk_scrolled_window_begin_input (scroller, GTK_SCROLL_INPUT_TOUCHPAD, 1000000);
  _gtk_scrolled_window_update_input (scroller, 0, 40, 1010000);
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (text_view),
                                                &before_x,
                                                &before_y));
  g_assert_cmpfloat (before_x, ==, 0);
  g_assert_cmpfloat (before_y, <, 0);

  gtk_css_provider_load_from_string (provider,
                                     "textview.overscroll-range-test { line-height: 2.0; }");
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (text_view), 48);
  allocate_test_window (window, 200, 200);

  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (text_view),
                                                &after_x,
                                                &after_y));
  g_assert_cmpfloat (after_x, ==, 0);
  g_assert_cmpfloat_with_epsilon (after_y, before_y, 0.001);

  gtk_style_context_remove_provider_for_display (gdk_display_get_default (),
                                                 GTK_STYLE_PROVIDER (provider));
  g_object_unref (provider);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_text_view_range_change_keeps_kinetic (void)
{
  GtkScrolledWindow *scroller;
  GtkCssProvider *provider;
  GtkWidget *text_view;
  GtkWidget *window;
  GString *text;

  text_view = gtk_text_view_new ();
  gtk_widget_add_css_class (text_view, "kinetic-range-test");
  text = g_string_new (NULL);
  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider,
                                     "textview.kinetic-range-test { line-height: 1.0; }");
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  for (guint i = 0; i < 100; i++)
    g_string_append (text, "A line of text that wraps to the available width.\n");

  gtk_text_buffer_set_text (gtk_text_view_get_buffer (GTK_TEXT_VIEW (text_view)),
                            text->str,
                            text->len);
  g_string_free (text, TRUE);

  gtk_text_view_set_wrap_mode (GTK_TEXT_VIEW (text_view), GTK_WRAP_WORD_CHAR);

  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, text_view);

  _gtk_scrolled_window_begin_input (scroller, GTK_SCROLL_INPUT_TOUCHPAD, 1000000);
  _gtk_scrolled_window_update_input (scroller, 0, 40, 1010000);
  _gtk_scrolled_window_release_input (scroller, 0, 1000, 1020000);

  g_assert_cmpuint (_gtk_scrolled_window_get_motion_drivers (scroller), >, 0);

  gtk_css_provider_load_from_string (provider,
                                     "textview.kinetic-range-test { line-height: 2.0; }");
  gtk_text_view_set_bottom_margin (GTK_TEXT_VIEW (text_view), 48);
  allocate_test_window (window, 200, 200);

  g_assert_cmpuint (_gtk_scrolled_window_get_motion_drivers (scroller), >, 0);

  gtk_style_context_remove_provider_for_display (gdk_display_get_default (),
                                                 GTK_STYLE_PROVIDER (provider));
  g_object_unref (provider);
  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_column_view_content_moves (void)
{
  GtkScrolledWindow *scroller;
  GtkWidget *column_view;
  GtkWidget *window;
  GtkListView *listview;
  GtkWidget *row;
  graphene_point_t before_listview;
  graphene_point_t after_listview;
  graphene_point_t before_row;
  graphene_point_t after_row;
  double x = 0;
  double y = 0;

  column_view = create_column_view (100);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, column_view);
  listview = gtk_column_view_get_list_view (GTK_COLUMN_VIEW (column_view));

  set_value_at_end (gtk_scrolled_window_get_hadjustment (scroller));
  set_value_at_end (gtk_scrolled_window_get_vadjustment (scroller));
  allocate_test_window (window, 200, 200);

  row = find_visible_list_child (listview, GTK_WIDGET (scroller), &before_row);
  g_assert_nonnull (row);
  g_assert_true (gtk_widget_compute_point (GTK_WIDGET (listview), GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &before_listview));

  g_assert_true (pull_overscroll (scroller, GTK_SCROLL_INPUT_TOUCHPAD));
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (column_view), &x, &y));
  allocate_test_window (window, 200, 200);

  g_assert_true (gtk_widget_compute_point (GTK_WIDGET (listview), GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &after_listview));
  g_assert_true (gtk_widget_compute_point (row, GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &after_row));
  g_assert_cmpfloat_with_epsilon (after_listview.x - before_listview.x, 0, 0.001);
  g_assert_cmpfloat_with_epsilon (after_listview.y - before_listview.y, 0, 0.001);
  g_assert_cmpfloat_with_epsilon (after_row.x - before_row.x, x, 0.001);
  g_assert_cmpfloat_with_epsilon (after_row.y - before_row.y, y, 0.001);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_column_view_clips_picking (void)
{
  GtkScrolledWindow *scroller;
  GtkWidget *column_view;
  GtkWidget *window;

  column_view = create_column_view (100);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, column_view);

  g_assert_cmpint (gtk_widget_get_width (column_view), ==, 200);
  g_assert_null (gtk_widget_pick (column_view, 220, 5, GTK_PICK_DEFAULT));

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_list_view_content_moves (void)
{
  GtkScrolledWindow *scroller;
  GtkWidget *list_view;
  GtkWidget *window;
  GtkWidget *row;
  graphene_point_t before_row;
  graphene_point_t after_row;
  double x = 0;
  double y = 0;

  list_view = create_list_view (100);
  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  window = create_window_for_scroller (scroller, list_view);

  set_value_at_end (gtk_scrolled_window_get_vadjustment (scroller));
  allocate_test_window (window, 200, 200);

  row = find_visible_list_child (GTK_LIST_VIEW (list_view), GTK_WIDGET (scroller), &before_row);
  g_assert_nonnull (row);

  g_assert_true (pull_overscroll (scroller, GTK_SCROLL_INPUT_TOUCHPAD));
  g_assert_true (gtk_scrollable_get_overscroll (GTK_SCROLLABLE (list_view), &x, &y));
  allocate_test_window (window, 200, 200);

  g_assert_true (gtk_widget_compute_point (row, GTK_WIDGET (scroller),
                                           &GRAPHENE_POINT_INIT (0, 0), &after_row));
  g_assert_cmpfloat_with_epsilon (after_row.x - before_row.x, x, 0.001);
  g_assert_cmpfloat_with_epsilon (after_row.y - before_row.y, y, 0.001);

  gtk_window_destroy (GTK_WINDOW (window));
}

static void
test_modern_scrollables_opt_in (void)
{
  GtkWidget *widgets[3];
  GtkStringList *model;
  GtkListItemFactory *factory;
  GtkSelectionModel *selection;
  GtkColumnViewColumn *column;

  model = gtk_string_list_new (NULL);
  gtk_string_list_append (model, "One");
  gtk_string_list_append (model, "Two");

  factory = gtk_signal_list_item_factory_new ();
  widgets[0] =
    gtk_list_view_new (GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (model)))),
                       g_object_ref (factory));

  selection = GTK_SELECTION_MODEL (gtk_no_selection_new (G_LIST_MODEL (g_object_ref (model))));
  widgets[1] = gtk_column_view_new (selection);
  column = gtk_column_view_column_new ("Column", g_object_ref (factory));
  gtk_column_view_append_column (GTK_COLUMN_VIEW (widgets[1]), column);
  g_object_unref (column);

  widgets[2] = gtk_text_view_new ();

  for (guint i = 0; i < G_N_ELEMENTS (widgets); i++)
    {
      g_assert_cmpint (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (widgets[i]), GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_AUTO);
      g_assert_cmpint (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (widgets[i]), GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_AUTO);
      g_object_ref_sink (widgets[i]);
      g_object_unref (widgets[i]);
    }

  g_object_unref (factory);
  g_object_unref (model);
}

static void
test_deprecated_defaults_none (void)
{
  GtkWidget *tree_view;
  GtkWidget *icon_view;

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS

  tree_view = gtk_tree_view_new ();
  icon_view = gtk_icon_view_new ();

  G_GNUC_END_IGNORE_DEPRECATIONS

  g_assert_cmpint (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (tree_view), GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);
  g_assert_cmpint (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (tree_view), GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);
  g_assert_cmpint (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (icon_view), GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);
  g_assert_cmpint (gtk_scrollable_get_overscroll_behavior (GTK_SCROLLABLE (icon_view), GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);

  g_object_ref_sink (tree_view);
  g_object_unref (tree_view);
  g_object_ref_sink (icon_view);
  g_object_unref (icon_view);
}

static void
test_deprecated_tree_view_uses_legacy_kinetic (void)
{
  GtkScrolledWindow *scroller;
  GtkWidget *tree_view;
  GtkWidget *text_view;

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS

  tree_view = gtk_tree_view_new ();

  G_GNUC_END_IGNORE_DEPRECATIONS

  scroller = GTK_SCROLLED_WINDOW (gtk_scrolled_window_new ());
  g_object_ref_sink (scroller);

  gtk_scrolled_window_set_child (scroller, tree_view);
  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_HORIZONTAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);
  g_assert_cmpint (gtk_scrolled_window_get_overscroll_behavior (scroller, GTK_ORIENTATION_VERTICAL), ==, GTK_OVERSCROLL_BEHAVIOR_NONE);
  g_assert_cmpint (get_scrolled_window_drag_phase (scroller), ==, GTK_PHASE_CAPTURE);

  text_view = gtk_text_view_new ();
  gtk_scrolled_window_set_child (scroller, text_view);
  g_assert_cmpint (get_scrolled_window_drag_phase (scroller), ==, GTK_PHASE_BUBBLE);

  gtk_scrolled_window_set_child (scroller, NULL);
  g_object_unref (scroller);
}

int
main (int   argc,
      char *argv[])
{
  gtk_test_init (&argc, &argv);

  g_test_add_func ("/scrolledwindow/overscroll/getter", test_overscroll_getter);
  g_test_add_func ("/scrolledwindow/overscroll/adjustment-range-expands-during-pull",
                   test_adjustment_range_expands_during_pull);
  g_test_add_func ("/scrolledwindow/overscroll/detached", test_detached_getter);
  g_test_add_func ("/scrolledwindow/overscroll/policy-api", test_policy_api);
  g_test_add_func ("/scrolledwindow/overscroll/child-default-precedence",
                   test_child_default_precedence);
  g_test_add_func ("/scrolledwindow/overscroll/continuous-only-presentation",
                   test_continuous_only_presentation);
  g_test_add_func ("/scrolledwindow/overscroll/none", test_none_contains_without_presentation);
  g_test_add_func ("/scrolledwindow/overscroll/empty-contained-axis",
                   test_empty_contained_axis_stops_parent);
  g_test_add_func ("/scrolledwindow/overscroll/native-boundary-stops-routing",
                   test_native_boundary_stops_routing);
  g_test_add_func ("/scrolledwindow/overscroll/release-returns", test_release_returns_to_rest);
  g_test_add_func ("/scrolledwindow/overscroll/viewport-wrapper", test_viewport_wrapper);
  g_test_add_func ("/scrolledwindow/overscroll/text-view-content-moves",
                   test_text_view_content_moves);
  g_test_add_func ("/scrolledwindow/overscroll/text-view-bottom-range-change",
                   test_text_view_bottom_range_change_keeps_overscroll);
  g_test_add_func ("/scrolledwindow/overscroll/text-view-range-change-keeps-kinetic",
                   test_text_view_range_change_keeps_kinetic);
  g_test_add_func ("/scrolledwindow/overscroll/column-view-content-moves",
                   test_column_view_content_moves);
  g_test_add_func ("/scrolledwindow/overscroll/column-view-clips-picking",
                   test_column_view_clips_picking);
  g_test_add_func ("/scrolledwindow/overscroll/list-view-content-moves",
                   test_list_view_content_moves);
  g_test_add_func ("/scrolledwindow/overscroll/modern-scrollables-opt-in",
                   test_modern_scrollables_opt_in);
  g_test_add_func ("/scrolledwindow/overscroll/deprecated-defaults-none",
                   test_deprecated_defaults_none);
  g_test_add_func ("/scrolledwindow/overscroll/deprecated-tree-view-legacy-kinetic",
                   test_deprecated_tree_view_uses_legacy_kinetic);

  return g_test_run ();
}
