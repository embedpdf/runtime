// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

// This "C" (not "C++") file ensures that our public headers compile and link
// for "C" (and not just "C++"), the way fpdf_view_c_api_test.c does for
// upstream's. It lists every function public/epdf_*.h declares;
// scripts/embedpdf-runtime/check-c-api.py checks that it does.

#include <stdio.h>

#include "fpdfsdk/epdf_c_api_test.h"

#include "public/epdf_action.h"
#include "public/epdf_annot.h"
#include "public/epdf_attachment.h"
#include "public/epdf_capture.h"
#include "public/epdf_catalog.h"
#include "public/epdf_digest.h"
#include "public/epdf_doc.h"
#include "public/epdf_edit.h"
#include "public/epdf_flatten.h"
#include "public/epdf_font.h"
#include "public/epdf_form.h"
#include "public/epdf_layer.h"
#include "public/epdf_measure.h"
#include "public/epdf_named_pages.h"
#include "public/epdf_outline.h"
#include "public/epdf_pieceinfo.h"
#include "public/epdf_png.h"
#include "public/epdf_progressive.h"
#include "public/epdf_redact.h"
#include "public/epdf_save.h"
#include "public/epdf_signature.h"
#include "public/epdf_text.h"
#include "public/epdf_view.h"

// Scheme for avoiding LTO out of existence, warnings, etc.
typedef void (*epdf_fnptr)(void);  // Legal generic function type for casts.
epdf_fnptr g_epdf_c_api_test_fnptr = NULL;  // Extern, so can't know it
                                            // doesn't change.
#define CHK(x)                                    \
  if ((epdf_fnptr)(x) == g_epdf_c_api_test_fnptr) \
  return 0

