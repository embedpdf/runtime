// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#ifndef PUBLIC_FPDF_PROGRESSIVE_H_
#define PUBLIC_FPDF_PROGRESSIVE_H_

// clang-format off
// NOLINTNEXTLINE(build/include)
#include "fpdfview.h"

// Flags for progressive process status.
#define FPDF_RENDER_READY 0
#define FPDF_RENDER_TOBECONTINUED 1
#define FPDF_RENDER_DONE 2
#define FPDF_RENDER_FAILED 3

#ifdef __cplusplus
extern "C" {
#endif

// IFPDF_RENDERINFO interface.
typedef struct _IFSDK_PAUSE {
  // Version number of the interface. Currently must be 1.
  int version;

  // Method: NeedToPauseNow
  //           Check if we need to pause a progressive process now.
  // Interface Version:
  //           1
  // Implementation Required:
  //           yes
  // Parameters:
  //           pThis       -   Pointer to the interface structure itself
  // Return Value:
  //           Non-zero for pause now, 0 for continue.
  FPDF_BOOL (*NeedToPauseNow)(struct _IFSDK_PAUSE* pThis);

  // A user defined data pointer, used by user's application. Can be NULL.
  void* user;
} IFSDK_PAUSE;

// Experimental API.
// Function: FPDF_RenderPageBitmapWithColorScheme_Start
//          Start to render page contents to a device independent bitmap
//          progressively with a specified color scheme for the content.
// Parameters:
//          bitmap       -   Handle to the device independent bitmap (as the
//                           output buffer). Bitmap handle can be created by
//                           FPDFBitmap_Create function.
//          page         -   Handle to the page as returned by FPDF_LoadPage
//                           function.
//          start_x      -   Left pixel position of the display area in the
//                           bitmap coordinate.
//          start_y      -   Top pixel position of the display area in the
//                           bitmap coordinate.
//          size_x       -   Horizontal size (in pixels) for displaying the
//                           page.
//          size_y       -   Vertical size (in pixels) for displaying the page.
//          rotate       -   Page orientation: 0 (normal), 1 (rotated 90
//                           degrees clockwise), 2 (rotated 180 degrees),
//                           3 (rotated 90 degrees counter-clockwise).
//          flags        -   0 for normal display, or combination of flags
//                           defined in fpdfview.h. With FPDF_ANNOT flag, it
//                           renders all annotations that does not require
//                           user-interaction, which are all annotations except
//                           widget and popup annotations.
//          color_scheme -   Color scheme to be used in rendering the |page|.
//                           If null, this function will work similar to
//                           FPDF_RenderPageBitmap_Start().
//          pause        -   The IFSDK_PAUSE interface. A callback mechanism
//                           allowing the page rendering process.
// Return value:
//          Rendering Status. See flags for progressive process status for the
//          details.
FPDF_EXPORT int FPDF_CALLCONV
FPDF_RenderPageBitmapWithColorScheme_Start(FPDF_BITMAP bitmap,
                                           FPDF_PAGE page,
                                           int start_x,
                                           int start_y,
                                           int size_x,
                                           int size_y,
                                           int rotate,
                                           int flags,
                                           const FPDF_COLORSCHEME* color_scheme,
                                           IFSDK_PAUSE* pause);

// Function: FPDF_RenderPageBitmap_Start
//          Start to render page contents to a device independent bitmap
//          progressively.
// Parameters:
//          bitmap      -   Handle to the device independent bitmap (as the
//                          output buffer). Bitmap handle can be created by
//                          FPDFBitmap_Create().
//          page        -   Handle to the page, as returned by FPDF_LoadPage().
//          start_x     -   Left pixel position of the display area in the
//                          bitmap coordinates.
//          start_y     -   Top pixel position of the display area in the bitmap
//                          coordinates.
//          size_x      -   Horizontal size (in pixels) for displaying the page.
//          size_y      -   Vertical size (in pixels) for displaying the page.
//          rotate      -   Page orientation: 0 (normal), 1 (rotated 90 degrees
//                          clockwise), 2 (rotated 180 degrees), 3 (rotated 90
//                          degrees counter-clockwise).
//          flags       -   0 for normal display, or combination of flags
//                          defined in fpdfview.h. With FPDF_ANNOT flag, it
//                          renders all annotations that does not require
//                          user-interaction, which are all annotations except
//                          widget and popup annotations.
//          pause       -   The IFSDK_PAUSE interface.A callback mechanism
//                          allowing the page rendering process
// Return value:
//          Rendering Status. See flags for progressive process status for the
//          details.
FPDF_EXPORT int FPDF_CALLCONV FPDF_RenderPageBitmap_Start(FPDF_BITMAP bitmap,
                                                          FPDF_PAGE page,
                                                          int start_x,
                                                          int start_y,
                                                          int size_x,
                                                          int size_y,
                                                          int rotate,
                                                          int flags,
                                                          IFSDK_PAUSE* pause);

// Function: FPDF_RenderPage_Continue
//          Continue rendering a PDF page.
// Parameters:
//          page        -   Handle to the page, as returned by FPDF_LoadPage().
//          pause       -   The IFSDK_PAUSE interface (a callback mechanism
//                          allowing the page rendering process to be paused
//                          before it's finished). This can be NULL if you
//                          don't want to pause.
// Return value:
//          The rendering status. See flags for progressive process status for
//          the details.
FPDF_EXPORT int FPDF_CALLCONV FPDF_RenderPage_Continue(FPDF_PAGE page,
                                                       IFSDK_PAUSE* pause);

// Function: FPDF_RenderPage_Close
//          Release the resource allocate during page rendering. Need to be
//          called after finishing rendering or
//          cancel the rendering.
// Parameters:
//          page        -   Handle to the page, as returned by FPDF_LoadPage().
// Return value:
//          None.
FPDF_EXPORT void FPDF_CALLCONV FPDF_RenderPage_Close(FPDF_PAGE page);

// Experimental EmbedPDF Extension API.
// Starts rendering `page` into `bitmap` as FPDF_RenderPageBitmapWithMatrix()
// does, and pauses once `budget_ms` milliseconds have passed, so a caller can
// look at other work, or cancel, between slices. However a render is sliced,
// the bitmap ends with the bytes FPDF_RenderPageBitmapWithMatrix() writes.
//
//   bitmap    - Handle to the bitmap to render into.
//   page      - Handle to the page.
//   matrix    - The transform, as for FPDF_RenderPageBitmapWithMatrix().
//   clipping  - The clip rectangle, as for FPDF_RenderPageBitmapWithMatrix().
//   flags     - 0 or a combination of the flags defined in fpdfview.h.
//   budget_ms - How long to render before pausing; 0 pauses at every chance.
//
// Returns FPDF_RENDER_TOBECONTINUED when paused, FPDF_RENDER_DONE, or
// FPDF_RENDER_FAILED, which it also returns when a render of `page` is in
// progress. Continue a paused render with EPDF_RenderPage_Continue(). Call
// FPDF_RenderPage_Close() after every start, whatever it returned; closing a
// paused render cancels it. Until then the page holds the render: nothing may
// change the page's document, render the page or reset its render cache, and
// `bitmap` must stay alive.
FPDF_EXPORT int FPDF_CALLCONV
EPDF_RenderPageBitmapWithMatrix_Start(FPDF_BITMAP bitmap,
                                      FPDF_PAGE page,
                                      const FS_MATRIX* matrix,
                                      const FS_RECTF* clipping,
                                      int flags,
                                      int budget_ms);

// Experimental EmbedPDF Extension API.
// Continues a render that EPDF_RenderPageBitmapWithMatrix_Start() started,
// pausing again once `budget_ms` milliseconds have passed.
//
//   page      - Handle to the page.
//   budget_ms - How long to render before pausing; 0 pauses at every chance.
//
// Returns FPDF_RENDER_TOBECONTINUED, FPDF_RENDER_DONE or FPDF_RENDER_FAILED.
FPDF_EXPORT int FPDF_CALLCONV EPDF_RenderPage_Continue(FPDF_PAGE page,
                                                       int budget_ms);

#ifdef __cplusplus
}
#endif

#endif  // PUBLIC_FPDF_PROGRESSIVE_H_
