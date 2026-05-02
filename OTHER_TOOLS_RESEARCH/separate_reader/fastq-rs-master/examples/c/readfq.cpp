#include <fstream>
#include <iostream>

#include <seqan/seq_io.h>

using namespace seqan;

int main(int argc, char const ** argv)
{
    if (argc != 2)
        return 1;

    StringSet<CharString> ids;
    StringSet<CharString> seqs;

    SeqFileIn reader(argv[1]);

    while (!atEnd(reader))
    {
        readRecords(ids, seqs, reader, 5000);
    }

    return 0;
}
