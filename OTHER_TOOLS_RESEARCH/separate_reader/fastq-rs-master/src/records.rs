use memchr::memchr;
use std::io::{Error, ErrorKind, Result, Write};

/// Trait to be implemented by types that represent fastq records.
pub trait Record {
    /// Return the fastq sequence as byte slice
    fn seq(&self) -> &[u8];
    /// Return the id-line of the record as byte slice
    fn head(&self) -> &[u8];
    /// Return the quality of the bases as byte slice
    fn qual(&self) -> &[u8];
    /// Write the record to a writer
    fn write<W: Write>(&self, writer: &mut W) -> Result<usize>;

    /// Return true if the sequence contains only A, C, T and G.
    ///
    /// FIXME This might be much faster with a [bool; 256] array
    /// or using some simd instructions (eg with the jetscii crate).
    fn validate_dna(&self) -> bool {
        self.seq()
            .iter()
            .all(|&x| x == b'A' || x == b'C' || x == b'T' || x == b'G')
    }

    /// Return true if the sequence contains only A, C, T, G and N.
    ///
    /// FIXME This might be much faster with a [bool; 256] array
    /// or using some simd instructions (eg with the jetscii crate).
    fn validate_dnan(&self) -> bool {
        self.seq()
            .iter()
            .all(|&x| x == b'A' || x == b'C' || x == b'T' || x == b'G' || x == b'N')
    }
}

/// A fastq record that borrows data from an array.
#[derive(Debug)]
pub struct RefRecord<'a> {
    // (start, stop), but might include \r at the end
    head: usize,
    seq: usize,
    sep: usize,
    qual: usize,
    data: &'a [u8],
}

/// A fastq record that ownes its data arrays.
#[derive(Debug)]
pub struct OwnedRecord {
    pub head: Vec<u8>,
    pub seq: Vec<u8>,
    pub sep: Option<Vec<u8>>,
    pub qual: Vec<u8>,
}

#[derive(Debug)]
pub struct IdxRecord {
    head: usize,
    seq: usize,
    sep: usize,
    qual: usize,
    pub data: (usize, usize),
}

/// Remove a final '\r' from a byte slice
#[inline]
fn trim_winline(line: &[u8]) -> &[u8] {
    if let Some((&b'\r', remaining)) = line.split_last() {
        remaining
    } else {
        line
    }
}

impl<'a> Record for RefRecord<'a> {
    #[inline]
    fn head(&self) -> &[u8] {
        // skip the '@' at the beginning
        trim_winline(&self.data[1..self.head])
    }

    #[inline]
    fn seq(&self) -> &[u8] {
        trim_winline(&self.data[self.head + 1..self.seq])
    }

    #[inline]
    fn qual(&self) -> &[u8] {
        trim_winline(&self.data[self.sep + 1..self.qual])
    }

    #[inline]
    fn write<W: Write>(&self, writer: &mut W) -> Result<usize> {
        writer.write_all(&self.data)?;
        Ok(self.data.len())
    }
}

impl Record for OwnedRecord {
    fn head(&self) -> &[u8] {
        // skip the '@' at the beginning
        &self.head
    }

    fn seq(&self) -> &[u8] {
        &self.seq
    }

    fn qual(&self) -> &[u8] {
        &self.qual
    }

    fn write<W: Write>(&self, writer: &mut W) -> Result<usize> {
        let mut written = 0;
        written += writer.write(b"@")?;
        written += writer.write(self.head())?;
        written += writer.write(b"\n")?;
        written += writer.write(self.seq())?;
        written += writer.write(b"\n")?;
        match self.sep {
            Some(ref s) => written += writer.write(s)?,
            None => written += writer.write(b"+")?,
        }
        written += writer.write(b"\n")?;
        written += writer.write(self.qual())?;
        written += writer.write(b"\n")?;
        Ok(written)
    }
}

pub enum IdxRecordResult {
    Incomplete,
    EmptyBuffer,
    Record(IdxRecord),
}

#[inline]
fn read_header(buffer: &[u8]) -> Result<Option<usize>> {
    match buffer.first() {
        None => Ok(None),
        Some(&b'@') => Ok(memchr(b'\n', buffer)),
        Some(_) => {
            return Err(Error::new(
                ErrorKind::InvalidData,
                "Fastq headers must start with '@'",
            ))
        }
    }
}

#[inline]
fn read_sep(buffer: &[u8]) -> Result<Option<usize>> {
    match buffer.first() {
        None => return Ok(None),
        Some(&b'+') => Ok(memchr(b'\n', buffer)),
        Some(_) => {
            return Err(Error::new(
                ErrorKind::InvalidData,
                "Sequence and quality not separated by +",
            ));
        }
    }
}

impl<'a> RefRecord<'a> {
    /// Copy the borrowed data array and return an owned record.
    pub fn to_owned_record(&self) -> OwnedRecord {
        OwnedRecord {
            seq: self.seq().to_vec(),
            qual: self.qual().to_vec(),
            head: self.head().to_vec(),
            sep: Some(trim_winline(&self.data[self.seq + 1..self.sep]).to_vec()),
        }
    }
}

impl IdxRecord {
    #[inline]
    pub fn to_ref_record<'a>(&self, buffer: &'a [u8]) -> RefRecord<'a> {
        let data = &buffer[self.data.0..self.data.1];
        let datalen = data.len();
        debug_assert!(datalen == self.data.1 - self.data.0);

        debug_assert!(self.head < datalen);
        debug_assert!(self.qual < datalen);
        debug_assert!(self.seq < datalen);
        debug_assert!(self.sep < datalen);
        debug_assert!(self.head < self.seq);
        debug_assert!(self.seq < self.sep);
        debug_assert!(self.sep < self.qual);

        RefRecord {
            data: data,
            head: self.head,
            seq: self.seq,
            sep: self.sep,
            qual: self.qual,
        }
    }

    #[inline]
    pub fn from_buffer(buffer: &[u8]) -> Result<IdxRecordResult> {
        if buffer.len() == 0 {
            return Ok(IdxRecordResult::EmptyBuffer);
        }

        let head_end = match read_header(buffer)? {
            None => return Ok(IdxRecordResult::Incomplete),
            Some(val) => val,
        };
        let pos = head_end + 1;

        let buffer_ = &buffer[pos..];
        let seq_end = match memchr(b'\n', buffer_) {
            None => return Ok(IdxRecordResult::Incomplete),
            Some(end) => end + pos,
        };
        let pos = seq_end + 1;

        let buffer_ = &buffer[pos..];
        let sep_end = match read_sep(buffer_)? {
            None => return Ok(IdxRecordResult::Incomplete),
            Some(end) => end + pos,
        };
        let pos = sep_end + 1;

        let buffer_ = &buffer[pos..];
        let qual_end = match memchr(b'\n', buffer_) {
            None => return Ok(IdxRecordResult::Incomplete),
            Some(end) => end + pos,
        };

        if qual_end - sep_end != seq_end - head_end {
            return Err(Error::new(
                ErrorKind::InvalidData,
                "Sequence and quality length mismatch",
            ));
        }

        Ok(IdxRecordResult::Record(IdxRecord {
            data: (0, qual_end + 1),
            head: head_end,
            seq: seq_end,
            sep: sep_end,
            qual: qual_end,
        }))
    }
}
