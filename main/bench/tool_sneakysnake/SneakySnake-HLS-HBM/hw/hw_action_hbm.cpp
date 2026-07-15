/*
 * Copyright 2019 International Business Machines
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* SNAP HLS_MEMCOPY EXAMPLE */


#include "ap_int.h"
#include "hw_action_hbm.H"
#include "fetch_mem.h"
#include "pipeline.h"

#ifndef __SYNTHESIS__
#include <string>
using std::string;  
#endif
// #include "xf_fintech/enums.hpp"
// #include "xf_fintech/mc_engine.hpp"
// #include "xf_fintech/rng.hpp"
// typedef float TEST_DT;


//======================== IMPORTANT ================================================//
// The following number defines the number of AXI interfaces for the HBM that is used in the HLS code below.
//    (see #pragma HLS INTERFACE m_axi port=d_hbm_pxx bundle=card_hbm_pxx)
// It is used to check the compatibility with the number of AXI interfaces in the wrapper (set in Kconfig menu) 
// This number is written in the binary image so that the "oc_maint" command displays the number of implemented HBM.
// Minimum is 1 - Maximum is 32 
//          NOTE : for VU3P chip it is not recommended to use more than 12, as
//                 timing closure is too difficult otherwise.
// You can define this number to a lower number than the number of AXI interfaces coded in this HLS code BUT 
// the application shouldn't use more interfaces than the number you have defined in Kconfig menu.
// (extra interfaces not connected will be removed if not connected to the wrapper)

#define HBM_AXI_IF_NB 1

//===================================================================================//

//======================== convert buffers format ===================================//
//Convert a 1024 bits buffer to a 256 bits buffer



static void membus_to_HBMbus(snap_membus_256_t* out_hbm, hls::stream<snap_membus_256_t>& hbm_stream,snapu64_t output_address,
                             snapu64_t wide_lines)
{

// #pragma HLS INLINE 
 
        snap_membus_256_t* temp_ptr;
        temp_ptr= (snap_membus_256_t  *) (out_hbm +output_address);
          for (int k=0; k<wide_lines; k++) {
          #pragma HLS PIPELINE
            temp_ptr[k]=hbm_stream.read();
              //  out_stream.write(data_entry.range((8*sizeof(snap_membus_256_t)*(j+1))-1, (8*sizeof(snap_membus_256_t)*j)));  
        }

}




//convert buffer 256b to 512b
 void HBMbus_to_membus(hls::stream<snap_membus_256_t>& in_stream, hls::stream<snap_membus_512_t>& wide_stream,
                             snapu64_t wide_lines)
{
    //#pragma HLS INLINE off
    snap_membus_512_t data_entry = 0;

    hbm2mem_loop:
    for (int k=0; k<wide_lines; k++) 
    {
        #pragma HLS PIPELINE
        for (int j = 0; j < 2; j++) 
        {
            
            data_entry.range((8*sizeof(snap_membus_256_t)*(j+1))-1, (8*sizeof(snap_membus_256_t)*j))=in_stream.read();
        }
        wide_stream.write(data_entry);
        data_entry = 0;
    }
}


