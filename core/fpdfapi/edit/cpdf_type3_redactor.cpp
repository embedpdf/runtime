// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#include "core/fpdfapi/edit/cpdf_type3_redactor.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "core/fpdfapi/edit/cpdf_pagecontentgenerator.h"
#include "core/fpdfapi/edit/cpdf_redaction_copies.h"
#include "core/fpdfapi/edit/cpdf_resourceuse.h"
#include "core/fpdfapi/edit/cpdf_tounicodecmap.h"
#include "core/fpdfapi/font/cpdf_tounicodemap.h"
#include "core/fpdfapi/font/cpdf_type3char.h"
#include "core/fpdfapi/font/cpdf_type3font.h"
#include "core/fpdfapi/page/cpdf_clippath.h"
#include "core/fpdfapi/page/cpdf_color.h"
#include "core/fpdfapi/page/cpdf_colorstate.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_formobject.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageobject.h"
#include "core/fpdfapi/page/cpdf_pattern.h"
#include "core/fpdfapi/page/cpdf_textobject.h"
#include "core/fpdfapi/page/cpdf_textstate.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_reference.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fxcrt/containers/contains.h"
#include "core/fxcrt/data_vector.h"

namespace {

// The codes text shows in each Type3 font.
using CodesByFont = std::map<CPDF_Font*, std::set<uint32_t>>;

// What a copy draws instead of an object, by the original: the object number
// of its replacement.
struct Replacements {
  // Fonts, by the resources of the content selecting them: content reading
  // other resources can need another copy of the same font.
  std::map<std::pair<const CPDF_Dictionary*, const CPDF_Object*>, uint32_t>
      fonts;
  // Content with resources of its own - a form, a soft mask's graphics state,
  // a tiling pattern: the same copy wherever it is drawn.
  std::map<const CPDF_Object*, uint32_t> contents;
  // The fonts content shows text in, by its context, replaced or not. A font
  // its content selects without showing text in holds no glyph for it.
  std::set<std::pair<const CPDF_Dictionary*, const CPDF_Object*>> shown_fonts;

  // The replacement content reading `context` draws for `object`, a
  // resource of `type`.
  std::optional<uint32_t> Find(const CPDF_Dictionary* context,
                               const ByteString& type,
                               const CPDF_Object* object) const {
    if (type == "Font") {
      auto it = fonts.find({context, object});
      return it != fonts.end() ? std::optional<uint32_t>(it->second)
                               : std::nullopt;
    }
    auto it = contents.find(object);
    return it != contents.end() ? std::optional<uint32_t>(it->second)
                                : std::nullopt;
  }
};

// Whether text in `holder`, or in a form it draws, shows in `font`.
bool ShowsTextIn(const CPDF_PageObjectHolder* holder, const CPDF_Font* font) {
  for (const auto& object : *holder) {
    if (!object->IsActive()) {
      continue;
    }
    const CPDF_TextObject* text = object->AsText();
    if (text && text->GetFont().Get() == font) {
      return true;
    }
    const CPDF_ClipPath& clip_path = object->clip_path();
    for (size_t i = 0; clip_path.HasRef() && i < clip_path.GetTextCount();
         ++i) {
      if (clip_path.GetText(i) &&
          clip_path.GetText(i)->GetFont().Get() == font) {
        return true;
      }
    }
    const CPDF_FormObject* form = object->AsForm();
    if (form && ShowsTextIn(form->form(), font)) {
      return true;
    }
  }
  return false;
}

// A Type3 glyph procedure as PDFium draws it: a form whose page resources
// are the glyphs' (CPDF_Type3Font::LoadChar()).
std::unique_ptr<CPDF_Form> ParseGlyph(CPDF_Document* document,
                                      const CPDF_Dictionary* glyph_resources,
                                      const CPDF_Stream* proc) {
  auto glyph = std::make_unique<CPDF_Form>(
      document,
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(glyph_resources)),
      pdfium::WrapRetain(const_cast<CPDF_Stream*>(proc)));
  CPDF_Type3Char unused;
  glyph->ParseContentForType3Char(&unused);
  return glyph;
}

// What content that is never written again shows - Type3 glyph procedures,
// soft masks' groups, tiling patterns, and the forms they draw: the codes of
// each font, by the resources of the content selecting it, and the content
// with resources of its own, innermost first. Such content keeps its bytes;
// what its names resolve to can change.
class ContentUses {
 public:
  // A font, and the resources of the content selecting it: a Tf there looks
  // its name up in them (CPDF_Form::ChooseResourcesOfType()), and a Type3
  // font without /Resources draws its glyphs with them.
  using FontKey =
      std::pair<RetainPtr<CPDF_Font>, RetainPtr<const CPDF_Dictionary>>;
  struct Content {
    // What names it in resources: its stream, or a soft mask's graphics
    // state.
    RetainPtr<const CPDF_Object> key;
    RetainPtr<const CPDF_Stream> stream;
    // What its names fall back to: a form's page resources, the page's for
    // a soft mask's group; a tiling pattern's, none.
    RetainPtr<const CPDF_Dictionary> fallback;
    // For a soft mask an object draws: the name of its graphics state.
    ByteString name;

    bool IsSoftMask() const { return key.Get() != stream.Get(); }
  };

  ContentUses(CPDF_Document* document,
              RetainPtr<const CPDF_Dictionary> page_resources)
      : document_(document), page_resources_(std::move(page_resources)) {}

  // What `holder` shows and draws.
  void Add(const CPDF_PageObjectHolder* holder);

  // What `object` of `holder` draws through its soft mask and its pattern
  // colours, which it returns.
  std::vector<Content> AddDrawnContent(const CPDF_PageObject* object,
                                       const CPDF_PageObjectHolder* holder);

  // The codes shown, by font.
  const std::map<FontKey, std::set<uint32_t>>& fonts() const { return fonts_; }
  const std::vector<Content>& contents() const { return contents_; }

