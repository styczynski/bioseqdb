// Copyright 2025 Don MacAskill. Licensed under MIT or Apache-2.0.

//! This module contains a software fallback for unsupported architectures.

use crate::tables;
use crate::CrcAlgorithm;
use crate::CrcParams;

// ============================================================================
// Native Table Generation Functions
// ============================================================================

/// Computes a single CRC-16 value for table generation.
const fn crc16_single(poly: u16, reflect: bool, mut value: u16) -> u16 {
    if reflect {
        let mut i = 0;
        while i < 8 {
            value = (value >> 1) ^ ((value & 1) * poly);
            i += 1;
        }
    } else {
        value <<= 8;
        let mut i = 0;
        while i < 8 {
            value = (value << 1) ^ (((value >> 15) & 1) * poly);
            i += 1;
        }
    }
    value
}

/// Computes a single CRC-32 value for table generation.
const fn crc32_single(poly: u32, reflect: bool, mut value: u32) -> u32 {
    if reflect {
        let mut i = 0;
        while i < 8 {
            value = (value >> 1) ^ ((value & 1) * poly);
            i += 1;
        }
    } else {
        value <<= 24;
        let mut i = 0;
        while i < 8 {
            value = (value << 1) ^ (((value >> 31) & 1) * poly);
            i += 1;
        }
    }
    value
}

/// Computes a single CRC-64 value for table generation.
const fn crc64_single(poly: u64, reflect: bool, mut value: u64) -> u64 {
    if reflect {
        let mut i = 0;
        while i < 8 {
            value = (value >> 1) ^ ((value & 1) * poly);
            i += 1;
        }
    } else {
        value <<= 56;
        let mut i = 0;
        while i < 8 {
            value = (value << 1) ^ (((value >> 63) & 1) * poly);
            i += 1;
        }
    }
    value
}

/// Generates a 16-lane lookup table for CRC-16 calculations.
///
/// This function creates a table compatible with the `crc` crate's `Table<16>` format,
/// enabling processing of 16 bytes at a time for improved performance.
pub const fn generate_table_u16(width: u8, poly: u16, reflect: bool) -> [[u16; 256]; 16] {
    let poly = if reflect {
        let poly = poly.reverse_bits();
        poly >> (16u8 - width)
    } else {
        poly << (16u8 - width)
    };

    let mut table = [[0u16; 256]; 16];

    // Generate first table (lane 0) directly
    let mut i = 0;
    while i < 256 {
        table[0][i] = crc16_single(poly, reflect, i as u16);
        i += 1;
    }

    // Generate subsequent lanes based on lane 0
    let mut i = 0;
    while i < 256 {
        let mut e = 1;
        while e < 16 {
            let one_lower = table[e - 1][i];
            if reflect {
                table[e][i] = (one_lower >> 8) ^ table[0][(one_lower & 0xFF) as usize];
            } else {
                table[e][i] = (one_lower << 8) ^ table[0][((one_lower >> 8) & 0xFF) as usize];
            }
            e += 1;
        }
        i += 1;
    }

    table
}

/// Generates a 16-lane lookup table for CRC-32 calculations.
///
/// This function creates a table compatible with the `crc` crate's `Table<16>` format,
/// enabling processing of 16 bytes at a time for improved performance.
pub const fn generate_table_u32(width: u8, poly: u32, reflect: bool) -> [[u32; 256]; 16] {
    let poly = if reflect {
        let poly = poly.reverse_bits();
        poly >> (32u8 - width)
    } else {
        poly << (32u8 - width)
    };

    let mut table = [[0u32; 256]; 16];

    // Generate first table (lane 0) directly
    let mut i = 0;
    while i < 256 {
        table[0][i] = crc32_single(poly, reflect, i as u32);
        i += 1;
    }

    // Generate subsequent lanes based on lane 0
    let mut i = 0;
    while i < 256 {
        let mut e = 1;
        while e < 16 {
            let one_lower = table[e - 1][i];
            if reflect {
                table[e][i] = (one_lower >> 8) ^ table[0][(one_lower & 0xFF) as usize];
            } else {
                table[e][i] = (one_lower << 8) ^ table[0][((one_lower >> 24) & 0xFF) as usize];
            }
            e += 1;
        }
        i += 1;
    }

    table
}

/// Generates a 16-lane lookup table for CRC-64 calculations.
///
/// This function creates a table compatible with the `crc` crate's `Table<16>` format,
/// enabling processing of 16 bytes at a time for improved performance.
pub const fn generate_table_u64(width: u8, poly: u64, reflect: bool) -> [[u64; 256]; 16] {
    let poly = if reflect {
        let poly = poly.reverse_bits();
        poly >> (64u8 - width)
    } else {
        poly << (64u8 - width)
    };

    let mut table = [[0u64; 256]; 16];

    // Generate first table (lane 0) directly
    let mut i = 0;
    while i < 256 {
        table[0][i] = crc64_single(poly, reflect, i as u64);
        i += 1;
    }

    // Generate subsequent lanes based on lane 0
    let mut i = 0;
    while i < 256 {
        let mut e = 1;
        while e < 16 {
            let one_lower = table[e - 1][i];
            if reflect {
                table[e][i] = (one_lower >> 8) ^ table[0][(one_lower & 0xFF) as usize];
            } else {
                table[e][i] = (one_lower << 8) ^ table[0][((one_lower >> 56) & 0xFF) as usize];
            }
            e += 1;
        }
        i += 1;
    }

    table
}

// ============================================================================
// Caching for custom CRC algorithms
// ============================================================================

#[cfg(feature = "alloc")]
#[cfg(feature = "std")]
use std::collections::HashMap;
#[cfg(feature = "alloc")]
#[cfg(feature = "std")]
use std::sync::{Mutex, OnceLock};

#[cfg(feature = "alloc")]
#[cfg(all(not(feature = "std"), feature = "cache"))]
use hashbrown::HashMap;
#[cfg(feature = "alloc")]
#[cfg(all(not(feature = "std"), feature = "cache"))]
use spin::{Mutex, Once};