// WRITE DATA TO MEMORY
short write_burst_of_data_to_mem(snap_membus_512_t *dout_gmem,
                 snapu16_t memory_type,
                 snapu64_t output_address_1024,
                 snap_membus_512_t *buffer1024,
                 snapu64_t size_in_bytes_to_transfer)
{
        short rc;

        switch (memory_type) {
        case SNAP_ADDRTYPE_HOST_DRAM:
               memcpy((snap_membus_512_t  *) (dout_gmem + output_address_1024),
                   buffer1024, size_in_bytes_to_transfer);
                rc =  0;
                break;
        case SNAP_ADDRTYPE_UNUSED: /* no copy but with rc =0 */
                rc =  0;
                break;
        default:
                rc = 1;
        }

        return rc;
}
short write_burst_of_data_to_HBM(snap_membus_256_t *d_hbm_p0,
                 snap_membus_256_t *d_hbm_p1,
                 snap_membus_256_t *d_hbm_p2,
                 snap_membus_256_t *d_hbm_p3,
                 snap_membus_256_t *d_hbm_p4,
                 snap_membus_256_t *d_hbm_p5,
                 snap_membus_256_t *d_hbm_p6,
                 snap_membus_256_t *d_hbm_p7,
                 snap_membus_256_t *d_hbm_p8,
                 snap_membus_256_t *d_hbm_p9,
                 snap_membus_256_t *d_hbm_p10,
                 snap_membus_256_t *d_hbm_p11,
                 snap_membus_256_t *d_hbm_p12,
                 snap_membus_256_t *d_hbm_p13,
                 snap_membus_256_t *d_hbm_p14,
                 snap_membus_256_t *d_hbm_p15,
                 snap_membus_256_t *d_hbm_p16,
                 snap_membus_256_t *d_hbm_p17,
                 snap_membus_256_t *d_hbm_p18,
                 snap_membus_256_t *d_hbm_p19,
                 snap_membus_256_t *d_hbm_p20,
                 snap_membus_256_t *d_hbm_p21,
                 snap_membus_256_t *d_hbm_p22,
                 snap_membus_256_t *d_hbm_p23,
                 snap_membus_256_t *d_hbm_p24,
                 snap_membus_256_t *d_hbm_p25,
                 snap_membus_256_t *d_hbm_p26,
                 snap_membus_256_t *d_hbm_p27,
                 snap_membus_256_t *d_hbm_p28,
                 snap_membus_256_t *d_hbm_p29,
                 snap_membus_256_t *d_hbm_p30,
                 snap_membus_256_t *d_hbm_p31,
                 snapu16_t memory_type,
                 snapu64_t output_address,
                 snap_membus_256_t *buffer256,
                 snapu64_t size_in_bytes_to_transfer)
{
        short rc;

        switch (memory_type) 
        {
          case SNAP_ADDRTYPE_HBM_P0:
                  memcpy((snap_membus_256_t  *) (d_hbm_p0 + output_address),
                      buffer256, size_in_bytes_to_transfer);
                  rc = 0;
                  break;
          case SNAP_ADDRTYPE_HBM_P1:
                  memcpy((snap_membus_256_t  *) (d_hbm_p1 + output_address),
                      buffer256, size_in_bytes_to_transfer);
                  rc = 0;
                  break;
         case SNAP_ADDRTYPE_HBM_P2:
                  memcpy((snap_membus_256_t  *) (d_hbm_p2 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P3:
                  memcpy((snap_membus_256_t  *) (d_hbm_p3 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P4:
                  memcpy((snap_membus_256_t  *) (d_hbm_p4 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P5:
                  memcpy((snap_membus_256_t  *) (d_hbm_p5 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P6:
                  memcpy((snap_membus_256_t  *) (d_hbm_p6 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P7:
                  memcpy((snap_membus_256_t  *) (d_hbm_p7 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;

          case SNAP_ADDRTYPE_HBM_P8:
                  memcpy((snap_membus_256_t  *) (d_hbm_p8 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P9:
                  memcpy((snap_membus_256_t  *) (d_hbm_p9 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P10:
                  memcpy((snap_membus_256_t  *) (d_hbm_p10 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P11:
                  memcpy((snap_membus_256_t  *) (d_hbm_p11 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P12:
                  memcpy((snap_membus_256_t  *) (d_hbm_p12 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P13:
                  memcpy((snap_membus_256_t  *) (d_hbm_p13 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P14:
                  memcpy((snap_membus_256_t  *) (d_hbm_p14 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P15:
                  memcpy((snap_membus_256_t  *) (d_hbm_p15 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P16:
                  memcpy((snap_membus_256_t  *) (d_hbm_p16 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P17:
                  memcpy((snap_membus_256_t  *) (d_hbm_p17 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P18:
                  memcpy((snap_membus_256_t  *) (d_hbm_p18 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P19:
                  memcpy((snap_membus_256_t  *) (d_hbm_p19 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P20:
                  memcpy((snap_membus_256_t  *) (d_hbm_p20 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P21:
                  memcpy((snap_membus_256_t  *) (d_hbm_p21 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P22:
                  memcpy((snap_membus_256_t  *) (d_hbm_p22 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P23:
                  memcpy((snap_membus_256_t  *) (d_hbm_p23 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P24:
                  memcpy((snap_membus_256_t  *) (d_hbm_p24 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P25:
                  memcpy((snap_membus_256_t  *) (d_hbm_p25 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P26:
                  memcpy((snap_membus_256_t  *) (d_hbm_p26 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P27:
                  memcpy((snap_membus_256_t  *) (d_hbm_p27 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P28:
                  memcpy((snap_membus_256_t  *) (d_hbm_p28 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P29:
                  memcpy((snap_membus_256_t  *) (d_hbm_p29 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P30:
                  memcpy((snap_membus_256_t  *) (d_hbm_p30 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;
          case SNAP_ADDRTYPE_HBM_P31:
                  memcpy((snap_membus_256_t  *) (d_hbm_p31 + output_address),
                         buffer256, size_in_bytes_to_transfer);
                  rc =  0;
                  break;

          case SNAP_ADDRTYPE_UNUSED: /* no copy but with rc =0 */
                  rc =  0;
                  break;
          default:
                  rc = 1;
            }
        return rc;
}

// READ DATA FROM MEMORY
short read_burst_of_data_from_mem(snap_membus_512_t *din_gmem,
                  snapu16_t memory_type,
                  snapu64_t input_address_1024,
                  snap_membus_512_t *buffer1024,
                  snapu64_t size_in_bytes_to_transfer)
{
        short rc;

        switch (memory_type) {

        case SNAP_ADDRTYPE_HOST_DRAM:
                memcpy(buffer1024, (snap_membus_512_t  *) (din_gmem + input_address_1024),
                     size_in_bytes_to_transfer);
                rc =  0;
                break;
        case SNAP_ADDRTYPE_UNUSED: /* no copy but with rc =0 */
                rc =  0;
                break;
        default:
                rc = 1;
        }

        return rc;
}

short read_burst_of_data_from_HBM(snap_membus_256_t *d_hbm_p0,
                  snap_membus_256_t *d_hbm_p1,
                  snap_membus_256_t *d_hbm_p2,
                  snap_membus_256_t *d_hbm_p3,
                  snap_membus_256_t *d_hbm_p4,
                  snap_membus_256_t *d_hbm_p5,
                  snap_membus_256_t *d_hbm_p6,
                  snap_membus_256_t *d_hbm_p7,
                  snap_membus_256_t *d_hbm_p8,
                  snap_membus_256_t *d_hbm_p9,
                  snap_membus_256_t *d_hbm_p10,
                  snap_membus_256_t *d_hbm_p11,
                  snap_membus_256_t *d_hbm_p12,
                  snap_membus_256_t *d_hbm_p13,
                  snap_membus_256_t *d_hbm_p14,
                  snap_membus_256_t *d_hbm_p15,
                  snap_membus_256_t *d_hbm_p16,
                  snap_membus_256_t *d_hbm_p17,
                  snap_membus_256_t *d_hbm_p18,
                  snap_membus_256_t *d_hbm_p19,
                  snap_membus_256_t *d_hbm_p20,
                  snap_membus_256_t *d_hbm_p21,
                  snap_membus_256_t *d_hbm_p22,
                  snap_membus_256_t *d_hbm_p23,
                  snap_membus_256_t *d_hbm_p24,
                  snap_membus_256_t *d_hbm_p25,
                  snap_membus_256_t *d_hbm_p26,
                  snap_membus_256_t *d_hbm_p27,
                  snap_membus_256_t *d_hbm_p28,
                  snap_membus_256_t *d_hbm_p29,
                  snap_membus_256_t *d_hbm_p30,
                  snap_membus_256_t *d_hbm_p31,
                  snapu16_t memory_type,
                  snapu64_t input_address_256,
                  snap_membus_256_t *buffer256,
                  snapu64_t size_in_bytes_to_transfer)
{
    short rc;
        int i;

        switch (memory_type) {

        case SNAP_ADDRTYPE_HBM_P0:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p0 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P1:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p1 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P2:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p2 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P3:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p3 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P4:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p4 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P5:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p5 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P6:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p6 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P7:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p7 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;

        case SNAP_ADDRTYPE_HBM_P8:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p8 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P9:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p9 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P10:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p10 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P11:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p11 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P12:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p12 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P13:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p13 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P14:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p14 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P15:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p15 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P16:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p16 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P17:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p17 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P18:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p18 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P19:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p19 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P20:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p20 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P21:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p21 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P22:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p22 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P23:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p23 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P24:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p24 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P25:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p25 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P26:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p26 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P27:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p27 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P28:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p28 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P29:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p29 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P30:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p30 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;
        case SNAP_ADDRTYPE_HBM_P31:
                memcpy(buffer256, (snap_membus_256_t  *) (d_hbm_p31 + input_address_256),
                     size_in_bytes_to_transfer);
            rc =  0;
                break;

        case SNAP_ADDRTYPE_UNUSED: /* no copy but with rc =0 */
            rc =  0;
                break;
        default:
                rc = 1;
        }

        return rc;
}

/*

void read_CAPI_HBM_stream(hls::stream<snap_membus_512_t>* wide_stream, 
                  hls::stream<snap_membus_256_t> *d_hbm_p0,
                  hls::stream<snap_membus_256_t> *d_hbm_p1,
                  hls::stream<snap_membus_256_t> *d_hbm_p2,
                  hls::stream<snap_membus_256_t> *d_hbm_p3,
                  hls::stream<snap_membus_256_t> *d_hbm_p4,
                  hls::stream<snap_membus_256_t> *d_hbm_p5,
                  hls::stream<snap_membus_256_t> *d_hbm_p6,
                  hls::stream<snap_membus_256_t> *d_hbm_p7,
                  hls::stream<snap_membus_256_t> *d_hbm_p8,
                  hls::stream<snap_membus_256_t> *d_hbm_p9,
                  hls::stream<snap_membus_256_t> *d_hbm_p10,
                  hls::stream<snap_membus_256_t> *d_hbm_p11,
                  hls::stream<snap_membus_256_t> *d_hbm_p12,
                  hls::stream<snap_membus_256_t> *d_hbm_p13,
                  hls::stream<snap_membus_256_t> *d_hbm_p14,
                  hls::stream<snap_membus_256_t> *d_hbm_p15,
                  hls::stream<snap_membus_256_t> *d_hbm_p16,
                  hls::stream<snap_membus_256_t> *d_hbm_p17,
                  hls::stream<snap_membus_256_t> *d_hbm_p18,
                  hls::stream<snap_membus_256_t> *d_hbm_p19,
                  hls::stream<snap_membus_256_t> *d_hbm_p20,
                  hls::stream<snap_membus_256_t> *d_hbm_p21,
                  hls::stream<snap_membus_256_t> *d_hbm_p22,
                  hls::stream<snap_membus_256_t> *d_hbm_p23,
                  hls::stream<snap_membus_256_t> *d_hbm_p24,
                  hls::stream<snap_membus_256_t> *d_hbm_p25,
                  hls::stream<snap_membus_256_t> *d_hbm_p26,
                  hls::stream<snap_membus_256_t> *d_hbm_p27,
                  hls::stream<snap_membus_256_t> *d_hbm_p28,
                  hls::stream<snap_membus_256_t> *d_hbm_p29,
                  hls::stream<snap_membus_256_t> *d_hbm_p30,
                  hls::stream<snap_membus_256_t> *d_hbm_p31,
                  snapu16_t memory_type,
                  snapu64_t input_address,
                  unsigned lines_transfer,
                  unsigned parallel_factor)
{

 

    snap_membus_t* temp_ptr = din_gmem + input_address;
    // snap_membus_256_t* temp_ptr;
    printf("****reading from hbm**********\n");
    int rc=0;
        //printf("merger::ACCELERATOR:::%d\n",p);
    switch (parallel_factor) {

    case 0:
        d_hbm_p0[i]=temp_ptr[k].range((8*sizeof(snap_membus_256_t)*(j+1))-1, (8*sizeof(mat_elmt_t)*j))
        // temp_ptr= (snap_membus_256_t  *) (d_hbm_p0 + input_address);
        printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 1:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p1 + input_address);
    printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 2:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p2 + input_address);
    printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 3:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p3 + input_address);
        printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 4:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p4 + input_address);
            rc =  0;
        break;
    case 5:
            temp_ptr= (snap_membus_256_t  *) (d_hbm_p5 + input_address);
            rc =  0;
        break;
    case 6:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p6 + input_address);
            rc =  0;
        break;
    case 7:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p7 + input_address);
            rc =  0;
        break;

        case 8:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p8 + input_address);
            rc =  0;
        break;
    
        case 9:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p9 + input_address);
            rc =  0;
        break;
    

        case 10:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p10 + input_address);
            rc =  0;
        break;
    

        case 11:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p11 + input_address);
            rc =  0;
        break;
    

        case 12:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p12 + input_address);
            rc =  0;
        break;
    

        case 13:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p13 + input_address);
            rc =  0;
        break;
    

        case 14:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p14 + input_address);
            rc =  0;
        break;
    

        case 15:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p15 + input_address);
            rc =  0;
        break;

        case 16:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p16 + input_address);
            rc =  0;
        break;
        
        case 17:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p17 + input_address);
            rc =  0;
        break;

        case 18:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p18 + input_address);
            rc =  0;
        break;

        case 19:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p19 + input_address);
            rc =  0;
        break;
        case 20:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p20 + input_address);
            rc =  0;
        break;
        case 21:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p21 + input_address);
            rc =  0;
        break;
        case 22:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p22 + input_address);
            rc =  0;
        break;
        case 23:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p23 + input_address);
            rc =  0;
        break;
        case 24:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p24 + input_address);
            rc =  0;
        break;
        case 25:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p25 + input_address);
            rc =  0;
        break;
        case 26:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p26 + input_address);
            rc =  0;
        break;
        case 27:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p27 + input_address);
            rc =  0;
        break;
        case 28:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p28 + input_address);
            rc =  0;
        break;
        case 29:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p29 + input_address);
            rc =  0;
        break;
        case 30:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p30 + input_address);
            rc =  0;
        break;

        case 31:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p31 + input_address);
            rc =  0;
        break;
    default:
        rc = 1;
    }
    printf("Total lines being read per acc=%d\n",lines_transfer );
    printf("size of data=%d\n",sizeof(d_hbm_p0));
            for (unsigned i = 0; i < lines_transfer; i ++)
            {
               // printf("received %s\n", (char *)((unsigned long)temp_ptr[i]));
                #pragma HLS PIPELINE
                in_stream.write(temp_ptr[i]);
           
}

}


*/


void read_HBM_stream(hls::stream<snap_membus_256_t>& in_stream, 
                  snap_membus_256_t *d_hbm_p0,
                  snap_membus_256_t *d_hbm_p1,
                  snap_membus_256_t *d_hbm_p2,
                  snap_membus_256_t *d_hbm_p3,
                  snap_membus_256_t *d_hbm_p4,
                  snap_membus_256_t *d_hbm_p5,
                  snap_membus_256_t *d_hbm_p6,
                  snap_membus_256_t *d_hbm_p7,
                  snap_membus_256_t *d_hbm_p8,
                  snap_membus_256_t *d_hbm_p9,
                  snap_membus_256_t *d_hbm_p10,
                  snap_membus_256_t *d_hbm_p11,
                  snap_membus_256_t *d_hbm_p12,
                  snap_membus_256_t *d_hbm_p13,
                  snap_membus_256_t *d_hbm_p14,
                  snap_membus_256_t *d_hbm_p15,
                  snap_membus_256_t *d_hbm_p16,
                  snap_membus_256_t *d_hbm_p17,
                  snap_membus_256_t *d_hbm_p18,
                  snap_membus_256_t *d_hbm_p19,
                  snap_membus_256_t *d_hbm_p20,
                  snap_membus_256_t *d_hbm_p21,
                  snap_membus_256_t *d_hbm_p22,
                  snap_membus_256_t *d_hbm_p23,
                  snap_membus_256_t *d_hbm_p24,
                  snap_membus_256_t *d_hbm_p25,
                  snap_membus_256_t *d_hbm_p26,
                  snap_membus_256_t *d_hbm_p27,
                  snap_membus_256_t *d_hbm_p28,
                  snap_membus_256_t *d_hbm_p29,
                  snap_membus_256_t *d_hbm_p30,
                  snap_membus_256_t *d_hbm_p31,
                  snapu16_t memory_type,
                  snapu64_t input_address,
                  unsigned lines_transfer,
                  unsigned parallel_factor)
{
// {
   #pragma HLS INLINE OFF
    snap_membus_256_t* temp_ptr;
    printf("****reading from hbm**********\n");
    int rc=0;
        //printf("merger::ACCELERATOR:::%d\n",p);
    switch (parallel_factor) {

    case 0:

        temp_ptr= (snap_membus_256_t  *) (d_hbm_p0 + input_address);
        printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 1:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p1 + input_address);
    printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 2:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p2 + input_address);
    printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 3:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p3 + input_address);
        printf("running ACCELERATOR=%d\n",parallel_factor);
            rc =  0;
        break;
    case 4:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p4 + input_address);
            rc =  0;
        break;
    case 5:
            temp_ptr= (snap_membus_256_t  *) (d_hbm_p5 + input_address);
            rc =  0;
        break;
    case 6:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p6 + input_address);
            rc =  0;
        break;
    case 7:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p7 + input_address);
            rc =  0;
        break;

        case 8:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p8 + input_address);
            rc =  0;
        break;
    
        case 9:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p9 + input_address);
            rc =  0;
        break;
    

        case 10:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p10 + input_address);
            rc =  0;
        break;
    

        case 11:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p11 + input_address);
            rc =  0;
        break;
    

        case 12:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p12 + input_address);
            rc =  0;
        break;
    

        case 13:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p13 + input_address);
            rc =  0;
        break;
    

        case 14:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p14 + input_address);
            rc =  0;
        break;
    

        case 15:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p15 + input_address);
            rc =  0;
        break;

        case 16:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p16 + input_address);
            rc =  0;
        break;
        
        case 17:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p17 + input_address);
            rc =  0;
        break;

        case 18:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p18 + input_address);
            rc =  0;
        break;

        case 19:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p19 + input_address);
            rc =  0;
        break;
        case 20:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p20 + input_address);
            rc =  0;
        break;
        case 21:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p21 + input_address);
            rc =  0;
        break;
        case 22:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p22 + input_address);
            rc =  0;
        break;
        case 23:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p23 + input_address);
            rc =  0;
        break;
        case 24:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p24 + input_address);
            rc =  0;
        break;
        case 25:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p25 + input_address);
            rc =  0;
        break;
        case 26:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p26 + input_address);
            rc =  0;
        break;
        case 27:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p27 + input_address);
            rc =  0;
        break;
        case 28:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p28 + input_address);
            rc =  0;
        break;
        case 29:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p29 + input_address);
            rc =  0;
        break;
        case 30:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p30 + input_address);
            rc =  0;
        break;

        case 31:
        temp_ptr= (snap_membus_256_t  *) (d_hbm_p31 + input_address);
            rc =  0;
        break;
    default:
        rc = 1;
    }
    printf("Total lines being read per acc=%d\n",lines_transfer );
    printf("size of data=%d\n",sizeof(d_hbm_p0));
            for (unsigned i = 0; i < lines_transfer; i ++)
            {
               // printf("received %s\n", (char *)((unsigned long)temp_ptr[i]));
                #pragma HLS PIPELINE
                in_stream.write(temp_ptr[i]);
           
}

}


void read_ref_stream_splitter (
   hls::stream<snap_membus_256_t> &bus_input ,
   hls::stream<snap_membus_256_t> &read_stream,
   hls::stream<snap_membus_256_t> &ref_stream,
   unsigned num_input_lines)
  {
    #pragma HLS INLINE OFF
    printf("stream splitter\n");

    int acc=MANY_ACC;

    
        split_fetch_const:for(unsigned f=0;f<2;f++)
        {
                // #pragma HLS UNROLL

            switch(f)
                {
                    case 0:
                        for (unsigned i = 0 ;i < num_input_lines/2; i ++)
                        {
                            printf("Read data\n" );

                            // #pragma HLS LOOP_TRIPCOUNT max=lines
                            #pragma HLS PIPELINE II=1
                            snap_membus_256_t temp = bus_input.read();
                            read_stream.write(temp);
                        }
                        break;
                    case 1:
                    for (unsigned i = 0 ;i < num_input_lines/2; i ++)
                        {
                            printf("Ref data\n" );
                            // #pragma HLS LOOP_TRIPCOUNT max=lines
                            #pragma HLS PIPELINE II=1
                            snap_membus_256_t temp = bus_input.read();
                            ref_stream.write(temp);

                    }
                        break;
                    
            
            }
        
        printf("END STREAM\n");
    }
}


void snake_out_stream(g_count_struct &global_count_out, unsigned acc,
        hls::stream<snap_membus_512_t> &data_write, snapu64_t size_in_lines) 
{
    #pragma HLS INLINE OFF

        // d_final_out_type value_u;
    
    
        // union {
        //     mat_elmt_t   value_d; //float

        //     uint8_t     value_u;
        // };


    snap_membus_512_t temp=0;

    g_count_struct *temp_ptr=&global_count_out;
    d_final_out_type *temp_count=NULL;
   temp_count=(d_final_out_type *)&temp_ptr->global_count[0];
    // d_final_out_type* temp_grid=NULL;

    //  temp_grid=(d_final_out_type *) global_count_out[0][0];
        snake_out: for (unsigned i = 0; i < TOTAL_OUT_CACHELINE; i++) 
        {
            #pragma HLS PIPELINE II=1
                
                for(unsigned j = 0; j < ELEMENTS_PER_CACHELINE; j++)
                {
                    // value_u = global_count_out[i*ELEMENTS_PER_CACHELINE + j];
                    
                   // value_u=*(global_count_out + (i*ELEMENTS_PER_CACHELINE) + j);
                    temp.range((sizeof(d_final_out_type)*(j+1))-1, (sizeof(d_final_out_type)*j))=*(temp_count+ ( i *ELEMENTS_PER_CACHELINE) + j);
                }
                  data_write.write(temp);
        }
}

void snake_out(d_final_out_type global_count_out[][MANY_ACC], unsigned acc,
         snap_membus_512_t *dout_gmem, snapu64_t size_in_lines) 
{
    #pragma HLS INLINE OFF

        d_final_out_type value_u;
    
    
        // union {
        //     mat_elmt_t   value_d; //float

        //     uint8_t     value_u;
        // };


    snap_membus_512_t temp=0;

   
    // d_final_out_type* temp_grid=NULL;

    //  temp_grid=(d_final_out_type *) global_count_out[0][0];
        snake_out: for (unsigned i = 0; i < size_in_lines; i++) 
        {
            #pragma HLS PIPELINE II=1
                
                for(unsigned j = 0; j < ELEMENTS_PER_CACHELINE; j++)
                {
                    value_u = global_count_out[acc][i*ELEMENTS_PER_CACHELINE + j];
                    
                   // value_u=*(global_count_out + (i*ELEMENTS_PER_CACHELINE) + j);
                    temp.range((sizeof(d_final_out_type)*(j+1))-1, (sizeof(d_final_out_type)*j))=value_u;
                }
                   dout_gmem[i]=temp;
        }
}

void stream_merger (
   hls::stream<snap_membus_512_t>& bus_output,
   hls::stream<snap_membus_512_t> output[MANY_ACC],
   unsigned num_output_lines)
  {
  snap_membus_512_t temp;
 // const int lines=TOTAL_OUTPUT_WORDS_WRITE;
    for (unsigned p = 0; p < MANY_ACC; p++) {
    // #pragma HLS UNROLL
          for (unsigned i = 0+p*TOTAL_OUT_CACHELINE/MANY_ACC; i < TOTAL_OUT_CACHELINE/MANY_ACC+p*TOTAL_OUT_CACHELINE/MANY_ACC; i++)  
              // for (unsigned i = 0+p*num_output_lines; i < num_output_lines+p*num_output_lines; i++)
    {
         #pragma HLS PIPELINE II=1
            printf("stream_splitter:::::%d\n",i);
          // #pragma HLS LOOP_TRIPCOUNT max=lines 
            temp = output[p].read();
          //  #pragma HLS LOOP_TRIPCOUNT max=lines avg=lines/2
           
               bus_output.write(temp);
    }
  }
}
// WRITE DATA TO MEMORY
void write_stream_to_mem (hls::stream<snap_membus_512_t>& out_stream,
                        snap_membus_512_t *dout_gmem,
                        snapu64_t output_address,
                        snapu64_t size_in_lines)
{
    snap_membus_512_t* temp_ptr = dout_gmem + output_address;
    wb_dout_loop: for (unsigned k=0; k < TOTAL_OUT_CACHELINE; k++) {
#pragma HLS PIPELINE
        temp_ptr[k] = out_stream.read();
    }
}

short complete_pipeline(
                  snap_membus_512_t *din_gmem,
                  snap_membus_512_t *dout_gmem,
                  snap_membus_256_t *d_hbm_p0,
                  snap_membus_256_t *d_hbm_p1,
                  snap_membus_256_t *d_hbm_p2,
                  snap_membus_256_t *d_hbm_p3,
                  snap_membus_256_t *d_hbm_p4,
                  snap_membus_256_t *d_hbm_p5,
                  snap_membus_256_t *d_hbm_p6,
                  snap_membus_256_t *d_hbm_p7,
                  snap_membus_256_t *d_hbm_p8,
                  snap_membus_256_t *d_hbm_p9,
                  snap_membus_256_t *d_hbm_p10,
                  snap_membus_256_t *d_hbm_p11,
                  snap_membus_256_t *d_hbm_p12,
                  snap_membus_256_t *d_hbm_p13,
                  snap_membus_256_t *d_hbm_p14,
                  snap_membus_256_t *d_hbm_p15,
                  snap_membus_256_t *d_hbm_p16,
                  snap_membus_256_t *d_hbm_p17,
                  snap_membus_256_t *d_hbm_p18,
                  snap_membus_256_t *d_hbm_p19,
                  snap_membus_256_t *d_hbm_p20,
                  snap_membus_256_t *d_hbm_p21,
                  snap_membus_256_t *d_hbm_p22,
                  snap_membus_256_t *d_hbm_p23,
                  snap_membus_256_t *d_hbm_p24,
                  snap_membus_256_t *d_hbm_p25,
                  snap_membus_256_t *d_hbm_p26,
                  snap_membus_256_t *d_hbm_p27,
                  snap_membus_256_t *d_hbm_p28,
                  snap_membus_256_t *d_hbm_p29,
                  snap_membus_256_t *d_hbm_p30,
                  snap_membus_256_t *d_hbm_p31,
                  // d_final_out_type global_count[][MANY_ACC],
                    snapu16_t memory_type,
                  snapu64_t input_address,
                  snapu64_t output_address,
                  snapu64_t lines_transfer)
{
short rc=0;
    #pragma HLS INLINE OFF
    #pragma HLS DATAFLOW

// d_bit_in_type RefSeq, d_bit_in_type ReadSeq;

    // hls::stream<snap_membus_t> hdiff_stream;


       // int acc=MANY_ACC;

    int acc=MANY_ACC;
    g_count_struct g_count[MANY_ACC];
    #pragma HLS ARRAY_PARTITION variable=g_count  dim=1 complete
    #pragma HLS RESOURCE variable=g_count->global_count core=XPM_MEMORY uram



    // d_final_out_type global_count[TOTAL_READS/MANY_ACC][MANY_ACC];
    // #pragma HLS ARRAY_PARTITION variable=global_count  dim=2 factor=acc
    // #pragma HLS RESOURCE variable=global_count core=XPM_MEMORY uram
    // #pragma HLS ARRAY_PARTITION variable=global_count  dim=2 complete
    hls::stream<snap_membus_256_t> hbm_in_stream[MANY_ACC];
     #pragma HLS stream variable=hbm_in_stream depth=1 

    hls::stream<snap_membus_256_t> read_in_stream[MANY_ACC];
    #pragma HLS stream variable=read_in_stream depth=1
    hls::stream<snap_membus_256_t> ref_in_stream[MANY_ACC];
    #pragma HLS stream variable=ref_in_stream depth=1 

    hls::stream<snap_membus_512_t> data_out_write[MANY_ACC];
    hls::stream<snap_membus_512_t> bus_out_stream("bus_out_stream");

    printf("DRAM chako \n");

    printf("Total lines=%d\n",HBM_MAX_NB_OF_LINES_READ );
    printf("Total capi lines=%d\n",CAPI_NB_OF_LINES_READ );

    // fetching_mem_loop:  for(unsigned i=0;i<acc;i++){
    //      #pragma HLS DATAFLOW
    //     #pragma HLS UNROLL 
   
    // }
//     write_HBM_stream_parallel(hbm_write_stream,
//      d_hbm_p0, d_hbm_p1,d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7,
//      d_hbm_p8,d_hbm_p9,d_hbm_p10,d_hbm_p11,d_hbm_p12,d_hbm_p13,d_hbm_p14,d_hbm_p15,
//      d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
//      d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
//      d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
//      d_hbm_p30, d_hbm_p31,
//
//      output_address>> ADDR_RIGHT_SHIFT_256,
//    HBM_MAX_NB_OF_LINES_READ/acc);
  main_pipe_loop:for(unsigned i=0;i<acc;i++){
     #pragma HLS UNROLL
      // #pragma HLS DATAFLOW

         //      write_HBM_stream(hbm_write_stream[i],
         // d_hbm_p0, d_hbm_p1,d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7,
         // d_hbm_p8,d_hbm_p9,d_hbm_p10,d_hbm_p11,d_hbm_p12,d_hbm_p13,d_hbm_p14,d_hbm_p15,
         // d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
         // d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
         // d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
         // d_hbm_p30, d_hbm_p31,
         // output_address,
         // HBM_MAX_NB_OF_LINES_READ/acc
         // ,i);

         read_HBM_stream(hbm_in_stream[i],
             d_hbm_p0, d_hbm_p1,
             d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5,
             d_hbm_p6, d_hbm_p7, d_hbm_p8, d_hbm_p9,
             d_hbm_p10, d_hbm_p11, d_hbm_p12, d_hbm_p13, d_hbm_p14,
             d_hbm_p15, d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
             d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
             d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
             d_hbm_p30, d_hbm_p31,
             memory_type,input_address,
             HBM_MAX_NB_OF_LINES_READ/acc
             , i);

         // HBMbus_to_membus(hbm_in_stream[i], wide_in_stream,
         //            CAPI_NB_OF_LINES_READ);
         read_ref_stream_splitter (hbm_in_stream[i],
             read_in_stream[i],ref_in_stream[i],
             //,in_stream3,in_stream4,
             HBM_MAX_NB_OF_LINES_READ/acc
             );
         SneakySnake_stream(read_in_stream[i], ref_in_stream[i],
             HBM_MAX_NB_OF_LINES_READ/acc, g_count[i] );

snake_out_stream(g_count[i] , i,
        data_out_write[i], TOTAL_OUT_CACHELINE/acc) ;
   }

stream_merger (bus_out_stream,data_out_write,TOTAL_OUT_CACHELINE/acc);
write_stream_to_mem(bus_out_stream, dout_gmem, output_address, TOTAL_OUT_CACHELINE);
// snake_out( global_count, 
//          dout_gmem,  HBM_MAX_NB_OF_LINES_READ);


   //          for (i = 0; i < 30000000 ; i++) {
   //              read = getline(&line, &len, fp);
   //              char *s = strdup(line);
   //              strncpy(ReadSeq, s, ReadLength);
   //              strncpy(RefSeq, s+ReadLength+1, ReadLength);

   //              for (n = 0; n < ReadLength; n++) {
   //              printf("%c",ReadSeq[n]);
   //              }
   //              printf("\t");
   //              for (n = 0; n < ReadLength; n++) {
   //              printf("%c",RefSeq[n]);
   //              }
   //              printf("\n");
                

            //  if (SneakySnake(ReadLength, RefSeq, ReadSeq, EditThreshold, KmerSize, DebugMode, IterationNo))
            //      Accepted++;
            //  //printf("i: %d TID:%d Accepted: %d\n",i, tid, step->Accepted[i]);
            // }



            // stream_splitter (wide_in_stream[i],
            // hdiff_stream[i],
            // //,in_stream3,in_stream4, 
            //         CAPI_NB_OF_LINES_READ

            // );
    
            // stream_2D_grid(
            // hdiff_in[i],
            // hdiff_stream[i],
            //   CAPI_NB_OF_LINES_READ

            // );
            // hdiff_3D_stream(hdiff_in[i],hdiff_out[i]);
        // }
    return rc;


}   

short hbm_writing(snap_membus_512_t *dout_gmem,
          snap_membus_256_t *d_hbm_p0,
          snap_membus_256_t *d_hbm_p1,
          snap_membus_256_t *d_hbm_p2,
          snap_membus_256_t *d_hbm_p3,
          snap_membus_256_t *d_hbm_p4,
          snap_membus_256_t *d_hbm_p5,
          snap_membus_256_t *d_hbm_p6,
          snap_membus_256_t *d_hbm_p7,
          snap_membus_256_t *d_hbm_p8,
          snap_membus_256_t *d_hbm_p9,
          snap_membus_256_t *d_hbm_p10,
          snap_membus_256_t *d_hbm_p11,
          snap_membus_256_t *d_hbm_p12,
          snap_membus_256_t *d_hbm_p13,
          snap_membus_256_t *d_hbm_p14,
          snap_membus_256_t *d_hbm_p15,
          snap_membus_256_t *d_hbm_p16,
          snap_membus_256_t *d_hbm_p17,
          snap_membus_256_t *d_hbm_p18,
          snap_membus_256_t *d_hbm_p19,
          snap_membus_256_t *d_hbm_p20,
          snap_membus_256_t *d_hbm_p21,
          snap_membus_256_t *d_hbm_p22,
          snap_membus_256_t *d_hbm_p23,
          snap_membus_256_t *d_hbm_p24,
          snap_membus_256_t *d_hbm_p25,
          snap_membus_256_t *d_hbm_p26,
          snap_membus_256_t *d_hbm_p27,
          snap_membus_256_t *d_hbm_p28,
          snap_membus_256_t *d_hbm_p29,
          snap_membus_256_t *d_hbm_p30,
          snap_membus_256_t *d_hbm_p31,
          snapu16_t memory_type,
        
          snapu64_t out_address,
          snapu64_t lines_transfer)

{
  #pragma HLS INLINE OFF
    hls::stream<snap_membus_256_t> hbm_out_stream;
  hls::stream<snap_membus_512_t> wide_out_stream;
  hls::stream<snap_membus_512_t> hdiff_out_stream;
 short rc=0;
/*
 
  #pragma HLS DATAFLOW

  membus_to_HBMbus(hbm_out_stream, wide_out_stream,
    CAPI_NB_OF_LINES_READ
                       );

    write_HBM_stream(hbm_out_stream, 
      d_hbm_p0, d_hbm_p1,d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7,
      d_hbm_p8,d_hbm_p9,d_hbm_p10,d_hbm_p11,d_hbm_p12,d_hbm_p13,d_hbm_p14,d_hbm_p15,
      d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
      d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
      d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
      d_hbm_p30, d_hbm_p31,
      memory_type,
      out_address,
    HBM_MAX_NB_OF_LINES_READ
       , 0);

*/
  // DRAM_stream_merger (
 //  hdiff_out_stream[i],
 //     wide_out_stream[i],
 //  CAPI_NB_OF_LINES_READ
  //          );
  //    }
  //  for(unsigned i=0;i<acc;i++){

  //    write_DRAM_stream(wide_out_stream[i], 
  //    dout_gmem,
  //    d_ddrmem,
  //     memory_type,
  //    out_address,
  //    CAPI_NB_OF_LINES_READ
  //    );



return rc;
}


//----------------------------------------------------------------------
//--- MAIN PROGRAM -----------------------------------------------------
//----------------------------------------------------------------------
static void process_action(snap_membus_512_t *din_gmem,
                           snap_membus_512_t *dout_gmem,
                           snap_membus_256_t *d_hbm_p0,
                           snap_membus_256_t *d_hbm_p1,
                           snap_membus_256_t *d_hbm_p2,
                           snap_membus_256_t *d_hbm_p3,
                           snap_membus_256_t *d_hbm_p4,
                           snap_membus_256_t *d_hbm_p5,
                           snap_membus_256_t *d_hbm_p6,
                           snap_membus_256_t *d_hbm_p7,
                           snap_membus_256_t *d_hbm_p8,
                           snap_membus_256_t *d_hbm_p9,
                           snap_membus_256_t *d_hbm_p10,
                           snap_membus_256_t *d_hbm_p11,
                           snap_membus_256_t *d_hbm_p12,
                           snap_membus_256_t *d_hbm_p13,
                           snap_membus_256_t *d_hbm_p14,
                           snap_membus_256_t *d_hbm_p15,
                           snap_membus_256_t *d_hbm_p16,
                           snap_membus_256_t *d_hbm_p17,
                           snap_membus_256_t *d_hbm_p18,
                           snap_membus_256_t *d_hbm_p19,
                           snap_membus_256_t *d_hbm_p20,
                           snap_membus_256_t *d_hbm_p21,
                           snap_membus_256_t *d_hbm_p22,
                           snap_membus_256_t *d_hbm_p23,
                           snap_membus_256_t *d_hbm_p24,
                           snap_membus_256_t *d_hbm_p25,
                           snap_membus_256_t *d_hbm_p26,
                           snap_membus_256_t *d_hbm_p27,
                           snap_membus_256_t *d_hbm_p28,
                           snap_membus_256_t *d_hbm_p29,
                           snap_membus_256_t *d_hbm_p30,
                           snap_membus_256_t *d_hbm_p31,
                           action_reg *act_reg)
{
    // VARIABLES
    snapu32_t xfer_size;
    snapu32_t action_xfer_size;
    snapu32_t nb_blocks_to_xfer;
    snapu16_t i;
    short rc = 0;
    snapu32_t ReturnCode = SNAP_RETC_SUCCESS;
    snapu64_t input_address;
    snapu64_t output_address;
    snapu64_t address_xfer_offset;
    snap_membus_512_t  buffer1024[MAX_NB_OF_WORDS_READ_1024];
    snap_membus_256_t   buffer256[MAX_NB_OF_WORDS_READ_256];

    // byte address received need to be aligned with port width
    // -- shift depends on the size of the bus => do it later
    input_address = (act_reg->Data.in.addr);
    output_address = (act_reg->Data.out.addr);

    address_xfer_offset = 0x0;
    
    action_xfer_size = MIN(act_reg->Data.in.size,
                   act_reg->Data.out.size);
    // print("act_reg->Data.in.size=%u\n",(uint*)(unsigned long)act_reg->Data.in.size);
    // print("act_reg->Data.out.size=%u\n",(uint*)(unsigned long)act_reg->Data.out.size);
    // buffer size is hardware limited by MAX_NB_OF_BYTES_READ
    if(action_xfer_size %MAX_NB_OF_BYTES_READ == 0)
        nb_blocks_to_xfer = (action_xfer_size / MAX_NB_OF_BYTES_READ);
    else
        nb_blocks_to_xfer = (action_xfer_size / MAX_NB_OF_BYTES_READ) + 1;


    printf("PARAMETERS:\n"
           "  addr_in:     0x%016llx\n"
   
           "  addr_out:    0x%016llx\n"
           "  size_in/out: %u\n",
  
         
            (long long)input_address,
           (long long)output_address,
           action_xfer_size);

        // memcopy can be from/to following types: 
        // SNAP_ADDRTYPE_UNUSED (=BRAM) , SNAP_ADDRTYPE_HOST_DRAM, SNAP_ADDRTYPE_HBM_P0 to SNAP_ADDRTYPE_HBM_P7
// xf::fintech::MCEuropeanEngine<TEST_DT, 2>(underlying, volatility, dividendYield,
//                                               riskFreeRate, // model parameter
//                                               timeLength, strike,
//                                               optionType, // option parameter
//                                               seed, output, requiredTolerance, requiredSamples, timeSteps);
    // xf::fintech::MCEuropeanEngine<TEST_DT, 2>(underlying, volatility, dividendYield,
 //                                              riskFreeRate, // model parameter
 //                                              timeLength, strike,
 //                                              optionType, // option parameter
 //                                              seed, output, requiredTolerance, requiredSamples, timeSteps);

// fetch_mem(din_gmem, d_hbm_p0, InputAddress>> ADDR_RIGHT_SHIFT_1024,OutputAddress>> ADDR_RIGHT_SHIFT_256,
//                   HBM_MAX_NB_OF_LINES_READ);

 // #pragma HLS DATAFLOW
    unsigned acc=MANY_ACC;

    printf("Allocation global_count of elements=%d\n",TOTAL_READS/MANY_ACC);
   fetch_mem(din_gmem, d_hbm_p0, d_hbm_p1,
            d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5,
            d_hbm_p6, d_hbm_p7, d_hbm_p8, d_hbm_p9,
            d_hbm_p10, d_hbm_p11, d_hbm_p12, d_hbm_p13, d_hbm_p14,
            d_hbm_p15, d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
            d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
            d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
            d_hbm_p30, d_hbm_p31,
            input_address>> ADDR_RIGHT_SHIFT_1024,output_address>> ADDR_RIGHT_SHIFT_256,
                   HBM_MAX_NB_OF_LINES_READ);
    
rc |=complete_pipeline( din_gmem,
    dout_gmem,
                    d_hbm_p0, d_hbm_p1,
                d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5,
                d_hbm_p6, d_hbm_p7, d_hbm_p8, d_hbm_p9,
                d_hbm_p10, d_hbm_p11, d_hbm_p12, d_hbm_p13, d_hbm_p14,
                d_hbm_p15, d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
                d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
                d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
                d_hbm_p30, d_hbm_p31,
                // global_count,
                    act_reg->Data.in.type,
                input_address+address_xfer_offset,
                output_address,
                    HBM_MAX_NB_OF_LINES_READ
                );
// snake_out( global_count, 
//          dout_gmem,  HBM_MAX_NB_OF_LINES_READ);
// rc |= hbm_writing(dout_gmem,
//         d_hbm_p0, d_hbm_p1,d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7,
//           d_hbm_p8,d_hbm_p9,d_hbm_p10,d_hbm_p11,d_hbm_p12,d_hbm_p13,d_hbm_p14,d_hbm_p15,
//           d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
//                 d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
//                 d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
//                 d_hbm_p30, d_hbm_p31,
//       act_reg->Data.out.type,
//         (output_address+address_xfer_offset)>>ADDR_RIGHT_SHIFT_256,
//           HBM_MAX_NB_OF_LINES_READ
//         );



    if (rc != 0)
        ReturnCode = SNAP_RETC_FAILURE;

    act_reg->Control.Retc = ReturnCode;
    return;
}
#ifndef __SYNTHESIS__
uint parseSubMap(const string submap){
    uint map = 0;
    
    for(int i = 0; i < 16; i++){
        string nucleotide = submap.substr(i, 1);

        if(nucleotide.compare("A") == 0){}
            //do nothing
        else if(nucleotide.compare("C") == 0)
            map |= 1;
        else if(nucleotide.compare("G") == 0)
            map |= 2;
        else if(nucleotide.compare("T") == 0)
            map |= 3;
        else{
            std::cout << "Error: unexpected character in the input file:" << nucleotide << ". Exiting..." << std::endl;
            exit(-1);
        }

        if(i != 15)
            map <<= 2;
    }

    return map;
}
void parseMapping(const string mapp, uint* buf){
    string submap ;
    for(int i = 0; i < 4; i++){
         submap = mapp.substr(i*16, 16);
          std::cout <<submap << '\n';
       //  printf("Input read: %s\n", (char *)((unsigned long)submap + i));
                buf[i] = parseSubMap(submap);
                std::cout<<buf[i] << '\n';
                printf("SIZE OF ENCODED=%d\n",sizeof(buf[i]));
    }
}
#endif

//--- TOP LEVEL MODULE -------------------------------------------------
// snap_membus_512_t and snap_membus_256_t are defined in actions/include/hls_snap_1024.H
void hls_action(snap_membus_512_t *din_gmem,
        snap_membus_512_t *dout_gmem,
        snap_membus_256_t *d_hbm_p0,
        snap_membus_256_t *d_hbm_p1,
        snap_membus_256_t *d_hbm_p2,
        snap_membus_256_t *d_hbm_p3,
        snap_membus_256_t *d_hbm_p4,
        snap_membus_256_t *d_hbm_p5,
        snap_membus_256_t *d_hbm_p6,
        snap_membus_256_t *d_hbm_p7,
        snap_membus_256_t *d_hbm_p8,
        snap_membus_256_t *d_hbm_p9,
        snap_membus_256_t *d_hbm_p10,
        snap_membus_256_t *d_hbm_p11,
                snap_membus_256_t *d_hbm_p12,
                snap_membus_256_t *d_hbm_p13,
                snap_membus_256_t *d_hbm_p14,
                snap_membus_256_t *d_hbm_p15,
                snap_membus_256_t *d_hbm_p16,
                snap_membus_256_t *d_hbm_p17,
                snap_membus_256_t *d_hbm_p18,
                snap_membus_256_t *d_hbm_p19,
                snap_membus_256_t *d_hbm_p20,
                snap_membus_256_t *d_hbm_p21,
                snap_membus_256_t *d_hbm_p22,
                snap_membus_256_t *d_hbm_p23,
                snap_membus_256_t *d_hbm_p24,
                snap_membus_256_t *d_hbm_p25,
                snap_membus_256_t *d_hbm_p26,
                snap_membus_256_t *d_hbm_p27,
                snap_membus_256_t *d_hbm_p28,
                snap_membus_256_t *d_hbm_p29,
                snap_membus_256_t *d_hbm_p30,
                snap_membus_256_t *d_hbm_p31,
        action_reg *act_reg)
{
    // Host Memory AXI Interface
#pragma HLS INTERFACE m_axi port=din_gmem bundle=host_mem offset=slave depth=512  \
  max_read_burst_length=64  max_write_burst_length=64 
#pragma HLS INTERFACE s_axilite port=din_gmem bundle=ctrl_reg offset=0x030

#pragma HLS INTERFACE m_axi port=dout_gmem bundle=host_mem offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 
#pragma HLS INTERFACE s_axilite port=dout_gmem bundle=ctrl_reg offset=0x040

    // HBM interfaces
#pragma HLS INTERFACE m_axi port=d_hbm_p0 bundle=card_hbm_p0 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p1 bundle=card_hbm_p1 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p2 bundle=card_hbm_p2 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p3 bundle=card_hbm_p3 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p4 bundle=card_hbm_p4 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p5 bundle=card_hbm_p5 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p6 bundle=card_hbm_p6 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p7 bundle=card_hbm_p7 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p8 bundle=card_hbm_p8 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p9 bundle=card_hbm_p9 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p10 bundle=card_hbm_p10 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p11 bundle=card_hbm_p11 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p12 bundle=card_hbm_p12 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p13 bundle=card_hbm_p13 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p14 bundle=card_hbm_p14 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p15 bundle=card_hbm_p15 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p16 bundle=card_hbm_p16 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p17 bundle=card_hbm_p17 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p18 bundle=card_hbm_p18 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p19 bundle=card_hbm_p19 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p20 bundle=card_hbm_p20 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p21 bundle=card_hbm_p21 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p22 bundle=card_hbm_p22 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p23 bundle=card_hbm_p23 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p24 bundle=card_hbm_p24 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p25 bundle=card_hbm_p25 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p26 bundle=card_hbm_p26 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p27 bundle=card_hbm_p27 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p28 bundle=card_hbm_p28 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p29 bundle=card_hbm_p29 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p30 bundle=card_hbm_p30 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

#pragma HLS INTERFACE m_axi port=d_hbm_p31 bundle=card_hbm_p31 offset=slave depth=512 \
  max_read_burst_length=64  max_write_burst_length=64 

    // Host Memory AXI Lite Master Interface
#pragma HLS DATA_PACK variable=act_reg
#pragma HLS INTERFACE s_axilite port=act_reg bundle=ctrl_reg offset=0x100
#pragma HLS INTERFACE s_axilite port=return bundle=ctrl_reg

        process_action(din_gmem, dout_gmem, d_hbm_p0, d_hbm_p1,
                       d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, 
                       d_hbm_p6, d_hbm_p7, d_hbm_p8, d_hbm_p9, 
               d_hbm_p10, d_hbm_p11, d_hbm_p12, d_hbm_p13, d_hbm_p14,
               d_hbm_p15, d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
               d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
               d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
               d_hbm_p30, d_hbm_p31,
                      act_reg);
}

//-----------------------------------------------------------------------------
//--- TESTBENCH ---------------------------------------------------------------
//-----------------------------------------------------------------------------

#ifdef NO_SYNTH

int main(void)
{
#define MEMORY_LINES_256  2048 /* 64 KiB */
#define MEMORY_LINES_1024 512  /* 64 KiB */
    int rc = 0;
    unsigned int i;
    static snap_membus_512_t  din_gmem[CAPI_NB_OF_LINES_READ];
    static snap_membus_512_t  dout_gmem[CAPI_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p0[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p1[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p2[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p3[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p4[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p5[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p6[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p7[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p8[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p9[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p10[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p11[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p12[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p13[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p14[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p15[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p16[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p17[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p18[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p19[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p20[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p21[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p22[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p23[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p24[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p25[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p26[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p27[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p28[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p29[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p30[HBM_MAX_NB_OF_LINES_READ];
    static snap_membus_256_t   d_hbm_p31[HBM_MAX_NB_OF_LINES_READ];


    action_reg act_reg;


    //     // Discovery Phase .....
    // // when flags = 0 then action will just return action type and release
    // act_reg.Control.flags = 0x0;
    // printf("Discovery : calling action to get config data\n");
    // hls_action(din_gmem, dout_gmem, d_hbm_p0, d_hbm_p1, d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7, d_hbm_p8, d_hbm_p9,
    //      d_hbm_p10, d_hbm_p11, d_hbm_p12, d_hbm_p13, d_hbm_p14, d_hbm_p15, d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19, 
    //      d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24, d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
    //      d_hbm_p30, d_hbm_p31,&act_reg);
    // fprintf(stderr,
    // "RETC:      %04x\n",
    // (unsigned int)act_reg.Control.Retc);


    FILE * fp;
    char * line = NULL;
    size_t len = 0;
    ssize_t read;
    
    fp = fopen("/home/20170356/SneakySnake/Datasets/ERR240727_1_E2_30000Pairs.txt", "r");

    int loopPar;

    long start, end;
    
    // int DebugMode = atoi(argv[1]);
    int KmerSize =  100; //atoi(argv[2]);
    int IterationNo = 100; //atoi(argv[4]);
    int EditThreshold = 10; //atoi(argv[8]);
    int Accepted =0;
    char RefSeq[READ_LENGTH];
    char ReadSeq[READ_LENGTH];

      
    memset(din_gmem,  0xA, sizeof(din_gmem));
    memset(dout_gmem, 0xB, sizeof(dout_gmem));
    memset(d_hbm_p0,  0xC, sizeof(d_hbm_p0));
    memset(d_hbm_p1,  0xC, sizeof(d_hbm_p1));
    printf("size of hbm= %d\n", sizeof(d_hbm_p0));

    for (i = 0; i < TOTAL_READS; i++) 
    {
        read = getline(&line, &len, fp);
        char *s = strdup(line);
        printf("open file %s\n", (char *)((unsigned long)s));
        strncpy(ReadSeq, s, READ_LENGTH);

        strncpy(RefSeq, s+READ_LENGTH+1, READ_LENGTH);

        for (int n = 0; n < READ_LENGTH; n++) {
        printf("%c",ReadSeq[n]);
        }
    
        printf("\n");

        for (int n = 0; n < READ_LENGTH; n++) {
            printf("%c",RefSeq[n]);
        }
        printf("\n");

        union {
        char   value_c;
        uint8_t     value_u;
        } ;

     
        char *ptr1 = ReadSeq;
        char *ptr2 = RefSeq;
        uint* rbuf_read, *rbuf_ref;
        rbuf_read = new uint[READ_LENGTH];
        rbuf_ref = new uint[READ_LENGTH];

        parseMapping(ReadSeq, rbuf_read);
        parseMapping(RefSeq, rbuf_ref);
        // for(int l=0;l<6;l++)
        // printf("Encoded read: %u\n", rbuf_read[l]);


        printf("number of elements %u\n", sizeof(rbuf_read)/sizeof(uint)); /* 5 */

        printf("final size of encoded=%d\n",sizeof(rbuf_read));

        printf("size of read=%d\n",READ_LENGTH*2/8 );
        // memcpy( d_hbm_p0+i, ReadSeq, READ_LENGTH*2/8);
        // memcpy( d_hbm_p0+i+32, RefSeq, READ_LENGTH*2/8);
        memcpy( &d_hbm_p0[i], rbuf_read, sizeof(rbuf_read));
        memcpy( &d_hbm_p0[i+1],rbuf_ref, sizeof(rbuf_ref));
        // memcpy( &d_hbm_p0[i], ReadSeq, READ_LENGTH*2/8);
        
        // memcpy( &d_hbm_p0[i+1], RefSeq, READ_LENGTH*2/8);
        printf("Input read: %u\n", (uint *)((unsigned long)d_hbm_p0 + i));

        printf("Input ref : %u\n", (uint *)((unsigned long)d_hbm_p0 +i+ 32));
    //     for(int j=0;j<READ_LENGTH;j++)
    //     {
            
    //         // value_c=ReadSeq[j];
    //         // print("%c",ReadSeq[j]);
    //         // print("%c\n",value_c);
    //     // d_hbm_p0[i]((2*(j+1))-1, (2*j))=*(ptr1+j);
         
    //     d_hbm_p0[i]((2*(j+1))-1, (2*j))= *(ptr1+j) ;


    //     d_hbm_p0[i+TOTAL_READS]((2*(j+1))-1, (2*j))=*(ptr2+j);
       
    //     printf("PACKING DATA ELEMENT:::[%d]=%d\n",j,*(ptr1+j));

    //     value_u=(uint8_t)d_hbm_p0[i]((2*(j+1))-1, (2*j));
    //     printf("PACKING DATA ELEMENT:::[%d]=%d\n",j,value_c);
    //     }

    //     for(int j=READ_LENGTH;j<128;j++)
    //     {
    //     d_hbm_p0[i]((2*(j+1))-1, (2*j))='X';
    //     d_hbm_p0[i+TOTAL_READS]((2*(j+1))-1, (2*j))='X';
    //  //   printf("PACKING DATA ELEMENT:::[%d]=%c\n",j, (char)(d_hbm_p0[i+TOTAL_READS]((2*(j+1))-1, (2*j))));
    //     }

        

       }

    // for (i = 0; i < TOTAL_READS; i++) {
    //     read = getline(&line, &len, fp);
    //     char *s = strdup(line);

        
      
    //     for (int n = 0; n < READ_LENGTH; n++) {
    //         printf("%c",RefSeq[n]);
    //     }
    //     printf("\n");

        

    //     for(int j=0;j<READ_LENGTH;j++)
    //     {
    //     d_hbm_p0[i+TOTAL_READS]((2*(j+1))-1, (2*j))=*(ptr+j);
    //     printf("PACKING DATA ELEMENT:::[%d]=%c\n",j,*(ptr+j));
    //     }

    //     for(int j=READ_LENGTH;j<128;j++)
    //     {
    //     d_hbm_p0[i+TOTAL_READS]((2*(j+1))-1, (2*j))='X';
    //     printf("PACKING DATA ELEMENT:::[%d]=%c\n",j, (char)(d_hbm_p0[i+TOTAL_READS]((2*(j+1))-1, (2*j))));
    //     }


    // }



    
    printf("size %d\n", sizeof(ReadSeq));
    
    act_reg.Control.flags = 0x1; /* just not 0x0 */

    act_reg.Data.in.addr = 0;
    act_reg.Data.in.size = 64;
    act_reg.Data.in.type = SNAP_ADDRTYPE_HOST_DRAM;

    act_reg.Data.out.addr = 4096;
    act_reg.Data.out.size = 64;
    act_reg.Data.out.type = SNAP_ADDRTYPE_HOST_DRAM;

    hls_action(din_gmem, dout_gmem, d_hbm_p0, d_hbm_p1, d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7, d_hbm_p8, d_hbm_p9,
         d_hbm_p10, d_hbm_p11, d_hbm_p12, d_hbm_p13, d_hbm_p14, d_hbm_p15, d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19, 
         d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24, d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
         d_hbm_p30, d_hbm_p31,&act_reg);
    if (act_reg.Control.Retc == SNAP_RETC_FAILURE) {
        fprintf(stderr, " ==> RETURN CODE FAILURE <==\n");
        return 1;
    }
    if (memcmp((void *)((unsigned long)din_gmem + 0),
           (void *)((unsigned long)dout_gmem + 4096), 4096) != 0) {
        fprintf(stderr, " ==> DATA COMPARE FAILURE <==\n");
        return 1;
    }
    else
        printf(" ==> DATA COMPARE OK <==\n");

    return 0;
}

#endif
