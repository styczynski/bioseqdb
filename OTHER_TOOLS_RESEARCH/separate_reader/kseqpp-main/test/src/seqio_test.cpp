/**
 *    @file  seqio_test.cc
 *   @brief  Test for seqio.h header file
 *
 *  Test cases for `seqio.h` header file.
 *
 *  @author  Ali Ghaffaari (\@cartoonist), <ali.ghaffaari@mpi-inf.mpg.de>
 *
 *  @internal
 *       Created:  Sun Aug 19, 2018  18:26
 *  Organization:  Max-Planck-Institut fuer Informatik
 *     Copyright:  Copyright (c) 2018, Ali Ghaffaari
 *
 *  This source code is released under the terms of the MIT License.
 *  See LICENSE file for more information.
 */

#include <cstdlib>
#include <iostream>
#include <string>

#include <kseq++/seqio.hpp>
#include "kseq.h"

#define DEFAULT_TMPDIR "/tmp"
#define TMPFILE_TEMPLATE "/kseqpp-XXXXXX"


using namespace klibpp;

KSEQ_INIT(gzFile, gzread)

  inline std::string
get_env( const std::string& var )
{
  const char* val = ::getenv( var.c_str() );
  if ( val == 0 ) {
    return "";
  }
  else {
    return val;
  }
}

  inline std::string
get_tmpdir_env( )
{
  return get_env( "TMPDIR" );
}

  inline std::string
get_tmpdir( )
{
  std::string tmpdir = get_tmpdir_env();
  if ( tmpdir.size() == 0 ) tmpdir = DEFAULT_TMPDIR;
  return tmpdir;
}

  inline std::string
get_tmpfile( )
{
  std::string tmpfile_templ = get_tmpdir() + TMPFILE_TEMPLATE;
  char* tmpl = new char [ tmpfile_templ.size() + 1 ];
  std::strcpy( tmpl, tmpfile_templ.c_str() );
  int fd = mkstemp( tmpl );
  tmpfile_templ = tmpl;

  ::close( fd );
  delete[] tmpl;
  return tmpfile_templ;
}

  void
check( const char* filename, size_t nrec, size_t tot, size_t min, size_t max )
{
  gzFile fp;
  kseq_t *seq;
  int l;
  fp = gzopen( filename, "r" );
  seq = kseq_init( fp );
  size_t count = 0;
  size_t total_len = 0;
  size_t max_len = 0;
  size_t min_len = 0;
  --min_len;  // equals to the largest possible integer.
  while ( ( l = kseq_read( seq ) ) >= 0 ) {
    total_len += seq->seq.l;
    if ( seq->seq.l < min_len ) min_len = seq->seq.l;
    if ( seq->seq.l > max_len ) max_len = seq->seq.l;
    ++count;
  }
  assert( l == -1 );
  assert( count == nrec );
  assert( total_len == tot );
  assert( min_len == min );
  assert( max_len == max );
  kseq_destroy(seq);
  gzclose(fp);
}

  int
main( int argc, char* argv[] )
{
  if ( argc == 1 ) {
    std::cerr << "Usage: " << argv[0] << " FILE" << std::endl;
    return EXIT_FAILURE;
  }

  // Using a file in TMPDIR for writing output.
  std::string tmpfile = get_tmpfile();
  std::cout << "Output temporary file: " << tmpfile << std::endl;

  bool compressed = false;
  for ( auto i = 0; i < 2; ++i ) {
    KSeq record;
    {
      SeqStreamIn iss( argv[1] );
      SeqStreamOut oss( tmpfile.c_str(), compressed );
      while ( iss >> record ) oss << record;
    }

    SeqStreamIn iss( tmpfile.c_str() );
    size_t count = 0;
    size_t total_len = 0;
    size_t max_len = 0;
    size_t min_len = 0;
    --min_len;  // equals to the largest possible integer.
    while( iss >> record ) {
      total_len += record.seq.length();
      if ( record.seq.size() < min_len ) min_len = record.seq.size();
      if ( record.seq.size() > max_len ) max_len = record.seq.size();
      ++count;
    }
    std::cout << "Verifying..." << std::endl;
    check( argv[1], count, total_len, min_len, max_len );
    std::cout << "PASSED" << std::endl;

    compressed = !compressed;
  }

  return EXIT_SUCCESS;
}
