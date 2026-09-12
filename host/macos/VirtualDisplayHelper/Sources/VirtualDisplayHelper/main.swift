//
//  main.swift
//  VirtualDisplayHelper
//
//  A tiny CLI that creates a macOS *virtual display* the owner can arrange in
//  System Settings > Displays and drag windows onto; BYOK's mirror then captures
//  it. The virtual display exists ONLY while this process runs — it disappears
//  the moment the process exits (Ctrl-C, kill, crash). See docs/sample-projects/mirror-and-virtual-display.md.
//
//  Subcommands:
//    VirtualDisplayHelper create --width W --height H --name "BYOK" [--hidpi]
//    VirtualDisplayHelper list
//    VirtualDisplayHelper probe     (safe self-test: builds descriptor/modes but
//                                    NEVER instantiates a display; no Displays
//                                    layout change — used to validate the dynamic
//                                    bindings without disturbing the session)
//
//  HARD RULE for developers: do not leave `create` running against the owner's
//  live session — it re-arranges Displays. `probe` is safe.

import Foundation
import CoreGraphics

// MARK: - stdio helpers

func writeStderr(_ s: String) { FileHandle.standardError.write(Data((s + "\n").utf8)) }
func writeStdout(_ s: String) { FileHandle.standardOutput.write(Data((s + "\n").utf8)) }

let usage = """
VirtualDisplayHelper — BYOK Mode 2 virtual display (private CoreGraphics API)

USAGE:
  VirtualDisplayHelper create [--width W] [--height H] [--name NAME] [--hidpi]
  VirtualDisplayHelper list
  VirtualDisplayHelper probe
  VirtualDisplayHelper -h | --help

create   Create a virtual display and hold it open until Ctrl-C (SIGINT/SIGTERM).
         Prints the new CGDirectDisplayID as `displayID=<n>` on stdout so callers
         can pipe it into the mirror. The display VANISHES when this process ends.
         --width / --height  logical/target size in pixels (default 240 x 80).
                             Tiny sizes are usually rejected by the OS, so the
                             helper ALSO registers 2x/3x/4x same-aspect modes so
                             at least one valid mode exists; the mirror downsamples.
         --name              label shown in System Settings > Displays (default "BYOK").
         --hidpi             register HiDPI (Retina) scaled modes (hiDPI=1). With
                             480x160 @2x the panel presents as 240x80 points and
                             downsamples crisply. Off by default.

list     Print all active displays (id, pixel size, main/builtin) and exit.

probe    Build a descriptor + settings + modes via the private API but DO NOT
         create a display (no Displays change). Reports whether the private
         classes are present and the dynamic bindings work. Safe to run anytime.
"""

// MARK: - argument parsing

let args = Array(CommandLine.arguments.dropFirst())
guard let sub = args.first else {
    writeStderr(usage)
    exit(2)
}

func flagValue(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name), i + 1 < args.count else { return nil }
    return args[i + 1]
}
func hasFlag(_ name: String) -> Bool { args.contains(name) }

// MARK: - list

func listDisplays() {
    var count: UInt32 = 0
    guard CGGetActiveDisplayList(0, nil, &count) == .success, count > 0 else {
        writeStdout("no active displays")
        return
    }
    var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
    guard CGGetActiveDisplayList(count, &ids, &count) == .success else {
        writeStderr("CGGetActiveDisplayList failed")
        exit(1)
    }
    let main = CGMainDisplayID()
    writeStdout("displayID\twidth\theight\tflags")
    for id in ids {
        let w = CGDisplayPixelsWide(id)
        let h = CGDisplayPixelsHigh(id)
        var flags: [String] = []
        if id == main { flags.append("main") }
        if CGDisplayIsBuiltin(id) != 0 { flags.append("builtin") }
        if CGDisplayIsAsleep(id) != 0 { flags.append("asleep") }
        writeStdout("\(id)\t\(w)\t\(h)\t\(flags.isEmpty ? "-" : flags.joined(separator: ","))")
    }
}

// MARK: - mode set construction

/// Register the requested pixel size plus 2x/3x/4x same-aspect multiples so at
/// least one mode survives the OS's minimum-size filtering. Deduped, capped at
/// a reasonable ceiling.
func modeSizes(width: UInt, height: UInt) -> [(UInt, UInt)] {
    var out: [(UInt, UInt)] = []
    var seen = Set<String>()
    for k: UInt in [1, 2, 3, 4] {
        let w = width * k, h = height * k
        if w > 8192 || h > 8192 { continue }
        let key = "\(w)x\(h)"
        if seen.insert(key).inserted { out.append((w, h)) }
    }
    return out
}

// MARK: - probe (safe: never creates a display)

