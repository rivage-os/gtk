/*
 * Copyright © 2026 Christian Hergert
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include "config.h"

#include "gdklayersurface-wayland-private.h"
#include "gdkdisplay-wayland.h"
#include "gdkmonitor-wayland.h"
#include "gdksurface-wayland-private.h"
#include "gdktoplevelprivate.h"

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

struct _GdkWaylandLayerLayout
{
  unsigned int ref_count;
  GdkWaylandLayer layer;
  GdkWaylandLayerAnchor anchor;
  GdkWaylandLayerKeyboardMode keyboard_mode;
  int width;
  int height;
  int top;
  int right;
  int bottom;
  int left;
  int exclusive_zone;
};

struct _GdkWaylandLayerSurface
{
  GdkToplevel *toplevel; /* Owned by the Wayland toplevel. */
  GdkMonitor *monitor;
  char *namespace_;
  GdkWaylandLayerLayout *layout;
  struct zwlr_layer_surface_v1 *proxy;
  int configured_width;
  int configured_height;
  int sent_width;
  int sent_height;
  unsigned int version;
  gboolean closed;
};

G_DEFINE_BOXED_TYPE (GdkWaylandLayerLayout, gdk_wayland_layer_layout,
                     gdk_wayland_layer_layout_copy, gdk_wayland_layer_layout_free)

/**
 * gdk_wayland_layer_layout_new:
 * @layer: the layer
 *
 * Creates a mutable layer layout. Its initial size is widget measured.
 *
 * Returns: (transfer full): a new layer layout
 *
 * Since: 4.26
 */
GdkWaylandLayerLayout *
gdk_wayland_layer_layout_new (GdkWaylandLayer layer)
{
  GdkWaylandLayerLayout *layout;

  g_return_val_if_fail ((unsigned int) layer <= GDK_WAYLAND_LAYER_OVERLAY, NULL);

  layout = g_new0 (GdkWaylandLayerLayout, 1);
  layout->ref_count = 1;
  layout->layer = layer;
  layout->width = -1;
  layout->height = -1;

  return layout;
}

/**
 * gdk_wayland_layer_layout_copy:
 * @layout: a layer layout
 *
 * Returns: (transfer full): an independent copy
 *
 * Since: 4.26
 */
GdkWaylandLayerLayout *
gdk_wayland_layer_layout_copy (const GdkWaylandLayerLayout *layout)
{
  GdkWaylandLayerLayout *copy;

  g_return_val_if_fail (layout != NULL, NULL);

  copy = g_new (GdkWaylandLayerLayout, 1);
  *copy = *layout;
  copy->ref_count = 1;

  return copy;
}

/**
 * gdk_wayland_layer_layout_ref:
 * @layout: a layer layout
 *
 * Adds a reference to a layout. The returned descriptor shares mutable state.
 *
 * Returns: (transfer full): the layout
 *
 * Since: 4.26
 */
GdkWaylandLayerLayout *
gdk_wayland_layer_layout_ref (GdkWaylandLayerLayout *layout)
{
  g_return_val_if_fail (layout != NULL, NULL);
  g_return_val_if_fail (layout->ref_count > 0, NULL);

  layout->ref_count++;
  return layout;
}

/**
 * gdk_wayland_layer_layout_unref:
 * @layout: a layer layout
 *
 * Drops a reference to a layout.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_unref (GdkWaylandLayerLayout *layout)
{
  g_return_if_fail (layout != NULL);
  g_return_if_fail (layout->ref_count > 0);

  layout->ref_count--;
  if (layout->ref_count == 0)
    g_free (layout);
}

/**
 * gdk_wayland_layer_layout_free:
 * @layout: (nullable): a layer layout
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_free (GdkWaylandLayerLayout *layout)
{
  if (layout)
    gdk_wayland_layer_layout_unref (layout);
}

/**
 * gdk_wayland_layer_layout_set_layer:
 * @layout: a layer layout
 * @layer: the desired layer
 *
 * Sets the layer. Changing it on a mapped surface requires protocol version 2.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_set_layer (GdkWaylandLayerLayout *layout,
                                    GdkWaylandLayer        layer)
{
  g_return_if_fail (layout != NULL);
  layout->layer = layer;
}

/**
 * gdk_wayland_layer_layout_get_layer:
 * @layout: a layer layout
 *
 * Returns: the selected layer
 *
 * Since: 4.26
 */
GdkWaylandLayer
gdk_wayland_layer_layout_get_layer (const GdkWaylandLayerLayout *layout)
{
  g_return_val_if_fail (layout != NULL, GDK_WAYLAND_LAYER_BACKGROUND);
  return layout->layer;
}

