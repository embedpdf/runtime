// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Original code copyright 2014 Foxit Software Inc. http://www.foxitsoftware.com

#include "core/fxcrt/fx_number.h"

#include <array>
#include <limits>
#include <variant>

#include "core/fxcrt/fx_extension.h"
#include "core/fxcrt/fx_safe_types.h"
#include "core/fxcrt/fx_string.h"
#include "core/fxcrt/numerics/safe_conversions.h"

namespace {

// Powers of ten that a double holds exactly.
constexpr std::array<double, 13> kPowersOfTen = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12};

// Decodes the forms nearly every number in a content stream takes: an
// optional sign, then digits with at most one '.' among them. Gives exactly
// what the general code in FX_Number(ByteStringView) gives, and returns false
// for anything else and for numbers too long to decode this way.
bool DecodeCommonForm(ByteStringView str,
                      std::variant<uint32_t, int32_t, float>& value) {
  const bool negative = str.Front() == '-';
  const bool is_signed = negative || str.Front() == '+';
  uint64_t digits = 0;
  size_t digit_count = 0;
  size_t fraction_digits = 0;
  bool point = false;
  for (const auto ch : str.Substr(is_signed ? 1 : 0)) {
    if (FXSYS_IsDecimalDigit(ch)) {
      // Past 15 digits this may wrap, but such numbers are refused below.
      digits = digits * 10 + (ch - '0');
      ++digit_count;
      fraction_digits += point;
    } else if (ch == '.' && !point) {
      point = true;
    } else {
      return false;
    }
  }
  if (digit_count == 0 || digit_count > 15) {
    return false;
  }

  if (!point) {
    // Nine digits cannot overflow, so these are the general code's results:
    // unsigned without a sign, signed with one.
    if (digit_count > 9) {
      return false;
    }
    const auto magnitude = static_cast<int32_t>(digits);
    if (is_signed) {
      value = negative ? -magnitude : magnitude;
    } else {
      value = static_cast<uint32_t>(magnitude);
    }
    return true;
  }

  if (fraction_digits >= kPowersOfTen.size()) {
    return false;
  }
  // Both operands are exact, so the quotient is the correctly rounded double.
  // With at most 12 fraction digits and 15 digits in all, rounding it to
  // float cannot round twice, so this is the correctly rounded float that
  // StringToFloat() returns. StringToFloat() drops a leading '+'.
  const auto magnitude = static_cast<float>(static_cast<double>(digits) /
                                            kPowersOfTen[fraction_digits]);
  value = negative ? -magnitude : magnitude;
  return true;
}

}  // namespace

FX_Number::FX_Number() = default;

FX_Number::FX_Number(int32_t value) : value_(value) {}

FX_Number::FX_Number(float value) : value_(value) {}

FX_Number::FX_Number(ByteStringView strc) {
  if (strc.IsEmpty() || DecodeCommonForm(strc, value_)) {
    return;
  }

  if (strc.Contains('.')) {
    value_ = StringToFloat(strc);
    return;
  }

  // Note, numbers in PDF are typically of the form 123, -123, etc. But,
  // for things like the Permissions on the encryption hash the number is
  // actually an unsigned value. We use a uint32_t so we can deal with the
  // unsigned and then check for overflow if the user actually signed the value.
  // The Permissions flag is listed in Table 3.20 PDF 1.7 spec.
  FX_SAFE_UINT32 unsigned_val = 0;
  bool bIsSigned = false;
  bool bNegative = false;
  size_t cc = 0;
  if (strc[0] == '+') {
    bIsSigned = true;
    cc++;
  } else if (strc[0] == '-') {
    bIsSigned = true;
    bNegative = true;
    cc++;
  }

  for (; cc < strc.GetLength() && FXSYS_IsDecimalDigit(strc.CharAt(cc)); ++cc) {
    // Deliberately not using FXSYS_DecimalCharToInt() in a tight loop to avoid
    // a duplicate FXSYS_IsDecimalDigit() call. Note that the order of operation
    // is important to avoid unintentional overflows.
    unsigned_val = unsigned_val * 10 + (strc.CharAt(cc) - '0');
  }

  uint32_t uValue = unsigned_val.ValueOrDefault(0);
  if (!bIsSigned) {
    value_ = uValue;
    return;
  }

  // We have a sign, so if the value was greater then the signed integer
  // limits, then we've overflowed and must reset to the default value.
  static constexpr uint32_t uLimit =
      static_cast<uint32_t>(std::numeric_limits<int>::max());

  if (uValue > (bNegative ? uLimit + 1 : uLimit)) {
    uValue = 0;
  }

  // Switch back to the int space so we can flip to a negative if we need.
  int32_t value = static_cast<int32_t>(uValue);
  if (bNegative) {
    // |value| is usually positive, except in the corner case of "-2147483648",
    // where |uValue| is 2147483648. When it gets casted to an int, |value|
    // becomes -2147483648. For this case, avoid undefined behavior, because
    // an int32_t cannot represent 2147483648.
    static constexpr int kMinInt = std::numeric_limits<int>::min();
    value_ = LIKELY(value != kMinInt) ? -value : kMinInt;
  } else {
    value_ = value;
  }
}

bool FX_Number::IsInteger() const {
  return std::holds_alternative<uint32_t>(value_) ||
         std::holds_alternative<int32_t>(value_);
}

bool FX_Number::IsSigned() const {
  return std::holds_alternative<int32_t>(value_) ||
         std::holds_alternative<float>(value_);
}

int32_t FX_Number::GetSigned() const {
  if (std::holds_alternative<uint32_t>(value_)) {
    return static_cast<int32_t>(std::get<uint32_t>(value_));
  }
  if (std::holds_alternative<int32_t>(value_)) {
    return std::get<int32_t>(value_);
  }
  return pdfium::saturated_cast<int32_t>(std::get<float>(value_));
}

float FX_Number::GetFloat() const {
  if (std::holds_alternative<uint32_t>(value_)) {
    return static_cast<float>(std::get<uint32_t>(value_));
  }
  if (std::holds_alternative<int32_t>(value_)) {
    return static_cast<float>(std::get<int32_t>(value_));
  }
  return std::get<float>(value_);
}
