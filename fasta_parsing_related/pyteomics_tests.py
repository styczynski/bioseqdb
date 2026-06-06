from os import path
import tempfile
import unittest
import random
import string
import pickle
import re
from collections import Counter
import pyteomics
pyteomics.__path__ = [path.abspath(path.join(path.dirname(__file__), path.pardir, 'pyteomics'))]
from pyteomics import fasta

class ReadWriteTest(unittest.TestCase):
    maxDiff = None
    def setUp(self):
        self.fasta_file = 'test.fasta'
        self.fasta_entries_long = [
            ('test sequence test sequence 2', 'TEST'),
                          ('test sequence 3', 'TEST'),
                          ('test sequence 4', 'TEST')
        ]
        self.fasta_entries_short = [
            ('test sequence',   'TEST'),
            ('test sequence 3', 'TEST'),
            ('test sequence 4', 'TEST')
        ]

    def test_simple_read_long_comments(self):
        for reader in [fasta.read, fasta.FASTA]:
            with reader(self.fasta_file) as f:
                self.assertEqual(self.fasta_entries_long, list(f))

    def test_simple_read_short_comments(self):
        for reader in [fasta.read, fasta.FASTA]:
            with reader(self.fasta_file, ignore_comments=True) as f:
                self.assertEqual(self.fasta_entries_short, list(f))

    def test_indexed_read(self):
        with fasta.TwoLayerIndexedFASTA(self.fasta_file) as tlir, fasta.IndexedFASTA(self.fasta_file) as ir:
            for reader in [ir, tlir]:
                self.assertEqual(self.fasta_entries_short[1:], list(reader))

    def test_index_retrieve(self):
        key = 'test sequence 4'
        with fasta.IndexedFASTA(self.fasta_file) as ir:
            self.assertEqual(self.fasta_entries_short[2], ir[key])

    def test_two_layer_retrieve(self):
        with fasta.TwoLayerIndexedFASTA(self.fasta_file, r'test sequence (.*)') as tlir:
            self.assertEqual(self.fasta_entries_short[2], tlir['4'])

    def test_indexed_picklable(self):
        reader = fasta.TwoLayerIndexedFASTA(self.fasta_file, r'test sequence (.*)', block_size=7777)
        reader2 = pickle.loads(pickle.dumps(reader))
        self.assertEqual(reader2.block_size, reader.block_size)
        self.assertEqual(self.fasta_entries_short[2], reader2['4'])
        reader.close()
        reader2.close()

    def test_mp_map(self):
        with fasta.IndexedFASTA(self.fasta_file) as ir:
            self.assertEqual(
                sorted(self.fasta_entries_short[1:]),
                sorted(list(ir.map())))

    def test_read_and_write_fasta_short(self):
        with tempfile.TemporaryFile(mode='r+') as new_fasta_file:
            fasta.write(fasta.read(self.fasta_file, ignore_comments=True),
                new_fasta_file)
            new_fasta_file.seek(0)
            new_entries = list(fasta.read(new_fasta_file, ignore_comments=True))
            self.assertEqual(new_entries, self.fasta_entries_short)

    def test_read_and_write_long(self):
        with tempfile.TemporaryFile(mode='r+') as new_fasta_file:
            fasta.write(fasta.read(self.fasta_file), new_fasta_file)
            new_fasta_file.seek(0)
            new_entries = list(fasta.read(new_fasta_file))
            self.assertEqual(new_entries, self.fasta_entries_long)

    def test_write_decoy_db(self):
        with tempfile.TemporaryFile(mode='r+') as decdb:
            fasta.write_decoy_db(self.fasta_file, decdb,
                    decoy_only=False, prefix='PREFIX_')
            decdb.seek(0)
            all_entries = list(fasta.read(decdb, False))
        self.assertEqual(all_entries, self.fasta_entries_long +
                [('PREFIX_' + a, b[::-1]) for a, b in self.fasta_entries_long])

    def test_decoy_entries(self):
        with fasta.read(self.fasta_file) as f:
            self.assertEqual(sorted(fasta.decoy_entries(f, decoy_only=False, prefix='PREFIX_', mode='reverse')),
                sorted(self.fasta_entries_long + [('PREFIX_' + a, b[::-1]) for a, b in self.fasta_entries_long]))

    def test_decoy_entries_only(self):
        with fasta.read(self.fasta_file) as f:
            self.assertEqual(list(fasta.decoy_entries(f, decoy_only=True, prefix='PREFIX_', mode='reverse')),
                [('PREFIX_' + a, b[::-1]) for a, b in self.fasta_entries_long])


