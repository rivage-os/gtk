/* GTK - The GIMP Toolkit
 * gtkscrollboundariesprivate.h
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

#pragma once

#include "gtkadjustment.h"
#include "gtkeventcontrollerscroll.h"
#include "gtkscrollable.h"
#include "gtkscrolledwindow.h"
#include "gtkwidget.h"

G_BEGIN_DECLS

typedef enum
{
  GTK_SCROLL_ROUTE_X    = 1 << GTK_ORIENTATION_HORIZONTAL,
  GTK_SCROLL_ROUTE_Y    = 1 << GTK_ORIENTATION_VERTICAL,
  GTK_SCROLL_ROUTE_BOTH = (GTK_SCROLL_ROUTE_X | GTK_SCROLL_ROUTE_Y),
} GtkScrollRouteAxes;

typedef enum
{
  GTK_SCROLL_ROUTE_NORMALIZED,
  GTK_SCROLL_ROUTE_WHEEL,
} GtkScrollRouteUnit;

typedef enum
{
  GTK_SCROLL_ROUTE_COMPLETE,
  GTK_SCROLL_ROUTE_INVALID,
  GTK_SCROLL_ROUTE_INVALIDATED,
} GtkScrollRouteStatus;

typedef struct
{
  double value;
  double consumed;
  double remaining;
} GtkScrollRouteConsumption;

typedef struct
{
  GtkAdjustment *adjustment[2];
  double         units_per_input[2];
  guint64        generation;
  guint          axes;
  guint          stationary_axes;
  guint          chain_axes;
} GtkScrollRouteGeometry;

/* Query must be side-effect-free and return borrowed adjustments. It returns
 * FALSE for a detached/disposed or otherwise unsupported receiver. Generation
 * must change when child/coordinates/membership changes even when restoring.
 *
 * For NORMALIZED input, units_per_input maps the original, already normalized
 * vector to adjustment units (including receiver coordinate scale/sign).
 *
 * For WHEEL input it maps detents to adjustment units at this receiver. The
 * router never reapplies surface factors, natural scrolling, or axis swapping.
 */
typedef gboolean (*GtkScrollRouteQuery) (GtkWidget              *widget,
                                         GtkScrollRouteUnit      unit,
                                         GtkScrollRouteGeometry *geometry);

typedef struct
{
  GtkWidget           *widget;
  GtkScrollRouteQuery  query;
} GtkScrollRouteReceiver;

typedef struct _GtkScrollRouteSnapshot GtkScrollRouteSnapshot;

typedef struct
{
  GtkScrollRouteStatus status;
  double               consumed[2];
  double               remaining[2];
  guint                stopped_axes;
  /* Index of the last participating receiver, or G_MAXSIZE. Only usable
   * after COMPLETE and for an axis with nonzero remaining movement.
   */
  gsize                terminal[2];
} GtkScrollRouteResult;

/* Deterministic input and motion, in frame-clock microseconds. */
typedef enum
{
  GTK_SCROLL_INPUT_TOUCHPAD,
  GTK_SCROLL_INPUT_TOUCHSCREEN,
  GTK_SCROLL_INPUT_WHEEL,
  GTK_SCROLL_INPUT_KEYBOARD,
  GTK_SCROLL_INPUT_PROGRAMMATIC
} GtkScrollInputSource;

typedef enum
{
  GTK_SCROLL_MOTION_BEGIN,
  GTK_SCROLL_MOTION_UNWIND,
  GTK_SCROLL_MOTION_PULL,
  GTK_SCROLL_MOTION_RELEASE,
  GTK_SCROLL_MOTION_TICK,
  GTK_SCROLL_MOTION_CANCEL
} GtkScrollMotionCommand;

typedef enum
{
  GTK_SCROLL_MOTION_REST,
  GTK_SCROLL_MOTION_DIRECT,
  GTK_SCROLL_MOTION_RETURN
} GtkScrollMotionPhase;

typedef struct
{
  GtkScrollMotionCommand phase;
  GtkScrollInputSource   source;
  guint64                sequence;
  gint64                 timestamp;
  double                 delta_x;
  double                 delta_y;
  double                 velocity_x;
  double                 velocity_y;
  guint                  axes;
  GtkPositionType        side_x;
  GtkPositionType        side_y;
  guint                  local_axes;
  guint                  chain_axes;
  guint64                generation;
  gboolean               animations;
} GtkScrollMotionEvent;

typedef struct
{
  double   consumed_x;
  double   consumed_y;
  guint    active_axes;
  gboolean needs_frame;
} GtkScrollMotionReply;

