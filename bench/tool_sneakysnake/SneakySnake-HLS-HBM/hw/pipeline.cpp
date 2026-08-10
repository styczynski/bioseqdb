
#include "pipeline.h"

/*
int NeighborhoodMap(int read_length,
    d_in_type & DNA_read, d_in_type & DNA_ref, 
    d_out_type & DNA_nsh,
    d_out_type & DNA_shl_one,d_out_type & DNA_shl_two,d_out_type & DNA_shl_three,d_out_type & DNA_shl_four,d_out_type & DNA_shl_five, 
    d_out_type & DNA_shr_one,d_out_type & DNA_shr_two,d_out_type & DNA_shr_three,d_out_type & DNA_shr_four,d_out_type & DNA_shr_five)
{
    
    d_in_type  DNA_1[LENGTH], DNA_2[LENGTH], DNA_3[LENGTH], DNA_4[LENGTH], DNA_5[LENGTH], DNA_6[LENGTH],
     DNA_7[LENGTH], DNA_8[LENGTH], DNA_9[LENGTH], DNA_10[LENGTH], DNA_11[LENGTH];
    int index, i;
    for (int j=0;j<LENGTH;j++)
        DNA_1[j]= DNA_ref[j] ^ DNA_read[j];
    index=0;
    
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_1[i] || DNA_1[i+1])
            DNA_nsh[index] = 1;            //Bp mismatch
        else
            DNA_nsh[index] = 0;            //Bp match
        index=index+1;
  }

      ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<1 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
        DNA_2[j]= DNA_ref[j] ^ (DNA_read[j]<<2);            //shift for 2bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_2[i] || DNA_2[i+1])
            DNA_shl_one[index] = 1;            //Bp mismatch
        else
            DNA_shl_one[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shl_one[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 

     ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<2 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
        DNA_3[j]= DNA_ref[j] ^ (DNA_read[j]<<4);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_3[i] || DNA_3[i+1])
            DNA_shl_two[index] = 1;            //Bp mismatch
        else
            DNA_shl_two[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shl_two[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_two[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<3 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
        DNA_6[j]= DNA_ref[j] ^ (DNA_read[j]<<6);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
      if(DNA_6[i] || DNA_6[i+1])
          DNA_shl_three[index] = 1;            //Bp mismatch
      else
          DNA_shl_three[index] = 0;            //Bp match
      index=index+1;
    }
    DNA_shl_three[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_three[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shl_three[(LENGTH/2)-3]=1;        //referencce bp mapped to empty bp due to shifting                
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<4 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
    DNA_7[j]= DNA_ref[j] ^ (DNA_read[j]<<8);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
      if(DNA_7[i] || DNA_7[i+1])
          DNA_shl_four[index] = 1;            //Bp mismatch
      else
          DNA_shl_four[index] = 0;            //Bp match
      index=index+1;
    }
    DNA_shl_four[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four[(LENGTH/2)-3]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four[(LENGTH/2)-4]=1;        //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<5 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
      DNA_8[j]= DNA_ref[j] ^ (DNA_read[j]<<10);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
      if(DNA_8[i] || DNA_8[i+1])
          DNA_shl_five[index] = 1;            //Bp mismatch
      else
          DNA_shl_five[index] = 0;            //Bp match
      index=index+1;
    }
    DNA_shl_five[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shl_five[(LENGTH/2)-3]=1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five[(LENGTH/2)-4]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shl_five[(LENGTH/2)-5]=1;        //referencce bp mapped to empty bp due to shifting             
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>1 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
    DNA_4[j]= DNA_ref[j] ^ (DNA_read[j]>>2);            //shift for 2bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_4[i] || DNA_4[i+1])
            DNA_shr_one[index] = 1;            //Bp mismatch
        else
            DNA_shr_one[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shr_one[0]=1;        //referencce bp mapped to empty bp due to shifting  
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>2 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
    DNA_5[j]= DNA_ref[j] ^ (DNA_read[j]>>4);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_5[i] || DNA_5[i+1])
            DNA_shr_two[index] = 1;            //Bp mismatch
        else
            DNA_shr_two[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shr_two[0]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_two[1]=1;        //referencce bp mapped to empty bp due to shifting
        
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>3 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
    DNA_9[j]= DNA_ref[j] ^ (DNA_read[j]>>6);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_9[i] || DNA_9[i+1])
            DNA_shr_three[index] = 1;            //Bp mismatch
        else
            DNA_shr_three[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shr_three[0]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_three[1]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_three[2]=1;        //referencce bp mapped to empty bp due to shifting
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>4 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
    DNA_10[j]= DNA_ref[j] ^ (DNA_read[j]>>8);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_10[i] || DNA_10[i+1])
            DNA_shr_four[index] = 1;            //Bp mismatch
        else
            DNA_shr_four[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shr_four[0]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_four[1]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_four[2]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_four[3]=1;        //referencce bp mapped to empty bp due to shifting
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>5 & Reference
    //
    ////////////////////////////////////////////////////////
    for (int j=0;j<LENGTH;j++)
    DNA_11[j]= DNA_ref[j] ^ (DNA_read[j]>>10);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    {
        if(DNA_11[i] || DNA_11[i+1])
            DNA_shr_five[index] = 1;            //Bp mismatch
        else
            DNA_shr_five[index] = 0;            //Bp match
        index=index+1;
    }
    DNA_shr_five[0]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[1]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[2]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[3]=1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[4]=1;        //referencce bp mapped to empty bp due to shifting       
}
int count_one(d_in_type &input_seq, int read_length)
{
    int index=0;

    for (index=0;index<LENGTH;index++)
    {
        if(input_seq[index]==1)
        break;

    }
    return index;
}

int SneakySnake(int ReadLength, d_out_type &RefSeq, d_out_type & ReadSeq, int EditThreshold, int KmerSize)
{
    int Accepted=1;
    int n;
    int e;
    int K;
    int index=0;
    int Edits=0;
    int count = 0;
    int GlobalCount=0;
    
    int KmerStart=0;
    int KmerEnd=0;
    
    int roundsNo=0;

    // d_out_type  DNA_shl_one,  DNA_shl_two,d_out_type & DNA_shl_three,d_out_type & DNA_shl_four,d_out_type & DNA_shl_five, 
 //    d_out_type  DNA_shr_one,d_out_type & DNA_shr_two,d_out_type & DNA_shr_three,d_out_type & DNA_shr_four,d_out_type & DNA_shr_five
    
    d_out_type DNA_nsh[LENGTH/2], DNA_shl_one[LENGTH/2], DNA_shl_two[LENGTH/2], DNA_shl_three[LENGTH/2], DNA_shl_four[LENGTH/2], DNA_shl_five[LENGTH/2],
    DNA_shr_one[LENGTH/2], DNA_shr_two[LENGTH/2], DNA_shr_three[LENGTH/2], DNA_shr_four[LENGTH/2], DNA_shr_five[LENGTH/2];
    
    int large_count=0,global_count=0;
    int counter_A[11];

    //below function creates the neighbourhood for 2E+1 with E=5

    NeighborhoodMap(LENGTH,
        ReadSeq, RefSeq,
        DNA_nsh,
        DNA_shl_one, DNA_shl_two, DNA_shl_three, DNA_shl_four, DNA_shl_five, 
        DNA_shr_one, DNA_shr_two, DNA_shr_three, DNA_shr_four, DNA_shr_five);

    for (int iter=0;iter<3;iter++)
    {
    counter_A[0]=count_one(DNA_nsh,LENGTH);
    counter_A[1]=count_one(DNA_shl_one,LENGTH);
    counter_A[2]=count_one(DNA_shl_two,LENGTH);
    counter_A[3]=count_one(DNA_shl_three,LENGTH);
    counter_A[4]=count_one(DNA_shl_four,LENGTH);
    counter_A[5]=count_one(DNA_shl_five,LENGTH);
    counter_A[6]=count_one(DNA_shr_one,LENGTH);
    counter_A[7]=count_one(DNA_shr_two,LENGTH);
    counter_A[8]=count_one(DNA_shr_three,LENGTH);
    counter_A[9]=count_one(DNA_shr_four,LENGTH);
    counter_A[10]=count_one(DNA_shr_five,LENGTH);
    
    large_count=largest(counter_A, 11);
    global_count+=large_count

    *DNA_nsh=*DNA_nsh<<large_count;
    *DNA_shl_one=*DNA_shl_one<<large_count;
    *DNA_shl_two=*DNA_shl_two<<large_count;
    *DNA_shl_three=*DNA_shl_three<<large_count;
    *DNA_shl_four=*DNA_shl_four<<large_count;
    *DNA_shl_five=*DNA_shl_five<<large_count;
    *DNA_shr_one=*DNA_shr_one<<large_count;
    *DNA_shr_two=*DNA_shr_two<<large_count;
    *DNA_shr_three=*DNA_shr_three<<large_count;
    *DNA_shr_four=*DNA_shr_four<<large_count;
    *DNA_shr_five=*DNA_shr_five<<large_count;
    }

}
*/

