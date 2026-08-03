# Top-level build. The pieces build in dependency order: the Rust static
# library and its generated header must exist before the Swift app can
# link against them.

.PHONY: all rust header app driver test clean

all: app driver

## Build the Rust DSP core + FFI static library (release).
rust:
	cd RustCore && cargo build --release

## Copy the cbindgen-generated header into the Swift package.
## This copy is generated — never edit App/Sources/CMixerFFI/include/
## by hand, or Swift and Rust will silently disagree about the ABI.
header: rust
	cp RustCore/ffi/include/mixer_ffi.h App/Sources/CMixerFFI/include/mixer_ffi.h

## Build the SwiftUI app against the Rust library.
app: header
	cd App && swift build

## Build the CoreAudio HAL virtual device driver.
driver:
	$(MAKE) -C HALPlugin

## Run all tests: Rust unit tests + the Swift↔Rust FFI boundary tests.
test: header
	cd RustCore && cargo test
	cd App && swift test

clean:
	cd RustCore && cargo clean
	cd App && swift package clean
	$(MAKE) -C HALPlugin clean