typedef struct
{
  gboolean             active;
  gboolean             needs_frame;
  gint64               last_tick;
  GtkScrollMotionPhase motion_phase;
  double               raw;
  double               offset;
  double               velocity;
  double               initial_offset;
  double               initial_velocity;
  gint64               release_time;
} GtkScrollMotionAxis;

typedef struct
{
  graphene_rect_t bounds;
  guint           axes;
  double          mapping_x;
  double          mapping_y;
  guint64         generation;
} GtkScrolledWindowScrollableGeometry;

/* Deltas and velocity are in origin widget units (wheel deltas stay in detents).
 * Timestamps use the frame clock's monotonic microsecond domain.
 */
typedef struct
{
  GtkScrollInputSource source;
  GtkScrollRouteUnit   unit;
  guint64              session;
  gint64               timestamp;
  double               velocity[2];
  gboolean             inertia;
  gboolean             continuous;
  gint64               interval_start;
} GtkScrolledWindowScrollInput;

typedef struct
{
  GtkScrollable                       *child;
  GSignalGroup                        *origin_signals;
  GtkScrollMotionEvent                 event;
  guint64                              session;
  guint64                              generation;
  double                               scale;
  double                               notification_distance;
  guint64                              adjustment_serial;
  GtkScrolledWindowScrollableGeometry  geometry;
  GtkScrollMotionAxis                  motion;
  guint                                released : 1;
} GtkScrolledWindowOverscrollOwner;

typedef struct
{
  GtkOverscrollBehavior             overscroll_behavior[2];
  gboolean                          overscroll_behavior_set[2];
  guint64                           route_generation;
  GdkEvent                         *routed_event;
  guint                             route_depth;
  GtkWidget                        *route_origin;

  GtkScrolledWindowOverscrollOwner  overscroll_axis[2];

  guint64                           input_session;
  guint                             overscroll_tick_id;
  GtkScrolledWindowScrollInput      input;

  double                            drag_offset[2];
  gboolean                          touch_claimed;
  gboolean                          release_pending;
  GtkEventControllerScroll         *release_controller;

  double                            motion_velocity[2];
  gint64                            motion_time;
  guint64                           motion_session;
  gboolean                          advancing_motion;
  gboolean                          touch_release_pending;
  GtkScrollRouteSnapshot           *motion_snapshot;

  gboolean                          allocating_child;
} GtkScrolledWindowBoundary;

gboolean                      _gtk_scroll_route_consume                         (double                               value,
                                                                                 double                               lower,
                                                                                 double                               upper,
                                                                                 double                               page_size,
                                                                                 double                               units_per_input,
                                                                                 double                               delta,
                                                                                 GtkScrollRouteConsumption           *result);
void                          _gtk_scroll_route_dispatch                        (const GtkScrollRouteReceiver        *receivers,
                                                                                 gsize                                n_receivers,
                                                                                 GtkScrollRouteUnit                   unit,
                                                                                 const double                         delta[2],
                                                                                 GtkScrollRouteResult                *result,
                                                                                 GtkScrollRouteSnapshot             **snapshot);
guint64                       _gtk_scroll_route_snapshot_get_adjustment_serial  (GtkScrollRouteSnapshot              *snapshot,
                                                                                 gsize                                position,
                                                                                 guint                                axis);
const GtkScrollRouteGeometry *_gtk_scroll_route_snapshot_get_geometry           (GtkScrollRouteSnapshot              *snapshot,
                                                                                 gsize                                position);
gsize                         _gtk_scroll_route_snapshot_get_n_receivers        (GtkScrollRouteSnapshot              *snapshot);
GtkWidget                    *_gtk_scroll_route_snapshot_get_parent             (GtkScrollRouteSnapshot              *snapshot,
                                                                                 gsize                                position,
                                                                                 gboolean                            *had_parent);
GtkWidget                    *_gtk_scroll_route_snapshot_get_receiver           (GtkScrollRouteSnapshot              *snapshot,
                                                                                 gsize                                position);
void                          _gtk_scroll_route_snapshot_reconcile_adjustment   (GtkScrollRouteSnapshot              *snapshot,
                                                                                 GtkAdjustment                       *adjustment);
GtkScrollRouteSnapshot       *_gtk_scroll_route_snapshot_ref                    (GtkScrollRouteSnapshot              *snapshot);
void                          _gtk_scroll_route_snapshot_unref                  (GtkScrollRouteSnapshot              *snapshot);
guint                         _gtk_scroll_route_snapshot_validate               (GtkScrollRouteSnapshot              *snapshot,
                                                                                 GtkScrollRouteUnit                   unit);
