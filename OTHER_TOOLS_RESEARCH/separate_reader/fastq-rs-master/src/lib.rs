//! A fast parser for fastq.
//!
//! The records in a file can be accessed through three different interfaces:
//!
//! - `Parser::each`: This function takes a closure that is executed for each
//!   fastq record. It is the fastest way to iterate over the records, since
//!   no copying of the records is needed and we don't allocate anything during
//!   parsing.
//! - `Parser::record_sets`. This function returns an iterator over record sets.
//!   All records in a record set share the same data array, so we only need
//!   one allocation per record set.
//! - `Parser::parallel_each`. This is a convenience function that wraps
//!   `Parser::record_sets` but passes the record sets to a number of
//!   background threads. A closure is executed on each thread with an iterator
//!   over record sets. Results from the threads are collected and returned
//!   to the caller.
//!
//! Since fastq file are usually compressed, this crate also includes a function
//! `thread_reader` to offload the decompression to a different core, and
//! `from_path` to guess the compression.
//!
//! # The FastQ standard
//!
//! This library supports Windows and Unix line endings, it does not support
//! the old MAC line ending `\r`. It allows arbitrary data on the separator
//! line between sequence and quality as long as it starts with a `+` (some
//! fastq files repeat the id on this line). It does not validate that the
//! sequence or the quality contain only allowed characters. Sequence and
//! quality must have the same length. They are not allowed to contain
//! newline characters.
//!
//! At the moment it does not make any effort to pair reads. This means that
//! pairs that belong together might end up on different cores in a
//! multithreaded setup. (TODO This should change it the future!).
//!
//! # Examples
//!
//! A minimal program that reads uncompressed fastq from stdin and counts the
//! number of fastq records. Since we do not need ownership of the records we
//! can use the fastest `Parser::each`.
//!
//! ```rust,no_run
//! use std::io::stdin;
//! use fastq::Parser;
//!
//! let mut parser = Parser::new(stdin());
//! let mut total: usize = 0;
//! parser.each(|_| {
//!     total += 1;
//!     // return `true` if you want to continue iterating
//!     true
//! }).expect("Invalid fastq file");
//! println!("{}", total);
//! ```
//!
//! If we want to do more than just count the number of records (in this
//! example, count how many sequences align to an illumina adapter with a score
//! better than 10), we probably want to use more cores:
//!
//! ```rust,no_run
//! use fastq::{parse_path, Record};
//! use std::env::args;
//! use parasailors as align;
//!
//! extern crate fastq;
//! extern crate parasailors;
//!
//! fn main() {
//!     let filename = args().nth(1);
//!     // Treat "-" as stdin
//!     let path = match filename.as_ref().map(String::as_ref) {
//!         None | Some("-") => { None },
//!         Some(name) => Some(name)
//!     };
//!
//!     parse_path(path, |parser| {
//!         let nthreads = 4;
//!         let results: Vec<usize> = parser.parallel_each(nthreads, |record_sets| {
//!             // we can initialize thread local variables here.
//!             let adapter = b"AATGATACGGCGACCACCGAGATCTACACTCTTTCCCTACACGACGCTCTTCCGATCT";
//!             let matrix = align::Matrix::new(align::MatrixType::Identity);
//!             let profile = align::Profile::new(adapter, &matrix);
//!             let mut thread_total = 0;
//!
//!             for record_set in record_sets {
//!                 for record in record_set.iter() {
//!                     let score = align::local_alignment_score(&profile, record.seq(), 5, 1);
//!                     if score > 8 {
//!                         thread_total += 1;
//!                     }
//!                 }
//!             }
//!
//!             // The values we return (it can be any type implementing `Send`)
//!             // are collected from the different threads by
//!             // `parser.parallel_each` and returned. See doc for a description of
//!             // error handling.
//!             thread_total
//!         }).expect("Invalid fastq file");
//!         println!("{}", results.iter().sum::<usize>());
//!     }).expect("Invalid compression");
//! }
//! ```
//!
//! On my feeble 2 core laptop this ends up being bound by the alignment at ~300MB/s,
//! but it should scale well to a larger number of cores.

use std::io::{Error, ErrorKind, Read, Result};
use std::iter::FromIterator;
use std::path::Path;
use std::sync::mpsc::{sync_channel, SyncSender};
use std::sync::Arc;
use std::thread;

extern crate memchr;
extern crate niffler;