func probe() {
    guard PrivateVD.isAvailable else {
        writeStdout("probe: UNAVAILABLE — one or more CGVirtualDisplay* classes not found")
        exit(1)
    }
    guard let (_, descAPI) = PrivateVD.makeDescriptor(),
          let (_, setAPI) = PrivateVD.makeSettings() else {
        writeStdout("probe: FAIL — could not allocate descriptor/settings")
        exit(1)
    }
    descAPI.name = "BYOK-probe" as NSString
    descAPI.maxPixelsWide = 720
    descAPI.maxPixelsHigh = 240
    descAPI.sizeInMillimeters = CGSize(width: 60, height: 20)
    descAPI.setDispatchQueue(DispatchQueue.main)

    var modes: [AnyObject] = []
    for (w, h) in modeSizes(width: 240, height: 80) {
        if let m = PrivateVD.makeMode(width: w, height: h, refreshRate: 60) {
            modes.append(m)
        }
    }
    setAPI.hiDPI = 1
    setAPI.modes = modes as NSArray

    // Read a value back through the dynamic binding to prove round-trip works.
    // Must use unsafeBitCast, not `as?`: the private class does not declare the
    // @objc protocol conformance, so a checked cast (`as?`) always fails even
    // though the object responds to every selector.
    let echo = modes.first.map { obj -> String in
        let m = unsafeBitCast(obj, to: VDMode.self)
        return "\(m.width)x\(m.height)@\(Int(m.refreshRate))"
    } ?? "none"
    writeStdout("probe: OK — classes present, \(modes.count) mode(s) built, first=\(echo), no display created")
}

// MARK: - create (holds the display open until a signal)

// Strong references kept alive for the whole process lifetime.
var keepAlive: [AnyObject] = []

func create() {
    let width = UInt(flagValue("--width").flatMap { UInt($0) } ?? 240)
    let height = UInt(flagValue("--height").flatMap { UInt($0) } ?? 80)
    let name = flagValue("--name") ?? "BYOK"
    let hidpi = hasFlag("--hidpi")

    guard width > 0, height > 0 else {
        writeStderr("create: --width and --height must be positive")
        exit(2)
    }
    guard PrivateVD.isAvailable else {
        writeStderr("create: the CGVirtualDisplay* private API is not available in this process")
        exit(1)
    }
    guard let (descObj, descAPI) = PrivateVD.makeDescriptor(),
          let (setObj, setAPI) = PrivateVD.makeSettings() else {
        writeStderr("create: could not allocate descriptor/settings")
        exit(1)
    }

    let sizes = modeSizes(width: width, height: height)
    let maxW = sizes.map { $0.0 }.max() ?? width
    let maxH = sizes.map { $0.1 }.max() ?? height

    descAPI.name = name as NSString
    descAPI.maxPixelsWide = UInt32(maxW)
    descAPI.maxPixelsHigh = UInt32(maxH)
    // Small physical size => high DPI => the OS is willing to offer HiDPI modes.
    descAPI.sizeInMillimeters = CGSize(width: 60, height: 60 * Double(height) / Double(width))
    descAPI.productID = 0x0B10   // "BYOK"-ish, arbitrary
    descAPI.vendorID = 0x1209    // pid.codes generic vendor, arbitrary here
    descAPI.serialNum = 0x0001
    descAPI.setDispatchQueue(DispatchQueue.main)
    descAPI.terminationHandler = { _, _ in
        writeStderr("VirtualDisplayHelper: display terminated by the system")
        exit(0)
    }

    var modeObjs: [AnyObject] = []
    for (w, h) in sizes {
        if let m = PrivateVD.makeMode(width: w, height: h, refreshRate: 60) {
            modeObjs.append(m)
        }
    }
    guard !modeObjs.isEmpty else {
        writeStderr("create: failed to build any display mode")
        exit(1)
    }
    setAPI.hiDPI = hidpi ? 1 : 0
    setAPI.modes = modeObjs as NSArray

    guard let (dispObj, dispAPI) = PrivateVD.makeDisplay(descriptor: descObj) else {
        writeStderr("create: CGVirtualDisplay initWithDescriptor: returned nil")
        exit(1)
    }
    let applied = dispAPI.applySettings(setObj)
    let id = dispAPI.displayID
    guard id != 0 else {
        writeStderr("create: display was created but displayID is 0 (mode set likely rejected)")
        exit(1)
    }

    // Hold everything alive.
    keepAlive = [descObj, setObj, dispObj] + modeObjs

    // Machine-readable line first (callers parse this), then human notes to stderr.
    writeStdout("displayID=\(id)")
    writeStderr("VirtualDisplayHelper: created \"\(name)\" displayID=\(id) "
        + "modes=\(modeObjs.count) hiDPI=\(hidpi ? 1 : 0) applySettings=\(applied)")
    writeStderr("VirtualDisplayHelper: it is open now — arrange it in System Settings > Displays.")
    writeStderr("VirtualDisplayHelper: press Ctrl-C to remove the display and exit.")

    // Clean teardown on Ctrl-C / SIGTERM. The display vanishes when we exit.
    for signo in [SIGINT, SIGTERM] {
        signal(signo, SIG_IGN)
        let src = DispatchSource.makeSignalSource(signal: signo, queue: .main)
        src.setEventHandler {
            writeStderr("VirtualDisplayHelper: signal received, removing display.")
            exit(0)
        }
        src.resume()
        keepAlive.append(src as AnyObject)
    }

    dispatchMain() // never returns; the display lives as long as we do
}

// MARK: - dispatch

switch sub {
case "create":
    create()
case "list":
    listDisplays()
case "probe":
    probe()
case "-h", "--help":
    writeStdout(usage)
default:
    writeStderr("unknown subcommand: \(sub)\n")
    writeStderr(usage)
    exit(2)
}
