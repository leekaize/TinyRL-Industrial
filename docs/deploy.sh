#!/usr/bin/env bash
# Build the VitePress site and deploy it to Cloudflare Pages (direct upload).
# Run from anywhere; the script cd's to docs/ where package.json lives.
#
# First-time setup (once):
#   1. npx wrangler login
#      (opens a browser; on WSL copy the printed URL into the Windows browser,
#       the localhost callback reaches the WSL server)
#      -- or, without a browser login:
#      export CLOUDFLARE_API_TOKEN=<token with Cloudflare Pages:Edit>
#   2. npx wrangler pages project create tinyrl-industrial --production-branch main
#
# Usage:
#   bash docs/deploy.sh        (from the repo root)
#   npm run deploy             (from docs/)
#
# Nothing here commits or pushes; the script only builds and uploads.
set -euo pipefail
cd "$(dirname "$0")"

PROJECT="tinyrl-industrial"

echo "==> parse-checking mermaid diagrams"
node parse_diagrams.mjs

echo "==> building docs"
npm run build

echo "==> deploying to Cloudflare Pages (project: $PROJECT)"
npx wrangler pages deploy .vitepress/dist --project-name "$PROJECT" --commit-dirty=true

echo "==> done: https://tinyrl-industrial.pages.dev"
echo "    custom domain (once set in the dashboard): https://tinyrl-industrial.leekaize.com"
