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



module LongestLZC_8Bit (

	input [0:7]a,
	input [0:7]b,
	input [0:7]c,
	input [0:7]d,
	input [0:7]e,
	input [0:7]f,
	input [0:7]g,
	input [0:7]h,
	input [0:7]i,
	input [0:7]j,
	input [0:7]k,
	output L3, L2, L1, L0);

	wire Valid_A, Valid_B, A0, A1, A2, B0, B1, B2;
	wire Valid_C, Valid_D, C0, C1, C2, D0, D1, D2;
	wire Valid_E, Valid_F, E0, E1, E2, F0, F1, F2;
	wire Valid_G, Valid_H, G0, G1, G2, H0, H1, H2;
	wire Valid_I, Valid_J, I0, I1, I2, J0, J1, J2;
	wire Valid_K, K0, K1, K2;
	
	wire AB3,AB2,AB1,AB0;
	wire CD3,CD2,CD1,CD0;
	wire EF3,EF2,EF1,EF0;
	wire GH3,GH2,GH1,GH0;
	wire IJ3,IJ2,IJ1,IJ0;
	wire KL3,KL2,KL1,KL0;
	
	wire ABCD3,ABCD2,ABCD1,ABCD0;
	wire EFGH3,EFGH2,EFGH1,EFGH0;
	wire IJKL3,IJKL2,IJKL1,IJKL0;	
	
	wire ABCDEFGH3, ABCDEFGH2, ABCDEFGH1, ABCDEFGH0;
	wire IJKLMNOP3, IJKLMNOP2, IJKLMNOP1, IJKLMNOP0;
	
LZC_Dimitra_8 LZC_Dimitra_8_inst1
(
	.B0(a[0]) ,	
	.B1(a[1]) ,	
	.B2(a[2]) ,	
	.B3(a[3]) ,	
	.B4(a[4]) ,	
	.B5(a[5]) ,	
	.B6(a[6]) ,	
	.B7(a[7]) ,	
	.Valid(Valid_A) ,
	.C0(A0) ,
	.C1(A1) ,
	.C2(A2)
);	  


