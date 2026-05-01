//! Efficient FASTQ reading and writing
//!

use memchr::memchr;
use std::char;
use std::fs::File;
use std::io::{self, BufRead, Seek};
use std::iter;
use std::path::Path;
use std::slice;
use std::str::{self, Utf8Error};

use buffer_redux;

use super::policy::{BufPolicy, StdPolicy};
use super::*;

use std::error::Error as StdError;

type DefaultBufPolicy = StdPolicy;

const BUFSIZE: usize = 64 * 1024;

/// Reader state
#[derive(Debug, Eq, PartialEq, Clone, Copy)]
enum State {
    /// Uninitialized (buffer not filled yet)
    New,
    /// Regular parsing with `next()`, always leading to a complete
    /// record being found (or EOF); `buf_pos` points to the
    /// last parsed record.
    Parsing,
    /// The record coordinates stored in the reader (`buf_pos`)
    /// point to the record *to be parsed next*
    /// This state occurs after `read_record_set()` or `seek()`.
    /// `incomplete_pos` may contain the part of the record in which parsing
    /// stopped (with `read_record_set()`).
    Positioned,
    /// Parsing finished
    Finished,
}

/// Used to indicate the part of a FASTQ record, within which parsing stopped
#[derive(Debug, Copy, Clone, Eq, PartialEq, Ord, PartialOrd)]
enum RecordPos {
    Head,
    Seq,
    Sep,
    Qual,
}

/// FASTQ parser.
pub struct Reader<R: io::Read, P = DefaultBufPolicy> {
    buf_reader: buffer_redux::BufReader<R>,
    buf_pos: BufferPosition,
    incomplete_pos: Option<RecordPos>,
    position: Position,
    state: State,
    buf_policy: P,
}

impl<R> Reader<R, DefaultBufPolicy>
where
    R: io::Read,
{
    /// Creates a new reader with the default buffer size of 64 KiB
    ///
    /// # Example:
    ///
    /// ```
    /// use seq_io::fastq::{Reader, Record};
    /// let fastq = b"@id\nACGT\n+\nIIII";
    ///
    /// let mut reader = Reader::new(&fastq[..]);
    /// let record = reader.next().unwrap().unwrap();
    /// assert_eq!(record.id(), Ok("id"))
    /// ```
    #[inline]
    pub fn new(reader: R) -> Reader<R, StdPolicy> {
        Reader::with_capacity(reader, BUFSIZE)
    }

    /// Creates a new reader with a given (initial) buffer capacity.
    /// The reader will enlarge the buffer as needed, but may hit a hard limit
    /// if configured so with an according [buffer policy](policy).
    /// The minimum allowed capacity is 3.
    #[inline]
    pub fn with_capacity(reader: R, capacity: usize) -> Reader<R, StdPolicy> {
        assert!(capacity >= 3);
        Reader {
            buf_reader: buffer_redux::BufReader::with_capacity(capacity, reader),
            buf_pos: BufferPosition::default(),
            incomplete_pos: None,
            position: Position::new(1, 0),
            state: State::New,
            buf_policy: StdPolicy,
        }
    }
}

impl Reader<File, DefaultBufPolicy> {
    /// Creates a reader from a file path.
    ///
    /// # Example:
    ///
    /// ```no_run
    /// use seq_io::fastq::Reader;
    ///
    /// let mut reader = Reader::from_path("seqs.fastq").unwrap();
    ///
    /// // (... do something with the reader)
    /// ```
    #[inline]
    pub fn from_path<P: AsRef<Path>>(path: P) -> io::Result<Reader<File>> {
        File::open(path).map(Reader::new)
    }

    /// Creates a reader from a file path with a given (initial) buffer capacity.
    /// The reader will enlarge the buffer as needed, but may hit a hard limit
    /// if configured so with an according [buffer policy](policy).
    /// The minimum allowed capacity is 3.
    #[inline]
    pub fn from_path_with_capacity<P: AsRef<Path>>(
        path: P,
        cap: usize,
    ) -> io::Result<Reader<File>> {
        File::open(path).map(|f| Reader::with_capacity(f, cap))
    }
}

