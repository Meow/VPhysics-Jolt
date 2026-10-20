#!/bin/sh
# Builds Volt for Garry's Mod in Docker.
#
#   ./build.sh [linux32] [linux64] [win32] [win64]     (default: all four)
#
# Output goes to dist/<package>/, laid out like the game directory:
#
#   gmod-linux32          bin/vphysics.so          32-bit Linux client and listen server
#                                                  (public, prerelease and dev branches)
#   gmod-linux32-srv      bin/vphysics_srv.so      32-bit Linux dedicated server (public branch)
#   gmod-linux64          bin/linux64/vphysics.so  64-bit Linux dedicated server (x86-64 branch)
#   gmod-linux64-client   bin/linux64/vphysics_client.so   64-bit Linux client (x86-64 branch)
#   gmod-win32            bin/vphysics.dll         32-bit Windows, client and dedicated server
#   gmod-win64            bin/win64/vphysics.dll   64-bit Windows, client and dedicated server
#
# The 32-bit builds are for the public, prerelease and dev branches. The x86-64 branch's 32-bit
# binaries use the newer (64-bit style) interfaces and are not supported.
#
# Environment:
#   VJOLT_SDK_DIR   Source SDK src directory to build against. Default: the pinned
#                   mini-source-sdk (gmod) is cloned into build/sdk.
#   DOCKER          container runtime (default: docker)
#   JOBS            parallel jobs (default: nproc)
#   DEBIAN_MIRROR   Debian mirror host for the Windows toolchain image (default: deb.debian.org)
set -eu

root=$(cd "$(dirname "$0")" && pwd)
docker=${DOCKER:-docker}
jobs=${JOBS:-$(nproc)}

linux_image=registry.gitlab.steamos.cloud/steamrt/sniper/sdk:3.0.20260928.262393
windows_image=vphysics-jolt-windows

sdk_repo=https://github.com/RaphaelIT7/mini-source-sdk.git
sdk_commit=ba064cd7e560300103e1630ee90f7bad7f2a2224   # gmod-changes

targets=${*:-linux32 linux64 win32 win64}

for target in $targets; do
	case $target in
		linux32|linux64|win32|win64) ;;
		*) echo "unknown target: $target (expected linux32, linux64, win32 or win64)" >&2; exit 1 ;;
	esac
done

if [ ! -f "$root/joltphysics/src/Jolt/Jolt.h" ]; then
	git -C "$root" submodule update --init --recursive
fi

if [ -n "${VJOLT_SDK_DIR:-}" ]; then
	sdk=$(cd "$VJOLT_SDK_DIR" && pwd)
else
	checkout=$root/build/sdk/mini-source-sdk
	if [ "$(git -C "$checkout" rev-parse HEAD 2>/dev/null || true)" != "$sdk_commit" ]; then
		echo "Fetching mini-source-sdk $sdk_commit"
		rm -rf "$checkout"
		mkdir -p "$checkout"
		git -C "$checkout" init -q
		git -C "$checkout" fetch -q --depth 1 "$sdk_repo" "$sdk_commit"
		git -C "$checkout" checkout -q FETCH_HEAD
	fi
	sdk=$checkout/gmod/src
fi

# Runs a command in an image with the repo at /src and the SDK at /sdk.
run() {
	image=$1
	shift
	"$docker" run --rm \
		--user "$(id -u):$(id -g)" \
		-e JOBS="$jobs" \
		-v "$root:/src" \
		-v "$sdk:/sdk:ro" \
		-e HOME=/src/build/home \
		-e WINEPREFIX=/src/build/wineprefix \
		-w /src \
		"$image" "$@"
}

# build <target> <image> <bin dir> <variant:package>... -- <extra cmake args>
build() {
	target=$1 image=$2 bindir=$3
	shift 3
	packages=""
	variants=""
	while [ "$1" != "--" ]; do
		variants="$variants${variants:+;}${1%%:*}"
		packages="$packages $1"
		shift
	done
	shift

	echo "=== $target"
	mkdir -p "$root/build/home"
	run "$image" sh -c '
		set -e
		target=$0 bindir=$1 variants=$2
		shift 2
		if command -v wine >/dev/null 2>&1; then
			# Set up the Wine prefix before the compilers run in parallel
			wine wineboot --init >/dev/null 2>&1
			wineserver -w
		fi
		cmake -S /src -B "/src/build/$target" -G Ninja \
			-DCMAKE_BUILD_TYPE=RelWithDebInfo \
			-DVJOLT_SDK_DIR=/sdk \
			-DVJOLT_BIN_DIR="$bindir" \
			-DVJOLT_VARIANTS="$variants" \
			"$@"
		cmake --build "/src/build/$target" -j "$JOBS"
	' "$target" "$bindir" "$variants" "$@"
	for entry in $packages; do
		variant=${entry%%:*} package=${entry#*:}
		rm -rf "$root/dist/$package"
		run "$image" cmake --install "/src/build/$target" --component "$variant" --prefix "/src/dist/$package"
	done
}

for target in $targets; do
	case $target in
		win32|win64)
			if ! "$docker" image inspect "$windows_image" >/dev/null 2>&1; then
				"$docker" build -t "$windows_image" \
					--build-arg DEBIAN_MIRROR="${DEBIAN_MIRROR:-deb.debian.org}" \
					-f "$root/docker/windows.Dockerfile" "$root/docker"
			fi
			;;
	esac

	case $target in
		linux32)
			build linux32 "$linux_image" bin default:gmod-linux32 srv:gmod-linux32-srv -- \
				--toolchain /src/cmake/toolchains/linux32.cmake ;;
		linux64)
			build linux64 "$linux_image" bin/linux64 default:gmod-linux64 client:gmod-linux64-client -- \
				--toolchain /src/cmake/toolchains/linux64.cmake ;;
		win32)
			build win32 "$windows_image" bin default:gmod-win32 -- \
				--toolchain /src/cmake/toolchains/windows-msvc.cmake -DVJOLT_WIN_ARCH=x86 ;;
		win64)
			build win64 "$windows_image" bin/win64 default:gmod-win64 -- \
				--toolchain /src/cmake/toolchains/windows-msvc.cmake -DVJOLT_WIN_ARCH=x64 ;;
	esac
done

echo "Done, see $root/dist/"