mod buffer;
mod records;
mod thread_reader;

use records::{IdxRecord, IdxRecordResult};
pub use records::{OwnedRecord, Record, RefRecord};
pub use thread_reader::thread_reader;

#[cfg(fuzzing)]
const BUFSIZE: usize = 64;
#[cfg(not(fuzzing))]
const BUFSIZE: usize = 68 * 1024;

/// Parser for fastq files.
pub struct Parser<R: Read> {
    reader: R,
    buffer: buffer::Buffer,
}

/// Create a parser and guess the compression.
///
/// At the moment this supports gzip, lz4 and plain fastq files.
/// If the path is None, read from stdin.
///
/// # Examples
///
/// ```rust,no_run
/// use fastq::{parse_path, Record};
/// use std::env::args;
///
/// let filename = args().nth(1);
/// // Accept "-" as stdin
/// let path = match filename.as_ref().map(String::as_ref) {
///     None | Some("-") => { None },
///     Some(name) => Some(name)
/// };
///
/// parse_path(path, |mut parser| {
///     let stopped = parser.each(|record| {
///         // stop parsing if we find a sequnce containing 'N'
///         record.validate_dnan()
///     }).expect("Invalid fastq file");
///     if stopped {
///         println!("The file contains only sequences with ACTGN");
///     } else {
///         println!("The file contains invalid sequences");
///     }
/// }).expect("Invalid compression");
/// ```
pub fn parse_path<P, F, O>(path: Option<P>, func: F) -> Result<O>
where
    P: AsRef<Path>,
    F: FnOnce(Parser<&mut dyn Read>) -> O,
{
    let (mut reader, compression_format) = match path {
        None => niffler::send::get_reader(Box::new(std::io::stdin())).map_err(|e| {
            Error::new(
                ErrorKind::InvalidData,
                format!("Niffler failled in compression detection {}", e),
            )
        })?,
        Some(path) => {
            niffler::send::get_reader(Box::new(std::fs::File::open(path)?)).map_err(|e| {
                Error::new(
                    ErrorKind::InvalidData,
                    format!("Niffler failled in compression detection {}", e),
                )
            })?
        }
    };

    match compression_format {
        niffler::send::compression::Format::No => Ok(func(Parser::new(&mut reader))),
        _ => Ok(thread_reader(1 << 22, 2, reader, |mut reader| {
            func(Parser::new(&mut reader))
        })
        .expect("thread reader paniced")),
    }
}

impl<'a, R: 'a + Read> Parser<R> {
    /// Create a new fastq parser.
    pub fn new(reader: R) -> Parser<R> {
        Parser {
            reader: reader,
            buffer: buffer::Buffer::new(BUFSIZE),
        }
    }

    /// Get a RecordRefIter for this parser. Should only be used in custom iteration scenarios.
    pub fn ref_iter(self) -> RecordRefIter<R> {
        RecordRefIter {
            parser: self,
            current_length: None,
            current: None,
        }
    }

    /// Apply a function to each fastq record in an file.
    ///
    /// Stop the parser if the closure returns `false`.
    /// Return `true`, if the parser reached the end of the file.
    #[inline]
    pub fn each<F>(self, mut func: F) -> Result<bool>
    where
        F: FnMut(RefRecord) -> bool,
    {
        let mut iter = self.ref_iter();
        loop {
            iter.advance()?;
            match iter.get() {
                None => return Ok(true),
                Some(record) => {
                    let go_on = func(record);
                    if !go_on {
                        return Ok(false);
                    }
                }
            }
        }
    }
}

pub struct RecordRefIter<R: Read> {
    parser: Parser<R>,
    current: Option<IdxRecord>,
    current_length: Option<usize>,
}

impl<R: Read> RecordRefIter<R> {
    pub fn get(&self) -> Option<RefRecord> {
        match self.current {
            None => None,
            Some(ref rec) => Some(rec.to_ref_record(self.parser.buffer.data())),
        }
    }

