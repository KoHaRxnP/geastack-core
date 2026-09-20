// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace gea::embedded::ui {

using CssAtomId = std::uint16_t;

constexpr CssAtomId kInvalidCssAtom = 0;

CssAtomId findCssAtom(const char *text, std::size_t length);
CssAtomId findCssAtom(const std::string &text);
CssAtomId internCssAtom(const char *text, std::size_t length);
CssAtomId internCssAtom(const char *text);
CssAtomId internCssAtom(const std::string &text);
const char *cssAtomText(CssAtomId atom);

}  // namespace gea::embedded::ui