// Cache key types for custom algorithms
#[cfg(feature = "alloc")]
#[cfg(any(feature = "std", feature = "cache"))]
type Crc16Key = (u16, u16, bool, bool, u16, u16);
#[cfg(feature = "alloc")]
#[cfg(any(feature = "std", feature = "cache"))]
type Crc32Key = (u32, u32, bool, bool, u32, u32);
#[cfg(feature = "alloc")]
#[cfg(any(feature = "std", feature = "cache"))]
type Crc64Key = (u64, u64, bool, bool, u64, u64);

// Cache value types for custom algorithms - stores the generated table
#[cfg(feature = "alloc")]
#[cfg(any(feature = "std", feature = "cache"))]
type Crc16CacheValue = &'static [[u16; 256]; 16];
#[cfg(feature = "alloc")]
#[cfg(any(feature = "std", feature = "cache"))]
type Crc32CacheValue = &'static [[u32; 256]; 16];
#[cfg(feature = "alloc")]
#[cfg(any(feature = "std", feature = "cache"))]
type Crc64CacheValue = &'static [[u64; 256]; 16];

// Global caches for custom algorithms (std version)
#[cfg(feature = "alloc")]
#[cfg(feature = "std")]
static CUSTOM_CRC16_CACHE: OnceLock<Mutex<HashMap<Crc16Key, Crc16CacheValue>>> = OnceLock::new();
#[cfg(feature = "alloc")]
#[cfg(feature = "std")]
static CUSTOM_CRC32_CACHE: OnceLock<Mutex<HashMap<Crc32Key, Crc32CacheValue>>> = OnceLock::new();
#[cfg(feature = "alloc")]
#[cfg(feature = "std")]
static CUSTOM_CRC64_CACHE: OnceLock<Mutex<HashMap<Crc64Key, Crc64CacheValue>>> = OnceLock::new();

// Global caches for custom algorithms (no_std + cache version)
#[cfg(feature = "alloc")]
#[cfg(all(not(feature = "std"), feature = "cache"))]
static CUSTOM_CRC16_CACHE: Once<Mutex<HashMap<Crc16Key, Crc16CacheValue>>> = Once::new();
#[cfg(feature = "alloc")]
#[cfg(all(not(feature = "std"), feature = "cache"))]
static CUSTOM_CRC32_CACHE: Once<Mutex<HashMap<Crc32Key, Crc32CacheValue>>> = Once::new();
#[cfg(feature = "alloc")]
#[cfg(all(not(feature = "std"), feature = "cache"))]
static CUSTOM_CRC64_CACHE: Once<Mutex<HashMap<Crc64Key, Crc64CacheValue>>> = Once::new();

// ============================================================================
// Main dispatch function
// ============================================================================

#[allow(unused)]
#[allow(deprecated)]
pub(crate) fn update(state: u64, data: &[u8], params: &CrcParams) -> u64 {
    match params.width {
        16 => update_crc16(state as u16, data, params) as u64,
        32 => update_crc32(state as u32, data, params) as u64,
        64 => update_crc64(state, data, params),
        _ => panic!("Unsupported CRC width: {}", params.width),
    }
}

// ============================================================================
// CRC-16 dispatch
// ============================================================================

fn update_crc16(state: u16, data: &[u8], params: &CrcParams) -> u16 {
    let (table, refin) = match params.algorithm {
        CrcAlgorithm::Crc16Arc => (&tables::crc16::CRC16_ARC_TABLE, true),
        CrcAlgorithm::Crc16Cdma2000 => (&tables::crc16::CRC16_CDMA2000_TABLE, false),
        CrcAlgorithm::Crc16Cms => (&tables::crc16::CRC16_CMS_TABLE, false),
        CrcAlgorithm::Crc16Dds110 => (&tables::crc16::CRC16_DDS_110_TABLE, false),
        CrcAlgorithm::Crc16DectR => (&tables::crc16::CRC16_DECT_R_TABLE, false),
        CrcAlgorithm::Crc16DectX => (&tables::crc16::CRC16_DECT_X_TABLE, false),
        CrcAlgorithm::Crc16Dnp => (&tables::crc16::CRC16_DNP_TABLE, true),
        CrcAlgorithm::Crc16En13757 => (&tables::crc16::CRC16_EN_13757_TABLE, false),
        CrcAlgorithm::Crc16Genibus => (&tables::crc16::CRC16_GENIBUS_TABLE, false),
        CrcAlgorithm::Crc16Gsm => (&tables::crc16::CRC16_GSM_TABLE, false),
        CrcAlgorithm::Crc16Ibm3740 => (&tables::crc16::CRC16_IBM_3740_TABLE, false),
        CrcAlgorithm::Crc16IbmSdlc => (&tables::crc16::CRC16_IBM_SDLC_TABLE, true),
        CrcAlgorithm::Crc16IsoIec144433A => (&tables::crc16::CRC16_ISO_IEC_14443_3_A_TABLE, true),
        CrcAlgorithm::Crc16Kermit => (&tables::crc16::CRC16_KERMIT_TABLE, true),
        CrcAlgorithm::Crc16Lj1200 => (&tables::crc16::CRC16_LJ1200_TABLE, false),
        CrcAlgorithm::Crc16M17 => (&tables::crc16::CRC16_M17_TABLE, false),
        CrcAlgorithm::Crc16MaximDow => (&tables::crc16::CRC16_MAXIM_DOW_TABLE, true),
        CrcAlgorithm::Crc16Mcrf4xx => (&tables::crc16::CRC16_MCRF4XX_TABLE, true),
        CrcAlgorithm::Crc16Modbus => (&tables::crc16::CRC16_MODBUS_TABLE, true),
        CrcAlgorithm::Crc16Nrsc5 => (&tables::crc16::CRC16_NRSC_5_TABLE, true),
        CrcAlgorithm::Crc16OpensafetyA => (&tables::crc16::CRC16_OPENSAFETY_A_TABLE, false),
        CrcAlgorithm::Crc16OpensafetyB => (&tables::crc16::CRC16_OPENSAFETY_B_TABLE, false),
        CrcAlgorithm::Crc16Profibus => (&tables::crc16::CRC16_PROFIBUS_TABLE, false),
        CrcAlgorithm::Crc16Riello => (&tables::crc16::CRC16_RIELLO_TABLE, true),
        CrcAlgorithm::Crc16SpiFujitsu => (&tables::crc16::CRC16_SPI_FUJITSU_TABLE, false),
        CrcAlgorithm::Crc16T10Dif => (&tables::crc16::CRC16_T10_DIF_TABLE, false),
        CrcAlgorithm::Crc16Teledisk => (&tables::crc16::CRC16_TELEDISK_TABLE, false),
        CrcAlgorithm::Crc16Tms37157 => (&tables::crc16::CRC16_TMS37157_TABLE, true),
        CrcAlgorithm::Crc16Umts => (&tables::crc16::CRC16_UMTS_TABLE, false),
        CrcAlgorithm::Crc16Usb => (&tables::crc16::CRC16_USB_TABLE, true),
        CrcAlgorithm::Crc16Xmodem => (&tables::crc16::CRC16_XMODEM_TABLE, false),
        CrcAlgorithm::CrcCustom => {
            return update_crc16_custom(state, data, params);
        }
        _ => panic!("Invalid algorithm for u16 CRC"),
    };

    native_update_u16(state, table, refin, data)
}