impl<R, P> Reader<R, P>
where
    R: io::Read,
    P: BufPolicy,
{
    /// Returns a reader with the given buffer policy applied
    #[inline]
    pub fn set_policy<T: BufPolicy>(self, policy: T) -> Reader<R, T> {
        Reader {
            buf_reader: self.buf_reader,
            buf_pos: self.buf_pos,
            position: self.position,
            incomplete_pos: self.incomplete_pos,
            state: self.state,
            buf_policy: policy,
        }
    }

    /// Returns the `BufPolicy` of the reader
    #[inline]
    pub fn policy(&self) -> &P {
        &self.buf_policy
    }

    /// Searches the next FASTQ record and returns a [RefRecord](struct.RefRecord.html) that
    /// borrows its data from the underlying buffer of this reader.
    ///
    /// # Example:
    ///
    /// ```no_run
    /// use seq_io::fastq::{Reader, Record};
    ///
    /// let mut reader = Reader::from_path("seqs.fastq").unwrap();
    ///
    /// while let Some(record) = reader.next() {
    ///     let record = record.unwrap();
    ///     println!("{}", record.id().unwrap());
    /// }
    /// ```
    #[allow(clippy::should_implement_trait)]
    #[inline]
    pub fn next(&mut self) -> Option<Result<RefRecord, Error>> {
        // After next(), the state is always Parsing or Finished.
        match self.state {
            State::New => {
                if !try_opt!(self.init()) {
                    return None;
                }
                self.state = State::Parsing;
            }
            State::Positioned => {
                self.state = State::Parsing;
            }
            State::Finished => {
                return None;
            }
            State::Parsing => {
                self.increment_record();
            }
        };

        if self.incomplete_pos.is_none() {
            try_opt!(self.search());
        }

        if let Some(pos) = self.incomplete_pos {
            if !try_opt!(self.resume_incomplete_search(pos, true)) {
                return None;
            }
        }

        Some(Ok(RefRecord {
            buffer: self.get_buf(),
            buf_pos: &self.buf_pos,
        }))
    }

    /// Updates a [RecordSet](struct.RecordSet.html) with new data. The contents of the internal
    /// buffer are just copied over to the record set and the positions of all records are found.
    /// Old data will be erased. Returns `None` if the input reached its end.
    // TODO: in next major version, return Result<bool> instead!
    #[inline]
    pub fn read_record_set(&mut self, rset: &mut RecordSet) -> Option<Result<(), Error>> {
        self.read_record_set_exact(rset, None)
    }

    /// Updates a [RecordSet](struct.RecordSet.html) with new data.
    /// In comparison to [read_record_set](#method.read_record_set), this method
    /// allows specifying the exact number of records that should be read.
    /// This will grow the internal buffer (and the buffer of the record set)
    /// as needed, according to the configured [buffer policy](policy).
    /// Only the last call to `read_record_set_exact` at the end of the input
    /// can yield less than the specified number of records.
    // TODO: in next major version, return Result<bool> instead!
    #[inline]
    pub fn read_record_set_exact(
        &mut self,
        rset: &mut RecordSet,
        n_records: Option<usize>,
    ) -> Option<Result<(), Error>> {
        debug_assert!(n_records.unwrap_or(usize::MAX) > 0);
        // after read_record_set(), the state is always Positioned, Parsing or Finished
        match self.state {
            State::New => {
                if !try_opt!(self.init()) {
                    return None;
                }
                self.state = State::Positioned;
            }
            State::Finished => {
                return None;
            }
            State::Parsing => {
                // next() was previously called, the current record has
                // already been returned -> start parsing the next one
                self.increment_record();
                self.state = State::Positioned;
            }
            State::Positioned => {
                // The previous call to read_record_set() left the last record
                // incomplete, or a seek() to a position reachable from within
                // the current buffer was done.
                // Searching is resumed at the next resume_incomplete_search() call.
            }
        };

        rset.buf_positions.clear();
        let mut is_new = true;

        while self.state != State::Finished {
            if let Some(pos) = self.incomplete_pos.take() {
                // resume incomplete search after previous read_record_set(), or
                // after a seek() call.
                if !try_opt!(self.resume_incomplete_search(pos, is_new)) {
                    return None;
                }
            } else {
                // search the next complete record after `next()`, or in
                // later iterations of this loop
                if !try_opt!(self.search()) {
                    // At least one record must be present. If not, continue
                    // with `resume_incomplete_search()` in next iteration
                    if rset.buf_positions.is_empty() {
                        continue;
                    }
                    // Same if an exact number of records is required
                    if let Some(n) = n_records {
                        if rset.buf_positions.len() < n {
                            if is_new {
                                is_new = false;
                            }
                            continue;
                        }
                    }
                    break;
                }
            }
            rset.buf_positions.push(self.buf_pos.clone());
            self.increment_record();
            if let Some(n) = n_records {
                if rset.buf_positions.len() == n {
                    break;
                }
            }
        }

        rset.buffer.clear();
        rset.buffer.extend(self.get_buf());
        Some(Ok(()))
    }

    #[inline(never)]
    fn init(&mut self) -> Result<bool, Error> {
        let n = fill_buf(&mut self.buf_reader)?;
        if n == 0 {
            self.state = State::Finished;
            return Ok(false);
        }
        Ok(true)
    }

    #[inline]
    fn get_buf(&self) -> &[u8] {
        self.buf_reader.buffer()
    }

    // Sets starting points for next position
    fn increment_record(&mut self) {
        self.position.byte += (self.buf_pos.pos.1 + 1 - self.buf_pos.pos.0) as u64;
        self.position.line += 4;
        self.buf_pos.pos.0 = self.buf_pos.pos.1 + 1;
    }

    // Reads the current record and returns true if found.
    // Returns false if incomplete because end of buffer reached,
    // meaning that the last record may be incomplete.
    fn search(&mut self) -> Result<bool, Error> {
        self.buf_pos.seq = unwrap_or!(self.find_line(self.buf_pos.pos.0), {
            self.incomplete_pos = Some(RecordPos::Head);
            return Ok(false);
        });

        self.buf_pos.sep = unwrap_or!(self.find_line(self.buf_pos.seq), {
            self.incomplete_pos = Some(RecordPos::Seq);
            return Ok(false);
        });

        self.buf_pos.qual = unwrap_or!(self.find_line(self.buf_pos.sep), {
            self.incomplete_pos = Some(RecordPos::Sep);
            return Ok(false);
        });

        self.buf_pos.pos.1 = unwrap_or!(self.find_line(self.buf_pos.qual), {
            self.incomplete_pos = Some(RecordPos::Qual);
            return Ok(false);
        }) - 1;

        self.validate()?;
        Ok(true)
    }

    fn find_line(&self, search_start: usize) -> Option<usize> {
        memchr(b'\n', &self.get_buf()[search_start..]).map(|pos| search_start + pos + 1)
    }

    // To be called when the end of the buffer is reached and `search` does not find
    // the next record (the buffer *must* be fully searched).
    // Incomplete bytes are moved to the start of the buffer unless make_room is false.
    // If the record still doesn't fit in (or make_room is false), the buffer is enlarged.
    // After calling this function, the record search will therefore always be complete.
    // Returns true if if another record was found (false if at the end of the input).
    // **note**: self.state can be modified to State::Finished
    #[inline(never)]
    fn resume_incomplete_search(
        &mut self,
        mut incomplete_pos: RecordPos,
        make_room: bool,
    ) -> Result<bool, Error> {
        loop {
            if self.get_buf().len() < self.buf_reader.capacity() {
                // EOF reached, there will be no next record
                self.state = State::Finished;
                return self.check_end(incomplete_pos);
            } else if !make_room || self.buf_pos.pos.0 == 0 {
                // first record already incomplete -> buffer too small
                self.grow()?;
            } else {
                // not the first record -> buffer may be big enough
                self.make_room(incomplete_pos);
            }

            fill_buf(&mut self.buf_reader)?;

            if let Some(pos) = self.search_incomplete(incomplete_pos)? {
                incomplete_pos = pos;
            } else {
                return Ok(true);
            }
        }
    }

    #[inline(never)]
    fn check_end(&mut self, pos: RecordPos) -> Result<bool, Error> {
        if pos == RecordPos::Qual {
            // no line ending at end of last record
            self.buf_pos.pos.1 = self.get_buf().len();
            self.validate()?;
            return Ok(true);
        }

        let rest = &self.get_buf()[self.buf_pos.pos.0..];
        if rest.split(|c| *c == b'\n').all(|l| trim_cr(l).is_empty()) {
            // allow up to 3 newlines after last record (more will cause an Unexpected error)
            return Ok(false);
        }

        Err(Error::UnexpectedEnd {
            pos: self.get_error_pos(pos as u64, pos > RecordPos::Head),
        })
    }

    // Resumes reading an incomplete record without
    // re-searching positions that were already found.
    // The resulting position may still be incomplete (-> Some(RecordPos)).
    fn search_incomplete(&mut self, pos: RecordPos) -> Result<Option<RecordPos>, Error> {
        if pos == RecordPos::Head {
            self.buf_pos.seq = unwrap_or!(self.find_line(self.buf_pos.pos.0), {
                self.incomplete_pos = Some(RecordPos::Head);
                return Ok(self.incomplete_pos);
            });
        }

        if pos <= RecordPos::Seq {
            self.buf_pos.sep = unwrap_or!(self.find_line(self.buf_pos.seq), {
                self.incomplete_pos = Some(RecordPos::Seq);
                return Ok(self.incomplete_pos);
            });
        }

        if pos <= RecordPos::Sep {
            self.buf_pos.qual = unwrap_or!(self.find_line(self.buf_pos.sep), {
                self.incomplete_pos = Some(RecordPos::Sep);
                return Ok(self.incomplete_pos);
            });
        }

        if pos <= RecordPos::Qual {
            self.buf_pos.pos.1 = unwrap_or!(self.find_line(self.buf_pos.qual), {
                self.incomplete_pos = Some(RecordPos::Qual);
                return Ok(self.incomplete_pos);
            }) - 1;
        }

        self.incomplete_pos = None;

        self.validate()?;
        Ok(None)
    }

    // grow buffer based on policy
    fn grow(&mut self) -> Result<(), Error> {
        let cap = self.buf_reader.capacity();
        let new_size = self.buf_policy.grow_to(cap).ok_or(Error::BufferLimit)?;
        let additional = new_size - cap;
        self.buf_reader.reserve(additional);
        Ok(())
    }

    // move incomplete bytes to start of buffer and retry
    fn make_room(&mut self, incomplete_pos: RecordPos) {
        let consumed = self.buf_pos.pos.0;
        self.buf_reader.consume(consumed);
        self.buf_reader.make_room();

        self.buf_pos.pos.0 = 0;

        if incomplete_pos >= RecordPos::Seq {
            self.buf_pos.seq -= consumed;
        }
        if incomplete_pos >= RecordPos::Sep {
            self.buf_pos.sep -= consumed;
        }
        if incomplete_pos >= RecordPos::Qual {
            self.buf_pos.qual -= consumed;
        }
    }

    // should only be called on a complete BufferPosition
    #[inline(always)] // has performance impact and would not be inlined otherwise
    fn validate(&mut self) -> Result<(), Error> {
        let start_byte = self.get_buf()[self.buf_pos.pos.0];
        if start_byte != b'@' {
            self.state = State::Finished;
            return Err(Error::InvalidStart {
                found: start_byte,
                pos: self.get_error_pos(0, false),
            });
        }

        let sep_byte = self.get_buf()[self.buf_pos.sep];
        if sep_byte != b'+' {
            self.state = State::Finished;
            return Err(Error::InvalidSep {
                found: sep_byte,
                pos: self.get_error_pos(2, true),
            });
        }

        let qual_len = self.buf_pos.pos.1 - self.buf_pos.qual + 1;
        let seq_len = self.buf_pos.sep - self.buf_pos.seq;
        if seq_len != qual_len {
            self.state = State::Finished;
            return Err(Error::UnequalLengths {
                seq: self.buf_pos.seq(self.get_buf()).len(),
                qual: self.buf_pos.qual(self.get_buf()).len(),
                pos: self.get_error_pos(0, true),
            });
        }
        Ok(())
    }

    #[inline(never)]
    fn get_error_pos(&self, line_offset: u64, parse_id: bool) -> ErrorPosition {
        let id = if parse_id && self.buf_pos.seq - self.buf_pos.pos.0 > 1 {
            let id = self
                .buf_pos
                .head(self.get_buf())
                .split(|b| *b == b' ')
                .next()
                .unwrap();
            Some(String::from_utf8_lossy(id).into())
        } else {
            None
        };
        ErrorPosition {
            line: self.position.line + line_offset,
            id,
        }
    }

    /// Returns the current position (useful with `seek()`)
    ///
    /// # Example
    ///
    /// ```
    /// # fn main() {
    /// use seq_io::fastq::{Reader, Position};
    ///
    /// let fastq = b"@id1
    /// ACGT
    /// +
    /// IIII
    /// @id2
    /// TGCA
    /// +
    /// IIII";
    ///
    /// let mut reader = Reader::new(&fastq[..]);
    ///
    /// // skip one record
    /// reader.next().unwrap();
    /// // second position
    /// reader.next().unwrap();
    ///
    /// assert_eq!(reader.position(), &Position::new(5, 17));
    /// # }
    /// ```
    ///
    /// **Note:** After [read_record_set](Self::read_record_set), `position()`
    /// returns the position of the *next record after* the last record in the
    /// record set.
    #[inline]
    pub fn position(&self) -> &Position {
        &self.position
    }

    /// Returns a borrowed iterator over all FASTQ records. The records
    /// are owned (`OwnedRecord`), this is therefore slower than using
    /// `Reader::next()`.
    ///
    /// # Example
    ///
    /// ```
    /// # fn main() {
    /// use seq_io::fastq::{Reader, OwnedRecord};
    ///
    /// let fastq = b"@id1
    /// ACGT
    /// +
    /// IIII
    /// @id2
    /// TGCA
    /// +
    /// IIII";
    ///
    /// let mut reader = Reader::new(&fastq[..]);
    ///
    /// let records: Result<Vec<_>, _> = reader
    ///     .records()
    ///     .collect();
    ///
    /// assert_eq!(records.unwrap(),
    ///     vec![
    ///         OwnedRecord {head: b"id1".to_vec(), seq: b"ACGT".to_vec(), qual: b"IIII".to_vec()},
    ///         OwnedRecord {head: b"id2".to_vec(), seq: b"TGCA".to_vec(), qual: b"IIII".to_vec()}
    ///     ]
    /// );
    /// # }
    /// ```
    #[inline]
    pub fn records(&mut self) -> RecordsIter<R, P> {
        RecordsIter { rdr: self }
    }

    /// Returns an iterator over all FASTQ records like `Reader::records()`,
    /// but with the difference that it owns the underlying reader.
    #[inline]
    pub fn into_records(self) -> RecordsIntoIter<R, P> {
        RecordsIntoIter { rdr: self }
    }
}

