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
// Create Date: 07/17/2018 03:09:21 PM
// Design Name: 
// Module Name: SneakySnake
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


module SneakySnake  #(parameter LENGTH = 200) (
	input CLK,
    input [0:LENGTH-1] DNA_read, DNA_ref, 
    output [0:1] EditOut1, EditOut2, EditOut3, EditOut4, EditOut5, EditOut6,
    output [0:1] EditOut7, EditOut8, EditOut9, EditOut10, EditOut11, EditOut12,  
    output reg Accepted,
    output [0:(LENGTH/2)-1]
    DNA_nsh,
    DNA_shl_one,
    DNA_shl_two,
    DNA_shr_one,
    DNA_shr_two,
    DNA_shl_three, DNA_shl_four, DNA_shl_five, DNA_shr_three, DNA_shr_four, DNA_shr_five
    
);

//wire [0:(LENGTH/2)-1] DNA_shl_three, DNA_shl_four, DNA_shl_five, DNA_shr_three, DNA_shr_four, DNA_shr_five;
//wire [0:(LENGTH/2)-1] DNA_nsh, DNA_shl_one, DNA_shl_two, DNA_shr_one, DNA_shr_two;

wire [0:(LENGTH/2)-1] DNA_shl_three_o, DNA_shl_four_o, DNA_shl_five_o, DNA_shr_three_o, DNA_shr_four_o, DNA_shr_five_o;
wire [0:(LENGTH/2)-1] DNA_nsh_o, DNA_shl_one_o, DNA_shl_two_o, DNA_shr_one_o, DNA_shr_two_o;

wire [0:(LENGTH/2)-1] DNA_shl_three_o1, DNA_shl_four_o1, DNA_shl_five_o1, DNA_shr_three_o1, DNA_shr_four_o1, DNA_shr_five_o1;
wire [0:(LENGTH/2)-1] DNA_nsh_o1, DNA_shl_one_o1, DNA_shl_two_o1, DNA_shr_one_o1, DNA_shr_two_o1;

wire [0:(LENGTH/2)-1] DNA_shl_three_o2, DNA_shl_four_o2, DNA_shl_five_o2, DNA_shr_three_o2, DNA_shr_four_o2, DNA_shr_five_o2;
wire [0:(LENGTH/2)-1] DNA_nsh_o2, DNA_shl_one_o2, DNA_shl_two_o2, DNA_shr_one_o2, DNA_shr_two_o2;

wire [0:3] Sum1,Sum2,Sum3,Sum4,Sum5,Sum6,Sum7,Sum8,Sum9,Sum10,Sum11,Sum12,Sum13,Sum14,Sum15;
wire [0:3] Sum16,Sum17,Sum18,Sum19,Sum20,Sum21,Sum22,Sum23,Sum24,Sum25,Sum26,Sum27,Sum28,Sum29,Sum30;
wire [0:3] Sum31,Sum32,Sum33,Sum34,Sum35,Sum36;

wire [0:1] Lives1,Lives2,Lives3,Lives4,Lives5,Lives6,Lives7,Lives8,Lives9,Lives10,Lives11,Lives12,Lives13,Lives14,Lives15;
wire [0:1] Lives16,Lives17,Lives18,Lives19,Lives20,Lives21,Lives22,Lives23,Lives24,Lives25,Lives26,Lives27,Lives28,Lives29,Lives30;
wire [0:1] Lives31,Lives32,Lives33,Lives34,Lives35,Lives36;


integer ErrorThreshold = 5;
reg [0:2] count, MaxZeros;
reg [0:6] MinEdits;



NeighborhoodMap #(.LENGTH(LENGTH)) NeighborhoodMap_inst (
	.CLK(CLK),
    .DNA_read(DNA_read), .DNA_ref(DNA_ref),
    .DNA_nsh(DNA_nsh),
    .DNA_shl_one(DNA_shl_one),.DNA_shl_two(DNA_shl_two),.DNA_shl_three(DNA_shl_three), .DNA_shl_four(DNA_shl_four), .DNA_shl_five(DNA_shl_five), 
    .DNA_shr_one(DNA_shr_one),.DNA_shr_two(DNA_shr_two),.DNA_shr_three(DNA_shr_three), .DNA_shr_four(DNA_shr_four), .DNA_shr_five(DNA_shr_five)
);

SneakySnake_8bit SneakySnake_8bit_inst1 (
	.CLK(CLK),
	.EditIn(0),
	.SumIn(0),
	.A(DNA_nsh[0:7]),.B(DNA_shl_one[0:7]),.C(DNA_shl_two[0:7]),.D(DNA_shl_three[0:7]),.E(DNA_shl_four[0:7]),.F(DNA_shl_five[0:7]),
	.G(DNA_shr_one[0:7]),.H(DNA_shr_two[0:7]),.I(DNA_shr_three[0:7]),.J(DNA_shr_four[0:7]),.K(DNA_shr_five[0:7]),
	.Ao(DNA_nsh_o[0:7]),.Bo(DNA_shl_one_o[0:7]),.Co(DNA_shl_two_o[0:7]),.Do(DNA_shl_three_o[0:7]),.Eo(DNA_shl_four_o[0:7]),.Fo(DNA_shl_five_o[0:7]),
	.Go(DNA_shr_one_o[0:7]),.Ho(DNA_shr_two_o[0:7]),.Io(DNA_shr_three_o[0:7]),.Jo(DNA_shr_four_o[0:7]),.Ko(DNA_shr_five_o[0:7]),
	.Lives(Lives1),
	.SumOut(Sum1)
);

SneakySnake_8bit SneakySnake_8bit_inst2 (
	.CLK(CLK),
	.EditIn(Lives1),
	.SumIn(Sum1),
	.A(DNA_nsh_o[0:7]),.B(DNA_shl_one_o[0:7]),.C(DNA_shl_two_o[0:7]),.D(DNA_shl_three_o[0:7]),.E(DNA_shl_four_o[0:7]),.F(DNA_shl_five_o[0:7]),
	.G(DNA_shr_one_o[0:7]),.H(DNA_shr_two_o[0:7]),.I(DNA_shr_three_o[0:7]),.J(DNA_shr_four_o[0:7]),.K(DNA_shr_five_o[0:7]),
	.Ao(DNA_nsh_o1[0:7]),.Bo(DNA_shl_one_o1[0:7]),.Co(DNA_shl_two_o1[0:7]),.Do(DNA_shl_three_o1[0:7]),.Eo(DNA_shl_four_o1[0:7]),.Fo(DNA_shl_five_o1[0:7]),
	.Go(DNA_shr_one_o1[0:7]),.Ho(DNA_shr_two_o1[0:7]),.Io(DNA_shr_three_o1[0:7]),.Jo(DNA_shr_four_o1[0:7]),.Ko(DNA_shr_five_o1[0:7]),
	.Lives(Lives2),
	.SumOut(Sum2)
);


