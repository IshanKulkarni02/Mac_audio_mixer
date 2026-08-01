import Foundation

/// Holds the mixer's current strips. Gain/mute here only drive the UI so
/// far — nothing is wired to real audio yet. That wiring happens in two
/// later steps: (1) linking gain/mute through RustCore's FFI layer
/// (`mixer_strip_set_gain_db` / `mixer_strip_set_muted`) into the
/// dsp-engine, and (2) creating real inputs — a mic strip via
/// AVAudioEngine, per-app strips via CATapDescription (see
/// Spikes/ProcessTapSpike for the validated approach) — once the
/// HALPlugin virtual output is installed and can be selected as a real
/// Core Audio device.
@Observable
final class MixerStore {
    var inputs: [Strip]
    var outputs: [Strip]

    init() {
        inputs = [
            Strip(name: "Microphone", subtitle: "Built-in", kind: .hardwareInput),
            Strip(name: "Virtual In 1", subtitle: "App playback device", kind: .virtualInput),
            Strip(name: "Discord", subtitle: "Process tap", kind: .applicationInput),
        ]
        outputs = [
            Strip(name: "AudioMixer Output", subtitle: "Virtual · HALPlugin", kind: .output),
        ]
    }
}
