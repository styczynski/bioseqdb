//! Fuzz target for CRC-64/ECMA-182 checksum calculation, which is a forward variant.

#![no_main]

use crc_fast::{CrcAlgorithm, checksum};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    checksum(CrcAlgorithm::Crc64Ecma182, data);
});