// Function to call from gtest harness to ensure linker resolution.
int CheckEmbedPDFCApi(void) {
  // epdf_action.h
  CHK(EPDFAction_CloseModel);
  CHK(EPDFAction_CreateHide);
  CHK(EPDFAction_CreateJavaScript);
  CHK(EPDFAction_CreateResetForm);
  CHK(EPDFAction_CreateSubmitForm);
  CHK(EPDFAction_GetNextAt);
  CHK(EPDFAction_GetNextCount);
  CHK(EPDFAction_GetNodeCount);
  CHK(EPDFAction_GetNodeDest);
  CHK(EPDFAction_GetNodeFilePath);
  CHK(EPDFAction_GetNodeHideFlag);
  CHK(EPDFAction_GetNodeJavaScript);
  CHK(EPDFAction_GetNodeName);
  CHK(EPDFAction_GetNodeResetForm);
  CHK(EPDFAction_GetNodeSubmitForm);
  CHK(EPDFAction_GetNodeSubmitFormCharSet);
  CHK(EPDFAction_GetNodeSubmitFormURL);
  CHK(EPDFAction_GetNodeSubtype);
  CHK(EPDFAction_GetNodeTargetCount);
  CHK(EPDFAction_GetNodeTargetName);
  CHK(EPDFAction_GetNodeTargetObjectNumber);
  CHK(EPDFAction_GetNodeType);
  CHK(EPDFAction_GetNodeURI);
  CHK(EPDFAction_GetNodeURIIsMap);
  CHK(EPDFAction_GetRootNode);
  CHK(EPDFAction_GetWarningFlags);
  CHK(EPDFAction_IsComplete);
  CHK(EPDFAction_LoadModel);
  CHK(EPDFAction_NodeHasJavaScript);
  CHK(EPDFAction_SetNext);
  CHK(EPDFAnnot_GetActionModel);
  CHK(EPDFAnnot_SetEventAction);
  CHK(EPDFDoc_GetAdditionalActionModel);
  CHK(EPDFDoc_GetNamedJavaScriptActionModel);
  CHK(EPDFDoc_GetOpenActionDest);
  CHK(EPDFDoc_GetOpenActionModel);
  CHK(EPDFDoc_GetPageActionModel);

  // epdf_annot.h
  CHK(EPDFAnnot_ClearBorderEffect);
  CHK(EPDFAnnot_ClearColor);
  CHK(EPDFAnnot_ClearEmbedMetadata);
  CHK(EPDFAnnot_ClearEmbedMetadataKey);
  CHK(EPDFAnnot_ClearMKColor);
  CHK(EPDFAnnot_ClearRectangleDifferences);
  CHK(EPDFAnnot_ExportAppearance);
  CHK(EPDFAnnot_GenerateAppearance);
  CHK(EPDFAnnot_GenerateAppearanceWithBlend);
  CHK(EPDFAnnot_GenerateFormFieldAP);
  CHK(EPDFAnnot_GetAPMatrix);
  CHK(EPDFAnnot_GetAppearanceState);
  CHK(EPDFAnnot_GetAppearanceStateCount);
  CHK(EPDFAnnot_GetAppearanceStateName);
  CHK(EPDFAnnot_GetAppearanceTransform);
  CHK(EPDFAnnot_GetAvailableAppearanceModes);
  CHK(EPDFAnnot_GetBlendMode);
  CHK(EPDFAnnot_GetBooleanValue);
  CHK(EPDFAnnot_GetBorderDashPattern);
  CHK(EPDFAnnot_GetBorderDashPatternCount);
  CHK(EPDFAnnot_GetBorderEffect);
  CHK(EPDFAnnot_GetBorderStyle);
  CHK(EPDFAnnot_GetCalloutLine);
  CHK(EPDFAnnot_GetCalloutLineCount);
  CHK(EPDFAnnot_GetColor);
  CHK(EPDFAnnot_GetDefaultAppearance);
  CHK(EPDFAnnot_GetDefaultAppearanceFontFace);
  CHK(EPDFAnnot_GetDrawingRect);
  CHK(EPDFAnnot_GetEmbedMetadataBoolean);
  CHK(EPDFAnnot_GetEmbedMetadataJSON);
  CHK(EPDFAnnot_GetEmbedMetadataNumber);
  CHK(EPDFAnnot_GetEmbedMetadataRect);
  CHK(EPDFAnnot_GetEmbedMetadataString);
  CHK(EPDFAnnot_GetFileAttachmentForWrite);
  CHK(EPDFAnnot_GetIntent);
  CHK(EPDFAnnot_GetLineEndings);
  CHK(EPDFAnnot_GetMKColor);
  CHK(EPDFAnnot_GetMKRotation);
  CHK(EPDFAnnot_GetMKText);
  CHK(EPDFAnnot_GetMKTextPosition);
  CHK(EPDFAnnot_GetName);
  CHK(EPDFAnnot_GetObjectNumber);
  CHK(EPDFAnnot_GetOpacity);
  CHK(EPDFAnnot_GetOverlayText);
  CHK(EPDFAnnot_GetOverlayTextRepeat);
  CHK(EPDFAnnot_GetRect);
  CHK(EPDFAnnot_GetRectangleDifferences);
  CHK(EPDFAnnot_GetReplyType);
  CHK(EPDFAnnot_GetRichContent);
  CHK(EPDFAnnot_GetRichTextJSON);
  CHK(EPDFAnnot_GetRotate);
  CHK(EPDFAnnot_GetStampDrawing);
  CHK(EPDFAnnot_GetTextAlignment);
  CHK(EPDFAnnot_HasAppearanceStream);
  CHK(EPDFAnnot_HasEmbedMetadata);
  CHK(EPDFAnnot_IsValid);
  CHK(EPDFAnnot_RemoveAction);
  CHK(EPDFAnnot_RemoveDest);
  CHK(EPDFAnnot_RemoveKey);
  CHK(EPDFAnnot_SetAPMatrix);
  CHK(EPDFAnnot_SetAction);
  CHK(EPDFAnnot_SetAppearanceFromPage);
  CHK(EPDFAnnot_SetBooleanValue);
  CHK(EPDFAnnot_SetBorderDashPattern);
  CHK(EPDFAnnot_SetBorderEffect);
  CHK(EPDFAnnot_SetBorderStyle);
  CHK(EPDFAnnot_SetCalloutLine);
  CHK(EPDFAnnot_SetColor);
  CHK(EPDFAnnot_SetDefaultAppearance);
  CHK(EPDFAnnot_SetDefaultAppearanceRegisteredFont);
  CHK(EPDFAnnot_SetEmbedMetadataBoolean);
  CHK(EPDFAnnot_SetEmbedMetadataJSON);
  CHK(EPDFAnnot_SetEmbedMetadataNumber);
  CHK(EPDFAnnot_SetEmbedMetadataRect);
  CHK(EPDFAnnot_SetEmbedMetadataString);
  CHK(EPDFAnnot_SetIntent);
  CHK(EPDFAnnot_SetLine);
  CHK(EPDFAnnot_SetLineEndings);
  CHK(EPDFAnnot_SetLinkedAnnot);
  CHK(EPDFAnnot_SetMKColor);
  CHK(EPDFAnnot_SetMKRotation);
  CHK(EPDFAnnot_SetMKText);
  CHK(EPDFAnnot_SetMKTextPosition);
  CHK(EPDFAnnot_SetName);
  CHK(EPDFAnnot_SetNumberValue);
  CHK(EPDFAnnot_SetOpacity);
  CHK(EPDFAnnot_SetOverlayText);
  CHK(EPDFAnnot_SetOverlayTextRepeat);
  CHK(EPDFAnnot_SetRect);
  CHK(EPDFAnnot_SetReplyType);
  CHK(EPDFAnnot_SetRichTextJSON);
  CHK(EPDFAnnot_SetRichTextXHTML);
  CHK(EPDFAnnot_SetRotate);
  CHK(EPDFAnnot_SetStampDrawing);
  CHK(EPDFAnnot_SetStampOpacity);
  CHK(EPDFAnnot_SetTextAlignment);
  CHK(EPDFAnnot_SetVertices);
  CHK(EPDFAnnot_UpdateAppearanceToRect);
  CHK(EPDFDoc_CanonicalDrawing);
  CHK(EPDFDoc_ExportDrawing);
  CHK(EPDFDoc_GetStampDrawings);
  CHK(EPDFDoc_GetTypographicFeatures);
  CHK(EPDFDoc_ImportDrawing);
  CHK(EPDFDoc_SetTypographicFeatures);
  CHK(EPDFDocument_ClearEmbedMetadata);
  CHK(EPDFPage_CreateAnnot);
  CHK(EPDFPage_CreateAnnotRaw);
  CHK(EPDFPage_GetAnnotByName);
  CHK(EPDFPage_GetAnnotByObjectNumber);
  CHK(EPDFPage_GetAnnotCountRaw);
  CHK(EPDFPage_GetAnnotIndexByNameRaw);
  CHK(EPDFPage_GetAnnotIndexByObjectNumberRaw);
  CHK(EPDFPage_GetAnnotRaw);
  CHK(EPDFPage_MoveAnnots);
  CHK(EPDFPage_MoveAnnotsRaw);
  CHK(EPDFPage_PromoteInlineAnnotsRaw);
  CHK(EPDFPage_RemoveAnnot);
  CHK(EPDFPage_RemoveAnnotByName);
  CHK(EPDFPage_RemoveAnnotByObjectNumber);
  CHK(EPDFPage_RemoveAnnotRaw);

  // epdf_attachment.h
  CHK(EPDFAttachment_ExtractFile);
  CHK(EPDFAttachment_ExtractFileToOwnedBuffer);
  CHK(EPDFAttachment_GetDescription);
  CHK(EPDFAttachment_GetIntegerValue);
  CHK(EPDFAttachment_SetDescription);
  CHK(EPDFAttachment_SetName);
  CHK(EPDFAttachment_SetSubtype);
  CHK(EPDFDoc_GetAttachmentIndexByKey);
  CHK(EPDFDoc_GetAttachmentKey);

  // epdf_capture.h
  CHK(EPDFDoc_ExportDictRawToOwnedBuffer);
  CHK(EPDFDoc_ImportDictRaw);
  CHK(EPDFForm_ExportFieldRawToOwnedBuffer);
  CHK(EPDFForm_ImportFieldRaw);
  CHK(EPDFOutline_ExportItemRawToOwnedBuffer);
  CHK(EPDFOutline_ImportItemRaw);
  CHK(EPDFPage_ExportAnnotsRawToOwnedBuffer);
  CHK(EPDFPage_ExportRawToOwnedBuffer);
  CHK(EPDFPage_ImportAnnotsRaw);
  CHK(EPDFPage_ImportRaw);

  // epdf_catalog.h
  CHK(EPDFCatalog_GetLanguage);

  // epdf_digest.h
  CHK(EPDF_DigestBuffer);

  // epdf_doc.h
  CHK(EPDFAction_CreateGoTo);
  CHK(EPDFAction_CreateGoToNamed);
  CHK(EPDFAction_CreateLaunch);
  CHK(EPDFAction_CreateNamed);
  CHK(EPDFAction_CreateRemoteGoToByName);
  CHK(EPDFAction_CreateRemoteGoToDest);
  CHK(EPDFAction_CreateURI);
  CHK(EPDFDest_CreateRemoteView);
  CHK(EPDFDest_CreateRemoteXYZ);
  CHK(EPDFDest_CreateView);
  CHK(EPDFDest_CreateViewByObjectNumber);
  CHK(EPDFDest_CreateXYZ);
  CHK(EPDFDest_CreateXYZByObjectNumber);
  CHK(EPDFDest_GetPageObjectNumber);
  CHK(EPDFDest_GetViewNullParams);
  CHK(EPDFNamedDest_Remove);
  CHK(EPDFNamedDest_SetDest);
  CHK(EPDF_GetMetaKeyCount);
  CHK(EPDF_GetMetaKeyName);
  CHK(EPDF_GetMetaTrapped);
  CHK(EPDF_HasMetaText);
  CHK(EPDF_SetMetaText);
  CHK(EPDF_SetMetaTrapped);

  // epdf_edit.h
  CHK(EPDFImageObj_SetJpeg);
  CHK(EPDFImageObj_SetPng);
  CHK(EPDFPage_InsertBlankRaw);
  CHK(EPDFText_RedactInQuads);
  CHK(EPDFText_RedactInRect);

  // epdf_flatten.h
  CHK(EPDFDoc_ExportPagesFlattened);
  CHK(EPDFPage_ExportAnnotationsAsDocument);
  CHK(EPDFPage_Flatten);
  CHK(EPDFPage_FlattenAnnotations);

  // epdf_font.h
  CHK(EPDFDoc_GetFontEmbeddingPolicy);
  CHK(EPDFDoc_SetFontEmbeddingPolicy);
  CHK(EPDFFont_AddFallbackFont);
  CHK(EPDFFont_AuthorizeEditing);
  CHK(EPDFFont_ClearFallbackFonts);
  CHK(EPDFFont_ClearRegisteredFonts);
  CHK(EPDFFont_GetEmbeddingPermission);
  CHK(EPDFFont_GetFamilyName);
  CHK(EPDFFont_GetWeight);
  CHK(EPDFFont_IsEditingAuthorized);
  CHK(EPDFFont_IsInstanced);
  CHK(EPDFFont_IsItalic);
  CHK(EPDFFont_RegisterFont);
  CHK(EPDFFont_RegisterMemFont);
  CHK(EPDFFont_RegisterMemFont64);

  // epdf_form.h
  CHK(EPDFForm_AttachWidget);
  CHK(EPDFForm_ClearFieldEmbedMetadata);
  CHK(EPDFForm_ClearFieldEmbedMetadataKey);
  CHK(EPDFForm_CloseModel);
  CHK(EPDFForm_CountCalculationOrder);
  CHK(EPDFForm_CountFieldDefaultValues);
  CHK(EPDFForm_CountFieldOptions);
  CHK(EPDFForm_CountFieldValues);
  CHK(EPDFForm_CountFieldWidgets);
  CHK(EPDFForm_CountFields);
  CHK(EPDFForm_CreateField);
  CHK(EPDFForm_DeleteField);
  CHK(EPDFForm_DetachWidget);
  CHK(EPDFForm_ExportFDF);
  CHK(EPDFForm_ExportXFDF);
  CHK(EPDFForm_GetCalculationOrderFieldIndex);
  CHK(EPDFForm_GetFieldActionModel);
  CHK(EPDFForm_GetFieldAlternateName);
  CHK(EPDFForm_GetFieldDefaultValueAt);
  CHK(EPDFForm_GetFieldDefaultValueKind);
  CHK(EPDFForm_GetFieldEmbedMetadataBoolean);
  CHK(EPDFForm_GetFieldEmbedMetadataJSON);
  CHK(EPDFForm_GetFieldEmbedMetadataNumber);
  CHK(EPDFForm_GetFieldEmbedMetadataString);
  CHK(EPDFForm_GetFieldFamily);
  CHK(EPDFForm_GetFieldFlags);
  CHK(EPDFForm_GetFieldIndexByObjNum);
  CHK(EPDFForm_GetFieldIndexForWidget);
  CHK(EPDFForm_GetFieldMappingName);
  CHK(EPDFForm_GetFieldMaxLen);
  CHK(EPDFForm_GetFieldName);
  CHK(EPDFForm_GetFieldObjNum);
  CHK(EPDFForm_GetFieldOptionLabel);
  CHK(EPDFForm_GetFieldOptionValue);
  CHK(EPDFForm_GetFieldOrigin);
  CHK(EPDFForm_GetFieldTopIndex);
  CHK(EPDFForm_GetFieldValueAt);
  CHK(EPDFForm_GetFieldValueKind);
  CHK(EPDFForm_GetFieldWidgetExportValue);
  CHK(EPDFForm_GetFieldWidgetObjNum);
  CHK(EPDFForm_GetFieldWidgetOnState);
  CHK(EPDFForm_GetFieldWidgetPageObjNum);
  CHK(EPDFForm_GetFieldWidgetRect);
  CHK(EPDFForm_GetFormKind);
  CHK(EPDFForm_GetNeedAppearances);
  CHK(EPDFForm_HasFieldEmbedMetadata);
  CHK(EPDFForm_IsFieldOptionSelected);
  CHK(EPDFForm_IsFieldWidgetChecked);
  CHK(EPDFForm_LoadModel);
  CHK(EPDFForm_RedrawFieldWidgets);
  CHK(EPDFForm_Repair);
  CHK(EPDFForm_SetCalculationOrder);
  CHK(EPDFForm_SetFieldAlternateName);
  CHK(EPDFForm_SetFieldAppearanceText);
  CHK(EPDFForm_SetFieldDefaultValue);
  CHK(EPDFForm_SetFieldDisplay);
  CHK(EPDFForm_SetFieldEmbedMetadataBoolean);
  CHK(EPDFForm_SetFieldEmbedMetadataJSON);
  CHK(EPDFForm_SetFieldEmbedMetadataNumber);
  CHK(EPDFForm_SetFieldEmbedMetadataString);
  CHK(EPDFForm_SetFieldEventAction);
  CHK(EPDFForm_SetFieldFlags);
  CHK(EPDFForm_SetFieldMappingName);
  CHK(EPDFForm_SetFieldMaxLen);
  CHK(EPDFForm_SetFieldName);
  CHK(EPDFForm_SetFieldOptions);
  CHK(EPDFForm_SetFieldSelectedIndices);
  CHK(EPDFForm_SetFieldValue);
  CHK(EPDFForm_SetFieldWidgetsChecked);

  // epdf_layer.h
  CHK(EPDFLayer_AbortTransaction);
  CHK(EPDFLayer_BeginTransaction);
  CHK(EPDFLayer_CommitTransaction);
  CHK(EPDFLayer_GetBaseDocument);
  CHK(EPDFLayer_GetBaseSha256);
  CHK(EPDFLayer_GetBirthName);
  CHK(EPDFLayer_GetBirthObjectNumber);
  CHK(EPDFLayer_GetLastObjectNumber);
  CHK(EPDFLayer_GetPromotedObjectCount);
  CHK(EPDFLayer_IsInTransaction);
  CHK(EPDFLayer_IsObjectPromoted);
  CHK(EPDFLayer_IsPagePromoted);
  CHK(EPDFLayer_OpenLayer);
  CHK(EPDFLayer_OpenLayerArtifact);
  CHK(EPDFLayer_OpenLayerArtifactFromPath);
  CHK(EPDFLayer_RaiseLastObjectNumber);
  CHK(EPDFPage_IsValid);
  CHK(EPDF_LoadBaseDocument);
  CHK(EPDF_LoadMemBaseDocument);
  CHK(EPDF_LoadMemBaseDocument64);
  CHK(EPDF_ReleaseBaseDocument);
  CHK(EPDF_SetBaseDocumentSha256);

  // epdf_measure.h
  CHK(EPDFAnnot_AddMeasure);
  CHK(EPDFAnnot_GetLineCaption);
  CHK(EPDFAnnot_GetLineLeader);
  CHK(EPDFAnnot_GetMeasure);
  CHK(EPDFAnnot_GetShapeCaption);
  CHK(EPDFAnnot_RemoveMeasure);
  CHK(EPDFAnnot_SetLineCaption);
  CHK(EPDFAnnot_SetLineLeader);
  CHK(EPDFAnnot_SetShapeCaption);
  CHK(EPDFMeasure_AddFormat);
  CHK(EPDFMeasure_CountFormats);
  CHK(EPDFMeasure_GetCYX);
  CHK(EPDFMeasure_GetFormat);
  CHK(EPDFMeasure_GetOrigin);
  CHK(EPDFMeasure_GetRatio);
  CHK(EPDFMeasure_GetSubtype);
  CHK(EPDFMeasure_RemoveFormats);
  CHK(EPDFMeasure_SetCYX);
  CHK(EPDFMeasure_SetOrigin);
  CHK(EPDFMeasure_SetRatio);
  CHK(EPDFNumberFormat_GetConversion);
  CHK(EPDFNumberFormat_GetFixedDenominator);
  CHK(EPDFNumberFormat_GetFraction);
  CHK(EPDFNumberFormat_GetLabelPosition);
  CHK(EPDFNumberFormat_GetPrecision);
  CHK(EPDFNumberFormat_GetText);
  CHK(EPDFNumberFormat_GetUnit);
  CHK(EPDFNumberFormat_SetConversion);
  CHK(EPDFNumberFormat_SetFixedDenominator);
  CHK(EPDFNumberFormat_SetFraction);
  CHK(EPDFNumberFormat_SetLabelPosition);
  CHK(EPDFNumberFormat_SetPrecision);
  CHK(EPDFNumberFormat_SetText);
  CHK(EPDFNumberFormat_SetUnit);
  CHK(EPDFPage_AddViewport);
  CHK(EPDFPage_CountViewports);
  CHK(EPDFPage_FindViewport);
  CHK(EPDFPage_GetViewport);
  CHK(EPDFPage_RemoveViewport);
  CHK(EPDFViewport_AddMeasure);
  CHK(EPDFViewport_GetBBox);
  CHK(EPDFViewport_GetMeasure);
  CHK(EPDFViewport_GetName);
  CHK(EPDFViewport_RemoveMeasure);
  CHK(EPDFViewport_SetBBox);
  CHK(EPDFViewport_SetName);

  // epdf_named_pages.h
  CHK(EPDFDoc_GetNamedPageAt);
  CHK(EPDFDoc_GetNamedPageCount);
  CHK(EPDFDoc_RemoveNamedPage);
  CHK(EPDFDoc_RemoveNamedPagesForPage);
  CHK(EPDFDoc_SetNamedPage);

  // epdf_outline.h
  CHK(EPDFOutline_CreateItem);
  CHK(EPDFOutline_DeleteItem);
  CHK(EPDFOutline_GetItem);
  CHK(EPDFOutline_GetObjectNumber);
  CHK(EPDFOutline_MoveItem);
  CHK(EPDFOutline_SetAction);
  CHK(EPDFOutline_SetColor);
  CHK(EPDFOutline_SetOpen);
  CHK(EPDFOutline_SetStyle);
  CHK(EPDFOutline_SetTitle);

  // epdf_pieceinfo.h
  CHK(EPDFDoc_ClearPagePieceInfoEntry);
  CHK(EPDFDoc_ClearPagePieceInfoKey);
  CHK(EPDFDoc_ClearPieceInfoEntry);
  CHK(EPDFDoc_ClearPieceInfoKey);
  CHK(EPDFDoc_GetLastModified);
  CHK(EPDFDoc_GetPageLastModified);
  CHK(EPDFDoc_GetPagePieceInfoBoolean);
  CHK(EPDFDoc_GetPagePieceInfoEntryAt);
  CHK(EPDFDoc_GetPagePieceInfoEntryCount);
  CHK(EPDFDoc_GetPagePieceInfoKeyAt);
  CHK(EPDFDoc_GetPagePieceInfoKeyCount);
  CHK(EPDFDoc_GetPagePieceInfoLastModified);
  CHK(EPDFDoc_GetPagePieceInfoName);
  CHK(EPDFDoc_GetPagePieceInfoNumber);
  CHK(EPDFDoc_GetPagePieceInfoString);
  CHK(EPDFDoc_GetPagePieceInfoStringArrayAt);
  CHK(EPDFDoc_GetPagePieceInfoStringArrayCount);
  CHK(EPDFDoc_GetPagePieceInfoValueType);
  CHK(EPDFDoc_GetPieceInfoBoolean);
  CHK(EPDFDoc_GetPieceInfoEntryAt);
  CHK(EPDFDoc_GetPieceInfoEntryCount);
  CHK(EPDFDoc_GetPieceInfoKeyAt);
  CHK(EPDFDoc_GetPieceInfoKeyCount);
  CHK(EPDFDoc_GetPieceInfoLastModified);
  CHK(EPDFDoc_GetPieceInfoName);
  CHK(EPDFDoc_GetPieceInfoNumber);
  CHK(EPDFDoc_GetPieceInfoString);
  CHK(EPDFDoc_GetPieceInfoStringArrayAt);
  CHK(EPDFDoc_GetPieceInfoStringArrayCount);
  CHK(EPDFDoc_GetPieceInfoValueType);
  CHK(EPDFDoc_HasPagePieceInfoEntry);
  CHK(EPDFDoc_HasPieceInfoEntry);
  CHK(EPDFDoc_SetPagePieceInfoBoolean);
  CHK(EPDFDoc_SetPagePieceInfoName);
  CHK(EPDFDoc_SetPagePieceInfoNumber);
  CHK(EPDFDoc_SetPagePieceInfoString);
  CHK(EPDFDoc_SetPagePieceInfoStringArray);
  CHK(EPDFDoc_SetPieceInfoBoolean);
  CHK(EPDFDoc_SetPieceInfoName);
  CHK(EPDFDoc_SetPieceInfoNumber);
  CHK(EPDFDoc_SetPieceInfoString);
  CHK(EPDFDoc_SetPieceInfoStringArray);

  // epdf_png.h
  CHK(EPDF_PNG_EncodeRGBA);

  // epdf_progressive.h
  CHK(EPDF_RenderPageBitmapWithMatrix_Start);
  CHK(EPDF_RenderPage_Continue);

  // epdf_redact.h
  CHK(EPDFPage_ApplyRedactions);

  // epdf_save.h
  CHK(EPDFLayer_SaveDelta);
  CHK(EPDFLayer_SaveDeltaEx);
  CHK(EPDFLayer_SaveDeltaToOwnedBuffer);
  CHK(EPDFLayer_SaveDeltaToOwnedBufferEx);
  CHK(EPDFLayer_SaveLayerArtifact);
  CHK(EPDFLayer_SaveLayerArtifactEx);
  CHK(EPDFLayer_SaveLayerArtifactToOwnedBuffer);
  CHK(EPDFLayer_SaveLayerArtifactToOwnedBufferEx);
  CHK(EPDF_FreeBuffer);
  CHK(EPDF_SaveAsCopyEx);
  CHK(EPDF_SaveDocumentToOwnedBuffer);
  CHK(EPDF_SaveDocumentToOwnedBufferEx);
  CHK(EPDF_SaveDocumentToOwnedBufferWithVersion);

  // epdf_signature.h
  CHK(EPDFDoc_CompareRevisions);
  CHK(EPDFDoc_GetBaseBytesSize);
  CHK(EPDFDoc_GetLoadedBytesSize);
  CHK(EPDFDoc_GetRevision);
  CHK(EPDFDoc_GetRevisionCount);
  CHK(EPDFDoc_GetStructureObjectNumbers);
  CHK(EPDFDoc_OpenBaseOverlay);
  CHK(EPDFDoc_OpenBaseOverlayFromPath);
  CHK(EPDFDoc_OpenRevision);
  CHK(EPDFDoc_ReadLoadedBytes);
  CHK(EPDFObjectDiff_Close);
  CHK(EPDFObjectDiff_GetCount);
  CHK(EPDFObjectDiff_GetEntry);
  CHK(EPDFObjectDiff_GetReadStatus);
  CHK(EPDFObjectDiff_GetReferrer);
  CHK(EPDFObjectDiff_GetReferrerCount);
  CHK(EPDFObjectDiff_GetRevisionHealth);
  CHK(EPDFObjectDiff_GetValue);
  CHK(EPDFSig_CloseModel);
  CHK(EPDFSig_Count);
  CHK(EPDFSig_DigestByteRange);
  CHK(EPDFSig_DigestFileRange);
  CHK(EPDFSig_GetByteRange);
  CHK(EPDFSig_GetContents);
  CHK(EPDFSig_GetCoverage);
  CHK(EPDFSig_GetDocMDPPermission);
  CHK(EPDFSig_GetFieldMDPAction);
  CHK(EPDFSig_GetFieldName);
  CHK(EPDFSig_GetFieldNameAt);
  CHK(EPDFSig_GetFieldNameCount);
  CHK(EPDFSig_GetFieldObjNum);
  CHK(EPDFSig_GetIndexByFieldObjNum);
  CHK(EPDFSig_GetKind);
  CHK(EPDFSig_GetLockAction);
  CHK(EPDFSig_GetLockPermission);
  CHK(EPDFSig_GetRevisionIndex);
  CHK(EPDFSig_GetSeedValueFilter);
  CHK(EPDFSig_GetSeedValueListAt);
  CHK(EPDFSig_GetSeedValueListCount);
  CHK(EPDFSig_GetSeedValueMDP);
  CHK(EPDFSig_GetSeedValuePresentFlags);
  CHK(EPDFSig_GetSeedValueRequiredFlags);
  CHK(EPDFSig_GetSeedValueVersion);
  CHK(EPDFSig_GetString);
  CHK(EPDFSig_GetValueObjNum);
  CHK(EPDFSig_GetWidgetObjNum);
  CHK(EPDFSig_GetWidgetPageObjNum);
  CHK(EPDFSig_HasSeedValue);
  CHK(EPDFSig_IsCatalogCertification);
  CHK(EPDFSig_IsRevisionChainValid);
  CHK(EPDFSig_IsSigned);
  CHK(EPDFSig_LoadModel);
  CHK(EPDFSig_Prepare);
  CHK(EPDFSig_SaveCandidate);
  CHK(EPDFSig_SaveCandidateToOwnedBuffer);
  CHK(EPDFSig_Seal);
  CHK(EPDFSig_SealSpan);
  CHK(EPDFSig_SetFieldLock);
  CHK(EPDFSig_WriteContents);

  // epdf_text.h
  CHK(EPDFText_GetCharGeometry);
  CHK(EPDFText_GetCharToTextMap);
  CHK(EPDFText_GetTextFull);

  // epdf_view.h
  CHK(EPDFDoc_DeletePageByObjectNumber);
  CHK(EPDFDoc_GetPageObjectNumberByIndex);
  CHK(EPDFDoc_LoadPageByObjectNumber);
  CHK(EPDFDoc_LoadPageByObjectNumberNormalized);
  CHK(EPDFDoc_SetPageRotationByObjectNumber);
  CHK(EPDFDoc_StartLoadPageByObjectNumber);
  CHK(EPDFPage_ContinueLoad);
  CHK(EPDFPage_GetObjectNumber);
  CHK(EPDFPage_GetParsedSize);
  CHK(EPDFPage_IsContentCurrent);
  CHK(EPDFPage_ResetRenderCache);
  CHK(EPDF_CheckPasswordPermissions);
  CHK(EPDF_GetDecodedImageBytes);
  CHK(EPDF_GetPageBoxByIndex);
  CHK(EPDF_GetPageRotateByIndex);
  CHK(EPDF_GetPageTabOrderByIndex);
  CHK(EPDF_GetPageUserUnitByIndex);
  CHK(EPDF_InitThread);
  CHK(EPDF_IsEncrypted);
  CHK(EPDF_IsOwnerUnlocked);
  CHK(EPDF_LoadPageNormalized);
  CHK(EPDF_RemoveEncryption);
  CHK(EPDF_RenderAnnotBitmap);
  CHK(EPDF_RenderAnnotBitmapUnrotated);
  CHK(EPDF_SetDecodedImageBudget);
  CHK(EPDF_SetEncryption);
  CHK(EPDF_SetRuntimeOwnerPermissions);
  CHK(EPDF_ShutdownThread);
  CHK(EPDF_UnlockOwnerPermissions);

  return 1;
}

#undef CHK
