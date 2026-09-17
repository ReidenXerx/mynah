# Developer entry points. The scripts hold the logic; these are just names.

.PHONY: dev build-app

# Build the macOS app (debug), launch it and stream its log. Ctrl+C quits.
dev:
	@macos/scripts/dev.sh

# Build the macOS app without launching it. `make build-app CONFIG=release`
build-app:
	@macos/scripts/build-app.sh $(or $(CONFIG),debug)
