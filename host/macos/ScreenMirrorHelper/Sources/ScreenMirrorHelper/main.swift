// ScreenMirrorHelper — ScreenCaptureKit capture helper for BYOK Mode 2 (mirror).
//
// Device-independent groundwork (Task E): this binary knows nothing about the
// BYOK device, the USB transport, or the wire protocol. It only captures a
// display or window with ScreenCaptureKit, converts frames to 8-bit
// grayscale, letterbox-scales them to a configurable size, computes a
// changed-region bounding rect against the previous frame, and streams a
// simple length-implied binary record per frame to stdout (or writes PNGs
// for debugging with --png-dir). See docs/host-tools.md, section
// "Capture helper (implemented)", for the exact wire format and the planned
// Python consumer (host/macos/byok/mirror.py, written later by the CLI
// owner — not part of this package).
//
// This file is `main.swift`, so top-level `await` is a valid async entry
// point under the Swift 6.3 CLT toolchain; no @main type is needed.

import Foundation
import ScreenCaptureKit
import CoreGraphics
import CoreMedia
import CoreVideo
import Accelerate
import ImageIO
import UniformTypeIdentifiers
import AppKit
import Dispatch

// MARK: - Debug logging

// --debug logs each stage to stderr (never stdout, which carries the SMH1
// binary frame stream). Set once at startup from a raw scan of
// CommandLine.arguments so it works for every subcommand, including ones
// (like `list`) whose own arg loop doesn't otherwise recognize --debug.
var debugEnabled = false

func debugLog(_ s: @autoclosure () -> String) {
    guard debugEnabled else { return }
    writeStderr("[debug] \(s())")
}

// MARK: - Wire format

// Frame record (all multi-byte integers big-endian / network order):
//   magic      4 bytes   ASCII "SMH1"
//   width      u16       canvas width in pixels  (== --width)
//   height     u16       canvas height in pixels (== --height)
//   dirty_x    u16       changed-region bounding box, in canvas pixels
//   dirty_y    u16
//   dirty_w    u16       0x0 width/height means "nothing changed"
//   dirty_h    u16
//   pixels     width*height bytes, 8-bit grayscale, row-major, top-left
//              origin. No explicit length field: the reader derives the
//              payload size from width*height, which is always exactly
//              what follows the 16-byte header.
let MAGIC: [UInt8] = Array("SMH1".utf8)

func appendU16BE(_ value: Int, to data: inout Data) {
    let v = UInt16(clamping: max(0, value))
    data.append(UInt8((v >> 8) & 0xFF))
    data.append(UInt8(v & 0xFF))
}

