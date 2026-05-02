use fastq::parse_path;
use std::env::args;

extern crate fastq;

fn main() {
    let filename = args().nth(1);
    let path = match filename.as_ref().map(String::as_ref) {
        None | Some("-") => None,
        Some(name) => Some(name),
    };

    let mut total: usize = 0;
    parse_path(path, |parser| {
        parser
            .each(|_| {
                total += 1;
                true
            })
            .expect("Invalid fastq file");
    })
    .expect("Invalid compression");
    println!("{}", total);
}
