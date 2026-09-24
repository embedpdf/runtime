// Copyright 2014 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "public/fpdfview.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "build/build_config.h"
#include "constants/page_object.h"
#include "core/fpdfapi/page/cpdf_annotcontext.h"
#include "core/fpdfapi/page/cpdf_docpagedata.h"
#include "core/fpdfapi/page/cpdf_form.h"
#include "core/fpdfapi/page/cpdf_occontext.h"
#include "core/fpdfapi/page/cpdf_page.h"
#include "core/fpdfapi/page/cpdf_pageimagecache.h"
#include "core/fpdfapi/page/cpdf_pagemodule.h"
#include "core/fpdfapi/parser/cpdf_array.h"
#include "core/fpdfapi/parser/cpdf_boolean.h"
#include "core/fpdfapi/parser/cpdf_dictionary.h"
#include "core/fpdfapi/parser/cpdf_document.h"
#include "core/fpdfapi/parser/cpdf_document_view_scope.h"
#include "core/fpdfapi/parser/cpdf_name.h"
#include "core/fpdfapi/parser/cpdf_number.h"
#include "core/fpdfapi/parser/cpdf_parser.h"
#include "core/fpdfapi/parser/cpdf_security_handler.h"
#include "core/fpdfapi/parser/cpdf_stream.h"
#include "core/fpdfapi/parser/cpdf_string.h"
#include "core/fpdfapi/parser/fpdf_parser_decode.h"
#include "core/fpdfapi/render/cpdf_docrenderdata.h"
#include "core/fpdfapi/render/cpdf_pagerendercontext.h"
#include "core/fpdfapi/render/cpdf_rendercontext.h"
#include "core/fpdfapi/render/cpdf_renderoptions.h"
#include "core/fpdfdoc/cpdf_annot.h"
#include "core/fpdfdoc/cpdf_nametree.h"
#include "core/fpdfdoc/cpdf_viewerpreferences.h"
#include "core/fxcrt/cfx_fileaccess_stream.h"
#include "core/fxcrt/cfx_read_only_span_stream.h"
#include "core/fxcrt/cfx_timer.h"
#include "core/fxcrt/check_op.h"
#include "core/fxcrt/compiler_specific.h"
#include "core/fxcrt/epdf_tls.h"
#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_memcpy_wrappers.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/fx_system.h"
#include "core/fxcrt/numerics/safe_conversions.h"
#include "core/fxcrt/ptr_util.h"
#include "core/fxcrt/span.h"
#include "core/fxcrt/stl_util.h"
#include "core/fxcrt/unowned_ptr.h"
#include "core/fxge/cfx_defaultrenderdevice.h"
#include "core/fxge/cfx_fontregistry.h"
#include "core/fxge/cfx_gemodule.h"
#include "core/fxge/cfx_glyphcache.h"
#include "core/fxge/cfx_renderdevice.h"
#include "core/fxge/dib/cfx_dibitmap.h"
#include "fpdfsdk/cpdfsdk_customaccess.h"
#include "fpdfsdk/cpdfsdk_formfillenvironment.h"
#include "fpdfsdk/cpdfsdk_helpers.h"
#include "fpdfsdk/cpdfsdk_pageview.h"
#include "fpdfsdk/cpdfsdk_renderpage.h"
#include "fpdfsdk/epdf_wrapped_appearance.h"
#include "fxjs/ijs_runtime.h"
#include "public/epdf_named_pages.h"
#include "public/fpdf_formfill.h"

#ifdef PDF_ENABLE_V8
#include "fxjs/cfx_v8_array_buffer_allocator.h"
#endif

#ifdef PDF_ENABLE_XFA
#include "fpdfsdk/fpdfxfa/cpdfxfa_context.h"
#include "fpdfsdk/fpdfxfa/cpdfxfa_page.h"
#endif  // PDF_ENABLE_XFA

#if BUILDFLAG(IS_WIN)
#include "core/fpdfapi/render/cpdf_progressiverenderer.h"
#include "core/fpdfapi/render/cpdf_windowsrenderdevice.h"
#include "public/fpdf_edit.h"

#if defined(PDF_USE_SKIA)
class SkCanvas;
#endif  // defined(PDF_USE_SKIA)

// These checks are here because core/ and public/ cannot depend on each other.
static_assert(static_cast<int>(WindowsPrintMode::kEmf) == FPDF_PRINTMODE_EMF);
static_assert(static_cast<int>(WindowsPrintMode::kTextOnly) ==
              FPDF_PRINTMODE_TEXTONLY);
static_assert(static_cast<int>(WindowsPrintMode::kPostScript2) ==
              FPDF_PRINTMODE_POSTSCRIPT2);
static_assert(static_cast<int>(WindowsPrintMode::kPostScript3) ==
              FPDF_PRINTMODE_POSTSCRIPT3);
static_assert(static_cast<int>(WindowsPrintMode::kPostScript2PassThrough) ==
              FPDF_PRINTMODE_POSTSCRIPT2_PASSTHROUGH);
static_assert(static_cast<int>(WindowsPrintMode::kPostScript3PassThrough) ==
              FPDF_PRINTMODE_POSTSCRIPT3_PASSTHROUGH);
static_assert(static_cast<int>(WindowsPrintMode::kEmfImageMasks) ==
              FPDF_PRINTMODE_EMF_IMAGE_MASKS);
static_assert(static_cast<int>(WindowsPrintMode::kPostScript3Type42) ==
              FPDF_PRINTMODE_POSTSCRIPT3_TYPE42);
static_assert(
    static_cast<int>(WindowsPrintMode::kPostScript3Type42PassThrough) ==
    FPDF_PRINTMODE_POSTSCRIPT3_TYPE42_PASSTHROUGH);
#endif  // BUILDFLAG(IS_WIN)

#if defined(PDF_USE_SKIA)
// These checks are here because core/ and public/ cannot depend on each other.
static_assert(static_cast<int>(CFX_DefaultRenderDevice::RendererType::kAgg) ==
              FPDF_RENDERERTYPE_AGG);
static_assert(static_cast<int>(CFX_DefaultRenderDevice::RendererType::kSkia) ==
              FPDF_RENDERERTYPE_SKIA);
#endif  // defined(PDF_USE_SKIA)

namespace {

// EmbedPDF: thread-confined runtime - each worker thread tracks its own
// library-initialized state so per-thread Init/Destroy don't race.
EPDF_TLS bool g_bLibraryInitialized = false;

void SetRendererType(FPDF_RENDERER_TYPE public_type) {
  // Internal definition of renderer types must stay updated with respect to
  // the public definition, such that all public definitions can be mapped to
  // an internal definition in `CFX_DefaultRenderDevice`. A public definition
  // value might not be meaningful for a particular build configuration, which
  // would mean use of that value is an error for that build.

  // AGG is always present in a build. `FPDF_RENDERERTYPE_SKIA` is valid to use
  // only if it is included in the build.
#if defined(PDF_USE_SKIA)
  // This build configuration has the option for runtime renderer selection.
  CHECK(public_type == FPDF_RENDERERTYPE_AGG ||
        public_type == FPDF_RENDERERTYPE_SKIA);
  CFX_DefaultRenderDevice::SetRendererType(
      static_cast<CFX_DefaultRenderDevice::RendererType>(public_type));
#else
  // AGG-only builds should always use `FPDF_RENDERERTYPE_AGG`.
  CHECK_EQ(public_type, FPDF_RENDERERTYPE_AGG);
#endif
}

void ResetRendererType() {
#if defined(PDF_USE_SKIA)
  CFX_DefaultRenderDevice::SetRendererType(
      CFX_DefaultRenderDevice::kDefaultRenderer);
#endif
}

RetainPtr<const CPDF_Object> GetXFAEntryFromDocument(const CPDF_Document* doc) {
  const CPDF_Dictionary* root = doc->GetRoot();
  if (!root) {
    return nullptr;
  }

  RetainPtr<const CPDF_Dictionary> acro_form = root->GetDictFor("AcroForm");
  return acro_form ? acro_form->GetObjectFor("XFA") : nullptr;
}

struct XFAPacket {
  ByteString name;
  RetainPtr<const CPDF_Stream> data;
};

std::vector<XFAPacket> GetXFAPackets(RetainPtr<const CPDF_Object> xfa_object) {
  std::vector<XFAPacket> packets;

  if (!xfa_object) {
    return packets;
  }

  RetainPtr<const CPDF_Stream> xfa_stream = ToStream(xfa_object->GetDirect());
  if (xfa_stream) {
    packets.push_back({"", std::move(xfa_stream)});
    return packets;
  }

  RetainPtr<const CPDF_Array> xfa_array = ToArray(xfa_object->GetDirect());
  if (!xfa_array) {
    return packets;
  }

  packets.reserve(1 + (xfa_array->size() / 2));
  for (size_t i = 0; i < xfa_array->size(); i += 2) {
    if (i + 1 == xfa_array->size()) {
      break;
    }

    RetainPtr<const CPDF_String> name = xfa_array->GetStringAt(i);
    if (!name) {
      continue;
    }

    RetainPtr<const CPDF_Stream> data = xfa_array->GetStreamAt(i + 1);
    if (!data) {
      continue;
    }

    packets.push_back({name->GetString(), std::move(data)});
  }
  return packets;
}

FPDF_DOCUMENT LoadDocumentImpl(RetainPtr<IFX_SeekableReadStream> pFileAccess,
                               FPDF_BYTESTRING password) {
  if (!pFileAccess) {
    ProcessParseError(CPDF_Parser::FILE_ERROR);
    return nullptr;
  }

  auto document =
      std::make_unique<CPDF_Document>(std::make_unique<CPDF_DocRenderData>(),
                                      std::make_unique<CPDF_DocPageData>());

  CPDF_Parser::Error error =
      document->LoadDoc(std::move(pFileAccess), password);
  if (error != CPDF_Parser::SUCCESS) {
    ProcessParseError(error);
    return nullptr;
  }

  ReportUnsupportedFeatures(document.get());
  return FPDFDocumentFromCPDFDocument(document.release());
}

}  // namespace

FPDF_EXPORT void FPDF_CALLCONV FPDF_InitLibrary() {
  FPDF_InitLibraryWithConfig(nullptr);
}

