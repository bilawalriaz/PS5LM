#!/usr/bin/env bash
# Builds ps5/launch/launch.elf, the payload that starts the app from the PC.
#
#   PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk scripts/build-launch.sh
#
# The SDK comes from ps5-payload-dev/sdk (scripts/setup-sdk.sh fetches v0.43 for
# the app build; the same one compiles this). The link line is the same set of
# libraries the app's launch payload needs: Sony's libc, the kernel stubs, the
# user and system services (sceSystemServiceLaunchApp lives in the latter).
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=env.sh
source "$root/scripts/env.sh"

out="$root/build/launch"
mkdir -p "$out"

"$PS5_PAYLOAD_SDK/bin/prospero-clang" -O2 -Wall -Werror -g \
    -o "$out/launch.elf" "$root/ps5/launch/launch.c" \
    -lc -lkernel_sys -lkernel_web -lSceLibcInternal -lSceNet -lSceUserService -lSceSystemService

echo "build-launch: $out/launch.elf"
