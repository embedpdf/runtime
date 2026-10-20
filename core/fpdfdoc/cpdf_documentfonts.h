// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0
//
// EmbedPDF: the fonts a document uses, found in its dictionaries without
// loading a page, and the facts about each embedded program. Facts only: what
// may be written with a font, or sent where, is the engine's decision.

#ifndef CORE_FPDFDOC_CPDF_DOCUMENTFONTS_H_
#define CORE_FPDFDOC_CPDF_DOCUMENTFONTS_H_

#include <stddef.h>
#include <stdint.h>

#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fxcrt/bytestring.h"
#include "core/fxcrt/data_vector.h"
#include "core/fxcrt/retain_ptr.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxcrt/widestring.h"
#include "core/fxge/cfx_codepointrange.h"
#include "core/fxge/cfx_fontregistry.h"

class CPDF_Stream;

class CPDF_DocumentFonts final : public CPDF_Document::FontIndex {
 public:
  // What a font program is, from the font dictionary that holds it.
  enum class Format : uint8_t {
    kNone,         // nothing embedded: the PDF only names the font
    kTrueType,     // /FontFile2, or /FontFile3 /OpenType with glyf outlines
    kOpenTypeCFF,  // /FontFile3 /OpenType with CFF outlines
    kCFF,          // /FontFile3 /Type1C or /CIDFontType0C: bare CFF
    kType1,        // /FontFile
    kType3,        // glyphs drawn by content streams: no program
  };
  enum class Embedded : uint8_t { kNone, kWhole, kSubset };

  struct Program {
    // The program stream's object number; 0 when there is no program, or
    // when the stream is inline (it can't be addressed).
    uint32_t objnum = 0;
    Format format = Format::kNone;
    Embedded embedded = Embedded::kNone;
    // /BaseFont without its subset tag (a Type0 font's descendant's).
    ByteString postscript_name;
    // What the OS/2 fsType grants; nullopt when the program has no OS/2
    // table to say (Type 1, bare CFF) or there is no program.
    std::optional<CFX_FontRegistry::EmbeddingPermission> permission;
    // The program loads, has glyphs and maps characters through a Unicode
    // cmap: the layout can write any character it maps.
    bool maps_unicode = false;
    // An sfnt whose tables each start on a four-byte boundary, as the font
    // format puts them. FreeType reads one whose tables don't; a browser
    // refuses it.
    bool tables_aligned = false;
    // SHA-256 of the decoded program; empty without one.
    DataVector<uint8_t> sha256;
    // SHA-256 of what a browser loads for it: the program itself when it is
    // an sfnt that maps characters through a Unicode cmap, with its tables
    // realigned when they aren't (cfx_sfnt.h); else its display program
    // (cpdf_displayprogram.h). Empty when a browser can't draw it.
    DataVector<uint8_t> display_sha256;
    // Line metrics as the layout reads them (em, descent positive); 0 when
    // the program doesn't load.
    float ascent = 0;
    float descent = 0;
  };

  struct Face {
    WideString family;
    int weight = 400;
    bool italic = false;
    // One per program stream, in the order the walk found them; fonts
    // without a program (not embedded, Type 3) add one entry per format.
    std::vector<Program> programs;
  };

  explicit CPDF_DocumentFonts(CPDF_Document* doc);
  ~CPDF_DocumentFonts() override;

  // The document's index, made on first use.
  static CPDF_DocumentFonts* For(CPDF_Document* doc);
  // The document's index if it has one yet; null otherwise.
  static CPDF_DocumentFonts* Find(const CPDF_Document* doc);

  // Every face the document's fonts are, grouped by family, weight and
  // italic. Found in /AcroForm /DR, fields' own /DR, every page's resources
  // (inherited ones too), form XObjects, tiling patterns, soft masks, Type 3
  // glyph resources and every state of every annotation appearance. Each
  // dictionary is walked once; cycles stop. Programs are inspected once per
  // stream version.
  std::vector<Face> CollectFaces();

  // A program found by the walk, by its stream's object number: its decoded
  // bytes, the program as the layout reads it. Empty for any other number.
  DataVector<uint8_t> ProgramData(uint32_t objnum);
  // What a browser loads for such a program (see Program::display_sha256);
  // empty when it can't draw it or |objnum| isn't one.
  DataVector<uint8_t> DisplayProgramData(uint32_t objnum);
  // The scalars a browser draws with it, which are the ones the engine writes
  // with it, as ranges in order; empty when it maps none or |objnum| isn't
  // one.
  std::vector<CFX_CodePointRange> ProgramCoverage(uint32_t objnum);
  // The program stream itself, for the writer; null when |objnum| isn't one.
  RetainPtr<CPDF_Stream> ProgramStream(uint32_t objnum);

  // A /DA font naming a document program: like a registered font's, the
  // alias is this session's until an appearance installs the /DR entry.
  // Reserve picks "EDocF<objnum>", suffixed when /DR holds the key already;
  // false when |objnum| isn't a program the walk found.
  struct ReservedProgram {
    uint32_t objnum = 0;
    ByteString base_font_name;  // without its subset tag
    WideString family;
    int weight = 400;
    bool italic = false;
  };
  bool ReserveProgramAlias(uint32_t objnum, ByteString* alias);
  std::optional<ReservedProgram> ProgramForAlias(const ByteString& alias) const;

  // Characters an appearance drew with no font that has them (each a
  // grapheme, drawn as its face's missing glyph), first seen first, at most
  // kMaxMissingCharacters. The engine clears them before a write and reads
  // them after it.
  static constexpr size_t kMaxMissingCharacters = 256;
  void NoteMissingCharacter(const WideString& grapheme);
  const std::vector<WideString>& missing_characters() const {
    return missing_characters_;
  }
  void ClearMissingCharacters() { missing_characters_.clear(); }

 private:
  // What a browser loads in place of a program it can't load as it is.
  struct Display {
    DataVector<uint8_t> bytes;
    DataVector<uint8_t> sha256;
    std::vector<CFX_CodePointRange> coverage;
  };

  struct Inspected {
    RetainPtr<const CPDF_Stream> stream;  // the version inspected
    size_t raw_size = 0;
    Program facts;
    std::optional<std::vector<CFX_CodePointRange>> coverage;
    // Built from the first font dictionary found to use the stream.
    bool display_built = false;
    std::optional<Display> display;
  };

  // The facts about |stream|, inspected once per version.
  Program Inspect(RetainPtr<const CPDF_Stream> stream, Format format);
  Inspected* FindInspected(uint32_t objnum);
  // The decoded bytes of the stream |inspected| is.
  DataVector<uint8_t> BytesOf(const Inspected& inspected);
  // |program|'s display hash, its display program built through
  // |font_dict| once per stream version when it needs one.
  DataVector<uint8_t> DisplayHashOf(const Program& program,
                                    const CPDF_Dictionary* font_dict,
                                    const WideString& family,
                                    int weight,
                                    bool italic);

  UnownedPtr<CPDF_Document> const doc_;
  std::map<uint32_t, Inspected> inspected_;  // by program stream objnum
  std::map<ByteString, ReservedProgram> program_aliases_;
  std::vector<WideString> missing_characters_;
};

#endif  // CORE_FPDFDOC_CPDF_DOCUMENTFONTS_H_
