// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef PUBLIC_EPDF_CATALOG_H_
#define PUBLIC_EPDF_CATALOG_H_

// NOLINTNEXTLINE(build/include)
#include "fpdf_catalog.h"

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// EmbedPDF's additions to public/fpdf_catalog.h: the catalog's language and
// its metadata stream.

// Experimental EmbedPDF API.
// Gets the Catalog /Lang value as a UTF-16LE string with trailing NUL.
//
// If |buffer| is NULL or |length| is 0, the return value is the required length
// in 2-byte units (including the trailing NUL).
//
// If /Lang is missing, returns 0 to signal "not present".
// If /Lang exists but is empty, returns 2 (a single UTF-16 NUL).
//
// document - handle to a document.
// buffer   - destination buffer for UTF-16LE characters (can be NULL to probe).
// length   - size of |buffer| in 2-byte units.
//
// Returns the number of 2-byte units needed/written (including NUL),
// or 0 if /Lang is not present.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFCatalog_GetLanguage(FPDF_DOCUMENT document, void* buffer, unsigned long length);

// Experimental EmbedPDF API.
// The catalog's /Metadata stream (the document's XMP packet) as it reads: its
// bytes with every filter applied. What they say is the caller's to read.
//
// document - handle to a document.
// buffer   - destination for the bytes (can be NULL to probe).
// length   - size of |buffer| in bytes.
//
// Returns the number of bytes, copying them only when |buffer| holds them
// all. 0 when the catalog has no /Metadata stream, or an empty one.
FPDF_EXPORT unsigned long FPDF_CALLCONV
EPDFCatalog_GetMetadata(FPDF_DOCUMENT document, void* buffer, unsigned long length);

// Experimental EmbedPDF API.
// Replaces the bytes of the catalog's /Metadata stream with the |size| bytes
// at |data|, unfiltered, in a stream marked /Type /Metadata /Subtype /XML (so
// a save leaves them uncompressed, as XMP readers expect). The stream keeps
// its object number; a catalog without one gets a new stream. NULL |data|
// removes /Metadata from the catalog. The bytes are not checked: what they
// say is the caller's.
//
// Returns false for an invalid |document|, or for NULL |data| with a
// non-zero |size|.
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFCatalog_SetMetadata(FPDF_DOCUMENT document, const void* data, unsigned long size);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus

#endif  // PUBLIC_EPDF_CATALOG_H_
