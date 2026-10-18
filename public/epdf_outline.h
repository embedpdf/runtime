// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_OUTLINE_H_
#define PUBLIC_EPDF_OUTLINE_H_

#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "fpdf_doc.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// The document outline (/Root /Outlines, "bookmarks"), addressed by object
// number. The readers of public/fpdf_doc.h (FPDFBookmark_GetFirstChild() and
// the rest) read it; these functions name its items and write it.
//
// An item is an outline item dictionary, which ISO 32000 requires to be an
// indirect object: its object number names it, and a layer never gives that
// number to anything else. A |parent| is an item's number, or 0 for the
// outline itself, whose children are the top-level items. An item is in the
// outline when each step up its /Parent chain is among the parent's children,
// up to /Root /Outlines: a deleted item is not, even where its dictionary can
// still be read.
//
// A parent's children are its /First and the /Next that follow, a loop ending
// the list. Every write keeps the outline well formed:
//  * It rewrites /Parent, /Prev, /Next, /First and /Last of every list it
//    changes, from that walk, so a wrong /Last or /Prev never misleads it. It
//    writes only the keys whose value changes.
//  * It keeps /Count: every ancestor of what changed counts its visible
//    descendants again, from the tree and not from the counts it held,
//    keeping its sign (positive when open). The outline's own /Count is the
//    number of items visible when the document opens. An item without
//    children has no /Count.
//  * It refuses, changing nothing, when a list it would change holds an item
//    that isn't an indirect object, or when /Root /Outlines is a direct
//    dictionary.
// Every function here works on a layer inside or outside a transaction
// (EPDFLayer_BeginTransaction()), and on a plain document.

// Experimental EmbedPDF Extension API.
// The object number of |bookmark|, a handle from the readers of fpdf_doc.h.
//
// Returns 0 for NULL, or when the item isn't an indirect object.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFOutline_GetObjectNumber(FPDF_BOOKMARK bookmark);

// Experimental EmbedPDF Extension API.
// A handle to the item |objnum|, for the readers of fpdf_doc.h. It reads the
// item as the document holds it now; take a new one after a write.
//
//   document - handle to the document.
//   objnum   - the item's object number.
//
// Returns NULL when |objnum| is not an item in the outline.
FPDF_EXPORT FPDF_BOOKMARK FPDF_CALLCONV
EPDFOutline_GetItem(FPDF_DOCUMENT document, uint32_t objnum);

// Experimental EmbedPDF Extension API.
// Create an item with |title| and no target, closed, under |parent|, right
// after its child |after|. Creates /Root /Outlines when the document has
// none, at the next free number.
//
//   document - handle to the document.
//   objnum   - the object number the item gets, or 0 for the next free one.
//              A number other than 0 needs a layer transaction and must be
//              free (EPDFLayer_RaiseLastObjectNumber()).
//   parent   - the item it goes under, or 0 for the top level.
//   after    - the child of |parent| it follows, or 0 to be the first.
//   title    - UTF-16LE encoded; NULL for an empty title. /Title is always
//              written.
//
// Returns the new item's object number, or 0 when |parent| is not an item in
// the outline, |after| is not a child of |parent|, the number isn't free, or
// on error.
FPDF_EXPORT uint32_t FPDF_CALLCONV
EPDFOutline_CreateItem(FPDF_DOCUMENT document,
                       uint32_t objnum,
                       uint32_t parent,
                       uint32_t after,
                       FPDF_WIDESTRING title);

// Experimental EmbedPDF Extension API.
// Move the item |objnum|, with its descendants, under |parent|, right after
// its child |after|. The item and everything under it keep their numbers and
// every key; a closed item stays closed.
//
//   document - handle to the document.
//   objnum   - the item to move.
//   parent   - the item it goes under, or 0 for the top level.
//   after    - the child of |parent| it follows, or 0 to be the first.
//
// Returns true on success. False when |objnum| or |parent| is not an item in
// the outline, |parent| is |objnum| or one of its descendants, |after| is not
// a child of |parent|, or |after| is |objnum|.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_MoveItem(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         uint32_t parent,
                                                         uint32_t after);

// Experimental EmbedPDF Extension API.
// Delete the item |objnum| and its descendants: unlinked from its parent,
// their dictionaries deleted. Actions and destinations they refer to stay
// until nothing reaches them (a layer save drops them then). When the last
// item goes, /Root /Outlines stays, without /First, /Last and /Count.
//
//   document - handle to the document.
//   objnum   - the item to delete.
//
// Returns true on success, false when |objnum| is not an item in the outline.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_DeleteItem(FPDF_DOCUMENT document, uint32_t objnum);

// Experimental EmbedPDF Extension API.
// Set the item's /Title.
//
//   document - handle to the document.
//   objnum   - the item.
//   title    - UTF-16LE encoded; NULL for an empty title.
//
// Returns true on success, false when |objnum| is not an item in the outline.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetTitle(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         FPDF_WIDESTRING title);

// Experimental EmbedPDF Extension API.
// Set whether the item shows its children when the document opens: the sign
// of its /Count. Its ancestors count again. An item without children has no
// open state, and nothing is written.
//
//   document - handle to the document.
//   objnum   - the item.
//   open     - true for open.
//
// Returns true on success, false when |objnum| is not an item in the outline.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetOpen(FPDF_DOCUMENT document,
                                                        uint32_t objnum,
                                                        FPDF_BOOL open);

// Experimental EmbedPDF Extension API.
// Set the item's /C, the colour its title is drawn in.
//
//   document  - handle to the document.
//   objnum    - the item.
//   has_color - false removes /C (the default: black); |r|, |g| and |b| are
//               then ignored.
//   r, g, b   - the DeviceRGB components, each from 0 to 1.
//
// Returns true on success. False when |objnum| is not an item in the outline,
// or a component is outside 0 to 1.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetColor(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         FPDF_BOOL has_color,
                                                         float r,
                                                         float g,
                                                         float b);

// Experimental EmbedPDF Extension API.
// Set the item's /F, the style its title is drawn in: bit 1 italic, bit 2
// bold (ISO 32000-2 Table 152). FPDFBookmark_GetStyle() reads it.
//
//   document - handle to the document.
//   objnum   - the item.
//   flags    - the flags, written as given; 0 removes /F.
//
// Returns true on success. False when |objnum| is not an item in the outline,
// or |flags| is negative.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFOutline_SetStyle(FPDF_DOCUMENT document,
                                                         uint32_t objnum,
                                                         int flags);

// Experimental EmbedPDF Extension API.
// Set the item's target to |action|: /A refers to it and /Dest is removed,
// since an item has one or the other. NULL removes both.
//
//   document - handle to the document.
//   objnum   - the item.
//   action   - an action of |document| that is an indirect object, such as
//              one EPDFAction_CreateGoTo() made; or NULL.
//
// Returns true on success. False when |objnum| is not an item in the outline,
// or |action| belongs to another document or isn't indirect.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFOutline_SetAction(FPDF_DOCUMENT document,
                      uint32_t objnum,
                      FPDF_ACTION action);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_OUTLINE_H_
