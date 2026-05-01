

#[inline]
pub fn trim_newline(line: &[u8]) -> &[u8] {
    trim_end(trim_end(line, b'\n'), b'\r')
}

#[inline]
pub fn trim_end(line: &[u8], byte: u8) -> &[u8] {
    if let Some((&b, remaining)) = line.split_last() {
        if b == byte {
            return remaining;
        }
    }
    line
}

macro_rules! try_opt {
    ($expr: expr) => {
        match $expr {
            Ok(item) => item,
            Err(e) => return Some(Err(::std::convert::From::from(e))),
        }
    };
}
