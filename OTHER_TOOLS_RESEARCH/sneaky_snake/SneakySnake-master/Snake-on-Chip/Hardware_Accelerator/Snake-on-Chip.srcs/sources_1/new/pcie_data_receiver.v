`timescale 1ns / 1ps


`include "riffa.vh"

//Gets data from RIFFA and informs the Scheduler about data waiting to be processed
module pcie_data_receiver #(parameter C_PCI_DATA_WIDTH = 128, NUM_PES = 8) (
    input clk,
    input rst,
    
    //RIFFA Interface
    input CHNL_RX,
    output reg CHNL_RX_ACK,
    output CHNL_RX_DATA_REN,
    input CHNL_RX_DATA_VALID,
    input[C_PCI_DATA_WIDTH - 1:0] CHNL_RX_DATA,
    input[`SIG_CHNL_LENGTH_W - 1:0] CHNL_RX_LEN,
    
    //Scheduler Interface
    input dna_rd_en,
    output[C_PCI_DATA_WIDTH - 1:0] dna_data,
    output dna_valid,
    
    // RefRead Manager Interface
    output reg refread_set,
    //output reg[PE_BITS - 1:0] refread_cluster_id,
    output reg[C_PCI_DATA_WIDTH - 1:0] refread_data,
    
    //Sender Interface
    output reg[`SIG_CHNL_LENGTH_W - 1:0] dna_len,
    input sender_ready,
    output reg sender_en
);

parameter PE_BITS = $clog2(NUM_PES);

reg[C_PCI_DATA_WIDTH - 1:0] dna_r;

reg[PE_BITS - 1:0] ref_counter = 0;

reg[1:0] state = 0, state_next;
localparam STATE_IDLE = 2'b00;
localparam STATE_FETCH_REF = 2'b01;
localparam STATE_PROCESSING = 2'b10;

wire recv_fifo_full, recv_fifo_empty;
reg recv_fifo_wren = 1'b0;

//assign CHNL_RX_ACK = ~recv_fifo_full;
assign CHNL_RX_DATA_REN = (state != STATE_IDLE) && ~recv_fifo_full;


//next state combinational logic
always@* begin
    CHNL_RX_ACK = 1'b0;
    state_next = state;

    case(state)
        STATE_IDLE: begin
            //accept the transaction if the sender is ready
            if(sender_ready & CHNL_RX) begin
                CHNL_RX_ACK = 1'b1;
                
                //if(CHNL_RX_DATA_VALID) begin
                    state_next = STATE_FETCH_REF;
                //end
            end
        
        end //STATE_IDLE
        
        STATE_FETCH_REF: begin
            if(CHNL_RX_DATA_VALID) begin
                if(ref_counter == (NUM_PES - 1))
                    state_next <= STATE_PROCESSING;
            end        
        end //STATE_FETCH_REF
        
        STATE_PROCESSING: begin
            if(dna_len == 0 && ~recv_fifo_full) begin
                state_next = STATE_IDLE;
            end
        end //STATE_PROCESSING
    endcase
end

//next state sequential logic
always@(posedge clk) begin
    if(rst) begin
        state <= STATE_IDLE;
    end
    else begin
        state <= state_next;
    end
end

//calculate the length of the mapping and keep track of received data
always@(posedge clk) begin
    if(rst) begin
        dna_len <= 0;
        sender_en <= 1'b0;
    end
    else begin
        sender_en <= 1'b0;
        
        if(CHNL_RX_ACK /*&& CHNL_RX_DATA_VALID*/) begin
            dna_len <= CHNL_RX_LEN >> 2; //the first 4 words (16 bytes) are the reference dna data
            sender_en <= 1'b1;
        end
        else begin
            if(CHNL_RX_DATA_VALID && ~recv_fifo_full && (state != STATE_IDLE)) begin
                if(dna_len > 0)
                    dna_len <= dna_len - 1;
            end
        end
    end
end

// ref_counter logic
always@(posedge clk) begin
    if(rst) begin
        ref_counter <= 0;
    end
    else begin
        case(state)
            STATE_IDLE: begin
                ref_counter <= 0;
            end
            STATE_FETCH_REF: begin
                if(CHNL_RX_DATA_VALID) begin
                    ref_counter <= ref_counter + 1;
                end
            end
        endcase
    end
end

//fetch the dna reads
always@(posedge clk) begin
    if(rst) begin
        dna_r <= 0;
        recv_fifo_wren <= 1'b0;
        
        /*refread_set <= 1'b0;
        refread_cluster_id <= 0;
        refread_data <= 0;*/
    end
    else begin
        //refread_set <= 1'b0;
    
        if(CHNL_RX_DATA_VALID) begin
            case(state)
                STATE_IDLE: begin
                    recv_fifo_wren <= 1'b0;
                end //STATE_IDLE
                
                /*STATE_FETCH_REF: begin
                    refread_set <= 1'b1;
                    refread_cluster_id <= ref_counter;
                    refread_data <= CHNL_RX_DATA;
                end*/
                
                STATE_PROCESSING: begin
                    if(~recv_fifo_full) begin
                        dna_r <= CHNL_RX_DATA;
                        recv_fifo_wren <= 1'b1;
                    end
                end
            endcase
        end // CHNL_RX_DATA_VALID
         else begin
             if(~recv_fifo_full) begin
                 recv_fifo_wren <= 1'b0;
             end
         end
    end
end

always@* begin
    refread_set = 1'b0;
    //refread_cluster_id = {PE_BITS{1'bx}};
    refread_data = {C_PCI_DATA_WIDTH{1'bx}};
    
    if((state == STATE_FETCH_REF) && CHNL_RX_DATA_VALID) begin
        refread_set = 1'b1;
        //refread_cluster_id = ref_counter;
        refread_data = CHNL_RX_DATA;
    end
end

pcie_recv_fifo i_recv_fifo(
    .clk(clk),      // input wire clk
    .srst(rst),    // input wire srst
    .din({dna_r}),      // input wire [255 : 0] din
    .wr_en(recv_fifo_wren),  // input wire wr_en
    .rd_en(dna_rd_en & dna_valid),  // input wire rd_en
    .dout({dna_data}),    // output wire [255 : 0] dout
    .full(recv_fifo_full),    // output wire full
    .empty(recv_fifo_empty)  // output wire empty
);

assign dna_valid = ~recv_fifo_empty;


endmodule