# Benchmarks

The experiments here compare BioseqDB with minimap2 and test changes to the alignment pipeline, from small sequence pairs to searches against references containing billions of bases. Some tests focus on prefilters. These try to reject pairs that need too many edits before running alignment, and have separate checks for missed matches.

The alignment tests generate their inputs. Query fragments come from a  reference and receive substitutions and gaps, while their original positions are saved so we can check the matches returned by each algorithm. Fixed seeds make the inputs repeatable. Separate prefilter cases use sequence pairs supplied with SneakySnake.

The SQL benchmarks lond sequences into PostgreSQL and search them with mmsearch. One mode reads a previously built index, while another builds it during the timed search so we can measure the cost of finding matches when stored minimizers are unavailable. The, same experiments compare CPU and Vulkan implementations.

bench_framework takes care of repeating each run and  writing its report, using callbacks from this folder to prepare the data and check the algorithm's results. JSON settings choose sequence sizes and error rates, along with algorithm combinations and how many queries to submit together Scripts split large experiments into smaller jobs and combine the checked results afterwards.
