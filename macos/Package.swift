// swift-tools-version: 6.0
import PackageDescription

// The whiz macOS app.
//
// Deliberately dependency-free. The one thing an external package would buy
// us is TOML parsing, but whiz's config schema is a flat `key = value` file
// (see `whiz/config.py:_emit_toml`), so `FlatTOML.swift` handles it in ~120
// lines and the package builds offline.
//
// SwiftPM produces a bare executable; `scripts/build-app.sh` wraps it into a
// proper `Whiz.app` bundle with Info.plist. That is what gives us a stable
// bundle identifier, and therefore a TCC permission grant that survives
// upgrades.
let package = Package(
    name: "WhizApp",
    // macOS 13 (Ventura) is the floor, and it is a deliberate choice: it is the
    // oldest release that has both `MenuBarExtra` and `SMAppService`. Those two
    // are exactly what let us delete `macos_rumps.py` and `service.py`, so
    // targeting anything older would mean hand-rolling NSStatusItem and
    // LaunchAgent plists again — reintroducing the workarounds this port exists
    // to remove. Ventura also still covers Macs back to 2017.
    //
    // The cost of 13 over 14 is `ObservableObject` instead of `@Observable`;
    // see SessionController.
    platforms: [.macOS(.v13)],
    targets: [
        .executableTarget(
            name: "WhizApp",
            path: "Sources/WhizApp"
        ),
        .testTarget(
            name: "WhizAppTests",
            dependencies: ["WhizApp"],
            path: "Tests/WhizAppTests"
        ),
    ]
)
