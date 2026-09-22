// make-icon.swift — render the mynah mark (docs/assets/mynah-mark.svg) into
// an .icns for the app bundle. A bundle without CFBundleIconFile shows the
// generic placeholder tile in Finder, Dock and System Settings — which is
// exactly how a build gets mistaken for somebody else's.
//
// Usage: swift make-icon.swift <mark.svg> <out.icns>
// NSImage has read SVG since macOS 11; no external tooling. Run from
// build-app.sh when the bundle is assembled.

import AppKit

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: make-icon.swift <mark.svg> <out.icns>\n".data(using: .utf8)!)
    exit(2)
}
let svgURL = URL(fileURLWithPath: args[1])
let outURL = URL(fileURLWithPath: args[2])

// SVG support in NSImage is Big Sur and later — the deployment floor.
guard let source = NSImage(contentsOf: svgURL) else {
    FileHandle.standardError.write("make-icon: cannot read \(svgURL.path)\n".data(using: .utf8)!)
    exit(1)
}

func renderPNG(pixel: Int) -> Data? {
    let size = NSSize(width: pixel, height: pixel)
    let image = NSImage(size: size, flipped: false) { rect in
        // The mark is drawn centred with a small inset, the way app icons
        // are expected to sit in their tile.
        let inset = CGFloat(pixel) * 0.08
        let drawRect = rect.insetBy(dx: inset, dy: inset)
        source.draw(in: drawRect, from: .zero, operation: .sourceOver, fraction: 1.0)
        return true
    }
    guard let tiff = image.tiffRepresentation,
          let rep = NSBitmapImageRep(data: tiff),
          let png = rep.representation(using: .png, properties: [:]) else { return nil }
    return png
}

let staging = URL(fileURLWithPath: NSTemporaryDirectory())
    .appendingPathComponent("mynah-\(Int(Date().timeIntervalSince1970)).iconset")
try? FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)

// The iconset's required members.
let members: [(name: String, pixels: Int)] = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]

for (name, pixels) in members {
    guard let png = renderPNG(pixel: pixels) else {
        FileHandle.standardError.write("make-icon: failed to render \(name)\n".data(using: .utf8)!)
        exit(1)
    }
    try? png.write(to: staging.appendingPathComponent(name))
}

let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", staging.path, "-o", outURL.path]
let pipe = Pipe()
process.standardError = pipe
try? process.run()
process.waitUntilExit()

if process.terminationStatus != 0 {
    let message = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
    FileHandle.standardError.write("make-icon: iconutil failed: \(message)\n".data(using: .utf8)!)
    exit(1)
}

try? FileManager.default.removeItem(at: staging)
exit(0)