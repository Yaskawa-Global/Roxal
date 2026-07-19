#!/bin/sh
# Download the Freedoom IWADs (BSD-licensed Doom-compatible assets) next to
# this script. The Doom example uses freedoom1.wad automatically when present;
# the checked-in miniwad.wad remains the small default/test asset.
set -e
cd "$(dirname "$0")"

VERSION=0.13.0
ZIP=freedoom-$VERSION.zip
URL=https://github.com/freedoom/freedoom/releases/download/v$VERSION/$ZIP

if [ -f freedoom1.wad ] && [ -f freedoom2.wad ]; then
    echo "freedoom1.wad and freedoom2.wad already present"
    exit 0
fi

echo "Fetching $URL"
curl -L -o "$ZIP" "$URL"
unzip -j -o "$ZIP" "freedoom-$VERSION/freedoom1.wad" "freedoom-$VERSION/freedoom2.wad"
rm -f "$ZIP"
echo "Done: freedoom1.wad, freedoom2.wad"
