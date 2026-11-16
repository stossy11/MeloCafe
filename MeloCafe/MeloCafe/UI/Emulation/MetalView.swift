//
//  MetalView.swift
//  MeloCafe
//
//  Created by Stossy11 on 5/3/2026.
//

import UIKit
import QuartzCore
import Metal
import SwiftUI

struct MetalKitView: UIViewRepresentable {
    let mtkView: MetalView

    func makeUIView(context: Context) -> MetalView {
        mtkView.updateDrawableSize(); return mtkView
    }

    func updateUIView(_ uiView: MetalView, context: Context) {
        uiView.updateDrawableSize(); uiView.setNeedsDisplay()
    }

}


class MetalView: UIView {
    private(set) var main: Bool = true
    private var requestedScale: CGFloat?
    private var requestedRenderScale: CGFloat = 1.0
    private let captureRenderScale: CGFloat = 0.75
    private let nativeRecordingRenderScale: CGFloat = 0.75
    private var nativeRecordingActive = false
    private var observingCaptureChanges = false
    
    override class var layerClass: AnyClass {
        return CAMetalLayer.self
    }
    
    var metalLayer: CAMetalLayer {
        return layer as! CAMetalLayer
    }
    
    func configure(main: Bool) {
        backgroundColor = .black
        isOpaque = true
        
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.allowsNextDrawableTimeout = true
        metalLayer.maximumDrawableCount = 3
        metalLayer.presentsWithTransaction = false
        metalLayer.backgroundColor = UIColor.black.cgColor
        metalLayer.isOpaque = true
        
        if !observingCaptureChanges {
            NotificationCenter.default.addObserver(
                self,
                selector: #selector(screenCaptureStateChanged),
                name: UIScreen.capturedDidChangeNotification,
                object: nil
            )
            observingCaptureChanges = true
        }
        
        self.main = main
        isMultipleTouchEnabled = !main
        updateDrawableSize()
    }
    
    deinit {
        NotificationCenter.default.removeObserver(self)
    }
    
    override func didMoveToWindow() {
        super.didMoveToWindow()
        if window == nil { cancelActiveTouches() }
        updateDrawableSize()
    }
    
    override func layoutSubviews() {
        super.layoutSubviews()
        updateDrawableSize()
    }
    
    func updateDrawableSize(scale: CGFloat? = nil, renderScale: CGFloat? = nil) {
        if let scale {
            requestedScale = scale
        }
        if let renderScale {
            requestedRenderScale = renderScale
        }
        
        let scale = requestedScale ?? window?.screen.nativeScale ?? UIScreen.main.nativeScale
        let renderScale = effectiveRenderScale
        guard bounds.width > 0, bounds.height > 0 else { return }
        metalLayer.contentsScale = scale
        metalLayer.drawableSize = CGSize(
            width: bounds.width * scale * renderScale,
            height: bounds.height * scale * renderScale
        )
        metalLayer.contentsGravity = .resizeAspectFill
        
        if self.main {
            CemuUIKit_UpdateMainWindowSize(bounds.width * renderScale, bounds.height * renderScale, scale)
        } else {
            CemuUIKit_UpdatePadWindowSize()
        }
    }
    
    private var effectiveRenderScale: CGFloat {
        if nativeRecordingActive {
            return min(requestedRenderScale, nativeRecordingRenderScale)
        }
        if isScreenCaptureActive {
            return min(requestedRenderScale, captureRenderScale)
        }
        return requestedRenderScale
    }
    
    func setNativeRecordingActive(_ active: Bool) {
        nativeRecordingActive = active
        updateDrawableSize()
    }
    
    private var isScreenCaptureActive: Bool {
        if UIScreen.main.isCaptured {
            return true
        }
        return window?.screen.isCaptured ?? false
    }
    
    @objc private func screenCaptureStateChanged() {
        updateDrawableSize()
    }
    
    
    private var activeTouches: [UITouch] = []
    
    override func touchesBegan(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard !self.main else { return }
        for touch in touches {
            activeTouches.append(touch)
        }
        reportTopTouch()
    }
    
    override func touchesMoved(_ touches: Set<UITouch>, with event: UIEvent?) {
        guard !self.main, let top = activeTouches.last, touches.contains(top) else { return }
        reportTopTouch()
    }
    
    override func touchesEnded(_ touches: Set<UITouch>, with event: UIEvent?) {
        handleLifted(touches)
    }
    
    override func touchesCancelled(_ touches: Set<UITouch>, with event: UIEvent?) {
        handleLifted(touches)
    }
    
    func cancelActiveTouches() {
        guard !main, !activeTouches.isEmpty else { return }
        activeTouches.removeAll()
        CemuUIKit_SetPadTouch(0, 0, false)
    }
    
    private func handleLifted(_ touches: Set<UITouch>) {
        guard !self.main else { return }
        activeTouches.removeAll(where: { touches.contains($0) })
        if let top = activeTouches.last {
            forward(touch: top, down: true)
        } else {
            if let lifted = touches.first {
                forward(touch: lifted, down: false)
            }
        }
    }
    
    private func reportTopTouch() {
        guard let top = activeTouches.last else { return }
        forward(touch: top, down: true)
    }
    
    private func forward(touch: UITouch, down: Bool) {
        let p = touch.location(in: self)
        guard bounds.width > 0, bounds.height > 0 else { return }
        CemuUIKit_SetPadTouch(
            p.x * metalLayer.drawableSize.width / bounds.width,
            p.y * metalLayer.drawableSize.height / bounds.height, down)
    }
}