/**
 * gdk_wayland_layer_layout_set_size:
 * @layout: a layer layout
 * @width: requested logical width, `-1` to measure, or `0` to stretch
 * @height: requested logical height, `-1` to measure, or `0` to stretch
 *
 * Sets the desired dimensions. Zero requires opposing anchors on that axis.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_set_size (GdkWaylandLayerLayout *layout,
                                   int                    width,
                                   int                    height)
{
  g_return_if_fail (layout != NULL);
  layout->width = width;
  layout->height = height;
}

/**
 * gdk_wayland_layer_layout_get_size:
 * @layout: a layer layout
 * @width: (out) (optional): return location for width
 * @height: (out) (optional): return location for height
 *
 * Retrieves the requested logical dimensions.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_get_size (const GdkWaylandLayerLayout *layout,
                                   int                         *width,
                                   int                         *height)
{
  g_return_if_fail (layout != NULL);
  if (width)
    *width = layout->width;
  if (height)
    *height = layout->height;
}

/**
 * gdk_wayland_layer_layout_set_anchor:
 * @layout: a layer layout
 * @anchor: the edge bitmask
 *
 * Sets the edges used to position or stretch the layer surface.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_set_anchor (GdkWaylandLayerLayout *layout,
                                     GdkWaylandLayerAnchor   anchor)
{
  g_return_if_fail (layout != NULL);
  layout->anchor = anchor;
}

/**
 * gdk_wayland_layer_layout_get_anchor:
 * @layout: a layer layout
 *
 * Returns: the edge bitmask
 *
 * Since: 4.26
 */
GdkWaylandLayerAnchor
gdk_wayland_layer_layout_get_anchor (const GdkWaylandLayerLayout *layout)
{
  g_return_val_if_fail (layout != NULL, GDK_WAYLAND_LAYER_ANCHOR_NONE);
  return layout->anchor;
}

/**
 * gdk_wayland_layer_layout_set_margins:
 * @layout: a layer layout
 * @top: signed top margin in logical units
 * @right: signed right margin in logical units
 * @bottom: signed bottom margin in logical units
 * @left: signed left margin in logical units
 *
 * Sets the margins at anchored edges.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_set_margins (GdkWaylandLayerLayout *layout,
                                      int                    top,
                                      int                    right,
                                      int                    bottom,
                                      int                    left)
{
  g_return_if_fail (layout != NULL);
  layout->top = top;
  layout->right = right;
  layout->bottom = bottom;
  layout->left = left;
}

/**
 * gdk_wayland_layer_layout_get_margins:
 * @layout: a layer layout
 * @top: (out) (optional): return location for the top margin
 * @right: (out) (optional): return location for the right margin
 * @bottom: (out) (optional): return location for the bottom margin
 * @left: (out) (optional): return location for the left margin
 *
 * Retrieves the signed margins in logical units.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_get_margins (const GdkWaylandLayerLayout *layout,
                                      int                         *top,
                                      int                         *right,
                                      int                         *bottom,
                                      int                         *left)
{
  g_return_if_fail (layout != NULL);
  if (top)
    *top = layout->top;
  if (right)
    *right = layout->right;
  if (bottom)
    *bottom = layout->bottom;
  if (left)
    *left = layout->left;
}

/**
 * gdk_wayland_layer_layout_set_exclusive_zone:
 * @layout: a layer layout
 * @zone: the reservation, `0` for usable-area placement, or `-1` for full-output placement
 *
 * Sets the exclusive zone in logical units.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_set_exclusive_zone (GdkWaylandLayerLayout *layout,
                                              int                    zone)
{
  g_return_if_fail (layout != NULL);
  layout->exclusive_zone = zone;
}

/**
 * gdk_wayland_layer_layout_get_exclusive_zone:
 * @layout: a layer layout
 *
 * Returns: the exclusive zone in logical units
 *
 * Since: 4.26
 */
int
gdk_wayland_layer_layout_get_exclusive_zone (const GdkWaylandLayerLayout *layout)
{
  g_return_val_if_fail (layout != NULL, 0);
  return layout->exclusive_zone;
}

/**
 * gdk_wayland_layer_layout_set_keyboard_mode:
 * @layout: a layer layout
 * @mode: keyboard interactivity mode
 *
 * Sets keyboard behavior. On-demand mode requires protocol version 4.
 *
 * Since: 4.26
 */