 private:
  void AddText(const CPDF_TextObject* text,
               const CPDF_PageObjectHolder* holder);
  void AddContent(Content content, const CPDF_Dictionary* page_resources);

  UnownedPtr<CPDF_Document> const document_;
  RetainPtr<const CPDF_Dictionary> const page_resources_;
  std::map<FontKey, std::set<uint32_t>> fonts_;
  std::vector<Content> contents_;
  std::set<const CPDF_Object*> listed_;
};

void ContentUses::Add(const CPDF_PageObjectHolder* holder) {
  for (const auto& object : *holder) {
    if (!object->IsActive()) {
      continue;
    }
    AddText(object->AsText(), holder);
    const CPDF_ClipPath& clip_path = object->clip_path();
    for (size_t i = 0; clip_path.HasRef() && i < clip_path.GetTextCount();
         ++i) {
      AddText(clip_path.GetText(i), holder);
    }
    AddDrawnContent(object.get(), holder);
    const CPDF_FormObject* form_object = object->AsForm();
    if (!form_object) {
      continue;
    }
    const CPDF_Form* form = form_object->form();
    Add(form);
    RetainPtr<const CPDF_Stream> stream = form->GetStream();
    if (stream && stream->GetDict()->KeyExist("Resources") &&
        listed_.insert(stream.Get()).second) {
      contents_.push_back({stream, stream, form->GetPageResources(), ""});
    }
  }
}

void ContentUses::AddText(const CPDF_TextObject* text,
                          const CPDF_PageObjectHolder* holder) {
  RetainPtr<CPDF_Font> font = text ? text->GetFont() : nullptr;
  if (!font) {
    return;
  }
  // Where its font was selected: here, or for a font a form inherits, in
  // the content around it. Text made other than by parsing was selected here.
  RetainPtr<const CPDF_Dictionary> context =
      text->text_state().HasRef() ? text->text_state().GetFontResources()
                                  : nullptr;
  if (!context) {
    context = holder->GetResources();
  }
  const std::vector<uint32_t>& text_codes = text->GetCharCodes();
  fonts_[{font, std::move(context)}].insert(text_codes.begin(),
                                            text_codes.end());
}

std::vector<ContentUses::Content> ContentUses::AddDrawnContent(
    const CPDF_PageObject* object,
    const CPDF_PageObjectHolder* holder) {
  std::vector<Content> drawn;
  // A soft mask's group reads its own resources, else the page's
  // (CPDF_RenderStatus::LoadSMask()).
  RetainPtr<const CPDF_Dictionary> states = CPDF_Form::ChooseResourcesOfType(
      holder->GetResources().Get(), holder->GetPageResources().Get(),
      "ExtGState");
  for (const ByteString& name : object->GetGraphicsResourceNames()) {
    RetainPtr<const CPDF_Dictionary> state =
        states ? states->GetDictFor(name.AsStringView()) : nullptr;
    RetainPtr<const CPDF_Stream> group = CPDF_ResourceUse::SoftMaskGroup(state);
    if (group) {
      drawn.push_back({state, group, page_resources_, name});
      AddContent(drawn.back(), page_resources_.Get());
    }
  }
  // A tiling pattern reads its own only (CPDF_TilingPattern::Load()).
  const CPDF_ColorState& colors = object->color_state();
  for (const CPDF_Color* color :
       {colors.HasRef() ? colors.GetFillColor() : nullptr,
        colors.HasRef() ? colors.GetStrokeColor() : nullptr}) {
    RetainPtr<CPDF_Pattern> pattern =
        color && color->IsPattern() ? color->GetPattern() : nullptr;
    RetainPtr<const CPDF_Stream> stream = CPDF_ResourceUse::TilingPatternStream(
        pattern ? pattern->pattern_obj() : nullptr);
    if (stream) {
      drawn.push_back({stream, stream, nullptr, ""});
      AddContent(drawn.back(), nullptr);
    }
  }
  return drawn;
}

void ContentUses::AddContent(Content content,
                             const CPDF_Dictionary* page_resources) {
  if (!listed_.insert(content.key.Get()).second) {
    return;
  }
  CPDF_Form form(
      document_,
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(page_resources)),
      pdfium::WrapRetain(const_cast<CPDF_Stream*>(content.stream.Get())));
  form.ParseContent();
  Add(&form);
  contents_.push_back(std::move(content));
}

// What content showing `codes` in a Type3 font sees of it. Per glyph, the
// font holds a procedure (which can draw anything), a name (in /Encoding
// /Differences and a descriptor's /CharSet: any word) and the text it stands
// for (/ToUnicode: any string). Only the codes shown keep theirs; widths
// stay.
class ShownGlyphs {
 public:
  ShownGlyphs(CPDF_Type3Font* font,
              const CPDF_Dictionary& dict,
              const std::set<uint32_t>& codes);

  // The names of the glyph procedures the codes shown draw.
  const std::set<ByteString>& kept_procs() const { return kept_procs_; }

  // Whether nothing of the font goes.
  bool IsWholeFont() const;

  // Fills `copy` with what is seen: the kept procedures (`proc_copies`
  // replacing some, by name) reading `resources`, the names and the text of
  // the codes shown.
  void Fill(CPDF_Dictionary* copy,
            CPDF_Document* document,
            RetainPtr<CPDF_Dictionary> resources,
            const std::map<ByteString, uint32_t>& proc_copies) const;

 private:
  RetainPtr<CPDF_Dictionary> KeptEncoding() const;

  RetainPtr<const CPDF_Dictionary> const dict_;
  RetainPtr<const CPDF_Dictionary> const procs_;
  RetainPtr<const CPDF_Array> const differences_;
  // The names of the codes shown, and those that have a procedure.
  std::set<ByteString> shown_names_;
  std::set<ByteString> kept_procs_;
  // The text of each code shown that has some; whether any other code has.
  std::map<uint8_t, WideString> shown_texts_;
  bool other_texts_ = false;
};