FPDF_EXPORT void FPDF_CALLCONV
FPDF_InitLibraryWithConfig(const FPDF_LIBRARY_CONFIG* config) {
  if (g_bLibraryInitialized) {
    return;
  }

  FX_InitializeMemoryAllocators();
  CFX_Timer::InitializeGlobals();
  CFX_GEModule::Create(config ? config->m_pUserFontPaths : nullptr);
  pdfium::InitializePageModule();

#if defined(PDF_USE_SKIA)
  CFX_GlyphCache::InitializeGlobals();
#endif

#ifdef PDF_ENABLE_XFA
  CPDFXFA_ModuleInit();
#endif  // PDF_ENABLE_XFA

  if (config && config->version >= 2) {
    void* platform = config->version >= 3 ? config->m_pPlatform : nullptr;
    IJS_Runtime::Initialize(config->m_v8EmbedderSlot, config->m_pIsolate,
                            platform);

    if (config->version >= 4) {
      SetRendererType(config->m_RendererType);
    }
  }
  g_bLibraryInitialized = true;
}

FPDF_EXPORT void FPDF_CALLCONV FPDF_DestroyLibrary() {
  if (!g_bLibraryInitialized) {
    return;
  }

  // Note: we teardown/destroy things in reverse order.
  ResetRendererType();

  IJS_Runtime::Destroy();

#ifdef PDF_ENABLE_XFA
  CPDFXFA_ModuleDestroy();
#endif  // PDF_ENABLE_XFA

#if defined(PDF_USE_SKIA)
  CFX_GlyphCache::DestroyGlobals();
#endif

  // EmbedPDF: registered runtime fonts are global/TLS-backed PDFium state, so
  // tear them down with the rest of the library singletons.
  CFX_FontRegistry::DestroyGlobals();
  pdfium::DestroyPageModule();
  CFX_GEModule::Destroy();
  CFX_Timer::DestroyGlobals();
  FX_DestroyMemoryAllocators();

  g_bLibraryInitialized = false;
}

FPDF_EXPORT void FPDF_CALLCONV FPDF_SetSandBoxPolicy(FPDF_DWORD policy,
                                                     FPDF_BOOL enable) {
  return SetPDFSandboxPolicy(policy, enable);
}

#if BUILDFLAG(IS_WIN)
FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_SetPrintMode(int mode) {
  if (mode < FPDF_PRINTMODE_EMF ||
      mode > FPDF_PRINTMODE_POSTSCRIPT3_TYPE42_PASSTHROUGH) {
    return FALSE;
  }

  g_pdfium_print_mode = static_cast<WindowsPrintMode>(mode);
  return TRUE;
}
#endif  // BUILDFLAG(IS_WIN)

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
FPDF_LoadDocument(FPDF_STRING file_path, FPDF_BYTESTRING password) {
  // NOTE: the creation of the file needs to be by the embedder on the
  // other side of this API.
  return LoadDocumentImpl(CFX_FileAccessStream::CreateFromFilename(file_path),
                          password);
}

FPDF_EXPORT int FPDF_CALLCONV FPDF_GetFormType(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return FORMTYPE_NONE;
  }

  const CPDF_Dictionary* pRoot = doc->GetRoot();
  if (!pRoot) {
    return FORMTYPE_NONE;
  }

  RetainPtr<const CPDF_Dictionary> pAcroForm = pRoot->GetDictFor("AcroForm");
  if (!pAcroForm) {
    return FORMTYPE_NONE;
  }

  RetainPtr<const CPDF_Object> pXFA = pAcroForm->GetObjectFor("XFA");
  if (!pXFA) {
    return FORMTYPE_ACRO_FORM;
  }

  bool bNeedsRendering = pRoot->GetBooleanFor("NeedsRendering", false);
  return bNeedsRendering ? FORMTYPE_XFA_FULL : FORMTYPE_XFA_FOREGROUND;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_LoadXFA(FPDF_DOCUMENT document) {
#ifdef PDF_ENABLE_XFA
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return false;
  }

  auto* context = static_cast<CPDFXFA_Context*>(doc->GetExtension());
  if (context) {
    return context->LoadXFADoc();
  }
