import CMixerFFI
import Foundation

/// Swift-side owner of a Rust `Strip` handle.
///
/// The Rust side hands back a raw pointer that must be freed exactly once;
/// tying it to a class and freeing in `deinit` makes that automatic and
/// removes any chance of a caller forgetting. The pointer is never exposed,
/// so it can't outlive this object or be freed twice.
final class EngineStrip {
    private let handle: OpaquePointer

    init?() {
        guard let handle = mixer_strip_create() else { return nil }
        self.handle = handle
    }

    deinit {
        mixer_strip_destroy(handle)
    }

    var gainDB: Float = 0 {
        didSet { mixer_strip_set_gain_db(handle, gainDB) }
    }

    var muted: Bool = false {
        didSet { mixer_strip_set_muted(handle, muted) }
    }

    /// The gain the engine will actually apply. Read back from Rust rather
    /// than recomputed here, so the UI can never disagree with the DSP.
    var linearGain: Float {
        mixer_strip_linear_gain(handle)
    }
}
