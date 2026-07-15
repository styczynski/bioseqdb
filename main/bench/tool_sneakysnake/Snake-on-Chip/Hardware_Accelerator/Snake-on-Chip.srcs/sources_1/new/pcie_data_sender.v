`timescale 1ns / 1ps

`include "riffa.vh"

module pcie_data_sender #(parameter C_PCI_DATA_WIDTH = 128, INPUT_DATA_WIDTH = 8, NUM_PES = 8) (
        input clk,
        input rst,
        
        //Collector Interface
        output coll_ready,
        input coll_data_valid,
        input[INPUT_DATA_WIDTH - 1:0] coll_data,
        
        //RIFFA TX Interface
        output CHNL_TX, 
        input CHNL_TX_ACK, 
        output CHNL_TX_LAST, 
        output[`SIG_CHNL_LENGTH_W - 1:0] CHNL_TX_LEN, 
        output[30:0] CHNL_TX_OFF, 
        output[C_PCI_DATA_WIDTH - 1:0] CHNL_TX_DATA, 
        output reg CHNL_TX_DATA_VALID, 
        input CHNL_TX_DATA_REN,
        
        input[`SIG_CHNL_LENGTH_W - 1:0] dna_len,
        output idle,
        input en
    );
    
    localparam DATA_PER_TX = C_PCI_DATA_WIDTH/INPUT_DATA_WIDTH; //number of data chunks that can fit with in C_PCI_DATA_WIDTH
    parameter IT_BITS = $clog2(DATA_PER_TX);
    
    
    reg state = STATE_IDLE;
    localparam STATE_IDLE = 1'b0;
    localparam STATE_SENDING = 1'b1;
    
    reg[`SIG_CHNL_LENGTH_W - 1:0] dna_len_r, send_len;
    
    reg tx_issued;
    //state transition logic
    always@(posedge clk) begin
        if(rst) begin
            state <= STATE_IDLE;
            dna_len_r <= 0;
            send_len <= 0;
        end
        else begin
            case(state)
                STATE_IDLE: begin
                    if(en) begin
                        dna_len_r <= (dna_len - NUM_PES)*NUM_PES >> 7 /*to 128-bit chunks*/ ; // excluding reference dna reads
                        send_len <= (dna_len - NUM_PES)*NUM_PES >> 5/*to 32-bit words*/; // excluding reference dna reads
                        state <= STATE_SENDING;
                    end
                end //STATE_IDLE
                STATE_SENDING: begin
                    if(tx_issued) begin
                        dna_len_r <= dna_len_r - 1;
                        
                        if(dna_len_r == 1) begin
                            state <= STATE_IDLE;
                        end
                    end
                end //STATE_SENDING
            
            endcase
        end
    end
    
    assign idle = (state == STATE_IDLE);
    
    
    //Register Input Data
    reg[INPUT_DATA_WIDTH - 1:0] data_r;
    reg data_valid_r;
    wire fetch_input = coll_ready;
    
    always@(posedge clk) begin
        if(rst) begin
            data_r <= 0;
            data_valid_r <= 0;
        end
        else begin
            if(fetch_input) begin
                data_r <= coll_data;
                data_valid_r <= coll_data_valid;
            end
        end
    end
    
    //Put data chuck in tx buffer
    reg[IT_BITS - 1:0] iter = 0;
    reg[C_PCI_DATA_WIDTH - 1:0] tx_buffer;
    reg tx_buffer_valid = 0;
    
    always@(posedge clk) begin
        if(rst) begin
            tx_buffer <= 0;
            tx_buffer_valid <= 0;
        end
        else begin
            if(data_valid_r && coll_ready) begin
                tx_buffer[iter*INPUT_DATA_WIDTH +:INPUT_DATA_WIDTH] <= data_r;
                iter <= iter + 1'b1;
                
                tx_buffer_valid <= &iter;
            end
            else if(tx_issued) begin
                tx_buffer_valid <= 1'b0;
            end
        end
    end
    
    //TODO: consider adding one more register stage for better timing
    
    //Send tx buffer to RIFFA
    assign CHNL_TX_LEN = send_len; //C_PCI_DATA_WIDTH/32;
    assign CHNL_TX_LAST = 1'b1; //(dna_len_r == 1);
    assign CHNL_TX_OFF = 0;
    assign CHNL_TX_DATA = tx_buffer;
    assign CHNL_TX = (state == STATE_SENDING);
    
    always@* begin
        tx_issued = 1'b0;
        CHNL_TX_DATA_VALID = 1'b0;
        
        if(state == STATE_SENDING) begin
            if(tx_buffer_valid) begin
                CHNL_TX_DATA_VALID = 1'b1;
                
                if(CHNL_TX_DATA_REN) begin
                    tx_issued = 1'b1;
                end
            end //tx_buffer_valid
        end
    end
    
    assign coll_ready = ~tx_buffer_valid || (tx_buffer_valid && tx_issued);
    
endmodule