#endif  // PDF_ENABLE_XFA
  return false;
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
FPDF_LoadMemDocument(const void* data_buf, int size, FPDF_BYTESTRING password) {
  if (size < 0) {
    return nullptr;
  }
  // SAFETY: required from caller.
  auto data_span = UNSAFE_BUFFERS(pdfium::span(
      static_cast<const uint8_t*>(data_buf), static_cast<size_t>(size)));
  return LoadDocumentImpl(pdfium::MakeRetain<CFX_ReadOnlySpanStream>(data_span),
                          password);
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
FPDF_LoadMemDocument64(const void* data_buf,
                       size_t size,
                       FPDF_BYTESTRING password) {
  // SAFETY: required from caller.
  auto data_span =
      UNSAFE_BUFFERS(pdfium::span(static_cast<const uint8_t*>(data_buf), size));
  return LoadDocumentImpl(pdfium::MakeRetain<CFX_ReadOnlySpanStream>(data_span),
                          password);
}

FPDF_EXPORT FPDF_DOCUMENT FPDF_CALLCONV
FPDF_LoadCustomDocument(FPDF_FILEACCESS* pFileAccess,
                        FPDF_BYTESTRING password) {
  if (!pFileAccess) {
    return nullptr;
  }
  return LoadDocumentImpl(pdfium::MakeRetain<CPDFSDK_CustomAccess>(pFileAccess),
                          password);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_GetFileVersion(FPDF_DOCUMENT doc,
                                                        int* fileVersion) {
  if (!fileVersion) {
    return false;
  }

  *fileVersion = 0;
  CPDF_Document* document = CPDFDocumentFromFPDFDocument(doc);
  if (!document) {
    return false;
  }

  const CPDF_Parser* pParser = document->GetParser();
  if (!pParser) {
    return false;
  }

  *fileVersion = pParser->GetFileVersion();

  const CPDF_Dictionary* root_dict = document->GetRoot();
  if (root_dict) {
    ByteString version = root_dict->GetNameFor("Version");
    if (!version.IsEmpty()) {
      // Check for valid PDF version format "X.Y"
      const bool has_valid_length = version.GetLength() == 3;
      const bool has_valid_format =
          has_valid_length && FXSYS_IsDecimalDigit(version[0]) &&
          version[1] == '.' && FXSYS_IsDecimalDigit(version[2]);
      if (has_valid_format) {
        const int major = FXSYS_DecimalCharToInt(version[0]);
        const int minor = FXSYS_DecimalCharToInt(version[2]);
        *fileVersion = major * 10 + minor;
      }
    }
  }

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_DocumentHasValidCrossReferenceTable(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc && doc->has_valid_cross_reference_table();
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_GetDocPermissions(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc ? doc->GetUserPermissions(/*get_owner_perms=*/true) : 0;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_GetDocUserPermissions(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  return doc ? doc->GetUserPermissions(/*get_owner_perms=*/false) : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDF_GetSecurityHandlerRevision(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || !doc->GetParser()) {
    return -1;
  }

  RetainPtr<const CPDF_Dictionary> dict = doc->GetParser()->GetEncryptDict();
  return dict ? dict->GetIntegerFor("R") : -1;
}

namespace {

// Build P value with correct reserved bits for R>=3 (including R=4 and R=6)
// Input: allowed_flags - OR'd combination of permission bits user wants to
// ALLOW Output: proper P value with reserved bits set correctly
uint32_t BuildPermissionsForRevision(uint32_t allowed_flags) {
  // Enforce: PrintHighQuality implies Print (bit 12 requires bit 3)
  // Some readers interpret oddly if PRINT_HIGH is set without PRINT
  if (allowed_flags & EPDF_PERM_PRINT_HIGH) {
    allowed_flags |= EPDF_PERM_PRINT;
  }

  // Start with allowed flags
  uint32_t p = allowed_flags;

  // Apply reserved bit requirements (PDF Reference 1.7, Table 3.20)
  // Bits 1-2 must be 0
  p &= 0xFFFFFFFC;
  // Bits 7-8 must be 1 (for R>=3)
  // Bits 13-32 must be 1
  p |= 0xFFFFF0C0;

  return p;
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_SetEncryption(FPDF_DOCUMENT document,
                   FPDF_BYTESTRING user_password,
                   FPDF_BYTESTRING owner_password,
                   unsigned long allowed_flags) {
  // Validation
  if (!document || !owner_password || !*owner_password) {
    return false;
  }

  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  // NOTE: We allow re-keying already encrypted documents.
  // User opens with old password, calls setEncryption with new, saves.

  // Build permissions P value (negative, with reserved bits set)
  const uint32_t allowed_flags_32 = static_cast<uint32_t>(allowed_flags);
  int32_t permissions =
      static_cast<int32_t>(BuildPermissionsForRevision(allowed_flags_32));

  // Create the encrypt dictionary inline. CPDF_Creator::SetEncryption() owns
  // the trailer-only reference and writes inline encrypt dictionaries as
  // indirect objects during save.
  auto pEncryptDict = pDoc->New<CPDF_Dictionary>();
  pEncryptDict->SetNewFor<CPDF_Name>("Filter", "Standard");
  pEncryptDict->SetNewFor<CPDF_Number>("V", 5);
  pEncryptDict->SetNewFor<CPDF_Number>("R", 6);
  pEncryptDict->SetNewFor<CPDF_Number>("Length", 256);
  pEncryptDict->SetNewFor<CPDF_Number>("P", permissions);
  pEncryptDict->SetNewFor<CPDF_Boolean>("EncryptMetadata", true);

  // Crypt filters for AES-256
  auto pCF = pEncryptDict->SetNewFor<CPDF_Dictionary>("CF");
  auto pStdCF = pCF->SetNewFor<CPDF_Dictionary>("StdCF");
  pStdCF->SetNewFor<CPDF_Name>("Type", "CryptFilter");
  pStdCF->SetNewFor<CPDF_Name>("CFM", "AESV3");
  pStdCF->SetNewFor<CPDF_Name>("AuthEvent", "DocOpen");
  pStdCF->SetNewFor<CPDF_Number>("Length", 32);
  pEncryptDict->SetNewFor<CPDF_Name>("StmF", "StdCF");
  pEncryptDict->SetNewFor<CPDF_Name>("StrF", "StdCF");

  // Create security handler and initialize with BOTH passwords
  auto pSecurityHandler = pdfium::MakeRetain<CPDF_SecurityHandler>();
  ByteString owner_pwd(owner_password);
  ByteString user_pwd(user_password ? user_password : "");

  // OnCreateWithPasswords properly sets U, UE, O, OE, Perms for R=6
  // Returns false if LoadDict fails or R != 6
  if (!pSecurityHandler->OnCreateWithPasswords(pEncryptDict.Get(), user_pwd,
                                               owner_pwd)) {
    return false;
  }

  CPDF_Document::PendingSecurity pending;
  pending.mode = CPDF_Document::PendingSecurityMode::kEncrypt;
  pending.encrypt_dict = std::move(pEncryptDict);
  pending.security_handler = std::move(pSecurityHandler);
  pDoc->SetPendingSecurity(std::move(pending));

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_RemoveEncryption(FPDF_DOCUMENT document) {
  if (!document) {
    return false;
  }

  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  CPDF_Document::PendingSecurity pending;
  pending.mode = CPDF_Document::PendingSecurityMode::kRemove;
  pDoc->SetPendingSecurity(std::move(pending));

  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_UnlockOwnerPermissions(FPDF_DOCUMENT document,
                            FPDF_BYTESTRING owner_password) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  CPDF_Parser* pParser = pDoc->GetParser();
  if (!pParser) {
    return false;  // Document wasn't loaded from file
  }

  const auto& security_handler = pParser->GetSecurityHandler();
  if (!security_handler) {
    return false;  // Document isn't encrypted
  }

  // Pass raw password - encoding is handled inside UnlockOwner/CheckPassword
  // DO NOT call GetEncodedPassword here - that would double-encode
  ByteString password(owner_password ? owner_password : "");
  return security_handler->UnlockOwner(password);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_CheckPasswordPermissions(FPDF_DOCUMENT document,
                              FPDF_BYTESTRING password,
                              int* out_kind,
                              unsigned int* out_user_permissions,
                              unsigned int* out_effective_permissions,
                              int* out_security_handler_revision) {
  if (!out_kind || !out_user_permissions || !out_effective_permissions ||
      !out_security_handler_revision) {
    return false;
  }

  *out_kind = EPDF_PASSWORD_PERMISSION_INVALID;
  *out_user_permissions = 0;
  *out_effective_permissions = 0;
  *out_security_handler_revision = -1;

  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  CPDF_Parser* pParser = pDoc->GetParser();
  if (!pParser) {
    return false;
  }

  const auto& security_handler = pParser->GetSecurityHandler();
  if (!security_handler) {
    *out_kind = EPDF_PASSWORD_PERMISSION_NONE;
    *out_user_permissions = 0xFFFFFFFF;
    *out_effective_permissions = 0xFFFFFFFF;
    return true;
  }

  RetainPtr<const CPDF_Dictionary> encrypt_dict = pParser->GetEncryptDict();
  *out_security_handler_revision =
      encrypt_dict ? encrypt_dict->GetIntegerFor("R") : -1;

  const unsigned int user_permissions = static_cast<unsigned int>(
      security_handler->GetPermissionsForPasswordProbe(/*owner=*/false));
  *out_user_permissions = user_permissions;

  ByteString raw_password(password ? password : "");

  // This is a password probe, not a document-state probe. Do not use
  // IsOwnerUnlocked(), UnlockOwner(), or FPDF_GetDocPermissions() here; those
  // depend on or mutate the current handle state. Match PDFium's open path by
  // checking a non-empty password against owner credentials first.
  if (!raw_password.IsEmpty() &&
      security_handler->CheckPasswordNoMutate(raw_password, /*bOwner=*/true)) {
    *out_kind = EPDF_PASSWORD_PERMISSION_OWNER;
    *out_effective_permissions = static_cast<unsigned int>(
        security_handler->GetPermissionsForPasswordProbe(/*owner=*/true));
    return true;
  }

  if (security_handler->CheckPasswordNoMutate(raw_password, /*bOwner=*/false)) {
    *out_kind = EPDF_PASSWORD_PERMISSION_USER;
    *out_effective_permissions = user_permissions;
    return true;
  }

  return false;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_SetRuntimeOwnerPermissions(FPDF_DOCUMENT document, FPDF_BOOL enabled) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  CPDF_Parser* pParser = pDoc->GetParser();
  if (!pParser) {
    return false;
  }

  const auto& security_handler = pParser->GetSecurityHandler();
  if (!security_handler) {
    return false;
  }

  security_handler->SetRuntimeOwnerUnlocked(!!enabled);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV EPDF_IsEncrypted(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  CPDF_Parser* pParser = pDoc->GetParser();
  if (!pParser) {
    return false;
  }

  return pParser->GetSecurityHandler() != nullptr;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_IsOwnerUnlocked(FPDF_DOCUMENT document) {
  CPDF_Document* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  CPDF_Parser* pParser = pDoc->GetParser();
  if (!pParser) {
    return false;
  }

  const auto& security_handler = pParser->GetSecurityHandler();
  if (!security_handler) {
    return false;  // Not encrypted
  }

  return security_handler->IsOwnerUnlocked();
}

FPDF_EXPORT int FPDF_CALLCONV FPDF_GetPageCount(FPDF_DOCUMENT document) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 0;
  }

  auto* pExtension = doc->GetExtension();
  return pExtension ? pExtension->GetPageCount() : doc->GetPageCount();
}

namespace {

// Shared body of FPDF_LoadPage and EPDFDoc_LoadPageByObjectNumber. Validates
// `page_index` against the document's page count, then constructs and returns
// a leaked page handle. When `normalize` is true, the page's rotation is
// overridden to 0 so all subsequent operations use normalized 0-degree
// coordinates (the intrinsic rotation is surfaced separately via
// EPDF_GetPageRotationByIndex). Returns nullptr on any failure.
FPDF_PAGE LoadPageByValidatedIndex(FPDF_DOCUMENT document,
                                   CPDF_Document* doc,
                                   int page_index,
                                   bool normalize) {
  if (page_index < 0 || page_index >= FPDF_GetPageCount(document)) {
    return nullptr;
  }

#ifdef PDF_ENABLE_XFA
  auto* context = static_cast<CPDFXFA_Context*>(doc->GetExtension());
  if (context) {
    return FPDFPageFromIPDFPage(context->GetOrCreateXFAPage(page_index).Leak());
  }
#endif  // PDF_ENABLE_XFA

  RetainPtr<const CPDF_Dictionary> const_dict =
      doc->GetPageDictionary(page_index);
  RetainPtr<CPDF_Dictionary> dict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
  if (!dict) {
    return nullptr;
  }

  auto pPage = pdfium::MakeRetain<CPDF_Page>(doc, std::move(dict));
  pPage->AddPageImageCache();
  pPage->ParseContent();

  // Force rotation to 0 - this re-runs UpdateDimensions() so page_size_ and
  // page_matrix_ are calculated as if rotation=0.
  if (normalize) {
    pPage->SetRotationOverride(0);
  }

  return FPDFPageFromIPDFPage(pPage.Leak());
}

}  // namespace

FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV FPDF_LoadPage(FPDF_DOCUMENT document,
                                                  int page_index) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return nullptr;
  }
  return LoadPageByValidatedIndex(document, doc, page_index,
                                  /*normalize=*/false);
}

FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDFDoc_LoadPageByObjectNumber(FPDF_DOCUMENT document, unsigned int obj_num) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || obj_num == 0) {
    return nullptr;
  }
  return LoadPageByValidatedIndex(document, doc, doc->GetPageIndex(obj_num),
                                  /*normalize=*/false);
}

FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDFDoc_LoadPageByObjectNumberNormalized(FPDF_DOCUMENT document,
                                         unsigned int obj_num) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || obj_num == 0) {
    return nullptr;
  }
  return LoadPageByValidatedIndex(document, doc, doc->GetPageIndex(obj_num),
                                  /*normalize=*/true);
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFDoc_GetPageObjectNumberByIndex(FPDF_DOCUMENT document, int page_index) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || page_index < 0 || page_index >= FPDF_GetPageCount(document)) {
    return 0;
  }

#ifdef PDF_ENABLE_XFA
  // XFA pages do not have CPDF_Page dictionaries. Match
  // EPDFPage_GetObjectNumber()'s documented XFA behavior and return 0.
  if (doc->GetExtension()) {
    return 0;
  }
#endif  // PDF_ENABLE_XFA

  RetainPtr<const CPDF_Dictionary> dict = doc->GetPageDictionary(page_index);
  return dict ? dict->GetObjNum() : 0;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_DeletePageByObjectNumber(FPDF_DOCUMENT document, unsigned int obj_num) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || obj_num == 0) {
    return false;
  }

#ifdef PDF_ENABLE_XFA
  // XFA pages do not have CPDF_Page dictionaries. Match the other
  // EPDFDoc_*ByObjectNumber APIs and reject object-number page mutations for
  // XFA-backed documents.
  if (doc->GetExtension()) {
    return false;
  }
#endif  // PDF_ENABLE_XFA

  const int page_index = doc->GetPageIndex(obj_num);
  if (page_index < 0) {
    return false;
  }

  // A page that leaves the tree must not leave /Names /Pages registrations
  // pointing at it (they would resolve to the null object after
  // SetPageToNullObject). Done here, before the delete, so the references
  // still resolve while we search, and so every caller gets the invariant.
  EPDFDoc_RemoveNamedPagesForPage(document, obj_num);

  const uint32_t deleted_obj_num = doc->DeletePage(page_index);
  if (deleted_obj_num == 0) {
    return false;
  }

  doc->SetPageToNullObject(deleted_obj_num);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDFDoc_SetPageRotationByObjectNumber(FPDF_DOCUMENT document,
                                      unsigned int obj_num,
                                      int rotate) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || obj_num == 0 || rotate < 0 || rotate > 3) {
    return false;
  }

#ifdef PDF_ENABLE_XFA
  // XFA pages do not have CPDF_Page dictionaries. Match the other
  // EPDFDoc_*ByObjectNumber APIs and reject object-number page mutations for
  // XFA-backed documents.
  if (doc->GetExtension()) {
    return false;
  }
#endif  // PDF_ENABLE_XFA

  if (doc->GetPageIndex(obj_num) < 0) {
    return false;
  }

  RetainPtr<CPDF_Dictionary> page_dict =
      ToDictionary(doc->GetMutableIndirectObject(obj_num));
  if (!page_dict) {
    return false;
  }

  page_dict->SetNewFor<CPDF_Number>(pdfium::page_object::kRotate, rotate * 90);
  return true;
}