impl<R, P> Reader<R, P>
where
    R: io::Read + Seek,
    P: BufPolicy,
{
    /// Seeks to a specified position.
    /// Keep the underyling buffer if the seek position is found within it, otherwise it has to be
    /// discarded.
    /// If an error was returned before, seeking to that position will return the same error.
    /// The same is not always true with `None`. If there is no newline character at the end of the
    /// file, the last record will be returned instead of `None`.
    ///
    /// # Example
    ///
    /// ```
    /// # fn main() {
    /// use seq_io::fastq::{Reader, Position, OwnedRecord};
    /// use std::io::Cursor;
    ///
    /// let fastq = b"@id1
    /// ACGT
    /// +
    /// IIII
    /// @id2
    /// TGCA
    /// +
    /// IIII";
    ///
    /// let mut cursor = Cursor::new(&fastq[..]);
    /// let mut reader = Reader::new(cursor);
    ///
    /// // read the first record and get its position
    /// let record1 = reader.next().unwrap().unwrap().to_owned_record();
    /// let pos1 = reader.position().to_owned();
    ///
    /// // read the second record
    /// reader.next().unwrap().unwrap();
    ///
    /// // now seek to position of first record
    /// reader.seek(&pos1);
    /// assert_eq!(reader.next().unwrap().unwrap().to_owned_record(), record1);
    /// # }
    /// ```
    #[inline]
    pub fn seek(&mut self, to: &Position) -> Result<(), Error> {
        let offset = to.byte as i64 - self.position.byte as i64;
        let pos = self.buf_pos.pos.0 as i64 + offset;
        self.position = to.clone();
        self.incomplete_pos = None;
        self.state = State::Positioned;

        if pos >= 0 && pos < (self.get_buf().len() as i64) {
            // position reachable within buffer -> no actual seeking necessary
            self.buf_pos.reset(pos as usize);
            return Ok(());
        }

        self.buf_reader.seek(io::SeekFrom::Start(to.byte))?;
        fill_buf(&mut self.buf_reader)?;
        self.buf_pos.reset(0);
        Ok(())
    }
}

