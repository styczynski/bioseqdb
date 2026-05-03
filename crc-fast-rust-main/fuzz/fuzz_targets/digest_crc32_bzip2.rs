//! Fuzz target for CRC-32/BZIP2 digest calculation, which is a forward variant.

#![no_main]

use crc_fast::{CrcAlgorithm, Digest};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut digest = Digest::new(CrcAlgorithm::Crc32Bzip2);
    digest.update(data);
    digest.finalize();
});
