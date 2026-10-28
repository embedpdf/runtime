// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_PPO_H_
#define PUBLIC_EPDF_PPO_H_

#include <stdint.h>

// NOLINTNEXTLINE(build/include)
#include "fpdf_ppo.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's additions to public/fpdf_ppo.h: copying pages with the form
// fields of their widgets.
//
// A widget is a view of a form field: the field holds the name and the value,
// its widgets sit on pages. FPDF_ImportPagesByIndex() copies a page's widgets
// but not their fields, and leaves each copied widget's /Parent naming an
// object of the source document.
//
// The fields some pages bring are those above the widgets on them: each
// widget's field, that field's parent, and so on up to a top-level field. A
// widget with no parent and a field type (/FT) is its own top-level field. A
// widget's field is the one its /Parent names, or, when it names none, the
// field whose /Kids in the source's /AcroForm lists it. A field with no
// widget on those pages stays behind.

// Experimental EmbedPDF Extension API.
// FPDF_ImportPagesByIndex() that brings the pages' form fields along:
//
//   - Each field above the copied widgets is copied once. A copied field's
//     /Kids lists what was copied below it: the widgets on the copied pages,
//     and the fields above those. Each /Parent names a copy.
//   - Each copied top-level field is added at the end of |dest_doc|'s
//     /AcroForm /Fields, which is made when |dest_doc| has none. Names are
//     kept: renaming a top-level field whose name |dest_doc| already uses is
//     the caller's, in |src_doc| before the copy (EPDFForm_SetFieldName()).
//   - What the source's /AcroForm gives its fields comes along: the /DR
//     resources |dest_doc|'s /DR doesn't name (a font under a name
//     |dest_doc| uses for another font gets a free name, and the /DA of the
//     copied fields and widgets uses it); its /DA and /Q as a copied
//     top-level field's own, when the field has none and |dest_doc|'s
//     /AcroForm says otherwise; /NeedAppearances when it is set; the copied
//     fields in /CO, in their order, after |dest_doc|'s; and SignaturesExist
//     in /SigFlags when a signature field is copied. Never its /XFA.
//   - A copied annotation's /Parent names the copy of what it named, or is
//     removed when that wasn't copied: none names an object of |src_doc|.
//
// |page_indices| (|length| of them, 0-based) are the pages of |src_doc| to
// copy, in order; NULL copies every page, and |length| is then ignored.
// |index| is where the first goes in |dest_doc|. The documents must differ.
// As with FPDF_ImportPagesByIndex(), the copies may still read streams from
// |src_doc|: keep it open while |dest_doc| is in use.
//
// Returns TRUE on success. On FALSE, pages may already have been copied: on a
// layer document, the caller aborts its transaction.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDFDoc_ImportPages(FPDF_DOCUMENT dest_doc,
                                                        FPDF_DOCUMENT src_doc,
                                                        const int* page_indices,
                                                        unsigned long length,
                                                        int index);

// Experimental EmbedPDF Extension API.
// The top-level form fields EPDFDoc_ImportPages() brings with the pages at
// |page_indices| of |document| (as there; NULL: every page): the object
// numbers of their dictionaries, in the order it adds them to /Fields. Writes
// up to |buffer_count| of them to |buffer|, which may be NULL, and returns how
// many there are; 0 also for an index out of range.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFDoc_GetImportFieldRoots(FPDF_DOCUMENT document,
                            const int* page_indices,
                            unsigned long length,
                            uint32_t* buffer,
                            unsigned long buffer_count);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_PPO_H_
