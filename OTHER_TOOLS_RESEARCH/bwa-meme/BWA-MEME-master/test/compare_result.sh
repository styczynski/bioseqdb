#!/bin/bash

make fmi_seeding_big_read
make learned_seeding_big_read

number=0
python ~/bwa-meme-python/make_query.py ~/bwa-meme-python/ref.fa;./fmi_seeding_big_read  ~/bwa-meme-python/ref.fa query_ref.fa 1000 1 4  > reference_result.txt; ./learned_seeding_big_read  ~/bwa-meme-python/ref.fa query_ref.fa 1000 1 4  > result_learned.txt;
DIFF=$(diff reference_result.txt result_learned.txt)
echo $DIFF
while [ "$DIFF" == "" ]
do
	python ~/bwa-meme-python/make_query.py ~/bwa-meme-python/ref.fa;./fmi_seeding_big_read  ~/bwa-meme-python/ref.fa query_ref.fa 1000 1 4  >reference_result.txt; ./learned_seeding_big_read  ~/bwa-meme-python/ref.fa query_ref.fa 1000 1 4  > result_learned.txt;
	DIFF=$(diff reference_result.txt result_learned.txt)
	echo ""
	echo ""
	echo ""
	echo "Iteration: ${number}"
	((number++))
done
