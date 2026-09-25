#!/bin/sh
# Builds and runs the host unit tests with g++ inside a small Alpine container.
# Usage (from AeroScope/):  sh test/run_host_tests.sh
set -e
cd "$(dirname "$0")/.."
docker run --rm -v "$(pwd -W 2>/dev/null || pwd)":/src -w /src alpine:3.20 sh -c '
  apk add --no-cache g++ >/dev/null &&
  g++ -std=gnu++17 -Wall -Wextra -Werror -O1 -DSETTINGS_HOST_TEST -DALERTS_HOST_TEST \
      test/host_tests.cpp main/settings.cpp main/alerts.cpp -o /tmp/host_tests &&
  /tmp/host_tests'