ShownGlyphs::ShownGlyphs(CPDF_Type3Font* font,
                         const CPDF_Dictionary& dict,
                         const std::set<uint32_t>& codes)
    : dict_(pdfium::WrapRetain(&dict)),
      procs_(dict.GetDictFor("CharProcs")),
      differences_([&dict]() -> RetainPtr<const CPDF_Array> {
        RetainPtr<const CPDF_Dictionary> encoding = dict.GetDictFor("Encoding");
        return encoding ? encoding->GetArrayFor("Differences") : nullptr;
      }()) {
  for (uint32_t code : codes) {
    const char* name = font->GetCharProcName(code);
    if (!name) {
      continue;
    }
    shown_names_.insert(name);
    if (procs_->KeyExist(name)) {
      kept_procs_.insert(name);
    }
  }
  if (RetainPtr<const CPDF_Stream> to_unicode =
          dict.GetStreamFor("ToUnicode")) {
    CPDF_ToUnicodeMap map(std::move(to_unicode));
    for (uint32_t code = 0; code < 256; ++code) {
      WideString text = map.Lookup(code);
      if (text.IsEmpty()) {
        continue;
      }
      if (pdfium::Contains(codes, code)) {
        shown_texts_[static_cast<uint8_t>(code)] = std::move(text);
      } else {
        other_texts_ = true;
      }
    }
  }
}

bool ShownGlyphs::IsWholeFont() const {
  if (kept_procs_.size() != procs_->size() || other_texts_) {
    return false;
  }
  RetainPtr<const CPDF_Dictionary> descriptor =
      dict_->GetDictFor("FontDescriptor");
  if (descriptor && descriptor->KeyExist("CharSet")) {
    return false;
  }
  if (!differences_) {
    return true;
  }
  CPDF_ArrayLocker locker(differences_);
  for (const auto& item : locker) {
    if (item->IsName() && !pdfium::Contains(shown_names_, item->GetString())) {
      return false;
    }
  }
  return true;
}

void ShownGlyphs::Fill(
    CPDF_Dictionary* copy,
    CPDF_Document* document,
    RetainPtr<CPDF_Dictionary> resources,
    const std::map<ByteString, uint32_t>& proc_copies) const {
  // Everything else is the font's, shared: its widths, its matrix and the
  // procedures it keeps.
  {
    CPDF_DictionaryLocker locker(dict_);
    for (const auto& [key, value] : locker) {
      copy->SetFor(key, value->Clone());
    }
  }
  RetainPtr<CPDF_Dictionary> procs =
      copy->SetNewFor<CPDF_Dictionary>("CharProcs");
  for (const ByteString& name : kept_procs_) {
    if (auto it = proc_copies.find(name); it != proc_copies.end()) {
      procs->SetNewFor<CPDF_Reference>(name, document, it->second);
    } else {
      procs->SetFor(name, procs_->GetObjectFor(name.AsStringView())->Clone());
    }
  }
  // The glyphs read these and nothing else: none of the content drawing
  // them.
  copy->SetFor("Resources", std::move(resources));
  if (RetainPtr<CPDF_Dictionary> encoding = KeptEncoding()) {
    copy->SetFor("Encoding", std::move(encoding));
  }
  if (dict_->KeyExist("ToUnicode")) {
    copy->RemoveFor("ToUnicode");
    if (!shown_texts_.empty()) {
      const ByteString cmap = CreateSimpleFontToUnicodeCMap(shown_texts_);
      pdfium::span<const uint8_t> bytes = cmap.unsigned_span();
      auto to_unicode = pdfium::MakeRetain<CPDF_Stream>(
          DataVector<uint8_t>(bytes.begin(), bytes.end()),
          pdfium::MakeRetain<CPDF_Dictionary>());
      copy->SetNewFor<CPDF_Reference>("ToUnicode", document,
                                      document->AddIndirectObject(to_unicode));
    }
  }
  if (RetainPtr<const CPDF_Dictionary> descriptor =
          dict_->GetDictFor("FontDescriptor");
      descriptor && descriptor->KeyExist("CharSet")) {
    RetainPtr<CPDF_Dictionary> kept_descriptor =
        ToDictionary(descriptor->Clone());
    kept_descriptor->RemoveFor("CharSet");
    copy->SetFor("FontDescriptor", std::move(kept_descriptor));
  }
}

RetainPtr<CPDF_Dictionary> ShownGlyphs::KeptEncoding() const {
  if (!differences_) {
    return nullptr;
  }
  // A code not shown draws nothing whatever it names: its name goes.
  RetainPtr<CPDF_Dictionary> encoding =
      ToDictionary(dict_->GetDictFor("Encoding")->Clone());
  RetainPtr<CPDF_Array> differences =
      encoding->SetNewFor<CPDF_Array>("Differences");
  CPDF_ArrayLocker locker(differences_);
  for (const auto& item : locker) {
    if (item->IsName() && !pdfium::Contains(shown_names_, item->GetString())) {
      differences->AppendNew<CPDF_Name>(".notdef");
    } else {
      differences->Append(item->Clone());
    }
  }
  return encoding;
}

class Type3Subsetter {
 public:
  Type3Subsetter(CPDF_Document* document,
                 RetainPtr<const CPDF_Dictionary> page_resources)
      : document_(document), page_resources_(std::move(page_resources)) {}

  // Gives the text of `holder`, and of the forms it draws, copies of the
  // Type3 fonts they show holding only what each shows, and its soft masks
  // and pattern colours copies drawing those; writes the forms that changed.
  // Returns whether `holder`'s own objects or resources changed, or its own
  // resources can lead to a Type3 font: its content is to be written again,
  // naming only what it uses.
  bool SubsetHolder(CPDF_PageObjectHolder* holder);

