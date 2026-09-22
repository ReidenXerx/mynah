# Developer entry points. The scripts hold the logic; these are just names.

.PHONY: dev build-app install-app uninstall-app core core-test asan tsan clang

NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

# The build tree and extra CMake flags; the targets below reuse `core` with
# their own. e.g. `make core CMAKE_FLAGS=-DMYNAH_CPU=native`
BUILD ?= build
CMAKE_FLAGS ?=

# Configure + build the C++ core (libmynah + the pinned whisper.cpp). On
# Linux this is also the headless `mynah` (build/linux/mynah) and its suite.
core:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)
	cmake --build $(BUILD) -j $(NPROC)

# Build and run every test suite (doctest + ctest).
core-test: core
	ctest --test-dir $(BUILD) --output-on-failure

# The suites under AddressSanitizer + UBSan, and under ThreadSanitizer, each
# in a build tree of its own. CPU-only: the suites never touch a GPU, and
# GPU drivers are noise under a sanitizer.
SANITIZED := -DCMAKE_BUILD_TYPE=Debug -DMYNAH_CPU=native -DMYNAH_VULKAN=OFF
asan:
	@$(MAKE) core-test BUILD=build-asan CMAKE_FLAGS="$(SANITIZED) -DMYNAH_SANITIZE=address"
tsan:
	@$(MAKE) core-test BUILD=build-tsan CMAKE_FLAGS="$(SANITIZED) -DMYNAH_SANITIZE=thread"

# The same suites built with Clang (the default compiler is GCC on Arch,
# Apple clang on macOS) — each catches what the other lets through.
clang:
	@$(MAKE) core-test BUILD=build-clang CMAKE_FLAGS="-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ $(CMAKE_FLAGS)"

# Build the macOS app (debug), launch it and stream its log. Ctrl+C quits.
dev:
	@macos/scripts/dev.sh

# Build the macOS app without launching it. `make build-app CONFIG=release`
build-app:
	@macos/scripts/build-app.sh $(or $(CONFIG),debug)

# Build (release) and install Mynah.app at one fixed place — /Applications, or
# ~/Applications — signed so its Accessibility grant survives every rebuild.
# Grant once; run this again after changes. MYNAH_NO_LAUNCH=1 skips launching.
install-app:
	@macos/scripts/install-app.sh

# Quit and remove the installed app. The signing identity stays (see
# macos/scripts/create-signing-cert.sh for removing it too).
uninstall-app:
	@# Only when running: `tell application id ... to quit` LAUNCHES an app that is not.
	@if pgrep -x MynahApp >/dev/null; then osascript -e 'tell application id "com.reidenxerx.mynah" to quit' >/dev/null 2>&1 || true; fi
	@rm -rf /Applications/Mynah.app "$$HOME/Applications/Mynah.app"
	@echo "removed Mynah.app"