/// Borrowed iterator of `OwnedRecord`
pub struct RecordsIter<'a, R, P = DefaultBufPolicy>
where
    P: 'a,
    R: io::Read + 'a,
{
    rdr: &'a mut Reader<R, P>,
}

impl<'a, R, P> Iterator for RecordsIter<'a, R, P>
where
    P: BufPolicy + 'a,
    R: io::Read + 'a,
{
    type Item = Result<OwnedRecord, Error>;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        self.rdr.next().map(|rec| rec.map(|r| r.to_owned_record()))
    }
}

/// Iterator of `OwnedRecord` that owns the underlying reader
pub struct RecordsIntoIter<R: io::Read, P = DefaultBufPolicy> {
    rdr: Reader<R, P>,
}

impl<R, P> Iterator for RecordsIntoIter<R, P>
where
    P: BufPolicy,
    R: io::Read,
{
    type Item = Result<OwnedRecord, Error>;

    #[inline]
    fn next(&mut self) -> Option<Self::Item> {
        self.rdr.next().map(|rec| rec.map(|r| r.to_owned_record()))
    }
}

/// Holds line number and byte offset of a FASTQ record
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord)]
pub struct Position {
    line: u64,
    byte: u64,
}

impl Position {
    #[inline]
    pub fn new(line: u64, byte: u64) -> Position {
        Position { line, byte }
    }

