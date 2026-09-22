# Developer entry points. The scripts hold the logic; these are just names.

.PHONY: dev build-app install-app uninstall-app core core-test

NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc)

# Configure + build the C++ core (libmynah + the pinned whisper.cpp).
core:
	cmake -S . -B build -DMYNAH_WITH_WHISPER=ON
	cmake --build build -j $(NPROC)

# Build the core and run its test suite (doctest + ctest).
core-test: core
	ctest --test-dir build --output-on-failure

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
