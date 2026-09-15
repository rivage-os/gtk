/* scrollable-fixture.h
 *
 * Copyright 2026 Christian Hergert <christian@sourceandstack.com>
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

#pragma once

#include <gtk/gtk.h>

/* A scrollable without boundary participation, independent of built-in views. */
typedef struct
{
  GtkWidget            parent;
  GtkAdjustment       *adjustments[2];
  GtkScrollablePolicy  policies[2];
  guint                overscroll_changes;
  guint                configure_on_allocate : 1;
  guint                follow_upper_on_allocate : 1;
  double               offset[2];
} FixtureScrollable;

typedef GtkWidgetClass FixtureScrollableClass;

static GType fixture_scrollable_get_type (void);
static void fixture_scrollable_scrollable_init (GtkScrollableInterface *iface);

G_DEFINE_TYPE_WITH_CODE (FixtureScrollable, fixture_scrollable, GTK_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (GTK_TYPE_SCROLLABLE,
                                                fixture_scrollable_scrollable_init))

static void
fixture_scrollable_overscroll_changed (GtkScrollable *scrollable)
{
  FixtureScrollable *self = (FixtureScrollable *) scrollable;

  self->overscroll_changes++;

  gtk_scrollable_get_overscroll (scrollable, &self->offset[0], &self->offset[1]);
}

static GtkOverscrollBehavior
fixture_scrollable_get_overscroll_behavior (GtkScrollable  *scrollable,
                                            GtkOrientation  orientation)
{
  return GTK_OVERSCROLL_BEHAVIOR_AUTO;
}

static void
fixture_scrollable_scrollable_init (GtkScrollableInterface *iface)
{
  iface->get_overscroll_behavior = fixture_scrollable_get_overscroll_behavior;
  iface->overscroll_changed = fixture_scrollable_overscroll_changed;
}

static void
fixture_scrollable_get_property (GObject    *object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  FixtureScrollable *self = (FixtureScrollable *) object;

  if (prop_id <= 2)
    g_value_set_object (value, self->adjustments[prop_id - 1]);
  else
    g_value_set_enum (value, self->policies[prop_id - 3]);
}

static void
fixture_scrollable_set_property (GObject      *object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  FixtureScrollable *self = (FixtureScrollable *) object;

  if (prop_id <= 2)
    {
      GtkAdjustment *adjustment = g_value_get_object (value);

      if (adjustment == NULL)
        adjustment = gtk_adjustment_new (0, 0, 0, 0, 0, 0);

      g_object_ref_sink (adjustment);
      g_clear_object (&self->adjustments[prop_id - 1]);
      self->adjustments[prop_id - 1] = adjustment;
    }
  else
    self->policies[prop_id - 3] = g_value_get_enum (value);
}

static void
fixture_scrollable_dispose (GObject *object)
{
  FixtureScrollable *self = (FixtureScrollable *) object;

  g_clear_object (&self->adjustments[0]);
  g_clear_object (&self->adjustments[1]);

  G_OBJECT_CLASS (fixture_scrollable_parent_class)->dispose (object);
}

static void
fixture_scrollable_size_allocate (GtkWidget *widget,
                                  int        width,
                                  int        height,
                                  int        baseline)
{
  FixtureScrollable *self = (FixtureScrollable *) widget;

  if (self->configure_on_allocate)
    {
      GtkAdjustment *adjustment = self->adjustments[GTK_ORIENTATION_VERTICAL];

      self->configure_on_allocate = FALSE;
      gtk_adjustment_configure (adjustment,
                                gtk_adjustment_get_value (adjustment) + (self->follow_upper_on_allocate ? 10 : 0),
                                gtk_adjustment_get_lower (adjustment),
                                gtk_adjustment_get_upper (adjustment) + 10,
                                gtk_adjustment_get_step_increment (adjustment),
                                gtk_adjustment_get_page_increment (adjustment),
                                gtk_adjustment_get_page_size (adjustment));
    }
}

static void
fixture_scrollable_class_init (FixtureScrollableClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->get_property = fixture_scrollable_get_property;
  object_class->set_property = fixture_scrollable_set_property;
  object_class->dispose = fixture_scrollable_dispose;

  widget_class->size_allocate = fixture_scrollable_size_allocate;

  g_object_class_override_property (object_class, 1, "hadjustment");
  g_object_class_override_property (object_class, 2, "vadjustment");
  g_object_class_override_property (object_class, 3, "hscroll-policy");
  g_object_class_override_property (object_class, 4, "vscroll-policy");
}

static void
fixture_scrollable_init (FixtureScrollable *self)
{
}
