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



module LZC_Dimitra_8(
	B0,
	B1,
	B2,
	B3,
	B4,
	B5,
	B6,
	B7,
	Valid,
	C0,
	C1,
	C2
);


input wire	B0;
input wire	B1;
input wire	B2;
input wire	B3;
input wire	B4;
input wire	B5;
input wire	B6;
input wire	B7;
output wire	Valid;
output wire	C0;
output wire	C1;
output wire	C2;

wire	SYNTHESIZED_WIRE_0;
wire	SYNTHESIZED_WIRE_13;
wire	SYNTHESIZED_WIRE_2;
wire	SYNTHESIZED_WIRE_3;
wire	SYNTHESIZED_WIRE_14;
wire	SYNTHESIZED_WIRE_5;
wire	SYNTHESIZED_WIRE_6;
wire	SYNTHESIZED_WIRE_7;
wire	SYNTHESIZED_WIRE_9;
wire	SYNTHESIZED_WIRE_11;
wire	SYNTHESIZED_WIRE_12;

assign	C2 = SYNTHESIZED_WIRE_13;




LZC_Dimitra_4	b2v_inst(
	.B3(B3),
	.B2(B2),
	.B1(B1),
	.B0(B0),
	.P1(SYNTHESIZED_WIRE_11),
	.P0(SYNTHESIZED_WIRE_2),
	.Valid(SYNTHESIZED_WIRE_14));


LZC_Dimitra_4	b2v_inst1(
	.B3(B7),
	.B2(B6),
	.B1(B5),
	.B0(B4),
	.P1(SYNTHESIZED_WIRE_9),
	.P0(SYNTHESIZED_WIRE_0),
	.Valid(SYNTHESIZED_WIRE_5));

assign	SYNTHESIZED_WIRE_3 = SYNTHESIZED_WIRE_0 & SYNTHESIZED_WIRE_13;

assign	SYNTHESIZED_WIRE_7 = SYNTHESIZED_WIRE_2 | SYNTHESIZED_WIRE_3;

assign	Valid = SYNTHESIZED_WIRE_14 | SYNTHESIZED_WIRE_5;

assign	C1 =  ~SYNTHESIZED_WIRE_6;

assign	C0 =  ~SYNTHESIZED_WIRE_7;

assign	SYNTHESIZED_WIRE_13 =  ~SYNTHESIZED_WIRE_14;

assign	SYNTHESIZED_WIRE_12 = SYNTHESIZED_WIRE_9 & SYNTHESIZED_WIRE_13;

assign	SYNTHESIZED_WIRE_6 = SYNTHESIZED_WIRE_11 | SYNTHESIZED_WIRE_12;


endmodule
