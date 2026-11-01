// Copyright 2026 CloudPDF LTD
// SPDX-License-Identifier: Apache-2.0

#ifndef FPDFSDK_EPDF_NAMED_PAGES_HELPERS_H_
#define FPDFSDK_EPDF_NAMED_PAGES_HELPERS_H_

#include <stdint.h>

#include <vector>

#include "core/fxcrt/widestring.h"

class CPDF_Document;

// The catalog's /Names /Pages tree (public/epdf_named_pages.h), for the fork's
// own callers. Callers open a CPDF_DocumentViewScope.
namespace epdf {

// The keys whose value is the page |page_objnum|, in tree order.
std::vector<WideString> NamedPageKeysOf(CPDF_Document* doc,
                                        uint32_t page_objnum);

// Registers |key| -> the page |page_objnum| unless a registration holds |key|
// already, which then keeps it. False when one does, when |key| is empty, or
// when the page isn't in the page tree.
bool AddNamedPageIfFree(CPDF_Document* doc,
                        const WideString& key,
                        uint32_t page_objnum);

}  // namespace epdf

#endif  // FPDFSDK_EPDF_NAMED_PAGES_HELPERS_H_
