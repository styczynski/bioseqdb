/*
 * Copyright (c) <2017 - 2030>, ETH Zurich and Bilkent University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this list
 * of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice, this
 *   list of conditions and the following disclaimer in the documentation and/or other
 *   materials provided with the distribution.
 * - Neither the names of the ETH Zurich, Bilkent University,
 *   nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without specific
 *   prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

  Authors: 
  Mohammed Alser
	  mohammed.alser AT inf DOT ethz DOT ch
  Date:
  September 22nd, 2019
*/




`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: 
// 
// Create Date: 07/17/2018 03:11:55 PM
// Design Name: 
// Module Name: NeighborhoodMap
// Project Name: 
// Target Devices: 
// Tool Versions: 
// Description: 
// 
// Dependencies: 
// 
// Revision:
// Revision 0.01 - File Created
// Additional Comments:
// 
//////////////////////////////////////////////////////////////////////////////////


module NeighborhoodMap #(parameter LENGTH = 32) (
	input CLK,
    input [0:LENGTH-1] DNA_read, DNA_ref,
    output reg [0:(LENGTH/2)-1] DNA_nsh,
    DNA_shl_one,DNA_shl_two,DNA_shl_three,DNA_shl_four,DNA_shl_five, 
    DNA_shr_one,DNA_shr_two,DNA_shr_three,DNA_shr_four,DNA_shr_five
);

reg [0:LENGTH-1] DNA_1, DNA_2, DNA_3, DNA_4, DNA_5, DNA_6, DNA_7, DNA_8, DNA_9, DNA_10, DNA_11;
integer index, i;

//////////////////////////////////////////////////////////
//
//                Shifted Hamming Mask (SHM) Calculation
//
//////////////////////////////////////////////////////////

always@(posedge CLK)
begin
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_1= DNA_ref ^ DNA_read;
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_1[i] || DNA_1[i+1])
            DNA_nsh[index] = 1'b1;            //Bp mismatch
        else
            DNA_nsh[index] = 1'b0;            //Bp match
        index=index+1;
    end   
    
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<1 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_2= DNA_ref ^ (DNA_read<<2);            //shift for 2bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_2[i] || DNA_2[i+1])
            DNA_shl_one[index] = 1'b1;            //Bp mismatch
        else
            DNA_shl_one[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shl_one[(LENGTH/2)-1]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<2 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_3= DNA_ref ^ (DNA_read<<4);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_3[i] || DNA_3[i+1])
            DNA_shl_two[index] = 1'b1;            //Bp mismatch
        else
            DNA_shl_two[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shl_two[(LENGTH/2)-1]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_two[(LENGTH/2)-2]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<3 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_6= DNA_ref ^ (DNA_read<<6);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
      if(DNA_6[i] || DNA_6[i+1])
          DNA_shl_three[index] = 1'b1;            //Bp mismatch
      else
          DNA_shl_three[index] = 1'b0;            //Bp match
      index=index+1;
    end
    DNA_shl_three[(LENGTH/2)-1]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_three[(LENGTH/2)-2]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shl_three[(LENGTH/2)-3]=1'b1;        //referencce bp mapped to empty bp due to shifting				
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<4 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_7= DNA_ref ^ (DNA_read<<8);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
      if(DNA_7[i] || DNA_7[i+1])
          DNA_shl_four[index] = 1'b1;            //Bp mismatch
      else
          DNA_shl_four[index] = 1'b0;            //Bp match
      index=index+1;
    end
    DNA_shl_four[(LENGTH/2)-1]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four[(LENGTH/2)-2]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four[(LENGTH/2)-3]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_four[(LENGTH/2)-4]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read<<5 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_8= DNA_ref ^ (DNA_read<<10);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
      if(DNA_8[i] || DNA_8[i+1])
          DNA_shl_five[index] = 1'b1;            //Bp mismatch
      else
          DNA_shl_five[index] = 1'b0;            //Bp match
      index=index+1;
    end
    DNA_shl_five[(LENGTH/2)-1]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five[(LENGTH/2)-2]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shl_five[(LENGTH/2)-3]=1'b1;        //referencce bp mapped to empty bp due to shifting 
    DNA_shl_five[(LENGTH/2)-4]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shl_five[(LENGTH/2)-5]=1'b1;        //referencce bp mapped to empty bp due to shifting 			
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>1 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_4= DNA_ref ^ (DNA_read>>2);            //shift for 2bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_4[i] || DNA_4[i+1])
            DNA_shr_one[index] = 1'b1;            //Bp mismatch
        else
            DNA_shr_one[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shr_one[0]=1'b1;        //referencce bp mapped to empty bp due to shifting  
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>2 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_5= DNA_ref ^ (DNA_read>>4);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_5[i] || DNA_5[i+1])
            DNA_shr_two[index] = 1'b1;            //Bp mismatch
        else
            DNA_shr_two[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shr_two[0]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_two[1]=1'b1;        //referencce bp mapped to empty bp due to shifting
        
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>3 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_9= DNA_ref ^ (DNA_read>>6);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_9[i] || DNA_9[i+1])
            DNA_shr_three[index] = 1'b1;            //Bp mismatch
        else
            DNA_shr_three[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shr_three[0]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_three[1]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_three[2]=1'b1;        //referencce bp mapped to empty bp due to shifting
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>4 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_10= DNA_ref ^ (DNA_read>>8);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_10[i] || DNA_10[i+1])
            DNA_shr_four[index] = 1'b1;            //Bp mismatch
        else
            DNA_shr_four[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shr_four[0]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_four[1]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_four[2]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_four[3]=1'b1;        //referencce bp mapped to empty bp due to shifting
    
    ////////////////////////////////////////////////////////
    //
    //                Hamming Mask for Read>>5 & Reference
    //
    ////////////////////////////////////////////////////////
    DNA_11= DNA_ref ^ (DNA_read>>10);            //shift for 4bits coz each SNP is encoded by two bits
    index=0;
    for (i = 0; i < LENGTH; i = i + 2)
    begin
        if(DNA_11[i] || DNA_11[i+1])
            DNA_shr_five[index] = 1'b1;            //Bp mismatch
        else
            DNA_shr_five[index] = 1'b0;            //Bp match
        index=index+1;
    end
    DNA_shr_five[0]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[1]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[2]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[3]=1'b1;        //referencce bp mapped to empty bp due to shifting
    DNA_shr_five[4]=1'b1;        //referencce bp mapped to empty bp due to shifting       
     
end 

endmodule