void
gdk_wayland_layer_layout_set_keyboard_mode (GdkWaylandLayerLayout       *layout,
                                             GdkWaylandLayerKeyboardMode  mode)
{
  g_return_if_fail (layout != NULL);
  layout->keyboard_mode = mode;
}

/**
 * gdk_wayland_layer_layout_get_keyboard_mode:
 * @layout: a layer layout
 *
 * Returns: the keyboard interactivity mode
 *
 * Since: 4.26
 */
GdkWaylandLayerKeyboardMode
gdk_wayland_layer_layout_get_keyboard_mode (const GdkWaylandLayerLayout *layout)
{
  g_return_val_if_fail (layout != NULL, GDK_WAYLAND_LAYER_KEYBOARD_NONE);
  return layout->keyboard_mode;
}

/**
 * gdk_wayland_display_get_layer_shell_version:
 * @display: a Wayland display
 *
 * Returns: the negotiated layer-shell version, or zero if unavailable
 *
 * Since: 4.26
 */
unsigned int
gdk_wayland_display_get_layer_shell_version (GdkDisplay *display)
{
  g_return_val_if_fail (GDK_IS_DISPLAY (display), 0);

  if (!GDK_IS_WAYLAND_DISPLAY (display))
    return 0;

  if (GDK_WAYLAND_DISPLAY (display)->layer_shell == NULL)
    return 0;

  return zwlr_layer_shell_v1_get_version (GDK_WAYLAND_DISPLAY (display)->layer_shell);
}

gboolean
_gdk_wayland_layer_layout_validate (const GdkWaylandLayerLayout *layout,
                                     unsigned int                 version,
                                     GError                      **error)
{
  g_return_val_if_fail (layout != NULL, FALSE);

  if ((unsigned int) layout->layer > GDK_WAYLAND_LAYER_OVERLAY ||
      layout->anchor & ~15 ||
      (unsigned int) layout->keyboard_mode > GDK_WAYLAND_LAYER_KEYBOARD_ON_DEMAND ||
      layout->width < -1 || layout->height < -1 ||
      layout->exclusive_zone < -1 ||
      (layout->width == 0 &&
       (layout->anchor & (GDK_WAYLAND_LAYER_ANCHOR_LEFT | GDK_WAYLAND_LAYER_ANCHOR_RIGHT)) !=
       (GDK_WAYLAND_LAYER_ANCHOR_LEFT | GDK_WAYLAND_LAYER_ANCHOR_RIGHT)) ||
      (layout->height == 0 &&
       (layout->anchor & (GDK_WAYLAND_LAYER_ANCHOR_TOP | GDK_WAYLAND_LAYER_ANCHOR_BOTTOM)) !=
       (GDK_WAYLAND_LAYER_ANCHOR_TOP | GDK_WAYLAND_LAYER_ANCHOR_BOTTOM)))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                           "Invalid layer layout or unsupported anchor and size combination");
      return FALSE;
    }

  if (layout->keyboard_mode == GDK_WAYLAND_LAYER_KEYBOARD_ON_DEMAND && version < 4)
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "Layer layout requires a newer layer-shell version");
      return FALSE;
    }

  return TRUE;
}

static void
layer_surface_configure (void                         *data,
                         struct zwlr_layer_surface_v1 *proxy,
                         uint32_t                      serial,
                         uint32_t                      width,
                         uint32_t                      height)
{
  GdkWaylandLayerSurface *layer = data;

  if (layer->closed)
    return;

  layer->configured_width = width;
  layer->configured_height = height;
  gdk_wayland_surface_handle_configure (GDK_WAYLAND_SURFACE (layer->toplevel), serial);
}

static void
layer_surface_closed (void                         *data,
                      struct zwlr_layer_surface_v1 *proxy)
{
  GdkWaylandLayerSurface *layer = data;

  layer->closed = TRUE;
  GDK_WAYLAND_SURFACE (layer->toplevel)->suppress_commit = TRUE;
  gdk_surface_hide (GDK_SURFACE (layer->toplevel));
  _gdk_wayland_toplevel_layer_closed (layer->toplevel);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  layer_surface_configure,
  layer_surface_closed,
};

