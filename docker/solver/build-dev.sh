#!/bin/bash
set -euo pipefail

scriptPath="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repoRoot="$(cd "$scriptPath/../.." && pwd)"
tmpDockerfile="$scriptPath/tmp.dockerfile"

cleanup() {
    rm -f "$tmpDockerfile"
    if [ -f "$repoRoot/.dockerignore.tmp" ]; then
        mv -f "$repoRoot/.dockerignore.tmp" "$repoRoot/.dockerignore"
    fi
}
trap cleanup EXIT

cd "$scriptPath"
cat common.dockerfile dev.dockerfile > "$tmpDockerfile"

cd "$repoRoot"
cp .dockerignore .dockerignore.tmp
cat .gitignore >> .dockerignore

docker build -t astrometrynet/solver:dev -f docker/solver/tmp.dockerfile .
