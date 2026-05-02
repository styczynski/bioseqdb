use fastq::{each_zipped, parse_path};
use std::env::args;

extern crate fastq;

fn main() {
    let path1 = args().nth(1).expect("Need two input files.");
    let path2 = args().nth(2).expect("Need two input files.");

    let mut counts = (0u64, 0u64);

    parse_path(Some(path1), |parser1| {
        parse_path(Some(path2), |parser2| {
            each_zipped(parser1, parser2, |rec1, rec2| {
                if rec1.is_some() {
                    counts.0 += 1;
                }
                if rec2.is_some() {
                    counts.1 += 1;
                }
                (true, true)
            })
            .expect("Invalid record.");
        })
        .expect("Unknown format for file 2.");
    })
    .expect("Unknown format for file 1.");

    println!("Number of reads: ({}, {})", counts.0, counts.1);
}
