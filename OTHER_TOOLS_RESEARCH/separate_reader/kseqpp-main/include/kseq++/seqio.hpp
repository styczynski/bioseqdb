/**
 *    @file  seqio.hpp
 *   @brief  SeqStream class definition
 *
 *  The `SeqStream` class introduces one level of abstraction to the KStream to hide
 *  some implementation details and provide simpler API for working with sequence files.
 *
 *  @author  Ali Ghaffaari (\@cartoonist), <ali.ghaffaari@mpi-inf.mpg.de>
 *
 *  @internal
 *       Created:  Sun Aug 19, 2018  12:39
 *  Organization:  Max-Planck-Institut fuer Informatik
 *     Copyright:  Copyright (c) 2018, Ali Ghaffaari
 *
 *  This source code is released under the terms of the MIT License.
 *  See LICENSE file for more information.
 */

#ifndef  KSEQPP_SEQIO_HPP__
#define  KSEQPP_SEQIO_HPP__

#include <zlib.h>

#include "kseq++.hpp"

namespace klibpp {
  class SeqStreamIn
    : public KStreamIn< gzFile, int(*)(gzFile_s*, void*, unsigned int) > {
    public:
      /* Typedefs */
      typedef KStreamIn< gzFile, int(*)(gzFile_s*, void*, unsigned int) > base_type;
      /* Lifecycle */
      SeqStreamIn( const char* filename )
        : base_type( gzopen( filename, "r" ), gzread, gzclose )
      { }

      SeqStreamIn( int fd )
        : base_type( gzdopen( fd, "r" ), gzread, gzclose )
      { }
  };

  class SeqStreamOut
    : public KStreamOut< gzFile, int(*)(gzFile_s*, const void*, unsigned int) > {
    public:
      /* Typedefs */
      typedef KStreamOut< gzFile, int(*)(gzFile_s*, const void*, unsigned int) > base_type;
      /* Lifecycle */
      SeqStreamOut( const char* filename, bool compressed=false,
                    format::Format fmt=base_type::DEFAULT_FORMAT )
        : base_type( gzopen( filename, ( compressed ? "w" : "wT" ) ), gzwrite, fmt, gzclose )
      { }

      SeqStreamOut( int fd, bool compressed=false,
                    format::Format fmt=base_type::DEFAULT_FORMAT )
        : base_type( gzdopen( fd, ( compressed ? "w" : "wT" ) ), gzwrite, fmt, gzclose )
      { }

      SeqStreamOut( const char* filename, format::Format fmt )
        : base_type( gzopen( filename, "wT" ), gzwrite, fmt, gzclose )
      { }

      SeqStreamOut( int fd, format::Format fmt )
        : base_type( gzdopen( fd, "wT" ), gzwrite, fmt, gzclose )
      { }
  };
}  /* -----  end of namespace klibpp  ----- */
#endif  /* ----- #ifndef KSEQPP_SEQIO_HPP__  ----- */
