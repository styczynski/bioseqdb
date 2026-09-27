#include "fetch_mem.h"
void read_ocapi_to_stream (hls::stream<snap_membus_512_t>& wide_stream,
                        snap_membus_512_t *din_gmem,
                        snapu64_t input_address,
                        snapu64_t size_in_lines)
{
// #pragma HLS INLINE 
    #pragma HLS INLINE OFF
    snap_membus_512_t* temp_ptr = din_gmem + input_address;
    rd_din_loop: for (unsigned k=0; k < size_in_lines; k++) {
        //printf("size_in lines=%d\n",k);
    #pragma HLS PIPELINE
        wide_stream.write(temp_ptr[k]);
    }
}

void membus_to_HBMbus_compatible_stream(hls::stream<snap_membus_256_t>& out_stream, hls::stream<snap_membus_512_t>& wide_stream,
                             snapu64_t wide_lines)
{

#pragma HLS INLINE OFF
        snap_membus_512_t data_entry = 0;
        membus_to_HBMbus_compatible_stream_loop1:  for (int k=0; k<wide_lines; k++) {
           #pragma HLS PIPELINE
            data_entry=wide_stream.read();
        membus_to_HBMbus_compatible_stream_loop2:      for (int j = 0; j < 2; j++) {
             #pragma HLS PIPELINE
                out_stream.write(data_entry.range((8*sizeof(snap_membus_256_t)*(j+1))-1, (8*sizeof(snap_membus_256_t)*j)));
            }
        }

}
void write_HBM_stream(hls::stream<snap_membus_256_t>& out_stream, 
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
             
            snapu64_t output_address,
            snapu64_t lines_transfer,
            unsigned parallel_factor)
{
     #pragma HLS INLINE OFF
  snap_membus_256_t* temp_ptr;

  int rc=0;
    //printf("merger::ACCELERATOR:::%d\n",p);
  switch (parallel_factor) {

  case 0:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p0 + output_address);
    printf("running ACCELERATOR=%d\n",parallel_factor);
          rc =  0;
    break;
  case 1:
  temp_ptr= (snap_membus_256_t  *) (d_hbm_p1 + output_address);
  printf("running ACCELERATOR=%d\n",parallel_factor);
          rc =  0;
    break;
  case 2:
  temp_ptr= (snap_membus_256_t  *) (d_hbm_p2 + output_address);
  printf("running ACCELERATOR=%d\n",parallel_factor);
          rc =  0;
    break;
  case 3:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p3 + output_address);
    printf("running ACCELERATOR=%d\n",parallel_factor);
          rc =  0;
    break;
  case 4:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p4 + output_address);
          rc =  0;
    break;
  case 5:
      temp_ptr= (snap_membus_256_t  *) (d_hbm_p5 + output_address);
          rc =  0;
    break;
  case 6:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p6 + output_address);
          rc =  0;
    break;
  case 7:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p7 + output_address);
          rc =  0;
    break;

    case 8:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p8 + output_address);
          rc =  0;
    break;
  
    case 9:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p9 + output_address);
          rc =  0;
    break;
  

    case 10:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p10 + output_address);
          rc =  0;
    break;
  

    case 11:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p11 + output_address);
          rc =  0;
    break;
  

    case 12:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p12 + output_address);
          rc =  0;
    break;
  

    case 13:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p13 + output_address);
          rc =  0;
    break;
  

    case 14:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p14 + output_address);
          rc =  0;
    break;
  

    case 15:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p15 + output_address);
          rc =  0;
    break;
    
      case 16:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p16 + output_address);
          rc =  0;
    break;

      case 17:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p17 + output_address);
          rc =  0;
    break;
      case 18:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p18 + output_address);
          rc =  0;
    break;
      case 19:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p19 + output_address);
          rc =  0;
    break;
      case 20:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p20 + output_address);
          rc =  0;
    break;
      case 21:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p21 + output_address);
          rc =  0;
    break;
      case 22:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p22 + output_address);
          rc =  0;
    break;
      case 23:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p23 + output_address);
          rc =  0;
    break;
      case 24:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p24 + output_address);
          rc =  0;
    break;
      case 25:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p25 + output_address);
          rc =  0;
    break;
      case 26:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p26 + output_address);
          rc =  0;
    break;
      case 27:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p27 + output_address);
          rc =  0;
    break;
      case 28:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p28 + output_address);
          rc =  0;
    break;
      case 29:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p29 + output_address);
          rc =  0;
    break;
      case 30:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p30 + output_address);
          rc =  0;
    break;
      case 31:
    temp_ptr= (snap_membus_256_t  *) (d_hbm_p31 + output_address);
          rc =  0;
    break;

  default:
    rc = 1;
  }

      for (unsigned i = 0; i < lines_transfer; i ++)
      {

       #pragma HLS PIPELINE
        temp_ptr[i]=out_stream.read();

      }
 

}
void hbm_stream_splitter (
   hls::stream<snap_membus_256_t>& bus_input,
   hls::stream<snap_membus_256_t> output[MANY_ACC],
   unsigned num_input_lines)
  {

    #pragma HLS INLINE OFF
  int lines=HBM_MAX_NB_OF_LINES_READ/MANY_ACC;
  for (unsigned p = 0; p < MANY_ACC; p++)
  {
    #pragma HLS PIPELINE II=1
    printf("stream_splitter::ACCELERATOR:::%d\n",p);
    for (unsigned i = 0+p*lines; i < lines+p*lines; i++)
    {
       //#pragma HLS LOOP_TRIPCOUNT avg=lines/2
       // #pragma HLS LOOP_TRIPCOUNT max=lines
      printf("stream_splitter:::::%d\n",i);
     #pragma HLS LOOP_TRIPCOUNT max=lines
    #pragma HLS PIPELINE II=1
    snap_membus_256_t temp = bus_input.read();
    output[p].write(temp);
    }
    
  }}


