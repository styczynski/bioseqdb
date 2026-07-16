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


module SneakySnake_8bit (
	input CLK,
	input [0:3] SumIn,
	input [0:1] EditIn,
	input [0:7] A,B,C,D,E,F,G,H,I,J,K,
	output reg [0:7] Ao,Bo,Co,Do,Eo,Fo,Go,Ho,Io,Jo,Ko,
	output reg [0:3] SumOut,
	output reg [0:1] EditOut,
	output reg [0:1] Lives
	);
	
	wire L3, L2, L1, L0;
	
LongestLZC_8Bit LongestLZC_8Bit_inst1(
	.a(A),.b(B),.c(C),.d(D),.e(E),.f(F),.g(G),.h(H),.i(I),.j(J),.k(K),
	.L3(L3), .L2(L2), .L1(L1), .L0(L0)
);	

always @(posedge CLK) 
begin
	if ({L0,L1,L2,L3}==4'b0001)// if no available path
	begin
		Ao = {A[1:7],(1'b1)};
		Bo = {B[1:7],(1'b1)};
		Co = {C[1:7],(1'b1)};
		Do = {D[1:7],(1'b1)};
		Eo = {E[1:7],(1'b1)};
		Fo = {F[1:7],(1'b1)};
		Go = {G[1:7],(1'b1)};
		Ho = {H[1:7],(1'b1)};
		Io = {I[1:7],(1'b1)};
		Jo = {J[1:7],(1'b1)};
		Ko = {K[1:7],(1'b1)};
		SumOut = SumIn ;
	end
	else if ({L0,L1,L2,L3}==4'b1001)
	begin
		Ao = {A[2:7],(2'b11)};
		Bo = {B[2:7],(2'b11)};
		Co = {C[2:7],(2'b11)};
		Do = {D[2:7],(2'b11)};
		Eo = {E[2:7],(2'b11)};
		Fo = {F[2:7],(2'b11)};
		Go = {G[2:7],(2'b11)};
		Ho = {H[2:7],(2'b11)};
		Io = {I[2:7],(2'b11)};
		Jo = {J[2:7],(2'b11)};
		Ko = {K[2:7],(2'b11)};
		SumOut = SumIn + 1'b1;
	end
	else if ({L0,L1,L2,L3}==4'b0101)
	begin
		Ao = {A[3:7],(3'b111)};
		Bo = {B[3:7],(3'b111)};
		Co = {C[3:7],(3'b111)};
		Do = {D[3:7],(3'b111)};
		Eo = {E[3:7],(3'b111)};
		Fo = {F[3:7],(3'b111)};
		Go = {G[3:7],(3'b111)};
		Ho = {H[3:7],(3'b111)};
		Io = {I[3:7],(3'b111)};
		Jo = {J[3:7],(3'b111)};
		Ko = {K[3:7],(3'b111)};
		SumOut = SumIn + 2;
	end
	else if ({L0,L1,L2,L3}==4'b1101)
	begin
		Ao = {A[4:7],(4'b1111)};
		Bo = {B[4:7],(4'b1111)};
		Co = {C[4:7],(4'b1111)};
		Do = {D[4:7],(4'b1111)};
		Eo = {E[4:7],(4'b1111)};
		Fo = {F[4:7],(4'b1111)};
		Go = {G[4:7],(4'b1111)};
		Ho = {H[4:7],(4'b1111)};
		Io = {I[4:7],(4'b1111)};
		Jo = {J[4:7],(4'b1111)};
		Ko = {K[4:7],(4'b1111)};
		SumOut = SumIn + 3;
	end
	else if ({L0,L1,L2,L3}==4'b0011)
	begin
		Ao = {A[5:7],(5'b11111)};
		Bo = {B[5:7],(5'b11111)};
		Co = {C[5:7],(5'b11111)};
		Do = {D[5:7],(5'b11111)};
		Eo = {E[5:7],(5'b11111)};
		Fo = {F[5:7],(5'b11111)};
		Go = {G[5:7],(5'b11111)};
		Ho = {H[5:7],(5'b11111)};
		Io = {I[5:7],(5'b11111)};
		Jo = {J[5:7],(5'b11111)};
		Ko = {K[5:7],(5'b11111)};
		SumOut = SumIn + 4;
	end
	else if ({L0,L1,L2,L3}==4'b1011)
	begin
		Ao = {A[6:7],(6'b111111)};
		Bo = {B[6:7],(6'b111111)};
		Co = {C[6:7],(6'b111111)};
		Do = {D[6:7],(6'b111111)};
		Eo = {E[6:7],(6'b111111)};
		Fo = {F[6:7],(6'b111111)};
		Go = {G[6:7],(6'b111111)};
		Ho = {H[6:7],(6'b111111)};
		Io = {I[6:7],(6'b111111)};
		Jo = {J[6:7],(6'b111111)};
		Ko = {K[6:7],(6'b111111)};
		SumOut = SumIn + 5;
	end
	else if ({L0,L1,L2,L3}==4'b0111)
	begin
		Ao = {A[7],(7'b1111111)};
		Bo = {B[7],(7'b1111111)};
		Co = {C[7],(7'b1111111)};
		Do = {D[7],(7'b1111111)};
		Eo = {E[7],(7'b1111111)};
		Fo = {F[7],(7'b1111111)};
		Go = {G[7],(7'b1111111)};
		Ho = {H[7],(7'b1111111)};
		Io = {I[7],(7'b1111111)};
		Jo = {J[7],(7'b1111111)};
		Ko = {K[7],(7'b1111111)};
		SumOut = SumIn + 6;
	end
	else
	begin
		Ao = (8'b11111111);
		Bo = (8'b11111111);
		Co = (8'b11111111);
		Do = (8'b11111111);
		Eo = (8'b11111111);
		Fo = (8'b11111111);
		Go = (8'b11111111);
		Ho = (8'b11111111);
		Io = (8'b11111111);
		Jo = (8'b11111111);
		Ko = (8'b11111111);
		if (L3==1)
			SumOut = 7;
		else
			SumOut = 8;
	end
	if ({L0,L1,L2,L3}!=4'b0001)// if no available path
		Lives = EditIn + 1;
	else 
		Lives = EditIn;
	if (Lives==3)
		EditOut=3;
	else
	begin
		if (SumOut == 8)
			EditOut=0;
		else if (SumOut == 7)
			EditOut=1;
		else if (SumOut == 6)
			EditOut=2;
		else 
			EditOut=3;
	end
end
	
endmodule
	
