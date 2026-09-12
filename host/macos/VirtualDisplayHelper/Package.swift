// swift-tools-version:5.9
// VirtualDisplayHelper — creates a macOS *virtual display* for BYOK Mode 2.
//
// This is a SEPARATE package from ScreenMirrorHelper. It uses the private
// CoreGraphics virtual-display API (CGVirtualDisplay*) reached dynamically at
// runtime via NSClassFromString + @objc protocol declarations — NO private
// headers are checked in. See docs/sample-projects/mirror-and-virtual-display.md for how it works, the
// accepted resolutions, the exact owner test sequence, and how to remove it.
import PackageDescription

let package = Package(
    name: "VirtualDisplayHelper",
    platforms: [
        .macOS(.v13) // matches ScreenMirrorHelper; the private API predates this
    ],
    targets: [
        .executableTarget(
            name: "VirtualDisplayHelper",
            path: "Sources/VirtualDisplayHelper"
        )
    ]
)
