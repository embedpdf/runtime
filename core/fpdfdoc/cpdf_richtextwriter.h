// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: writes a CPDF_RichTextDocument into a FreeText annotation: /RC
// (Acrobat's XHTML envelope), /DS, /DA, /Contents and the appearance, in the
// two halves of the Phase C note §6. Prepare builds everything off to the
// side and touches nothing; Publish writes it all, or nothing was written.

#ifndef CORE_FPDFDOC_CPDF_RICHTEXTWRITER_H_
#define CORE_FPDFDOC_CPDF_RICHTEXTWRITER_H_

#include <memory>

#include "core/fpdfdoc/cpdf_generateap.h"
#include "core/fpdfdoc/cpdf_richtext.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/widestring.h"

class CPDF_Dictionary;
class CPDF_Document;

class CPDF_RichTextWriter {
 public:
  struct Prepared {
    Prepared();
    Prepared(Prepared&& that) noexcept;
    Prepared& operator=(Prepared&& that) noexcept;
    ~Prepared();

    WideString ds;
    ByteString da;
    WideString contents;
    WideString rc;
    std::unique_ptr<CPDF_GenerateAP::PreparedRichFreeTextAP> appearance;
    bool degraded = false;
  };

  // Resolves faces, lays out and builds the appearance and its font
  // resources; allocates no object number, reserves no alias, changes no
  // dictionary. False (and nothing written) for an empty rect or a font
  // resource that cannot be staged; an unresolvable family is not a failure
  // (it substitutes and reports |degraded|).
  static bool Prepare(CPDF_Document* doc,
                      const CPDF_Dictionary* annot_dict,
                      const CPDF_RichTextDocument& document,
                      Prepared* out);
  // Publishes the fonts, installs the /DR entry of the /DA font, then sets
  // DS, DA, RC, Contents and AP.
  static void Publish(CPDF_Document* doc,
                      CPDF_Dictionary* annot_dict,
                      Prepared prepared);
  static bool Apply(CPDF_Document* doc,
                    CPDF_Dictionary* annot_dict,
                    const CPDF_RichTextDocument& document,
                    bool* degraded);

  // Test-only: forwards to CPDF_AnnotFontMap::FailAfterStagedFontsForTesting.
  static void FailAfterStagedFontsForTesting(int staged_count);

  // The four forms, exposed for tests. |da_color| is the annotation's DA
  // (border) colour, which DS repeats by Acrobat's convention.
  static WideString SerializeRC(const CPDF_RichTextDocument& document);
  static WideString BuildDefaultStyle(const CPDF_RichTextDocument& document,
                                      FX_ARGB da_color);
  static WideString ProjectPlainText(const CPDF_RichTextDocument& document);
};

#endif  // CORE_FPDFDOC_CPDF_RICHTEXTWRITER_H_