FPDF_EXPORT unsigned int FPDF_CALLCONV
EPDFPage_GetObjectNumber(FPDF_PAGE page) {
  // Note: CPDFPageFromFPDFPage() returns null for XFA pages, so this function
  // returns 0 for XFA pages (documented in the header).
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return 0;
  }
  const CPDF_Dictionary* dict = pPage->GetDict().Get();
  if (!dict) {
    return 0;
  }
  return dict->GetObjNum();
}

FPDF_EXPORT float FPDF_CALLCONV FPDF_GetPageWidthF(FPDF_PAGE page) {
  IPDF_Page* pPage = IPDFPageFromFPDFPage(page);
  return pPage ? pPage->GetPageWidth() : 0.0f;
}

FPDF_EXPORT double FPDF_CALLCONV FPDF_GetPageWidth(FPDF_PAGE page) {
  return FPDF_GetPageWidthF(page);
}

FPDF_EXPORT float FPDF_CALLCONV FPDF_GetPageHeightF(FPDF_PAGE page) {
  IPDF_Page* pPage = IPDFPageFromFPDFPage(page);
  return pPage ? pPage->GetPageHeight() : 0.0f;
}

FPDF_EXPORT double FPDF_CALLCONV FPDF_GetPageHeight(FPDF_PAGE page) {
  return FPDF_GetPageHeightF(page);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_GetPageBoundingBox(FPDF_PAGE page,
                                                            FS_RECTF* rect) {
  if (!rect) {
    return false;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    return false;
  }

  *rect = FSRectFFromCFXFloatRect(pPage->GetBBox());
  return true;
}

#if BUILDFLAG(IS_WIN)
namespace {

constexpr float kEpsilonSize = 0.01f;

bool IsPageTooSmall(const CPDF_Page* page) {
  const CFX_SizeF& page_size = page->GetPageSize();
  return page_size.width < kEpsilonSize || page_size.height < kEpsilonSize;
}

bool IsScalingTooSmall(const CFX_Matrix& matrix) {
  float horizontal;
  float vertical;
  if (matrix.a == 0.0f && matrix.d == 0.0f) {
    horizontal = matrix.b;
    vertical = matrix.c;
  } else {
    horizontal = matrix.a;
    vertical = matrix.d;
  }
  return fabsf(horizontal) < kEpsilonSize || fabsf(vertical) < kEpsilonSize;
}

// Get a bitmap of just the mask section defined by |mask_box| from a full page
// bitmap |pBitmap|.
RetainPtr<CFX_DIBitmap> GetMaskBitmap(CPDF_Page* pPage,
                                      int start_x,
                                      int start_y,
                                      int size_x,
                                      int size_y,
                                      int rotate,
                                      RetainPtr<const CFX_DIBitmap> source,
                                      const CFX_FloatRect& mask_box,
                                      FX_RECT* bitmap_area) {
  if (IsPageTooSmall(pPage)) {
    return nullptr;
  }

  FX_RECT page_rect(start_x, start_y, start_x + size_x, start_y + size_y);
  CFX_Matrix matrix = pPage->GetDisplayMatrixForRect(page_rect, rotate);
  if (IsScalingTooSmall(matrix)) {
    return nullptr;
  }

  *bitmap_area = matrix.TransformRect(mask_box).GetOuterRect();
  if (bitmap_area->IsEmpty()) {
    return nullptr;
  }

  // Create a new bitmap to transfer part of the page bitmap to.
  RetainPtr<CFX_DIBitmap> pDst = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!pDst->Create(bitmap_area->Width(), bitmap_area->Height(),
                    FXDIB_Format::kBgra)) {
    return nullptr;
  }
  pDst->Clear(0x00ffffff);
  pDst->TransferBitmap(bitmap_area->Width(), bitmap_area->Height(),
                       std::move(source), bitmap_area->left, bitmap_area->top);
  return pDst;
}

void RenderBitmap(CFX_RenderDevice* device,
                  RetainPtr<const CFX_DIBitmap> source,
                  const FX_RECT& mask_area) {
  int size_x_bm = mask_area.Width();
  int size_y_bm = mask_area.Height();
  if (size_x_bm == 0 || size_y_bm == 0) {
    return;
  }

  // Create a new bitmap from the old one
  RetainPtr<CFX_DIBitmap> dest = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!dest->Create(size_x_bm, size_y_bm, FXDIB_Format::kBgrx)) {
    return;
  }

  dest->Clear(0xffffffff);
  dest->CompositeBitmap(0, 0, size_x_bm, size_y_bm, std::move(source), 0, 0,
                        BlendMode::kNormal, nullptr, false);

  if (device->GetDeviceType() == DeviceType::kPrinter) {
    device->StretchDIBits(std::move(dest), mask_area.left, mask_area.top,
                          size_x_bm, size_y_bm);
  } else {
    device->SetDIBits(std::move(dest), mask_area.left, mask_area.top);
  }
}

}  // namespace

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_RenderPage(HDC dc,
                                                    FPDF_PAGE page,
                                                    int start_x,
                                                    int start_y,
                                                    int size_x,
                                                    int size_y,
                                                    int rotate,
                                                    int flags) {
  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return false;
  }
  CPDF_Page* pPage = page_view.Get();

  auto owned_context = std::make_unique<CPDF_PageRenderContext>();
  CPDF_PageRenderContext* context = owned_context.get();
  CPDF_Page::RenderContextClearer clearer(pPage);
  pPage->SetRenderContext(std::move(owned_context));

  // Don't render the full page to bitmap for a mask unless there are a lot
  // of masks. Full page bitmaps result in large spool sizes, so they should
  // only be used when necessary. For large numbers of masks, rendering each
  // individually is inefficient and unlikely to significantly improve spool
  // size.
  const bool bEnableImageMasks =
      g_pdfium_print_mode == WindowsPrintMode::kEmfImageMasks;
  const bool bNewBitmap = pPage->BackgroundAlphaNeeded() ||
                          (pPage->HasImageMask() && !bEnableImageMasks) ||
                          pPage->GetMaskBoundingBoxes().size() > 100;
  const bool bHasMask = pPage->HasImageMask() && !bNewBitmap;
  auto* render_data = CPDF_DocRenderData::FromDocument(pPage->GetDocument());
  if (!bNewBitmap && !bHasMask) {
    context->device_ = std::make_unique<CPDF_WindowsRenderDevice>(
        dc, render_data->GetPSFontTracker());
    CPDFSDK_RenderPageWithContext(context, pPage, start_x, start_y, size_x,
                                  size_y, rotate, flags,
                                  /*color_scheme=*/nullptr,
                                  /*need_to_restore=*/true, /*pause=*/nullptr);
    return true;
  }

  RetainPtr<CFX_DIBitmap> pBitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!pBitmap->Create(size_x, size_y, FXDIB_Format::kBgra)) {
    return false;
  }
  if (!CFX_DefaultRenderDevice::UseSkiaRenderer()) {
    // Not needed by Skia. Call it for AGG to preserve pre-existing behavior.
    pBitmap->Clear(0x00ffffff);
  }

  auto device = std::make_unique<CFX_DefaultRenderDevice>();
  device->Attach(pBitmap);
  context->device_ = std::move(device);
  if (bHasMask) {
    context->options_ = std::make_unique<CPDF_RenderOptions>();
    context->options_->GetOptions().bBreakForMasks = true;
  }

  CPDFSDK_RenderPageWithContext(context, pPage, start_x, start_y, size_x,
                                size_y, rotate, flags, /*color_scheme=*/nullptr,
                                /*need_to_restore=*/true,
                                /*pause=*/nullptr);

  if (!bHasMask) {
    CPDF_WindowsRenderDevice win_dc(dc, render_data->GetPSFontTracker());
    bool bitsStretched = false;
    if (win_dc.GetDeviceType() == DeviceType::kPrinter) {
      auto dest_bitmap = pdfium::MakeRetain<CFX_DIBitmap>();
      if (dest_bitmap->Create(size_x, size_y, FXDIB_Format::kBgrx)) {
        std::ranges::fill(dest_bitmap->GetWritableBuffer().first(
                              pBitmap->GetPitch() * size_y),
                          -1);
        dest_bitmap->CompositeBitmap(0, 0, size_x, size_y, pBitmap, 0, 0,
                                     BlendMode::kNormal, nullptr, false);
        win_dc.StretchDIBits(std::move(dest_bitmap), 0, 0, size_x, size_y);
        bitsStretched = true;
      }
    }
    if (!bitsStretched) {
      win_dc.SetDIBits(std::move(pBitmap), 0, 0);
    }
    return true;
  }

  // Finish rendering the page to bitmap and copy the correct segments
  // of the page to individual image mask bitmaps.
  const std::vector<CFX_FloatRect>& mask_boxes = pPage->GetMaskBoundingBoxes();
  std::vector<FX_RECT> bitmap_areas(mask_boxes.size());
  std::vector<RetainPtr<CFX_DIBitmap>> bitmaps(mask_boxes.size());
  for (size_t i = 0; i < mask_boxes.size(); i++) {
    bitmaps[i] = GetMaskBitmap(pPage, start_x, start_y, size_x, size_y, rotate,
                               pBitmap, mask_boxes[i], &bitmap_areas[i]);
    context->renderer_->Continue(nullptr);
  }

  // Begin rendering to the printer. Add flag to indicate the renderer should
  // pause after each image mask.
  pPage->ClearRenderContext();
  owned_context = std::make_unique<CPDF_PageRenderContext>();
  context = owned_context.get();
  pPage->SetRenderContext(std::move(owned_context));
  context->device_ = std::make_unique<CPDF_WindowsRenderDevice>(
      dc, render_data->GetPSFontTracker());
  context->options_ = std::make_unique<CPDF_RenderOptions>();
  context->options_->GetOptions().bBreakForMasks = true;

  CPDFSDK_RenderPageWithContext(context, pPage, start_x, start_y, size_x,
                                size_y, rotate, flags,
                                /*color_scheme=*/nullptr,
                                /*need_to_restore=*/true,
                                /*pause=*/nullptr);
  // Render masks
  for (size_t i = 0; i < mask_boxes.size(); i++) {
    // Render the bitmap for the mask and free the bitmap.
    if (bitmaps[i]) {  // will be null if mask has zero area
      RenderBitmap(context->device_.get(), std::move(bitmaps[i]),
                   bitmap_areas[i]);
    }
    // Render the next portion of page.
    context->renderer_->Continue(nullptr);
  }

  return true;
}
#endif  // BUILDFLAG(IS_WIN)

