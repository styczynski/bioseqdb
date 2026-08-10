`timescale 1ns / 1ps
//////////////////////////////////////////////////////////////////////////////////
// Company: 
// Engineer: Hasan Hassan
// 
// Create Date: 08/25/2015 03:27:08 PM
// Design Name: 
// Module Name: SHD_top_tb
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


module SHD_top_tb();

    localparam C_PCI_DATA_WIDTH = 128;
    
    reg riffa_clk = 0, riffa_rst;
    reg shd_clk = 0, shd_rst;
    
    wire CHNL_RX_CLK, CHNL_RX_ACK, CHNL_RX_DATA_REN;
    reg CHNL_RX = 1'b1, CHNL_RX_LAST = 1'b1, CHNL_RX_VALID = 1'b1;
    reg[31:0] CHNL_RX_LEN = 2048;
    reg[30:0] CHNL_RX_OFF = 0;
    wire[C_PCI_DATA_WIDTH - 1:0] CHNL_RX_DATA;
    
    wire CHNL_TX_CLK, CHNL_TX, CHNL_TX_LAST, CHNL_TX_DATA_VALID;
    reg CHNL_TX_ACK, CHNL_TX_DATA_REN;
    wire[31:0] CHNL_TX_LEN;
    wire[30:0] CHNL_TX_OFF;
    wire[C_PCI_DATA_WIDTH - 1:0] CHNL_TX_DATA;

    SHD_top #(.C_PCI_DATA_WIDTH(C_PCI_DATA_WIDTH)) uut (
        //RIFFA Interface
        .riffa_clk(riffa_clk),
        .riffa_rst(riffa_rst),    // riffa_reset includes riffa_endpoint resets
        // Rx interface
        .CHNL_RX_CLK(CHNL_RX_CLK), 
        .CHNL_RX(CHNL_RX), 
        .CHNL_RX_ACK(CHNL_RX_ACK), 
        .CHNL_RX_LAST(CHNL_RX_LAST), 
        .CHNL_RX_LEN(CHNL_RX_LEN), 
        .CHNL_RX_OFF(CHNL_RX_OFF), 
        .CHNL_RX_DATA(CHNL_RX_DATA), 
        .CHNL_RX_DATA_VALID(CHNL_RX_VALID), 
        .CHNL_RX_DATA_REN(CHNL_RX_DATA_REN),
        // Tx interface
        .CHNL_TX_CLK(CHNL_TX_CLK), 
        .CHNL_TX(CHNL_TX), 
        .CHNL_TX_ACK(CHNL_TX_ACK), 
        .CHNL_TX_LAST(CHNL_TX_LAST), 
        .CHNL_TX_LEN(CHNL_TX_LEN), 
        .CHNL_TX_OFF(CHNL_TX_OFF), 
        .CHNL_TX_DATA(CHNL_TX_DATA), 
        .CHNL_TX_DATA_VALID(CHNL_TX_DATA_VALID), 
        .CHNL_TX_DATA_REN(CHNL_TX_DATA_REN),
        
        //App Interface
        .clk(shd_clk),
        .rst(shd_rst)
    );
    
    //Clock gen
    localparam RIFFA_CLK_PERIOD = 4;
    localparam SHD_CLK_PERIOD = 20;
    
    always begin
        #(RIFFA_CLK_PERIOD/2);
        riffa_clk = ~riffa_clk;
    end
    
    always begin
        #(SHD_CLK_PERIOD/2);
        shd_clk = ~shd_clk;
    end
    
    reg[31:0] data_state = 0;
    //wire[91:0] data[0:3] = {92'b11110101010010010001001000011001001100100101111010110111111110110110110101001111100001001111,
    //                      92'b11110101010010010001000010000100010011111011101111111101111011100101100001010100101000010011,
    //                      92'b11010100111110000100111101101110000111011101011111011101110101010001010101111111100101010101,
    //                      92'b11110101010010010001000010000100010011111011101111111101111011100101100001010100101000010011 };
    
    wire[127:0] data[0:15] = {
                          128'h0000000_0000000_0000000_0000000,
                          128'h0000000_0000000_0000000_0000000,
                          128'h0000000_0000000_0000000_0000000,
                          128'h0000000_0000000_0000000_0000000,
                          
                          128'h0000000_0000000_0000000_0000000,
                          128'h0000000_0000000_0000000_0000000,
                          128'h0000000_0000000_0000000_0000000,
                          128'h0000000_0000000_0000000_0000000,
                          
                          
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          128'hffffffff_ffffffff_ffffffff_ffffffff,
                          128'hffffffff_ffffffff_ffffffff_ffffffff};
    
    reg[31:0] data_sent = 0;
    //Data Sender : always send data and increment the data value when sent
    always@(posedge CHNL_RX_CLK) begin
        if(riffa_rst) begin
            data_state <= 0;
            data_sent <= 0;
        end
        else begin
            if(CHNL_RX_DATA_REN) begin
                data_state <= data_state + 1;
                data_sent <= data_sent + 1;
            end
        end
    end
    
    //assign CHNL_RX_DATA[91:0] = data[data_state];
    //assign CHNL_RX_DATA[127:92] = 0;
    assign CHNL_RX_DATA = (data_state > 8) ? data[9] : data[data_state];
    /*assign CHNL_RX_DATA[31:0] = data_sent*4;
    assign CHNL_RX_DATA[63:32] = data_sent*4 + 1;
    assign CHNL_RX_DATA[95:64] = data_sent*4 + 2;
    assign CHNL_RX_DATA[127:96] = data_sent*4 + 3;*/
    
    reg[31:0] recv_counter = 0;
    
    //Data Receiver : just pop the data
    /*always@* begin
        CHNL_TX_ACK = 1'b0;
        CHNL_TX_DATA_REN = 1'b0;
                
        if(CHNL_TX_DATA_VALID) begin
            CHNL_TX_ACK = 1'b1;
            CHNL_TX_DATA_REN = 1'b1;
        end
    end*/
    
    always@(posedge riffa_clk) begin
        if(riffa_rst) begin
            recv_counter <= 0;
        end
        else begin
            if(CHNL_TX_DATA_REN) begin
                recv_counter <= recv_counter + 1;
            end
        end
    end
    
    
    
    initial begin
        riffa_rst = 1'b1;
        shd_rst = 1'b1;
        
        #100; //keep rst high for 100ns
        
        riffa_rst = 1'b0;
        shd_rst = 1'b0;
        
        CHNL_TX_ACK = 1'b1;
        CHNL_TX_DATA_REN = 1'b1;
        
        /*#1000000
        
        while(1) begin
            if(CHNL_TX_DATA_VALID) begin
                CHNL_TX_ACK = 1'b1;
                CHNL_TX_DATA_REN = 1'b1;
            end
            else begin
                CHNL_TX_ACK = 1'b0;
                CHNL_TX_DATA_REN = 1'b0;
            end
            #RIFFA_CLK_PERIOD;
        end*/
    
    
    end
    
    
endmodule
