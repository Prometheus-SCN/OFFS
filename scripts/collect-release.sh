#!/bin/bash
set -euo pipefail

TAG="${1:-}"
ARTIFACTS_DIR="${2:-./artifacts}"

if [ -z "$TAG" ]; then
  echo "Usage: $0 <tag> [artifacts-dir]"
  echo ""
  echo "Collects artifacts from all platform builds and creates a GitHub Release."
  echo "Run this after running scripts/release.sh on each platform."
  exit 1
fi

if [ ! -d "$ARTIFACTS_DIR" ]; then
  echo "Artifacts directory not found: $ARTIFACTS_DIR"
  echo "Run scripts/release.sh on each platform first."
  exit 1
fi

echo "=== OFFS Release Collector ==="
echo "Tag: $TAG"
echo "Artifacts: $ARTIFACTS_DIR"

# Check for gh CLI
if ! command -v gh &>/dev/null; then
  echo "ERROR: GitHub CLI (gh) not found. Install it: https://cli.github.com/"
  exit 1
fi

# Check authentication
if ! gh auth status &>/dev/null; then
  echo "ERROR: GitHub CLI not authenticated. Run: gh auth login"
  exit 1
fi

# Generate checksums for release body
echo "Generating checksums..."
CHECKSUMS=""
for f in "$ARTIFACTS_DIR"/*.tar.gz "$ARTIFACTS_DIR"/*.zip \
         "$ARTIFACTS_DIR"/*.deb "$ARTIFACTS_DIR"/*.rpm \
         "$ARTIFACTS_DIR"/*.pkg "$ARTIFACTS_DIR"/*.msi; do
  if [ -f "$f" ]; then
    SHA=$(sha256sum "$f" | cut -d' ' -f1)
    CHECKSUMS="${CHECKSUMS}${SHA}  $(basename "$f")\n"
  fi
done

# Create release
echo "Creating GitHub Release $TAG..."
RELEASE_BODY="OFFS ${TAG}

## Checksums

\`\`\`
$(echo -e "$CHECKSUMS")
\`\`\`"

gh release create "$TAG" \
  --title "OFFS ${TAG}" \
  --notes "$RELEASE_BODY" \
  "$ARTIFACTS_DIR"/*.tar.gz \
  "$ARTIFACTS_DIR"/*.zip \
  "$ARTIFACTS_DIR"/*.deb \
  "$ARTIFACTS_DIR"/*.rpm \
  "$ARTIFACTS_DIR"/*.pkg \
  "$ARTIFACTS_DIR"/*.msi 2>/dev/null || true

echo ""
echo "=== Release complete ==="
echo "View at: https://github.com/Prometheus-SCN/OFFS/releases/tag/${TAG}"
