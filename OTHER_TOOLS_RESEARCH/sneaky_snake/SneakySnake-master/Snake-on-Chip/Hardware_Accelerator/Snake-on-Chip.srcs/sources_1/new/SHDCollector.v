`timescale 1ns / 1ps

module SHDCollector #(parameter DNA_DATA_WIDTH = 128, NUM_CLUSTERS = 8, NUM_PES = 8) (
        input clk,
        input rst,
        
        //Cluster Interface
        output reg[NUM_CLUSTERS - 1:0] cluster_rd_en,
        input[NUM_CLUSTERS * NUM_PES - 1:0] cluster_data,
        input[NUM_CLUSTERS - 1:0] cluster_valid,
        
        //Sender Interface
        input sender_ready,
        output sender_data_valid,
        output[NUM_PES - 1:0] sender_data
    );
    
    
    //Cluster iterator
    wire cluster_fetched;
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
    
    assign advance_cluster_it = cluster_fetched;
    
    
    //Register Input
    reg[NUM_PES - 1:0] dna_err_r;
    reg dna_err_valid_r;
    wire accept_cluster_data;
    
    always@(posedge clk) begin
        if(rst) begin
            dna_err_r <= 0;
            dna_err_valid_r <= 1'b0;
        end
        else begin
            if(accept_cluster_data) begin
                dna_err_r <= cluster_data[cluster_iterator*NUM_PES +: NUM_PES];
                dna_err_valid_r <= cluster_valid[cluster_iterator];
            end
        end
    end
    
    always@* begin
        cluster_rd_en = 0;
        cluster_rd_en[cluster_iterator] = accept_cluster_data;
    end
    
    assign cluster_fetched = accept_cluster_data && cluster_valid[cluster_iterator];
    
    assign accept_cluster_data = sender_ready;
    
    assign sender_data_valid = dna_err_valid_r;
    assign sender_data = dna_err_r;
        
endmodule
