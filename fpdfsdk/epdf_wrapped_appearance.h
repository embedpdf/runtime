// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_WRAPPED_APPEARANCE_H_
#define FPDFSDK_EPDF_WRAPPED_APPEARANCE_H_

#include <optional>

#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/fx_coordinates.h"
#include "core/fxcrt/retain_ptr.h"

class CPDF_Dictionary;
class CPDF_Document;
class CPDF_Stream;

// The normal appearance of a placed drawing (a stamp, or a signature field's
// mark), as EmbedPDF writes it:
//
//   /N        `/R0 gs /MWFOForm Do`, only while the annotation's /CA is below 1
//   MWFOForm  our wrapper, a transparency group: `q a 0 0 d e f cm
//             /EPDFWRAP Do Q`, its /Matrix the rotation, with the turned box
//             at the origin
//   EPDFWRAP  the drawing
//
// The layer uses Acrobat's names. Acrobat drops a layer named so when its
// opacity changes, and keeps any other as part of the artwork, where a new
// opacity multiplies the old one.

// The annotation's constant opacity (/CA), clamped, 1 when absent.
float EpdfGetAnnotOpacity(const CPDF_Dictionary* annot_dict);

// A layer that paints an annotation's /CA over one form: the whole content
// is `[q] /GS gs /Form Do [Q]`, and /GS sets nothing but that opacity.
struct EpdfOpacityLayer {
  ByteString state;       // decoded /ExtGState name
  ByteString form_token;  // the form's name as written in the content
  RetainPtr<const CPDF_Stream> form;
};
std::optional<EpdfOpacityLayer> EpdfFindOpacityLayer(const CPDF_Stream* ap,
                                                     float opacity);

// Our wrapper in an appearance: at its top, or under what editors put around
// it, a layer that paints `opacity` and forms that add nothing. Nothing else
// is looked into. A layer that paints another opacity is part of the drawing:
// the data doesn't describe it. Without `opacity`, a layer of any value is
// looked through, for callers that only need the wrapper's placement.
struct EpdfWrappedAppearance {
  RetainPtr<const CPDF_Stream> wrapper;
  RetainPtr<const CPDF_Stream> drawing;
  // From the wrapper's space to the appearance's: the /Matrix of the wrapper
  // and of every form around it.
  CFX_Matrix wrapper_to_appearance;
};
std::optional<EpdfWrappedAppearance> EpdfFindWrappedAppearance(
    const CPDF_Stream* ap,
    std::optional<float> opacity);

// The opacity layer over `wrapper`, an indirect object of `doc`: a new direct
// stream to set as /N.
RetainPtr<CPDF_Stream> EpdfNewOpacityLayer(CPDF_Document* doc,
                                           const CPDF_Stream* wrapper,
                                           float opacity);

#endif  // FPDFSDK_EPDF_WRAPPED_APPEARANCE_H_