  // Writes `form`'s content again, into a private copy as redaction writes
  // a form it changed: another placement keeps what it draws. Returns the
  // stream written, or null when it could not be copied.
  RetainPtr<const CPDF_Stream> WriteForm(CPDF_Form* form);

 private:
  // A font as content reading a context shows some of its codes. The context
  // matters only to a font whose glyphs read it (no /Resources of its own).
  using Key = std::
      tuple<const CPDF_Dictionary*, std::set<uint32_t>, const CPDF_Dictionary*>;

  // Resources a copy owns (OwnResources()): whether a name it uses names a
  // replacement, and whether the content's own resources named anything it
  // doesn't use.
  struct Owned {
    RetainPtr<CPDF_Dictionary> resources;
    bool replaced = false;
    bool dropped = false;
  };
  using Walker = std::function<void(CPDF_ResourceUse&, const CPDF_Dictionary*)>;

  void AddCodes(const CPDF_TextObject* text, CodesByFont& codes) const;

  // The font dictionary content reading `context` draws `font` with when it
  // shows `codes`: a copy holding only what those codes draw, or `font`'s own
  // when that is all of it. In a copy the glyphs read resources of the
  // copy's own: what they use, the fonts they draw in turn copies as they
  // show them, the content they draw - forms, soft masks, tiling patterns -
  // copied when its resources change. A glyph drawing its own font, with the
  // same codes, draws the copy.
  RetainPtr<const CPDF_Dictionary> ShownFontDict(
      CPDF_Font* font,
      const std::set<uint32_t>& codes,
      const CPDF_Dictionary* context);

  // Replacements for what `uses` saw: each font by ShownFontDict(), for the
  // resources naming it; each content with resources of its own by a copy,
  // when those change.
  Replacements ReplaceUses(const ContentUses& uses);

  // `content`'s copy reading `resources`, the object naming it replaced: its
  // stream, or a soft mask's graphics state and mask.
  uint32_t CopyContent(const ContentUses::Content& content,
                       RetainPtr<CPDF_Dictionary> resources);

  // What content reads, as one dictionary of its own: from `own`, and for a
  // type `own` lacks from `fallback` (CPDF_Form::ChooseResourcesOfType()),
  // each object replaced as `replacements` say for content reading
  // `context`. `walk` walks the content against that dictionary - with the
  // replacements in place, so what only an original read goes - and only the
  // names it uses stay. Nothing is left to fall back on.
  Owned OwnResources(const CPDF_Dictionary* own,
                     const CPDF_Dictionary* fallback,
                     const CPDF_Dictionary* context,
                     const Replacements& replacements,
                     const Walker& walk);

  // Gives `object`'s pattern colours painting with `original` the pattern
  // `copy`, the same colour otherwise.
  void SwapPattern(CPDF_PageObject* object,
                   const CPDF_Object* original,
                   uint32_t copy);

  // Whether `object`'s graph - dictionaries, arrays, streams' dictionaries -
  // holds a Type3 font: what content can name, it can draw.
  bool ReachesType3(const CPDF_Object* object);

  // Whether what `uses` saw shows text in a Type3 font, or can name one.
  bool DrawsType3(const ContentUses& uses);

  RetainPtr<CPDF_Font> FontOf(const CPDF_Dictionary* dict) const;

  UnownedPtr<CPDF_Document> const document_;
  RetainPtr<const CPDF_Dictionary> const page_resources_;
  std::map<Key, RetainPtr<const CPDF_Dictionary>> shown_;
  // The copies being made; one a glyph drawing its own font asked for
  // before it is filled.
  std::map<Key, RetainPtr<CPDF_Dictionary>> making_;
  std::map<const CPDF_Object*, bool> reaches_type3_;
  // Each content's copy (0: none), by what it falls back on.
  std::map<std::pair<const CPDF_Object*, const CPDF_Dictionary*>, uint32_t>
      content_copies_;
};

void Type3Subsetter::AddCodes(const CPDF_TextObject* text,
                              CodesByFont& codes) const {
  RetainPtr<CPDF_Font> font = text ? text->GetFont() : nullptr;
  if (!font || !font->IsType3Font()) {
    return;
  }
  const std::vector<uint32_t>& text_codes = text->GetCharCodes();
  codes[font.Get()].insert(text_codes.begin(), text_codes.end());
}