SneakySnake_8bit SneakySnake_8bit_inst3 (
	.CLK(CLK),
	.SumIn(Sum2),
	.EditIn(Lives2),
	.A(DNA_nsh_o1[0:7]),.B(DNA_shl_one_o1[0:7]),.C(DNA_shl_two_o1[0:7]),.D(DNA_shl_three_o1[0:7]),.E(DNA_shl_four_o1[0:7]),.F(DNA_shl_five_o1[0:7]),
	.G(DNA_shr_one_o1[0:7]),.H(DNA_shr_two_o1[0:7]),.I(DNA_shr_three_o1[0:7]),.J(DNA_shr_four_o1[0:7]),.K(DNA_shr_five_o1[0:7]),
	//.Ao(DNA_nsh_o2[0:7]),.Bo(DNA_shl_one_o2[0:7]),.Co(DNA_shl_two_o2[0:7]),.Do(DNA_shl_three_o2[0:7]),.Eo(DNA_shl_four_o2[0:7]),.Fo(DNA_shl_five_o2[0:7]),
	//.Go(DNA_shr_one_o2[0:7]),.Ho(DNA_shr_two_o2[0:7]),.Io(DNA_shr_three_o2[0:7]),.Jo(DNA_shr_four_o2[0:7]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum3),
	.EditOut(EditOut1)
);


////////////////////////////////////////////////////////////////
SneakySnake_8bit SneakySnake_8bit_inst4 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[8:15]),.B(DNA_shl_one[8:15]),.C(DNA_shl_two[8:15]),.D(DNA_shl_three[8:15]),.E(DNA_shl_four[8:15]),.F(DNA_shl_five[8:15]),
	.G(DNA_shr_one[8:15]),.H(DNA_shr_two[8:15]),.I(DNA_shr_three[8:15]),.J(DNA_shr_four[8:15]),.K(DNA_shr_five[8:15]),
	.Ao(DNA_nsh_o[8:15]),.Bo(DNA_shl_one_o[8:15]),.Co(DNA_shl_two_o[8:15]),.Do(DNA_shl_three_o[8:15]),.Eo(DNA_shl_four_o[8:15]),.Fo(DNA_shl_five_o[8:15]),
	.Go(DNA_shr_one_o[8:15]),.Ho(DNA_shr_two_o[8:15]),.Io(DNA_shr_three_o[8:15]),.Jo(DNA_shr_four_o[8:15]),.Ko(DNA_shr_five_o[8:15]),
	.Lives(Lives4),
	.SumOut(Sum4)
);

SneakySnake_8bit SneakySnake_8bit_inst5 (
	.CLK(CLK),
	.SumIn(Sum4),
	.EditIn(Lives4),
	.A(DNA_nsh_o[8:15]),.B(DNA_shl_one_o[8:15]),.C(DNA_shl_two_o[8:15]),.D(DNA_shl_three_o[8:15]),.E(DNA_shl_four_o[8:15]),.F(DNA_shl_five_o[8:15]),
	.G(DNA_shr_one_o[8:15]),.H(DNA_shr_two_o[8:15]),.I(DNA_shr_three_o[8:15]),.J(DNA_shr_four_o[8:15]),.K(DNA_shr_five_o[8:15]),
	.Ao(DNA_nsh_o1[8:15]),.Bo(DNA_shl_one_o1[8:15]),.Co(DNA_shl_two_o1[8:15]),.Do(DNA_shl_three_o1[8:15]),.Eo(DNA_shl_four_o1[8:15]),.Fo(DNA_shl_five_o1[8:15]),
	.Go(DNA_shr_one_o1[8:15]),.Ho(DNA_shr_two_o1[8:15]),.Io(DNA_shr_three_o1[8:15]),.Jo(DNA_shr_four_o1[8:15]),.Ko(DNA_shr_five_o1[8:15]),
	.Lives(Lives5),
	.SumOut(Sum5)
);


SneakySnake_8bit SneakySnake_8bit_inst6 (
	.CLK(CLK),
	.SumIn(Sum5),
	.EditIn(Lives5),
	.A(DNA_nsh_o1[8:15]),.B(DNA_shl_one_o1[8:15]),.C(DNA_shl_two_o1[8:15]),.D(DNA_shl_three_o1[8:15]),.E(DNA_shl_four_o1[8:15]),.F(DNA_shl_five_o1[8:15]),
	.G(DNA_shr_one_o1[8:15]),.H(DNA_shr_two_o1[8:15]),.I(DNA_shr_three_o1[8:15]),.J(DNA_shr_four_o1[8:15]),.K(DNA_shr_five_o1[8:15]),
	//.Ao(DNA_nsh_o2[8:15]),.Bo(DNA_shl_one_o2[8:15]),.Co(DNA_shl_two_o2[8:15]),.Do(DNA_shl_three_o2[8:15]),.Eo(DNA_shl_four_o2[8:15]),.Fo(DNA_shl_five_o2[8:15]),
	//.Go(DNA_shr_one_o2[8:15]),.Ho(DNA_shr_two_o2[8:15]),.Io(DNA_shr_three_o2[8:15]),.Jo(DNA_shr_four_o2[8:15]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum6),
	.EditOut(EditOut2)
);
////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst7 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[16:23]),.B(DNA_shl_one[16:23]),.C(DNA_shl_two[16:23]),.D(DNA_shl_three[16:23]),.E(DNA_shl_four[16:23]),.F(DNA_shl_five[16:23]),
	.G(DNA_shr_one[16:23]),.H(DNA_shr_two[16:23]),.I(DNA_shr_three[16:23]),.J(DNA_shr_four[16:23]),.K(DNA_shr_five[16:23]),
	.Ao(DNA_nsh_o[16:23]),.Bo(DNA_shl_one_o[16:23]),.Co(DNA_shl_two_o[16:23]),.Do(DNA_shl_three_o[16:23]),.Eo(DNA_shl_four_o[16:23]),.Fo(DNA_shl_five_o[16:23]),
	.Go(DNA_shr_one_o[16:23]),.Ho(DNA_shr_two_o[16:23]),.Io(DNA_shr_three_o[16:23]),.Jo(DNA_shr_four_o[16:23]),.Ko(DNA_shr_five_o[16:23]),
	.Lives(Lives7),
	.SumOut(Sum7)
);