void                          _gtk_scroll_motion_axis_advance                   (GtkScrollMotionAxis                 *motion,
                                                                                 gint64                               timestamp,
                                                                                 double                               limit);
void                          _gtk_scroll_motion_axis_clear                     (GtkScrollMotionAxis                 *motion);
gboolean                      _gtk_scroll_motion_axis_handle                    (GtkScrollMotionAxis                 *motion,
                                                                                 const GtkScrollMotionEvent          *event,
                                                                                 double                               limit,
                                                                                 GtkScrollMotionReply                *reply);
gboolean                      _gtk_scrolled_window_advance_overscroll           (GtkScrolledWindow                   *self,
                                                                                 gint64                               timestamp);
void                          _gtk_scrolled_window_begin_input                  (GtkScrolledWindow                   *self,
                                                                                 GtkScrollInputSource                 source,
                                                                                 gint64                               timestamp);
void                          _gtk_scrolled_window_boundary_begin_input         (GtkScrolledWindow                   *self,
                                                                                 GtkScrollInputSource                 source,
                                                                                 gint64                               timestamp);
void                          _gtk_scrolled_window_boundary_cancel              (GtkScrolledWindow                   *self,
                                                                                 guint                                axes);
gboolean                      _gtk_scrolled_window_boundary_chain_uses_routing  (GtkWidget                           *widget);
gboolean                      _gtk_scrolled_window_boundary_changed_adjustment  (GtkScrolledWindow                   *self,
                                                                                 GtkAdjustment                       *adjustment,
                                                                                 gboolean                            *adopted);
void                          _gtk_scrolled_window_boundary_input_release       (GtkScrolledWindow                   *self,
                                                                                 double                               vx,
                                                                                 double                               vy,
                                                                                 gint64                               timestamp);
gboolean                      _gtk_scrolled_window_boundary_query               (GtkScrolledWindow                   *self,
                                                                                 GtkScrollRouteUnit                   unit,
                                                                                 GtkScrollRouteGeometry              *geometry);
void                          _gtk_scrolled_window_boundary_release             (GtkScrolledWindow                   *self,
                                                                                 GtkScrollMotionCommand               phase);
gboolean                      _gtk_scrolled_window_boundary_route_controller    (GtkScrolledWindow                   *self,
                                                                                 GtkEventControllerScroll            *scroll,
                                                                                 double                               dx,
                                                                                 double                               dy,
                                                                                 gboolean                             continuous);
gboolean                      _gtk_scrolled_window_boundary_route_input_full    (GtkScrolledWindow                   *self,
                                                                                 GtkScrollRouteUnit                   unit,
                                                                                 double                               dx,
                                                                                 double                               dy,
                                                                                 GdkEvent                            *event,
                                                                                 const GtkScrolledWindowScrollInput  *input);
void                          _gtk_scrolled_window_boundary_schedule            (GtkScrolledWindow                   *self);
void                          _gtk_scrolled_window_boundary_update_input        (GtkScrolledWindow                   *self,
                                                                                 double                               dx,
                                                                                 double                               dy,
                                                                                 gint64                               timestamp);
void                          _gtk_scrolled_window_clear_legacy_scrolling       (GtkScrolledWindow                   *self,
                                                                                 gboolean                             cancel_deceleration);
void                          _gtk_scrolled_window_emit_edge_overshot           (GtkScrolledWindow                   *self,
                                                                                 GtkPositionType                      side);
GtkAdjustment                *_gtk_scrolled_window_get_adjustment               (GtkScrolledWindow                   *self,
                                                                                 GtkOrientation                       orientation);
GtkScrolledWindowBoundary    *_gtk_scrolled_window_get_boundary                 (GtkScrolledWindow                   *self);
gboolean                      _gtk_scrolled_window_get_overscroll               (GtkScrolledWindow                   *self,
                                                                                 GtkScrollable                       *scrollable,
                                                                                 double                              *offset_x,
                                                                                 double                              *offset_y);
GtkWidget                    *_gtk_scrolled_window_get_scrollable_child         (GtkScrolledWindow                   *self);
double                        _gtk_scrolled_window_get_wheel_detent_scroll_step (GtkScrolledWindow                   *self,
                                                                                 GtkOrientation                       orientation);
void                          _gtk_scrolled_window_release_input                (GtkScrolledWindow                   *self,
                                                                                 double                               vx,
                                                                                 double                               vy,
                                                                                 gint64                               timestamp);
void                          _gtk_scrolled_window_update_input                 (GtkScrolledWindow                   *self,
                                                                                 double                               dx,
                                                                                 double                               dy,
                                                                                 gint64                               timestamp);

G_END_DECLS