void write_HBM_stream_parallel(hls::stream<snap_membus_256_t>hbm_write_stream[MANY_ACC], 
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
             
            snapu64_t output_address,
            snapu64_t lines_transfer)
{
    #pragma HLS INLINE OFF
 int acc=MANY_ACC;
 for(unsigned i=0;i<acc;i++){
        #pragma HLS UNROLL 
    write_HBM_stream(hbm_write_stream[i], 
      d_hbm_p0, d_hbm_p1,d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7,
      d_hbm_p8,d_hbm_p9,d_hbm_p10,d_hbm_p11,d_hbm_p12,d_hbm_p13,d_hbm_p14,d_hbm_p15,
      d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
      d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
      d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
      d_hbm_p30, d_hbm_p31,
    
      output_address,
    lines_transfer
       ,i);

}
}

void fetch_mem(snap_membus_512_t *din_gmem,
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
                  snapu64_t input_address,snapu64_t output_address,
                  snapu64_t lines_transfer)
{
    #pragma HLS INLINE OFF
    #pragma HLS DATAFLOW
        // hls::stream<snap_membus_512_t> wide_in_stream;
        // hls::stream<snap_membus_256_t> hbm_buf_stream;
        // read_mem_to_stream (wide_in_stream,
        //                     din_gmem,
        //                     input_address,
        //                     CAPI_NB_OF_LINES_READ);
        // membus_to_HBMbus_compatible_stream(hbm_buf_stream,wide_in_stream,CAPI_NB_OF_LINES_READ);
        // membus_to_HBMbus(d_hbm_p0, hbm_buf_stream,output_address,HBM_MAX_NB_OF_LINES_READ);


        // printf("DRAM chako \n");


    int acc=MANY_ACC;
   
    hls::stream<snap_membus_512_t> wide_in_stream;
    hls::stream<snap_membus_256_t> hbm_buf_stream;
    hls::stream<snap_membus_256_t> hbm_write_stream[MANY_ACC];
    read_ocapi_to_stream (wide_in_stream, din_gmem,  input_address, CAPI_NB_OF_LINES_READ);
    membus_to_HBMbus_compatible_stream(hbm_buf_stream,wide_in_stream,CAPI_NB_OF_LINES_READ);
    hbm_stream_splitter (hbm_buf_stream,hbm_write_stream, HBM_MAX_NB_OF_LINES_READ/acc);

      fetching_Write:for(unsigned i=0;i<acc;i++){
     #pragma HLS UNROLL
      // #pragma HLS DATAFLOW

              write_HBM_stream(hbm_write_stream[i],
         d_hbm_p0, d_hbm_p1,d_hbm_p2, d_hbm_p3, d_hbm_p4, d_hbm_p5, d_hbm_p6, d_hbm_p7,
         d_hbm_p8,d_hbm_p9,d_hbm_p10,d_hbm_p11,d_hbm_p12,d_hbm_p13,d_hbm_p14,d_hbm_p15,
         d_hbm_p16, d_hbm_p17, d_hbm_p18, d_hbm_p19,
         d_hbm_p20, d_hbm_p21, d_hbm_p22, d_hbm_p23, d_hbm_p24,
         d_hbm_p25, d_hbm_p26, d_hbm_p27, d_hbm_p28, d_hbm_p29,
         d_hbm_p30, d_hbm_p31,
         output_address,
         HBM_MAX_NB_OF_LINES_READ/acc
         ,i);
          }
   


}