FPDF_EXPORT void FPDF_CALLCONV FPDF_RenderPageBitmap(FPDF_BITMAP bitmap,
                                                     FPDF_PAGE page,
                                                     int start_x,
                                                     int start_y,
                                                     int size_x,
                                                     int size_y,
                                                     int rotate,
                                                     int flags) {
  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return;
  }
  CPDF_Page* pPage = page_view.Get();

  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pBitmap) {
    return;
  }
  ValidateBitmapPremultiplyState(pBitmap);

  auto owned_context = std::make_unique<CPDF_PageRenderContext>();
  CPDF_PageRenderContext* context = owned_context.get();
  CPDF_Page::RenderContextClearer clearer(pPage);
  pPage->SetRenderContext(std::move(owned_context));

#if defined(PDF_USE_SKIA)
  CFX_DIBitmap::ScopedPremultiplier scoped_premultiplier(pBitmap);
#endif
  auto device = std::make_unique<CFX_DefaultRenderDevice>();
  device->AttachWithRgbByteOrder(std::move(pBitmap),
                                 !!(flags & FPDF_REVERSE_BYTE_ORDER));
  context->device_ = std::move(device);

  CPDFSDK_RenderPageWithContext(context, pPage, start_x, start_y, size_x,
                                size_y, rotate, flags, /*color_scheme=*/nullptr,
                                /*need_to_restore=*/true,
                                /*pause=*/nullptr);
}

FPDF_EXPORT void FPDF_CALLCONV
FPDF_RenderPageBitmapWithMatrix(FPDF_BITMAP bitmap,
                                FPDF_PAGE page,
                                const FS_MATRIX* matrix,
                                const FS_RECTF* clipping,
                                int flags) {
  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return;
  }
  CPDF_Page* pPage = page_view.Get();

  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pBitmap) {
    return;
  }
  ValidateBitmapPremultiplyState(pBitmap);

  auto owned_context = std::make_unique<CPDF_PageRenderContext>();
  CPDF_PageRenderContext* context = owned_context.get();
  CPDF_Page::RenderContextClearer clearer(pPage);
  pPage->SetRenderContext(std::move(owned_context));

#if defined(PDF_USE_SKIA)
  CFX_DIBitmap::ScopedPremultiplier scoped_premultiplier(pBitmap);
#endif
  auto device = std::make_unique<CFX_DefaultRenderDevice>();
  device->AttachWithRgbByteOrder(std::move(pBitmap),
                                 !!(flags & FPDF_REVERSE_BYTE_ORDER));
  context->device_ = std::move(device);

  CFX_FloatRect clipping_rect;
  if (clipping) {
    clipping_rect = CFXFloatRectFromFSRectF(*clipping);
  }
  FX_RECT clip_rect = clipping_rect.ToFxRect();

  CFX_Matrix transform_matrix = pPage->GetDisplayMatrix();
  if (matrix) {
    transform_matrix *= CFXMatrixFromFSMatrix(*matrix);
  }
  CPDFSDK_RenderPage(context, pPage, transform_matrix, clip_rect, flags,
                     /*color_scheme=*/nullptr);
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_RenderAnnotBitmap(FPDF_BITMAP bitmap,
                       FPDF_PAGE page,
                       FPDF_ANNOTATION annot,
                       FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                       const FS_MATRIX* matrix,
                       int flags) {
  // Guards
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!bitmap || !pPage || !annot) {
    return false;
  }

  CPDF_AnnotContext* pAnnotContext = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnotContext) {
    return false;
  }

  CPDF_DocumentViewScope document_view(pPage->GetDocument());

  // Get the annotation's dictionary from the context.
  const CPDF_Dictionary* pAnnotDict = pAnnotContext->GetAnnotDict();
  if (!pAnnotDict) {
    return false;
  }

  // Get the document from the page. The CPDF_Annot constructor needs it.
  CPDF_Document* pDoc = pPage->GetDocument();
  if (!pDoc) {
    return false;
  }

  // Instantiate CPDF_Annot using its public constructor.
  auto pAnnot = std::make_unique<CPDF_Annot>(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(pAnnotDict)), pDoc);

  // ---------------------------------------------------------------- bitmaps
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pBitmap) {
    return false;
  }
  ValidateBitmapPremultiplyState(pBitmap);

#if defined(PDF_USE_SKIA)
  CFX_DIBitmap::ScopedPremultiplier scoped(pBitmap);
#endif

  auto device = std::make_unique<CFX_DefaultRenderDevice>();
  device->AttachWithRgbByteOrder(std::move(pBitmap),
                                 !!(flags & FPDF_REVERSE_BYTE_ORDER));

  //   CTM = DisplayMatrix * userMatrix * Translate(bbox.left, bbox.bottom)
  CFX_Matrix ctm = pPage->GetDisplayMatrix();
  if (matrix) {
    ctm.Concat(CFXMatrixFromFSMatrix(*matrix));
  }

  // Draw appearance
  const bool ok = pAnnot->DrawAppearance(
      pPage, device.get(), ctm,
      static_cast<CPDF_Annot::AppearanceMode>(appearanceMode));

  return ok;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_RenderAnnotBitmapUnrotated(FPDF_BITMAP bitmap,
                                FPDF_PAGE page,
                                FPDF_ANNOTATION annot,
                                FPDF_ANNOT_APPEARANCEMODE appearanceMode,
                                const FS_MATRIX* matrix,
                                int flags) {
  // Guards (same as EPDF_RenderAnnotBitmap)
  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!bitmap || !pPage || !annot) {
    return false;
  }

  CPDF_AnnotContext* pAnnotContext = CPDFAnnotContextFromFPDFAnnotation(annot);
  if (!pAnnotContext) {
    return false;
  }

  CPDF_DocumentViewScope document_view(pPage->GetDocument());
  const CPDF_Dictionary* pAnnotDict = pAnnotContext->GetAnnotDict();
  if (!pAnnotDict) {
    return false;
  }

  CPDF_Document* pDoc = pPage->GetDocument();
  if (!pDoc) {
    return false;
  }

  auto pAnnot = std::make_unique<CPDF_Annot>(
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(pAnnotDict)), pDoc);

  // Get the AP form for the requested mode.
  // Note: we skip ShouldDrawAnnotation/GenerateAPIfNeeded (private) because
  // the AP stream is expected to already exist when rendering stamps.
  auto mode = static_cast<CPDF_Annot::AppearanceMode>(appearanceMode);
  CPDF_Form* pForm = pAnnot->GetAPForm(pPage, mode);
  if (!pForm) {
    return false;
  }

  // Read the raw BBox WITHOUT applying the AP Matrix. For our wrapper the
  // rotation lives on the wrapper, which an opacity layer or another editor's
  // forms may hold further down: then its box and matrix are the ones to undo.
  CFX_FloatRect form_bbox = pForm->GetDict()->GetRectFor("BBox");
  CFX_Matrix form_matrix = pForm->GetDict()->GetMatrixFor("Matrix");
  if (std::optional<EpdfWrappedAppearance> ours = EpdfFindWrappedAppearance(
          pForm->GetStream().Get(), /*opacity=*/std::nullopt)) {
    form_bbox = ours->wrapper->GetDict()->GetRectFor("BBox");
    form_matrix = ours->wrapper_to_appearance;
  }

  // Use /EMBD_Metadata /UnrotatedRect as the target rect for MatchRect.
  // Falls back to /Rect if EmbedPDF metadata is not set.
  RetainPtr<const CPDF_Dictionary> metadata =
      pAnnot->GetAnnotDict()->GetDictFor("EMBD_Metadata");
  CFX_FloatRect target =
      metadata ? metadata->GetRectFor("UnrotatedRect") : CFX_FloatRect();
  if (target.IsEmpty()) {
    target = pAnnot->GetRect();
  }

  // The form's Matrix (rotation) was baked into the content objects during
  // parsing by CPDF_ContentParser.  We must undo it so the bitmap is unrotated.
  CFX_Matrix mtForm2Page = form_matrix.GetInverse();

  // Then map raw BBox -> target rect (identity when BBox == unrotatedRect).
  CFX_Matrix matchRect;
  matchRect.MatchRect(target, form_bbox);
  mtForm2Page.Concat(matchRect);

  // Build CTM = displayMatrix * userMatrix
  CFX_Matrix ctm = pPage->GetDisplayMatrix();
  if (matrix) {
    ctm.Concat(CFXMatrixFromFSMatrix(*matrix));
  }

  // Combine: form -> page -> device
  mtForm2Page.Concat(ctm);

  // ---- Bitmap setup (same as EPDF_RenderAnnotBitmap) ----
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pBitmap) {
    return false;
  }
  ValidateBitmapPremultiplyState(pBitmap);

#if defined(PDF_USE_SKIA)
  CFX_DIBitmap::ScopedPremultiplier scoped(pBitmap);
#endif

  auto device = std::make_unique<CFX_DefaultRenderDevice>();
  device->AttachWithRgbByteOrder(std::move(pBitmap),
                                 !!(flags & FPDF_REVERSE_BYTE_ORDER));

  // Render the AP form with our custom matrix (no AP Matrix distortion).
  CPDF_RenderContext context(pDoc,
                             pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(
                                 pPage->GetPageResources().Get())),
                             pPage->GetPageImageCache());
  context.AppendLayer(pForm, mtForm2Page);
  context.Render(device.get(), nullptr, nullptr, nullptr);
  return true;
}