    /// Line number (starting with 1)
    #[inline]
    pub fn line(&self) -> u64 {
        self.line
    }

    /// Byte offset within the file
    #[inline]
    pub fn byte(&self) -> u64 {
        self.byte
    }
}

/// FASTQ parsing error
#[derive(Debug)]
pub enum Error {
    Io(io::Error),
    /// sequence and qualitiy lengths are not equal
    UnequalLengths {
        /// Length of sequence
        seq: usize,
        /// Length of qualities
        qual: usize,
        /// Position within file.
        /// `ErrorPosition::line` has the position of the header, not sequence/qualities
        pos: ErrorPosition,
    },
    /// Invalid start byte encountered (expected `@`)
    InvalidStart {
        /// Byte found instead.
        found: u8,
        /// Position within file. `ErrorPosition::id` will be `None`.
        pos: ErrorPosition,
    },
    /// Invalid separator byte encountered (expected `+`)
    InvalidSep {
        /// Byte found instead.
        found: u8,
        /// Position within file
        pos: ErrorPosition,
    },
    /// Truncated record found
    UnexpectedEnd {
        /// Position within file.
        pos: ErrorPosition,
    },
    /// Size limit of buffer was reached, which happens if `policy::BufPolicy::grow_to()` returned
    /// `None`. This does not happen with the default `struct.DoubleUntil.html` policy.
    BufferLimit,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ErrorPosition {
    /// Line number where the error occurred (starting with 1)
    pub line: u64,
    /// ID of record if available
    pub id: Option<String>,
}

impl fmt::Display for ErrorPosition {
    #[inline]
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        if let Some(id) = self.id.as_ref() {
            write!(f, "record '{}' at ", id)?;
        }
        write!(f, "line {}", self.line)
    }
}

