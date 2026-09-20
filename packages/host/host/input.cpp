// SPDX-License-Identifier: Apache-2.0
#include "gea/embedded-host.h"
#include "input.h"

namespace gea::framework::input {

bool InputBackend::consumeBackButton()
{
	return gea::framework::input::consumeBackButton();
}

}  // namespace gea::framework::input