static void
measure_layout (GdkWaylandLayerSurface *layer,
                int                    *width,
                int                    *height)
{
  GdkSurface *surface = GDK_SURFACE (layer->toplevel);
  GdkRectangle geometry = { 0, 0, 1, 1 };
  GdkToplevelSize size;

  if (layer->monitor)
    gdk_monitor_get_geometry (layer->monitor, &geometry);
  else
    {
      GListModel *monitors = gdk_display_get_monitors (gdk_surface_get_display (surface));

      if (g_list_model_get_n_items (monitors) > 0)
        {
          GdkMonitor *monitor = g_list_model_get_item (monitors, 0);
          gdk_monitor_get_geometry (monitor, &geometry);
          g_object_unref (monitor);
        }
    }

  gdk_toplevel_size_init (&size, MAX (geometry.width, 1), MAX (geometry.height, 1));
  gdk_toplevel_notify_compute_size (layer->toplevel, &size);
  *width = MAX (size.width, 1);
  *height = MAX (size.height, 1);
}

static void
send_layout (GdkWaylandLayerSurface *layer,
             gboolean                initial)
{
  GdkWaylandLayerLayout *layout = layer->layout;
  struct zwlr_layer_surface_v1 *proxy = layer->proxy;
  unsigned int version = zwlr_layer_surface_v1_get_version (proxy);
  int measured_width, measured_height;
  int width, height;

  measure_layout (layer, &measured_width, &measured_height);
  width = layout->width == -1 ? measured_width : layout->width;
  height = layout->height == -1 ? measured_height : layout->height;

  zwlr_layer_surface_v1_set_size (proxy, width, height);
  layer->sent_width = width;
  layer->sent_height = height;
  zwlr_layer_surface_v1_set_anchor (proxy, layout->anchor);
  zwlr_layer_surface_v1_set_margin (proxy, layout->top, layout->right,
                                    layout->bottom, layout->left);
  zwlr_layer_surface_v1_set_exclusive_zone (proxy, layout->exclusive_zone);
  zwlr_layer_surface_v1_set_keyboard_interactivity (proxy, layout->keyboard_mode);
  if (!initial && version >= 2)
    zwlr_layer_surface_v1_set_layer (proxy, layout->layer);
}

GdkWaylandLayerSurface *
_gdk_wayland_layer_surface_new (GdkToplevel                 *toplevel,
                                GdkMonitor                  *monitor,
                                const char                  *namespace_,
                                const GdkWaylandLayerLayout *layout)
{
  GdkWaylandLayerSurface *layer;

  layer = g_new0 (GdkWaylandLayerSurface, 1);
  layer->toplevel = toplevel;
  layer->monitor = monitor ? g_object_ref (monitor) : NULL;
  layer->namespace_ = g_strdup (namespace_);
  layer->layout = gdk_wayland_layer_layout_copy (layout);
  layer->version = gdk_wayland_display_get_layer_shell_version (
    gdk_surface_get_display (GDK_SURFACE (toplevel)));

  return layer;
}

void
_gdk_wayland_layer_surface_free (GdkWaylandLayerSurface *layer)
{
  if (layer == NULL)
    return;

  g_clear_pointer (&layer->proxy, zwlr_layer_surface_v1_destroy);
  g_clear_object (&layer->monitor);
  g_clear_pointer (&layer->layout, gdk_wayland_layer_layout_free);
  g_clear_pointer (&layer->namespace_, g_free);
  g_free (layer);
}

gboolean
_gdk_wayland_layer_surface_show (GdkWaylandLayerSurface *layer)
{
  GdkSurface *surface = GDK_SURFACE (layer->toplevel);
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (surface);
  GdkWaylandDisplay *display = GDK_WAYLAND_DISPLAY (gdk_surface_get_display (surface));
  struct wl_output *output = layer->monitor ? gdk_wayland_monitor_get_wl_output (layer->monitor) : NULL;
  gboolean initial = layer->proxy == NULL;

  if (layer->closed)
    return FALSE;
  if ((initial && display->layer_shell == NULL) ||
      (layer->monitor && !gdk_monitor_is_valid (layer->monitor)))
    {
      layer->closed = TRUE;
      wayland_surface->suppress_commit = TRUE;
      _gdk_wayland_toplevel_layer_closed (layer->toplevel);
      return FALSE;
    }

  gdk_surface_freeze_updates (surface);
  if (layer->proxy == NULL)
    {
      layer->proxy = zwlr_layer_shell_v1_get_layer_surface (display->layer_shell,
                                                            wayland_surface->display_server.wl_surface,
                                                            output,
                                                            layer->layout->layer,
                                                            layer->namespace_);
      wl_proxy_set_queue ((struct wl_proxy *) layer->proxy, wayland_surface->event_queue);
      zwlr_layer_surface_v1_add_listener (layer->proxy, &layer_surface_listener, layer);
    }
  layer->configured_width = 0;
  layer->configured_height = 0;
  send_layout (layer, initial);
  wl_surface_commit (wayland_surface->display_server.wl_surface);
  return TRUE;
}

