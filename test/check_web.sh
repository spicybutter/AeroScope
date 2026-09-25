#!/bin/sh
# Syntax-checks the JavaScript embedded in main/web/app.html with Node (Docker).
# Usage (from AeroScope/):  sh test/check_web.sh
set -e
cd "$(dirname "$0")/.."
docker run --rm -v "$(pwd -W 2>/dev/null || pwd)":/src -w /src node:22-alpine sh -c '
  node -e "
    const fs=require(\"fs\");
    const html=fs.readFileSync(\"main/web/app.html\",\"utf8\");
    const scripts=[...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map(m=>m[1]);
    if(!scripts.length){console.error(\"no <script> found\");process.exit(1)}
    scripts.forEach((s,i)=>fs.writeFileSync(\"/tmp/app\"+i+\".js\",s));
    console.log(scripts.length+\" script block(s), \"+html.length+\" bytes of HTML\");
  " &&
  for f in /tmp/app*.js; do node --check "$f" && echo "$f: syntax OK"; done'