SneakySnake_8bit SneakySnake_8bit_inst8 (
	.CLK(CLK),
	.SumIn(Sum7),
	.EditIn(Lives7),
	.A(DNA_nsh_o[16:23]),.B(DNA_shl_one_o[16:23]),.C(DNA_shl_two_o[16:23]),.D(DNA_shl_three_o[16:23]),.E(DNA_shl_four_o[16:23]),.F(DNA_shl_five_o[16:23]),
	.G(DNA_shr_one_o[16:23]),.H(DNA_shr_two_o[16:23]),.I(DNA_shr_three_o[16:23]),.J(DNA_shr_four_o[16:23]),.K(DNA_shr_five_o[16:23]),
	.Ao(DNA_nsh_o1[16:23]),.Bo(DNA_shl_one_o1[16:23]),.Co(DNA_shl_two_o1[16:23]),.Do(DNA_shl_three_o1[16:23]),.Eo(DNA_shl_four_o1[16:23]),.Fo(DNA_shl_five_o1[16:23]),
	.Go(DNA_shr_one_o1[16:23]),.Ho(DNA_shr_two_o1[16:23]),.Io(DNA_shr_three_o1[16:23]),.Jo(DNA_shr_four_o1[16:23]),.Ko(DNA_shr_five_o1[16:23]),
	.Lives(Lives8),
	.SumOut(Sum8)
);


SneakySnake_8bit SneakySnake_8bit_inst9 (
	.CLK(CLK),
	.SumIn(Sum8),
	.EditIn(Lives8),
	.A(DNA_nsh_o1[16:23]),.B(DNA_shl_one_o1[16:23]),.C(DNA_shl_two_o1[16:23]),.D(DNA_shl_three_o1[16:23]),.E(DNA_shl_four_o1[16:23]),.F(DNA_shl_five_o1[16:23]),
	.G(DNA_shr_one_o1[16:23]),.H(DNA_shr_two_o1[16:23]),.I(DNA_shr_three_o1[16:23]),.J(DNA_shr_four_o1[16:23]),.K(DNA_shr_five_o1[16:23]),
	//.Ao(DNA_nsh_o2[16:23]),.Bo(DNA_shl_one_o2[16:23]),.Co(DNA_shl_two_o2[16:23]),.Do(DNA_shl_three_o2[16:23]),.Eo(DNA_shl_four_o2[16:23]),.Fo(DNA_shl_five_o2[16:23]),
	//.Go(DNA_shr_one_o2[16:23]),.Ho(DNA_shr_two_o2[16:23]),.Io(DNA_shr_three_o2[16:23]),.Jo(DNA_shr_four_o2[16:23]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum9),
	.EditOut(EditOut3)
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst10 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[24:31]),.B(DNA_shl_one[24:31]),.C(DNA_shl_two[24:31]),.D(DNA_shl_three[24:31]),.E(DNA_shl_four[24:31]),.F(DNA_shl_five[24:31]),
	.G(DNA_shr_one[24:31]),.H(DNA_shr_two[24:31]),.I(DNA_shr_three[24:31]),.J(DNA_shr_four[24:31]),.K(DNA_shr_five[24:31]),
	.Ao(DNA_nsh_o[24:31]),.Bo(DNA_shl_one_o[24:31]),.Co(DNA_shl_two_o[24:31]),.Do(DNA_shl_three_o[24:31]),.Eo(DNA_shl_four_o[24:31]),.Fo(DNA_shl_five_o[24:31]),
	.Go(DNA_shr_one_o[24:31]),.Ho(DNA_shr_two_o[24:31]),.Io(DNA_shr_three_o[24:31]),.Jo(DNA_shr_four_o[24:31]),.Ko(DNA_shr_five_o[24:31]),
	.Lives(Lives10),
	.SumOut(Sum10)
);

SneakySnake_8bit SneakySnake_8bit_inst11 (
	.CLK(CLK),
	.SumIn(Sum10),
	.EditIn(Lives10),
	.A(DNA_nsh_o[24:31]),.B(DNA_shl_one_o[24:31]),.C(DNA_shl_two_o[24:31]),.D(DNA_shl_three_o[24:31]),.E(DNA_shl_four_o[24:31]),.F(DNA_shl_five_o[24:31]),
	.G(DNA_shr_one_o[24:31]),.H(DNA_shr_two_o[24:31]),.I(DNA_shr_three_o[24:31]),.J(DNA_shr_four_o[24:31]),.K(DNA_shr_five_o[24:31]),
	.Ao(DNA_nsh_o1[24:31]),.Bo(DNA_shl_one_o1[24:31]),.Co(DNA_shl_two_o1[24:31]),.Do(DNA_shl_three_o1[24:31]),.Eo(DNA_shl_four_o1[24:31]),.Fo(DNA_shl_five_o1[24:31]),
	.Go(DNA_shr_one_o1[24:31]),.Ho(DNA_shr_two_o1[24:31]),.Io(DNA_shr_three_o1[24:31]),.Jo(DNA_shr_four_o1[24:31]),.Ko(DNA_shr_five_o1[24:31]),
	.Lives(Lives11),
	.SumOut(Sum11)
);


