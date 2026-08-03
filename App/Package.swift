// swift-tools-version: 5.10
import PackageDescription

// The Rust static library is built separately (see the repo-root Makefile:
// `make rust`). unsafeFlags is acceptable here because this is a root
// executable package, never consumed as a dependency.
//
// Engine-facing code lives in MixerCore rather than the executable so it
// can be unit-tested — the FFI boundary is exactly the part worth testing.
let package = Package(
    name: "MixerApp",
    platforms: [.macOS("14.4")],
    targets: [
        .systemLibrary(name: "CMixerFFI", path: "Sources/CMixerFFI"),
        .target(
            name: "MixerCore",
            dependencies: ["CMixerFFI"],
            linkerSettings: [
                .unsafeFlags([
                    "-L../RustCore/target/release",
                    "-lmixer_ffi",
                ])
            ]
        ),
        .executableTarget(name: "MixerApp", dependencies: ["MixerCore"]),
        .testTarget(name: "MixerCoreTests", dependencies: ["MixerCore"]),
    ]
)
