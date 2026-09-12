// swift-tools-version:5.9
// ScreenMirrorHelper — ScreenCaptureKit capture helper for BYOK Mode 2 (mirror).
// See docs/host-tools.md for the design and for the "capture helper" stdout
// binary format this produces.
import PackageDescription

let package = Package(
    name: "ScreenMirrorHelper",
    platforms: [
        .macOS(.v13) // ScreenCaptureKit's SCShareableContent/SCStream baseline
    ],
    targets: [
        .executableTarget(
            name: "ScreenMirrorHelper",
            path: "Sources/ScreenMirrorHelper"
        )
    ]
)
