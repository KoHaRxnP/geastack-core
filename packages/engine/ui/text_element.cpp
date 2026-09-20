// SPDX-License-Identifier: Apache-2.0
#include "text.h"

#include "tree_internal.h"

namespace gea::embedded::ui {

TextElement TextElement::create(const char *text)
{
	TextElement node(Tree::instance().createText());
	node.textContent(text);
	return node;
}

}  // namespace gea::embedded::ui
