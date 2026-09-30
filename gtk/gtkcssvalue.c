/* GTK - The GIMP Toolkit
 * Copyright (C) 2011 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "gtkprivate.h"
#include "gtkcssvalueprivate.h"

#include "gtkcssstyleprivate.h"
#include "gtkstyleproviderprivate.h"

struct _GtkCssValue {
  GTK_CSS_VALUE_BASE
};

G_DEFINE_BOXED_TYPE (GtkCssValue, gtk_css_value, gtk_css_value_ref, gtk_css_value_unref)

#ifdef CSS_VALUE_ACCOUNTING
static GHashTable *counters;
static GHashTable *sizes;
static GHashTable *shared_sizes;

typedef struct
{
  guint64 all;
  guint64 alive;
  guint64 requested;
  guint64 live_bytes;
  guint64 peak_bytes;
  guint computed;
  guint transitioned;
} ValueAccounting;

typedef struct
{
  ValueAccounting *counter;
  gsize bytes;
  guint users;
} SharedAccounting;

static void
dump_value_counts (void)
{
  GHashTableIter iter;
  gpointer key, value;
  ValueAccounting total = { 0, };

  g_print ("CSS value type,allocations,live,requested bytes,live bytes,peak bytes,computed,transitioned\n");
  g_hash_table_iter_init (&iter, counters);
  while (g_hash_table_iter_next (&iter, &key, &value))
    {
      const ValueAccounting *c = value;

      total.all += c->all;
      total.alive += c->alive;
      total.requested += c->requested;
      total.live_bytes += c->live_bytes;
      total.peak_bytes += c->peak_bytes;
      total.computed += c->computed;
      total.transitioned += c->transitioned;

      g_print ("%s,%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT
               ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",%u,%u\n",
               (const char *) key, c->all, c->alive, c->requested,
               c->live_bytes, c->peak_bytes, c->computed, c->transitioned);
    }

  g_print ("TOTAL,%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT
           ",%" G_GUINT64_FORMAT ",%" G_GUINT64_FORMAT ",%u,%u\n",
           total.all, total.alive, total.requested, total.live_bytes,
           total.peak_bytes, total.computed, total.transitioned);
}

static ValueAccounting *
get_accounting_data (const char *class)
{
  ValueAccounting *counter;

  if (counters == NULL)
    {
      counters = g_hash_table_new (g_str_hash, g_str_equal);
      sizes = g_hash_table_new (g_direct_hash, g_direct_equal);
      shared_sizes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
      atexit (dump_value_counts);
    }

  counter = g_hash_table_lookup (counters, class);
  if (counter == NULL)
    {
      counter = g_new0 (ValueAccounting, 1);
      g_hash_table_insert (counters, (gpointer) class, counter);
    }

  return counter;
}

static void
account_bytes (ValueAccounting *counter,
               gsize            bytes)
{
  counter->requested += bytes;
  counter->live_bytes += bytes;
  counter->peak_bytes = MAX (counter->peak_bytes, counter->live_bytes);
}
#endif

void
gtk_css_value_account_extra (GtkCssValue *value,
                             gsize        bytes)
{
#ifdef CSS_VALUE_ACCOUNTING
  ValueAccounting *counter = get_accounting_data (value->class->type_name);
  gsize size = GPOINTER_TO_SIZE (g_hash_table_lookup (sizes, value));

  g_hash_table_insert (sizes, value, GSIZE_TO_POINTER (size + bytes));
  account_bytes (counter, bytes);
#endif
}

void
gtk_css_value_account_shared (GtkCssValue *value,
                              gpointer     shared,
                              gsize        bytes,
                              gboolean     add)
{
#ifdef CSS_VALUE_ACCOUNTING
  SharedAccounting *account = g_hash_table_lookup (shared_sizes, shared);

  if (add)
    {
      if (account == NULL)
        {
          account = g_new0 (SharedAccounting, 1);
          account->counter = get_accounting_data (value->class->type_name);
          account->bytes = bytes;
          account_bytes (account->counter, bytes);
          g_hash_table_insert (shared_sizes, shared, account);
        }
      account->users++;
    }
  else if (account != NULL && --account->users == 0)
    {
      account->counter->live_bytes -= account->bytes;
      g_hash_table_remove (shared_sizes, shared);
    }
#endif
}

GtkCssValue *
gtk_css_value_alloc (const GtkCssValueClass *klass,
                     gsize                   size)
{
  GtkCssValue *value;

  value = g_malloc0 (size);

  value->class = klass;
  value->ref_count = 1;

#ifdef CSS_VALUE_ACCOUNTING
  {
    ValueAccounting *c;

    c = get_accounting_data (klass->type_name);
    c->all++;
    c->alive++;
    account_bytes (c, size);
    g_hash_table_insert (sizes, value, GSIZE_TO_POINTER (size));
  }
#endif

  return value;
}

GtkCssValue *
(gtk_css_value_ref) (GtkCssValue *value)
{
  gtk_internal_return_val_if_fail (value != NULL, NULL);

  value->ref_count += 1;

  return value;
}

void
(gtk_css_value_unref) (GtkCssValue *value)
{
  if (value == NULL)
    return;

  value->ref_count -= 1;
  if (value->ref_count > 0)
    return;

#ifdef CSS_VALUE_ACCOUNTING
  {
    ValueAccounting *c;

    c = get_accounting_data (value->class->type_name);
    c->alive--;
    c->live_bytes -= GPOINTER_TO_SIZE (g_hash_table_lookup (sizes, value));
    g_hash_table_remove (sizes, value);
  }
#endif

  value->class->free (value);
}

/**
 * gtk_css_value_compute:
 * @value: the value to compute from
 * @property_id: the ID of the property to compute
 * @context: the context containing the style provider, style
 *   parent style and variables that might be used during computation
 *
 * Converts the specified @value into the computed value for the CSS
 * property given by @property_id using the information in @context.
 * This step is explained in detail in the
 * [CSS Documentation](http://www.w3.org/TR/css3-cascade/#computed).
 *
 * Returns: the computed value
 **/
