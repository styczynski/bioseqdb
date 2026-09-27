# Papers

The BioseqDB thesis lives here. It explains how sequence alignment becomes part of a PostgreSQL query, starting with biological sequences and the search algorithms, then following the work  through the planner and sequence storage. Figures and pseudocode accompany the algorithm descriptions. Chapters on minimizers and chaining show how the search finds candidate regions before it spends time aligning individual bases.

The appendices include SQL examples. You can follow them from creating a table and loading FASTA daa to searching sequences, inspecting the query plan and choosing alignment options for one call. Saved plans sit beside the queries. Benchmark tables are generated from recorded measurements, and a separate analysis script compares runs with matching settings so changes in an algorithm can be discussed with the  other choices held fixed.

CMake builds PDFs with LuaLaTeX and Biber. Python helpers assemble the document settings and included chapters before compilation, and the same build setup covers BioseqDB slides and earlier presentations about Python sequence alignment and classification. The benchmark tables need a separate generation step. Rebuilding a document uses the saved results without running the benchmarks again
