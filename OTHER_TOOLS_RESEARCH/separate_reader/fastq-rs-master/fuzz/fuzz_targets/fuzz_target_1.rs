#![no_main]

use std::io::Cursor;
use fastq::Record;

#[macro_use] extern crate libfuzzer_sys;
extern crate fastq;
extern crate criterion;


fuzz_target!(|data: &[u8]| {
    let reader = Cursor::new(data);
    let mut parser = fastq::Parser::new(reader);

    let mut sum: usize = 0;
    let _ = parser.each(|rec| {sum += rec.seq().len(); true});
    criterion::black_box(sum);
});
