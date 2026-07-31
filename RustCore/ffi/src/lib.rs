//! C ABI surface. Both the HAL plugin (C++) and the Swift app will link
//! against this to reach the Rust DSP core — create/destroy a strip, set
//! its gain and mute state. Kept intentionally tiny for now; routing,
//! ring-buffer wiring, and EQ/comp/gate controls land here in later phases.

use dsp_engine::Strip;

#[no_mangle]
pub extern "C" fn mixer_strip_create() -> *mut Strip {
    Box::into_raw(Box::new(Strip::new()))
}

/// # Safety
/// `strip` must be a pointer returned by `mixer_strip_create` and not
/// already destroyed.
#[no_mangle]
pub unsafe extern "C" fn mixer_strip_destroy(strip: *mut Strip) {
    if strip.is_null() {
        return;
    }
    drop(Box::from_raw(strip));
}

/// # Safety
/// `strip` must be a live pointer from `mixer_strip_create`.
#[no_mangle]
pub unsafe extern "C" fn mixer_strip_set_gain_db(strip: *mut Strip, gain_db: f32) {
    if let Some(s) = strip.as_mut() {
        s.gain_db = gain_db;
    }
}

/// # Safety
/// `strip` must be a live pointer from `mixer_strip_create`.
#[no_mangle]
pub unsafe extern "C" fn mixer_strip_set_muted(strip: *mut Strip, muted: bool) {
    if let Some(s) = strip.as_mut() {
        s.muted = muted;
    }
}