func writeStderr(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

// MARK: - Screen Recording permission

// CGPreflightScreenCaptureAccess() is explicitly documented by Apple as
// non-prompting — it only reports current TCC state. We rely on exactly
// that property to make `list` and `capture` safe to run non-interactively:
// this tool never calls CGRequestScreenCaptureAccess() or anything else
// that could pop a system dialog. If access is missing, print a clear
// message and exit(3); the user grants it by hand in System Settings.
func hasScreenRecordingPermission() -> Bool {
    return CGPreflightScreenCaptureAccess()
}

func failNoPermission() -> Never {
    writeStderr("""
    ScreenMirrorHelper: Screen Recording permission is not granted.
    Grant it in System Settings > Privacy & Security > Screen Recording \
    for the terminal/binary running this tool, then re-run the command.
    This tool will not attempt to request or prompt for access itself.
    """)
    exit(3)
}

// MARK: - Usage

func printUsageAndExit(_ code: Int32) -> Never {
    let usage = """
    ScreenMirrorHelper — ScreenCaptureKit capture helper for BYOK Mode 2.

    USAGE:
      ScreenMirrorHelper list
      ScreenMirrorHelper capture (--display <id> | --window <id>)
                         [--width W] [--height H] [--fps N]
                         [--threshold T] [--cursor | --no-cursor]
                         [--png-dir DIR] [--debug]

    list       Print available displays and windows, one per line
               (requires Screen Recording permission; prints a message
               and exits 3 if not granted — never attempts to request it).

    capture    Capture the given display or window, convert each frame to
               8-bit grayscale, scale to fit --width x --height (aspect
               preserved, letterboxed with black padding), diff against
               the previous frame at --threshold, and write one SMH1
               record per frame to stdout at up to --fps frames/second.
               With --png-dir, write numbered PNGs to that directory
               instead of the binary stream (debugging).

    --debug logs each capture stage to stderr (permission check, content
             enumeration, filter/config setup, stream start, and one line
             per frame received) -- useful for narrowing down where a
             crash or hang happens without a debugger attached.

    Defaults: --width 240 --height 80 --fps 2 --threshold 8 --cursor
    (240x80 matches the BYOK panel's confirmed native resolution —
    see docs/display.md.)

    Exit codes: 0 ok, 2 bad usage, 3 Screen Recording permission not
    granted, 1 other runtime error.
    """
    print(usage)
    exit(code)
}

// MARK: - list

func runList() async {
    debugLog("runList: checking Screen Recording permission")
    guard hasScreenRecordingPermission() else { failNoPermission() }
    let content: SCShareableContent
    do {
        debugLog("runList: enumerating shareable content")
        content = try await SCShareableContent.excludingDesktopWindows(
            false, onScreenWindowsOnly: true)
        debugLog("runList: got \(content.displays.count) display(s), \(content.windows.count) window(s)")
    } catch {
        writeStderr("ScreenMirrorHelper: failed to enumerate shareable content: \(error)")
        exit(1)
    }

    for display in content.displays {
        print("DISPLAY id=\(display.displayID) width=\(display.width) height=\(display.height)")
    }
    for window in content.windows {
        let rawTitle = window.title ?? ""
        let title = rawTitle.replacingOccurrences(of: "\"", with: "'")
        let app = window.owningApplication?.applicationName ?? ""
        let pid = window.owningApplication?.processID ?? 0
        let w = Int(window.frame.width.rounded())
        let h = Int(window.frame.height.rounded())
        print("WINDOW id=\(window.windowID) title=\"\(title)\" app=\"\(app)\" pid=\(pid) width=\(w) height=\(h) onScreen=\(window.isOnScreen)")
    }
}

// MARK: - capture

struct CaptureArgs {
    var displayID: CGDirectDisplayID?
    var windowID: CGWindowID?
    var width: Int
    var height: Int
    var fps: Int
    var threshold: UInt8
    var showsCursor: Bool
    var pngDir: String?
}

final class FrameProcessor: NSObject, SCStreamOutput, SCStreamDelegate {
    let targetW: Int
    let targetH: Int
    let threshold: UInt8
    let pngDir: String?
    var frameIndex = 0
    var previousCanvas: [UInt8]?

    init(targetW: Int, targetH: Int, threshold: UInt8, pngDir: String?) {
        self.targetW = targetW
        self.targetH = targetH
        self.threshold = threshold
        self.pngDir = pngDir
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
                of type: SCStreamOutputType) {
        guard type == .screen, sampleBuffer.isValid else { return }

        // SCStreamOutput delivers status-only samples (frame started,
        // idle, blank, suspended, stopped) in addition to complete
        // frames; only .complete carries a usable image buffer. Skipping
        // anything else here avoids ever touching a stale/absent
        // CVPixelBuffer for a non-frame sample.
        if let attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(
            sampleBuffer, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]],
           let statusRaw = attachmentsArray.first?[.status] as? Int,
           let status = SCFrameStatus(rawValue: statusRaw),
           status != .complete {
            debugLog("frame sample skipped (status=\(status.rawValue), not .complete)")
            return
        }

        guard let pixelBuffer = sampleBuffer.imageBuffer else {
            debugLog("frame sample skipped (no image buffer)")
            return
        }
        process(pixelBuffer: pixelBuffer)
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        writeStderr("ScreenMirrorHelper: stream stopped: \(error)")
        exit(1)
    }

    func process(pixelBuffer: CVPixelBuffer) {
        CVPixelBufferLockBaseAddress(pixelBuffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pixelBuffer, .readOnly) }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)
        let bytesPerRow = CVPixelBufferGetBytesPerRow(pixelBuffer)
        guard width > 0, height > 0,
              let base = CVPixelBufferGetBaseAddress(pixelBuffer) else { return }
        guard CVPixelBufferGetPixelFormatType(pixelBuffer) == kCVPixelFormatType_32BGRA else {
            writeStderr("ScreenMirrorHelper: unexpected pixel format (expected BGRA32)")
            return
        }

        let srcPtr = base.assumingMemoryBound(to: UInt8.self)

        // 1. Full-resolution grayscale, Rec. 709 luma (matches the formula
        //    documented for the eventual on-device path in docs/host-tools.md
        //    section 4, so a debug PNG here looks like what the panel will
        //    ultimately show, modulo dithering).
        let gray = UnsafeMutablePointer<UInt8>.allocate(capacity: width * height)
        defer { gray.deallocate() }
        for y in 0..<height {
            let rowPtr = srcPtr + y * bytesPerRow
            let outRow = gray + y * width
            for x in 0..<width {
                let px = rowPtr + x * 4 // B, G, R, A
                let b = Double(px[0])
                let g = Double(px[1])
                let r = Double(px[2])
                let l = 0.2126 * r + 0.7152 * g + 0.0722 * b
                outRow[x] = UInt8(max(0, min(255, l.rounded())))
            }
        }

        // 2. Scale-to-fit into targetW x targetH, aspect preserved,
        //    letterboxed (black padding) — via vImage for quality/speed.
        let scale = min(Double(targetW) / Double(width), Double(targetH) / Double(height))
        let fitW = max(1, Int((Double(width) * scale).rounded()))
        let fitH = max(1, Int((Double(height) * scale).rounded()))
        let offX = (targetW - fitW) / 2
        let offY = (targetH - fitH) / 2

        let scaled = UnsafeMutablePointer<UInt8>.allocate(capacity: fitW * fitH)
        defer { scaled.deallocate() }
        var srcBuf = vImage_Buffer(data: gray, height: vImagePixelCount(height),
                                    width: vImagePixelCount(width), rowBytes: width)
        var dstBuf = vImage_Buffer(data: scaled, height: vImagePixelCount(fitH),
                                    width: vImagePixelCount(fitW), rowBytes: fitW)
        let scaleErr = vImageScale_Planar8(&srcBuf, &dstBuf, nil,
                                            vImage_Flags(kvImageHighQualityResampling))
        if scaleErr != kvImageNoError {
            writeStderr("ScreenMirrorHelper: vImageScale_Planar8 error \(scaleErr)")
            return
        }

        var canvas = [UInt8](repeating: 0, count: targetW * targetH)
        canvas.withUnsafeMutableBufferPointer { canvasBuf in
            guard let cbase = canvasBuf.baseAddress else { return }
            for row in 0..<fitH {
                let srcRow = scaled + row * fitW
                let dstRow = cbase + (offY + row) * targetW + offX
                dstRow.update(from: srcRow, count: fitW)
            }
        }

        // 3. Changed-region bounding box vs the previous emitted canvas.
        //    First frame (no previous canvas) is reported as fully dirty.
        var dirtyX = 0, dirtyY = 0, dirtyW = targetW, dirtyH = targetH
        if let prev = previousCanvas, prev.count == canvas.count {
            var minX = targetW, minY = targetH, maxX = -1, maxY = -1
            canvas.withUnsafeBufferPointer { c in
                prev.withUnsafeBufferPointer { p in
                    for y in 0..<targetH {
                        let rowStart = y * targetW
                        for x in 0..<targetW {
                            let idx = rowStart + x
                            let d = Int(c[idx]) - Int(p[idx])
                            if d > Int(threshold) || d < -Int(threshold) {
                                if x < minX { minX = x }
                                if x > maxX { maxX = x }
                                if y < minY { minY = y }
                                if y > maxY { maxY = y }
                            }
                        }
                    }
                }
            }
            if maxX >= minX && maxY >= minY {
                dirtyX = minX
                dirtyY = minY
                dirtyW = maxX - minX + 1
                dirtyH = maxY - minY + 1
            } else {
                dirtyX = 0; dirtyY = 0; dirtyW = 0; dirtyH = 0
            }
        }
        previousCanvas = canvas

        emit(canvas: canvas, dirtyX: dirtyX, dirtyY: dirtyY, dirtyW: dirtyW, dirtyH: dirtyH)
    }

    func emit(canvas: [UInt8], dirtyX: Int, dirtyY: Int, dirtyW: Int, dirtyH: Int) {
        defer { frameIndex += 1 }
        debugLog("frame \(frameIndex): \(targetW)x\(targetH), dirty=(\(dirtyX),\(dirtyY),\(dirtyW),\(dirtyH))")
        if let dir = pngDir {
            writePNG(canvas: canvas, index: frameIndex, dir: dir)
            return
        }
        var out = Data()
        out.reserveCapacity(4 + 12 + canvas.count)
        out.append(contentsOf: MAGIC)
        appendU16BE(targetW, to: &out)
        appendU16BE(targetH, to: &out)
        appendU16BE(dirtyX, to: &out)
        appendU16BE(dirtyY, to: &out)
        appendU16BE(dirtyW, to: &out)
        appendU16BE(dirtyH, to: &out)
        out.append(contentsOf: canvas)
        FileHandle.standardOutput.write(out)
    }

    func writePNG(canvas: [UInt8], index: Int, dir: String) {
        let url = URL(fileURLWithPath: dir)
            .appendingPathComponent(String(format: "frame-%06d.png", index))
        guard let provider = CGDataProvider(data: Data(canvas) as CFData) else { return }
        let colorSpace = CGColorSpaceCreateDeviceGray()
        guard let cgImage = CGImage(
            width: targetW, height: targetH, bitsPerComponent: 8, bitsPerPixel: 8,
            bytesPerRow: targetW, space: colorSpace,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider, decode: nil, shouldInterpolate: false,
            intent: .defaultIntent) else { return }
        guard let dest = CGImageDestinationCreateWithURL(
            url as CFURL, UTType.png.identifier as CFString, 1, nil) else { return }
        CGImageDestinationAddImage(dest, cgImage, nil)
        CGImageDestinationFinalize(dest)
    }
}

