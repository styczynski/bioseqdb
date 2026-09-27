`timescale 1ns / 1ps

`include "riffa.vh"

/*
    NUM_CLUSTERS: Number of processing cluster's composed of multiple Processing Elements (PE).
        Incoming stream of reads is distributed across the clusters (operating at lower frequency than RIFFA) to achieve high bandwidth.
        Increasing it causes higher incoming (download) PCIe bandwidth utilization.
    NUM_PES: Number of Processing Elements per cluster. In a cluster, each PE evaluates the read assigned to the cluster against its
        own reference read (so each cluster produces NUM_PES results for a single read).
        Each PE in a cluster has different reference read. However, the PEs share the same reference reads across the clusters.
        (i.e., cluster[n].pe[i].reference_read == cluster[m].pe[i].reference_read)
        Increasing it causes higher outgoing (upload) PCIe bandwidth utilization.
*/
module SHD_top #(parameter C_PCI_DATA_WIDTH = 128, NUM_CLUSTERS = 8, NUM_PES = 8) (
        //RIFFA Interface
        input riffa_clk,
        input riffa_rst,    // riffa_reset includes riffa_endpoint resets
        // Rx interface
        output CHNL_RX_CLK, 
        input CHNL_RX, 
        output CHNL_RX_ACK, 
        input CHNL_RX_LAST, 
        input[31:0] CHNL_RX_LEN, 
        input[30:0] CHNL_RX_OFF, 
        input[C_PCI_DATA_WIDTH - 1:0] CHNL_RX_DATA, 
        input CHNL_RX_DATA_VALID, 
        output CHNL_RX_DATA_REN,
        // Tx interface
        output CHNL_TX_CLK, 
        output CHNL_TX, 
        input CHNL_TX_ACK, 
        output CHNL_TX_LAST, 
        output[31:0] CHNL_TX_LEN, 
        output[30:0] CHNL_TX_OFF, 
        output[C_PCI_DATA_WIDTH - 1:0] CHNL_TX_DATA, 
        output CHNL_TX_DATA_VALID, 
        input CHNL_TX_DATA_REN,
        
        //App Interface
        input clk,
        input rst
    );
    
    parameter PE_BITS = $clog2(NUM_PES);
    
    assign CHNL_RX_CLK = riffa_clk;
    assign CHNL_TX_CLK = riffa_clk;
    
    wire sched_rd_en;
    wire[C_PCI_DATA_WIDTH - 1:0] sched_dna_data;
    wire sched_dna_valid;
    
    wire[`SIG_CHNL_LENGTH_W - 1:0] dna_len;
    wire sender_en, sender_idle;
    
    wire refread_set;
    //wire[PE_BITS - 1:0] refread_cluster_id;
    wire[C_PCI_DATA_WIDTH - 1:0] refread_data;
    
    pcie_data_receiver #(.C_PCI_DATA_WIDTH(C_PCI_DATA_WIDTH), .NUM_PES(NUM_PES)) i_riffa_recv(
        .clk(riffa_clk),
        .rst(rst),
        
        //RIFFA Interface
        .CHNL_RX(CHNL_RX),
        .CHNL_RX_ACK(CHNL_RX_ACK),
        .CHNL_RX_DATA_REN(CHNL_RX_DATA_REN),
        .CHNL_RX_DATA_VALID(CHNL_RX_DATA_VALID),
        .CHNL_RX_DATA(CHNL_RX_DATA),
        .CHNL_RX_LEN(CHNL_RX_LEN),
        
        
        //Scheduler Interface
        .dna_rd_en(sched_rd_en),
        .dna_data(sched_dna_data),
        .dna_valid(sched_dna_valid),
        
        // RefRead Manager Interface
        .refread_set(refread_set),
        //.refread_cluster_id(refread_cluster_id),
        .refread_data(refread_data),
        
        //Sender Interface
        .dna_len(dna_len),
        .sender_ready(sender_idle),
        .sender_en(sender_en)
    );
    
    wire[C_PCI_DATA_WIDTH*NUM_PES - 1:0] refread_reads;
    
    refread_manager #(.C_PCI_DATA_WIDTH(C_PCI_DATA_WIDTH), .NUM_REFS(NUM_PES)) i_refread_manager(
        .clk(CHNL_RX_CLK),
        .rst(rst),
        
        .set(refread_set),
        //.cluster_id(refread_cluster_id),
        .refread_in(refread_data),
        
        .refread_out(refread_reads)
    );
    
    wire[NUM_CLUSTERS - 1:0] shd_rd_en, shd_valid;
    wire[NUM_CLUSTERS*C_PCI_DATA_WIDTH - 1:0] shd_dna;
    
    SHDScheduler #(.DNA_DATA_WIDTH(C_PCI_DATA_WIDTH), .NUM_CLUSTERS(NUM_CLUSTERS)) i_sched (
        .clk(riffa_clk),
        .rst(rst),
        
        //Receiver Interface
        .dna_rd_en(sched_rd_en),
        .dna_data_in(sched_dna_data),
        .dna_valid_in(sched_dna_valid),
        
        //Cluster Interface
        .shd_clk(clk),
        .shd_rd_en_in(shd_rd_en),
        .shd_valid_out(shd_valid),
        .shd_dna_data_out(shd_dna)
    );
    
    wire[NUM_CLUSTERS - 1:0] coll_rd_en, coll_data_valid;
    wire[NUM_CLUSTERS*NUM_PES - 1:0] coll_data;
    
    genvar i;
    generate
        for(i = 0; i < NUM_CLUSTERS; i = i + 1) begin
            SHDCluster #(.DNA_DATA_WIDTH(C_PCI_DATA_WIDTH), .NUM_PES(NUM_PES)) i_cluster (
                .clk(clk),
                .rst(rst),
                
                //Scheduler Interface
                .dna_in(shd_dna[i*C_PCI_DATA_WIDTH +: C_PCI_DATA_WIDTH]),
                .dna_ref_in(refread_reads),
                .dna_valid_in(shd_valid[i]),
                .dna_rd_en(shd_rd_en[i]),
                
                //Collector Interface
                .coll_clk(riffa_clk),
                .coll_rd_en(coll_rd_en[i]),
                .coll_dna_err(coll_data[i*NUM_PES +: NUM_PES]),
                .coll_valid(coll_data_valid[i])
            );
        end
    endgenerate
    
    wire sender_ready, sender_data_valid;
    wire[NUM_PES - 1:0] sender_data;
    
    SHDCollector #(.DNA_DATA_WIDTH(C_PCI_DATA_WIDTH), .NUM_CLUSTERS(NUM_CLUSTERS), .NUM_PES(NUM_PES)) i_collector (
        .clk(riffa_clk),
        .rst(rst),
        
        //Cluster Interface
        .cluster_rd_en(coll_rd_en),
        .cluster_data(coll_data),
        .cluster_valid(coll_data_valid),
        
        //Sender Interface
        .sender_ready(sender_ready),
        .sender_data_valid(sender_data_valid),
        .sender_data(sender_data)
    );
    
    pcie_data_sender #(.C_PCI_DATA_WIDTH(C_PCI_DATA_WIDTH), .INPUT_DATA_WIDTH(NUM_PES), .NUM_PES(NUM_PES)) i_sender(
        .clk(CHNL_TX_CLK),
        .rst(rst),
        
        //Collector Interface
        .coll_ready(sender_ready),
        .coll_data_valid(sender_data_valid),
        .coll_data(sender_data),
        
        //RIFFA TX Interface
        .CHNL_TX(CHNL_TX), 
        .CHNL_TX_ACK(CHNL_TX_ACK), 
        .CHNL_TX_LAST(CHNL_TX_LAST), 
        .CHNL_TX_LEN(CHNL_TX_LEN), 
        .CHNL_TX_OFF(CHNL_TX_OFF), 
        .CHNL_TX_DATA(CHNL_TX_DATA), 
        .CHNL_TX_DATA_VALID(CHNL_TX_DATA_VALID), 
        .CHNL_TX_DATA_REN(CHNL_TX_DATA_REN),
        
        //Sender Interface
        .dna_len(dna_len),
        .en(sender_en),
        .idle(sender_idle)
    );
    
endmodule