class DecoyTest(unittest.TestCase):
    def test_decoy_sequence_reverse(self):
        sequence = ''.join(random.choice(string.ascii_uppercase)
                             for i in range(random.randint(1, 50)))
        self.assertEqual(fasta.decoy_sequence(sequence, 'reverse'), sequence[::-1])
        self.assertEqual(fasta.reverse(sequence), sequence[::-1])

    def test_decoy_sequence_shuffle(self):
        sequences = [''.join(random.choice(string.ascii_uppercase)
                             for i in range(random.randint(1, 50)))
                                for j in range(10)]
        test = True
        for s in sequences:
            ss = fasta.decoy_sequence(s, 'shuffle')
            self.assertEqual(sorted(list(s)), sorted(list(ss)))
            if not all(a == b for a, b in zip(s, ss)):
                test = False
        self.assertFalse(test)

        test = True
        for s in sequences:
            ss = fasta.shuffle(s)
            self.assertEqual(sorted(list(s)), sorted(list(ss)))
            if not all(a == b for a, b in zip(s, ss)):
                test = False
        self.assertFalse(test)

        test = True
        for s in sequences:
            n = random.randint(1, 5)
            fix_aa = [random.choice(string.ascii_uppercase) for _ in range(n)]
            ss = fasta.shuffle(s, fix_aa=fix_aa)
            self.assertEqual(len(s), len(ss))
            self.assertEqual(Counter(s), Counter(ss))
            for aa in fix_aa:
                self.assertEqual([_.span() for _ in re.finditer(aa, s)],
                                  [_.span() for _ in re.finditer(aa, ss)])
            if not all(a == b for a, b in zip(s, ss)):
                test = False
        self.assertFalse(test)

    def test_decoy_sequence_fused(self):
        sequences = [''.join(random.choice(string.ascii_uppercase)
                             for i in range(random.randint(1, 50)))
                                for j in range(10)]
        for s in sequences:
            ss = fasta.decoy_sequence(s, 'fused')
            self.assertEqual(ss, s[::-1] + 'R' + s)
            self.assertEqual(ss, fasta.fused_decoy(s))

    def test_decoy_keep_nterm(self):
        sequences = [''.join(random.choice(string.ascii_uppercase)
                             for i in range(random.randint(1, 50)))
                                for j in range(10)]
        for mode in ('shuffle', 'reverse'):
            for seq in sequences:
                self.assertEqual(seq[0], fasta.decoy_sequence(seq, mode, keep_nterm=True)[0])

        for seq in sequences:
            self.assertEqual(seq[1:][::-1], fasta.reverse(seq, keep_nterm=True)[1:])

    def test_decoy_keep_cterm(self):
        sequences = [''.join(random.choice(string.ascii_uppercase)
                             for i in range(random.randint(1, 50)))
                                for j in range(10)]
        for mode in ('shuffle', 'reverse'):
            for seq in sequences:
                self.assertEqual(seq[-1], fasta.decoy_sequence(seq, mode, keep_cterm=True)[-1])

        for seq in sequences:
            self.assertEqual(seq[:-1][::-1], fasta.reverse(seq, keep_cterm=True)[:-1])


