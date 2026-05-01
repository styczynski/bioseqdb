use crate::parse_fasta_str;

#[test]
fn empty_fasta() {
    let empty = "";
    assert!(parse_fasta_str(&empty).unwrap().sequences.is_empty());
}

#[test]
fn empty_description() {
    let empty = ">\nA";
    let fasta = parse_fasta_str(&empty).unwrap();
    assert_eq!(fasta.sequences.len(), 1);

    let seq = &fasta.sequences[0];
    assert_eq!(seq.description, b"");
    assert_eq!(seq.sequence, b"A");
}

#[test]
fn one_sequence() {
    let seq = r#"
>P32234 1-368
MSTILEKISAIESEMARTQKNKATSAHLGLLKAKLAKLRRELISPKGGGGGTGEAGFEVAKTGDARVGFVGFPSVGKSTL
    "#
    .trim();

    let fasta = parse_fasta_str(&seq).expect("Failed to parse FASTA");
    assert_eq!(fasta.sequences.len(), 1);
    assert_eq!(fasta.sequences[0].description, b"P32234 1-368");
    assert_eq!(
        String::from_utf8(fasta.sequences[0].iter().copied().collect::<Vec<_>>()).unwrap(),
        "MSTILEKISAIESEMARTQKNKATSAHLGLLKAKLAKLRRELISPKGGGGGTGEAGFEVAKTGDARVGFVGFPSVGKSTL"
    );
}

#[test]
fn multi_sequence() {
    let seq = r"
>P32234 1-368
MSTILEKISAIESEMARTQKNKATSAHLGLLKAKLAKLRRELISPKGGGGGTGEAGFEVAKTGDARVGFVGFPSVGKSTL

>O77448 1-1117
MQKINNINNNKQMLTRKEDLLTVLKQISALKYVSNLYEFLLATEKIVQTSELDTQFQEFLTTTIIASEQNLVENYKQKYN
    "
    .trim();

    let fasta = parse_fasta_str(&seq).expect("Failed to parse FASTA");
    assert_eq!(fasta.sequences.len(), 2);

    assert_eq!(fasta.sequences[0].description, b"P32234 1-368");
    assert_eq!(
        String::from_utf8(fasta.sequences[0].iter().copied().collect::<Vec<_>>()).unwrap(),
        "MSTILEKISAIESEMARTQKNKATSAHLGLLKAKLAKLRRELISPKGGGGGTGEAGFEVAKTGDARVGFVGFPSVGKSTL"
    );

    assert_eq!(fasta.sequences[1].description, b"O77448 1-1117");
    assert_eq!(
        String::from_utf8(fasta.sequences[1].iter().copied().collect::<Vec<_>>()).unwrap(),
        "MQKINNINNNKQMLTRKEDLLTVLKQISALKYVSNLYEFLLATEKIVQTSELDTQFQEFLTTTIIASEQNLVENYKQKYN"
    );
}

#[test]
fn test_relaxed_fasta() {
    let seq = r"
>P32234 1-368
MSTILEKISAIESEMARTQKNKATSAHLGLLKAKLAKLRRELISPKGGGGGTGEAGFEVAKTGDARVGFVGFPSVGKSTL
MQKINNINNNKQMLTRKEDLLTVLKQISALKYVSNLYEFLLATEKIVQTSELDTQFQEFLTTTII

ASEQNLVENYKQKYN


    "
    .trim();

    let fasta = parse_fasta_str(&seq).expect("Failed to parse FASTA");
    assert_eq!(fasta.sequences.len(), 1);

    assert_eq!(fasta.sequences[0].description, b"P32234 1-368");
    assert_eq!(String::from_utf8(fasta.sequences[0].iter().copied().collect::<Vec<_>>()).unwrap(),
               "MSTILEKISAIESEMARTQKNKATSAHLGLLKAKLAKLRRELISPKGGGGGTGEAGFEVAKTGDARVGFVGFPSVGKSTLMQKINNINNNKQMLTRKEDLLTVLKQISALKYVSNLYEFLLATEKIVQTSELDTQFQEFLTTTIIASEQNLVENYKQKYN")
}

#[test]
fn test_copy_sequential() {
    let seq = ">ABCD\nATG\nGTA\nCCC\nCGC\nAT";
    let fasta = parse_fasta_str(&seq).expect("Failed to parse FASTA");

    let copied = fasta.sequences[0].copy_sequential();
    assert_eq!(copied.as_ref(), b"ATGGTACCCCGCAT");
}
