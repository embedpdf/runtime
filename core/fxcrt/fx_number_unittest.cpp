// Copyright 2018 The PDFium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/fxcrt/fx_number.h"

#include <stdint.h>

#include <bit>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "core/fxcrt/fx_string.h"
#include "testing/gtest/include/gtest/gtest.h"

TEST(fxnumber, Default) {
  FX_Number number;
  EXPECT_TRUE(number.IsInteger());
  EXPECT_FALSE(number.IsSigned());
  EXPECT_EQ(0, number.GetSigned());
  EXPECT_FLOAT_EQ(0.0f, number.GetFloat());
}

TEST(fxnumber, FromSigned) {
  FX_Number number(-128);
  EXPECT_TRUE(number.IsInteger());
  EXPECT_TRUE(number.IsSigned());
  EXPECT_EQ(-128, number.GetSigned());
  EXPECT_FLOAT_EQ(-128.0f, number.GetFloat());

  // Show that assignment works.
  FX_Number number2 = number;
  EXPECT_TRUE(number2.IsInteger());
  EXPECT_TRUE(number2.IsSigned());
  EXPECT_EQ(-128, number2.GetSigned());
  EXPECT_FLOAT_EQ(-128.0f, number2.GetFloat());
}

TEST(fxnumber, FromFloat) {
  FX_Number number(-100.001f);
  EXPECT_FALSE(number.IsInteger());
  EXPECT_TRUE(number.IsSigned());
  EXPECT_EQ(-100, number.GetSigned());
  EXPECT_FLOAT_EQ(-100.001f, number.GetFloat());

  // Show that assignment works.
  FX_Number number2 = number;
  EXPECT_FALSE(number2.IsInteger());
  EXPECT_TRUE(number2.IsSigned());
  EXPECT_EQ(-100, number2.GetSigned());
  EXPECT_FLOAT_EQ(-100.001f, number2.GetFloat());

  // Show positive saturation.
  FX_Number number3(1e17f);
  EXPECT_FALSE(number3.IsInteger());
  EXPECT_TRUE(number3.IsSigned());
  EXPECT_EQ(std::numeric_limits<int32_t>::max(), number3.GetSigned());

  // Show negative saturation.
  FX_Number number4(-1e17f);
  EXPECT_FALSE(number4.IsInteger());
  EXPECT_TRUE(number4.IsSigned());
  EXPECT_EQ(std::numeric_limits<int32_t>::min(), number4.GetSigned());
}

TEST(fxnumber, FromStringUnsigned) {
  struct TestCase {
    const char* input;
    int expected_output;
  };

  auto test_func = [](pdfium::span<const TestCase> test_cases) {
    for (const auto& test : test_cases) {
      FX_Number number(test.input);
      EXPECT_TRUE(number.IsInteger());
      EXPECT_FALSE(number.IsSigned());
      EXPECT_EQ(test.expected_output, number.GetSigned());
    }
  };

  static constexpr TestCase kNormalCases[] = {
      {"", 0},
      {"0", 0},
      {"10", 10},
  };
  test_func(kNormalCases);

  static constexpr TestCase kOverflowCases[] = {
      {"4223423494965252", 0},
      {"4294967296", 0},
      {"4294967297", 0},
      {"5000000000", 0},
  };
  test_func(kOverflowCases);

  // No explicit sign will allow the number to go negative if retrieved as a
  // signed value. This is needed for things like the encryption permissions
  // flag (Table 3.20 PDF 1.7 spec)
  static constexpr TestCase kNegativeCases[] = {
      {"4294965252", -2044}, {"4294967247", -49}, {"4294967248", -48},
      {"4294967292", -4},    {"4294967295", -1},
  };
  test_func(kNegativeCases);
}

TEST(fxnumber, FromStringSigned) {
  {
    FX_Number number("-0");
    EXPECT_TRUE(number.IsInteger());
    EXPECT_TRUE(number.IsSigned());
    EXPECT_EQ(0, number.GetSigned());
  }
  {
    FX_Number number("+0");
    EXPECT_TRUE(number.IsInteger());
    EXPECT_TRUE(number.IsSigned());
    EXPECT_EQ(0, number.GetSigned());
  }
  {
    FX_Number number("-10");
    EXPECT_TRUE(number.IsInteger());
    EXPECT_TRUE(number.IsSigned());
    EXPECT_EQ(-10, number.GetSigned());
  }
  {
    FX_Number number("+10");
    EXPECT_TRUE(number.IsInteger());
    EXPECT_TRUE(number.IsSigned());
    EXPECT_EQ(10, number.GetSigned());
  }
  {
    FX_Number number("-2147483648");
    EXPECT_TRUE(number.IsInteger());
    EXPECT_TRUE(number.IsSigned());
    EXPECT_EQ(std::numeric_limits<int32_t>::min(), number.GetSigned());
  }
  {
    FX_Number number("+2147483647");
    EXPECT_TRUE(number.IsInteger());
    EXPECT_TRUE(number.IsSigned());
    EXPECT_EQ(std::numeric_limits<int32_t>::max(), number.GetSigned());
  }
  {
    // Value underflows.
    FX_Number number("-2147483649");
    EXPECT_EQ(0, number.GetSigned());
  }
  {
    // Value overflows.
    FX_Number number("+2147483648");
    EXPECT_EQ(0, number.GetSigned());
  }
}

