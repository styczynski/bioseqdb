//! Fuzz target for CRC-64/NVME digest calculation, which is a reflected variant.

#![no_main]

use crc_fast::{CrcAlgorithm, Digest};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut digest = Digest::new(CrcAlgorithm::Crc64Nvme);
    digest.update(data);
    digest.finalize();
});
