`timescale 1ns / 1ps

// Shifted Hamming Distance (SHD)
//	Read::: TCCATTGACATTCGTGACTCTCCTTCTCTCCCACCCCTTTGCCCCC (46 Bp)
//	Ref ::: TCCATTGACATTCGTGAGCTGCTCCTTCTCTCCCACCCCTTTGCCC
// Encoding Scheme ::: 	A --> 00		C --> 01		G --> 10		T --> 11
//	Read::: 11010100111110000100111101101110000111011101011111011101110101010001010101111111100101010101 (92 Encoded Bp)
//	Ref ::: 11010100111110000100111101101110001001111001110101111101110111010101000101010111111110010101
//        [91]<.....................................................................................>[0]

// Speculative removal of short-matches (SRS)
//	Read::: TTCCCAGCACAGACGCATAGCCTGGTCTTTGTCGTCCATTGACATT (46 Bp)
//	Ref ::: TTCCCAGCACAAGACACATTGTGTTTTCTGTGCCGACCCAGGACAT
// Encoding Scheme ::: 	A --> 00		C --> 01		G --> 10		T --> 11
//	Read::: 11110101010010010001001000011001001100100101111010110111111110110110110101001111100001001111 (92 Encoded Bp)
//	Ref ::: 11110101010010010001000010000100010011111011101111111101111011100101100001010100101000010011
//        [91]<.....................................................................................>[0]
				 
module SHD_tb();

	parameter LENGTH = 128;   // width of input and output data

//	reg clk;
	//integer file_data, file_read;
	reg [LENGTH-1:0] DNA_read, DNA_ref;	
	//wire [(LENGTH/2)-1:0] DNA_nsh, DNA_shl_one, DNA_shl_two, DNA_shr_one , DNA_shr_two, DNA_out;
	
	wire [7:0] DNA_MinErrors;

	initial
		begin
		DNA_read = 92'b11110101010010010001001000011001001100100101111010110111111110110110110101001111100001001111;
        DNA_ref = 92'b11110101010010010001000010000100010011111011101111111101111011100101100001010100101000010011;
        
        #20
        DNA_read = 92'b11010100111110000100111101101110000111011101011111011101110101010001010101111111100101010101;
        #20
        DNA_read = 92'b11110101010010010001001000011001001100100101111010110111111110110110110101001111100001001111;
        #20
        DNA_read = 92'b11010100111110000100111101101110000111011101011111011101110101010001010101111111100101010101;
        #20
        DNA_read = 92'b11110101010010010001001000011001001100100101111010110111111110110110110101001111100001001111;
//			clk = 1'b1;
//			$monitor("DNA_read=%b",DNA_read);
//			file_data = $fopen("C:\\Users\\ALSERCS\\Desktop\\Systems Biology\\Quartus Project\\DNA_Short_Reads(Ref_Reads).txt","rb");
//			if(!file_data) $display("File Open Error!");
//			  
//			file_read = $fscanf(file_data,"%b %b\n",DNA_ref, DNA_read);
//			$fclose(file_data);
		end
	 
	 
//  always   #2.5 clk=!clk; //200MHz Clock = 5ns
  
  //always   #5 clk=!clk;
  
	SHD #(.LENGTH(LENGTH)) SHD_tb(
//	.clk(clk), 
	.DNA_read(DNA_read),
	.DNA_ref(DNA_ref),
	.DNA_MinErrors(DNA_MinErrors)
//	,
//	.DNA_out(DNA_out),
//	.DNA_nsh(DNA_nsh),
//	.DNA_shl_one(DNA_shl_one),
//	.DNA_shl_two(DNA_shl_two),
//	.DNA_shr_one(DNA_shr_one),
//	.DNA_shr_two(DNA_shr_two)
	);
  	
endmodule