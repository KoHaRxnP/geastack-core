// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gea::embedded::ui {

// Read-only native diagnostics. This header intentionally exposes no node or
// style representation and cannot introduce CSS operations behind the source
// analyzer's back. As with Tree, callers must respect the UI task's ownership.
struct TreeInspection {
    static int nodeCount();
    static int mountedWidth();
    static bool hasMountedText(const char *text);
};

} // namespace gea::embedded::ui