GtkCssValue *
gtk_css_value_compute (GtkCssValue          *value,
                       guint                 property_id,
                       GtkCssComputeContext *context)
{
  if (gtk_css_value_is_computed (value))
    return gtk_css_value_ref (value);

#ifdef CSS_VALUE_ACCOUNTING
  get_accounting_data (value->class->type_name)->computed++;
#endif

  return value->class->compute (value, property_id, context);
}

/**
 * gtk_css_value_resolve:
 * @value: the value to resolve
 * @context: the context containing the style provider, style
 *   parent style and variables that might be used during computation
 * @current_color: the value to use for currentcolor
 *
 * Converts the computed @value into the used value, by replacing
 * currentcolor with @current_color.
 *
 * Returns: the used value
 */
GtkCssValue *
gtk_css_value_resolve (GtkCssValue          *value,
                       GtkCssComputeContext *context,
                       GtkCssValue          *current_color)
{
  if (!gtk_css_value_contains_current_color (value))
    return gtk_css_value_ref (value);

  if (!value->class->resolve)
    return gtk_css_value_ref (value);

  return value->class->resolve (value, context, current_color);
}

gboolean
gtk_css_value_equal (const GtkCssValue *value1,
                     const GtkCssValue *value2)
{
  gtk_internal_return_val_if_fail (value1 != NULL, FALSE);
  gtk_internal_return_val_if_fail (value2 != NULL, FALSE);

  if (value1 == value2)
    return TRUE;

  if (value1->class != value2->class)
    return FALSE;

  return value1->class->equal (value1, value2);
}

gboolean
gtk_css_value_equal0 (const GtkCssValue *value1,
                      const GtkCssValue *value2)
{
  /* Includes both values being NULL */
  if (value1 == value2)
    return TRUE;

  if (value1 == NULL || value2 == NULL)
    return FALSE;

  return gtk_css_value_equal (value1, value2);
}

GtkCssValue *
gtk_css_value_transition (GtkCssValue *start,
                          GtkCssValue *end,
                          guint        property_id,
                          double       progress)
{
  gtk_internal_return_val_if_fail (start != NULL, NULL);
  gtk_internal_return_val_if_fail (end != NULL, NULL);

  if (start->class != end->class)
    return NULL;

  if (progress == 0)
    return gtk_css_value_ref (start);

  if (progress == 1)
    return gtk_css_value_ref (end);

  if (start == end)
    return gtk_css_value_ref (start);

#ifdef CSS_VALUE_ACCOUNTING
  get_accounting_data (start->class->type_name)->transitioned++;
#endif

  return start->class->transition (start, end, property_id, progress);
}

char *
gtk_css_value_to_string (const GtkCssValue *value)
{
  GString *string;

  gtk_internal_return_val_if_fail (value != NULL, NULL);

  string = g_string_new (NULL);
  gtk_css_value_print (value, string);
  return g_string_free (string, FALSE);
}

/**
 * gtk_css_value_print:
 * @value: the value to print
 * @string: the string to print to
 *
 * Prints @value to the given @string in CSS format. The @value must be a
 * valid specified value as parsed using the parse functions or as assigned
 * via _gtk_style_property_assign().
 **/
void
gtk_css_value_print (const GtkCssValue *value,
                     GString           *string)
{
  gtk_internal_return_if_fail (value != NULL);
  gtk_internal_return_if_fail (string != NULL);

  value->class->print (value, string);
}

/**
 * gtk_css_value_is_dynamic:
 * @value: a `GtkCssValue`
 *
 * A "dynamic" value has a different value at different times. This means that
 * the value needs to be animated when time is progressing.
 *
 * Examples of dynamic values are animated images, such as videos or dynamic shaders.
 *
 * Use gtk_css_value_get_dynamic_value() to get the value for a given timestamp.
 *
 * Returns %TRUE if the value is dynamic
 */
gboolean
gtk_css_value_is_dynamic (const GtkCssValue *value)
{
  gtk_internal_return_val_if_fail (value != NULL, FALSE);

  if (!value->class->is_dynamic)
    return FALSE;

  return value->class->is_dynamic (value);
}

/**
 * gtk_css_value_get_dynamic_value:
 * @value: a `GtkCssValue`
 * @monotonic_time: the timestamp for which to get the dynamic value
 *
 * Gets the dynamic value for a given timestamp. If @monotonic_time is 0,
 * the default value is returned.
 *
 * See gtk_css_value_is_dynamic() for details about dynamic values.
 *
 * Returns: (transfer full): The dynamic value for @value at the given
 *   timestamp
 */
GtkCssValue *
gtk_css_value_get_dynamic_value (GtkCssValue *value,
                                 gint64       monotonic_time)
{
  gtk_internal_return_val_if_fail (value != NULL, NULL);

  if (!value->class->get_dynamic_value)
    return gtk_css_value_ref (value);

  return value->class->get_dynamic_value (value, monotonic_time);
}