SneakySnake_8bit SneakySnake_8bit_inst12 (
	.CLK(CLK),
	.SumIn(Sum11),
	.EditIn(Lives11),
	.A(DNA_nsh_o1[24:31]),.B(DNA_shl_one_o1[24:31]),.C(DNA_shl_two_o1[24:31]),.D(DNA_shl_three_o1[24:31]),.E(DNA_shl_four_o1[24:31]),.F(DNA_shl_five_o1[24:31]),
	.G(DNA_shr_one_o1[24:31]),.H(DNA_shr_two_o1[24:31]),.I(DNA_shr_three_o1[24:31]),.J(DNA_shr_four_o1[24:31]),.K(DNA_shr_five_o1[24:31]),
	//.Ao(DNA_nsh_o2[24:31]),.Bo(DNA_shl_one_o2[24:31]),.Co(DNA_shl_two_o2[24:31]),.Do(DNA_shl_three_o2[24:31]),.Eo(DNA_shl_four_o2[24:31]),.Fo(DNA_shl_five_o2[24:31]),
	//.Go(DNA_shr_one_o2[24:31]),.Ho(DNA_shr_two_o2[24:31]),.Io(DNA_shr_three_o2[24:31]),.Jo(DNA_shr_four_o2[24:31]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum12),
	.EditOut(EditOut4)	
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst13 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[32:39]),.B(DNA_shl_one[32:39]),.C(DNA_shl_two[32:39]),.D(DNA_shl_three[32:39]),.E(DNA_shl_four[32:39]),.F(DNA_shl_five[32:39]),
	.G(DNA_shr_one[32:39]),.H(DNA_shr_two[32:39]),.I(DNA_shr_three[32:39]),.J(DNA_shr_four[32:39]),.K(DNA_shr_five[32:39]),
	.Ao(DNA_nsh_o[32:39]),.Bo(DNA_shl_one_o[32:39]),.Co(DNA_shl_two_o[32:39]),.Do(DNA_shl_three_o[32:39]),.Eo(DNA_shl_four_o[32:39]),.Fo(DNA_shl_five_o[32:39]),
	.Go(DNA_shr_one_o[32:39]),.Ho(DNA_shr_two_o[32:39]),.Io(DNA_shr_three_o[32:39]),.Jo(DNA_shr_four_o[32:39]),.Ko(DNA_shr_five_o[32:39]),
	.Lives(Lives13),
	.SumOut(Sum13)
);

SneakySnake_8bit SneakySnake_8bit_inst14 (
	.CLK(CLK),
	.SumIn(Sum13),
	.EditIn(Lives13),
	.A(DNA_nsh_o[32:39]),.B(DNA_shl_one_o[32:39]),.C(DNA_shl_two_o[32:39]),.D(DNA_shl_three_o[32:39]),.E(DNA_shl_four_o[32:39]),.F(DNA_shl_five_o[32:39]),
	.G(DNA_shr_one_o[32:39]),.H(DNA_shr_two_o[32:39]),.I(DNA_shr_three_o[32:39]),.J(DNA_shr_four_o[32:39]),.K(DNA_shr_five_o[32:39]),
	.Ao(DNA_nsh_o1[32:39]),.Bo(DNA_shl_one_o1[32:39]),.Co(DNA_shl_two_o1[32:39]),.Do(DNA_shl_three_o1[32:39]),.Eo(DNA_shl_four_o1[32:39]),.Fo(DNA_shl_five_o1[32:39]),
	.Go(DNA_shr_one_o1[32:39]),.Ho(DNA_shr_two_o1[32:39]),.Io(DNA_shr_three_o1[32:39]),.Jo(DNA_shr_four_o1[32:39]),.Ko(DNA_shr_five_o1[32:39]),
	.Lives(Lives14),
	.SumOut(Sum14)
);


SneakySnake_8bit SneakySnake_8bit_inst15 (
	.CLK(CLK),
	.SumIn(Sum14),
	.EditIn(Lives14),
	.A(DNA_nsh_o1[32:39]),.B(DNA_shl_one_o1[32:39]),.C(DNA_shl_two_o1[32:39]),.D(DNA_shl_three_o1[32:39]),.E(DNA_shl_four_o1[32:39]),.F(DNA_shl_five_o1[32:39]),
	.G(DNA_shr_one_o1[32:39]),.H(DNA_shr_two_o1[32:39]),.I(DNA_shr_three_o1[32:39]),.J(DNA_shr_four_o1[32:39]),.K(DNA_shr_five_o1[32:39]),
	//.Ao(DNA_nsh_o2[32:39]),.Bo(DNA_shl_one_o2[32:39]),.Co(DNA_shl_two_o2[32:39]),.Do(DNA_shl_three_o2[32:39]),.Eo(DNA_shl_four_o2[32:39]),.Fo(DNA_shl_five_o2[32:39]),
	//.Go(DNA_shr_one_o2[32:39]),.Ho(DNA_shr_two_o2[32:39]),.Io(DNA_shr_three_o2[32:39]),.Jo(DNA_shr_four_o2[32:39]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum15),
	.EditOut(EditOut5)
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst16 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[40:47]),.B(DNA_shl_one[40:47]),.C(DNA_shl_two[40:47]),.D(DNA_shl_three[40:47]),.E(DNA_shl_four[40:47]),.F(DNA_shl_five[40:47]),
	.G(DNA_shr_one[40:47]),.H(DNA_shr_two[40:47]),.I(DNA_shr_three[40:47]),.J(DNA_shr_four[40:47]),.K(DNA_shr_five[40:47]),
	.Ao(DNA_nsh_o[40:47]),.Bo(DNA_shl_one_o[40:47]),.Co(DNA_shl_two_o[40:47]),.Do(DNA_shl_three_o[40:47]),.Eo(DNA_shl_four_o[40:47]),.Fo(DNA_shl_five_o[40:47]),
	.Go(DNA_shr_one_o[40:47]),.Ho(DNA_shr_two_o[40:47]),.Io(DNA_shr_three_o[40:47]),.Jo(DNA_shr_four_o[40:47]),.Ko(DNA_shr_five_o[40:47]),
	.Lives(Lives16),
	.SumOut(Sum16)
);

SneakySnake_8bit SneakySnake_8bit_inst17 (
	.CLK(CLK),
	.SumIn(Sum16),
	.EditIn(Lives16),
	.A(DNA_nsh_o[40:47]),.B(DNA_shl_one_o[40:47]),.C(DNA_shl_two_o[40:47]),.D(DNA_shl_three_o[40:47]),.E(DNA_shl_four_o[40:47]),.F(DNA_shl_five_o[40:47]),
	.G(DNA_shr_one_o[40:47]),.H(DNA_shr_two_o[40:47]),.I(DNA_shr_three_o[40:47]),.J(DNA_shr_four_o[40:47]),.K(DNA_shr_five_o[40:47]),
	.Ao(DNA_nsh_o1[40:47]),.Bo(DNA_shl_one_o1[40:47]),.Co(DNA_shl_two_o1[40:47]),.Do(DNA_shl_three_o1[40:47]),.Eo(DNA_shl_four_o1[40:47]),.Fo(DNA_shl_five_o1[40:47]),
	.Go(DNA_shr_one_o1[40:47]),.Ho(DNA_shr_two_o1[40:47]),.Io(DNA_shr_three_o1[40:47]),.Jo(DNA_shr_four_o1[40:47]),.Ko(DNA_shr_five_o1[40:47]),
	.Lives(Lives17),
	.SumOut(Sum17)
);


