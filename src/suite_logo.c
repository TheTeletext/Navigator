/*
 * suite_logo.c - Shared high-quality logo renderer (GDI+ flat C API).
 *
 * Part of Teletext Navigator.
 * Copyright (C) 2026 Dimitri Papadopoulos and The Teletext
 * This file is part of Teletext Navigator.
 * Licensed under the GNU Affero General Public License v3.0 or later.
 * See /COPYING for the full license text.
 *
 * See suite_logo.h for the contract. Implementation notes:
 *
 *   - The suite already drives GDI+ through its flat C entry points (the
 *     same pattern as newspaper_print.c); this file mirrors those extern
 *     decls so no C++ <gdiplus.h> is pulled in.
 *   - PNG bytes live in an RCDATA resource. We copy them into an HGLOBAL,
 *     wrap it in an IStream, and hand that to GdipCreateBitmapFromStream.
 *     GDI+ keeps a reference to the stream for the bitmap's lifetime, so
 *     the stream (and its HGLOBAL) are cached alongside the bitmap and
 *     never released -- the OS reclaims them at process exit.
 *   - High-quality scaling (bicubic + smoothing + half-pixel offset) is
 *     set on the Graphics context before every DrawImage so logos drawn
 *     at a size other than their native resolution stay crisp.
 */

#include "suite_logo.h"
#include <stdlib.h>
#include <string.h>

#include <objbase.h>
#include <ole2.h>

/* ------------------------------------------------------------------ */
/* GDI+ flat extern decls (same subset the rest of the suite uses).    */
/* ------------------------------------------------------------------ */

typedef struct MgtLogoGdiplusStartupInput_ {
    UINT32 GdiplusVersion;
    void  *DebugEventCallback;
    BOOL   SuppressBackgroundThread;
    BOOL   SuppressExternalCodecs;
} MgtLogoGdiplusStartupInput;

extern int WINAPI GdiplusStartup(ULONG_PTR *token,
                                 const MgtLogoGdiplusStartupInput *input,
                                 void *output);
extern int WINAPI GdipCreateBitmapFromStream(IStream *stream, void **bitmap);
extern int WINAPI GdipGetImageWidth(void *image, UINT *w);
extern int WINAPI GdipGetImageHeight(void *image, UINT *h);
extern int WINAPI GdipCreateFromHDC(HDC hdc, void **graphics);
extern int WINAPI GdipDeleteGraphics(void *graphics);
extern int WINAPI GdipDrawImageRectI(void *graphics, void *image,
                                     INT x, INT y, INT w, INT h);
extern int WINAPI GdipSetInterpolationMode(void *graphics, int mode);
extern int WINAPI GdipSetSmoothingMode(void *graphics, int mode);
extern int WINAPI GdipSetPixelOffsetMode(void *graphics, int mode);
extern int WINAPI GdipCreateImageAttributes(void **imageattr);
extern int WINAPI GdipDisposeImageAttributes(void *imageattr);
extern int WINAPI GdipSetImageAttributesColorMatrix(void *imageattr, int type,
                                 BOOL enableFlag, const void *colorMatrix,
                                 const void *grayMatrix, int flags);
extern int WINAPI GdipDrawImageRectRectI(void *graphics, void *image,
                                 INT dstx, INT dsty, INT dstw, INT dsth,
                                 INT srcx, INT srcy, INT srcw, INT srch,
                                 int srcUnit, void *imageAttributes,
                                 void *callback, void *callbackData);

/* GDI+ enum values (avoid pulling in <gdiplus.h>). */
#define MGT_INTERPOLATION_HIGHQUALITY_BICUBIC  7   /* InterpolationModeHighQualityBicubic */
#define MGT_SMOOTHING_HIGHQUALITY              2   /* SmoothingModeHighQuality */
#define MGT_PIXEL_OFFSET_HIGHQUALITY           2   /* PixelOffsetModeHighQuality */

/* ------------------------------------------------------------------ */
/* Lazy GDI+ startup.                                                  */
/* ------------------------------------------------------------------ */

static ULONG_PTR g_logo_gdiplus_token  = 0;
static BOOL      g_logo_gdiplus_inited = FALSE;
static BOOL      g_logo_gdiplus_failed = FALSE;