    pub fn advance(&mut self) -> Result<()> {
        let buffer = &mut self.parser.buffer;
        let reader = &mut self.parser.reader;
        if let Some(len) = self.current_length.take() {
            buffer.consume(len);
        }
        loop {
            match IdxRecord::from_buffer(buffer.data()) {
                Err(e) => return Err(e),
                Ok(IdxRecordResult::EmptyBuffer) => {
                    buffer.clean();
                    match buffer.read_into(reader) {
                        Err(e) => return Err(e),
                        Ok(0) => {
                            self.current = None;
                            self.current_length = None;
                            return Ok(());
                        }
                        _ => continue,
                    }
                }
                Ok(IdxRecordResult::Incomplete) => {
                    buffer.clean();
                    if buffer.n_free() == 0 {
                        return Err(Error::new(
                            ErrorKind::InvalidData,
                            "Fastq record is too long",
                        ));
                    }
                    match buffer.read_into(reader) {
                        Err(e) => return Err(e),
                        Ok(0) => {
                            return Err(Error::new(
                                ErrorKind::InvalidData,
                                "Possibly truncated input file",
                            ));
                        }
                        _ => continue,
                    }
                }
                Ok(IdxRecordResult::Record(record)) => {
                    let length = record.data.1.checked_sub(record.data.0).unwrap();
                    self.current = Some(record);
                    self.current_length = Some(length);
                    return Ok(());
                }
            }
        }
    }
}

/// A collection of fastq records used to iterate over records in chunks.
#[derive(Debug)]
pub struct RecordSet {
    buffer: Box<[u8]>,
    records: Vec<IdxRecord>,
}

impl RecordSet {
    fn from_records(buffer: Box<[u8]>, records: Vec<IdxRecord>) -> RecordSet {
        RecordSet {
            buffer: buffer,
            records: records,
        }
    }

    /// Return an iterator over all fastq records in this record set.
    pub fn iter<'a>(&'a self) -> RecordSetItems<'a> {
        RecordSetItems {
            idx_records: self.records.iter(),
            buffer: &self.buffer,
        }
    }

    pub fn len(&self) -> usize {
        self.records.len()
    }

    pub fn is_empty(&self) -> bool {
        self.records.is_empty()
    }
}

pub struct RecordSetItems<'a> {
    idx_records: ::std::slice::Iter<'a, IdxRecord>,
    buffer: &'a [u8],
}

impl<'a> Iterator for RecordSetItems<'a> {
    type Item = RefRecord<'a>;

    #[inline]
    fn next(&mut self) -> Option<RefRecord<'a>> {
        match self.idx_records.next() {
            Some(idx_record) => Some(idx_record.to_ref_record(self.buffer)),
            None => None,
        }
    }
}

struct RecordSetIter<R: Read> {
    parser: Parser<R>,
    num_records_guess: usize,
    reader_at_end: bool,
}

impl<R: Read> Iterator for RecordSetIter<R> {
    type Item = Result<RecordSet>;

    fn next(&mut self) -> Option<Result<RecordSet>> {
        if self.reader_at_end {
            return None;
        }

        let mut records: Vec<IdxRecord> = Vec::with_capacity(self.num_records_guess);

        loop {
            let parse_result = {
                match IdxRecord::from_buffer(self.parser.buffer.data()) {
                    Ok(val) => val,
                    Err(e) => return Some(Err(e)),
                }
            };
            let buffer_pos = self.parser.buffer.pos();
            use IdxRecordResult::*;
            match parse_result {
                EmptyBuffer => {
                    self.num_records_guess = records.len() + 1;

                    let buffer = vec![0u8; BUFSIZE].into_boxed_slice();
                    let buffer = self.parser.buffer.replace_buffer(buffer);
                    match self.parser.buffer.read_into(&mut self.parser.reader) {
                        Err(e) => return Some(Err(e)),
                        Ok(0) => self.reader_at_end = true,
                        _ => {}
                    }
                    return Some(Ok(RecordSet::from_records(buffer, records)));
                }
                Incomplete => {
                    self.num_records_guess = records.len() + 1;

                    let buffer = vec![0u8; BUFSIZE].into_boxed_slice();
                    let buffer = self.parser.buffer.replace_buffer(buffer);
                    if self.parser.buffer.n_free() == 0 {
                        return Some(Err(Error::new(
                            ErrorKind::InvalidData,
                            "Fastq record is too long.",
                        )));
                    }
                    match self.parser.buffer.read_into(&mut self.parser.reader) {
                        Err(e) => return Some(Err(e)),
                        Ok(0) => {
                            return Some(Err(Error::new(
                                ErrorKind::InvalidData,
                                "Truncated input file.",
                            )))
                        }
                        _ => {}
                    }
                    return Some(Ok(RecordSet::from_records(buffer, records)));
                }
                Record(mut record) => {
                    record.data.0 += buffer_pos;
                    record.data.1 += buffer_pos;
                    let (start, end) = (record.data.0, record.data.1);
                    records.push(record);
                    self.parser.buffer.consume(end - start);
                }
            }
        }
    }
}

