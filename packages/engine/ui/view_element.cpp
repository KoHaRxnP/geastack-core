// SPDX-License-Identifier: Apache-2.0
#include "view.h"

#include "tree_internal.h"

namespace gea::embedded::ui {

ViewElement ViewElement::create()
{
	return ViewElement(Tree::instance().createView());
}

}  // namespace gea::embedded::ui
