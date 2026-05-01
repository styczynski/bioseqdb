
extern crate seq_io;
extern crate bio;

use seq_io::fasta::Record;
use seq_io::fastq::Record as FqRecord;

use std::env::args;

fn main() {
    for filename in args().skip(1) {

        if filename.ends_with(".fasta") || filename.ends_with(".fa") {
            let bio_reader = bio::io::fasta::Reader::from_file(&filename).unwrap();
            let mut reader = seq_io::fasta::Reader::from_path(&filename).unwrap();

            for bio_rec in bio_reader.records() {
                let bio_rec = bio_rec.unwrap();
                let rec = reader.next().expect(&format!("Record {:?} not found in reader", bio_rec.id())).unwrap();
                assert_eq!(bio_rec.id(), rec.id().unwrap());
                assert_eq!(bio_rec.desc(), rec.desc().map(|d| d.unwrap()));
                assert_eq!(bio_rec.seq(), rec.owned_seq().as_slice());
            }
            assert!(reader.next().is_none());

        } else if filename.ends_with(".fastq") || filename.ends_with(".fq") {
            let bio_reader = bio::io::fastq::Reader::from_file(&filename).unwrap();
            let mut reader = seq_io::fastq::Reader::from_path(&filename).unwrap();

            for bio_rec in bio_reader.records() {
                let bio_rec = bio_rec.unwrap();
                let rec = reader.next().expect(&format!("Record {:?} not found in reader", bio_rec.id())).unwrap();
                assert_eq!(bio_rec.id(), rec.id().unwrap());
                assert_eq!(bio_rec.desc(), rec.desc().map(|d| d.unwrap()));
                assert_eq!(bio_rec.seq(), rec.seq());
                assert_eq!(bio_rec.qual(), rec.qual());
            }
            assert!(reader.next().is_none());
        }
    }
}