impl fmt::Display for Error {
    #[inline]
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Error::Io(ref e) => e.fmt(f),
            Error::UnequalLengths { seq, qual, ref pos } => write!(
                f,
                "FASTQ parse error: sequence length is {}, but quality length is {} ({}).",
                seq, qual, pos
            ),
            Error::InvalidStart { found, ref pos } => write!(
                f,
                "FASTQ parse error: expected '@' at record start but found '{}' ({}).",
                (found as char).escape_default(),
                pos
            ),
            Error::InvalidSep { found, ref pos } => write!(
                f,
                "FASTQ parse error: Expected '+' separator but found '{}' ({}).",
                (found as char).escape_default(),
                pos
            ),
            Error::UnexpectedEnd { ref pos } => {
                write!(f, "FASTQ parse error: unexpected end of input ({}).", pos)
            }
            Error::BufferLimit => write!(f, "FASTQ parse error: Buffer limit reached."),
        }
    }
}

impl From<io::Error> for Error {
    #[inline]
    fn from(e: io::Error) -> Error {
        Error::Io(e)
    }
}

impl StdError for Error {
    #[inline]
    fn source(&self) -> Option<&(dyn StdError + 'static)> {
        match *self {
            Error::Io(ref err) => Some(err),
            _ => None,
        }
    }
}