#if defined(PDF_USE_SKIA)
FPDF_EXPORT void FPDF_CALLCONV FPDF_RenderPageSkia(FPDF_SKIA_CANVAS canvas,
                                                   FPDF_PAGE page,
                                                   int size_x,
                                                   int size_y) {
  SkCanvas* sk_canvas = SkCanvasFromFPDFSkiaCanvas(canvas);
  if (!sk_canvas) {
    return;
  }

  ScopedFPDFPageView page_view(page);
  if (!page_view) {
    return;
  }
  CPDF_Page* cpdf_page = page_view.Get();

  auto owned_context = std::make_unique<CPDF_PageRenderContext>();
  CPDF_PageRenderContext* context = owned_context.get();
  CPDF_Page::RenderContextClearer clearer(cpdf_page);
  cpdf_page->SetRenderContext(std::move(owned_context));

  auto device = std::make_unique<CFX_DefaultRenderDevice>();
  if (!device->AttachCanvas(*sk_canvas)) {
    return;
  }
  context->device_ = std::move(device);

  CPDFSDK_RenderPageWithContext(context, cpdf_page, 0, 0, size_x, size_y, 0, 0,
                                /*color_scheme=*/nullptr,
                                /*need_to_restore=*/true, /*pause=*/nullptr);
}
#endif  // defined(PDF_USE_SKIA)

FPDF_EXPORT void FPDF_CALLCONV FPDF_ClosePage(FPDF_PAGE page) {
  if (!page) {
    return;
  }

  // Take it back across the API and hold for duration of this function.
  RetainPtr<IPDF_Page> pPage;
  pPage.Unleak(IPDFPageFromFPDFPage(page));

  if (pPage->AsXFAPage()) {
    return;
  }

  // This will delete the PageView object corresponding to |pPage|. We must
  // cleanup the PageView before releasing the reference on |pPage| as it will
  // attempt to reset the PageView during destruction.
  pPage->AsPDFPage()->ClearView();
}