bool Type3Subsetter::SubsetHolder(CPDF_PageObjectHolder* holder) {
  bool changed = false;
  CodesByFont codes;
  const CPDF_Dictionary* content_resources = holder->GetResources().Get();
  for (const auto& object : *holder) {
    if (!object->IsActive()) {
      continue;
    }
    if (CPDF_FormObject* form_object = object->AsForm()) {
      CPDF_Form* form = form_object->form();
      const bool written = SubsetHolder(form) && WriteForm(form);
      if (written) {
        form_object->SetDirty(true);
        changed = true;
      }
      // The font the form inherits is written before it is drawn
      // (CPDF_PageContentGenerator::WriteInheritedTextState()). Once the form
      // draws with its own fonts - written again, or never showing that one
      // - inheriting none keeps the whole font off the page.
      const CPDF_TextState& inherited = form_object->text_state();
      RetainPtr<CPDF_Font> inherited_font =
          inherited.HasRef() ? inherited.GetFont() : nullptr;
      if (inherited_font && inherited_font->IsType3Font() &&
          (written || !ShowsTextIn(form, inherited_font.Get()))) {
        form_object->mutable_text_state().SetFont(nullptr);
        form_object->SetDirty(true);
        changed = true;
      }
    }
    // What it draws through a soft mask or a pattern colour: content never
    // written again, so the object draws a copy of it instead.
    ContentUses drawn_uses(document_, page_resources_);
    const std::vector<ContentUses::Content> drawn =
        drawn_uses.AddDrawnContent(object.get(), holder);
    if (!drawn.empty() && DrawsType3(drawn_uses)) {
      const Replacements replacements = ReplaceUses(drawn_uses);
      for (const ContentUses::Content& content : drawn) {
        auto it = replacements.contents.find(content.key.Get());
        if (it == replacements.contents.end()) {
          continue;
        }
        // A graphics state is written by its name; a pattern colour is
        // written with the pattern it holds.
        if (content.IsSoftMask()) {
          holder->ReplaceResource("ExtGState", content.name, it->second);
        } else {
          SwapPattern(object.get(), content.stream.Get(), it->second);
        }
        changed = true;
      }
    }
    AddCodes(object->AsText(), codes);
    const CPDF_ClipPath& clip_path = object->clip_path();
    for (size_t i = 0; clip_path.HasRef() && i < clip_path.GetTextCount();
         ++i) {
      AddCodes(clip_path.GetText(i), codes);
    }
  }

  for (const auto& [font, font_codes] : codes) {
    RetainPtr<const CPDF_Dictionary> dict =
        ShownFontDict(font, font_codes, content_resources);
    RetainPtr<CPDF_Font> subset =
        dict != font->GetFontDict() ? FontOf(dict.Get()) : nullptr;
    if (!subset) {
      continue;
    }
    auto uses_font = [font](const CPDF_TextObject* text) {
      return text && text->GetFont().Get() == font;
    };
    for (const auto& object : *holder) {
      bool swapped = false;
      if (CPDF_TextObject* text = object->AsText(); uses_font(text)) {
        text->mutable_text_state().SetFont(subset);
        swapped = true;
      }
      const CPDF_ClipPath& clip_path = object->clip_path();
      bool clip_uses_font = false;
      for (size_t i = 0; clip_path.HasRef() && i < clip_path.GetTextCount();
           ++i) {
        clip_uses_font = clip_uses_font || uses_font(clip_path.GetText(i));
      }
      if (clip_uses_font) {
        object->mutable_clip_path().EditTexts([&](CPDF_TextObject* text) {
          if (uses_font(text)) {
            text->mutable_text_state().SetFont(subset);
          }
          return true;
        });
        swapped = true;
      }
      if (swapped) {
        object->SetDirty(true);
      }
    }
    changed = true;
  }
  // Written again, a form's resources keep only the names its content uses:
  // a name it doesn't use can lead to a Type3 font all the same.
  if (!holder->IsPage()) {
    RetainPtr<const CPDF_Stream> stream =
        static_cast<CPDF_Form*>(holder)->GetStream();
    RetainPtr<const CPDF_Dictionary> own =
        stream ? stream->GetDict()->GetDictFor("Resources") : nullptr;
    if (own && ReachesType3(own.Get())) {
      changed = true;
    }
  }
  return changed;
}

RetainPtr<const CPDF_Stream> Type3Subsetter::WriteForm(CPDF_Form* form) {
  if (!form->CloneBackingStreamForWrite()) {
    return nullptr;
  }
  // The copy holds the redacted drawing, and none of the original's other
  // copies of it.
  RemoveContentCopies(form->GetMutableDict().Get());
  // Written in full, its resources keep only the names it uses.
  form->MarkStreamDirty(0);
  CPDF_PageContentGenerator(form).GenerateContent();
  return form->GetStream();
}

