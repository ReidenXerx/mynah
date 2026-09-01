// swift-tools-version: 6.0
import Foundation
import PackageDescription

// swift-testing lives in Testing.framework. A full Xcode install wires it into
// SwiftPM automatically; a Command Line Tools install ships the framework but
// not the search path, so `swift test` fails with "no such module 'Testing'".
// XCTest is not an escape hatch — it ships only with Xcode, so on a CLT machine
// there is no test framework at all without this.
//
// Detected rather than hardcoded, so the flag is absent on Xcode machines where
// it would be wrong.
// Paths in `unsafeFlags` are NOT resolved relative to the package root — they
// reach the compiler as written and are interpreted against its working
// directory, so "vendor/install/include" silently fails to find whisper.h.
// Deriving an absolute path from the manifest's own location works from any
// checkout and any invocation directory.
let packageDirectory = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path
let vendorInclude = "\(packageDirectory)/vendor/install/include"
let vendorLib = "\(packageDirectory)/vendor/install/lib"

let cltFrameworks = "/Library/Developer/CommandLineTools/Library/Developer/Frameworks"
let testFrameworkFlags: [String] =
    FileManager.default.fileExists(atPath: cltFrameworks + "/Testing.framework")
        ? ["-F", cltFrameworks]
        : []

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
        // whisper.cpp's C API. Homebrew's prefix differs by architecture and
        // SwiftPM has no way to ask for it, so both are listed; the one that
        // does not exist is ignored.
        .systemLibrary(
            name: "CWhisper",
            path: "Sources/CWhisper"
        ),
        .executableTarget(
            name: "WhizApp",
            dependencies: ["CWhisper"],
            path: "Sources/WhizApp",
            // Headers and libraries come from the vendored whisper.cpp build in
            // `vendor/install`, produced by `scripts/build-whisper.sh` from the
            // pinned submodule — never from Homebrew.
            //
            // This manifest previously pointed at /opt/homebrew, which was wrong
            // in both directions: `swift build` failed outright on machines
            // without Homebrew whisper.cpp, and on machines that had it, it
            // silently linked that copy — a different, unpinned build than the
            // v1.9.2 submodule the app is compiled against everywhere else.
            //
            // The paths are relative to this package root, so they work from any
            // checkout. SwiftPM has no way to resolve a path at manifest
            // evaluation time, hence the literal "vendor/install".
            //
            // NOTE: `swift build` still needs `scripts/build-whisper.sh` to have
            // run first. `scripts/build-app.sh` does that automatically and is
            // the supported path; this manifest exists for `swift test` and
            // editor tooling.
            cSettings: [
                .unsafeFlags(["-I\(vendorInclude)"]),
            ],
            swiftSettings: [
                .unsafeFlags(["-Xcc", "-I\(vendorInclude)"]),
            ],
            linkerSettings: [
                .unsafeFlags([
                    // Static archives, dependents before dependencies.
                    "\(vendorLib)/libwhisper.a",
                    "\(vendorLib)/libggml.a",
                    "\(vendorLib)/libggml-metal.a",
                    "\(vendorLib)/libggml-cpu.a",
                    "\(vendorLib)/libggml-base.a",
                    // whisper.cpp and ggml are C++; Swift does not link libc++
                    // for a static archive reached through a C module map.
                    "-lc++",
                ]),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Accelerate"),
                .linkedFramework("CoreML"),
            ]
        ),
        .testTarget(
            name: "WhizAppTests",
            dependencies: ["WhizApp"],
            path: "Tests/WhizAppTests",
            swiftSettings: [.unsafeFlags(testFrameworkFlags)],
            linkerSettings: [.unsafeFlags(testFrameworkFlags)]
        ),
    ]
)