FPDF_EXPORT void FPDF_CALLCONV FPDF_CloseDocument(FPDF_DOCUMENT document) {
  // Take it back across the API and throw it away,
  std::unique_ptr<CPDF_Document>(CPDFDocumentFromFPDFDocument(document));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV FPDF_GetLastError() {
  return FXSYS_GetLastError();
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_DeviceToPage(FPDF_PAGE page,
                                                      int start_x,
                                                      int start_y,
                                                      int size_x,
                                                      int size_y,
                                                      int rotate,
                                                      int device_x,
                                                      int device_y,
                                                      double* page_x,
                                                      double* page_y) {
  if (!page || !page_x || !page_y) {
    return false;
  }

  IPDF_Page* pPage = IPDFPageFromFPDFPage(page);
  const FX_RECT rect(start_x, start_y, start_x + size_x, start_y + size_y);
  std::optional<CFX_PointF> pos =
      pPage->DeviceToPage(rect, rotate, CFX_PointF(device_x, device_y));
  if (!pos.has_value()) {
    return false;
  }

  *page_x = pos->x;
  *page_y = pos->y;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDF_PageToDevice(FPDF_PAGE page,
                                                      int start_x,
                                                      int start_y,
                                                      int size_x,
                                                      int size_y,
                                                      int rotate,
                                                      double page_x,
                                                      double page_y,
                                                      int* device_x,
                                                      int* device_y) {
  if (!page || !device_x || !device_y) {
    return false;
  }

  IPDF_Page* pPage = IPDFPageFromFPDFPage(page);
  const FX_RECT rect(start_x, start_y, start_x + size_x, start_y + size_y);
  CFX_PointF page_point(static_cast<float>(page_x), static_cast<float>(page_y));
  std::optional<CFX_PointF> pos = pPage->PageToDevice(rect, rotate, page_point);
  if (!pos.has_value()) {
    return false;
  }

  *device_x = FXSYS_roundf(pos->x);
  *device_y = FXSYS_roundf(pos->y);
  return true;
}

FPDF_EXPORT FPDF_BITMAP FPDF_CALLCONV FPDFBitmap_Create(int width,
                                                        int height,
                                                        int alpha) {
  auto pBitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!pBitmap->Create(width, height,
                       alpha ? FXDIB_Format::kBgra : FXDIB_Format::kBgrx)) {
    return nullptr;
  }

  ValidateBitmapPremultiplyState(pBitmap);

  // Caller takes ownership.
  return FPDFBitmapFromCFXDIBitmap(pBitmap.Leak());
}

FPDF_EXPORT FPDF_BITMAP FPDF_CALLCONV FPDFBitmap_CreateEx(int width,
                                                          int height,
                                                          int format,
                                                          void* first_scan,
                                                          int stride) {
  FXDIB_Format fx_format = FXDIBFormatFromFPDFFormat(format);
  if (fx_format == FXDIB_Format::kInvalid) {
    return nullptr;
  }

  // Ensure external memory is good at least for the duration of this call.
  UnownedPtr<uint8_t> pChecker(static_cast<uint8_t*>(first_scan));
  auto pBitmap = pdfium::MakeRetain<CFX_DIBitmap>();
  if (!pBitmap->Create(width, height, fx_format, pChecker, stride)) {
    return nullptr;
  }

  ValidateBitmapPremultiplyState(pBitmap);

  // Caller takes ownership.
  return FPDFBitmapFromCFXDIBitmap(pBitmap.Leak());
}

FPDF_EXPORT int FPDF_CALLCONV FPDFBitmap_GetFormat(FPDF_BITMAP bitmap) {
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pBitmap) {
    return FPDFBitmap_Unknown;
  }

  switch (pBitmap->GetFormat()) {
    case FXDIB_Format::k8bppRgb:
    case FXDIB_Format::k8bppMask:
      return FPDFBitmap_Gray;
    case FXDIB_Format::kBgr:
      return FPDFBitmap_BGR;
    case FXDIB_Format::kBgrx:
      return FPDFBitmap_BGRx;
    case FXDIB_Format::kBgra:
      return FPDFBitmap_BGRA;
#if defined(PDF_USE_SKIA)
    case FXDIB_Format::kBgraPremul:
      return CFX_DefaultRenderDevice::UseSkiaRenderer() ? FPDFBitmap_BGRA_Premul
                                                        : FPDFBitmap_Unknown;
#endif
    default:
      return FPDFBitmap_Unknown;
  }
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV FPDFBitmap_FillRect(FPDF_BITMAP bitmap,
                                                        int left,
                                                        int top,
                                                        int width,
                                                        int height,
                                                        FPDF_DWORD color) {
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  if (!pBitmap) {
    return false;
  }
  ValidateBitmapPremultiplyState(pBitmap);

  FX_SAFE_INT32 right = left;
  right += width;
  if (!right.IsValid()) {
    return false;
  }

  FX_SAFE_INT32 bottom = top;
  bottom += height;
  if (!bottom.IsValid()) {
    return false;
  }

  FX_RECT fill_rect(left, top, right.ValueOrDie(), bottom.ValueOrDie());

  if (!pBitmap->IsAlphaFormat()) {
    color |= 0xFF000000;
  }

  // Let CFX_DefaultRenderDevice handle the 8-bit case.
  const int bpp = pBitmap->GetBPP();
  if (bpp == 8) {
    CFX_DefaultRenderDevice device;
    device.Attach(std::move(pBitmap));
    return device.FillRect(fill_rect, static_cast<uint32_t>(color));
  }

  // Handle filling 24/32-bit bitmaps directly without CFX_DefaultRenderDevice.
  // When CFX_DefaultRenderDevice is using Skia, this avoids extra work to
  // change `pBitmap` to be premultiplied and back, or extra work to change
  // `pBitmap` to 32 BPP and back.
  fill_rect.Intersect(FX_RECT(0, 0, pBitmap->GetWidth(), pBitmap->GetHeight()));
  if (fill_rect.IsEmpty()) {
    // CFX_DefaultRenderDevice treats this as success. Match that.
    return true;
  }

  const int row_end = fill_rect.top + fill_rect.Height();
  if (bpp == 32) {
    for (int row = fill_rect.top; row < row_end; ++row) {
      auto span32 = pBitmap->GetWritableScanlineAs<uint32_t>(row).subspan(
          static_cast<size_t>(fill_rect.left),
          static_cast<size_t>(fill_rect.Width()));
      std::ranges::fill(span32, static_cast<uint32_t>(color));
    }
    return true;
  }

  CHECK_EQ(bpp, 24);
  const FX_BGR_STRUCT<uint8_t> bgr = {.blue = FXARGB_B(color),
                                      .green = FXARGB_G(color),
                                      .red = FXARGB_R(color)};
  for (int row = fill_rect.top; row < row_end; ++row) {
    auto bgr_span =
        pBitmap->GetWritableScanlineAs<FX_BGR_STRUCT<uint8_t>>(row).subspan(
            static_cast<size_t>(fill_rect.left),
            static_cast<size_t>(fill_rect.Width()));
    std::ranges::fill(bgr_span, bgr);
  }
  return true;
}

FPDF_EXPORT void* FPDF_CALLCONV FPDFBitmap_GetBuffer(FPDF_BITMAP bitmap) {
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  return pBitmap ? pBitmap->GetWritableBuffer().data() : nullptr;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFBitmap_GetWidth(FPDF_BITMAP bitmap) {
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  return pBitmap ? pBitmap->GetWidth() : 0;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFBitmap_GetHeight(FPDF_BITMAP bitmap) {
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  return pBitmap ? pBitmap->GetHeight() : 0;
}

FPDF_EXPORT int FPDF_CALLCONV FPDFBitmap_GetStride(FPDF_BITMAP bitmap) {
  RetainPtr<CFX_DIBitmap> pBitmap(CFXDIBitmapFromFPDFBitmap(bitmap));
  return pBitmap ? pBitmap->GetPitch() : 0;
}

FPDF_EXPORT void FPDF_CALLCONV FPDFBitmap_Destroy(FPDF_BITMAP bitmap) {
  RetainPtr<CFX_DIBitmap> destroyer;
  destroyer.Unleak(CFXDIBitmapFromFPDFBitmap(bitmap));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_GetPageSizeByIndexF(FPDF_DOCUMENT document,
                         int page_index,
                         FS_SIZEF* size) {
  if (!size) {
    return false;
  }

  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return false;
  }

#ifdef PDF_ENABLE_XFA
  if (page_index < 0 || page_index >= FPDF_GetPageCount(document)) {
    return false;
  }

  auto* context = static_cast<CPDFXFA_Context*>(doc->GetExtension());
  if (context) {
    RetainPtr<CPDFXFA_Page> pPage = context->GetOrCreateXFAPage(page_index);
    if (!pPage) {
      return false;
    }

    size->width = pPage->GetPageWidth();
    size->height = pPage->GetPageHeight();
    return true;
  }
#endif  // PDF_ENABLE_XFA

  RetainPtr<const CPDF_Dictionary> const_dict =
      doc->GetPageDictionary(page_index);
  RetainPtr<CPDF_Dictionary> dict =
      pdfium::WrapRetain(const_cast<CPDF_Dictionary*>(const_dict.Get()));
  if (!dict) {
    return false;
  }

  auto page = pdfium::MakeRetain<CPDF_Page>(doc, std::move(dict));
  page->AddPageImageCache();
  size->width = page->GetPageWidth();
  size->height = page->GetPageHeight();
  return true;
}

static RetainPtr<const CPDF_Dictionary> GetPageDictionaryByIndex(
    FPDF_DOCUMENT document,
    CPDF_Document* doc,
    int page_index);
static int GetInheritedPageRotation(const CPDF_Dictionary* page_dict);

FPDF_EXPORT int FPDF_CALLCONV
EPDF_GetPageRotationByIndex(FPDF_DOCUMENT document, int page_index) {
  auto* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return -1;
  }

  RetainPtr<const CPDF_Dictionary> dict =
      GetPageDictionaryByIndex(document, pDoc, page_index);
  if (!dict) {
    return -1;
  }
  return GetInheritedPageRotation(dict.Get());
}

static RetainPtr<const CPDF_Dictionary> GetPageDictionaryByIndex(
    FPDF_DOCUMENT document,
    CPDF_Document* doc,
    int page_index) {
  if (page_index < 0 || page_index >= FPDF_GetPageCount(document)) {
    return nullptr;
  }
  return doc->GetPageDictionary(page_index);
}

static ByteStringView GetPageBoxKey(EPDF_PAGE_BOX_TYPE box_type) {
  switch (box_type) {
    case EPDF_PAGE_BOX_MEDIA:
      return pdfium::page_object::kMediaBox;
    case EPDF_PAGE_BOX_CROP:
      return pdfium::page_object::kCropBox;
    case EPDF_PAGE_BOX_BLEED:
      return pdfium::page_object::kBleedBox;
    case EPDF_PAGE_BOX_TRIM:
      return pdfium::page_object::kTrimBox;
    case EPDF_PAGE_BOX_ART:
      return pdfium::page_object::kArtBox;
  }
  return ByteStringView();
}

// Walk the page tree (/Parent chain) to resolve an inherited rectangle
// attribute. Mirrors the logic of CPDF_Page::GetPageAttr + GetBox but works
// directly on a dictionary pointer so we can avoid constructing a CPDF_Page.
static CFX_FloatRect GetInheritedRect(const CPDF_Dictionary* page_dict,
                                      ByteStringView name) {
  std::set<const CPDF_Dictionary*> visited;
  const CPDF_Dictionary* dict = page_dict;
  while (dict && !visited.contains(dict)) {
    RetainPtr<const CPDF_Object> object = dict->GetDirectObjectFor(name);
    if (object) {
      RetainPtr<const CPDF_Array> array = ToArray(std::move(object));
      if (array) {
        CFX_FloatRect rect = array->GetRect();
        rect.Normalize();
        return rect;
      }
    }
    visited.insert(dict);
    dict = dict->GetDictFor(pdfium::page_object::kParent).Get();
  }
  return CFX_FloatRect();
}

static int GetInheritedPageRotation(const CPDF_Dictionary* page_dict) {
  std::set<const CPDF_Dictionary*> visited;
  const CPDF_Dictionary* dict = page_dict;
  while (dict && !visited.contains(dict)) {
    if (dict->KeyExist(pdfium::page_object::kRotate)) {
      int rotation =
          (dict->GetIntegerFor(pdfium::page_object::kRotate) / 90) % 4;
      return rotation < 0 ? rotation + 4 : rotation;
    }
    visited.insert(dict);
    dict = dict->GetDictFor(pdfium::page_object::kParent).Get();
  }
  return 0;
}

static CFX_FloatRect GetEffectiveMediaBox(const CPDF_Dictionary* page_dict) {
  CFX_FloatRect media_box =
      GetInheritedRect(page_dict, pdfium::page_object::kMediaBox);
  if (media_box.IsEmpty()) {
    media_box = CFX_FloatRect(0, 0, 612, 792);
  }
  return media_box;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_GetPageSizeByIndexNormalized(FPDF_DOCUMENT document,
                                  int page_index,
                                  FS_SIZEF* size) {
  if (!size) {
    return false;
  }

  auto* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> dict =
      GetPageDictionaryByIndex(document, pDoc, page_index);
  if (!dict) {
    return false;
  }

  // Resolve MediaBox/CropBox via page tree inheritance (not just the page dict)
  CFX_FloatRect mediabox = GetEffectiveMediaBox(dict.Get());
  CFX_FloatRect cropbox =
      GetInheritedRect(dict.Get(), pdfium::page_object::kCropBox);
  CFX_FloatRect bbox = cropbox.IsEmpty() ? mediabox : cropbox;
  bbox.Intersect(mediabox);

  // Return original dimensions - NO swap for rotation
  size->width = bbox.Width();
  size->height = bbox.Height();
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_GetPageBoxByIndex(FPDF_DOCUMENT document,
                       int page_index,
                       EPDF_PAGE_BOX_TYPE box_type,
                       FS_RECTF* box) {
  if (!box) {
    return false;
  }

  auto* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> dict =
      GetPageDictionaryByIndex(document, pDoc, page_index);
  if (!dict) {
    return false;
  }

  CFX_FloatRect rect;
  switch (box_type) {
    case EPDF_PAGE_BOX_MEDIA:
      rect = GetEffectiveMediaBox(dict.Get());
      break;
    case EPDF_PAGE_BOX_CROP:
      rect = GetInheritedRect(dict.Get(), pdfium::page_object::kCropBox);
      if (rect.IsEmpty()) {
        rect = GetEffectiveMediaBox(dict.Get());
      }
      break;
    case EPDF_PAGE_BOX_BLEED:
    case EPDF_PAGE_BOX_TRIM:
    case EPDF_PAGE_BOX_ART:
      rect = GetInheritedRect(dict.Get(), GetPageBoxKey(box_type));
      if (rect.IsEmpty()) {
        return false;
      }
      break;
  }

  if (rect.IsEmpty()) {
    return false;
  }

  *box = FSRectFFromCFXFloatRect(rect);
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
EPDF_GetPageUserUnitByIndex(FPDF_DOCUMENT document,
                            int page_index,
                            float* user_unit) {
  if (!user_unit) {
    return false;
  }

  auto* pDoc = CPDFDocumentFromFPDFDocument(document);
  if (!pDoc) {
    return false;
  }

  RetainPtr<const CPDF_Dictionary> dict =
      GetPageDictionaryByIndex(document, pDoc, page_index);
  if (!dict) {
    return false;
  }

  float value = dict->GetFloatFor("UserUnit");
  *user_unit = value > 0 ? value : 1.0f;
  return true;
}

FPDF_EXPORT FPDF_PAGE FPDF_CALLCONV
EPDF_LoadPageNormalized(FPDF_DOCUMENT document,
                        int page_index,
                        int* out_original_rotation) {
  // Load page normally first
  FPDF_PAGE page = FPDF_LoadPage(document, page_index);
  if (!page) {
    return nullptr;
  }

  CPDF_Page* pPage = CPDFPageFromFPDFPage(page);
  if (!pPage) {
    FPDF_ClosePage(page);
    return nullptr;
  }

  // Store original rotation before we override it
  if (out_original_rotation) {
    *out_original_rotation = pPage->GetOriginalRotation();
  }

  // Force rotation to 0 - this re-runs UpdateDimensions()
  // Now page_size_ and page_matrix_ are calculated as if rotation=0
  pPage->SetRotationOverride(0);

  return page;
}

FPDF_EXPORT int FPDF_CALLCONV FPDF_GetPageSizeByIndex(FPDF_DOCUMENT document,
                                                      int page_index,
                                                      double* width,
                                                      double* height) {
  if (!width || !height) {
    return false;
  }

  FS_SIZEF size;
  if (!FPDF_GetPageSizeByIndexF(document, page_index, &size)) {
    return false;
  }

  *width = size.width;
  *height = size.height;
  return true;
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_VIEWERREF_GetPrintScaling(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return true;
  }
  CPDF_ViewerPreferences viewRef(doc);
  return viewRef.PrintScaling();
}

FPDF_EXPORT int FPDF_CALLCONV
FPDF_VIEWERREF_GetNumCopies(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 1;
  }
  CPDF_ViewerPreferences viewRef(doc);
  return viewRef.NumCopies();
}

FPDF_EXPORT FPDF_PAGERANGE FPDF_CALLCONV
FPDF_VIEWERREF_GetPrintPageRange(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return nullptr;
  }
  CPDF_ViewerPreferences viewRef(doc);

  // Unretained reference in public API. NOLINTNEXTLINE
  return FPDFPageRangeFromCPDFArray(viewRef.PrintPageRange());
}

FPDF_EXPORT size_t FPDF_CALLCONV
FPDF_VIEWERREF_GetPrintPageRangeCount(FPDF_PAGERANGE pagerange) {
  const CPDF_Array* pArray = CPDFArrayFromFPDFPageRange(pagerange);
  return pArray ? pArray->size() : 0;
}

FPDF_EXPORT int FPDF_CALLCONV
FPDF_VIEWERREF_GetPrintPageRangeElement(FPDF_PAGERANGE pagerange,
                                        size_t index) {
  const CPDF_Array* pArray = CPDFArrayFromFPDFPageRange(pagerange);
  if (!pArray || index >= pArray->size()) {
    return -1;
  }
  return pArray->GetIntegerAt(index);
}

FPDF_EXPORT FPDF_DUPLEXTYPE FPDF_CALLCONV
FPDF_VIEWERREF_GetDuplex(FPDF_DOCUMENT document) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return DuplexUndefined;
  }
  CPDF_ViewerPreferences viewRef(doc);
  ByteString duplex = viewRef.Duplex();
  if ("Simplex" == duplex) {
    return Simplex;
  }
  if ("DuplexFlipShortEdge" == duplex) {
    return DuplexFlipShortEdge;
  }
  if ("DuplexFlipLongEdge" == duplex) {
    return DuplexFlipLongEdge;
  }
  return DuplexUndefined;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_VIEWERREF_GetName(FPDF_DOCUMENT document,
                       FPDF_BYTESTRING key,
                       char* buffer,
                       unsigned long length) {
  const CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 0;
  }

  CPDF_ViewerPreferences viewRef(doc);
  std::optional<ByteString> bsVal = viewRef.GenericName(key);
  if (!bsVal.has_value()) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      bsVal.value(), UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, length)));
}