#[cfg(feature = "alloc")]
fn update_crc16_custom(state: u16, data: &[u8], params: &CrcParams) -> u16 {
    extern crate alloc;
    use alloc::boxed::Box;

    let refin = params.refin;

    #[cfg(any(feature = "std", feature = "cache"))]
    let table: &'static [[u16; 256]; 16] = {
        let key: Crc16Key = (
            params.poly as u16,
            params.init as u16,
            params.refin,
            params.refout,
            params.xorout as u16,
            params.check as u16,
        );

        #[cfg(feature = "std")]
        {
            let cache = CUSTOM_CRC16_CACHE.get_or_init(|| Mutex::new(HashMap::new()));
            let mut cache_guard = cache.lock().unwrap();

            cache_guard.entry(key).or_insert_with(|| {
                let table = generate_table_u16(params.width, params.poly as u16, refin);
                Box::leak(Box::new(table))
            })
        }

        #[cfg(all(not(feature = "std"), feature = "cache"))]
        {
            let cache = CUSTOM_CRC16_CACHE.call_once(|| Mutex::new(HashMap::new()));
            let mut cache_guard = cache.lock();

            cache_guard.entry(key).or_insert_with(|| {
                let table = generate_table_u16(params.width, params.poly as u16, refin);
                Box::leak(Box::new(table))
            })
        }
    };

    #[cfg(not(any(feature = "std", feature = "cache")))]
    let table: &'static [[u16; 256]; 16] = {
        let table = generate_table_u16(params.width, params.poly as u16, refin);
        Box::leak(Box::new(table))
    };

    native_update_u16(state, table, refin, data)
}

#[cfg(not(feature = "alloc"))]
fn update_crc16_custom(_state: u16, _data: &[u8], _params: &CrcParams) -> u16 {
    panic!("Custom CRC parameters require the 'alloc' feature")
}

// ============================================================================
// CRC-32 dispatch
// ============================================================================

fn update_crc32(state: u32, data: &[u8], params: &CrcParams) -> u32 {
    let (table, refin) = match params.algorithm {
        CrcAlgorithm::Crc32Aixm => (&tables::crc32::CRC32_AIXM_TABLE, false),
        CrcAlgorithm::Crc32Autosar => (&tables::crc32::CRC32_AUTOSAR_TABLE, true),
        CrcAlgorithm::Crc32Base91D => (&tables::crc32::CRC32_BASE91_D_TABLE, true),
        CrcAlgorithm::Crc32Bzip2 => (&tables::crc32::CRC32_BZIP2_TABLE, false),
        CrcAlgorithm::Crc32CdRomEdc => (&tables::crc32::CRC32_CD_ROM_EDC_TABLE, true),
        CrcAlgorithm::Crc32Cksum => (&tables::crc32::CRC32_CKSUM_TABLE, false),
        CrcAlgorithm::Crc32Iscsi => (&tables::crc32::CRC32_ISCSI_TABLE, true),
        CrcAlgorithm::Crc32IsoHdlc => (&tables::crc32::CRC32_ISO_HDLC_TABLE, true),
        CrcAlgorithm::Crc32Jamcrc => (&tables::crc32::CRC32_JAMCRC_TABLE, true),
        CrcAlgorithm::Crc32Mef => (&tables::crc32::CRC32_MEF_TABLE, true),
        CrcAlgorithm::Crc32Mpeg2 => (&tables::crc32::CRC32_MPEG_2_TABLE, false),
        CrcAlgorithm::Crc32Xfer => (&tables::crc32::CRC32_XFER_TABLE, false),
        #[allow(deprecated)]
        CrcAlgorithm::Crc32Custom | CrcAlgorithm::CrcCustom => {
            return update_crc32_custom(state, data, params);
        }
        _ => panic!("Invalid algorithm for u32 CRC"),
    };

    native_update_u32(state, table, refin, data)
}

#[cfg(feature = "alloc")]
fn update_crc32_custom(state: u32, data: &[u8], params: &CrcParams) -> u32 {
    extern crate alloc;
    use alloc::boxed::Box;

    let refin = params.refin;

    #[cfg(any(feature = "std", feature = "cache"))]
    let table: &'static [[u32; 256]; 16] = {
        let key: Crc32Key = (
            params.poly as u32,
            params.init as u32,
            params.refin,
            params.refout,
            params.xorout as u32,
            params.check as u32,
        );

        #[cfg(feature = "std")]
        {
            let cache = CUSTOM_CRC32_CACHE.get_or_init(|| Mutex::new(HashMap::new()));
            let mut cache_guard = cache.lock().unwrap();

            cache_guard.entry(key).or_insert_with(|| {
                let table = generate_table_u32(params.width, params.poly as u32, refin);
                Box::leak(Box::new(table))
            })
        }

        #[cfg(all(not(feature = "std"), feature = "cache"))]
        {
            let cache = CUSTOM_CRC32_CACHE.call_once(|| Mutex::new(HashMap::new()));
            let mut cache_guard = cache.lock();

            cache_guard.entry(key).or_insert_with(|| {
                let table = generate_table_u32(params.width, params.poly as u32, refin);
                Box::leak(Box::new(table))
            })
        }
    };

    #[cfg(not(any(feature = "std", feature = "cache")))]
    let table: &'static [[u32; 256]; 16] = {
        let table = generate_table_u32(params.width, params.poly as u32, refin);
        Box::leak(Box::new(table))
    };

    native_update_u32(state, table, refin, data)
}