unsigned largest(unsigned arr[], unsigned n) 
{ 
    unsigned i; 
      
    // Initialize maximum element 
    unsigned max = arr[0]; 
  
    // Traverse array elements  
    // from second and compare 
    // every element with current max  
    for (i = 1; i < 11; i++) 
        if (arr[i] > max) 
            max = arr[i]; 
  
    return max; 
} 



unsigned count_one_bit(d_bit_out_tiny_type &input_seq, unsigned seq_length)
{
    unsigned index=0;

    for (index=0;index<8;index++)
    {
        #pragma HLS PIPELINE
        if(input_seq.range((index+1)-1,(index))==1)
        break;

    }
    return index;
}



int largest_bit(d_out_type arr, int n) 
{ 
    int i; 
      
    // Initialize maximum element 
    int max = arr(sizeof(d_in_type)*(0)+1,sizeof(d_in_type)*(0)); 
  
    // Traverse array elements  
    // from second and compare 
    // every element with current max  
    for (i = 1; i < n; i++) 
        if (arr(sizeof(d_in_type)*(i+1)-1,sizeof(d_in_type)*(i)) > max) 
            max = arr[i]; 
  
    return max; 
} 

void NeighborhoodMap_bit(int read_length,
    d_bit_in_type & DNA_read, d_bit_in_type & DNA_ref, 
    d_bit_out_type & DNA_nsh,
    d_bit_out_type & DNA_shl_one,d_bit_out_type & DNA_shl_two,d_bit_out_type & DNA_shl_three,d_bit_out_type & DNA_shl_four,d_bit_out_type & DNA_shl_five, 
    d_bit_out_type & DNA_shr_one,d_bit_out_type & DNA_shr_two,d_bit_out_type & DNA_shr_three,d_bit_out_type & DNA_shr_four,d_bit_out_type & DNA_shr_five)
{
    printf("NeighborhoodMap\n" );
    d_bit_in_type DNA_1, DNA_2, DNA_3, DNA_4, DNA_5, DNA_6,
     DNA_7, DNA_8, DNA_9, DNA_10, DNA_11;

     //   d_bit_in_type DNA_1, DNA_2, DNA_3, DNA_4, DNA_5, DNA_6,
     // DNA_7, DNA_8, DNA_9, DNA_10, DNA_11;
    int index, i;
    printf("DNA_ref %f\n", DNA_ref.to_float());
    DNA_1= DNA_ref ^ DNA_read;
    index=0;


 


    for (i = 0; i < LENGTH-2; i = i + 2)
    {
        // printf("base read=%d\n",i);
        // printf("row read=%d\n",index);
        #pragma HLS PIPELINE
        if(DNA_1((i+1),i) || DNA_1((i+2),(i+1)))
            DNA_nsh((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_nsh((index+1),index)  = 0;            //Bp match
        index=index+1;

    }
    printf("DNA_nsh %f\n", DNA_nsh.to_float());
     ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<1 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_2= DNA_ref ^ (DNA_read<<2);            //shift for 2bits coz each SNP is encoded by two bits
    index=0;
      for (i = 0; i < LENGTH-2; i = i + 2)
    {
    #pragma HLS PIPELINE
        if(DNA_2((i+1),i) || DNA_2((i+2),(i+1)))
            DNA_shl_one((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shl_one((index+1),index)  = 0;            //Bp match
        index=index+1;
    }

    DNA_shl_one((LENGTH/2)-1,(LENGTH/2)-2) =1;        //referencce bp mapped to empty bp due to shifting 
    printf("DNA_shl_one %f\n", DNA_shl_one.to_float());
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<2 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_3= DNA_ref ^ (DNA_read<<4);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH-2; i = i + 2)
    {
    #pragma HLS PIPELINE
        if(DNA_3((i+1),i) || DNA_3((i+2),(i+1)))
            DNA_shl_two((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shl_two((index+1),index)  = 0;            //Bp match
        index=index+1;
    }
    DNA_shl_two((LENGTH/2)-1,(LENGTH/2)-2) =1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_two((LENGTH/2)-3,(LENGTH/2)-4) =1;       //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<3 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_6= DNA_ref ^ (DNA_read<<6);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
      for (i = 0; i < LENGTH-2; i = i + 2)
    {
    #pragma HLS PIPELINE
        if(DNA_6((i+1),i) || DNA_6((i+2),(i+1)))
            DNA_shl_three((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shl_three((index+1),index)  = 0;            //Bp match
        index=index+1;
    }
    DNA_shl_three((LENGTH/2)-1,(LENGTH/2)-2) =1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_three((LENGTH/2)-3,(LENGTH/2)-4) =1;       //referencce bp mapped to empty bp due to shifting 
    DNA_shl_three((LENGTH/2)-5,(LENGTH/2)-6) =1;       //referencce bp mapped to empty bp due to shifting 
  
    // DNA_shl_three[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    // DNA_shl_three[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shl_three[(LENGTH/2)-3]=1;        //referencce bp mapped to empty bp due to shifting                
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<4 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_7= DNA_ref ^ (DNA_read<<8);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
      for (i = 0; i < LENGTH-2; i = i + 2)
    {
    #pragma HLS PIPELINE
        if(DNA_7((i+1),i) || DNA_7((i+2),(i+1)))
            DNA_shl_four((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shl_four((index+1),index)  = 0;            //Bp match
        index=index+1;
    }
    DNA_shl_four((LENGTH/2)-1,(LENGTH/2)-2) =1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four((LENGTH/2)-3,(LENGTH/2)-4) =1;       //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four((LENGTH/2)-5,(LENGTH/2)-6) =1;       //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four((LENGTH/2)-7,(LENGTH/2)-8) =1;       //referencce bp mapped to empty bp due to shifting 
  
    // DNA_shl_four[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    // DNA_shl_four[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting 
    // DNA_shl_four[(LENGTH/2)-3]=1;        //referencce bp mapped to empty bp due to shifting 
    // DNA_shl_four[(LENGTH/2)-4]=1;        //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<5 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_8= DNA_ref ^ (DNA_read<<10);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
        for (i = 0; i < LENGTH-2; i = i + 2)
    {
        #pragma HLS PIPELINE
        if(DNA_8((i+1),i) || DNA_8((i+2),(i+1)))
            DNA_shl_five((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shl_five((index+1),index)  = 0;            //Bp match
        index=index+1;
     }
        DNA_shl_five((LENGTH/2)-1,(LENGTH/2)-2) =1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five((LENGTH/2)-3,(LENGTH/2)-4) =1;       //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five((LENGTH/2)-5,(LENGTH/2)-6) =1;       //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five((LENGTH/2)-7,(LENGTH/2)-8) =1;       //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five((LENGTH/2)-9,(LENGTH/2)-10) =1;       //referencce bp mapped to empty bp due to shifting 
  
    // DNA_shl_five[(LENGTH/2)-1]=1;        //referencce bp mapped to empty bp due to shifting 
    // DNA_shl_five[(LENGTH/2)-2]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shl_five[(LENGTH/2)-3]=1;        //referencce bp mapped to empty bp due to shifting 
    // DNA_shl_five[(LENGTH/2)-4]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shl_five[(LENGTH/2)-5]=1;        //referencce bp mapped to empty bp due to shifting             
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>1 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_4= DNA_ref ^ (DNA_read>>2);            //shift for 2bits coz each SNP is encoded by two bits
    index=0;
         for (i = 0; i < LENGTH-2; i = i + 2)
    {
        #pragma HLS PIPELINE
        if(DNA_4((i+1),i) || DNA_4((i+2),(i+1)))
            DNA_shr_one((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shr_one((index+1),index)  = 0;            //Bp match
        index=index+1;
    }
        DNA_shr_one(1,0) =1;         //referencce bp mapped to empty bp due to shifting  
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>2 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_5= DNA_ref ^ (DNA_read>>4);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
         for (i = 0; i < LENGTH-2; i = i + 2)
    {
        #pragma HLS PIPELINE
        if(DNA_5((i+1),i) || DNA_5((i+2),(i+1)))
            DNA_shr_two((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shr_two((index+1),index)  = 0;            //Bp match
        index=index+1;
     }
      DNA_shr_two(1,0) =1;         //referencce bp mapped to empty bp due to shifting  
       DNA_shr_two(2,1) =1;         //referencce bp mapped to empty bp due to shifting  
        
    // DNA_shr_two[0]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_two[1]=1;        //referencce bp mapped to empty bp due to shifting
        
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>3 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_9= DNA_ref ^ (DNA_read>>6);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
         for (i = 0; i < LENGTH-2; i = i + 2)
    {
        #pragma HLS PIPELINE
        if(DNA_9((i+1),i) || DNA_9((i+2),(i+1)))
            DNA_shr_three((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shr_three((index+1),index)  = 0;            //Bp match
        index=index+1;
  }
    DNA_shr_three(1,0) =1;         //referencce bp mapped to empty bp due to shifting  
       DNA_shr_three(2,1) =1;         //referencce bp mapped to empty bp due to shifting  
       DNA_shr_three(3,2) =1;         //referencce bp mapped to empty bp due to shifting  
          
    // DNA_shr_three[0]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_three[1]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_three[2]=1;        //referencce bp mapped to empty bp due to shifting
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>4 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_10= DNA_ref ^ (DNA_read>>8);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
        for (i = 0; i < LENGTH-2; i = i + 2)
    {
        #pragma HLS PIPELINE
        if(DNA_10((i+1),i) || DNA_10((i+2),(i+1)))
            DNA_shr_four((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shr_four((index+1),index)  = 0;            //Bp match
        index=index+1;
  }

    DNA_shr_four(1,0) =1;         //referencce bp mapped to empty bp due to shifting  
       DNA_shr_four(2,1) =1;         //referencce bp mapped to empty bp due to shifting  
       DNA_shr_four(3,2) =1;         //referencce bp mapped to empty bp due to shifting  
         DNA_shr_four(4,3) =1;         //referencce bp mapped to empty bp due to shifting  
        
    // DNA_shr_four[0]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_four[1]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_four[2]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_four[3]=1;        //referencce bp mapped to empty bp due to shifting
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>5 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_11= DNA_ref ^ (DNA_read>>10);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
         for (i = 0; i < LENGTH-2; i = i + 2)
    {
        #pragma HLS PIPELINE
        if(DNA_11((i+1),i) || DNA_11((i+2),(i+1)))
            DNA_shr_five((index+1),index)  = 1;            //Bp mismatch
        else
            DNA_shr_five((index+1),index)  = 0;            //Bp match
          index=index+1;
    }

    DNA_shr_five(1,0) =1;         //referencce bp mapped to empty bp due to shifting  
    DNA_shr_five(2,1) =1;         //referencce bp mapped to empty bp due to shifting  
    DNA_shr_five(3,2) =1;         //referencce bp mapped to empty bp due to shifting  
    DNA_shr_five(4,3) =1;         //referencce bp mapped to empty bp due to shifting  
    DNA_shr_five(5,4) =1;         //referencce bp mapped to empty bp due to shifting  

    // DNA_shr_five[0]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_five[1]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_five[2]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_five[3]=1;        //referencce bp mapped to empty bp due to shifting
    // DNA_shr_five[4]=1;        //referencce bp mapped to empty bp due to shifting       
     
}

unsigned after_neighbohood(d_bit_out_tiny_type  DNA_nsh,
    d_bit_out_tiny_type  DNA_shl_one,d_bit_out_tiny_type  DNA_shl_two,d_bit_out_tiny_type  DNA_shl_three,d_bit_out_tiny_type DNA_shl_four,d_bit_out_tiny_type DNA_shl_five, 
    d_bit_out_tiny_type  DNA_shr_one,d_bit_out_tiny_type  DNA_shr_two,d_bit_out_tiny_type  DNA_shr_three,d_bit_out_tiny_type DNA_shr_four,d_bit_out_tiny_type  DNA_shr_five)
{
    d_final_out_type global_count=0;
    unsigned counter_A[11];

    unsigned large_count=0;
    //count leading zeros
     for (int iter=0;iter<3;iter++)
    {
        // printf("Running iteraiont=%d\n",iter );
        #pragma HLS PIPELINE
        counter_A[0]=count_one_bit(DNA_nsh,LENGTH/2);
        counter_A[1]=count_one_bit(DNA_shl_one,LENGTH/2);
        counter_A[2]=count_one_bit(DNA_shl_two,LENGTH/2);
        counter_A[3]=count_one_bit(DNA_shl_three,LENGTH/2);
        counter_A[4]=count_one_bit(DNA_shl_four,LENGTH/2);
        counter_A[5]=count_one_bit(DNA_shl_five,LENGTH/2);
        counter_A[6]=count_one_bit(DNA_shr_one,LENGTH/2);
        counter_A[7]=count_one_bit(DNA_shr_two,LENGTH/2);
        counter_A[8]=count_one_bit(DNA_shr_three,LENGTH/2);
        counter_A[9]=count_one_bit(DNA_shr_four,LENGTH/2);
        counter_A[10]=count_one_bit(DNA_shr_five,LENGTH/2);
        
        //find the largest
        large_count=largest(counter_A, 11);
        global_count+=large_count+1;
        if(global_count>=8)
            break;
        printf("Current iteration=%d global_count=%d\n",iter,global_count);
        //shift all the rows by the largest
        DNA_nsh=DNA_nsh<<large_count;
        DNA_shl_one=DNA_shl_one<<large_count;
        DNA_shl_two=DNA_shl_two<<large_count;
        DNA_shl_three=DNA_shl_three<<large_count;
        DNA_shl_four=DNA_shl_four<<large_count;
        DNA_shl_five=DNA_shl_five<<large_count;
        DNA_shr_one=DNA_shr_one<<large_count;
        DNA_shr_two=DNA_shr_two<<large_count;
        DNA_shr_three=DNA_shr_three<<large_count;
        DNA_shr_four=DNA_shr_four<<large_count;
        DNA_shr_five=DNA_shr_five<<large_count;

    }
    return global_count;
}

int SneakySnake_bit(int ReadLength, d_bit_in_type &ReadSeq, d_bit_in_type &RefSeq, int EditThreshold, int KmerSize)
{
    // int Accepted=1;
    // int n;
    // int e;
    // int K;
    // int index=0;
    // int Edits=0;
    // int count = 0;
    // int GlobalCount=0;
    
    // int KmerStart=0;
    // int KmerEnd=0;
    
    // int roundsNo=0;

    #pragma HLS INLINE OFF
    d_bit_out_type DNA_nsh, DNA_shl_one, DNA_shl_two, DNA_shl_three, DNA_shl_four, DNA_shl_five,
    DNA_shr_one, DNA_shr_two, DNA_shr_three, DNA_shr_four, DNA_shr_five;
       
    
    //below function creates the neighbourhood for 2E+1 with E=5
    NeighborhoodMap_bit(ReadLength,
        ReadSeq, RefSeq,
        DNA_nsh,
        DNA_shl_one, DNA_shl_two, DNA_shl_three, DNA_shl_four, DNA_shl_five, 
        DNA_shr_one, DNA_shr_two, DNA_shr_three, DNA_shr_four, DNA_shr_five);
    d_final_out_type global_count=0;
        for(unsigned i=0; i<12;i++)
        {

        global_count+= after_neighbohood(DNA_nsh.range(8*i-1,8*i),
        DNA_shl_one.range(8*i-1,8*i), DNA_shl_two.range(8*i-1,8*i), DNA_shl_three.range(8*i-1,8*i), DNA_shl_four.range(8*i-1,8*i), DNA_shl_five.range(8*i-1,8*i), 
        DNA_shr_one.range(8*i-1,8*i), DNA_shr_two.range(8*i-1,8*i), DNA_shr_three.range(8*i-1,8*i), DNA_shr_four.range(8*i-1,8*i), DNA_shr_five.range(8*i-1,8*i));
       
       }

    
    return global_count;
}
void SneakySnake_stream(hls::stream<snap_membus_256_t> &ReadSeq_stream, hls::stream<snap_membus_256_t> &RefSeq_stream, int num_input_lines, g_count_struct &g_count)
{
    #pragma HLS INLINE OFF
    printf("Running sneaky snake for line=%d\n",num_input_lines/2 );
    snap_membus_256_t RefSeq_256_t, ReadSeq_256_t;
    d_bit_in_type ReadSeq,RefSeq;

    g_count_struct *temp_ptr=&g_count;
    d_final_out_type *temp_count=NULL;

    temp_count=(d_final_out_type *)&temp_ptr->global_count[0];

    for (unsigned i = 0 ;i < num_input_lines/2; i ++)
    {
        printf("Running sequence: %d\n", i);
        #pragma HLS PIPELINE
        RefSeq_256_t= RefSeq_stream.read();
        RefSeq=RefSeq_256_t.range(200,0);
        ReadSeq_256_t= ReadSeq_stream.read();
        ReadSeq=ReadSeq_256_t.range(200,0);
        *(temp_count+ ( i ))=SneakySnake_bit( LENGTH, ReadSeq, RefSeq,  0,  0);
    }
}