/// Represents the position of a record within a buffer
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
struct BufferPosition {
    // (start, stop), but might include \r at the end
    pos: (usize, usize),
    seq: usize,
    sep: usize,
    qual: usize,
}

impl BufferPosition {
    #[inline]
    fn reset(&mut self, start: usize) {
        self.pos.0 = start;
        self.pos.1 = 0;
    }

    #[inline]
    fn head<'a>(&'a self, buffer: &'a [u8]) -> &'a [u8] {
        trim_cr(&buffer[self.pos.0 + 1..self.seq - 1])
    }

    #[inline]
    fn seq<'a>(&'a self, buffer: &'a [u8]) -> &'a [u8] {
        trim_cr(&buffer[self.seq..self.sep - 1])
    }

    #[inline]
    fn qual<'a>(&'a self, buffer: &'a [u8]) -> &'a [u8] {
        trim_cr(&buffer[self.qual..self.pos.1])
    }
}

/// FASTQ record trait implemented by both `RefRecord` and `OwnedRecord`
pub trait Record {
    /// Return the header line of the record as byte slice
    fn head(&self) -> &[u8];
    /// Return the FASTQ sequence as byte slice
    fn seq(&self) -> &[u8];
    /// Return the FASTQ qualities as byte slice
    fn qual(&self) -> &[u8];

    #[inline]
    fn id_bytes(&self) -> &[u8] {
        self.head().split(|b| *b == b' ').next().unwrap()
    }

    /// Return the ID of the record (everything before an optional space) as string slice
    #[inline]
    fn id(&self) -> Result<&str, Utf8Error> {
        str::from_utf8(self.id_bytes())
    }

    #[inline]
    fn desc_bytes(&self) -> Option<&[u8]> {
        self.head().splitn(2, |b| *b == b' ').nth(1)
    }

    /// Return the description of the record as string slice, if present. Otherwise, `None` is returned.
    #[inline]
    fn desc(&self) -> Option<Result<&str, Utf8Error>> {
        self.desc_bytes().map(str::from_utf8)
    }

    /// Return both the ID and the description of the record (if present)
    /// This should be faster than calling `id()` and `desc()` separately.
    #[inline]
    fn id_desc_bytes(&self) -> (&[u8], Option<&[u8]>) {
        let mut h = self.head().splitn(2, |c| *c == b' ');
        (h.next().unwrap(), h.next())
    }

    /// Return both the ID and the description of the record (if present)
    /// This should be faster than calling `id()` and `desc()` separately.
    #[inline]
    fn id_desc(&self) -> Result<(&str, Option<&str>), Utf8Error> {
        let mut h = str::from_utf8(self.head())?.splitn(2, ' ');
        Ok((h.next().unwrap(), h.next()))
    }

    /// Writes a record to the given `io::Write` instance
    #[inline]
    fn write<W: io::Write>(&self, writer: W) -> io::Result<()> {
        write_to(writer, self.head(), self.seq(), self.qual())
    }
}

/// A FASTQ record that borrows data from a buffer
#[derive(Debug, Clone)]
pub struct RefRecord<'a> {
    buffer: &'a [u8],
    buf_pos: &'a BufferPosition,
}

impl Record for RefRecord<'_> {
    #[inline]
    fn head(&self) -> &[u8] {
        self.buf_pos.head(self.buffer)
    }

    #[inline]
    fn seq(&self) -> &[u8] {
        self.buf_pos.seq(self.buffer)
    }

    #[inline]
    fn qual(&self) -> &[u8] {
        self.buf_pos.qual(self.buffer)
    }
}