impl<R: Read> Parser<R> {
    /// Return the fastq records in chunks.
    fn record_sets(self) -> RecordSetIter<R> {
        RecordSetIter {
            parser: self,
            reader_at_end: false,
            num_records_guess: 100,
        }
    }

    /// Apply a function to each record, but distribute the work on a number of threads.
    ///
    /// # Parameters
    ///
    /// - `n_threads`: The number of worker threads to start.
    /// - `func`: A closure that is executed on each new thread and takes an
    ///    Iterator of RecordSets as argument.
    ///
    /// This function parses the fastq file and passes record sets to the worker threads.
    ///
    /// It terminates if one of the following happes:
    ///
    /// - The parser exhauts the fastq file. This function waits for all worker
    ///   threads to terminate (their iterator will not yield new values after
    ///   they finish buffered ones). It collects their return values and returns them.
    /// - The parser finds a syntax error. The iterators in the worker threads stop
    ///   yielding new record sets. (The worker threads are not notified of the error).
    ///   The function waits for them to terminate, discards their return values and
    ///   returns an IO error with error kind `std::error::ErrorKind::InvalidData`.
    /// - The underlying Reader yields an io error other an `Interrupted`. The behaviour
    ///   is the same as the previous, but it returns the error value of the inner reader.
    /// - A worker thread terminates before its iterator is exhausted. The parser stops,
    ///   waits for all workers to exit and returns the collected return values. If the
    ///   caller wants to know whether we parsed the whole file, this information must
    ///   be encoded in the return values of the worker threads.
    /// - A worker panics. The parser stops and this function panics.
    ///
    /// # Panics
    ///
    /// Panics, if one of the worker threads panics.
    ///
    /// # Examples
    ///
    /// Parse a fastq file and print a sequence starting with ATTAATTA if the file
    /// contains one (it might not be the first one):
    ///
    /// ```rust
    /// use std::io::{Result, ErrorKind, Cursor};
    /// use fastq::{Parser, Record};
    ///
    /// let reader = Cursor::new(b"@hi\nATTAATTAATTA\n+\n++++++++++++\n");
    /// let parser = Parser::new(reader);
    /// let result: Result<Vec<_>> = parser.parallel_each(4, |record_sets| {
    ///     for record_set in record_sets {
    ///         for record in record_set.iter() {
    ///             if record.seq().starts_with(b"ATTAATTA") {
    ///                 // Early return stops the parser
    ///                 return Some(record.seq().to_vec());
    ///             }
    ///         }
    ///     };
    ///     None
    /// });
    /// match result {
    ///     Ok(res) => {
    ///         match res.iter().filter(|x| x.is_some()).next() {
    ///             None => { assert!(false) } // nothing found
    ///             Some(seq) => {
    ///                 // Yay! we found it.
    ///                 assert_eq!(seq.as_ref().unwrap(), b"ATTAATTAATTA")
    ///             }
    ///         }
    ///     },
    ///     Err(e) => {
    ///         if e.kind() == ErrorKind::InvalidData {
    ///             assert!(false);  // this is not a valid fastq file.
    ///         } else {
    ///             assert!(false);  // some other io error.
    ///         }
    ///     }
    /// }
    pub fn parallel_each<O, S, F>(self, n_threads: usize, func: F) -> Result<S>
    where
        S: FromIterator<O>,
        O: Send + 'static,
        F: Send + Sync + 'static,
        F: Fn(Box<dyn Iterator<Item = RecordSet>>) -> O,
    {
        let mut senders: Vec<SyncSender<_>> = vec![];
        let mut threads: Vec<thread::JoinHandle<_>> = vec![];

        let func = Arc::new(func);

        for i in 0..n_threads {
            let (tx, rx): (SyncSender<RecordSet>, _) = sync_channel(10);
            let func = func.clone();

            let thread = thread::Builder::new()
                .name(format!("worker-{}", i))
                .spawn(move || func(Box::new(rx.into_iter())))
                .expect("Could not start worker threads");

            senders.push(tx);
            threads.push(thread);
        }

        let mut io_error = None;
        for (record_set, sender) in self.record_sets().zip(senders.iter().cycle()) {
            match record_set {
                Ok(records) => {
                    // We ignore send errors. If the caller wants to know if we exhausted
                    // the file they have to return that information in the worker thread.
                    if let Err(_) = sender.send(records) {
                        break;
                    }
                }
                Err(e) => {
                    io_error = Some(e);
                    break;
                }
            }
        }
        // Make iterators in the workers should stop yielding values
        ::std::mem::drop(senders);

        let results = threads
            .into_iter()
            .map(|thread| thread.join())
            .collect::<Vec<_>>()
            .into_iter()
            .map(|res| res.expect("Panic in worker thread"))
            .collect();

        match io_error {
            Some(e) => Err(e),
            None => Ok(results),
        }
    }
}