void
_gdk_wayland_layer_surface_hide (GdkWaylandLayerSurface *layer)
{
  GdkWaylandSurface *surface = GDK_WAYLAND_SURFACE (layer->toplevel);

  if (!GDK_SURFACE_DESTROYED (surface))
    gdk_surface_queue_state_change (GDK_SURFACE (surface), GDK_TOPLEVEL_STATE_FOCUSED, 0);

  if (layer->proxy == NULL)
    return;

  if (layer->closed)
    g_clear_pointer (&layer->proxy, zwlr_layer_surface_v1_destroy);
  layer->configured_width = 0;
  layer->configured_height = 0;
  if (!surface->initial_configure_received)
    gdk_surface_thaw_updates (GDK_SURFACE (surface));
  surface->initial_configure_received = FALSE;
}

void
_gdk_wayland_layer_surface_configure (GdkWaylandLayerSurface *layer)
{
  GdkWaylandSurface *surface = GDK_WAYLAND_SURFACE (layer->toplevel);

  surface->next_layout.configured_width = layer->configured_width;
  surface->next_layout.configured_height = layer->configured_height;
  surface->next_layout.surface_geometry_dirty = TRUE;
  gdk_surface_request_layout (GDK_SURFACE (surface));
  zwlr_layer_surface_v1_ack_configure (layer->proxy, surface->pending.serial);
}

void
_gdk_wayland_layer_surface_compute_size (GdkWaylandLayerSurface *layer)
{
  GdkWaylandSurface *wayland_surface = GDK_WAYLAND_SURFACE (layer->toplevel);
  GdkSurface *surface = GDK_SURFACE (layer->toplevel);
  int measured_width, measured_height;
  int width, height;

  if (!wayland_surface->next_layout.surface_geometry_dirty)
    return;

  measure_layout (layer, &measured_width, &measured_height);
  if (layer->proxy && !layer->closed && wayland_surface->initial_configure_received)
    {
      int requested_width = layer->layout->width == -1 ? measured_width : layer->layout->width;
      int requested_height = layer->layout->height == -1 ? measured_height : layer->layout->height;

      if (requested_width != layer->sent_width || requested_height != layer->sent_height)
        {
          zwlr_layer_surface_v1_set_size (layer->proxy, requested_width, requested_height);
          layer->sent_width = requested_width;
          layer->sent_height = requested_height;
          gdk_wayland_surface_force_next_commit (surface);
        }
    }
  width = layer->layout->width > 0 ? layer->layout->width :
          layer->layout->width == 0 && layer->configured_width > 0 ? layer->configured_width : measured_width;
  height = layer->layout->height > 0 ? layer->layout->height :
           layer->layout->height == 0 && layer->configured_height > 0 ? layer->configured_height : measured_height;
  gdk_wayland_surface_update_size (surface, MAX (width, 1), MAX (height, 1),
                                   &wayland_surface->scale);
  wayland_surface->next_layout.surface_geometry_dirty = FALSE;
}

void
_gdk_wayland_layer_surface_set_layout (GdkWaylandLayerSurface      *layer,
                                       const GdkWaylandLayerLayout *layout)
{
  GdkSurface *surface = GDK_SURFACE (layer->toplevel);

  g_clear_pointer (&layer->layout, gdk_wayland_layer_layout_free);
  layer->layout = gdk_wayland_layer_layout_copy (layout);
  if (layer->proxy != NULL && !layer->closed)
    {
      send_layout (layer, FALSE);
      gdk_wayland_surface_force_next_commit (surface);
    }
  GDK_WAYLAND_SURFACE (surface)->next_layout.surface_geometry_dirty = TRUE;
  gdk_surface_request_layout (surface);
}

gboolean
_gdk_wayland_layer_surface_is_closed (GdkWaylandLayerSurface *layer)
{
  return layer->closed;
}

GdkWaylandLayer
_gdk_wayland_layer_surface_get_layer (GdkWaylandLayerSurface *layer)
{
  return layer->layout->layer;
}

unsigned int
_gdk_wayland_layer_surface_get_version (GdkWaylandLayerSurface *layer)
{
  return layer->version;
}

GdkWaylandLayerLayout *
_gdk_wayland_layer_surface_dup_layout (GdkWaylandLayerSurface *layer)
{
  return gdk_wayland_layer_layout_copy (layer->layout);
}

struct zwlr_layer_surface_v1 *
_gdk_wayland_layer_surface_get_proxy (GdkWaylandLayerSurface *layer)
{
  return layer->proxy;
}