// Parks the calling thread until a signal handler (SIGINT/SIGTERM) posts
// to it. See the long comment at the bottom of this function for why this
// replaces the dispatchMain() this file used to call.
let keepAlive = DispatchSemaphore(value: 0)

// A plain (non-async) function so `keepAlive.wait()` is a synchronous
// blocking call in its own right, not a blocking call written inline in
// an async context (which the compiler otherwise flags -- and which is a
// hard error, not a warning, under the Swift 6 language mode).
func parkUntilSignaled() -> Never {
    keepAlive.wait()
    fatalError("ScreenMirrorHelper: keepAlive semaphore signaled without exiting")
}

func runCapture(_ args: CaptureArgs) async {
    debugLog("runCapture: checking Screen Recording permission")
    guard hasScreenRecordingPermission() else { failNoPermission() }

    let content: SCShareableContent
    do {
        debugLog("runCapture: enumerating shareable content")
        content = try await SCShareableContent.excludingDesktopWindows(
            false, onScreenWindowsOnly: true)
    } catch {
        writeStderr("ScreenMirrorHelper: failed to enumerate shareable content: \(error)")
        exit(1)
    }

    let filter: SCContentFilter
    let srcW: Int
    let srcH: Int

    if let displayID = args.displayID {
        debugLog("runCapture: resolving display id \(displayID)")
        guard let display = content.displays.first(where: { $0.displayID == displayID }) else {
            writeStderr("ScreenMirrorHelper: no display with id \(displayID) (run 'list' first)")
            exit(2)
        }
        filter = SCContentFilter(display: display, excludingWindows: [])
        // CGDisplayCopyDisplayMode gives native pixel dimensions (accounts
        // for HiDPI); SCDisplay.width/height is a points-based fallback.
        if let mode = CGDisplayCopyDisplayMode(display.displayID) {
            srcW = mode.pixelWidth
            srcH = mode.pixelHeight
        } else {
            srcW = display.width
            srcH = display.height
        }
    } else if let windowID = args.windowID {
        debugLog("runCapture: resolving window id \(windowID)")
        guard let window = content.windows.first(where: { $0.windowID == windowID }) else {
            writeStderr("ScreenMirrorHelper: no window with id \(windowID) (run 'list' first)")
            exit(2)
        }
        filter = SCContentFilter(desktopIndependentWindow: window)
        srcW = max(1, Int(window.frame.width.rounded()))
        srcH = max(1, Int(window.frame.height.rounded()))
    } else {
        writeStderr("ScreenMirrorHelper: capture requires --display <id> or --window <id>")
        exit(2)
    }
    debugLog("runCapture: source size \(srcW)x\(srcH) -> canvas \(args.width)x\(args.height)")

    let config = SCStreamConfiguration()
    config.width = srcW
    config.height = srcH
    config.pixelFormat = kCVPixelFormatType_32BGRA
    config.minimumFrameInterval = CMTime(value: 1, timescale: CMTimeScale(max(1, args.fps)))
    config.queueDepth = 5
    config.showsCursor = args.showsCursor
    config.scalesToFit = false // we letterbox-scale ourselves; see module doc comment

    if let dir = args.pngDir {
        do {
            try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        } catch {
            writeStderr("ScreenMirrorHelper: could not create --png-dir '\(dir)': \(error)")
            exit(1)
        }
    }

    let processor = FrameProcessor(targetW: args.width, targetH: args.height,
                                    threshold: args.threshold, pngDir: args.pngDir)
    let stream = SCStream(filter: filter, configuration: config, delegate: processor)
    let outputQueue = DispatchQueue(label: "byok.smh.frame-output")

    do {
        debugLog("runCapture: adding stream output")
        try stream.addStreamOutput(processor, type: .screen, sampleHandlerQueue: outputQueue)
        debugLog("runCapture: starting capture")
        try await stream.startCapture()
        debugLog("runCapture: capture started, waiting for frames")
    } catch {
        writeStderr("ScreenMirrorHelper: failed to start capture: \(error)")
        exit(1)
    }

    // --- Why this parks on a semaphore instead of calling dispatchMain() ---
    //
    // This file is main.swift with top-level `await`, so the whole capture
    // path above (SCShareableContent.excludingDesktopWindows, and
    // especially the `await stream.startCapture()` right above this
    // comment) already crosses multiple suspension points. Swift's
    // top-level-async-main machinery resumes those continuations wherever
    // the concurrency runtime schedules them, which by this point is *not*
    // guaranteed to be dispatch's notion of "the main thread" -- even code
    // that is nominally MainActor-isolated can reach here off the literal
    // pthread the process started on.
    //
    // dispatchMain() asserts it is being called from that literal main
    // thread and that nothing has already put the main queue into a
    // "draining forever" state; top-level async main.swift has *already*
    // done the equivalent of that internally to keep the process alive
    // across `await`. Calling dispatchMain() again after an await here
    // hits that libdispatch assertion and the process dies immediately
    // with SIGTRAP (confirmed independently with a minimal `await
    // Task.sleep(...); dispatchMain()` repro -- and it still traps even
    // when the whole call chain is pinned @MainActor, which rules out
    // "just hop back to the main thread" as a fix). That SIGTRAP is
    // exit code -5 -- the exact "ScreenMirrorHelper exited with code -5"
    // / "trace trap" crash this fix addresses.
    //
    // A DispatchSemaphore doesn't care which thread waits on it, so this
    // works no matter where the runtime resumed us. Frame delivery runs on
    // `outputQueue` (a queue of its own, not the main queue), so blocking
    // here never blocks a callback that needs to run -- only the process's
    // own defined exit path (SIGINT/SIGTERM) wakes it.
    signal(SIGINT) { _ in exit(0) }
    signal(SIGTERM) { _ in exit(0) }
    debugLog("runCapture: parking on keepAlive semaphore")
    parkUntilSignaled()
}

