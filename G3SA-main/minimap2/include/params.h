#ifndef __PARAMS_H__
#define __PARAMS_H__

#pragma once
#include <cstdint>  

struct Params {
  int   batch_size;
  uint64_t   max_seq_len;
}; // tmp 

extern __constant__ Params d_params;

#endif