#[cfg(not(feature = "alloc"))]
fn update_crc32_custom(_state: u32, _data: &[u8], _params: &CrcParams) -> u32 {
    panic!("Custom CRC parameters require the 'alloc' feature")
}

// ============================================================================
// CRC-64 dispatch
// ============================================================================

fn update_crc64(state: u64, data: &[u8], params: &CrcParams) -> u64 {
    let (table, refin) = match params.algorithm {
        CrcAlgorithm::Crc64Ecma182 => (&tables::crc64::CRC64_ECMA_182_TABLE, false),
        CrcAlgorithm::Crc64GoIso => (&tables::crc64::CRC64_GO_ISO_TABLE, true),
        CrcAlgorithm::Crc64Ms => (&tables::crc64::CRC64_MS_TABLE, true),
        CrcAlgorithm::Crc64Nvme => (&tables::crc64::CRC64_NVME_TABLE, true),
        CrcAlgorithm::Crc64Redis => (&tables::crc64::CRC64_REDIS_TABLE, true),
        CrcAlgorithm::Crc64We => (&tables::crc64::CRC64_WE_TABLE, false),
        CrcAlgorithm::Crc64Xz => (&tables::crc64::CRC64_XZ_TABLE, true),
        #[allow(deprecated)]
        CrcAlgorithm::Crc64Custom | CrcAlgorithm::CrcCustom => {
            return update_crc64_custom(state, data, params);
        }
        _ => panic!("Invalid algorithm for u64 CRC"),
    };

    native_update_u64(state, table, refin, data)
}

#[cfg(feature = "alloc")]
fn update_crc64_custom(state: u64, data: &[u8], params: &CrcParams) -> u64 {
    extern crate alloc;
    use alloc::boxed::Box;

    let refin = params.refin;

    #[cfg(any(feature = "std", feature = "cache"))]
    let table: &'static [[u64; 256]; 16] = {
        let key: Crc64Key = (
            params.poly,
            params.init,
            params.refin,
            params.refout,
            params.xorout,
            params.check,
        );

        #[cfg(feature = "std")]
        {
            let cache = CUSTOM_CRC64_CACHE.get_or_init(|| Mutex::new(HashMap::new()));
            let mut cache_guard = cache.lock().unwrap();

            cache_guard.entry(key).or_insert_with(|| {
                let table = generate_table_u64(params.width, params.poly, refin);
                Box::leak(Box::new(table))
            })
        }

        #[cfg(all(not(feature = "std"), feature = "cache"))]
        {
            let cache = CUSTOM_CRC64_CACHE.call_once(|| Mutex::new(HashMap::new()));
            let mut cache_guard = cache.lock();

            cache_guard.entry(key).or_insert_with(|| {
                let table = generate_table_u64(params.width, params.poly, refin);
                Box::leak(Box::new(table))
            })
        }
    };

    #[cfg(not(any(feature = "std", feature = "cache")))]
    let table: &'static [[u64; 256]; 16] = {
        let table = generate_table_u64(params.width, params.poly, refin);
        Box::leak(Box::new(table))
    };

    native_update_u64(state, table, refin, data)
}

#[cfg(not(feature = "alloc"))]
fn update_crc64_custom(_state: u64, _data: &[u8], _params: &CrcParams) -> u64 {
    panic!("Custom CRC parameters require the 'alloc' feature")
}

// ============================================================================
// Native CRC Update Functions (Table<16> equivalent)
// ============================================================================

/// Native CRC-16 update function using 16-lane lookup tables.
///
/// Processes 16 bytes at a time for improved performance, then handles
/// remaining bytes with single-byte lookups.
#[allow(dead_code)]
pub(crate) fn native_update_u16(
    mut crc: u16,
    table: &[[u16; 256]; 16],
    reflect: bool,
    bytes: &[u8],
) -> u16 {
    let len = bytes.len();
    let mut i = 0;

    // Process 16 bytes at a time
    while i + 16 <= len {
        if reflect {
            let current0 = bytes[i] ^ (crc as u8);
            let current1 = bytes[i + 1] ^ ((crc >> 8) as u8);

            crc = table[0][bytes[i + 15] as usize]
                ^ table[1][bytes[i + 14] as usize]
                ^ table[2][bytes[i + 13] as usize]
                ^ table[3][bytes[i + 12] as usize]
                ^ table[4][bytes[i + 11] as usize]
                ^ table[5][bytes[i + 10] as usize]
                ^ table[6][bytes[i + 9] as usize]
                ^ table[7][bytes[i + 8] as usize]
                ^ table[8][bytes[i + 7] as usize]
                ^ table[9][bytes[i + 6] as usize]
                ^ table[10][bytes[i + 5] as usize]
                ^ table[11][bytes[i + 4] as usize]
                ^ table[12][bytes[i + 3] as usize]
                ^ table[13][bytes[i + 2] as usize]
                ^ table[14][current1 as usize]
                ^ table[15][current0 as usize];
        } else {
            let current0 = bytes[i] ^ ((crc >> 8) as u8);
            let current1 = bytes[i + 1] ^ (crc as u8);

            crc = table[0][bytes[i + 15] as usize]
                ^ table[1][bytes[i + 14] as usize]
                ^ table[2][bytes[i + 13] as usize]
                ^ table[3][bytes[i + 12] as usize]
                ^ table[4][bytes[i + 11] as usize]
                ^ table[5][bytes[i + 10] as usize]
                ^ table[6][bytes[i + 9] as usize]
                ^ table[7][bytes[i + 8] as usize]
                ^ table[8][bytes[i + 7] as usize]
                ^ table[9][bytes[i + 6] as usize]
                ^ table[10][bytes[i + 5] as usize]
                ^ table[11][bytes[i + 4] as usize]
                ^ table[12][bytes[i + 3] as usize]
                ^ table[13][bytes[i + 2] as usize]
                ^ table[14][current1 as usize]
                ^ table[15][current0 as usize];
        }
        i += 16;
    }

    // Process remaining bytes one at a time
    if reflect {
        while i < len {
            let table_index = ((crc ^ bytes[i] as u16) & 0xFF) as usize;
            crc = table[0][table_index] ^ (crc >> 8);
            i += 1;
        }
    } else {
        while i < len {
            let table_index = (((crc >> 8) ^ bytes[i] as u16) & 0xFF) as usize;
            crc = table[0][table_index] ^ (crc << 8);
            i += 1;
        }
    }

    crc
}

