//! This library provides an(other) attempt at high performance FASTA and FASTQ parsing and writing.
//! The FASTA parser can read and write multi-line files. The FASTQ parser supports only single
//! lines.
//!
//! By default, the parsers avoid allocations and copying as much as possible.
//! [`fasta::RefRecord`](fasta/struct.RefRecord.html) and
//! [`fastq::RefRecord`](fastq/struct.RefRecord.html) borrow from the underlying buffered
//! reader. In addition, `fasta::RefRecord` offers the
//! [`seq_lines()`](fasta/struct.RefRecord.html#method.seq_lines) method,
//! which allows iterating over individual sequence lines in a multi-line FASTA file
//! without the need to copy the data.
//!
//! By default, both parsers use a buffer of 64 KiB size. If a record with a longer
//! sequence is encountered, the buffer will automatically grow. How it grows can be
//! configured. See [below](#large-sequences) for more information.
//!
//! # More detailed documentation
//!
//! Please refer to the module docs for more information on how to use the reading and writing
//! functions, as well as information on the exact parsing behaviour:
//!
//! * [`fasta module`](fasta) and [`fasta::Reader`](fasta/struct.Reader.html)
//! * [`fastq module`](fastq) and [`fastq::Reader`](fastq/struct.Reader.html)
//!
//! # Example FASTQ parser:
//!
//! This code prints the ID string from each FASTQ record.
//!
//! ```no_run
//! use seq_io::fastq::{Reader,Record};
//!
//! let mut reader = Reader::from_path("seqs.fastq").unwrap();
//!
//! while let Some(record) = reader.next() {
//!     let record = record.expect("Error reading record");
//!     println!("{}", record.id().unwrap());
//! }
//! ```
//!
//! # Example FASTA parser calculating mean sequence length:
//!
//! The FASTA reader works just the same. One challenge with the FASTA
//! format is that the sequence can be broken into multiple lines.
//! Therefore, it is not always possible to get a slice to the whole sequence
//! without copying the data. But it is possible to use `seq_lines()`
//! for efficiently iterating over each sequence line:
//!
//! ```no_run
//! use seq_io::fasta::{Reader,Record};
//!
//! let mut reader = Reader::from_path("seqs.fasta").unwrap();
//!
//! let mut n = 0;
//! let mut sum = 0;
//! while let Some(record) = reader.next() {
//!     let record = record.expect("Error reading record");
//!     for s in record.seq_lines() {
//!         sum += s.len();
//!     }
//!     n += 1;
//! }
//! println!("mean sequence length of {} records: {:.1} bp", n, sum as f32 / n as f32);
//! ```
//! If the whole sequence is required at once, there is the
//! [`full_seq`](fasta/struct.RefRecord.html#method.full_seq),
//! which will only allocate the sequence if there are multiple lines.
//! use seq_io::fasta::{Reader,OwnedRecord};
//!
//! # Large sequences
//!
//! Due to the design of the parsers, each sequence record must fit into the underlying
//! buffer as a whole. There are different ways to deal with large sequences:
//! It is possible configure initial buffer size using `Reader::with_capacity()`.
//! However, the buffer will also automatically double its size if a record doesn't fit.
//! How it grows can be configured by applying another policy.
//!
//! For example, the readers can be configured to return
//! [`fasta::Error::BufferLimit`](fasta/enum.Error.html#variant.BufferLimit) /
//! [`fastq::Error::BufferLimit`](fastq/enum.Error.html#variant.BufferLimit)
//! if buffer size grows too large. This is done using `set_policy()`:
//!
//! ```no_run
//! use seq_io::fasta::Reader;
//! use seq_io::policy::DoubleUntilLimited;
//!
//! // The buffer doubles its size until 128 MiB, then grows by steps
//! // of 128 MiB. If it reaches 1 GiB, there will be an error.
//! let policy = DoubleUntilLimited::new(1 << 30, 1 << 32);
//! let mut reader = Reader::from_path("input.fasta").unwrap()
//!     .set_policy(policy);
//! // (...)
//! ```
//! For information on how to create a custom policy, refer to the
//! [`policy`](policy) module docs.
//!
//! # Owned records
//! Both readers also provide iterators similar to *Rust-Bio*, which return owned data. This
//! is slower, but make sense, e.g. if the records are collected in to a vector:
//!
//! ```no_run
//! use seq_io::fasta::Reader;
//!
//! let mut reader = Reader::from_path("input.fasta").unwrap();
//!
//! let records: Result<Vec<_>, _> = reader.records().collect();
//! ```
//!
//! # Parallel processing
//! Functions for parallel processing can be found in the [`parallel`](parallel/index.html) module

#[macro_use]
extern crate serde_derive;

use std::error;
use std::fmt;
use std::io;

macro_rules! try_opt {
    ($expr: expr) => {
        match $expr {
            Ok(item) => item,
            Err(e) => return Some(Err(::std::convert::From::from(e))),
        }
    };
}

macro_rules! unwrap_or {
    ($expr:expr, $or:block) => {
        match $expr {
            Some(item) => item,
            None => $or,
        }
    };
}

pub mod fasta;
pub mod fastq;
pub mod parallel;
pub mod policy;

/// Remove a final '\r' from a byte slice
#[inline]
fn trim_cr(line: &[u8]) -> &[u8] {
    if let Some((&b'\r', remaining)) = line.split_last() {
        remaining
    } else {
        line
    }
}

/// Makes sure the buffer is full after this call (unless EOF reached)
/// code adapted from `io::Read::read_exact`
#[inline(never)]
fn fill_buf<R>(
    reader: &mut buffer_redux::BufReader<R, buffer_redux::policy::StdPolicy>,
) -> io::Result<usize>
where
    R: io::Read,
{
    let initial_size = reader.buffer().len();
    let mut num_read = 0;
    while initial_size + num_read < reader.capacity() {
        match reader.read_into_buf() {
            Ok(0) => break,
            Ok(n) => num_read += n,
            Err(ref e) if e.kind() == io::ErrorKind::Interrupted => {}
            Err(e) => return Err(e),
        }
    }
    Ok(num_read)
}
