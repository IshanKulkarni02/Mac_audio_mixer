import Foundation

public enum StripKind {
    case hardwareInput
    case virtualInput
    case applicationInput
    case output
}

/// A mixer channel strip. UI state here is not a separate copy of the
/// truth — every gain/mute change is pushed straight through to the Rust
/// engine, and `appliedGain` reads back what the engine will actually do.
@Observable
public final class Strip: Identifiable {
    public let id = UUID()
    public let name: String
    public let subtitle: String
    public let kind: StripKind

    /// Retained for the lifetime of the strip; frees the Rust handle on deinit.
    @ObservationIgnored private let engine: EngineStrip?

    public var gainDB: Double {
        didSet { engine?.gainDB = Float(gainDB) }
    }

    public var muted: Bool {
        didSet { engine?.muted = muted }
    }

    /// Linear gain as computed by the Rust engine — 0 when muted. Falls
    /// back to 0 if the engine handle could not be created, which is
    /// audibly safe (silence) rather than a surprise full-scale signal.
    public var appliedGain: Float {
        engine?.linearGain ?? 0
    }

    /// True when this strip is backed by a live Rust engine handle.
    public var isEngineBacked: Bool { engine != nil }

    public init(name: String, subtitle: String, kind: StripKind, gainDB: Double = 0, muted: Bool = false) {
        self.name = name
        self.subtitle = subtitle
        self.kind = kind
        self.gainDB = gainDB
        self.muted = muted

        let engine = EngineStrip()
        engine?.gainDB = Float(gainDB)
        engine?.muted = muted
        self.engine = engine
    }
}
