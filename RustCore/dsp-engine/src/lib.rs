//! Real-time mixer core: no allocation, no locks, nothing blocking on the
//! audio thread. This crate owns the parts of the plan's Phase 1 scope that
//! are safe to write and test before any Core Audio glue exists: per-strip
//! gain/mute math and the lock-free ring buffer that will bridge producer
//! threads (taps, mic input) to the render callback.

use ringbuf::{traits::Split, HeapCons, HeapProd, HeapRb};

#[derive(Debug, Clone, Copy)]
pub struct Strip {
    pub gain_db: f32,
    pub muted: bool,
}

impl Strip {
    pub fn new() -> Self {
        Self {
            gain_db: 0.0,
            muted: false,
        }
    }

    /// Public so callers across the FFI boundary can read back the gain
    /// the engine will actually apply — a mute reads 0.0 regardless of
    /// the dB value, which is exactly what a meter or UI needs to show.
    pub fn linear_gain(&self) -> f32 {
        if self.muted {
            0.0
        } else {
            10f32.powf(self.gain_db / 20.0)
        }
    }
}

impl Default for Strip {
    fn default() -> Self {
        Self::new()
    }
}

/// Sums one sample per active strip into a single output sample. This is
/// the per-sample shape the render callback will use once real input
/// buffers exist; exposed standalone so the mix math is unit-testable
/// without any audio hardware involved.
pub fn mix_frame(strips: &[Strip], samples: &[f32]) -> f32 {
    debug_assert_eq!(strips.len(), samples.len());
    strips
        .iter()
        .zip(samples.iter())
        .map(|(strip, sample)| sample * strip.linear_gain())
        .sum()
}

pub type AudioProducer = HeapProd<f32>;
pub type AudioConsumer = HeapCons<f32>;

/// A lock-free SPSC ring buffer sized in samples. One producer thread
/// (a tap, mic input, or a network receiver) and exactly one consumer
/// (the Core Audio render callback) share it — never more than one of
/// each, per the plan's real-time threading rules.
pub fn make_ring(capacity_samples: usize) -> (AudioProducer, AudioConsumer) {
    HeapRb::<f32>::new(capacity_samples).split()
}

#[cfg(test)]
mod tests {
    use super::*;
    use ringbuf::traits::{Consumer, Producer};

    #[test]
    fn muted_strip_contributes_silence() {
        let strips = [Strip::new(), Strip { gain_db: 0.0, muted: true }];
        let samples = [1.0, 1.0];
        assert_eq!(mix_frame(&strips, &samples), 1.0);
    }

    #[test]
    fn gain_is_applied_in_db() {
        let strips = [Strip { gain_db: -6.0, muted: false }];
        let samples = [1.0];
        let mixed = mix_frame(&strips, &samples);
        assert!((mixed - 0.501187).abs() < 0.0001);
    }

    #[test]
    fn ring_buffer_round_trips_samples() {
        let (mut producer, mut consumer) = make_ring(8);
        producer.try_push(0.5).unwrap();
        producer.try_push(-0.25).unwrap();
        assert_eq!(consumer.try_pop(), Some(0.5));
        assert_eq!(consumer.try_pop(), Some(-0.25));
        assert_eq!(consumer.try_pop(), None);
    }
}
