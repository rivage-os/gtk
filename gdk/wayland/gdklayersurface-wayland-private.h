/*
 * Copyright © 2026 Christian Hergert
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#pragma once

#include "gdkwaylandlayer.h"

typedef struct _GdkWaylandLayerSurface GdkWaylandLayerSurface;
struct zwlr_layer_surface_v1;

struct zwlr_layer_surface_v1 *
_gdk_wayland_layer_surface_get_proxy (GdkWaylandLayerSurface *layer);

gboolean                _gdk_wayland_layer_layout_validate      (const GdkWaylandLayerLayout  *layout,
                                                                 unsigned int                  version,
                                                                 GError                      **error);
GdkWaylandLayerSurface *_gdk_wayland_layer_surface_new          (GdkToplevel                  *toplevel,
                                                                 GdkMonitor                   *monitor,
                                                                 const char                   *namespace_,
                                                                 const GdkWaylandLayerLayout  *layout);
void                    _gdk_wayland_layer_surface_free         (GdkWaylandLayerSurface       *layer);
gboolean                _gdk_wayland_layer_surface_show         (GdkWaylandLayerSurface       *layer);
void                    _gdk_wayland_layer_surface_hide         (GdkWaylandLayerSurface       *layer);
void                    _gdk_wayland_layer_surface_configure    (GdkWaylandLayerSurface       *layer);
void                    _gdk_wayland_layer_surface_compute_size (GdkWaylandLayerSurface       *layer);
void                    _gdk_wayland_layer_surface_set_layout   (GdkWaylandLayerSurface       *layer,
                                                                 const GdkWaylandLayerLayout  *layout);
gboolean                _gdk_wayland_layer_surface_is_closed    (GdkWaylandLayerSurface       *layer);
GdkWaylandLayer         _gdk_wayland_layer_surface_get_layer    (GdkWaylandLayerSurface       *layer);
unsigned int            _gdk_wayland_layer_surface_get_version  (GdkWaylandLayerSurface       *layer);
GdkWaylandLayerLayout  *_gdk_wayland_layer_surface_dup_layout   (GdkWaylandLayerSurface       *layer);
void                    _gdk_wayland_toplevel_layer_closed      (GdkToplevel                  *toplevel);
