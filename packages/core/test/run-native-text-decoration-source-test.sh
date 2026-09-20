#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# The macOS host sources are in @geastack/apple, which core cannot depend on
# (apple sits above core and depends on this package), so the location is
# supplied rather than guessed. GEA_APPLE_ROOT is the package root -- the
# directory holding its package.json. The old spelling walked to a sibling
# apple checkout and looked under `targets/macos`, which stopped being the layout
# at the split: the package lives at apple/packages/geastack-apple.
APPLE_ROOT="${GEA_APPLE_ROOT:-}"
if [ -z "$APPLE_ROOT" ] || [ ! -d "$APPLE_ROOT/targets/macos" ]; then
  echo "SKIP: set GEA_APPLE_ROOT to the @geastack/apple package root (the directory holding its package.json)" >&2
  exit 0
fi

require_source() {
  local file="$1"
  local pattern="$2"
  local message="$3"
  if ! grep -Fq "$pattern" "$file"; then
    echo "[test_native_text_decoration] $message" >&2
    return 1
  fi
}

require_source "$APPLE_ROOT/targets/macos/main/macos_renderer.mm" \
  "NSUnderlineStyleAttributeName" \
  "macOS native text labels should use AppKit underline attributes"
require_source "$APPLE_ROOT/targets/macos/main/macos_renderer.mm" \
  "NSStrikethroughStyleAttributeName" \
  "macOS native text labels should use AppKit strikethrough attributes"
require_source "$APPLE_ROOT/targets/macos/main/macos_renderer.mm" \
  "node.style.text_decoration" \
  "macOS native text labels should read the framework text-decoration style"

require_source "$APPLE_ROOT/targets/ios/main/ios_main.mm" \
  "NSUnderlineStyleAttributeName" \
  "iOS native text inputs should use UIKit underline attributes"
require_source "$APPLE_ROOT/targets/ios/main/ios_main.mm" \
  "NSStrikethroughStyleAttributeName" \
  "iOS native text inputs should use UIKit strikethrough attributes"
require_source "$APPLE_ROOT/targets/ios/main/ios_main.mm" \
  "defaultTextAttributes" \
  "iOS native text inputs should apply decoration to typed text"
require_source "$APPLE_ROOT/targets/ios/main/ios_main.mm" \
  "attributedText" \
  "iOS native text inputs should apply decoration to synced text"
