/**
 *    @file  kseq++_bench.cc
 *   @brief  Benchmark for kseq++.
 *
 *  Benchmark for kseq++ header file.
 *
 *  @author  Ali Ghaffaari (\@cartoonist), <ali.ghaffaari@mpi-inf.mpg.de>
 *
 *  @internal
 *       Created:  Tue Jul 17, 2018  23:02
 *  Organization:  Max-Planck-Institut fuer Informatik
 *     Copyright:  Copyright (c) 2018, Ali Ghaffaari
 *
 *  This source code is released under the terms of the MIT License.
 *  See LICENSE file for more information.
 */

#include <zlib.h>
#include <iostream>
#include <ios>
#include <iomanip>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>
#include <fcntl.h>

#include <kseq++/kseq++.hpp>
#include "kseq.h"
#include "seqan/seq_io.h"

KSEQ_INIT(gzFile, gzread)

#define SMALL_BUF_SIZE 4096
#define BIG_BUF_SIZE 65536


using namespace klibpp;

  int
main( int argc, char* argv[] )
{
  if ( argc < 3 ) {
    std::cerr << "Usage: " << argv[0] << " INPUT OUTPUT" << std::endl;
    return EXIT_FAILURE;
  }

  gzFile fp;
  clock_t t;
  std::cerr << "=== READ TESTS ===" << std::endl;
  {
    std::string buf;
    buf.resize( SMALL_BUF_SIZE );
    fp = gzopen( argv[1], "r" );
    t = clock();
    while ( gzread( fp, &buf[0], SMALL_BUF_SIZE ) > 0 );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[gzread] " << std::setprecision(3) << d << " sec" << std::endl;
    gzclose( fp );
  }
  {
    fp = gzopen( argv[1], "r" );
    auto ks = make_ikstream( fp, gzread, SMALL_BUF_SIZE );
    t = clock();
    while ( ks.getc( ) );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[ks_getc] " << std::setprecision(3) << d << " sec" << std::endl;
    gzclose( fp );
  }
  {
    fp = gzopen( argv[1], "r" );
    auto ks = make_kstream( fp, gzread, mode::in, SMALL_BUF_SIZE );
    std::string s;
    char dret;
    t = clock();
    while ( ks.getuntil( '\n', s, &dret ) );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[ks_getuntil] " << std::setprecision(3) << d << " sec" << std::endl;
    gzclose( fp );
  }
  {
    fp = gzopen( argv[1], "r" );
    t = clock();
    while ( gzgetc( fp ) >= 0 );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[gzgetc] " << std::setprecision(3) << d << " sec" << std::endl;
    gzclose( fp );
  }
  {
    fp = gzopen( argv[1], "r" );
    std::string buf;
    buf.resize( SMALL_BUF_SIZE );
    t = clock();
    while ( gzgets( fp, &buf[0], SMALL_BUF_SIZE ) != Z_NULL );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[gzgets] " << std::setprecision(3) << d << " sec" << std::endl;
    gzclose( fp );
  }
  {
    FILE *fp;
    std::string s;
    t = clock();
    s.resize( BIG_BUF_SIZE );
    fp = fopen( argv[1], "r" );
    while ( fgets( &s[0], BIG_BUF_SIZE, fp ) );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[fgets] " << std::setprecision(3) << d << " sec" << std::endl;
    fclose( fp );
  }
  {
    int fd;
    char dret;
    std::string s;
    t = clock();
    fd = open( argv[1], O_RDONLY );
    auto ks = make_ikstream( fd, read, BIG_BUF_SIZE );
    while ( ks.getuntil( '\n', s, &dret ) );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[kstream] " << std::setprecision(3) << d << " sec" << std::endl;
    close( fd );
  }
  {
    seqan2::SeqFileIn f;
    seqan2::CharString name;
    seqan2::CharString str;
    seqan2::CharString qual;
    open( f, argv[1] );
    t = clock();
    while ( !atEnd( f ) ) readRecord( name, str, qual, f );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[seqan] " << std::setprecision(3) << d << " sec" << std::endl;
    close( f );
  }
  {
    kseq_t *seq;
    int l;
    fp = gzopen( argv[1], "r" );
    seq = kseq_init( fp );
    t = clock();
    while ( ( l = kseq_read( seq ) ) >= 0 );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[kseq] " << std::setprecision(3) << d << " sec" << std::endl;
    kseq_destroy(seq);
    gzclose( fp );
  }
  {
    KSeq record;
    int fd = open( argv[1], O_RDONLY );
    auto ks = make_ikstream( fd, read );
    t = clock();
    while ( ks >> record );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[kseq++] " << std::setprecision(3) << d << " sec" << std::endl;
    close( fd );
  }
  {
    std::ifstream ifs( argv[1], std::ifstream::in );
    KSeq record;
    char buf[1000];
    unsigned long int count = 0;
    t = clock();
    while ( ifs.peek() != EOF ) {
      while ( std::isspace( ifs.peek() ) ) ifs.get();
      char c = ifs.get( );
      if ( c != '>' && c != '@' ) break;
      ifs >> record.name;
      if ( ifs.peek() == ' ' ) ifs >> record.comment;
      while ( ifs.peek() != EOF && ifs.peek() != '>' && ifs.peek() != '@' && ifs.peek() != '+' ) {
        if ( std::isspace( ifs.peek() ) ) {
          ifs.get();
          continue;
        }
        ifs.getline( buf, 1000 );
        record.seq += buf;
      }
      while ( std::isspace( ifs.peek() ) ) ifs.get();
      if ( ifs.peek() == '+' ) ifs.get();
      while ( ifs.peek() != EOF && ifs.peek() != '>' && ifs.peek() != '@' ) {
        if ( std::isspace( ifs.peek() ) ) {
          ifs.get();
          continue;
        }
        ifs.getline( buf, 1000 );
        record.qual += buf;
      }
      assert( record.qual.size() == record.seq.size() );
      ++count;
      record.clear();
    }
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[ifstream] " << std::setprecision(3) << d << " sec" << std::endl;
  }
  std::vector< KSeq > records;
  {
    fp = gzopen( argv[1], "r" );
    auto iks = make_kstream( fp, gzread, mode::in );
    t = clock();
    records = iks.read( );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[kseq++/read_all] " << std::setprecision(3) << d << " sec" << std::endl;
    gzclose( fp );
  }
  seqan2::StringSet< seqan2::CharString > names;
  seqan2::StringSet< seqan2::CharString > seqs;
  seqan2::StringSet< seqan2::CharString > quals;
  {
    seqan2::SeqFileIn i_file;
    open( i_file, argv[1] );
    t = clock();
    readRecords( names, seqs, quals, i_file );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[seqan/readRecords] " << std::setprecision(3) << d << " sec" << std::endl;
    close( i_file );
  }
  std::cerr << "=== WRITE TESTS ===" << std::endl;
  {
    seqan2::SeqFileOut o_file;
    open( o_file, argv[2], seqan2::FileOpenMode::OPEN_WRONLY );
    t = clock();
    writeRecords( o_file, names, seqs, quals );
    close( o_file );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[seqan] " << std::setprecision(3) << d << " sec" << std::endl;
  }
  {
    int fd = open( argv[2], O_CREAT | O_WRONLY );
    auto oks = make_okstream( fd, write );
    t = clock();
    for ( const auto& r : records ) oks << r;
    oks << kend;
    close( fd );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[kseq++] " << std::setprecision(3) << d << " sec" << std::endl;
  }
  {
    fp = gzopen( argv[2], "w" );
    auto oks = make_okstream( fp, gzwrite );
    t = clock();
    for ( const auto& r : records ) oks << r;
    oks << kend;
    gzclose( fp );
    auto d = static_cast< float >( clock() - t ) / CLOCKS_PER_SEC;
    std::cerr << "[kseq++/gz] " << std::setprecision(3) << d << " sec" << std::endl;
  }

  return EXIT_SUCCESS;
}
