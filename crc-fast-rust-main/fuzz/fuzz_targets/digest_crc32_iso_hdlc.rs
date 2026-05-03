//! Fuzz target for CRC-32/ISO-HDLC digest calculation, which is a reflected variant which can use
//! fusion techniques on aarch64 architectures.

#![no_main]

use crc_fast::{CrcAlgorithm, Digest};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let mut digest = Digest::new(CrcAlgorithm::Crc32IsoHdlc);
    digest.update(data);
    digest.finalize();
});