/* Run once, from whichever thread asks first: since Navigator 1.8 the
 * channel store decodes downloaded logos on its worker thread
 * (suite_image_from_bytes below), so two threads may race here. */
static INIT_ONCE g_logo_gdiplus_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK logo_gdiplus_once(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    MgtLogoGdiplusStartupInput in;
    (void)once; (void)param; (void)ctx;
    ZeroMemory(&in, sizeof(in));
    in.GdiplusVersion = 1;
    if (GdiplusStartup(&g_logo_gdiplus_token, &in, NULL) == 0)
        g_logo_gdiplus_inited = TRUE;
    else
        g_logo_gdiplus_failed = TRUE;
    return TRUE;
}

static BOOL logo_ensure_gdiplus(void)
{
    if (g_logo_gdiplus_inited) return TRUE;
    if (g_logo_gdiplus_failed)  return FALSE;
    InitOnceExecuteOnce(&g_logo_gdiplus_once, logo_gdiplus_once, NULL, NULL);
    return g_logo_gdiplus_inited;
}

/* ------------------------------------------------------------------ */
/* Per-resource decoded-bitmap cache.                                  */
/* ------------------------------------------------------------------ */

typedef struct LogoCacheEntry {
    int      res_id;
    void    *bitmap;   /* GpBitmap* */
    IStream *stream;   /* kept alive for the bitmap's lifetime */
} LogoCacheEntry;

#define LOGO_CACHE_MAX 16
static LogoCacheEntry g_logo_cache[LOGO_CACHE_MAX];
static int            g_logo_cache_count = 0;

/* Decode the RCDATA PNG `res_id` into a cached GDI+ bitmap. Returns the
 * GpBitmap* or NULL on failure. Repeated calls return the cached value
 * (including a cached NULL is avoided -- a failed decode simply retries,
 * which is harmless because failures are rare and cheap relative to a
 * repaint). */
static void *logo_get_bitmap(int res_id)
{
    int      i;
    HRSRC    hres;
    HGLOBAL  hresdata;
    DWORD    size;
    const void *src;
    HGLOBAL  hg = NULL;
    IStream *stream = NULL;
    void    *bitmap = NULL;
    void    *dst;

    for (i = 0; i < g_logo_cache_count; i++)
        if (g_logo_cache[i].res_id == res_id && g_logo_cache[i].bitmap)
            return g_logo_cache[i].bitmap;

    if (!logo_ensure_gdiplus()) return NULL;

    hres = FindResourceA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(res_id),
                         (LPCSTR)RT_RCDATA);
    if (!hres) return NULL;
    size = SizeofResource(GetModuleHandleA(NULL), hres);
    hresdata = LoadResource(GetModuleHandleA(NULL), hres);
    if (!hresdata || size == 0) return NULL;
    src = LockResource(hresdata);
    if (!src) return NULL;

    hg = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!hg) return NULL;
    dst = GlobalLock(hg);
    if (!dst) { GlobalFree(hg); return NULL; }
    memcpy(dst, src, size);
    GlobalUnlock(hg);

    /* TRUE => the stream takes ownership of hg and frees it on release. */
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK || !stream) {
        GlobalFree(hg);
        return NULL;
    }
    if (GdipCreateBitmapFromStream(stream, &bitmap) != 0 || !bitmap) {
        stream->lpVtbl->Release(stream);
        return NULL;
    }

    if (g_logo_cache_count < LOGO_CACHE_MAX) {
        g_logo_cache[g_logo_cache_count].res_id = res_id;
        g_logo_cache[g_logo_cache_count].bitmap = bitmap;
        g_logo_cache[g_logo_cache_count].stream = stream;   /* keep alive */
        g_logo_cache_count++;
    }
    /* If the cache is full we still return the bitmap; it (and its stream)
     * leak until process exit, but LOGO_CACHE_MAX comfortably exceeds the
     * handful of distinct logos the suite ships. */
    return bitmap;
}

/* ------------------------------------------------------------------ */
/* Public draw.                                                        */
/* ------------------------------------------------------------------ */

