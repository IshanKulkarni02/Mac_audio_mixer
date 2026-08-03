import XCTest
@testable import MixerCore

/// These tests exercise the Swift↔Rust FFI boundary for real: every
/// assertion below is checking a value computed inside the Rust
/// dsp-engine, not re-derived in Swift. A regression in the FFI layer,
/// the cbindgen header, or the linked static library fails here.
final class StripEngineTests: XCTestCase {

    func testStripIsBackedByRustEngine() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput)
        XCTAssertTrue(strip.isEngineBacked, "Rust handle should be created; check libmixer_ffi.a is built and linked")
    }

    func testUnityGainAtZeroDB() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput)
        XCTAssertEqual(strip.appliedGain, 1.0, accuracy: 0.0001)
    }

    func testMinusSixDBIsRoughlyHalfAmplitude() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput, gainDB: -6)
        // -6 dB ≈ 0.501187 in amplitude, the classic "half power point".
        XCTAssertEqual(strip.appliedGain, 0.501187, accuracy: 0.0001)
    }

    func testGainChangePropagatesToEngine() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput)
        XCTAssertEqual(strip.appliedGain, 1.0, accuracy: 0.0001)

        strip.gainDB = -20
        XCTAssertEqual(strip.appliedGain, 0.1, accuracy: 0.0001, "gain set in Swift must reach Rust")

        strip.gainDB = 6
        XCTAssertEqual(strip.appliedGain, 1.995262, accuracy: 0.0001)
    }

    func testMuteForcesSilenceRegardlessOfGain() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput, gainDB: 12)
        XCTAssertGreaterThan(strip.appliedGain, 1.0)

        strip.muted = true
        XCTAssertEqual(strip.appliedGain, 0.0, "mute must win over any gain value")
    }

    func testUnmuteRestoresPreviousGain() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput, gainDB: -6)
        strip.muted = true
        XCTAssertEqual(strip.appliedGain, 0.0)

        strip.muted = false
        XCTAssertEqual(strip.appliedGain, 0.501187, accuracy: 0.0001, "gain must survive a mute/unmute cycle")
    }

    func testInitialMutedStateReachesEngine() {
        let strip = Strip(name: "Mic", subtitle: "Built-in", kind: .hardwareInput, muted: true)
        XCTAssertEqual(strip.appliedGain, 0.0, "muted:true passed to init must be pushed to Rust, not just stored in Swift")
    }

    func testManyStripsEachHoldIndependentEngineHandles() {
        let strips = (0..<50).map { i in
            Strip(name: "Strip \(i)", subtitle: "test", kind: .applicationInput, gainDB: Double(-i))
        }
        for (i, strip) in strips.enumerated() {
            let expected = powf(10, Float(-i) / 20)
            XCTAssertEqual(strip.appliedGain, expected, accuracy: 0.0001, "strip \(i) should not share state with its siblings")
        }
    }

    func testStoreStripsAreAllEngineBacked() {
        let store = MixerStore()
        for strip in store.inputs + store.outputs {
            XCTAssertTrue(strip.isEngineBacked, "\(strip.name) is not engine-backed")
        }
    }
}