SneakySnake_8bit SneakySnake_8bit_inst18 (
	.CLK(CLK),
	.SumIn(Sum17),
	.EditIn(Lives17),
	.A(DNA_nsh_o1[40:47]),.B(DNA_shl_one_o1[40:47]),.C(DNA_shl_two_o1[40:47]),.D(DNA_shl_three_o1[40:47]),.E(DNA_shl_four_o1[40:47]),.F(DNA_shl_five_o1[40:47]),
	.G(DNA_shr_one_o1[40:47]),.H(DNA_shr_two_o1[40:47]),.I(DNA_shr_three_o1[40:47]),.J(DNA_shr_four_o1[40:47]),.K(DNA_shr_five_o1[40:47]),
	//.Ao(DNA_nsh_o2[40:47]),.Bo(DNA_shl_one_o2[40:47]),.Co(DNA_shl_two_o2[40:47]),.Do(DNA_shl_three_o2[40:47]),.Eo(DNA_shl_four_o2[40:47]),.Fo(DNA_shl_five_o2[40:47]),
	//.Go(DNA_shr_one_o2[40:47]),.Ho(DNA_shr_two_o2[40:47]),.Io(DNA_shr_three_o2[40:47]),.Jo(DNA_shr_four_o2[40:47]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum18),
	.EditOut(EditOut6)
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst19 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[48:55]),.B(DNA_shl_one[48:55]),.C(DNA_shl_two[48:55]),.D(DNA_shl_three[48:55]),.E(DNA_shl_four[48:55]),.F(DNA_shl_five[48:55]),
	.G(DNA_shr_one[48:55]),.H(DNA_shr_two[48:55]),.I(DNA_shr_three[48:55]),.J(DNA_shr_four[48:55]),.K(DNA_shr_five[48:55]),
	.Ao(DNA_nsh_o[48:55]),.Bo(DNA_shl_one_o[48:55]),.Co(DNA_shl_two_o[48:55]),.Do(DNA_shl_three_o[48:55]),.Eo(DNA_shl_four_o[48:55]),.Fo(DNA_shl_five_o[48:55]),
	.Go(DNA_shr_one_o[48:55]),.Ho(DNA_shr_two_o[48:55]),.Io(DNA_shr_three_o[48:55]),.Jo(DNA_shr_four_o[48:55]),.Ko(DNA_shr_five_o[48:55]),
	.Lives(Lives19),
	.SumOut(Sum19)
);

SneakySnake_8bit SneakySnake_8bit_inst20 (
	.CLK(CLK),
	.SumIn(Sum19),
	.EditIn(Lives19),
	.A(DNA_nsh_o[48:55]),.B(DNA_shl_one_o[48:55]),.C(DNA_shl_two_o[48:55]),.D(DNA_shl_three_o[48:55]),.E(DNA_shl_four_o[48:55]),.F(DNA_shl_five_o[48:55]),
	.G(DNA_shr_one_o[48:55]),.H(DNA_shr_two_o[48:55]),.I(DNA_shr_three_o[48:55]),.J(DNA_shr_four_o[48:55]),.K(DNA_shr_five_o[48:55]),
	.Ao(DNA_nsh_o1[48:55]),.Bo(DNA_shl_one_o1[48:55]),.Co(DNA_shl_two_o1[48:55]),.Do(DNA_shl_three_o1[48:55]),.Eo(DNA_shl_four_o1[48:55]),.Fo(DNA_shl_five_o1[48:55]),
	.Go(DNA_shr_one_o1[48:55]),.Ho(DNA_shr_two_o1[48:55]),.Io(DNA_shr_three_o1[48:55]),.Jo(DNA_shr_four_o1[48:55]),.Ko(DNA_shr_five_o1[48:55]),
	.Lives(Lives20),
	.SumOut(Sum20)
);


SneakySnake_8bit SneakySnake_8bit_inst21 (
	.CLK(CLK),
	.SumIn(Sum20),
	.EditIn(Lives20),
	.A(DNA_nsh_o1[48:55]),.B(DNA_shl_one_o1[48:55]),.C(DNA_shl_two_o1[48:55]),.D(DNA_shl_three_o1[48:55]),.E(DNA_shl_four_o1[48:55]),.F(DNA_shl_five_o1[48:55]),
	.G(DNA_shr_one_o1[48:55]),.H(DNA_shr_two_o1[48:55]),.I(DNA_shr_three_o1[48:55]),.J(DNA_shr_four_o1[48:55]),.K(DNA_shr_five_o1[48:55]),
	//.Ao(DNA_nsh_o2[48:55]),.Bo(DNA_shl_one_o2[48:55]),.Co(DNA_shl_two_o2[48:55]),.Do(DNA_shl_three_o2[48:55]),.Eo(DNA_shl_four_o2[48:55]),.Fo(DNA_shl_five_o2[48:55]),
	//.Go(DNA_shr_one_o2[48:55]),.Ho(DNA_shr_two_o2[48:55]),.Io(DNA_shr_three_o2[48:55]),.Jo(DNA_shr_four_o2[48:55]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum21),
	.EditOut(EditOut7)
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst22 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[56:63]),.B(DNA_shl_one[56:63]),.C(DNA_shl_two[56:63]),.D(DNA_shl_three[56:63]),.E(DNA_shl_four[56:63]),.F(DNA_shl_five[56:63]),
	.G(DNA_shr_one[56:63]),.H(DNA_shr_two[56:63]),.I(DNA_shr_three[56:63]),.J(DNA_shr_four[56:63]),.K(DNA_shr_five[56:63]),
	.Ao(DNA_nsh_o[56:63]),.Bo(DNA_shl_one_o[56:63]),.Co(DNA_shl_two_o[56:63]),.Do(DNA_shl_three_o[56:63]),.Eo(DNA_shl_four_o[56:63]),.Fo(DNA_shl_five_o[56:63]),
	.Go(DNA_shr_one_o[56:63]),.Ho(DNA_shr_two_o[56:63]),.Io(DNA_shr_three_o[56:63]),.Jo(DNA_shr_four_o[56:63]),.Ko(DNA_shr_five_o[56:63]),
	.Lives(Lives22),
	.SumOut(Sum22)
);

