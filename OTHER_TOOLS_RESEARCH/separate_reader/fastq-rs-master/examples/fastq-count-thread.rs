use fastq::parse_path;
use std::env::args;

extern crate fastq;

fn main() {
    let filename = args().nth(1);
    let path = match filename.as_ref().map(String::as_ref) {
        None | Some("-") => None,
        Some(name) => Some(name),
    };

    parse_path(path, |parser| {
        let results: Vec<usize> = parser
            .parallel_each(1, |record_sets| {
                let mut thread_total = 0;
                for record_set in record_sets {
                    thread_total += record_set.len();
                }
                thread_total
            })
            .expect("Invalid fastq file");
        println!("{}", results.iter().sum::<usize>());
    })
    .expect("Invalid compression");
}
