import SwiftUI

@main
struct MixerApp: App {
    var body: some Scene {
        WindowGroup("Mixer") {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}