RetainPtr<const CPDF_Dictionary> Type3Subsetter::ShownFontDict(
    CPDF_Font* font,
    const std::set<uint32_t>& codes,
    const CPDF_Dictionary* context) {
  RetainPtr<const CPDF_Dictionary> dict = font->GetFontDict();
  CPDF_Type3Font* type3 = font->AsType3Font();
  RetainPtr<const CPDF_Dictionary> procs =
      type3 && dict ? dict->GetDictFor("CharProcs") : nullptr;
  if (!procs) {
    return dict;
  }
  const bool borrows = CPDF_Type3Font::GlyphsUseContentResources(dict.Get());
  RetainPtr<const CPDF_Dictionary> glyph_resources =
      borrows ? pdfium::WrapRetain(context) : dict->GetDictFor("Resources");
  const Key key(dict.Get(), codes, borrows ? context : nullptr);
  if (auto it = shown_.find(key); it != shown_.end()) {
    return it->second;
  }
  if (auto it = making_.find(key); it != making_.end()) {
    if (!it->second) {
      it->second = pdfium::MakeRetain<CPDF_Dictionary>();
      document_->AddIndirectObject(it->second);
    }
    return it->second;
  }
  making_[key] = nullptr;

  // What the kept glyphs draw.
  ShownGlyphs shown(type3, *dict, codes);
  std::vector<std::pair<ByteString, RetainPtr<const CPDF_Stream>>> kept;
  ContentUses uses(document_, page_resources_);
  for (const ByteString& name : shown.kept_procs()) {
    RetainPtr<const CPDF_Stream> proc =
        ToStream(procs->GetDirectObjectFor(name.AsStringView()));
    if (!proc) {
      continue;
    }
    uses.Add(ParseGlyph(document_, glyph_resources.Get(), proc.Get()).get());
    kept.emplace_back(name, std::move(proc));
  }
  const Replacements replacements = ReplaceUses(uses);

  // A glyph with resources of its own reads them, and for a type they lack
  // the glyphs' resources: it gets a copy owning what it uses when that
  // changes.
  std::map<ByteString, uint32_t> proc_copies;
  std::map<ByteString, RetainPtr<const CPDF_Dictionary>> proc_reads;
  for (const auto& [name, proc] : kept) {
    RetainPtr<const CPDF_Dictionary> own =
        proc->GetDict()->GetDictFor("Resources");
    if (!own) {
      continue;
    }
    Owned owned = OwnResources(
        own.Get(), glyph_resources.Get(), own.Get(), replacements,
        [&](CPDF_ResourceUse& use, const CPDF_Dictionary* reachable) {
          use.Walk(proc.Get(), reachable, reachable);
        });
    if (!owned.replaced && !owned.dropped) {
      continue;
    }
    proc_reads[name] = owned.resources;
    RetainPtr<CPDF_Stream> proc_copy = CopyWithoutContentCopies(proc.Get());
    proc_copy->GetMutableDict()->SetFor("Resources",
                                        std::move(owned.resources));
    proc_copies[name] = document_->AddIndirectObject(proc_copy);
  }
  // The resources the kept glyphs read, the copy's own.
  Owned resources = OwnResources(
      glyph_resources.Get(), nullptr, glyph_resources.Get(), replacements,
      [&](CPDF_ResourceUse& use, const CPDF_Dictionary* reachable) {
        for (const auto& [name, proc] : kept) {
          if (auto it = proc_reads.find(name); it != proc_reads.end()) {
            use.Walk(proc.Get(), it->second.Get(), reachable);
            continue;
          }
          RetainPtr<const CPDF_Dictionary> own =
              proc->GetDict()->GetDictFor("Resources");
          use.Walk(
              proc.Get(),
              CPDF_Form::ChooseResourcesDict(own.Get(), nullptr, reachable),
              reachable);
        }
      });

  RetainPtr<CPDF_Dictionary> copy = std::move(making_[key]);
  making_.erase(key);
  // Glyphs reading the content's resources use only some of them: owning
  // just those changes nothing.
  const bool changed = copy || !shown.IsWholeFont() || resources.replaced ||
                       (resources.dropped && !borrows) || !proc_copies.empty();
  if (!changed) {
    shown_[key] = dict;
    return dict;
  }
  // A kept glyph of a font copied here is part of the copy, so it holds no
  // other copy of what the font drew, whether or not its resources changed.
  for (const auto& [name, proc] : kept) {
    if (!proc_copies.contains(name) &&
        HasContentCopies(proc->GetDict().Get())) {
      proc_copies[name] =
          document_->AddIndirectObject(CopyWithoutContentCopies(proc.Get()));
    }
  }
  if (!copy) {
    copy = pdfium::MakeRetain<CPDF_Dictionary>();
    document_->AddIndirectObject(copy);
  }
  shown.Fill(copy.Get(), document_, std::move(resources.resources),
             proc_copies);
  shown_[key] = copy;
  return copy;
}

Replacements Type3Subsetter::ReplaceUses(const ContentUses& uses) {
  Replacements replacements;
  for (const auto& [key, codes] : uses.fonts()) {
    const auto& [font, context] = key;
    RetainPtr<const CPDF_Dictionary> dict = font->GetFontDict();
    RetainPtr<const CPDF_Dictionary> shown =
        ShownFontDict(font.Get(), codes, context.Get());
    if (!dict) {
      continue;
    }
    replacements.shown_fonts.emplace(context.Get(), dict.Get());
    if (shown && shown != dict) {
      replacements.fonts[{context.Get(), dict.Get()}] = shown->GetObjNum();
    }
  }
  // Innermost first: content drawing other content reads their copies.
  for (const ContentUses::Content& content : uses.contents()) {
    const auto memo = std::make_pair(content.key.Get(), content.fallback.Get());
    auto known = content_copies_.find(memo);
    if (known == content_copies_.end()) {
      // It reads its own resources, else - a soft mask's group - the page's.
      RetainPtr<const CPDF_Dictionary> own =
          content.stream->GetDict()->GetDictFor("Resources");
      RetainPtr<const CPDF_Dictionary> context = own ? own : content.fallback;
      uint32_t copy = 0;
      if (context) {
        Owned owned = OwnResources(
            own.Get(), content.fallback.Get(), context.Get(), replacements,
            [&](CPDF_ResourceUse& use, const CPDF_Dictionary* reachable) {
              use.Walk(content.stream.Get(), reachable, reachable);
            });
        if (owned.replaced || owned.dropped) {
          copy = CopyContent(content, std::move(owned.resources));
        }
      }
      known = content_copies_.emplace(memo, copy).first;
    }
    if (known->second) {
      replacements.contents[content.key.Get()] = known->second;
    }
  }
  return replacements;
}

uint32_t Type3Subsetter::CopyContent(const ContentUses::Content& content,
                                     RetainPtr<CPDF_Dictionary> resources) {
  RetainPtr<CPDF_Stream> stream =
      CopyWithoutContentCopies(content.stream.Get());
  stream->GetMutableDict()->SetFor("Resources", std::move(resources));
  const uint32_t stream_objnum = document_->AddIndirectObject(stream);
  if (!content.IsSoftMask()) {
    return stream_objnum;
  }
  // A soft mask's graphics state: its mask draws the copy.
  RetainPtr<const CPDF_Dictionary> state = ToDictionary(content.key);
  RetainPtr<CPDF_Dictionary> mask =
      ToDictionary(state->GetDictFor("SMask")->Clone());
  mask->SetNewFor<CPDF_Reference>("G", document_, stream_objnum);
  RetainPtr<CPDF_Dictionary> state_copy = ToDictionary(state->Clone());
  state_copy->SetFor("SMask", std::move(mask));
  return document_->AddIndirectObject(state_copy);
}

