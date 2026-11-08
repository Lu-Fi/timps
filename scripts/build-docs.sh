#!/bin/sh
# Builds the docs site: docs/wiki -> _site/docs (mkdocs-material), next to the project page in site/.
set -e
cd "$(dirname "$0")/.."
rm -rf _docs _site
mkdir -p _docs/assets _site
cp -r docs/wiki/. _docs/
mv _docs/Home.md _docs/index.md
cp site/assets/timps-icon.png site/assets/favicon-64.png _docs/assets/
cp -r site/. _site/
mkdocs build --quiet "$@"
rm -rf _docs