class ParserTest(unittest.TestCase):
    def test_parser_uniprotkb_decoydb(self):
        header = ('sp|P27748|ACOX_RALEH Acetoin catabolism protein X OS=Ralstonia'
            ' eutropha (strain ATCC 17699 / H16 / DSM 428 / Stanier 337)'
            ' GN=acoX PE=4 SV=2')
        sequence = 'SEQUENCE'
        with tempfile.TemporaryFile(mode='r+') as db:
            fasta.write([(header, sequence)], db)
            db.seek(0)
            entries = list(fasta.decoy_db(db, prefix='PREFIX_', parser=fasta.parse, decoy_only=True))

        parsed = {'GN': 'acoX',
                 'OS': 'Ralstonia eutropha '
                    '(strain ATCC 17699 / H16 / DSM 428 / Stanier 337)',
                 'PE': 4,
                 'SV': 2,
                 'db': 'PREFIX_sp',
                 'entry': 'ACOX_RALEH',
                 'id': 'P27748',
                 'gene_id': 'ACOX',
                 'name': 'Acetoin catabolism protein X',
                 'taxon': 'RALEH',
                 fasta.RAW_HEADER_KEY: 'PREFIX_' + header}
        self.assertEqual(entries[0][0], parsed)
        self.assertEqual(entries[0][1], 'SEQUENCE'[::-1])
        self.assertEqual(len(entries), 1)

    def test_parser_uniprotkb(self):
        header = ('sp|P27748|ACOX_RALEH Acetoin catabolism protein X OS=Ralstonia'
            ' eutropha (strain ATCC 17699 / H16 / DSM 428 / Stanier 337)'
            ' GN=acoX PE=4 SV=2')
        parsed = {'GN': 'acoX',
                 'OS': 'Ralstonia eutropha '
                    '(strain ATCC 17699 / H16 / DSM 428 / Stanier 337)',
                 'PE': 4,
                 'SV': 2,
                 'db': 'sp',
                 'entry': 'ACOX_RALEH',
                 'id': 'P27748',
                 'gene_id': 'ACOX',
                 'name': 'Acetoin catabolism protein X',
                 'taxon': 'RALEH',
                 fasta.RAW_HEADER_KEY: header}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_uniprotkb_write(self):
        header = ('sp|P27748|ACOX_RALEH Acetoin catabolism protein X OS=Ralstonia'
            ' eutropha (strain ATCC 17699 / H16 / DSM 428 / Stanier 337)'
            ' GN=acoX PE=4 SV=2')
        parsed = {'GN': 'acoX',
                 'OS': 'Ralstonia eutropha '
                    '(strain ATCC 17699 / H16 / DSM 428 / Stanier 337)',
                 'PE': 4,
                 'SV': 2,
                 'db': 'sp',
                 'entry': 'ACOX_RALEH',
                 'id': 'P27748',
                 'gene_id': 'ACOX',
                 'name': 'Acetoin catabolism protein X',
                 'taxon': 'RALEH',
                 fasta.RAW_HEADER_KEY: header}
        with tempfile.TemporaryFile(mode='r+') as new_fasta_file:
            fasta.write([(parsed, 'SEQUENCE')], new_fasta_file)
            new_fasta_file.seek(0)
            new_entries = list(fasta.read(new_fasta_file))
            self.assertEqual([(header, 'SEQUENCE')], new_entries)

    def test_parser_uniprotkb_isoform(self):
        header = 'sp|Q4R572-2|1433B_MACFA Isoform Short of 14-3-3 protein beta/alpha OS=Macaca fascicularis GN=YWHAB'
        parsed = {'GN': 'YWHAB',
                 'OS': 'Macaca fascicularis',
                 'db': 'sp',
                 'entry': '1433B_MACFA',
                 'gene_id': '1433B',
                 'id': 'Q4R572-2',
                 'name': 'Isoform Short of 14-3-3 protein beta/alpha',
                 'taxon': 'MACFA',
                 fasta.RAW_HEADER_KEY: header}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_uniprot_equals(self):
        header = 'tr|Q9S8M8|Q9S8M8_WHEAT FRIII-2-VIII=GAMMA-gliadin (Fragment) OS=Triticum aestivum OX=4565 PE=1 SV=1'
        parsed = {
            'db': 'tr',
            'id': 'Q9S8M8',
            'entry': 'Q9S8M8_WHEAT',
            'taxon': 'WHEAT',
            'gene_id': 'Q9S8M8',
            'name': 'FRIII-2-VIII=GAMMA-gliadin (Fragment)',
            'OS': 'Triticum aestivum',
            'OX': 4565,
            'PE': 1,
            'SV': 1,
            fasta.RAW_HEADER_KEY: header
        }
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_uniprot_hyphen(self):
        header = 'tr|Q00M55|Q00M55_WHEAT LMW-GS P-32 OS=Triticum aestivum OX=4565 GN=GluD3-3 PE=4 SV=1'
        parsed = {
            'db': 'tr',
            'id': 'Q00M55',
            'gene_id': 'Q00M55',
            'taxon': 'WHEAT',
            'entry': 'Q00M55_WHEAT',
            'name': 'LMW-GS P-32',
            'OS': 'Triticum aestivum',
            'OX': 4565,
            'GN': 'GluD3-3',
            'PE': 4,
            'SV': 1,
            fasta.RAW_HEADER_KEY: header
        }
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_uniref(self):
        header = ('>UniRef100_A5DI11 Elongation factor 2 n=1 '
                'Tax=Pichia guilliermondii RepID=EF2_PICGU')
        parsed = {'RepID': 'EF2_PICGU',
                 # 'taxon': 'PICGU',
                 # 'gene_id': 'EF2',
                 'Tax': 'Pichia guilliermondii',
                 'cluster': 'Elongation factor 2',
                 'id': 'UniRef100_A5DI11',
                 # 'type': 'UniRef100',
                 # 'accession': 'A5DI11',
                 'n': 1,
                 fasta.RAW_HEADER_KEY: header[1:]}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_uniparc(self):
        header = '>UPI0000000005 status=active'
        parsed = {'id': 'UPI0000000005',
                  'status': 'active',
                  fasta.RAW_HEADER_KEY: header[1:]}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_unimes(self):
        header = ('MES00000000005 Putative uncharacterized protein GOS_3018412 '
            '(Fragment) OS=marine metagenome Pep=JCVI_PEP_1096688850003 SV=1')
        parsed = {'OS': 'marine metagenome',
                 'Pep': 'JCVI_PEP_1096688850003',
                 'SV': 1,
                 'id': 'MES00000000005',
                 'name': 'Putative uncharacterized protein GOS_3018412 (Fragment)',
                 fasta.RAW_HEADER_KEY: header}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_spd(self):
        header = ('>P31947|1433S_HUMAN| 14-3-3 protein sigma (Stratifin) '
            '(Epithelial cell marker protein 1).')
        parsed = {'description': '14-3-3 protein sigma (Stratifin) '
                '(Epithelial cell marker protein 1).',
                 'gene': '1433S_HUMAN',
                 'gene_id': '1433S',
                 'id': 'P31947',
                 'taxon': 'HUMAN',
                 fasta.RAW_HEADER_KEY: header[1:]}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_spd_mult_ids(self):
        header = ('>P02763 Q8TC16|A1AG1_HUMAN| Alpha-1-acid glycoprotein 1 '
            'precursor (AGP 1) (Orosomucoid-1) (OMD 1)')
        parsed = {'description': 'Alpha-1-acid glycoprotein 1 precursor (AGP 1)'
                ' (Orosomucoid-1) (OMD 1)',
                 'gene': 'A1AG1_HUMAN',
                 'gene_id': 'A1AG1',
                 'id': 'P02763 Q8TC16',
                 'taxon': 'HUMAN',
                 fasta.RAW_HEADER_KEY: header[1:]}
        self.assertEqual(fasta.parse(header), parsed)

    def test_parser_ncbi(self):
        header = '>NP_001351877.1 acylglycerol kinase, mitochondrial isoform 2 [Homo sapiens]'
        parsed = {'description': 'acylglycerol kinase, mitochondrial isoform 2',
                 'id': 'NP_001351877.1',
                 'taxon': 'Homo sapiens',
                 fasta.RAW_HEADER_KEY: header[1:]}
        self.assertEqual(fasta.parse(header), parsed)


if __name__ == '__main__':
    unittest.main()