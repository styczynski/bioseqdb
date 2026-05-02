use bio::io::fastq::{Reader, Record};
use std::io::stdin;
use std::sync::mpsc::sync_channel;
use std::thread::Builder;

extern crate bio;

fn main() {
    let reader = Reader::new(stdin());
    let (tx, rx) = sync_channel::<Vec<Record>>(10);

    let handle = Builder::new()
        .name("worker".to_string())
        .spawn(move || {
            let mut count: usize = 0;
            while let Ok(val) = rx.recv() {
                count += val.len();
            }
            count
        })
        .unwrap();

    //let mut record = Record::new();
    //while let Ok(_) = reader.read(&mut record) {
    //    if record.is_empty() { break };
    //    tx.send(record.clone()).unwrap();
    //}
    let mut record_buffer = vec![];
    for record in reader.records() {
        record_buffer.push(record.unwrap());
        if record_buffer.len() == 1000 {
            tx.send(record_buffer).unwrap();
            record_buffer = Vec::with_capacity(1000);
        }
    }
    tx.send(record_buffer).unwrap();
    ::std::mem::drop(tx);
    println!("{}", handle.join().unwrap());
}
