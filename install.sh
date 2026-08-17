#!/bin/sh
set -eu

# default to latest stable release tag
TAG="v1.2.0"
ZIP_DIR="drawbridge-1.2.0"
URL="https://github.com/EC-SH/drawbridge/archive/refs/tags/${TAG}.zip"

# check if we requested bleeding edge / unreleased
REQ_BRANCH="${1:-${DRAWBRIDGE_BRANCH:-${POCKET_DIAL_BRANCH:-}}}"
if [ "$REQ_BRANCH" = "main" ] || [ "$REQ_BRANCH" = "unreleased" ]; then
    echo "Using bleeding-edge (unreleased) main branch..."
    URL="https://github.com/EC-SH/drawbridge/archive/refs/heads/main.zip"
    ZIP_DIR="drawbridge-main"
fi

# check if --service flag is present
INSTALL_SERVICE=false
for arg in "$@"; do
    if [ "$arg" = "--service" ]; then
        INSTALL_SERVICE=true
    fi
done

T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
echo "Downloading drawbridge..."
curl -fsSL "$URL" -o "$T/pd.zip"

unzip -q "$T/pd.zip" -d "$T"
cd "$T/$ZIP_DIR"
chmod +x quickstart.sh

if [ "$INSTALL_SERVICE" = true ]; then
    ./quickstart.sh --service
else
    ./quickstart.sh
fi