SneakySnake_8bit SneakySnake_8bit_inst23 (
	.CLK(CLK),
	.SumIn(Sum22),
	.EditIn(Lives22),
	.A(DNA_nsh_o[56:63]),.B(DNA_shl_one_o[56:63]),.C(DNA_shl_two_o[56:63]),.D(DNA_shl_three_o[56:63]),.E(DNA_shl_four_o[56:63]),.F(DNA_shl_five_o[56:63]),
	.G(DNA_shr_one_o[56:63]),.H(DNA_shr_two_o[56:63]),.I(DNA_shr_three_o[56:63]),.J(DNA_shr_four_o[56:63]),.K(DNA_shr_five_o[56:63]),
	.Ao(DNA_nsh_o1[56:63]),.Bo(DNA_shl_one_o1[56:63]),.Co(DNA_shl_two_o1[56:63]),.Do(DNA_shl_three_o1[56:63]),.Eo(DNA_shl_four_o1[56:63]),.Fo(DNA_shl_five_o1[56:63]),
	.Go(DNA_shr_one_o1[56:63]),.Ho(DNA_shr_two_o1[56:63]),.Io(DNA_shr_three_o1[56:63]),.Jo(DNA_shr_four_o1[56:63]),.Ko(DNA_shr_five_o1[56:63]),
	.Lives(Lives23),
	.SumOut(Sum23)
);


SneakySnake_8bit SneakySnake_8bit_inst24 (
	.CLK(CLK),
	.SumIn(Sum23),
	.EditIn(Lives23),
	.A(DNA_nsh_o1[56:63]),.B(DNA_shl_one_o1[56:63]),.C(DNA_shl_two_o1[56:63]),.D(DNA_shl_three_o1[56:63]),.E(DNA_shl_four_o1[56:63]),.F(DNA_shl_five_o1[56:63]),
	.G(DNA_shr_one_o1[56:63]),.H(DNA_shr_two_o1[56:63]),.I(DNA_shr_three_o1[56:63]),.J(DNA_shr_four_o1[56:63]),.K(DNA_shr_five_o1[56:63]),
	//.Ao(DNA_nsh_o2[56:63]),.Bo(DNA_shl_one_o2[56:63]),.Co(DNA_shl_two_o2[56:63]),.Do(DNA_shl_three_o2[56:63]),.Eo(DNA_shl_four_o2[56:63]),.Fo(DNA_shl_five_o2[56:63]),
	//.Go(DNA_shr_one_o2[56:63]),.Ho(DNA_shr_two_o2[56:63]),.Io(DNA_shr_three_o2[56:63]),.Jo(DNA_shr_four_o2[56:63]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum24),
	.EditOut(EditOut8)
);

