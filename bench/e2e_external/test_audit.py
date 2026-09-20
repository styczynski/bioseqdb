"""Negative controls for the publication-critical occurrence/interval audit."""
import unittest
import importlib.util
from pathlib import Path
import tempfile
import json
from campaign import validate, interval_union, read_fasta


class AuditTests(unittest.TestCase):
    def setUp(self):
        self.truth=[{'start':100,'end':104,'reverse':False,'window_start':100,'window':'ACGT'}]
        self.queries={'q0':'ACGT'}
        self.row={'query':0,'target':'ref_part0','qfirst':1,'qlast':4,
                  'tfirst':101,'tlast':104,'qaln':'ACGT','taln':'ACGT','mapq':42}

    def check(self,rows):
        return validate(rows,self.truth,self.queries,1,True)

    def test_valid_occurrence(self):
        self.assertTrue(self.check([self.row])['valid'])

    def test_empty_rejected(self):
        with self.assertRaises(ValueError): self.check([])

    def test_duplicate_rejected(self):
        with self.assertRaises(ValueError): self.check([self.row,self.row])

    def test_shifted_occurrence_rejected(self):
        with self.assertRaises(ValueError): self.check([{**self.row,'tfirst':117,'tlast':120}])

    def test_wrong_strand_rejected(self):
        with self.assertRaises(ValueError): self.check([{**self.row,'tfirst':104,'tlast':101}])

    def test_incorrect_gapped_sequence_rejected(self):
        with self.assertRaises(ValueError): self.check([{**self.row,'taln':'AAAA'}])

    def test_mapq_absence_rejected(self):
        with self.assertRaises(ValueError): self.check([{**self.row,'mapq':None}])

    def test_overlapping_regions_union_once(self):
        self.assertEqual(interval_union([(1,10),(3,6),(8,13),(20,25)]),17)

    def test_cigar_nm_must_match_bases(self):
        row={**self.row,'qaln':None,'taln':None,'cigar':'4M','edit_distance':0}
        self.assertTrue(self.check([row])['valid'])
        with self.assertRaises(ValueError):self.check([{**row,'edit_distance':1}])

    def test_cigar_cannot_hide_mismatches(self):
        self.queries={'q0':'AGGT'}
        row={**self.row,'qaln':None,'taln':None,'cigar':'4M','edit_distance':0}
        with self.assertRaises(ValueError):self.check([row])
        self.assertTrue(self.check([{**row,'edit_distance':1}])['valid'])

    def test_cigar_ambiguous_bases_follow_native_nm(self):
        self.queries={'q0':'ACNT'};self.truth[0]['window']='ACNT'
        row={**self.row,'qaln':None,'taln':None,'cigar':'4M','edit_distance':1}
        self.assertTrue(self.check([row])['valid'])
        with self.assertRaises(ValueError):self.check([{**row,'edit_distance':0}])

    def test_lexicmap_reference_encoding_is_narrow(self):
        self.truth[0]['window']='ACNT';self.queries={'q0':'ACNT'}
        row={**self.row,'qaln':'ACNT','taln':'ACAT','reference_encoding':'lexicmap_two_bit'}
        self.assertEqual(self.check([row])['native_encoded_reference_bases_checked'],1)
        with self.assertRaises(ValueError):self.check([{**row,'taln':'AAAT'}])
        with self.assertRaises(ValueError):self.check([{**row,'qaln':'ACAT'}])
        with self.assertRaises(ValueError):self.check([{**row,'reference_encoding':None}])

    def test_lexicmap_encoding_precedes_reverse_complement(self):
        self.truth[0].update({'window':'ACNT','reverse':True});self.queries={'q0':'ANGT'}
        row={**self.row,'tfirst':104,'tlast':101,'qaln':'ANGT','taln':'ATGT',
             'reference_encoding':'lexicmap_two_bit'}
        self.assertEqual(self.check([row])['native_encoded_reference_bases_checked'],1)
        with self.assertRaises(ValueError):self.check([{**row,'taln':'AAGT'}])

    def test_unknown_reference_encoding_rejected(self):
        with self.assertRaises(ValueError):self.check([{**self.row,'reference_encoding':'unchecked'}])

    def test_review_cache_rechecks_evidence_bytes(self):
        from publish import save_review_cache, load_review_cache, digest
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);evidence=root/'intervals.tsv';evidence.write_text('original intervals')
            cache=root/'review.json';signature={'audit':'v1','report':'original'}
            save_review_cache(cache,signature,{'accepted':True},{'intervals.tsv':digest(evidence)})
            self.assertEqual(load_review_cache(cache,signature,root),{'accepted':True})
            evidence.write_text('different intervals')
            self.assertIsNone(load_review_cache(cache,signature,root))

    def test_review_cache_invalidated_by_audit_or_report_changes(self):
        from publish import save_review_cache, load_review_cache
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);cache=root/'review.json';signature={'audit':'v1','report':'original'}
            save_review_cache(cache,signature,{'accepted':True},{})
            self.assertIsNone(load_review_cache(cache,{**signature,'audit':'v2'},root))
            self.assertIsNone(load_review_cache(cache,{**signature,'report':'different'},root))

    def test_review_cache_rejects_modified_result(self):
        from publish import save_review_cache, load_review_cache
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);cache=root/'review.json';signature={'audit':'v1'}
            save_review_cache(cache,signature,{'p50_a':1000},{})
            value=json.loads(cache.read_text());value['row']['p50_a']=1;cache.write_text(json.dumps(value))
            self.assertIsNone(load_review_cache(cache,signature,root))

    def test_partition_preserves_boundary_crossing_occurrences(self):
        spec=importlib.util.spec_from_file_location('lexic_adapter',Path(__file__).resolve().parents[1]/'tool_lexicmap/adapter.py')
        adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
        with tempfile.TemporaryDirectory() as directory:
            source=Path(directory)/'source.fasta';target=Path(directory)/'target.fasta'
            sequence='ACGTNAGTCCTAGGCTATNCAGCT'
            source.write_text('>a\n'+sequence[:12]+'\n>b\n'+sequence[12:]+'\n')
            adapter.partition_reference(source,target,10,4)
            records=read_fasta(target)
            for name,record in records.items():
                offset=int(name.removeprefix('ref_offset'))
                self.assertEqual(record,sequence[offset:offset+len(record)])
                self.assertLessEqual(len(record),10)
            for start in range(len(sequence)-4+1):
                self.assertTrue(any(int(name.removeprefix('ref_offset'))<=start and
                    start+4<=int(name.removeprefix('ref_offset'))+len(record) for name,record in records.items()))

    def test_mmseqs_groups_cover_queries_without_internal_overlap(self):
        spec=importlib.util.spec_from_file_location('mmseqs_adapter',Path(__file__).resolve().parents[1]/'tool_mmseqs2/adapter.py')
        adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'source.fasta';sequence='ACGTNAGTCCTAGGCTATNCAGCT'
            source.write_text('>a\n'+sequence[:12]+'\n>b\n'+sequence[12:]+'\n')
            layout=adapter.partition_reference(source,root,10,4);intervals=[]
            for group in range(layout['groups']):
                previous_end=0
                for name,record in read_fasta(root/f'group{group}'/'reference.fasta').items():
                    start=int(name.removeprefix('ref_offset'));end=start+len(record)
                    self.assertGreaterEqual(start,previous_end);previous_end=end
                    self.assertEqual(record,sequence[start:end]);intervals.append((start,end))
            for start in range(len(sequence)-4+1):
                self.assertTrue(any(begin<=start and start+4<=end for begin,end in intervals))

    def test_mmseqs_additional_index_groups_preserve_records_and_occurrences(self):
        spec=importlib.util.spec_from_file_location('mmseqs_adapter',Path(__file__).resolve().parents[1]/'tool_mmseqs2/adapter.py')
        adapter=importlib.util.module_from_spec(spec);spec.loader.exec_module(adapter)
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);source=root/'source.fasta';sequence='ACGTNAGTCCTAGGCTATNCAGCT'*7
            source.write_text('>a\n'+sequence[:79]+'\n>b\n'+sequence[79:]+'\n')
            records_by_multiplier=[]
            for multiplier in [1,2]:
                target=root/str(multiplier);target.mkdir()
                layout=adapter.partition_reference(source,target,17,11,multiplier)
                self.assertEqual(layout['groups'],3*multiplier)
                records={}
                for group in range(layout['groups']):
                    previous_end=0
                    for name,record in read_fasta(target/f'group{group}'/'reference.fasta').items():
                        start=int(name.removeprefix('ref_offset'));end=start+len(record)
                        self.assertGreaterEqual(start,previous_end);previous_end=end
                        self.assertNotIn(name,records);records[name]=record
                        self.assertEqual(record,sequence[start:end])
                for start in range(len(sequence)-11+1):
                    self.assertTrue(any(int(name.removeprefix('ref_offset'))<=start and
                        start+11<=int(name.removeprefix('ref_offset'))+len(record) for name,record in records.items()))
                records_by_multiplier.append(records)
            self.assertEqual(*records_by_multiplier)


if __name__=='__main__': unittest.main()
