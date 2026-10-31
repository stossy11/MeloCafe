import UIKit
import SwiftUI
import GameController
import Combine

@main
final class AppDelegate: UIResponder, UIApplicationDelegate {
    func application(_ application: UIApplication,
                     didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]?) -> Bool {
        setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 1)
        setenv("MVK_CONFIG_DEBUG", "0", 1)
        setenv("MVK_CONFIG_MAX_ACTIVE_METAL_COMMAND_BUFFERS_PER_QUEUE", "128", 1)
        CemuManager.initialize()
        return true
    }
}

final class MainSceneDelegate: UIResponder, UIWindowSceneDelegate {
    var window: UIWindow?
    
    func scene(_ scene: UIScene, willConnectTo session: UISceneSession,
               options connectionOptions: UIScene.ConnectionOptions) {
        guard let windowScene = scene as? UIWindowScene else { return }
        let controller: ContentHostingController
        if #available(iOS 27.0, *) {
            controller = AccessoryContentHostingController()
        } else {
            controller = ContentHostingController()
        }
        let window = UIWindow(windowScene: windowScene)
        self.window = window
        
        CemuUIKit_SetMainWindow(window)
        window.rootViewController = controller
        controller.open(connectionOptions.urlContexts.map(\.url))
        window.makeKeyAndVisible()
    }
    
    func scene(_ scene: UIScene, openURLContexts URLContexts: Set<UIOpenURLContext>) {
        (window?.rootViewController as? ContentHostingController)?.open(URLContexts.map(\.url))
    }
}

class ContentHostingController: UIHostingController<AnyView> {
    private let gameManager = GamesManager.shared
    private let cemuView = MetalView()
    private let cemuPadView = MetalView()
    private var pendingURLs: [URL] = []
    private var ready = false
    
    init() {
        super.init(rootView: AnyView(ContentView(cemuView: cemuView, cemuPadView: cemuPadView)
            .environmentObject(GamesManager.shared)))
    }
    
    @MainActor required dynamic init?(coder aDecoder: NSCoder) {
        fatalError("Use init() to create the app interface")
    }
    
    override func viewDidLoad() {
        super.viewDidLoad()
        installJITTrapHandler()
        configureJITEnvironment()
        if !checkAppEntitlement("get-task-allow") && !CPUMode(CemuConfigWrapper.shared().cpuMode).isInterpreter {
            CemuConfigWrapper.shared().cpuMode = .multicoreInterpreter
        }
        setupEmulation()
    }
    
    
    override func viewDidAppear(_ animated: Bool) {
        super.viewDidAppear(animated)
        guard !ready else { return }
        ready = true
        
        if !pendingURLs.isEmpty {
            let urls = pendingURLs
            pendingURLs.removeAll()
            open(urls)
        } else if let gameCString = getenv("GAME") {
            let gameID = String(cString: gameCString)
            if let game = gameManager.games.first(where: { $0.id == UInt64(gameID) || $0.title == gameID }) {
                gameManager.loadGame(game)
            }
        }
    }
    
    func open(_ urls: [URL]) {
        guard ready else {
            pendingURLs.append(contentsOf: urls)
            return
        }
        urls.forEach(gameManager.handleDeepLink)
    }
    
    private func configureJITEnvironment() {
        if #available(iOS 19.0, *) {
            setenv("DUAL_MAPPED_JIT", "1", 1)
            setenv("HAS_TXM", !ProcessInfo.processInfo.isiOSAppOnMac && ProcessInfo.processInfo.hasTXM ? "1" : "0", 1)
        } else {
            setenv("HAS_TXM", "0", 1)
        }
    }
    
    private func setupEmulation() {
        let cemuConfig = CemuConfigWrapper.shared()
        if FileManager.default.fileExists(atPath: URL.configURL.path) {
            cemuConfig.loadConfig()
        } else {
            cemuConfig.saveConfig()
            cemuConfig.loadConfig()
        }
        
        cemuConfig.logFlag = 0
        cemuConfig.advancedPPCLogging = false
        gameManager.loadGames()
        setupMetalViews()
        setupControllers()
        
        if !CPUMode(cemuConfig.cpuMode).isInterpreter {
            CemuInitJIT()
        }
    }
    
    private func setupMetalViews() {
        let bounds = view.bounds
        
        cemuView.frame = bounds
        cemuView.configure(main: true)
        
        cemuPadView.frame = bounds
        cemuPadView.configure(main: false)
        
        CemuUIKit_SetMainView(cemuView)
        CemuUIKit_SetPadView(cemuPadView)
    }
    
    private func setupControllers() {
        guard ControllerManager.shared.controllerCount() <= 0,
              !GCController.controllers().isEmpty else { return }
        
        ControllerManager.shared.searchForControllers()
    }
}

@available(iOS 27.0, *)
final class AccessoryContentHostingController: ContentHostingController {
    private var displayRegistration: UISceneAccessoryRegistration?
    private var playbackSubscription: AnyCancellable?
    
    override func viewDidLoad() {
        super.viewDidLoad()
        let configuration = UISceneConfiguration()
        configuration.delegateClass = ExternalDisplaySceneDelegate.self
        let registration = registerSceneAccessory(.externalNonInteractive(sceneConfiguration: configuration))
        displayRegistration = registration
        playbackSubscription = Air.shared.$isPlaying.sink { [weak registration] playing in
            registration?.isEnabled = playing
        }
    }
}
