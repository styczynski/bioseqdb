//! Fuzz target for CRC-64/NVME checksum calculation, which is a reflected variant.

#![no_main]

use crc_fast::{CrcAlgorithm, checksum};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    checksum(CrcAlgorithm::Crc64Nvme, data);
});
