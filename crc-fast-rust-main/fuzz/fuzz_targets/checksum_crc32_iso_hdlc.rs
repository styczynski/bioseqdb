//! Fuzz target for CRC-32/ISO-HDLC checksum calculation, which is a reflected variant which can use
//! fusion techniques on aarch64 architectures.

#![no_main]

use crc_fast::{CrcAlgorithm, checksum};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    checksum(CrcAlgorithm::Crc32IsoHdlc, data);
});
