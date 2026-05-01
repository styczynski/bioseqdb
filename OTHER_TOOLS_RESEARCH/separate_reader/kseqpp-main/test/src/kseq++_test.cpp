/**
 *    @file  kseq++_test.cc
 *   @brief  Test cases for kseq++.
 *
 *  Test cases for kseq++ header file.
 *
 *  @author  Ali Ghaffaari (\@cartoonist), <ali.ghaffaari@mpi-inf.mpg.de>
 *
 *  @internal
 *       Created:  Tue Jul 17, 2018  19:48
 *  Organization:  Max-Planck-Institut fuer Informatik
 *     Copyright:  Copyright (c) 2018, Ali Ghaffaari
 *
 *  This source code is released under the terms of the MIT License.
 *  See LICENSE file for more information.
 */

#include <zlib.h>
#include <iostream>
#include <iomanip>
#include <string>
#include <fcntl.h>

#include <kseq++/kseq++.hpp>
#include "kseq.h"

#define SEQ_TRUNC_LEN 20
#define MAX_SHOWN_REC 10
#define SEQ_WRAPLEN 100
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
print_trunc( std::string prefix, const std::string& seq )
{
  std::cout << prefix << std::left
            << std::setw( SEQ_TRUNC_LEN + ( seq.size() <= SEQ_TRUNC_LEN ? 3 : 0 ) )
            << seq.substr( 0, SEQ_TRUNC_LEN )
            << ( seq.size() <= SEQ_TRUNC_LEN ? " " : "... " )
            << "(length=" << seq.size() << ")" << std::endl;
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

  gzFile ifp = gzopen( argv[1], "r" );
  int ofd = open( tmpfile.c_str(), O_CREAT | O_WRONLY );
  auto iks = make_kstream( ifp, gzread, mode::in );
  auto oks = make_kstream( ofd, write, mode::out );
  oks.set_wraplen( SEQ_WRAPLEN );
  KSeq record;
  size_t count = 0;
  size_t total_len = 0;
  size_t max_len = 0;
  size_t min_len = 0;
  --min_len;  // equals to the largest possible integer.
  while ( iks >> record ) {
    oks << record;
    total_len += record.seq.size();
    if ( record.seq.size() < min_len ) min_len = record.seq.size();
    if ( record.seq.size() > max_len ) max_len = record.seq.size();
    if ( ++count > MAX_SHOWN_REC ) {
      std::cout << "\r... and " << count - MAX_SHOWN_REC << " other records\r";
      continue;
    }
    std::cout << "Record " << count << ": " << record.name;
    if ( ! record.comment.empty() ) std::cout << " [" << record.comment << "]";
    std::cout << '\n';
    print_trunc( "  seq:  ", record.seq );
    if ( ! record.qual.empty() ) print_trunc( "  qual: ", record.seq );
  }
  assert( count == iks.counts() );
  if ( count > MAX_SHOWN_REC ) {
    std::cout << "... and " << count - MAX_SHOWN_REC << " other records." << '\n';
  }
  std::cout << "total length: " << total_len << '\n'
            << "minimum length: " << min_len << '\n'
            << "maximum length: " << max_len << '\n'
            << "average length: " << std::fixed << std::setprecision( 1 )
            << total_len / static_cast< double >( count ) << std::endl;
  oks << kend;  // flush the buffer before closing the file.
  close(ofd);
  gzclose(ifp);
  std::cout << "Verifying..." << std::endl;
  check( argv[1], count, total_len, min_len, max_len );
  std::cout << "PASSED" << std::endl;

  return EXIT_SUCCESS;
}
