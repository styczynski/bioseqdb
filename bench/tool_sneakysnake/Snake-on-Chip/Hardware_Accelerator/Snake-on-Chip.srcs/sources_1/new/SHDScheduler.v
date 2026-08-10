`timescale 1ns / 1ps


module SHDScheduler #(parameter DNA_DATA_WIDTH = 128, NUM_CLUSTERS = 8) (
        input clk,
        input rst,
        
        //Receiver Interface
        output dna_rd_en,
        input[DNA_DATA_WIDTH - 1:0] dna_data_in,
        input dna_valid_in,
        
        //Cluster Interface
        input shd_clk,
        input[NUM_CLUSTERS - 1:0] shd_rd_en_in,
        output[NUM_CLUSTERS - 1:0] shd_valid_out,
        output[NUM_CLUSTERS*DNA_DATA_WIDTH - 1:0] shd_dna_data_out
    );
    
    reg[DNA_DATA_WIDTH - 1:0] dna_data_r;
    reg dna_valid_r = 0;
    wire accept_dna_data;
    reg dna_issued;
    
    //Register incoming dna data
    always@(posedge clk) begin
        
        if(rst) begin
            dna_data_r <= 0;
            dna_valid_r <= 0;
        end
        else begin
            if(accept_dna_data) begin
                dna_data_r <= dna_data_in;
                dna_valid_r <= dna_valid_in;
            end
        end
    end
    
    assign accept_dna_data = !dna_valid_r || dna_issued;
    assign dna_rd_en = accept_dna_data;
    
    
    
    //SHD FIFOs
    wire[NUM_CLUSTERS - 1:0] shd_fifo_full, shd_fifo_empty;
    reg[NUM_CLUSTERS - 1:0] shd_fifo_wr_en;
    
    genvar i;
    generate
        for (i=0; i < NUM_CLUSTERS; i=i+1) begin
            shd_fifo i_shd_fifo (
              .rst(rst),        // input wire rst
              .wr_clk(clk),  // input wire wr_clk
              .rd_clk(shd_clk),  // input wire rd_clk
              .din(dna_data_r),        // input wire [255 : 0] din
              .wr_en(shd_fifo_wr_en[i]),    // input wire wr_en
              .rd_en(shd_rd_en_in[i]),    // input wire rd_en
              .dout(shd_dna_data_out[i*DNA_DATA_WIDTH +: DNA_DATA_WIDTH]),      // output wire [255 : 0] dout
              .full(shd_fifo_full[i]),      // output wire full
              .empty(shd_fifo_empty[i])    // output wire empty
            );
        
        end
    endgenerate
    
    assign shd_valid_out = ~shd_fifo_empty;
    
    
    // --- ARBITRATION LOGIC ---
    
    //SHD PE iterator
    parameter CLUSTER_BITS = $clog2(NUM_CLUSTERS);
    reg[CLUSTER_BITS - 1:0] cluster_iterator = 0;
    wire advance_cluster_it;
    
    always@(posedge clk) begin
        if(rst) begin
            cluster_iterator <= 0;
        end
        else begin
            if(advance_cluster_it) begin
                cluster_iterator <= cluster_iterator + 1'b1;
            end
        end
    end
    
    assign advance_cluster_it = dna_issued; //We want to preserve the order. Looking for non-full FIFOs may break it
    
    //Issue to current FIFO if not full
    always@* begin
        shd_fifo_wr_en = {NUM_CLUSTERS{1'b0}};
        dna_issued = 1'b0;
        
        if(dna_valid_r && ~shd_fifo_full[cluster_iterator]) begin
            shd_fifo_wr_en[cluster_iterator] = 1'b1;
            dna_issued = 1'b1;
        end
    end
    
    // --- END - ARBITRATION LOGIC ---
    
endmodule
