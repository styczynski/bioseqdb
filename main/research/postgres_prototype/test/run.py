import os
import psycopg2
import string
import sys
import traceback

_status = 0
_conn = psycopg2.connect(os.environ['DB_URI'])
_conn.cursor().execute("CREATE EXTENSION IF NOT EXISTS bioseqdb;")
_conn.commit()
def test(case):
    global _status
    try:
        case(_conn.cursor())
        print(f'{case.__name__} \x1B[1;32mok\x1B[0m', file=sys.stderr)
    except:
        traceback.print_exc()
        print(f'{case.__name__} \x1B[1;31merror\x1B[0m', file=sys.stderr)
        _status = 1
    _conn.rollback()
test.rollback = lambda: _conn.rollback()

ALLOWED_NUCLEOTIDES = set('ACGTNRYKMSWBDHVN')

@test
def nuclseq_accept_basic_symbols(sql):
    sql.execute("SELECT 'ACGT'::NUCLSEQ;")
    assert sql.fetchone() == ('ACGT',)

@test
def nuclseq_accept_wildcard_symbol(sql):
    sql.execute("SELECT 'N'::NUCLSEQ;")
    assert sql.fetchone() == ('N',)

@test
def nuclseq_accept_subset_symbols(sql):
    sql.execute("SELECT 'RYKMSWBDHVN'::NUCLSEQ;")
    assert sql.fetchone() == ('RYKMSWBDHVN',)

@test
def nuclseq_reject_lowercase_symbols(sql):
    for symbol in string.ascii_lowercase:
        failed = False
        try:
            sql.execute("SELECT %s::NUCLSEQ;", (symbol,))
        except psycopg2.DataError as e:
            assert f"invalid nucleotide in nuclseq_in: '{symbol}'" in e.pgerror
            failed = True
        assert failed
        test.rollback()

@test
def nuclseq_reject_unknown_letters(sql):
    for symbol in set(string.ascii_uppercase) - ALLOWED_NUCLEOTIDES:
        failed = False
        try:
            sql.execute("SELECT %s::NUCLSEQ;", (symbol,))
        except psycopg2.DataError as e:
            assert f"invalid nucleotide in nuclseq_in: '{symbol}'" in e.pgerror
            failed = True
        assert failed
        test.rollback()

@test
def nuclseq_reject_nonletter_ascii(sql):
    for symbol in set(chr(i) for i in range(1, 128)) - set(string.ascii_letters):
        failed = False
        try:
            sql.execute("SELECT %s::NUCLSEQ;", (symbol,))
        except psycopg2.DataError as e:
            assert f"invalid nucleotide in nuclseq_in: '{symbol}'" in e.pgerror
            failed = True
        assert failed
        test.rollback()

@test
def nuclseq_reject_valid_utf8(sql):
    failed = False
    try:
        sql.execute("SELECT %s::NUCLSEQ;", ("żółć",))
    except psycopg2.DataError as e:
        assert f"invalid nucleotide in nuclseq_in: '�'" in e.pgerror
        failed = True
    assert failed

@test
def nuclseq_reject_invalid_utf8(sql):
    failed = False
    try:
        sql.execute("SELECT %s::NUCLSEQ;", ("\xc3\x28",))
    except psycopg2.DataError as e:
        assert f"invalid nucleotide in nuclseq_in: '�'" in e.pgerror
        failed = True
    assert failed

@test
def nuclseq_length_zero(sql):
    sql.execute("SELECT nuclseq_len('');")
    assert sql.fetchone() == (0,)

@test
def nuclseq_length_one(sql):
    sql.execute("SELECT nuclseq_len('A');")
    assert sql.fetchone() == (1,)

@test
def nuclseq_length_ten(sql):
    sql.execute("SELECT nuclseq_len('ACGTNRYKMS');")
    assert sql.fetchone() == (10,)

@test
def nuclseq_content_zero(sql):
    sql.execute("SELECT nuclseq_content('ACACACAC', 'G');")
    assert sql.fetchone() == (0.0,)

@test
def nuclseq_content_one(sql):
    sql.execute("SELECT nuclseq_content('GGGGGGGG', 'G');")
    assert sql.fetchone() == (1.0,)

@test
def nuclseq_content_half(sql):
    sql.execute("SELECT nuclseq_content('ACACACAC', 'A');")
    assert sql.fetchone() == (0.5,)

@test
def nuclseq_content_with_wildcards(sql):
    sql.execute("SELECT nuclseq_content('ANNNANNN', 'A');")
    assert sql.fetchone() == (0.25,)

@test
def nuclseq_content_with_subsets(sql):
    sql.execute("SELECT nuclseq_content('ARRRARRR', 'A');")
    assert sql.fetchone() == (0.25,)

@test
def nuclseq_content_of_wildcard(sql):
    sql.execute("SELECT nuclseq_content('ARNNARNN', 'N');")
    assert sql.fetchone() == (0.5,)

@test
def nuclseq_content_of_subset(sql):
    sql.execute("SELECT nuclseq_content('ARRRARRR', 'R');")
    assert sql.fetchone() == (0.75,)

@test
def nuclseq_content_null_on_empty_sequence(sql):
    sql.execute("SELECT nuclseq_content('', 'A');")
    assert sql.fetchone() == (None,)

@test
def nuclseq_content_rejects_empty_needle(sql):
    failed = False
    try:
        sql.execute("SELECT nuclseq_content('ACGT', '');")
    except psycopg2.DataError as e:
        assert "invalid nucleotide in nuclseq_content: ''" in e.pgerror
        failed = True
    assert failed

# TODO: Test other invalid needles in nuclseq_content.

@test
def nuclseq_content_rejects_empty_needle_on_empty_sequence(sql):
    failed = False
    try:
        sql.execute("SELECT nuclseq_content('', '');")
    except psycopg2.DataError as e:
        assert "invalid nucleotide in nuclseq_content: ''" in e.pgerror
        failed = True
    assert failed

_conn.close()
sys.exit(_status)