////////////////////////////////////////////////////////
/*

SneakySnake_8bit SneakySnake_8bit_inst25 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[64:71]),.B(DNA_shl_one[64:71]),.C(DNA_shl_two[64:71]),.D(DNA_shl_three[64:71]),.E(DNA_shl_four[64:71]),.F(DNA_shl_five[64:71]),
	.G(DNA_shr_one[64:71]),.H(DNA_shr_two[64:71]),.I(DNA_shr_three[64:71]),.J(DNA_shr_four[64:71]),.K(DNA_shr_five[64:71]),
	.Ao(DNA_nsh_o[64:71]),.Bo(DNA_shl_one_o[64:71]),.Co(DNA_shl_two_o[64:71]),.Do(DNA_shl_three_o[64:71]),.Eo(DNA_shl_four_o[64:71]),.Fo(DNA_shl_five_o[64:71]),
	.Go(DNA_shr_one_o[64:71]),.Ho(DNA_shr_two_o[64:71]),.Io(DNA_shr_three_o[64:71]),.Jo(DNA_shr_four_o[64:71]),.Ko(DNA_shr_five_o[64:71]),
	.Lives(Lives25),
	.SumOut(Sum25)
);

SneakySnake_8bit SneakySnake_8bit_inst26 (
	.CLK(CLK),
	.SumIn(Sum25),
	.EditIn(Lives25),
	.A(DNA_nsh_o[64:71]),.B(DNA_shl_one_o[64:71]),.C(DNA_shl_two_o[64:71]),.D(DNA_shl_three_o[64:71]),.E(DNA_shl_four_o[64:71]),.F(DNA_shl_five_o[64:71]),
	.G(DNA_shr_one_o[64:71]),.H(DNA_shr_two_o[64:71]),.I(DNA_shr_three_o[64:71]),.J(DNA_shr_four_o[64:71]),.K(DNA_shr_five_o[64:71]),
	.Ao(DNA_nsh_o1[64:71]),.Bo(DNA_shl_one_o1[64:71]),.Co(DNA_shl_two_o1[64:71]),.Do(DNA_shl_three_o1[64:71]),.Eo(DNA_shl_four_o1[64:71]),.Fo(DNA_shl_five_o1[64:71]),
	.Go(DNA_shr_one_o1[64:71]),.Ho(DNA_shr_two_o1[64:71]),.Io(DNA_shr_three_o1[64:71]),.Jo(DNA_shr_four_o1[64:71]),.Ko(DNA_shr_five_o1[64:71]),
	.Lives(Lives26),
	.SumOut(Sum26)
);


SneakySnake_8bit SneakySnake_8bit_inst27 (
	.CLK(CLK),
	.SumIn(Sum26),
	.EditIn(Lives26),
	.A(DNA_nsh_o1[64:71]),.B(DNA_shl_one_o1[64:71]),.C(DNA_shl_two_o1[64:71]),.D(DNA_shl_three_o1[64:71]),.E(DNA_shl_four_o1[64:71]),.F(DNA_shl_five_o1[64:71]),
	.G(DNA_shr_one_o1[64:71]),.H(DNA_shr_two_o1[64:71]),.I(DNA_shr_three_o1[64:71]),.J(DNA_shr_four_o1[64:71]),.K(DNA_shr_five_o1[64:71]),
	//.Ao(DNA_nsh_o2[64:71]),.Bo(DNA_shl_one_o2[64:71]),.Co(DNA_shl_two_o2[64:71]),.Do(DNA_shl_three_o2[64:71]),.Eo(DNA_shl_four_o2[64:71]),.Fo(DNA_shl_five_o2[64:71]),
	//.Go(DNA_shr_one_o2[64:71]),.Ho(DNA_shr_two_o2[64:71]),.Io(DNA_shr_three_o2[64:71]),.Jo(DNA_shr_four_o2[64:71]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum27),
	.EditOut(EditOut9)
);


////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst28 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[72:79]),.B(DNA_shl_one[72:79]),.C(DNA_shl_two[72:79]),.D(DNA_shl_three[72:79]),.E(DNA_shl_four[72:79]),.F(DNA_shl_five[72:79]),
	.G(DNA_shr_one[72:79]),.H(DNA_shr_two[72:79]),.I(DNA_shr_three[72:79]),.J(DNA_shr_four[72:79]),.K(DNA_shr_five[72:79]),
	.Ao(DNA_nsh_o[72:79]),.Bo(DNA_shl_one_o[72:79]),.Co(DNA_shl_two_o[72:79]),.Do(DNA_shl_three_o[72:79]),.Eo(DNA_shl_four_o[72:79]),.Fo(DNA_shl_five_o[72:79]),
	.Go(DNA_shr_one_o[72:79]),.Ho(DNA_shr_two_o[72:79]),.Io(DNA_shr_three_o[72:79]),.Jo(DNA_shr_four_o[72:79]),.Ko(DNA_shr_five_o[72:79]),
	.Lives(Lives28),
	.SumOut(Sum28)
);

SneakySnake_8bit SneakySnake_8bit_inst29 (
	.CLK(CLK),
	.SumIn(Sum28),
	.EditIn(Lives28),
	.A(DNA_nsh_o[72:79]),.B(DNA_shl_one_o[72:79]),.C(DNA_shl_two_o[72:79]),.D(DNA_shl_three_o[72:79]),.E(DNA_shl_four_o[72:79]),.F(DNA_shl_five_o[72:79]),
	.G(DNA_shr_one_o[72:79]),.H(DNA_shr_two_o[72:79]),.I(DNA_shr_three_o[72:79]),.J(DNA_shr_four_o[72:79]),.K(DNA_shr_five_o[72:79]),
	.Ao(DNA_nsh_o1[72:79]),.Bo(DNA_shl_one_o1[72:79]),.Co(DNA_shl_two_o1[72:79]),.Do(DNA_shl_three_o1[72:79]),.Eo(DNA_shl_four_o1[72:79]),.Fo(DNA_shl_five_o1[72:79]),
	.Go(DNA_shr_one_o1[72:79]),.Ho(DNA_shr_two_o1[72:79]),.Io(DNA_shr_three_o1[72:79]),.Jo(DNA_shr_four_o1[72:79]),.Ko(DNA_shr_five_o1[72:79]),
	.Lives(Lives29),
	.SumOut(Sum29)
);


SneakySnake_8bit SneakySnake_8bit_inst30 (
	.CLK(CLK),
	.SumIn(Sum29),
	.EditIn(Lives29),
	.A(DNA_nsh_o1[72:79]),.B(DNA_shl_one_o1[72:79]),.C(DNA_shl_two_o1[72:79]),.D(DNA_shl_three_o1[72:79]),.E(DNA_shl_four_o1[72:79]),.F(DNA_shl_five_o1[72:79]),
	.G(DNA_shr_one_o1[72:79]),.H(DNA_shr_two_o1[72:79]),.I(DNA_shr_three_o1[72:79]),.J(DNA_shr_four_o1[72:79]),.K(DNA_shr_five_o1[72:79]),
	//.Ao(DNA_nsh_o2[72:79]),.Bo(DNA_shl_one_o2[72:79]),.Co(DNA_shl_two_o2[72:79]),.Do(DNA_shl_three_o2[72:79]),.Eo(DNA_shl_four_o2[72:79]),.Fo(DNA_shl_five_o2[72:79]),
	//.Go(DNA_shr_one_o2[72:79]),.Ho(DNA_shr_two_o2[72:79]),.Io(DNA_shr_three_o2[72:79]),.Jo(DNA_shr_four_o2[72:79]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum30),
	.EditOut(EditOut10)
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst31 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[80:87]),.B(DNA_shl_one[80:87]),.C(DNA_shl_two[80:87]),.D(DNA_shl_three[80:87]),.E(DNA_shl_four[80:87]),.F(DNA_shl_five[80:87]),
	.G(DNA_shr_one[80:87]),.H(DNA_shr_two[80:87]),.I(DNA_shr_three[80:87]),.J(DNA_shr_four[80:87]),.K(DNA_shr_five[80:87]),
	.Ao(DNA_nsh_o[80:87]),.Bo(DNA_shl_one_o[80:87]),.Co(DNA_shl_two_o[80:87]),.Do(DNA_shl_three_o[80:87]),.Eo(DNA_shl_four_o[80:87]),.Fo(DNA_shl_five_o[80:87]),
	.Go(DNA_shr_one_o[80:87]),.Ho(DNA_shr_two_o[80:87]),.Io(DNA_shr_three_o[80:87]),.Jo(DNA_shr_four_o[80:87]),.Ko(DNA_shr_five_o[80:87]),
	.Lives(Lives31),
	.SumOut(Sum31)
);

SneakySnake_8bit SneakySnake_8bit_inst32 (
	.CLK(CLK),
	.SumIn(Sum31),
	.EditIn(Lives31),
	.A(DNA_nsh_o[80:87]),.B(DNA_shl_one_o[80:87]),.C(DNA_shl_two_o[80:87]),.D(DNA_shl_three_o[80:87]),.E(DNA_shl_four_o[80:87]),.F(DNA_shl_five_o[80:87]),
	.G(DNA_shr_one_o[80:87]),.H(DNA_shr_two_o[80:87]),.I(DNA_shr_three_o[80:87]),.J(DNA_shr_four_o[80:87]),.K(DNA_shr_five_o[80:87]),
	.Ao(DNA_nsh_o1[80:87]),.Bo(DNA_shl_one_o1[80:87]),.Co(DNA_shl_two_o1[80:87]),.Do(DNA_shl_three_o1[80:87]),.Eo(DNA_shl_four_o1[80:87]),.Fo(DNA_shl_five_o1[80:87]),
	.Go(DNA_shr_one_o1[80:87]),.Ho(DNA_shr_two_o1[80:87]),.Io(DNA_shr_three_o1[80:87]),.Jo(DNA_shr_four_o1[80:87]),.Ko(DNA_shr_five_o1[80:87]),
	.Lives(Lives32),
	.SumOut(Sum32)
);


SneakySnake_8bit SneakySnake_8bit_inst33 (
	.CLK(CLK),
	.SumIn(Sum32),
	.EditIn(Lives32),
	.A(DNA_nsh_o1[80:87]),.B(DNA_shl_one_o1[80:87]),.C(DNA_shl_two_o1[80:87]),.D(DNA_shl_three_o1[80:87]),.E(DNA_shl_four_o1[80:87]),.F(DNA_shl_five_o1[80:87]),
	.G(DNA_shr_one_o1[80:87]),.H(DNA_shr_two_o1[80:87]),.I(DNA_shr_three_o1[80:87]),.J(DNA_shr_four_o1[80:87]),.K(DNA_shr_five_o1[80:87]),
	//.Ao(DNA_nsh_o2[80:87]),.Bo(DNA_shl_one_o2[80:87]),.Co(DNA_shl_two_o2[80:87]),.Do(DNA_shl_three_o2[80:87]),.Eo(DNA_shl_four_o2[80:87]),.Fo(DNA_shl_five_o2[80:87]),
	//.Go(DNA_shr_one_o2[80:87]),.Ho(DNA_shr_two_o2[80:87]),.Io(DNA_shr_three_o2[80:87]),.Jo(DNA_shr_four_o2[80:87]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum33),
	.EditOut(EditOut11)
);

////////////////////////////////////////////////////////


SneakySnake_8bit SneakySnake_8bit_inst34 (
	.CLK(CLK),
	.SumIn(0),
	.EditIn(0),
	.A(DNA_nsh[88:95]),.B(DNA_shl_one[88:95]),.C(DNA_shl_two[88:95]),.D(DNA_shl_three[88:95]),.E(DNA_shl_four[88:95]),.F(DNA_shl_five[88:95]),
	.G(DNA_shr_one[88:95]),.H(DNA_shr_two[88:95]),.I(DNA_shr_three[88:95]),.J(DNA_shr_four[88:95]),.K(DNA_shr_five[88:95]),
	.Ao(DNA_nsh_o[88:95]),.Bo(DNA_shl_one_o[88:95]),.Co(DNA_shl_two_o[88:95]),.Do(DNA_shl_three_o[88:95]),.Eo(DNA_shl_four_o[88:95]),.Fo(DNA_shl_five_o[88:95]),
	.Go(DNA_shr_one_o[88:95]),.Ho(DNA_shr_two_o[88:95]),.Io(DNA_shr_three_o[88:95]),.Jo(DNA_shr_four_o[88:95]),.Ko(DNA_shr_five_o[88:95]),
	.Lives(Lives34),
	.SumOut(Sum34)
);

SneakySnake_8bit SneakySnake_8bit_inst35 (
	.CLK(CLK),
	.SumIn(Sum34),
	.EditIn(Lives34),
	.A(DNA_nsh_o[88:95]),.B(DNA_shl_one_o[88:95]),.C(DNA_shl_two_o[88:95]),.D(DNA_shl_three_o[88:95]),.E(DNA_shl_four_o[88:95]),.F(DNA_shl_five_o[88:95]),
	.G(DNA_shr_one_o[88:95]),.H(DNA_shr_two_o[88:95]),.I(DNA_shr_three_o[88:95]),.J(DNA_shr_four_o[88:95]),.K(DNA_shr_five_o[88:95]),
	.Ao(DNA_nsh_o1[88:95]),.Bo(DNA_shl_one_o1[88:95]),.Co(DNA_shl_two_o1[88:95]),.Do(DNA_shl_three_o1[88:95]),.Eo(DNA_shl_four_o1[88:95]),.Fo(DNA_shl_five_o1[88:95]),
	.Go(DNA_shr_one_o1[88:95]),.Ho(DNA_shr_two_o1[88:95]),.Io(DNA_shr_three_o1[88:95]),.Jo(DNA_shr_four_o1[88:95]),.Ko(DNA_shr_five_o1[88:95]),
	.Lives(Lives35),
	.SumOut(Sum35)
);


SneakySnake_8bit SneakySnake_8bit_inst36 (
	.CLK(CLK),
	.SumIn(Sum35),
	.EditIn(Lives35),
	.A(DNA_nsh_o1[88:95]),.B(DNA_shl_one_o1[88:95]),.C(DNA_shl_two_o1[88:95]),.D(DNA_shl_three_o1[88:95]),.E(DNA_shl_four_o1[88:95]),.F(DNA_shl_five_o1[88:95]),
	.G(DNA_shr_one_o1[88:95]),.H(DNA_shr_two_o1[88:95]),.I(DNA_shr_three_o1[88:95]),.J(DNA_shr_four_o1[88:95]),.K(DNA_shr_five_o1[88:95]),
	//.Ao(DNA_nsh_o2[88:95]),.Bo(DNA_shl_one_o2[88:95]),.Co(DNA_shl_two_o2[88:95]),.Do(DNA_shl_three_o2[88:95]),.Eo(DNA_shl_four_o2[88:95]),.Fo(DNA_shl_five_o2[88:95]),
	//.Go(DNA_shr_one_o2[88:95]),.Ho(DNA_shr_two_o2[88:95]),.Io(DNA_shr_three_o2[88:95]),.Jo(DNA_shr_four_o2[88:95]),.Ko(DNA_shr_five_o2[0:7]),
	//.SumOut(Sum36),
	.EditOut(EditOut12)
);
*/
/////////////////////////////////////////////// Producing the final result after 5 clock cycles
always @ (posedge CLK)
begin
    MinEdits = EditOut1 + EditOut2 + EditOut3 + EditOut4 + EditOut5 + EditOut6 + EditOut7 + EditOut8;
    //MinEdits = MinEdits + EditOut7 + EditOut8 + EditOut9 + EditOut10 + EditOut11 + EditOut12;  
   if (MinEdits <= ErrorThreshold)
          Accepted = 1'b1;
      else
          Accepted = 1'b0;
end


endmodule

