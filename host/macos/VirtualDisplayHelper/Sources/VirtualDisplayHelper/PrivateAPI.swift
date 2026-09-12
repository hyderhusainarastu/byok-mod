//
//  PrivateAPI.swift
//  VirtualDisplayHelper
//
//  Dynamic, header-free bindings for the private CoreGraphics virtual-display
//  API. We declare ONLY the selectors we call, as @objc protocols, and reach
//  the concrete private classes at runtime with NSClassFromString. No private
//  headers are vendored. The protocol shapes mirror the ObjC declarations found
//  in com.apple.CoreGraphics (as used by DeskPad / BetterDisplay), with scalar
//  widths chosen to match the ObjC types exactly:
//
//      ObjC type          Swift type used here
//      -----------------  --------------------
//      unsigned int       UInt32
//      NSUInteger         UInt        (64-bit on arm64/x86_64)
//      CGFloat            CGFloat
//      CGSize             CGSize
//      CGDirectDisplayID  UInt32
//
//  The four classes and the exact selectors we depend on:
//
//    @interface CGVirtualDisplayMode : NSObject
//      - (instancetype)initWithWidth:(NSUInteger)w height:(NSUInteger)h refreshRate:(CGFloat)r;
//      @property(readonly) NSUInteger width, height; @property(readonly) CGFloat refreshRate;
//
//    @interface CGVirtualDisplaySettings : NSObject
//      - (instancetype)init;
//      @property(retain) NSArray<CGVirtualDisplayMode*> *modes;
//      @property unsigned int hiDPI;
//
//    @interface CGVirtualDisplayDescriptor : NSObject
//      - (instancetype)init;
//      @property(copy) NSString *name;
//      @property unsigned int maxPixelsWide, maxPixelsHigh, serialNum, productID, vendorID;
//      @property CGSize sizeInMillimeters;
//      @property(copy) void (^terminationHandler)(id, CGVirtualDisplay*);
//      - (void)setDispatchQueue:(dispatch_queue_t)q;
//
//    @interface CGVirtualDisplay : NSObject
//      - (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor*)d;
//      - (BOOL)applySettings:(CGVirtualDisplaySettings*)s;
//      @property(readonly) CGDirectDisplayID displayID;
//

import Foundation
import CoreGraphics

// MARK: - @objc protocol shapes for the private classes

@objc protocol VDDescriptor {
    var name: NSString { get set }
    var maxPixelsWide: UInt32 { get set }
    var maxPixelsHigh: UInt32 { get set }
    var sizeInMillimeters: CGSize { get set }
    var serialNum: UInt32 { get set }
    var productID: UInt32 { get set }
    var vendorID: UInt32 { get set }
    @objc(setDispatchQueue:) func setDispatchQueue(_ queue: DispatchQueue)
    var terminationHandler: (@convention(block) (AnyObject?, AnyObject?) -> Void)? { get set }
}

@objc protocol VDSettings {
    var modes: NSArray { get set }
    var hiDPI: UInt32 { get set }
}

@objc protocol VDMode {
    // initWithWidth:height:refreshRate: — "init" family, so ARC balances the
    // return automatically. NSUInteger => UInt to match the ObjC ABI on 64-bit.
    @objc(initWithWidth:height:refreshRate:)
    func initWith(width: UInt, height: UInt, refreshRate: CGFloat) -> VDMode
    var width: UInt { get }
    var height: UInt { get }
    var refreshRate: CGFloat { get }
}

@objc protocol VDDisplay {
    @objc(initWithDescriptor:)
    func initWith(descriptor: AnyObject) -> VDDisplay
    @objc(applySettings:)
    func applySettings(_ settings: AnyObject) -> Bool
    var displayID: UInt32 { get }
}

// MARK: - Runtime factory

enum PrivateVD {
    /// True only if every private class we need is present in this process.
    static var isAvailable: Bool {
        NSClassFromString("CGVirtualDisplayDescriptor") != nil
            && NSClassFromString("CGVirtualDisplaySettings") != nil
            && NSClassFromString("CGVirtualDisplayMode") != nil
            && NSClassFromString("CGVirtualDisplay") != nil
    }

    /// Allocate + `-init` a class that has a plain designated initializer.
    /// Returns the raw NSObject (keep a strong ref) plus a protocol view of it.
    private static func makePlain<P>(_ className: String, as _: P.Type) -> (NSObject, P)? {
        guard let cls = NSClassFromString(className) as? NSObject.Type else { return nil }
        let obj = cls.init()
        return (obj, unsafeBitCast(obj, to: P.self))
    }

    static func makeDescriptor() -> (NSObject, VDDescriptor)? {
        makePlain("CGVirtualDisplayDescriptor", as: VDDescriptor.self)
    }

    static func makeSettings() -> (NSObject, VDSettings)? {
        makePlain("CGVirtualDisplaySettings", as: VDSettings.self)
    }

    /// CGVirtualDisplayMode has no plain -init; drive alloc/initWith… manually.
    /// alloc returns +1: we take it UNRETAINED so ARC does not own that +1, then
    /// initWith… (init family) consumes it and returns a +1 the existential owns.
    static func makeMode(width: UInt, height: UInt, refreshRate: CGFloat) -> AnyObject? {
        guard let cls = NSClassFromString("CGVirtualDisplayMode") else { return nil }
        guard let allocated = (cls as AnyObject).perform(NSSelectorFromString("alloc"))?
            .takeUnretainedValue() else { return nil }
        let mode = unsafeBitCast(allocated, to: VDMode.self)
            .initWith(width: width, height: height, refreshRate: refreshRate)
        return mode as AnyObject
    }

    /// CGVirtualDisplay is created with -initWithDescriptor:; same alloc dance.
    static func makeDisplay(descriptor: NSObject) -> (AnyObject, VDDisplay)? {
        guard let cls = NSClassFromString("CGVirtualDisplay") else { return nil }
        guard let allocated = (cls as AnyObject).perform(NSSelectorFromString("alloc"))?
            .takeUnretainedValue() else { return nil }
        let display = unsafeBitCast(allocated, to: VDDisplay.self)
            .initWith(descriptor: descriptor)
        return (display as AnyObject, display)
    }
}
