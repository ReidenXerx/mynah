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
// directory, so "vendor/install/include" silently fails to find mynah.h.
// Deriving an absolute path from the manifest's own location works from any
// checkout and any invocation directory.
let cltFrameworks = "/Library/Developer/CommandLineTools/Library/Developer/Frameworks"
let testFrameworkFlags: [String] =
    FileManager.default.fileExists(atPath: cltFrameworks + "/Testing.framework")
        ? ["-F", cltFrameworks]
        : []

let packageDirectory = URL(fileURLWithPath: #filePath).deletingLastPathComponent().path
let vendorInclude = "\(packageDirectory)/vendor/install/include"
let vendorLib = "\(packageDirectory)/vendor/install/lib"

// The mynah macOS app, running on the C++ core (Phase 5).
//
// Deliberately dependency-free on the Swift side: config, segmentation,
// VAD, whisper and the hallucination filter all live in libmynah now, and
// the app's Swift is capture, input and UI.
//
// SwiftPM produces a bare executable; `scripts/build-app.sh` wraps it into a
// proper `Mynah.app` bundle with Info.plist. That is what gives us a stable
// bundle identifier, and therefore a TCC permission grant that survives
// upgrades.
let package = Package(
    name: "MynahApp",
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
        // The core's C API — the only header a front end includes. Built
        // into `vendor/install` by `scripts/build-core.sh` from the pinned
        // sources, statically, with the whisper.cpp submodule (M6) — never
        // from Homebrew.
        .systemLibrary(
            name: "CMynah",
            path: "Sources/CMynah"
        ),
        .executableTarget(
            name: "MynahApp",
            dependencies: ["CMynah"],
            path: "Sources/MynahApp",
            // Headers and libraries come from the installed core in
            // `vendor/install`, produced by `scripts/build-core.sh`.
            //
            // Link order matters for static archives: dependents before
            // dependencies — mynah -> whisper -> ggml -> backends -> base.
            //
            // -lc++ is required: the core, whisper.cpp and ggml are C++,
            // and Swift does not link libc++ for a static archive reached
            // through a C module map.
            cSettings: [
                .unsafeFlags(["-I\(vendorInclude)"]),
            ],
            swiftSettings: [
                .unsafeFlags(["-Xcc", "-I\(vendorInclude)"]),
            ],
            linkerSettings: [
                .unsafeFlags([
                    "\(vendorLib)/libmynah.a",
                    "\(vendorLib)/libwhisper.a",
                    "\(vendorLib)/libggml.a",
                    "\(vendorLib)/libggml-metal.a",
                    "\(vendorLib)/libggml-cpu.a",
                    "\(vendorLib)/libggml-base.a",
                    "-lc++",
                ]),
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("Accelerate"),
                .linkedFramework("CoreML"),
            ]
        ),
        // The app's own logic: the settings adapter and the hotkey parser.
        // The engine's tests live in the core's suite (`make core-test`);
        // these cover what is on THIS side of the C API, which the core
        // cannot see — and which shipped three settings bugs without one.
        .testTarget(
            name: "MynahAppTests",
            dependencies: ["MynahApp"],
            path: "Tests/MynahAppTests",
            swiftSettings: [.unsafeFlags(testFrameworkFlags)],
            linkerSettings: [.unsafeFlags(testFrameworkFlags)]
        ),
    ]
)