/// Step through two fastq files and call a callback for pairs of Records.
///
/// The callback returns a tuple of bools that indicate for each parser
/// if it should advance the iterator. This can be used to deal with
/// single reads that are interleaved in paired sequences and would otherwise
/// lead to misaligned pairs. Iteration ends if either both advance flags
/// are `false` or if both iterators are exhausted.
///
/// The returned bool tuple indicates which parsers were exhausetd.
pub fn each_zipped<R1, R2, F>(
    parser1: Parser<R1>,
    parser2: Parser<R2>,
    mut callback: F,
) -> Result<(bool, bool)>
where
    R1: Read,
    R2: Read,
    F: FnMut(Option<RefRecord>, Option<RefRecord>) -> (bool, bool),
{
    let mut iter1 = parser1.ref_iter();
    let mut iter2 = parser2.ref_iter();
    let mut finished = (false, false);
    iter1.advance()?;
    iter2.advance()?;
    loop {
        let advance_flags = {
            let val1 = if finished.0 { None } else { iter1.get() };
            let val2 = if finished.1 { None } else { iter2.get() };
            finished = (val1.is_none(), val2.is_none());
            callback(val1, val2)
        };
        if advance_flags == (false, false) || finished == (true, true) {
            return Ok(finished);
        }
        if advance_flags.0 && !finished.0 {
            iter1.advance()?;
        }
        if advance_flags.1 && !finished.1 {
            iter2.advance()?;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{Parser, Record};
    use std::io::{Cursor, ErrorKind, Seek, SeekFrom, Write};

    #[test]
    fn correct() {
        let data = Cursor::new(b"@hi\nNN\n+\n++\n@hallo\nTCC\n+\nabc\n");
        let parser = Parser::new(data);
        let mut i: u64 = 0;
        let ok = parser.each(move |record| {
            if i == 0 {
                assert_eq!(record.head(), b"hi");
                assert_eq!(record.seq(), b"NN");
                assert_eq!(record.qual(), b"++");

                let record = record.to_owned_record();
                assert_eq!(record.head(), b"hi");
                assert_eq!(record.seq(), b"NN");
                assert_eq!(record.qual(), b"++");
                let mut out = Cursor::new(Vec::new());
                assert_eq!(record.write(&mut out).unwrap(), 12);
                assert_eq!(&out.into_inner()[..], b"@hi\nNN\n+\n++\n");
            } else {
                assert_eq!(record.head(), b"hallo");
                assert_eq!(record.seq(), b"TCC");
                assert_eq!(record.qual(), b"abc");

                let record = record.to_owned_record();
                assert_eq!(record.head(), b"hallo");
                assert_eq!(record.seq(), b"TCC");
                assert_eq!(record.qual(), b"abc");
                let mut out = Cursor::new(Vec::new());
                assert_eq!(record.write(&mut out).unwrap(), 17);
                assert_eq!(&out.into_inner()[..], b"@hallo\nTCC\n+\nabc\n");
            }
            assert!(i < 2);
            i += 1;
            true
        });
        ok.unwrap();
    }

    #[test]
    fn empty_id() {
        let data = Cursor::new(b"@\nNN\n+\n++\n");
        let parser = Parser::new(data);
        parser
            .each(|record| {
                assert_eq!(record.head(), b"");
                assert_eq!(record.seq(), b"NN");
                assert_eq!(record.qual(), b"++");
                true
            })
            .unwrap();
    }

    #[test]
    fn missing_lines() {
        let data = Cursor::new(b"@hi\nNN\n+\n++\n@hi\nNN");
        let parser = Parser::new(data);
        let ok = parser.each(|record| {
            assert_eq!(record.head(), b"hi");
            assert_eq!(record.seq(), b"NN");
            assert_eq!(record.qual(), b"++");
            true
        });
        match ok {
            Err(e) => {
                assert!(e.kind() == ErrorKind::InvalidData)
            }
            Ok(_) => {
                panic!("should fail")
            }
        }
    }

    #[test]
    fn truncated() {
        let data = Cursor::new(b"@hi\nNN\n+\n++");
        let parser = Parser::new(data);
        let ok = parser.each(|_| {
            assert!(false);
            true
        });
        assert!(ok.is_err());
    }

    #[test]
    fn second_idline() {
        let data = Cursor::new(b"@hi\nNN\n+hi\n++\n@hi\nNN\n+hi\n++\n");
        let parser = Parser::new(data);
        let ok = parser.each(|record| {
            assert_eq!(record.head(), b"hi");
            assert_eq!(record.seq(), b"NN");
            assert_eq!(record.qual(), b"++");

            let mut out = Cursor::new(Vec::new());
            assert_eq!(record.write(&mut out).unwrap(), 14);
            assert_eq!(&out.into_inner()[..], b"@hi\nNN\n+hi\n++\n");
            true
        });
        ok.unwrap();
    }

    #[test]
    fn windows_lineend() {
        let data = Cursor::new(b"@hi\r\nNN\r\n+\r\n++\r\n@hi\r\nNN\r\n+\r\n++\r\n");
        let parser = Parser::new(data);
        let ok = parser.each(|record| {
            assert_eq!(record.head(), b"hi");
            assert_eq!(record.seq(), b"NN");
            assert_eq!(record.qual(), b"++");
            true
        });
        ok.unwrap();
    }

    #[test]
    fn length_mismatch() {
        let data = Cursor::new(b"@hi\nNN\n+\n+\n");
        let parser = Parser::new(data);
        let ok = parser.each(|_| {
            assert!(false);
            true
        });
        assert!(ok.is_err());
    }

    #[test]
    fn huge_incomplete() {
        let mut data: Cursor<Vec<u8>> = Cursor::new(vec![]);
        data.write(b"@").unwrap();
        for _ in 0..super::BUFSIZE {
            data.write(b"longid").unwrap();
        }
        data.seek(SeekFrom::Start(0)).unwrap();
        let parser = Parser::new(data);
        assert!(parser.each(|_| true).is_err());
    }

    #[test]
    fn bufflen() {
        let mut data: Cursor<Vec<u8>> = Cursor::new(vec![]);
        data.write(b"@").unwrap();
        for _ in 0..(super::BUFSIZE - 8) {
            data.write(b"a").unwrap();
        }
        data.write(b"\nA\n+\nB\n").unwrap();
        data.seek(SeekFrom::Start(0)).unwrap();
        let parser = Parser::new(data);
        let vals: Vec<u64> = parser
            .parallel_each(2, |sets| {
                let mut count = 0;
                for set in sets {
                    for _ in set.iter() {
                        count += 1;
                    }
                }
                count
            })
            .unwrap();
        assert!(vals.iter().sum::<u64>() == 1);
    }

    #[test]
    fn refset() {
        let data = Cursor::new(b"@hi\nNN\n+\n++\n@hi\nNN\n+\n++\n");
        let parser = Parser::new(data);
        let mut count: usize = 0;
        for set in parser.record_sets() {
            let set = set.unwrap();
            for record in set.iter() {
                count += 1;
                assert_eq!(record.head(), b"hi");
                assert_eq!(record.seq(), b"NN");
                assert_eq!(record.qual(), b"++");
            }
        }
        assert_eq!(count, 2)
    }

    #[test]
    fn refset_incomplete() {
        let data = Cursor::new(b"@hi\nNN\n+\n++\n@hi\nNN\n+\n++");
        let parser = Parser::new(data);
        assert!(parser.record_sets().any(|x| x.is_err()));
    }

    #[test]
    fn refset_huge_incomplete() {
        let mut data: Cursor<Vec<u8>> = Cursor::new(vec![]);
        data.write(b"@").unwrap();
        for _ in 0..super::BUFSIZE {
            data.write(b"longid").unwrap();
        }
        data.seek(SeekFrom::Start(0)).unwrap();
        let parser = Parser::new(data);
        assert!(parser.record_sets().any(|x| x.is_err()));
    }
}