/// Native CRC-32 update function using 16-lane lookup tables.
///
/// Processes 16 bytes at a time for improved performance, then handles
/// remaining bytes with single-byte lookups.
#[allow(dead_code)]
pub(crate) fn native_update_u32(
    mut crc: u32,
    table: &[[u32; 256]; 16],
    reflect: bool,
    bytes: &[u8],
) -> u32 {
    let len = bytes.len();
    let mut i = 0;

    // Process 16 bytes at a time
    while i + 16 <= len {
        if reflect {
            let current0 = bytes[i] ^ (crc as u8);
            let current1 = bytes[i + 1] ^ ((crc >> 8) as u8);
            let current2 = bytes[i + 2] ^ ((crc >> 16) as u8);
            let current3 = bytes[i + 3] ^ ((crc >> 24) as u8);

            crc = table[0][bytes[i + 15] as usize]
                ^ table[1][bytes[i + 14] as usize]
                ^ table[2][bytes[i + 13] as usize]
                ^ table[3][bytes[i + 12] as usize]
                ^ table[4][bytes[i + 11] as usize]
                ^ table[5][bytes[i + 10] as usize]
                ^ table[6][bytes[i + 9] as usize]
                ^ table[7][bytes[i + 8] as usize]
                ^ table[8][bytes[i + 7] as usize]
                ^ table[9][bytes[i + 6] as usize]
                ^ table[10][bytes[i + 5] as usize]
                ^ table[11][bytes[i + 4] as usize]
                ^ table[12][current3 as usize]
                ^ table[13][current2 as usize]
                ^ table[14][current1 as usize]
                ^ table[15][current0 as usize];
        } else {
            let current0 = bytes[i] ^ ((crc >> 24) as u8);
            let current1 = bytes[i + 1] ^ ((crc >> 16) as u8);
            let current2 = bytes[i + 2] ^ ((crc >> 8) as u8);
            let current3 = bytes[i + 3] ^ (crc as u8);

            crc = table[0][bytes[i + 15] as usize]
                ^ table[1][bytes[i + 14] as usize]
                ^ table[2][bytes[i + 13] as usize]
                ^ table[3][bytes[i + 12] as usize]
                ^ table[4][bytes[i + 11] as usize]
                ^ table[5][bytes[i + 10] as usize]
                ^ table[6][bytes[i + 9] as usize]
                ^ table[7][bytes[i + 8] as usize]
                ^ table[8][bytes[i + 7] as usize]
                ^ table[9][bytes[i + 6] as usize]
                ^ table[10][bytes[i + 5] as usize]
                ^ table[11][bytes[i + 4] as usize]
                ^ table[12][current3 as usize]
                ^ table[13][current2 as usize]
                ^ table[14][current1 as usize]
                ^ table[15][current0 as usize];
        }
        i += 16;
    }

    // Process remaining bytes one at a time
    if reflect {
        while i < len {
            let table_index = ((crc ^ bytes[i] as u32) & 0xFF) as usize;
            crc = table[0][table_index] ^ (crc >> 8);
            i += 1;
        }
    } else {
        while i < len {
            let table_index = (((crc >> 24) ^ bytes[i] as u32) & 0xFF) as usize;
            crc = table[0][table_index] ^ (crc << 8);
            i += 1;
        }
    }

    crc
}

/// Native CRC-64 update function using 16-lane lookup tables.
///
/// Processes 16 bytes at a time for improved performance, then handles
/// remaining bytes with single-byte lookups.
#[allow(dead_code)]
pub(crate) fn native_update_u64(
    mut crc: u64,
    table: &[[u64; 256]; 16],
    reflect: bool,
    bytes: &[u8],
) -> u64 {
    let len = bytes.len();
    let mut i = 0;

    // Process 16 bytes at a time
    while i + 16 <= len {
        if reflect {
            let current0 = bytes[i] ^ (crc as u8);
            let current1 = bytes[i + 1] ^ ((crc >> 8) as u8);
            let current2 = bytes[i + 2] ^ ((crc >> 16) as u8);
            let current3 = bytes[i + 3] ^ ((crc >> 24) as u8);
            let current4 = bytes[i + 4] ^ ((crc >> 32) as u8);
            let current5 = bytes[i + 5] ^ ((crc >> 40) as u8);
            let current6 = bytes[i + 6] ^ ((crc >> 48) as u8);
            let current7 = bytes[i + 7] ^ ((crc >> 56) as u8);

            crc = table[0][bytes[i + 15] as usize]
                ^ table[1][bytes[i + 14] as usize]
                ^ table[2][bytes[i + 13] as usize]
                ^ table[3][bytes[i + 12] as usize]
                ^ table[4][bytes[i + 11] as usize]
                ^ table[5][bytes[i + 10] as usize]
                ^ table[6][bytes[i + 9] as usize]
                ^ table[7][bytes[i + 8] as usize]
                ^ table[8][current7 as usize]
                ^ table[9][current6 as usize]
                ^ table[10][current5 as usize]
                ^ table[11][current4 as usize]
                ^ table[12][current3 as usize]
                ^ table[13][current2 as usize]
                ^ table[14][current1 as usize]
                ^ table[15][current0 as usize];
        } else {
            let current0 = bytes[i] ^ ((crc >> 56) as u8);
            let current1 = bytes[i + 1] ^ ((crc >> 48) as u8);
            let current2 = bytes[i + 2] ^ ((crc >> 40) as u8);
            let current3 = bytes[i + 3] ^ ((crc >> 32) as u8);
            let current4 = bytes[i + 4] ^ ((crc >> 24) as u8);
            let current5 = bytes[i + 5] ^ ((crc >> 16) as u8);
            let current6 = bytes[i + 6] ^ ((crc >> 8) as u8);
            let current7 = bytes[i + 7] ^ (crc as u8);

            crc = table[0][bytes[i + 15] as usize]
                ^ table[1][bytes[i + 14] as usize]
                ^ table[2][bytes[i + 13] as usize]
                ^ table[3][bytes[i + 12] as usize]
                ^ table[4][bytes[i + 11] as usize]
                ^ table[5][bytes[i + 10] as usize]
                ^ table[6][bytes[i + 9] as usize]
                ^ table[7][bytes[i + 8] as usize]
                ^ table[8][current7 as usize]
                ^ table[9][current6 as usize]
                ^ table[10][current5 as usize]
                ^ table[11][current4 as usize]
                ^ table[12][current3 as usize]
                ^ table[13][current2 as usize]
                ^ table[14][current1 as usize]
                ^ table[15][current0 as usize];
        }
        i += 16;
    }

    // Process remaining bytes one at a time
    if reflect {
        while i < len {
            let table_index = ((crc ^ bytes[i] as u64) & 0xFF) as usize;
            crc = table[0][table_index] ^ (crc >> 8);
            i += 1;
        }
    } else {
        while i < len {
            let table_index = (((crc >> 56) ^ bytes[i] as u64) & 0xFF) as usize;
            crc = table[0][table_index] ^ (crc << 8);
            i += 1;
        }
    }

    crc
}

