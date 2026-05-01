#![forbid(unsafe_code)]
#![warn(missing_docs)]

//! # Fire-Fasta
//! Ultra-fast, lightweight, zero-copy, lazy Multi-FASTA parser.
//!
//! The parser is intended for high performance applications where the input is expected to be well-formed.
//! Therefore, it sacrifices input validation and deprecated features for parsing performance.
//!
//! ### Sequence Characters
//! The parser makes no assumptions about the sequence alphabet:
//! It is explicitly intended for custom sequences with characters that do not conform to NCBI specifications.
//! The only characters not allowed in sequences are unix-style newlines (`LF`),
//! which are ignored, and the greater-than sign (`>`),
//! which starts a new sequence descriptor in Multi-FASTA files.
//! Note, that the parser does not validate whether a sequence description starts at the beginning of a new line.
//!
//! The parser expects input data that is compatible with ASCII.
//! Multibyte UTF-8 codepoints are processed as separate ASCII characters.
//!
//! Windows-style newlines (`CRLF`) are not supported.
//! Instead, the parser treats the `LF` as a unix-style newline and preserve the `CR` as a valid sequence character.
//! Old FASTA comments starting with `;` are also not supported, they are treated as part of the sequence.
//!
//! ### Usage and Lazy Parsing
//! Calling the parser does one pass over the entire input, separating individual fasta sequences from each other.
//! No further processing is done and no data is copied.
//! ```rust
//! # use fire_fasta::parse_fasta_str;
//! # use std::error::Error;
//! # fn main() -> Result<(), Box<dyn Error>> {
//! let seq = ">example\nMSTIL\nAATIL\n\n";
//! let fasta = parse_fasta_str(&seq)?;
//! // or parse_fasta(&data) for &[u8] slices
//!
//! assert_eq!(fasta.sequences.len(), 1);
//!
//! // Iterating over a sequence removes newlines from the iterator on the fly:
//! assert_eq!(
//!     String::from_utf8(fasta.sequences[0].iter().copied().collect::<Vec<_>>())?,
//!     "MSTILAATIL"
//! );
//!
//! //If you want to iterate over a sequence multiple times, it may be faster to first copy the full sequence into its own buffer:
//! let copied: Box<[u8]> = fasta.sequences[0].copy_sequential();
//! assert_eq!(copied.as_ref(), b"MSTILAATIL");
//! # Ok(())
//! # }
//! ```
//!
//! Parsing and copying use the [memchr](https://crates.io/crates/memchr) crate,
//! and thus operations use SIMD instructions when available.

use memchr::memchr;
use std::error::Error;
use std::fmt::{Display, Formatter};

/// A Multi FASTA file containing zero, one, or more [`FastaSequences`].
/// Access the sequences simply through its `sequences` field:
///
/// ```rust
/// # use fire_fasta::parse_fasta;
/// # use std::error::Error;
/// # fn main() -> Result<(), Box<dyn Error>> {
/// let fasta_file = b">Sample1\nACGTCA\n>Sample2\nACGTCC";
/// let fasta = parse_fasta(fasta_file)?;
///
/// assert_eq!(fasta.sequences[0].description, b"Sample1");
/// assert_eq!(fasta.sequences[1].description, b"Sample2");
///
/// assert_eq!(*fasta.sequences[0].iter().nth(2).unwrap(), b'G');
/// # Ok(())
/// # }
/// ```
///
/// [`FastaSequences`]: FastaSequence
#[derive(Clone, Debug)]
pub struct Fasta<'a> {
    /// A vector of sequences present in the fasta file.
    pub sequences: Vec<FastaSequence<'a>>,
}

/// A FASTA sequence with a description from a FASTA file.
/// The sequence is not processed in any way, meaning accessing it performs further parsing when necessary.
#[derive(Clone, Debug)]
pub struct FastaSequence<'a> {
    /// A byte slice containing the sequence description (without the leading '>' character,
    /// and without the trailing newline.
    pub description: &'a [u8],
    sequence: &'a [u8],
}

/// FASTA parsing error thrown during the initial parsing step in [`parse_fasta`]
///
/// [`parse_fasta`]: parse_fasta
#[derive(Clone, Debug)]
pub enum ParseError {
    /// Invalid descriptor start character.
    /// The parser expects any FASTA description line to start with '>'.
    /// The invalid character is returned in the error.
    ///
    /// Since the parser doesn't mind excess newlines between sequences,
    /// this error can only occur if the very first character of a FASTA file isn't a `>`.
    /// If further descriptors in a Multi-FASTA file don't start with `>`, they are added to their
    /// preceding sequence as valid sequence characters.
    InvalidDescription {
        /// The one-byte code point of the wrong descriptor character in the file.
        invalid: u8,
    },

    /// A valid descriptor was parsed, but no sequence is following
    EmptySequence,
}

impl Display for ParseError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        write!(f, "{self:?}")
    }
}

impl Error for ParseError {}

