// SPDX-License-Identifier: Apache-2.0
#include "image.h"

#include "tree_internal.h"

namespace gea::embedded::ui {

ImageElement ImageElement::create()
{
	return ImageElement(Tree::instance().createImage());
}

}  // namespace gea::embedded::ui