// MARK: - Entry point

let arguments = Array(CommandLine.arguments.dropFirst())
debugEnabled = arguments.contains("--debug")

// Touch NSApplication.shared once, before anything else, to establish the
// process's CoreGraphics/SkyLight (CGS) connection to the window server.
// This binary is a bare `swift build` executable: no Info.plist, no app
// bundle, never linked/initialized as an AppKit app. Without a CGS
// connection, some CoreGraphics calls that ScreenCaptureKit's window path
// makes internally (window-server lookups behind
// SCContentFilter(desktopIndependentWindow:) and friends) hit the
// `CGS_REQUIRE_INIT` assertion in CGInitialization.c and abort(SIGABRT) --
// this is the exact "Assertion failed: (did_initialize), function
// CGS_REQUIRE_INIT..." / "exited with code -6" crash `capture --window`
// hit. Merely referencing NSApplication.shared (no .run(), no app
// delegate, no event loop) is enough to create that connection; it's a
// no-op for the --display path but required for --window.
debugLog("startup: touching NSApplication.shared to establish CGS connection")
_ = NSApplication.shared

guard let command = arguments.first else { printUsageAndExit(2) }

switch command {
case "list":
    await runList()

case "capture":
    var displayID: CGDirectDisplayID?
    var windowID: CGWindowID?
    var width = 240
    var height = 80
    var fps = 2
    var threshold: UInt8 = 8
    var showsCursor = true
    var pngDir: String?

    var i = 1
    while i < arguments.count {
        let a = arguments[i]
        switch a {
        case "--display":
            i += 1
            guard i < arguments.count, let v = UInt32(arguments[i]) else { printUsageAndExit(2) }
            displayID = v
        case "--window":
            i += 1
            guard i < arguments.count, let v = UInt32(arguments[i]) else { printUsageAndExit(2) }
            windowID = v
        case "--width":
            i += 1
            guard i < arguments.count, let v = Int(arguments[i]), v > 0 else { printUsageAndExit(2) }
            width = v
        case "--height":
            i += 1
            guard i < arguments.count, let v = Int(arguments[i]), v > 0 else { printUsageAndExit(2) }
            height = v
        case "--fps":
            i += 1
            guard i < arguments.count, let v = Int(arguments[i]), v > 0 else { printUsageAndExit(2) }
            fps = v
        case "--threshold":
            i += 1
            guard i < arguments.count, let v = Int(arguments[i]), v >= 0, v <= 255 else { printUsageAndExit(2) }
            threshold = UInt8(v)
        case "--cursor":
            showsCursor = true
        case "--no-cursor":
            showsCursor = false
        case "--png-dir":
            i += 1
            guard i < arguments.count else { printUsageAndExit(2) }
            pngDir = arguments[i]
        case "--debug":
            break // already consumed into the global `debugEnabled` above
        case "-h", "--help":
            printUsageAndExit(0)
        default:
            writeStderr("ScreenMirrorHelper: unknown argument '\(a)'")
            printUsageAndExit(2)
        }
        i += 1
    }

    guard (displayID != nil) != (windowID != nil) else {
        writeStderr("ScreenMirrorHelper: capture requires exactly one of --display <id> or --window <id>")
        exit(2)
    }

    await runCapture(CaptureArgs(displayID: displayID, windowID: windowID, width: width,
                                  height: height, fps: fps, threshold: threshold,
                                  showsCursor: showsCursor, pngDir: pngDir))

case "-h", "--help":
    printUsageAndExit(0)

default:
    writeStderr("ScreenMirrorHelper: unknown command '\(command)'")
    printUsageAndExit(2)
}
