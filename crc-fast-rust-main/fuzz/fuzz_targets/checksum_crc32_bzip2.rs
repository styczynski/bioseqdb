//! Fuzz target for CRC-32/BZIP2 checksum calculation, which is a forward variant.

#![no_main]

use crc_fast::{CrcAlgorithm, checksum};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    checksum(CrcAlgorithm::Crc32Bzip2, data);
});