Type3Subsetter::Owned Type3Subsetter::OwnResources(
    const CPDF_Dictionary* own,
    const CPDF_Dictionary* fallback,
    const CPDF_Dictionary* context,
    const Replacements& replacements,
    const Walker& walk) {
  // Every name the content can look up, a type from `own` when it has it.
  auto reachable = pdfium::MakeRetain<CPDF_Dictionary>();
  std::set<std::pair<ByteString, ByteString>> replaced;
  for (const CPDF_Dictionary* source : {fallback, own}) {
    if (!source) {
      continue;
    }
    CPDF_DictionaryLocker locker(source);
    for (const auto& [type, value] : locker) {
      RetainPtr<const CPDF_Dictionary> names = ToDictionary(value->GetDirect());
      if (!names || !pdfium::Contains(CPDF_ResourceUse::kTypes, type)) {
        reachable->SetFor(type, value->Clone());
        continue;
      }
      RetainPtr<CPDF_Dictionary> copied =
          reachable->SetNewFor<CPDF_Dictionary>(type);
      CPDF_DictionaryLocker names_locker(names);
      for (const auto& [name, entry] : names_locker) {
        RetainPtr<const CPDF_Object> direct = entry->GetDirect();
        std::optional<uint32_t> replacement =
            direct ? replacements.Find(context, type, direct.Get())
                   : std::nullopt;
        if (replacement.has_value()) {
          copied->SetNewFor<CPDF_Reference>(name, document_.get(),
                                            replacement.value());
          replaced.emplace(type, name);
        } else {
          copied->SetFor(name, entry->Clone());
        }
      }
    }
  }
  // A Type3 font the content selects (Tf) without showing text in holds no
  // glyph: its name points at such a copy before the walk that counts what
  // the content uses, so the original's glyphs count for nothing.
  {
    CPDF_ResourceUse selected(document_, reachable);
    selected.SetPageResources(page_resources_);
    walk(selected, reachable.Get());
    RetainPtr<CPDF_Dictionary> fonts = reachable->GetMutableDictFor("Font");
    auto used = selected.used().find("Font");
    for (const ByteString& name : fonts&& used != selected.used().end()
                                      ? used->second
                                      : std::set<ByteString>()) {
      RetainPtr<const CPDF_Dictionary> dict =
          ToDictionary(fonts->GetDirectObjectFor(name.AsStringView()));
      if (!dict ||
          pdfium::Contains(replaced,
                           std::make_pair(ByteString("Font"), name)) ||
          pdfium::Contains(
              replacements.shown_fonts,
              std::make_pair(context,
                             static_cast<const CPDF_Object*>(dict.Get())))) {
        continue;
      }
      RetainPtr<CPDF_Font> font = FontOf(dict.Get());
      RetainPtr<const CPDF_Dictionary> empty =
          font && font->IsType3Font() ? ShownFontDict(font.Get(), {}, context)
                                      : nullptr;
      if (empty && empty != dict) {
        fonts->SetNewFor<CPDF_Reference>(name, document_.get(),
                                         empty->GetObjNum());
        replaced.emplace("Font", name);
      }
    }
  }
  CPDF_ResourceUse use(document_, reachable);
  use.SetPageResources(page_resources_);
  walk(use, reachable.Get());

  // Only what it uses.
  Owned owned;
  owned.resources = pdfium::MakeRetain<CPDF_Dictionary>();
  CPDF_DictionaryLocker locker(reachable);
  for (const auto& [type, value] : locker) {
    const CPDF_Dictionary* names = value->AsDictionary();
    if (!names || !pdfium::Contains(CPDF_ResourceUse::kTypes, type)) {
      owned.resources->SetFor(type, value->Clone());
      continue;
    }
    auto used = use.used().find(type);
    if (used == use.used().end()) {
      continue;
    }
    RetainPtr<CPDF_Dictionary> kept;
    CPDF_DictionaryLocker names_locker(names);
    for (const auto& [name, entry] : names_locker) {
      if (!pdfium::Contains(used->second, name)) {
        continue;
      }
      if (!kept) {
        kept = owned.resources->SetNewFor<CPDF_Dictionary>(type);
      }
      owned.replaced = owned.replaced ||
                       pdfium::Contains(replaced, std::make_pair(type, name));
      kept->SetFor(name, entry->Clone());
    }
  }
  // What the content's own resources name that it doesn't use.
  for (const char* type : CPDF_ResourceUse::kTypes) {
    RetainPtr<const CPDF_Dictionary> names =
        own ? own->GetDictFor(type) : nullptr;
    if (!names) {
      continue;
    }
    auto used = use.used().find(type);
    CPDF_DictionaryLocker names_locker(names);
    for (const auto& [name, entry] : names_locker) {
      if (used == use.used().end() || !pdfium::Contains(used->second, name)) {
        owned.dropped = true;
      }
    }
  }
  return owned;
}

void Type3Subsetter::SwapPattern(CPDF_PageObject* object,
                                 const CPDF_Object* original,
                                 uint32_t copy) {
  RetainPtr<CPDF_Object> copy_object =
      document_->GetMutableIndirectObject(copy);
  CPDF_ColorState& colors = object->mutable_color_state();
  for (bool stroke : {false, true}) {
    const CPDF_Color* color =
        stroke ? colors.GetStrokeColor() : colors.GetFillColor();
    RetainPtr<CPDF_Pattern> pattern =
        color && color->IsPattern() ? color->GetPattern() : nullptr;
    if (!pattern || pattern->pattern_obj().Get() != original) {
      continue;
    }
    RetainPtr<CPDF_Pattern> copied =
        CPDF_DocPageData::FromDocument(document_)->GetPattern(
            copy_object, pattern->parent_matrix());
    if (!copied) {
      continue;
    }
    pdfium::span<const float> comps = color->GetPatternComps();
    std::vector<float> values(comps.begin(), comps.end());
    if (stroke) {
      colors.SetStrokePattern(std::move(copied), values);
    } else {
      colors.SetFillPattern(std::move(copied), values);
    }
  }
  object->SetDirty(true);
}