LZC_Dimitra_8 LZC_Dimitra_8_inst2
(
	.B0(b[0]) ,	
	.B1(b[1]) ,	
	.B2(b[2]) ,	
	.B3(b[3]) ,	
	.B4(b[4]) ,	
	.B5(b[5]) ,	
	.B6(b[6]) ,	
	.B7(b[7]) ,	
	.Valid(Valid_B) ,
	.C0(B0) ,
	.C1(B1) ,
	.C2(B2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst3
(
	.B0(c[0]) ,	
	.B1(c[1]) ,	
	.B2(c[2]) ,	
	.B3(c[3]) ,	
	.B4(c[4]) ,	
	.B5(c[5]) ,	
	.B6(c[6]) ,	
	.B7(c[7]) ,	
	.Valid(Valid_C) ,
	.C0(C0) ,
	.C1(C1) ,
	.C2(C2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst4
(
	.B0(d[0]) ,	
	.B1(d[1]) ,	
	.B2(d[2]) ,	
	.B3(d[3]) ,	
	.B4(d[4]) ,	
	.B5(d[5]) ,	
	.B6(d[6]) ,	
	.B7(d[7]) ,	
	.Valid(Valid_D) ,
	.C0(D0) ,
	.C1(D1) ,
	.C2(D2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst5
(
	.B0(e[0]) ,	
	.B1(e[1]) ,	
	.B2(e[2]) ,	
	.B3(e[3]) ,	
	.B4(e[4]) ,	
	.B5(e[5]) ,	
	.B6(e[6]) ,	
	.B7(e[7]) ,	
	.Valid(Valid_E) ,
	.C0(E0) ,
	.C1(E1) ,
	.C2(E2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst6
(
	.B0(f[0]) ,	
	.B1(f[1]) ,	
	.B2(f[2]) ,	
	.B3(f[3]) ,	
	.B4(f[4]) ,	
	.B5(f[5]) ,	
	.B6(f[6]) ,	
	.B7(f[7]) ,	
	.Valid(Valid_F) ,
	.C0(F0) ,
	.C1(F1) ,
	.C2(F2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst7
(
	.B0(g[0]) ,	
	.B1(g[1]) ,	
	.B2(g[2]) ,	
	.B3(g[3]) ,	
	.B4(g[4]) ,	
	.B5(g[5]) ,	
	.B6(g[6]) ,	
	.B7(g[7]) ,	
	.Valid(Valid_G) ,
	.C0(G0) ,
	.C1(G1) ,
	.C2(G2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst8
(
	.B0(h[0]) ,	
	.B1(h[1]) ,	
	.B2(h[2]) ,	
	.B3(h[3]) ,	
	.B4(h[4]) ,	
	.B5(h[5]) ,	
	.B6(h[6]) ,	
	.B7(h[7]) ,	
	.Valid(Valid_H) ,
	.C0(H0) ,
	.C1(H1) ,
	.C2(H2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst9
(
	.B0(i[0]) ,	
	.B1(i[1]) ,	
	.B2(i[2]) ,	
	.B3(i[3]) ,	
	.B4(i[4]) ,	
	.B5(i[5]) ,	
	.B6(i[6]) ,	
	.B7(i[7]) ,	
	.Valid(Valid_I) ,
	.C0(I0) ,
	.C1(I1) ,
	.C2(I2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst10
(
	.B0(j[0]) ,	
	.B1(j[1]) ,	
	.B2(j[2]) ,	
	.B3(j[3]) ,	
	.B4(j[4]) ,	
	.B5(j[5]) ,	
	.B6(j[6]) ,	
	.B7(j[7]) ,	
	.Valid(Valid_J) ,
	.C0(J0) ,
	.C1(J1) ,
	.C2(J2)
);	


LZC_Dimitra_8 LZC_Dimitra_8_inst11
(
	.B0(k[0]) ,	
	.B1(k[1]) ,	
	.B2(k[2]) ,	
	.B3(k[3]) ,	
	.B4(k[4]) ,	
	.B5(k[5]) ,	
	.B6(k[6]) ,	
	.B7(k[7]) ,	
	.Valid(Valid_K) ,
	.C0(K0) ,
	.C1(K1) ,
	.C2(K2)
);	

////////////////////////////////// Comparator Level-1

Comparator_3Bit Comparator_3Bit_inst1(
	.Valid_A(Valid_A), .Valid_B(Valid_B), 
	.A0(A0), .A1(A1), .A2(A2), .B0(B0), .B1(B1), .B2(B2),
	.AB3(AB3), .AB2(AB2), .AB1(AB1),	.AB0(AB0)
);
	

Comparator_3Bit Comparator_3Bit_inst2(
	.Valid_A(Valid_C), .Valid_B(Valid_D), 
	.A0(C0), .A1(C1), .A2(C2), .B0(D0), .B1(D1), .B2(D2),
	.AB3(CD3), .AB2(CD2), .AB1(CD1),	.AB0(CD0)
);

Comparator_3Bit Comparator_3Bit_inst3(
	.Valid_A(Valid_E), .Valid_B(Valid_F), 
	.A0(E0), .A1(E1), .A2(E2), .B0(F0), .B1(F1), .B2(F2),
	.AB3(EF3), .AB2(EF2), .AB1(EF1),	.AB0(EF0)
);

Comparator_3Bit Comparator_3Bit_inst4(
	.Valid_A(Valid_G), .Valid_B(Valid_H), 
	.A0(G0), .A1(G1), .A2(G2), .B0(H0), .B1(H1), .B2(H2),
	.AB3(GH3), .AB2(GH2), .AB1(GH1),	.AB0(GH0)
);


Comparator_3Bit Comparator_3Bit_inst5(
	.Valid_A(Valid_I), .Valid_B(Valid_J), 
	.A0(I0), .A1(I1), .A2(I2), .B0(J0), .B1(J1), .B2(J2),
	.AB3(IJ3), .AB2(IJ2), .AB1(IJ1),	.AB0(IJ0)
);

Comparator_3Bit Comparator_3Bit_inst6(
	.Valid_A(Valid_K), .Valid_B(1'b1), 
	.A0(K0), .A1(K1), .A2(K2), .B0(1'b0), .B1(1'b0), .B2(1'b0),
	.AB3(KL3), .AB2(KL2), .AB1(KL1),	.AB0(KL0)
);

/////////////////////////////////////////////////////// Comparator Level-2

Comparator_3Bit Comparator_3Bit_inst7(
	.Valid_A(AB3), .Valid_B(CD3), 
	.A0(AB0), .A1(AB1), .A2(AB2), .B0(CD0), .B1(CD1), .B2(CD2),
	.AB3(ABCD3), .AB2(ABCD2), .AB1(ABCD1),	.AB0(ABCD0)
);

Comparator_3Bit Comparator_3Bit_inst8(
	.Valid_A(EF3), .Valid_B(GH3), 
	.A0(EF0), .A1(EF1), .A2(EF2), .B0(GH0), .B1(GH1), .B2(GH2),
	.AB3(EFGH3), .AB2(EFGH2), .AB1(EFGH1),	.AB0(EFGH0)
);

Comparator_3Bit Comparator_3Bit_inst9(
	.Valid_A(IJ3), .Valid_B(KL3), 
	.A0(IJ0), .A1(IJ1), .A2(IJ2), .B0(KL0), .B1(KL1), .B2(KL2),
	.AB3(IJKL3), .AB2(IJKL2), .AB1(IJKL1),	.AB0(IJKL0)
);

/////////////////////////////////// Comparator Level-3

Comparator_3Bit Comparator_3Bit_inst10(
	.Valid_A(ABCD3), .Valid_B(EFGH3), 
	.A0(ABCD0), .A1(ABCD1), .A2(ABCD2), .B0(EFGH0), .B1(EFGH1), .B2(EFGH2),
	.AB3(ABCDEFGH3), .AB2(ABCDEFGH2), .AB1(ABCDEFGH1),	.AB0(ABCDEFGH0)
);

Comparator_3Bit Comparator_3Bit_inst11(
	.Valid_A(IJKL3), .Valid_B(1'b1), 
	.A0(IJKL0), .A1(IJKL1), .A2(IJKL2), .B0(1'b0), .B1(1'b0), .B2(1'b0),
	.AB3(IJKLMNOP3), .AB2(IJKLMNOP2), .AB1(IJKLMNOP1),	.AB0(IJKLMNOP0)
);

///////////////////////////////// Comparator Level-4

Comparator_3Bit Comparator_3Bit_inst12(
	.Valid_A(ABCDEFGH3), .Valid_B(IJKLMNOP3), 
	.A0(ABCDEFGH0), .A1(ABCDEFGH1), .A2(ABCDEFGH2), .B0(IJKLMNOP0), .B1(IJKLMNOP1), .B2(IJKLMNOP2),
	.AB3(L3), .AB2(L2), .AB1(L1),	.AB0(L0)
);

endmodule
