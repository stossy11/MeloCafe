//
//  AppIconSwitcher.swift
//  MeloCafe
//
//  Created by Stossy11 on 18/6/2026.
//

import SwiftUI

struct AppIconSwitcher: View {
    @State var icons: [AppIconPosition] = [
        .init(creator: "Transistor", icons: [.init(id: "AppIcon", name: "App Icon", def: true), .init(id: "AppIcon-Classic", name: "Classic App Icon")]),
        .init(creator: "sky (@dootskyre)", icons: [
            .init(id: "PixelCafeAppIcon", name: "PixelCafé")
        ])
    ]

    var body: some View {
        List {
            ForEach(icons) { iconSet in
                Section(iconSet.creator) {
                    ForEach(iconSet.icons) { icon in
                        Button {
                            UIApplication.shared.setAlternateIconName(icon.id == "AppIcon" ? nil : icon.id)
                        } label: {
                            HStack {
                                AppIconView(app: icon.id)
                                    .frame(width: 60, height: 60)
                                    .aspectRatio(1, contentMode: .fill)
                                    .padding(.leading)
                                    .padding(.trailing, 8)

                                VStack(alignment: .leading, spacing: 4) {
                                    Text(icon.name)
                                        .font(.headline)
                                }
                            }
                        }
                    }
                }
            }
        }
        .navigationTitle("App Icon Switcher")
    }
}

struct AppIconView: View {
    let app: String

    var body: some View {
        if let iconImage = UIImage(named: app) ?? UIImage.loadFromAssetsCatalog(named: app) {
            Image(uiImage: iconImage)
                .resizable()
                .scaledToFill()
                .clipShape(RoundedRectangle(cornerRadius: 15.0, style: .continuous))
                .clipped()
        } else {
            ZStack {
                Rectangle()
                    .foregroundStyle(.tertiary)

                ProgressView()
            }
            .clipShape(RoundedRectangle(cornerRadius: 15.0, style: .continuous))
        }
    }
}


extension UIImage {
    static func loadFromAssetsCatalog(named name: String) -> UIImage? {
        func runtimeName(_ encoded: String) -> String {
            String(decoding: Data(base64Encoded: encoded)!, as: UTF8.self)
        }

        guard let carURL = Bundle.main.url(forResource: "Assets", withExtension: "car") else {
            print("Could not find Assets.car")
            return nil
        }
        
        guard let coreUIBundle = Bundle(path: runtimeName("L1N5c3RlbS9MaWJyYXJ5L1ByaXZhdGVGcmFtZXdvcmtzL0NvcmVVSS5mcmFtZXdvcms=")) else {
            print("Could not load the asset catalog framework")
            return nil
        }
        
        if !coreUIBundle.isLoaded { coreUIBundle.load() }
        
        guard let catalogClass = NSClassFromString(runtimeName("Q1VJQ2F0YWxvZw==")) as? NSObject.Type else {
            print("Could not find the asset catalog class")
            return nil
        }
        
        let catalog = catalogClass.init()
        let initSel = NSSelectorFromString(runtimeName("aW5pdFdpdGhVUkw6ZXJyb3I6"))
        guard catalog.responds(to: initSel) else { return nil }
        
        var error: NSError?
        guard let validCatalog = withUnsafeMutablePointer(to: &error, { errorPtr in
            catalog.perform(initSel, with: carURL, with: errorPtr)?.takeUnretainedValue() as? NSObject
        }) else {
            print("Failed to open the asset catalog: \(error?.localizedDescription ?? "unknown")")
            return nil
        }
        
        let imagesWithNameSel = NSSelectorFromString(runtimeName("aW1hZ2VzV2l0aE5hbWU6"))
        guard validCatalog.responds(to: imagesWithNameSel),
              let results = validCatalog.perform(imagesWithNameSel, with: name)?.takeUnretainedValue() as? [NSObject] else {
            print("Failed to look up the asset named \(name)")
            return nil
        }
        
        let cuiNamedImageClass: AnyClass? = NSClassFromString(runtimeName("Q1VJTmFtZWRJbWFnZQ=="))
        let cuiMultisizeClass: AnyClass? = NSClassFromString(runtimeName("Q1VJTmFtZWRNdWx0aXNpemVJbWFnZVNldA=="))
        let imageSel = NSSelectorFromString(runtimeName("aW1hZ2U="))
        
        guard let namedImage = results.first(where: { obj in
            guard let cls = cuiNamedImageClass else { return false }
            if let multiCls = cuiMultisizeClass, obj.isKind(of: multiCls) { return false }
            return obj.isKind(of: cls) && obj.responds(to: imageSel)
        }) else {
            print("No usable image found for \(name)")
            return nil
        }
        
        typealias ImageIMP = @convention(c) (NSObject, Selector) -> CGImage?
        let imageIMP = unsafeBitCast(namedImage.method(for: imageSel), to: ImageIMP.self)
        
        guard let cgImage = imageIMP(namedImage, imageSel) else {
            print("CGImageRef was nil for: \(name)")
            return nil
        }
        
        let scaleSel = NSSelectorFromString(runtimeName("c2NhbGU="))
        typealias ScaleIMP = @convention(c) (NSObject, Selector) -> Double
        let scaleIMP = unsafeBitCast(namedImage.method(for: scaleSel), to: ScaleIMP.self)
        let imageScale = CGFloat(scaleIMP(namedImage, scaleSel))
        
        return UIImage(cgImage: cgImage, scale: imageScale, orientation: .up)
    }
}