TEST(fxnumber, FromStringFloat) {
  FX_Number number("3.24");
  EXPECT_FLOAT_EQ(3.24f, number.GetFloat());
}

TEST(fxnumber, FromStringDecimalsMatchStringToFloat) {
  // The forms numbers in content streams take, decoded without the general
  // parser, must give StringToFloat()'s float bit for bit.
  std::vector<std::string> strings = {
      "0.5",       ".5",           "5.",           "-.5",
      "-5.",       "+.5",          "+5.25",        "00.50",
      "-0.0",      "-.0",          "0.",           "0.000000000001",
      "-0.1",      "0.3",          "123456.7",     "-254.40491",
      "16777217.", "16777217.5",   "0.1234567",    "999999999999.999",
      "99999999.9999999", "123456789012.345", "3.402823",  "1.000000000001",
  };
  // Values with up to four digits on either side of the point.
  for (int whole = 0; whole < 10000; whole += 37) {
    for (int fraction = 0; fraction < 10000; fraction += 41) {
      std::string fraction_digits = std::to_string(fraction);
      fraction_digits.insert(0, 4 - fraction_digits.size(), '0');
      for (size_t width = 1; width <= 4; ++width) {
        strings.push_back((whole % 2 ? "-" : "") + std::to_string(whole) + "." +
                          fraction_digits.substr(0, width));
      }
    }
  }
  for (const std::string& str : strings) {
    const FX_Number number{ByteStringView(str.c_str())};
    const float expected = StringToFloat(ByteStringView(str.c_str()));
    EXPECT_FALSE(number.IsInteger()) << str;
    EXPECT_EQ(std::bit_cast<uint32_t>(expected),
              std::bit_cast<uint32_t>(number.GetFloat()))
        << str;
  }
}

TEST(fxnumber, FromStringIntegersKeepTheirKind) {
  for (int value = -1000000; value <= 1000000; value += 997) {
    const std::string digits = std::to_string(value < 0 ? -value : value);
    {
      const FX_Number number{ByteStringView(digits.c_str())};
      EXPECT_TRUE(number.IsInteger()) << digits;
      EXPECT_FALSE(number.IsSigned()) << digits;
      EXPECT_EQ(value < 0 ? -value : value, number.GetSigned()) << digits;
    }
    const std::string with_sign = (value < 0 ? "-" : "+") + digits;
    const FX_Number number{ByteStringView(with_sign.c_str())};
    EXPECT_TRUE(number.IsInteger()) << with_sign;
    EXPECT_TRUE(number.IsSigned()) << with_sign;
    EXPECT_EQ(value, number.GetSigned()) << with_sign;
    EXPECT_EQ(static_cast<float>(value), number.GetFloat()) << with_sign;
  }
  // Nine digits and ten digits, on either side of the general code.
  EXPECT_EQ(999999999, FX_Number("999999999").GetSigned());
  EXPECT_EQ(-999999999, FX_Number("-999999999").GetSigned());
  EXPECT_EQ(1234567890, FX_Number("1234567890").GetSigned());
  // "-0" is the integer 0, so its float has no sign.
  EXPECT_FALSE(std::signbit(FX_Number("-0").GetFloat()));
}

TEST(fxnumber, FromStringOtherFormsUseTheGeneralCode) {
  // A sign alone, a point alone, two points, two signs and trailing letters
  // decode as they always have.
  EXPECT_EQ(0, FX_Number("-").GetSigned());
  EXPECT_EQ(0, FX_Number("+").GetSigned());
  EXPECT_EQ(0.0f, FX_Number(".").GetFloat());
  EXPECT_FLOAT_EQ(1.5f, FX_Number("1.5.3").GetFloat());
  EXPECT_FLOAT_EQ(-2.5f, FX_Number("+-2.5").GetFloat());
  EXPECT_FLOAT_EQ(2.5f, FX_Number("-+2.5").GetFloat());
  EXPECT_EQ(0, FX_Number("--5").GetSigned());
  EXPECT_EQ(12, FX_Number("12abc").GetSigned());
  EXPECT_FLOAT_EQ(0.1f, FX_Number("0.1000000000000").GetFloat());
}
