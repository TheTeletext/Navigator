/*
 * suite_logo.h - Shared high-quality logo renderer (GDI+ flat C API).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * Decodes a PNG embedded as an RCDATA resource (alpha preserved) once,
 * caches the GDI+ bitmap for the process lifetime, and draws it scaled
 * with InterpolationModeHighQualityBicubic + SmoothingModeHighQuality +
 * PixelOffsetModeHighQuality so transparent corners composite cleanly
 * over whatever the caller already painted into hdc.
 *
 * Used by the Modern-mode Live Radio and Live TV splash panes to show
 * the Montreal Greek Radio / Montreal Greek TV logos. Retro mode (F1-F6)
 * does not use this service.
 */

#ifndef SUITE_LOGO_H
#define SUITE_LOGO_H

#include <windows.h>
#include <stddef.h>

/* Draw the PNG stored in RCDATA resource `res_id` into `hdc`, scaled to
 * a `dim` x `dim` square with its top-left corner at (x, y). The source
 * PNG's alpha channel is preserved and alpha-blended over the existing
 * pixels in hdc -- no background fill, no flatten, no re-encode.
 *
 * The bitmap is decoded on first use and cached; subsequent calls reuse
 * the cached GDI+ image. GDI+ is started lazily on first call.
 *
 * Returns TRUE if the logo was drawn, FALSE on any failure (missing
 * resource, decode error, GDI+ unavailable). Callers should fall back to
 * their prior drawing when FALSE is returned. */
BOOL suite_logo_draw(HDC hdc, int res_id, int x, int y, int dim);

/* Same as suite_logo_draw, but desaturates the real bitmap to grayscale by
 * luminance (Rec.601 weights 0.299/0.587/0.114) via a GDI+ ImageAttributes
 * ColorMatrix. The source PNG's alpha channel is preserved, so transparent
 * corners still composite cleanly. Used by the F7 CU-SeeMe Start screen so the
 * MGT-TV logo matches the grayscale video. Returns FALSE on any failure. */
BOOL suite_logo_draw_gray(HDC hdc, int res_id, int x, int y, int dim);

/* ------------------------------------------------------------------ */
/* Images from downloaded bytes (Navigator 1.8, Teletext Desktop:      */
/* channel logos, docs/2026-11-20_TELETEXT_DESKTOP_PHASE2A.md).        */
/* ------------------------------------------------------------------ */

/* A decoded image owned by the caller: GIF (first frame; GIF89a
 * transparency kept), ICO/CUR (GDI+, or the icon API when GDI+ refuses
 * one), PNG, JPEG, BMP. Held as 32-bit ARGB, independent of the bytes,
 * which the caller may free at once. Safe to create and free on any
 * thread; draw it on the thread that owns the DC. */
typedef struct SuiteImage SuiteImage;

/* NULL on failure (empty, not an image, larger than 4096 x 4096). */
SuiteImage *suite_image_from_bytes(const void *bytes, size_t n);

/* Native size. Returns FALSE (and 0 x 0) for NULL. */
BOOL suite_image_size(const SuiteImage *img, int *w, int *h);

/* Draw with its top-left corner at (x, y), w x h pixels (w or h <= 0 =
 * native size), NEAREST-NEIGHBOUR so period pixel art stays crisp at
 * whole multiples, alpha composited over what hdc already holds. */
BOOL suite_image_draw(HDC hdc, const SuiteImage *img, int x, int y, int w, int h);

/* The same, scaled SMOOTHLY (high-quality bicubic): for a size that is not
 * a whole multiple, where nearest-neighbour would make uneven pixels. */
BOOL suite_image_draw_hq(HDC hdc, const SuiteImage *img, int x, int y, int w, int h);

void suite_image_free(SuiteImage *img);

#endif /* SUITE_LOGO_H */