bool Type3Subsetter::ReachesType3(const CPDF_Object* object) {
  if (auto it = reaches_type3_.find(object); it != reaches_type3_.end()) {
    return it->second;
  }
  std::set<const CPDF_Object*> seen;
  std::vector<RetainPtr<const CPDF_Object>> pending{pdfium::WrapRetain(object)};
  bool reaches = false;
  while (!pending.empty() && !reaches) {
    RetainPtr<const CPDF_Object> current =
        pending.back() ? pending.back()->GetDirect() : nullptr;
    pending.pop_back();
    if (!current || !seen.insert(current.Get()).second) {
      continue;
    }
    if (const CPDF_Stream* stream = current->AsStream()) {
      pending.push_back(stream->GetDict());
    } else if (const CPDF_Dictionary* dict = current->AsDictionary()) {
      if (dict->GetNameFor("Subtype") == "Type3") {
        reaches = true;
        break;
      }
      CPDF_DictionaryLocker locker(dict);
      for (const auto& [key, value] : locker) {
        // Not back up to a page or an annotation.
        if (key != "Parent" && key != "P") {
          pending.push_back(value);
        }
      }
    } else if (const CPDF_Array* array = current->AsArray()) {
      CPDF_ArrayLocker locker(array);
      for (const auto& value : locker) {
        pending.push_back(value);
      }
    }
  }
  reaches_type3_[object] = reaches;
  return reaches;
}

bool Type3Subsetter::DrawsType3(const ContentUses& uses) {
  for (const auto& [key, codes] : uses.fonts()) {
    if (key.first->IsType3Font()) {
      return true;
    }
  }
  for (const ContentUses::Content& content : uses.contents()) {
    RetainPtr<const CPDF_Dictionary> own =
        content.stream->GetDict()->GetDictFor("Resources");
    // A soft mask's group without resources of its own names the page's.
    if (own ? ReachesType3(own.Get()) : content.IsSoftMask()) {
      return true;
    }
  }
  return false;
}

RetainPtr<CPDF_Font> Type3Subsetter::FontOf(const CPDF_Dictionary* dict) const {
  if (!dict) {
    return nullptr;
  }
  return CPDF_DocPageData::FromDocument(document_)->GetFont(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(dict)));
}

// Gives the appearances of the annotations on `page` copies of the fonts
// holding only the glyphs each shows, writing each that changed into a copy
// of its own. An /AP dictionary, or a dictionary of appearances by state, can
// be shared with annotations elsewhere: the annotation gets its own before
// pointing at the copy. Returns whether any changed.
bool SubsetAppearances(CPDF_Page* page, Type3Subsetter& subsetter) {
  CPDF_Document* document = page->GetDocument();
  RetainPtr<CPDF_Array> annots =
      page->GetMutableDict()->GetMutableArrayFor("Annots");
  // Each appearance stream written, by the stream it replaces: one stream
  // drawn in several places on the page is written once.
  std::map<const CPDF_Stream*, uint32_t> written;
  auto write = [&](RetainPtr<CPDF_Stream> stream) -> uint32_t {
    if (auto it = written.find(stream.Get()); it != written.end()) {
      return it->second;
    }
    CPDF_Form form(document, page->GetMutableResources(), stream);
    form.ParseContent();
    RetainPtr<const CPDF_Stream> copy;
    if (subsetter.SubsetHolder(&form)) {
      copy = subsetter.WriteForm(&form);
    }
    const uint32_t objnum = copy ? copy->GetObjNum() : 0;
    written[stream.Get()] = objnum;
    return objnum;
  };

  bool changed = false;
  for (size_t i = 0; annots && i < annots->size(); ++i) {
    RetainPtr<CPDF_Dictionary> annot = annots->GetMutableDictAt(i);
    RetainPtr<CPDF_Dictionary> ap =
        annot ? annot->GetMutableDictFor("AP") : nullptr;
    if (!ap) {
      continue;
    }
    // The annotation's own /AP, made on the first replacement.
    RetainPtr<CPDF_Dictionary> own_ap;
    for (const char* mode : {"N", "R", "D"}) {
      // An appearance is a stream, or a dictionary of them by state.
      if (RetainPtr<CPDF_Stream> stream = ap->GetMutableStreamFor(mode)) {
        const uint32_t objnum = write(std::move(stream));
        if (objnum) {
          if (!own_ap) {
            own_ap = ToDictionary(ap->Clone());
          }
          own_ap->SetNewFor<CPDF_Reference>(mode, document, objnum);
        }
        continue;
      }
      RetainPtr<CPDF_Dictionary> states = ap->GetMutableDictFor(mode);
      if (!states) {
        continue;
      }
      RetainPtr<CPDF_Dictionary> own_states;
      for (const ByteString& state : states->GetKeys()) {
        RetainPtr<CPDF_Stream> stream =
            states->GetMutableStreamFor(state.AsStringView());
        const uint32_t objnum = stream ? write(std::move(stream)) : 0;
        if (!objnum) {
          continue;
        }
        if (!own_states) {
          own_states = ToDictionary(states->Clone());
        }
        own_states->SetNewFor<CPDF_Reference>(state, document, objnum);
      }
      if (own_states) {
        if (!own_ap) {
          own_ap = ToDictionary(ap->Clone());
        }
        own_ap->SetFor(mode, std::move(own_states));
      }
    }
    if (own_ap) {
      annot->SetFor("AP", std::move(own_ap));
      changed = true;
    }
  }
  return changed;
}

}  // namespace

bool RedactType3Glyphs(CPDF_Page* page,
                       const std::set<RetainPtr<CPDF_Font>>& fonts) {
  if (!page || fonts.empty()) {
    return false;
  }
  // Reading through the base must see what a layer changed, as the content
  // generator does.
  CPDF_DocumentViewScope document_view(page->GetDocument());
  Type3Subsetter subsetter(page->GetDocument(), page->GetResources());
  const bool page_changed = subsetter.SubsetHolder(page);
  const bool appearances_changed = SubsetAppearances(page, subsetter);
  return page_changed || appearances_changed;
}