FPDF_EXPORT FPDF_DWORD FPDF_CALLCONV
FPDF_CountNamedDests(FPDF_DOCUMENT document) {
  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return 0;
  }

  const CPDF_Dictionary* pRoot = doc->GetRoot();
  if (!pRoot) {
    return 0;
  }

  auto name_tree = CPDF_NameTree::CreateForReading(doc, "Dests");
  FX_SAFE_UINT32 count = name_tree ? name_tree->GetCount() : 0;
  RetainPtr<const CPDF_Dictionary> pOldStyleDests = pRoot->GetDictFor("Dests");
  if (pOldStyleDests) {
    count += pOldStyleDests->size();
  }
  return count.ValueOrDefault(0);
}

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV
FPDF_GetNamedDestByName(FPDF_DOCUMENT document, FPDF_BYTESTRING name) {
  if (!name || name[0] == 0) {
    return nullptr;
  }

  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return nullptr;
  }

  ByteString dest_name(name);

  // TODO(tsepez): murky ownership, should caller get a reference?
  // Unretained reference in public API. NOLINTNEXTLINE
  return FPDFDestFromCPDFArray(CPDF_NameTree::LookupNamedDest(doc, dest_name));
}

#ifdef PDF_ENABLE_V8
FPDF_EXPORT const char* FPDF_CALLCONV FPDF_GetRecommendedV8Flags() {
  // Use interpreted JS only to avoid RWX pages in our address space. Also,
  // --jitless implies --no-expose-wasm, which reduce exposure since no PDF
  // should contain web assembly.
  return "--jitless";
}

FPDF_EXPORT void* FPDF_CALLCONV FPDF_GetArrayBufferAllocatorSharedInstance() {
  // Deliberately leaked. This allocator is used outside of the library
  // initialization / destruction lifecycle, and the caller does not take
  // ownership of the object. Thus there is no existing way to delete this.
  static auto* s_allocator = new CFX_V8ArrayBufferAllocator();
  return s_allocator;
}
#endif  // PDF_ENABLE_V8

#ifdef PDF_ENABLE_XFA
FPDF_EXPORT FPDF_RESULT FPDF_CALLCONV FPDF_BStr_Init(FPDF_BSTR* bstr) {
  if (!bstr) {
    return -1;
  }

  bstr->str = nullptr;
  bstr->len = 0;
  return 0;
}

FPDF_EXPORT FPDF_RESULT FPDF_CALLCONV FPDF_BStr_Set(FPDF_BSTR* bstr,
                                                    const char* cstr,
                                                    int length) {
  if (!bstr || !cstr) {
    return -1;
  }
  if (length == -1) {
    // SAFETY: required from caller.
    length = pdfium::checked_cast<int>(UNSAFE_BUFFERS(strlen(cstr)));
  }
  if (length == 0) {
    FPDF_BStr_Clear(bstr);
    return 0;
  }

  if (!bstr->str) {
    bstr->str = FX_Alloc(char, length + 1);
  } else if (bstr->len < length) {
    bstr->str = FX_Realloc(char, bstr->str, length + 1);
  }

  // SAFETY: only alloc/realloc is performed above and will ensure at least
  // length + 1 bytes are available.
  UNSAFE_BUFFERS({
    bstr->str[length] = 0;
    FXSYS_memcpy(bstr->str, cstr, length);
  });
  bstr->len = length;
  return 0;
}

FPDF_EXPORT FPDF_RESULT FPDF_CALLCONV FPDF_BStr_Clear(FPDF_BSTR* bstr) {
  if (!bstr) {
    return -1;
  }

  if (bstr->str) {
    FX_Free(bstr->str);
    bstr->str = nullptr;
  }
  bstr->len = 0;
  return 0;
}
#endif  // PDF_ENABLE_XFA

FPDF_EXPORT FPDF_DEST FPDF_CALLCONV FPDF_GetNamedDest(FPDF_DOCUMENT document,
                                                      int index,
                                                      void* buffer,
                                                      long* buflen) {
  if (!buffer) {
    *buflen = 0;
  }

  if (index < 0) {
    return nullptr;
  }

  ScopedFPDFDocumentView document_view(document);
  CPDF_Document* doc = document_view.Get();
  if (!doc) {
    return nullptr;
  }

  const CPDF_Dictionary* pRoot = doc->GetRoot();
  if (!pRoot) {
    return nullptr;
  }

  auto name_tree = CPDF_NameTree::CreateForReading(doc, "Dests");
  size_t name_tree_count = name_tree ? name_tree->GetCount() : 0;
  RetainPtr<const CPDF_Object> pDestObj;
  WideString wsName;
  if (static_cast<size_t>(index) >= name_tree_count) {
    // If |index| is out of bounds, then try to retrieve the Nth old style named
    // destination. Where N is 0-indexed, with N = index - name_tree_count.
    RetainPtr<const CPDF_Dictionary> pDest = pRoot->GetDictFor("Dests");
    if (!pDest) {
      return nullptr;
    }

    FX_SAFE_INT32 checked_count = name_tree_count;
    checked_count += pDest->size();
    if (!checked_count.IsValid() || index >= checked_count.ValueOrDie()) {
      return nullptr;
    }

    index -= name_tree_count;
    int i = 0;
    ByteStringView bsName;
    CPDF_DictionaryLocker locker(pDest);
    for (const auto& it : locker) {
      bsName = it.first.AsStringView();
      pDestObj = it.second;
      if (i == index) {
        break;
      }
      i++;
    }
    wsName = PDF_DecodeText(bsName.unsigned_span());
  } else {
    pDestObj = name_tree->LookupValueAndName(index, &wsName);
  }
  if (!pDestObj) {
    return nullptr;
  }
  if (const CPDF_Dictionary* dict = pDestObj->AsDictionary()) {
    pDestObj = dict->GetArrayFor("D");
    if (!pDestObj) {
      return nullptr;
    }
  }
  if (!pDestObj->IsArray()) {
    return nullptr;
  }

  ByteString utf16Name = wsName.ToUTF16LE();
  int len = pdfium::checked_cast<int>(utf16Name.GetLength());
  if (!buffer) {
    *buflen = len;
  } else if (len <= *buflen) {
    // SAFETY: required from caller.
    auto buffer_span = UNSAFE_BUFFERS(
        pdfium::span(static_cast<char*>(buffer), static_cast<size_t>(*buflen)));
    fxcrt::Copy(utf16Name.span(), buffer_span);
    *buflen = len;
  } else {
    *buflen = -1;
  }
  return FPDFDestFromCPDFArray(pDestObj->AsArray());
}

FPDF_EXPORT int FPDF_CALLCONV FPDF_GetXFAPacketCount(FPDF_DOCUMENT document) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return -1;
  }

  return fxcrt::CollectionSize<int>(
      GetXFAPackets(GetXFAEntryFromDocument(doc)));
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_GetXFAPacketName(FPDF_DOCUMENT document,
                      int index,
                      void* buffer,
                      unsigned long buflen) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || index < 0) {
    return 0;
  }

  std::vector<XFAPacket> xfa_packets =
      GetXFAPackets(GetXFAEntryFromDocument(doc));
  if (static_cast<size_t>(index) >= xfa_packets.size()) {
    return 0;
  }
  // SAFETY: required from caller.
  return NulTerminateMaybeCopyAndReturnLength(
      xfa_packets[index].name,
      UNSAFE_BUFFERS(SpanFromFPDFApiArgs(buffer, buflen)));
}

FPDF_EXPORT FPDF_BOOL FPDF_CALLCONV
FPDF_GetXFAPacketContent(FPDF_DOCUMENT document,
                         int index,
                         void* buffer,
                         unsigned long buflen,
                         unsigned long* out_buflen) {
  CPDF_Document* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc || index < 0 || !out_buflen) {
    return false;
  }

  std::vector<XFAPacket> xfa_packets =
      GetXFAPackets(GetXFAEntryFromDocument(doc));
  if (static_cast<size_t>(index) >= xfa_packets.size()) {
    return false;
  }

  // SAFETY: caller ensures `buffer` points to at least `buflen` bytes.
  *out_buflen = DecodeStreamMaybeCopyAndReturnLength(
      xfa_packets[index].data,
      UNSAFE_BUFFERS(pdfium::span(static_cast<uint8_t*>(buffer),
                                  static_cast<size_t>(buflen))));
  return true;
}

FPDF_EXPORT unsigned long FPDF_CALLCONV
FPDF_GetTrailerEnds(FPDF_DOCUMENT document,
                    unsigned int* buffer,
                    unsigned long length) {
  auto* doc = CPDFDocumentFromFPDFDocument(document);
  if (!doc) {
    return 0;
  }

  // Start recording trailer ends.
  auto* parser = doc->GetParser();
  std::vector<unsigned int> trailer_ends = parser->GetTrailerEnds();
  const unsigned long trailer_ends_len =
      fxcrt::CollectionSize<unsigned long>(trailer_ends);
  if (buffer && length >= trailer_ends_len) {
    // SAFETY: required from caller.
    fxcrt::Copy(trailer_ends, UNSAFE_BUFFERS(pdfium::span(buffer, length)));
  }

  return trailer_ends_len;
}
