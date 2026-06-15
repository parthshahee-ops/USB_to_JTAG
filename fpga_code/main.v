`timescale 1ns/1ns

(* top *) module top ( 
    (* iopad_external_pin, clkbuf_inhibit *) input clk,     // System Clock (50MHz) 
    (* iopad_external_pin *) output clk_en, 
    (* iopad_external_pin *) input rst_n,                   // System Reset (Active Low) [PIN 17]
    
    // Physical SPI Pins (Connect these to FPGA I/O) 
    (* iopad_external_pin *) input spi_ss_n,                // Chip Select [PIN 4]
    (* iopad_external_pin *) input spi_sck,                 // Serial Clock [PIN 3]
    (* iopad_external_pin *) input spi_mosi,                // Master Out [PIN 5]
    (* iopad_external_pin *) output spi_miso,               // Slave Out [PIN 6]
    (* iopad_external_pin *) output spi_miso_en,             // MISO OE Pad Link [PIN 6 OE]
    
    // Unused Audio Port (Preserved for pin alignment compatibility)
    (* iopad_external_pin *) output reg debug_led,                     
    	(* iopad_external_pin *)	output debug_led_en
);

    assign clk_en = 1'b1;      // Power up core oscillator block
	assign debug_led_en = 1'b1; 
    
    reg [7:0] por_counter = 8'd0;
    wire sys_rst_n = (por_counter == 8'hFF);

    always @(posedge clk) begin
        if (por_counter != 8'hFF) begin
            por_counter <= por_counter + 1'b1;
        end
    end
    
    // Internal Data Busses
    wire [7:0] rx_data;
    wire       rx_valid;
    
    // Direct hardware loopback: Send exactly what we just received
    wire [7:0] tx_data = rx_data; 
    
    // Instantiate your SPI protocol core
    spi_target #(
        .CPOL(1'b0),
        .CPHA(1'b0),
        .WIDTH(8),
        .LSB(1'b0)
    ) u_spi_target (
        .i_clk(clk),
        .i_rst_n(rst_n),
        .i_enable(1'b1),               // Hard-wire enable to HIGH
        
        // Physical SPI Bus
        .i_ss_n(spi_ss_n),
        .i_sck(spi_sck),
        .i_mosi(spi_mosi),
        .o_miso(spi_miso),
        .o_miso_oe(spi_miso_en),       // Dynamically drive MISO buffer based on CS status
        
        // Internal Data Interface
        .o_rx_data(rx_data),
        .o_rx_data_valid(rx_valid),
        .i_tx_data(tx_data),
        .o_tx_data_hold()              // Floating: Used for flow control, but not needed for simple loopback
    );

 // 50MHz clock * 0.25 seconds = 12,500,000 cycles
    parameter BLINK_TIME = 24'd12_500_000; 
    reg [23:0] blink_timer = 24'd0;

    always @(posedge clk or negedge sys_rst_n) begin
        if (!sys_rst_n) begin
            debug_led <= 1'b0;
            blink_timer <= 24'd0;
        end else if (rx_valid) begin
            // ANY valid byte arrived! Turn LED on and reset the countdown timer.
            debug_led <= 1'b1;
            blink_timer <= BLINK_TIME;
        end else if (blink_timer > 24'd0) begin
            // Keep LED on and count down
            blink_timer <= blink_timer - 1'b1;
            debug_led <= 1'b1;
        end else begin
            // Timer finished, turn LED off
            debug_led <= 1'b0;
        end
    end
endmodule