// ============================================================================
// Property Tests for Native Implementation
// ============================================================================

#[cfg(test)]
mod property_tests {
    use crate::test::consts::{
        RUST_CRC16_ARC, RUST_CRC16_IBM_SDLC, RUST_CRC16_T10_DIF, RUST_CRC32_BZIP2,
        RUST_CRC32_ISCSI, RUST_CRC32_ISO_HDLC, RUST_CRC64_ECMA_182, RUST_CRC64_NVME, RUST_CRC64_XZ,
        TEST_CHECK_STRING,
    };
    use crate::test::miri_compatible_proptest_config;
    use crate::{checksum, checksum_with_params, CrcAlgorithm, CrcParams, Digest};
    use proptest::prelude::*;

    proptest! {
        #![proptest_config(miri_compatible_proptest_config())]

        /// Feature: remove-crc-runtime-dependency, Property 1: Native Implementation Correctness
        /// *For any* supported CRC algorithm and *for any* byte sequence, the native implementation
        /// SHALL produce the same checksum as the `crc` crate reference implementation.
        /// **Validates: Requirements 2.2, 2.3, 4.2, 4.3, 6.2, 6.5**
        #[test]
        fn prop_native_crc16_reflected_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Test CRC-16/IBM-SDLC (reflected)
            let our_result = checksum(CrcAlgorithm::Crc16IbmSdlc, &data);
            let reference_result = RUST_CRC16_IBM_SDLC.checksum(&data) as u64;
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-16/IBM-SDLC native mismatch for {} bytes: our=0x{:04X}, ref=0x{:04X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 1: Native Implementation Correctness
        /// *For any* supported CRC algorithm and *for any* byte sequence, the native implementation
        /// SHALL produce the same checksum as the `crc` crate reference implementation.
        /// **Validates: Requirements 2.2, 2.3, 4.2, 4.3, 6.2, 6.5**
        #[test]
        fn prop_native_crc16_forward_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Test CRC-16/T10-DIF (forward/non-reflected)
            let our_result = checksum(CrcAlgorithm::Crc16T10Dif, &data);
            let reference_result = RUST_CRC16_T10_DIF.checksum(&data) as u64;
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-16/T10-DIF native mismatch for {} bytes: our=0x{:04X}, ref=0x{:04X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 1: Native Implementation Correctness
        /// *For any* supported CRC algorithm and *for any* byte sequence, the native implementation
        /// SHALL produce the same checksum as the `crc` crate reference implementation.
        /// **Validates: Requirements 2.2, 2.3, 4.2, 4.3, 6.2, 6.5**
        #[test]
        fn prop_native_crc32_reflected_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Test CRC-32/ISO-HDLC (reflected)
            let our_result = checksum(CrcAlgorithm::Crc32IsoHdlc, &data);
            let reference_result = RUST_CRC32_ISO_HDLC.checksum(&data) as u64;
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-32/ISO-HDLC native mismatch for {} bytes: our=0x{:08X}, ref=0x{:08X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 1: Native Implementation Correctness
        /// *For any* supported CRC algorithm and *for any* byte sequence, the native implementation
        /// SHALL produce the same checksum as the `crc` crate reference implementation.
        /// **Validates: Requirements 2.2, 2.3, 4.2, 4.3, 6.2, 6.5**
        #[test]
        fn prop_native_crc32_forward_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Test CRC-32/BZIP2 (forward/non-reflected)
            let our_result = checksum(CrcAlgorithm::Crc32Bzip2, &data);
            let reference_result = RUST_CRC32_BZIP2.checksum(&data) as u64;
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-32/BZIP2 native mismatch for {} bytes: our=0x{:08X}, ref=0x{:08X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 1: Native Implementation Correctness
        /// *For any* supported CRC algorithm and *for any* byte sequence, the native implementation
        /// SHALL produce the same checksum as the `crc` crate reference implementation.
        /// **Validates: Requirements 2.2, 2.3, 4.2, 4.3, 6.2, 6.5**
        #[test]
        fn prop_native_crc64_reflected_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Test CRC-64/XZ (reflected)
            let our_result = checksum(CrcAlgorithm::Crc64Xz, &data);
            let reference_result = RUST_CRC64_XZ.checksum(&data);
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-64/XZ native mismatch for {} bytes: our=0x{:016X}, ref=0x{:016X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 1: Native Implementation Correctness
        /// *For any* supported CRC algorithm and *for any* byte sequence, the native implementation
        /// SHALL produce the same checksum as the `crc` crate reference implementation.
        /// **Validates: Requirements 2.2, 2.3, 4.2, 4.3, 6.2, 6.5**
        #[test]
        fn prop_native_crc64_forward_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Test CRC-64/ECMA-182 (forward/non-reflected)
            let our_result = checksum(CrcAlgorithm::Crc64Ecma182, &data);
            let reference_result = RUST_CRC64_ECMA_182.checksum(&data);
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-64/ECMA-182 native mismatch for {} bytes: our=0x{:016X}, ref=0x{:016X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 3: Incremental Update Equivalence
        /// *For any* CRC algorithm and *for any* way of partitioning input data into chunks,
        /// computing the CRC incrementally via `Digest::update()` calls SHALL produce the same
        /// result as computing it in a single `checksum()` call.
        /// **Validates: Requirements 6.3, 6.5**
        #[test]
        fn prop_incremental_update_crc32_equivalence(
            data in proptest::collection::vec(any::<u8>(), 0..1024),
            split_point in 0usize..=1024usize
        ) {
            let split_point = split_point.min(data.len());
            let (part1, part2) = data.split_at(split_point);

            // Compute incrementally
            let mut digest = Digest::new(CrcAlgorithm::Crc32IsoHdlc);
            digest.update(part1);
            digest.update(part2);
            let incremental_result = digest.finalize();

            // Compute in one shot
            let single_result = checksum(CrcAlgorithm::Crc32IsoHdlc, &data);

            prop_assert_eq!(
                incremental_result, single_result,
                "CRC-32/ISO-HDLC incremental mismatch: incremental=0x{:08X}, single=0x{:08X}, split_point={}",
                incremental_result, single_result, split_point
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 3: Incremental Update Equivalence
        /// *For any* CRC algorithm and *for any* way of partitioning input data into chunks,
        /// computing the CRC incrementally via `Digest::update()` calls SHALL produce the same
        /// result as computing it in a single `checksum()` call.
        /// **Validates: Requirements 6.3, 6.5**
        #[test]
        fn prop_incremental_update_crc64_equivalence(
            data in proptest::collection::vec(any::<u8>(), 0..1024),
            split_point in 0usize..=1024usize
        ) {
            let split_point = split_point.min(data.len());
            let (part1, part2) = data.split_at(split_point);

            // Compute incrementally
            let mut digest = Digest::new(CrcAlgorithm::Crc64Nvme);
            digest.update(part1);
            digest.update(part2);
            let incremental_result = digest.finalize();

            // Compute in one shot
            let single_result = checksum(CrcAlgorithm::Crc64Nvme, &data);

            prop_assert_eq!(
                incremental_result, single_result,
                "CRC-64/NVME incremental mismatch: incremental=0x{:016X}, single=0x{:016X}, split_point={}",
                incremental_result, single_result, split_point
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 4: Custom Parameters Correctness
        /// *For any* valid custom CRC parameters (width 16, 32, or 64) and *for any* byte sequence,
        /// the native implementation SHALL produce the same checksum as the `crc` crate when
        /// configured with equivalent parameters.
        /// **Validates: Requirements 5.1, 6.4**
        #[test]
        fn prop_custom_params_crc16_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Custom CRC-16 parameters equivalent to CRC-16/ARC
            let custom_params = CrcParams::new(
                "CRC-16/CUSTOM-ARC",
                16,
                0x8005,  // poly
                0x0000,  // init
                true,    // refin
                0x0000,  // xorout
                0xBB3D,  // check
            );
            let our_result = checksum_with_params(custom_params, &data);
            let reference_result = RUST_CRC16_ARC.checksum(&data) as u64;
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-16 custom params mismatch for {} bytes: our=0x{:04X}, ref=0x{:04X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 4: Custom Parameters Correctness
        /// *For any* valid custom CRC parameters (width 16, 32, or 64) and *for any* byte sequence,
        /// the native implementation SHALL produce the same checksum as the `crc` crate when
        /// configured with equivalent parameters.
        /// **Validates: Requirements 5.1, 6.4**
        #[test]
        fn prop_custom_params_crc32_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Custom CRC-32 parameters equivalent to CRC-32/ISCSI
            let custom_params = CrcParams::new(
                "CRC-32/CUSTOM-ISCSI",
                32,
                0x1EDC6F41,  // poly
                0xFFFFFFFF,  // init
                true,        // refin
                0xFFFFFFFF,  // xorout
                0xE3069283,  // check
            );
            let our_result = checksum_with_params(custom_params, &data);
            let reference_result = RUST_CRC32_ISCSI.checksum(&data) as u64;
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-32 custom params mismatch for {} bytes: our=0x{:08X}, ref=0x{:08X}",
                data.len(), our_result, reference_result
            );
        }

        /// Feature: remove-crc-runtime-dependency, Property 4: Custom Parameters Correctness
        /// *For any* valid custom CRC parameters (width 16, 32, or 64) and *for any* byte sequence,
        /// the native implementation SHALL produce the same checksum as the `crc` crate when
        /// configured with equivalent parameters.
        /// **Validates: Requirements 5.1, 6.4**
        #[test]
        fn prop_custom_params_crc64_matches_reference(data in proptest::collection::vec(any::<u8>(), 0..1024)) {
            // Custom CRC-64 parameters equivalent to CRC-64/NVME
            let custom_params = CrcParams::new(
                "CRC-64/CUSTOM-NVME",
                64,
                0xAD93D23594C93659,  // poly
                0xFFFFFFFFFFFFFFFF,  // init
                true,                 // refin
                0xFFFFFFFFFFFFFFFF,  // xorout
                0xAE8B14860A799888,  // check
            );
            let our_result = checksum_with_params(custom_params, &data);
            let reference_result = RUST_CRC64_NVME.checksum(&data);
            prop_assert_eq!(
                our_result, reference_result,
                "CRC-64 custom params mismatch for {} bytes: our=0x{:016X}, ref=0x{:016X}",
                data.len(), our_result, reference_result
            );
        }
    }

    /// Feature: remove-crc-runtime-dependency, Property 2: Check Value Verification
    /// *For any* known CRC algorithm constant, computing the CRC of the byte sequence
    /// `b"123456789"` SHALL produce the documented check value from the CRC catalogue specification.
    /// **Validates: Requirements 3.2, 3.3, 3.4, 3.5**
    #[test]
    fn test_check_values_crc16() {
        use crate::crc16::consts::*;

        let test_cases: &[(&str, CrcParams, u64)] = &[
            ("CRC-16/ARC", CRC16_ARC, 0xBB3D),
            ("CRC-16/CDMA2000", CRC16_CDMA2000, 0x4C06),
            ("CRC-16/CMS", CRC16_CMS, 0xAEE7),
            ("CRC-16/DDS-110", CRC16_DDS_110, 0x9ECF),
            ("CRC-16/DECT-R", CRC16_DECT_R, 0x007E),
            ("CRC-16/DECT-X", CRC16_DECT_X, 0x007F),
            ("CRC-16/DNP", CRC16_DNP, 0xEA82),
            ("CRC-16/EN-13757", CRC16_EN_13757, 0xC2B7),
            ("CRC-16/GENIBUS", CRC16_GENIBUS, 0xD64E),
            ("CRC-16/GSM", CRC16_GSM, 0xCE3C),
            ("CRC-16/IBM-3740", CRC16_IBM_3740, 0x29B1),
            ("CRC-16/IBM-SDLC", CRC16_IBM_SDLC, 0x906E),
            ("CRC-16/ISO-IEC-14443-3-A", CRC16_ISO_IEC_14443_3_A, 0xBF05),
            ("CRC-16/KERMIT", CRC16_KERMIT, 0x2189),
            ("CRC-16/LJ1200", CRC16_LJ1200, 0xBDF4),
            ("CRC-16/M17", CRC16_M17, 0x772B),
            ("CRC-16/MAXIM-DOW", CRC16_MAXIM_DOW, 0x44C2),
            ("CRC-16/MCRF4XX", CRC16_MCRF4XX, 0x6F91),
            ("CRC-16/MODBUS", CRC16_MODBUS, 0x4B37),
            ("CRC-16/NRSC-5", CRC16_NRSC_5, 0xA066),
            ("CRC-16/OPENSAFETY-A", CRC16_OPENSAFETY_A, 0x5D38),
            ("CRC-16/OPENSAFETY-B", CRC16_OPENSAFETY_B, 0x20FE),
            ("CRC-16/PROFIBUS", CRC16_PROFIBUS, 0xA819),
            ("CRC-16/RIELLO", CRC16_RIELLO, 0x63D0),
            ("CRC-16/SPI-FUJITSU", CRC16_SPI_FUJITSU, 0xE5CC),
            ("CRC-16/T10-DIF", CRC16_T10_DIF, 0xD0DB),
            ("CRC-16/TELEDISK", CRC16_TELEDISK, 0x0FB3),
            ("CRC-16/TMS37157", CRC16_TMS37157, 0x26B1),
            ("CRC-16/UMTS", CRC16_UMTS, 0xFEE8),
            ("CRC-16/USB", CRC16_USB, 0xB4C8),
            ("CRC-16/XMODEM", CRC16_XMODEM, 0x31C3),
        ];

        for (name, params, expected_check) in test_cases {
            let result = checksum_with_params(*params, TEST_CHECK_STRING);
            assert_eq!(
                result, *expected_check,
                "{} check value mismatch: got 0x{:04X}, expected 0x{:04X}",
                name, result, expected_check
            );
        }
    }

    /// Feature: remove-crc-runtime-dependency, Property 2: Check Value Verification
    /// *For any* known CRC algorithm constant, computing the CRC of the byte sequence
    /// `b"123456789"` SHALL produce the documented check value from the CRC catalogue specification.
    /// **Validates: Requirements 3.2, 3.3, 3.4, 3.5**
    #[test]
    fn test_check_values_crc32() {
        use crate::crc32::consts::*;

        let test_cases: &[(&str, CrcParams, u64)] = &[
            ("CRC-32/AIXM", CRC32_AIXM, 0x3010BF7F),
            ("CRC-32/AUTOSAR", CRC32_AUTOSAR, 0x1697D06A),
            ("CRC-32/BASE91-D", CRC32_BASE91_D, 0x87315576),
            ("CRC-32/BZIP2", CRC32_BZIP2, 0xFC891918),
            ("CRC-32/CD-ROM-EDC", CRC32_CD_ROM_EDC, 0x6EC2EDC4),
            ("CRC-32/CKSUM", CRC32_CKSUM, 0x765E7680),
            ("CRC-32/ISCSI", CRC32_ISCSI, 0xE3069283),
            ("CRC-32/ISO-HDLC", CRC32_ISO_HDLC, 0xCBF43926),
            ("CRC-32/JAMCRC", CRC32_JAMCRC, 0x340BC6D9),
            ("CRC-32/MEF", CRC32_MEF, 0xD2C22F51),
            ("CRC-32/MPEG-2", CRC32_MPEG_2, 0x0376E6E7),
            ("CRC-32/XFER", CRC32_XFER, 0xBD0BE338),
        ];

        for (name, params, expected_check) in test_cases {
            let result = checksum_with_params(*params, TEST_CHECK_STRING);
            assert_eq!(
                result, *expected_check,
                "{} check value mismatch: got 0x{:08X}, expected 0x{:08X}",
                name, result, expected_check
            );
        }
    }

    /// Feature: remove-crc-runtime-dependency, Property 2: Check Value Verification
    /// *For any* known CRC algorithm constant, computing the CRC of the byte sequence
    /// `b"123456789"` SHALL produce the documented check value from the CRC catalogue specification.
    /// **Validates: Requirements 3.2, 3.3, 3.4, 3.5**
    #[test]
    fn test_check_values_crc64() {
        use crate::crc64::consts::*;

        let test_cases: &[(&str, CrcParams, u64)] = &[
            ("CRC-64/ECMA-182", CRC64_ECMA_182, 0x6C40DF5F0B497347),
            ("CRC-64/GO-ISO", CRC64_GO_ISO, 0xB90956C775A41001),
            ("CRC-64/MS", CRC64_MS, 0x75D4B74F024ECEEA),
            ("CRC-64/NVME", CRC64_NVME, 0xAE8B14860A799888),
            ("CRC-64/REDIS", CRC64_REDIS, 0xE9C6D914C4B8D9CA),
            ("CRC-64/WE", CRC64_WE, 0x62EC59E3F1A4F00A),
            ("CRC-64/XZ", CRC64_XZ, 0x995DC9BBDF1939FA),
        ];

        for (name, params, expected_check) in test_cases {
            let result = checksum_with_params(*params, TEST_CHECK_STRING);
            assert_eq!(
                result, *expected_check,
                "{} check value mismatch: got 0x{:016X}, expected 0x{:016X}",
                name, result, expected_check
            );
        }
    }
}
