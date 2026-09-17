# Developer entry points. The scripts hold the logic; these are just names.

.PHONY: dev build-app core core-test

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
