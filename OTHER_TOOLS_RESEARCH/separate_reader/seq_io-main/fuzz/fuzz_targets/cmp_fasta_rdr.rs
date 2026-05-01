#![no_main]
#[macro_use] extern crate libfuzzer_sys;
extern crate seq_io;
#[macro_use] extern crate matches;

use std::io::prelude::*;
use std::io;
use seq_io::fasta::{self, Record};

#[macro_use]
mod common;

fuzz_target!(|data: &[u8]| {

    // determine reader capacity (max: 65 KiB)
    if data.len() < 2 {
        return;
    }

    let mut a: [u8; 2] = Default::default();
    a.copy_from_slice(&data[..2]);
    let cap = u16::from_le_bytes(a) as usize;

    let data = &data[2..];

    // Ensure minimum capacity and only accept UTF-8 data for easier debugging
    if cap < 3 || ::std::str::from_utf8(&data).is_err() {
        return;
    }

    let mut simple_reader = SimpleReader::new(data);
    let mut reader = fasta::Reader::with_capacity(data, cap);

    let mut i = 0;
    while let Some(simple_res) = simple_reader.next() {
        i += 1;
        let res = reader.next().unwrap_or_else(|| {
            panic!(format!(
                "Result {} not returned by seq_io reader: {:?}",
                i, simple_res
            ));
        });

        let (mut simple_rec, rec) = match (simple_res, res) {
            (Ok(simple_r), Ok(r)) => (simple_r, r),
            (Err(e), Ok(_)) =>
                panic!(format!("simple reader produced error, seq_io didn't ({})", e)),
            (Ok(_), Err(e)) =>
                panic!(format!("seq_io produced error, rust-bio didn't ({})", e)),
            _ => continue,
        };

        assert_eq!(simple_rec.id.as_slice(), rec.id_bytes());
        assert_eq!(simple_rec.desc.as_ref().map(|d| d.as_slice()), rec.desc_bytes());
        assert_eq!(&simple_rec.seq, &rec.owned_seq());
    }
    if let Some(res) = reader.next() {
        panic!(format!(
            "Result {} not returned by seq_io reader: {:?}",
            i + 1, res
        ))
    }
});



#[derive(Default, Clone, Debug)]
pub struct SimpleRecord {
    id: Vec<u8>,
    desc: Option<Vec<u8>>,
    seq: Vec<u8>,
}

/// FASTA reader that should behave like seq_io::fasta::Reader, but using a much simpler
/// (and slower) implementation and reduced functionality
/// Based on the Rust-Bio FASTA reader implementation.
#[derive(Debug)]
pub struct SimpleReader<R: io::Read> {
    reader: io::BufReader<R>,
    line: Vec<u8>,
    error_has_occured: bool,
}

impl<R: io::Read> SimpleReader<R> {
    pub fn new(reader: R) -> Self {
        SimpleReader {
            reader: io::BufReader::new(reader),
            line: vec![],
            error_has_occured: false,
        }
    }
}

impl<R> SimpleReader<R>
where
    R: io::Read,
{
    fn next(&mut self) -> Option<io::Result<SimpleRecord>> {

        if self.error_has_occured {
            return None;
        }

        let mut record = SimpleRecord::default();

        if self.line.is_empty() {
            loop {
                if try_opt!(self.reader.read_until(b'\n', &mut self.line)) == 0 {
                    // End of input
                    return None;
                }
                if !common::trim_newline(&self.line).is_empty() {
                    // initial empty lines
                    break;
                }
                self.line.clear();
            }
        }

        if self.line.first() != Some(&b'>') {
            self.error_has_occured = true;
            return Some(Err(io::Error::new(
                io::ErrorKind::Other,
                "Expected > at record start.",
            )));
        }

        {
            let mut header_fields = common::trim_newline(&self.line[1..])
                .splitn(2, |&b| b == b' ');
            record.id = header_fields.next().unwrap().to_owned();
            record.desc = header_fields.next().map(|s| s.to_owned());
        }

        loop {
            self.line.clear();
            try_opt!(self.reader.read_until(b'\n', &mut self.line));
            if self.line.is_empty() || self.line.starts_with(b">") {
                break;
            }
            record.seq.extend_from_slice(common::trim_newline(&self.line));
        }

        Some(Ok(record))
    }
}