BOOL suite_logo_draw(HDC hdc, int res_id, int x, int y, int dim)
{
    void *bitmap;
    void *graphics = NULL;
    BOOL  ok = FALSE;

    if (!hdc || dim <= 0) return FALSE;

    bitmap = logo_get_bitmap(res_id);
    if (!bitmap) return FALSE;

    if (GdipCreateFromHDC(hdc, &graphics) != 0 || !graphics)
        return FALSE;

    /* High-quality scaling for a logo drawn below its native resolution. */
    GdipSetInterpolationMode(graphics, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
    GdipSetSmoothingMode    (graphics, MGT_SMOOTHING_HIGHQUALITY);
    GdipSetPixelOffsetMode  (graphics, MGT_PIXEL_OFFSET_HIGHQUALITY);

    if (GdipDrawImageRectI(graphics, bitmap, x, y, dim, dim) == 0)
        ok = TRUE;

    GdipDeleteGraphics(graphics);
    return ok;
}

BOOL suite_logo_draw_gray(HDC hdc, int res_id, int x, int y, int dim)
{
    /* Luminance desaturation matrix (Rec.601). GDI+ multiplies the row vector
     * [R G B A 1] by this 5x5, so column j is each input channel's weight into
     * output channel j; columns 0..2 (R,G,B out) all use the luminance weights,
     * alpha passes through (m[3][3]=1). */
    static const float lum[5][5] = {
        { 0.299f, 0.299f, 0.299f, 0.0f, 0.0f },
        { 0.587f, 0.587f, 0.587f, 0.0f, 0.0f },
        { 0.114f, 0.114f, 0.114f, 0.0f, 0.0f },
        { 0.0f,   0.0f,   0.0f,   1.0f, 0.0f },
        { 0.0f,   0.0f,   0.0f,   0.0f, 1.0f }
    };
    void *bitmap;
    void *graphics = NULL;
    void *attr = NULL;
    UINT  iw = 0, ih = 0;
    BOOL  ok = FALSE;

    if (!hdc || dim <= 0) return FALSE;

    bitmap = logo_get_bitmap(res_id);
    if (!bitmap) return FALSE;
    if (GdipGetImageWidth(bitmap, &iw) != 0 || GdipGetImageHeight(bitmap, &ih) != 0
        || iw == 0 || ih == 0)
        return FALSE;

    if (GdipCreateFromHDC(hdc, &graphics) != 0 || !graphics)
        return FALSE;

    GdipSetInterpolationMode(graphics, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
    GdipSetSmoothingMode    (graphics, MGT_SMOOTHING_HIGHQUALITY);
    GdipSetPixelOffsetMode  (graphics, MGT_PIXEL_OFFSET_HIGHQUALITY);

    if (GdipCreateImageAttributes(&attr) == 0 && attr) {
        /* type 0 = ColorAdjustTypeDefault, flags 0 = ColorMatrixFlagsDefault */
        GdipSetImageAttributesColorMatrix(attr, 0, TRUE, lum, NULL, 0);
        /* srcUnit 2 = UnitPixel */
        if (GdipDrawImageRectRectI(graphics, bitmap, x, y, dim, dim,
                                   0, 0, (INT)iw, (INT)ih, 2, attr, NULL, NULL) == 0)
            ok = TRUE;
        GdipDisposeImageAttributes(attr);
    }

    GdipDeleteGraphics(graphics);
    return ok;
}


/* ------------------------------------------------------------------ */
/* Images from downloaded bytes (Navigator 1.8).                       */
/* ------------------------------------------------------------------ */

extern int WINAPI GdipCreateBitmapFromScan0(INT w, INT h, INT stride, INT format,
                                            BYTE *scan0, void **bitmap);
extern int WINAPI GdipCreateBitmapFromHICON(HICON hicon, void **bitmap);
extern int WINAPI GdipDisposeImage(void *image);

typedef struct { INT X, Y, Width, Height; } MgtGpRect;
typedef struct {
    UINT      Width;
    UINT      Height;
    INT       Stride;
    INT       PixelFormat;
    void     *Scan0;
    UINT_PTR  Reserved;
} MgtGpBitmapData;
extern int WINAPI GdipBitmapLockBits(void *bitmap, const MgtGpRect *rect, UINT flags,
                                     INT format, MgtGpBitmapData *data);
extern int WINAPI GdipBitmapUnlockBits(void *bitmap, MgtGpBitmapData *data);
#define MGT_LOCK_READ   1
#define MGT_LOCK_WRITE  2

#define MGT_PIXELFORMAT_32BPP_ARGB     0x0026200A  /* PixelFormat32bppARGB */
#define MGT_INTERPOLATION_NEAREST      5           /* InterpolationModeNearestNeighbor */
#define MGT_PIXEL_OFFSET_HALF          4           /* PixelOffsetModeHalf */
#define MGT_SMOOTHING_NONE             3           /* SmoothingModeNone */
#define SUITE_IMAGE_MAX_DIM            4096

struct SuiteImage {
    void *bitmap;   /* GpBitmap*, 32bpp ARGB, owns its pixels */
    int   w, h;
};

/* An .ico the GDI+ decoder refused: let the icon API pick the entry
 * nearest 32 x 32 and convert that. */
static void *image_from_ico(const void *bytes, size_t n)
{
    const BYTE *b = (const BYTE *)bytes;
    int id, count;
    DWORD off, size;
    HICON hi;
    void *bm = NULL;
    if (n < 22 || b[0] != 0 || b[1] != 0 || (b[2] != 1 && b[2] != 2) || b[3] != 0) return NULL;
    count = b[4] | (b[5] << 8);
    if (count <= 0 || (size_t)(6 + 16 * count) > n) return NULL;
    id = LookupIconIdFromDirectoryEx((PBYTE)b, TRUE, 32, 32, LR_DEFAULTCOLOR);
    if (id <= 0 || id > count) return NULL;
    {
        const BYTE *e = b + 6 + 16 * (id - 1);
        size = (DWORD)(e[8] | (e[9] << 8) | (e[10] << 16) | ((DWORD)e[11] << 24));
        off  = (DWORD)(e[12] | (e[13] << 8) | (e[14] << 16) | ((DWORD)e[15] << 24));
    }
    if (off >= n || size > n - off) return NULL;
    hi = CreateIconFromResourceEx((PBYTE)b + off, size, TRUE, 0x00030000, 0, 0, LR_DEFAULTCOLOR);
    if (!hi) return NULL;
    if (GdipCreateBitmapFromHICON(hi, &bm) != 0) bm = NULL;
    DestroyIcon(hi);
    return bm;
}

SuiteImage *suite_image_from_bytes(const void *bytes, size_t n)
{
    HGLOBAL hg;
    void *p, *src = NULL, *dst = NULL;
    IStream *stream = NULL;
    UINT w = 0, h = 0;
    SuiteImage *img;

    if (!bytes || n == 0 || n > 16u * 1024u * 1024u) return NULL;
    if (!logo_ensure_gdiplus()) return NULL;

    hg = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!hg) return NULL;
    p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return NULL; }
    memcpy(p, bytes, n);
    GlobalUnlock(hg);
    if (CreateStreamOnHGlobal(hg, TRUE, &stream) != S_OK || !stream) {
        GlobalFree(hg);
        return NULL;
    }
    if (GdipCreateBitmapFromStream(stream, &src) != 0) src = NULL;
    if (src && (GdipGetImageWidth(src, &w) != 0 || GdipGetImageHeight(src, &h) != 0)) {
        GdipDisposeImage(src);
        src = NULL;
    }
    if (!src) {
        src = image_from_ico(bytes, n);
        if (src && (GdipGetImageWidth(src, &w) != 0 || GdipGetImageHeight(src, &h) != 0)) {
            GdipDisposeImage(src);
            src = NULL;
        }
    }
    if (!src || w == 0 || h == 0 || w > SUITE_IMAGE_MAX_DIM || h > SUITE_IMAGE_MAX_DIM) {
        if (src) GdipDisposeImage(src);
        stream->lpVtbl->Release(stream);
        return NULL;
    }
    /* Copy into a bitmap of our own so the stream can go now. The copy is
     * pixel for pixel through LockBits (GDI+ converts to 32-bit ARGB on
     * the read side): drawing the source instead resampled icons one
     * pixel up and to the left (seen on a 16 x 16 .ico, 2026-11-20). */
    if (GdipCreateBitmapFromScan0((INT)w, (INT)h, 0, MGT_PIXELFORMAT_32BPP_ARGB, NULL, &dst) == 0) {
        MgtGpRect r;
        MgtGpBitmapData sd, dd;
        int ok = 0;
        r.X = 0; r.Y = 0; r.Width = (INT)w; r.Height = (INT)h;
        memset(&sd, 0, sizeof sd);
        memset(&dd, 0, sizeof dd);
        if (GdipBitmapLockBits(src, &r, MGT_LOCK_READ, MGT_PIXELFORMAT_32BPP_ARGB, &sd) == 0) {
            if (GdipBitmapLockBits(dst, &r, MGT_LOCK_WRITE, MGT_PIXELFORMAT_32BPP_ARGB, &dd) == 0) {
                UINT y;
                for (y = 0; y < h; y++)
                    memcpy((BYTE *)dd.Scan0 + (INT_PTR)dd.Stride * (INT)y,
                           (const BYTE *)sd.Scan0 + (INT_PTR)sd.Stride * (INT)y, (size_t)w * 4);
                GdipBitmapUnlockBits(dst, &dd);
                ok = 1;
            }
            GdipBitmapUnlockBits(src, &sd);
        }
        if (!ok) {
            GdipDisposeImage(dst);
            dst = NULL;
        }
    } else {
        dst = NULL;
    }
    GdipDisposeImage(src);
    stream->lpVtbl->Release(stream);
    if (!dst) return NULL;

    img = (SuiteImage *)calloc(1, sizeof *img);
    if (!img) { GdipDisposeImage(dst); return NULL; }
    img->bitmap = dst;
    img->w = (int)w;
    img->h = (int)h;
    return img;
}

