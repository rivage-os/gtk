/*
 * Copyright © 2026 Christian Hergert
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#pragma once

#if !defined (__GDKWAYLAND_H_INSIDE__) && !defined (GTK_COMPILATION)
#error "Only <gdk/wayland/gdkwayland.h> can be included directly."
#endif

#include <gdk/gdk.h>

G_BEGIN_DECLS

/**
 * GdkWaylandLayer:
 * @GDK_WAYLAND_LAYER_BACKGROUND: behind ordinary windows
 * @GDK_WAYLAND_LAYER_BOTTOM: below ordinary windows
 * @GDK_WAYLAND_LAYER_TOP: above ordinary windows
 * @GDK_WAYLAND_LAYER_OVERLAY: overlay layer
 *
 * The standard layer-shell layers, in stacking order.
 *
 * Since: 4.26
 */
typedef enum
{
  GDK_WAYLAND_LAYER_BACKGROUND,
  GDK_WAYLAND_LAYER_BOTTOM,
  GDK_WAYLAND_LAYER_TOP,
  GDK_WAYLAND_LAYER_OVERLAY
} GdkWaylandLayer;

/**
 * GdkWaylandLayerAnchor:
 * @GDK_WAYLAND_LAYER_ANCHOR_NONE: centered on the output
 * @GDK_WAYLAND_LAYER_ANCHOR_TOP: top edge
 * @GDK_WAYLAND_LAYER_ANCHOR_BOTTOM: bottom edge
 * @GDK_WAYLAND_LAYER_ANCHOR_LEFT: left edge
 * @GDK_WAYLAND_LAYER_ANCHOR_RIGHT: right edge
 *
 * Edges used to anchor a layer surface. Values can be combined.
 *
 * Since: 4.26
 */
typedef enum
{
  GDK_WAYLAND_LAYER_ANCHOR_NONE   = 0,
  GDK_WAYLAND_LAYER_ANCHOR_TOP    = 1 << 0,
  GDK_WAYLAND_LAYER_ANCHOR_BOTTOM = 1 << 1,
  GDK_WAYLAND_LAYER_ANCHOR_LEFT   = 1 << 2,
  GDK_WAYLAND_LAYER_ANCHOR_RIGHT  = 1 << 3
} GdkWaylandLayerAnchor;

/**
 * GdkWaylandLayerKeyboardMode:
 * @GDK_WAYLAND_LAYER_KEYBOARD_NONE: no keyboard focus
 * @GDK_WAYLAND_LAYER_KEYBOARD_EXCLUSIVE: request exclusive keyboard focus
 * @GDK_WAYLAND_LAYER_KEYBOARD_ON_DEMAND: focus when the user interacts
 *
 * Keyboard interactivity for a layer root and its attached popups.
 *
 * Since: 4.26
 */
typedef enum
{
  GDK_WAYLAND_LAYER_KEYBOARD_NONE,
  GDK_WAYLAND_LAYER_KEYBOARD_EXCLUSIVE,
  GDK_WAYLAND_LAYER_KEYBOARD_ON_DEMAND
} GdkWaylandLayerKeyboardMode;

/**
 * GdkWaylandLayerLayout:
 *
 * A copyable descriptor for a Wayland layer surface's desired state.
 *
 * Since: 4.26
 */
typedef struct _GdkWaylandLayerLayout GdkWaylandLayerLayout;

#define GDK_TYPE_WAYLAND_LAYER_LAYOUT (gdk_wayland_layer_layout_get_type())

GDK_AVAILABLE_IN_4_26
GType                        gdk_wayland_layer_layout_get_type           (void);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayerLayout       *gdk_wayland_layer_layout_new                (GdkWaylandLayer               layer);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayerLayout       *gdk_wayland_layer_layout_copy               (const GdkWaylandLayerLayout  *layout);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayerLayout       *gdk_wayland_layer_layout_ref                (GdkWaylandLayerLayout        *layout);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_unref              (GdkWaylandLayerLayout        *layout);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_free               (GdkWaylandLayerLayout        *layout);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_set_layer          (GdkWaylandLayerLayout        *layout,
                                                                          GdkWaylandLayer               layer);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayer              gdk_wayland_layer_layout_get_layer          (const GdkWaylandLayerLayout  *layout);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_set_size           (GdkWaylandLayerLayout        *layout,
                                                                          int                           width,
                                                                          int                           height);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_get_size           (const GdkWaylandLayerLayout  *layout,
                                                                          int                          *width,
                                                                          int                          *height);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_set_anchor         (GdkWaylandLayerLayout        *layout,
                                                                          GdkWaylandLayerAnchor         anchor);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayerAnchor        gdk_wayland_layer_layout_get_anchor         (const GdkWaylandLayerLayout  *layout);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_set_margins        (GdkWaylandLayerLayout        *layout,
                                                                          int                           top,
                                                                          int                           right,
                                                                          int                           bottom,
                                                                          int                           left);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_get_margins        (const GdkWaylandLayerLayout  *layout,
                                                                          int                          *top,
                                                                          int                          *right,
                                                                          int                          *bottom,
                                                                          int                          *left);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_set_exclusive_zone (GdkWaylandLayerLayout        *layout,
                                                                          int                           zone);
GDK_AVAILABLE_IN_4_26
int                          gdk_wayland_layer_layout_get_exclusive_zone (const GdkWaylandLayerLayout  *layout);
GDK_AVAILABLE_IN_4_26
void                         gdk_wayland_layer_layout_set_keyboard_mode  (GdkWaylandLayerLayout        *layout,
                                                                          GdkWaylandLayerKeyboardMode   mode);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayerKeyboardMode  gdk_wayland_layer_layout_get_keyboard_mode  (const GdkWaylandLayerLayout  *layout);
GDK_AVAILABLE_IN_4_26
unsigned int                 gdk_wayland_display_get_layer_shell_version (GdkDisplay                   *display);
GDK_AVAILABLE_IN_4_26
gboolean                     gdk_wayland_toplevel_enable_layer_shell     (GdkToplevel                  *toplevel,
                                                                          GdkMonitor                   *monitor,
                                                                          const char                   *namespace_,
                                                                          const GdkWaylandLayerLayout  *layout,
                                                                          GError                      **error);
GDK_AVAILABLE_IN_4_26
gboolean                     gdk_wayland_toplevel_set_layer_layout       (GdkToplevel                  *toplevel,
                                                                          const GdkWaylandLayerLayout  *layout,
                                                                          GError                      **error);
GDK_AVAILABLE_IN_4_26
GdkWaylandLayerLayout       *gdk_wayland_toplevel_dup_layer_layout       (GdkToplevel                  *toplevel);
GDK_AVAILABLE_IN_4_26
gboolean                     gdk_wayland_toplevel_is_layer_shell         (GdkToplevel                  *toplevel);

G_END_DECLS