impl<'a> FastaSequence<'a> {
    /// Returns an iterator over the FASTA sequence characters, excluding newlines.
    /// Note that the parser expects unix-style line breaks, thus, CR-characters are preserved.
    ///
    /// Newlines are filtered out on the fly, meaning that multiple calls to `iter` repeatedly
    /// search and skip them during iteration.
    #[inline]
    pub fn iter(&self) -> impl Iterator<Item = &u8> {
        self.sequence.iter().filter(|&x| *x != b'\n')
    }

    /// Copy the sequence into a consecutive memory region.
    /// This method allocates a buffer and copies the sequence into it, skipping newline symbols.
    /// Note that any other symbols (including whitespace and line feeds) get preserved.
    /// The capacity of the return value may be larger than the actual sequence.
    /// It is guaranteed, however, that only one allocation is performed.
    #[must_use]
    pub fn copy_sequential(&self) -> Box<[u8]> {
        let mut buffer = vec![0u8; self.size_hint()];
        let mut target = 0;
        let mut pos = 0;
        loop {
            let pivot = memchr(b'\n', &self.sequence[pos..]).unwrap_or(self.sequence.len() - pos);
            buffer[target..target + pivot].copy_from_slice(&self.sequence[pos..pos + pivot]);
            pos += pivot + 1;
            target += pivot;

            if pos >= self.sequence.len() {
                break;
            }
        }
        buffer.truncate(target);
        buffer.into_boxed_slice()
    }

    /// Returns the maximum size in bytes this sequence occupies.
    /// This size is a limit and could be smaller,
    /// for example if newlines are filtered out of the sequence (see [`copy_sequential`])
    ///
    /// [`copy_sequential`]: FastaSequence::copy_sequential
    pub fn size_hint(&self) -> usize {
        self.sequence.len()
    }
}

/// Parse a FASTA or Multi FASTA file.
/// Sequence descriptions are expected to start with '>'.
/// The deprecated comment character ';' is not parsed, neither for sequence descriptors nor for
/// additional comment lines.
/// Parsing is done lazily: Sequence descriptions and sequences are identified, but are not further
/// processed.
///
/// # Errors
/// If the file is not empty, but the first character is not a greater-than sign (`>`), the function
/// returns an [`InvalidDescription`] error.
///
/// If the file ends in a valid FASTA sequence description, but no sequence follows, the function
/// returns an [`EmptySequence`] error.
///
/// # Returns
/// A [`Fasta`] instance containing all sequences from the Multi-Fasta file
///
/// [`InvalidDescription`]: ParseError::InvalidDescription
/// [`EmptySequence`]: ParseError::EmptySequence
pub fn parse_fasta_str(s: &str) -> Result<Fasta, ParseError> {
    parse_fasta(s.as_bytes())
}

/// Parse a FASTA or Multi FASTA file.
/// Sequence descriptions are expected to start with '>'.
/// The deprecated comment character ';' is not parsed, neither for sequence descriptors nor for
/// additional comment lines.
/// Parsing is done lazily: Sequence descriptions and sequences are identified, but are not further
/// processed.
///
/// # Errors
/// If the file is not empty, but the first character is not a greater-than sign (`>`), the function
/// returns an [`InvalidDescription`] error.
///
/// If the file ends in a valid FASTA sequence description, but no sequence follows, the function
/// returns an [`EmptySequence`] error.
///
/// # Returns
/// A [`Fasta`] instance containing all sequences from the Multi-Fasta file
///
/// [`InvalidDescription`]: ParseError::InvalidDescription
/// [`EmptySequence`]: ParseError::EmptySequence
pub fn parse_fasta(data: &[u8]) -> Result<Fasta, ParseError> {
    let mut sequences = Vec::new();

    if data.is_empty() {
        return Ok(Fasta { sequences });
    }

    let mut cursor = 0usize;

    loop {
        if !expect(data, b'>', &mut cursor) {
            return Err(ParseError::InvalidDescription {
                invalid: data[cursor],
            });
        }

        let header_end = memchr(b'\n', &data[cursor..]).unwrap_or(data.len() - cursor);
        let description = &data[cursor..cursor + header_end];
        cursor += header_end + 1;

        if cursor >= data.len() {
            return Err(ParseError::EmptySequence);
        }

        let sequence_end = memchr(b'>', &data[cursor..]).unwrap_or(data.len() - cursor);
        // may contain trailing white space
        let sequence = &data[cursor..cursor + sequence_end];
        cursor += sequence_end;

        sequences.push(FastaSequence {
            description,
            sequence,
        });

        if cursor >= data.len() {
            break;
        }
    }

    Ok(Fasta { sequences })
}

/// Expect that the byte at [cursor] is equal to [expected]. If it is, advance the cursor by one.
/// Returns false, if the byte is not equal to the expected byte.
#[inline]
fn expect(data: &[u8], expected: u8, cursor: &mut usize) -> bool {
    if data[*cursor] == expected {
        *cursor += 1;
        true
    } else {
        false
    }
}

#[cfg(test)]
mod tests;