BOOL suite_image_size(const SuiteImage *img, int *w, int *h)
{
    if (w) *w = img ? img->w : 0;
    if (h) *h = img ? img->h : 0;
    return img != NULL;
}

BOOL suite_image_draw(HDC hdc, const SuiteImage *img, int x, int y, int w, int h)
{
    void *g = NULL;
    BOOL ok = FALSE;
    if (!hdc || !img || !img->bitmap) return FALSE;
    if (w <= 0 || h <= 0) { w = img->w; h = img->h; }
    if (GdipCreateFromHDC(hdc, &g) != 0 || !g) return FALSE;
    GdipSetInterpolationMode(g, MGT_INTERPOLATION_NEAREST);
    GdipSetPixelOffsetMode(g, MGT_PIXEL_OFFSET_HALF);
    GdipSetSmoothingMode(g, MGT_SMOOTHING_NONE);
    if (GdipDrawImageRectRectI(g, img->bitmap, x, y, w, h, 0, 0, img->w, img->h,
                               2 /* UnitPixel */, NULL, NULL, NULL) == 0)
        ok = TRUE;
    GdipDeleteGraphics(g);
    return ok;
}

BOOL suite_image_draw_hq(HDC hdc, const SuiteImage *img, int x, int y, int w, int h)
{
    void *g = NULL;
    BOOL ok = FALSE;
    if (!hdc || !img || !img->bitmap) return FALSE;
    if (w <= 0 || h <= 0) { w = img->w; h = img->h; }
    if (GdipCreateFromHDC(hdc, &g) != 0 || !g) return FALSE;
    GdipSetInterpolationMode(g, MGT_INTERPOLATION_HIGHQUALITY_BICUBIC);
    GdipSetPixelOffsetMode(g, MGT_PIXEL_OFFSET_HIGHQUALITY);
    GdipSetSmoothingMode(g, MGT_SMOOTHING_HIGHQUALITY);
    if (GdipDrawImageRectRectI(g, img->bitmap, x, y, w, h, 0, 0, img->w, img->h,
                               2 /* UnitPixel */, NULL, NULL, NULL) == 0)
        ok = TRUE;
    GdipDeleteGraphics(g);
    return ok;
}

void suite_image_free(SuiteImage *img)
{
    if (!img) return;
    if (img->bitmap) GdipDisposeImage(img->bitmap);
    free(img);
}