impl RefRecord<'_> {
    #[inline]
    pub fn to_owned_record(&self) -> OwnedRecord {
        OwnedRecord {
            head: self.head().to_vec(),
            seq: self.seq().to_vec(),
            qual: self.qual().to_vec(),
        }
    }

    /// Writes a record to the given `io::Write` instance
    /// by just writing the unmodified input, which is faster than `RefRecord::write`
    #[inline]
    pub fn write_unchanged<W: io::Write>(&self, mut writer: W) -> io::Result<()> {
        let data = &self.buffer[self.buf_pos.pos.0..self.buf_pos.pos.1];
        writer.write_all(data)?;
        writer.write_all(b"\n")
    }
}

/// A FASTQ record that ownes its data (requires allocations)
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct OwnedRecord {
    pub head: Vec<u8>,
    pub seq: Vec<u8>,
    pub qual: Vec<u8>,
}

impl Record for OwnedRecord {
    #[inline]
    fn head(&self) -> &[u8] {
        &self.head
    }
    #[inline]
    fn seq(&self) -> &[u8] {
        &self.seq
    }
    #[inline]
    fn qual(&self) -> &[u8] {
        &self.qual
    }
}

/// Set of FASTQ records that owns it's buffer
/// and knows the positions of each record.
#[derive(Default, Clone, Debug, Serialize, Deserialize)]
pub struct RecordSet {
    buffer: Vec<u8>,
    buf_positions: Vec<BufferPosition>,
}

impl RecordSet {
    /// Returns the number of records in the record set
    #[inline]
    pub fn len(&self) -> usize {
        self.buf_positions.len()
    }

    /// Returns true if there are no records in the set
    #[inline]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Returns the capacity of the record set buffer
    #[inline]
    pub fn buf_capacity(&self) -> usize {
        self.buffer.capacity()
    }

    /// Shrinks the internal buffer to fit the data.
    /// This can be useful to reduce excessive memory usage.
    #[inline]
    pub fn shrink_buffer_to_fit(&mut self) {
        self.buffer.shrink_to_fit();
    }
}

impl<'a> iter::IntoIterator for &'a RecordSet {
    type Item = RefRecord<'a>;
    type IntoIter = RecordSetIter<'a>;

    #[inline]
    fn into_iter(self) -> Self::IntoIter {
        RecordSetIter {
            buffer: &self.buffer,
            pos: self.buf_positions.iter(),
        }
    }
}

/// Iterator over record sets
pub struct RecordSetIter<'a> {
    buffer: &'a [u8],
    pos: slice::Iter<'a, BufferPosition>,
}

impl<'a> Iterator for RecordSetIter<'a> {
    type Item = RefRecord<'a>;

    #[inline]
    fn next(&mut self) -> Option<RefRecord<'a>> {
        self.pos.next().map(|p| RefRecord {
            buffer: self.buffer,
            buf_pos: p,
        })
    }
}

/// Helper function for writing data (not necessarily stored in a `Record` instance)
/// to the FASTQ format
#[inline]
pub fn write_to<W: io::Write>(
    mut writer: W,
    head: &[u8],
    seq: &[u8],
    qual: &[u8],
) -> io::Result<()> {
    writer.write_all(b"@")?;
    writer.write_all(head)?;
    writer.write_all(b"\n")?;
    writer.write_all(seq)?;
    writer.write_all(b"\n+\n")?;
    writer.write_all(qual)?;
    writer.write_all(b"\n")?;
    Ok(())
}

/// Helper function for writing data (not necessarily stored in a `Record` instance)
/// to the FASTQ format. The ID and description parts of the header are supplied separately
/// instead of a whole header line
#[inline]
pub fn write_parts<W: io::Write>(
    mut writer: W,
    id: &[u8],
    desc: Option<&[u8]>,
    seq: &[u8],
    qual: &[u8],
) -> io::Result<()> {
    writer.write_all(b"@")?;
    writer.write_all(id)?;
    if let Some(d) = desc {
        writer.write_all(b" ")?;
        writer.write_all(d)?;
    }
    writer.write_all(b"\n")?;
    writer.write_all(seq)?;
    writer.write_all(b"\n+\n")?;
    writer.write_all(qual)?;
    writer.write_all(b"\n")?;
    Ok(())
}
