`timescale 1ns / 1ps


module refread_manager #(parameter C_PCI_DATA_WIDTH = 128, NUM_REFS = 8) (
        input clk,
        input rst,
        
        input set,
        input[C_PCI_DATA_WIDTH - 1:0] refread_in,
        
        output[C_PCI_DATA_WIDTH * NUM_REFS - 1 :0] refread_out
    );
    
    
    parameter REF_BITS = $clog2(NUM_REFS);
    
    reg[C_PCI_DATA_WIDTH - 1:0] ref_reads [0:(NUM_REFS - 1)];
    
    integer k;
    
    always@(posedge clk) begin
        /*if(rst) begin
            for(k = 0; k < NUM_REFS; k = k + 1) begin
                ref_reads[k] <= 0;
            end
        end*/
        //else begin
            if(set) begin
                ref_reads[NUM_REFS - 1] <= refread_in;
                
                for(k = 0; k < (NUM_REFS - 1); k = k + 1) begin
                    ref_reads[k] <= ref_reads[k + 1];
                end
            end
        //end
    end
    
    genvar i;
    
    generate
        for (i = 0; i < NUM_REFS; i = i + 1) begin
            assign refread_out[(i*C_PCI_DATA_WIDTH) +: C_PCI_DATA_WIDTH] = ref_reads[i];
        end
    endgenerate
    
endmodule
