import SwiftUI

@main
struct OpenPlaudApp: App {
    @StateObject private var settings = AppSettings()
    @StateObject private var model: AppModel

    init() {
        let s = AppSettings()
        _settings = StateObject(wrappedValue: s)
        _model = StateObject(wrappedValue: AppModel(settings: s))
    }

    var body: some Scene {
        Window("open-plaud", id: "main") {
            ContentView()
                .environmentObject(settings)
                .environmentObject(model)
                .frame(minWidth: 560, minHeight: 420)
                .onAppear {
                    model.checkHealth()
                    if model.folderURL != nil { model.scan() }
                }
        }
        .windowResizability(.contentSize)

        Settings {
            SettingsView()
                .environmentObject(settings)
                .environmentObject(model)
                .frame(width: 460)
        }
    }
}
