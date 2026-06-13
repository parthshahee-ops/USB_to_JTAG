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
    (* iopad_external_pin *) input PDM                      // PDM Audio Stream [PIN 18]
);

    assign clk_en = 1'b1; // Power up core oscillator block 

    wire [7:0] rx_data_wire;   // Data bus coming out of receiver 
    wire       rx_valid_pulse; // High for 1 cycle when an 8-bit frame completes 
    reg  [7:0] tx_data_reg;    // Register feeding the transmitter buffer 

    // Standard Echo Loopback Pipeline Logic    
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            tx_data_reg <= 8'h00; // Reset register to clear bus memory 
        end else if (rx_valid_pulse) begin
            tx_data_reg <= rx_data_wire; // Latch incoming data straight to output 
        end
    end
    // Factory SPI Target Core Instantiation 
    spi_target #(
        .CPOL(1'b0),   // Mode 0 (Clock idles low) 
        .CPHA(1'b0),   // Mode 0 (Sample on rising edge) 
        .WIDTH(8),     // Standard 8-bit transmission frames 
        .LSB(1'b0)     // MSB First byte order 
    ) u_spi_target (
        .i_clk(clk),
        .i_rst_n(rst_n),
        .i_enable(1'b1),        // Permanently activate the peripheral block 

        // Connect physical SPI bus tracks 
        .i_ss_n(spi_ss_n),
        .i_sck(spi_sck),
        .i_mosi(spi_mosi),
        .o_miso(spi_miso),
        .o_miso_oe(spi_miso_en),

        // Connect internal logic data lanes 
        .o_rx_data(rx_data_wire),
        .o_rx_data_valid(rx_valid_pulse),
        .i_tx_data(tx_data_reg), 
        .o_tx_data_hold()        
    );

endmodule

module spi_target #(
  parameter CPOL   = 1'b0,  // When one, clock is low in idle, otherwise clock is high
  parameter CPHA   = 1'b0,  // When one, sampling occurs at falling edge, otherwise at rising edge of non-inverted clock
  parameter WIDTH  = 8,     // Determines the data width of SPI to the input and output data buses
  parameter LSB    = 1'b0   // When one, data starts from LSB, otherwise data starts from MSB 
) (
// common ports
  input                  i_clk,           // input clock signal
  input                  i_rst_n,         // input negative reset signal
// control signal
  input                  i_enable,        // input enable SPI target signal
// SPI interface ports
  input                  i_ss_n,          // input target select signal
  input                  i_sck,           // input spi clock signal
  input                  i_mosi,          // input controller output target input signal
  output                 o_miso,          // output controller input target output signal
  output                 o_miso_oe,       // output miso enable output signal
//RX internal ports
  output reg [WIDTH-1:0] o_rx_data,       // output data bus
  output reg             o_rx_data_valid, // output receive data valid signal
//TX internal ports
  input      [WIDTH-1:0] i_tx_data,       // input data bus
  output                 o_tx_data_hold   // output signal used to get tx data from i_tx_data input
);

// Signal declaration
  reg               [2:0] r_ss_n_sync, r_sck_sync;
  reg [$clog2(WIDTH-1):0] r_transmision_count;
  reg         [WIDTH-1:0] r_miso_data;
  wire                    w_sck_r_edge, w_sck_f_edge, w_sck_edge, w_sck_edge_op;

// SPI input signals synchronisation
  always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
      r_ss_n_sync <= 'b111;
    end else if (i_enable) begin
      r_ss_n_sync <= {r_ss_n_sync[1:0], i_ss_n};
    end else begin
      r_ss_n_sync <= 'b111;
    end
  end

  always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
      r_sck_sync <= 'h0;
    end else if (i_enable) begin
      r_sck_sync <= {r_sck_sync[1:0], i_sck};
    end else begin
      r_sck_sync <= 'h0;
    end
  end

// Create rising and falling edge signals from spi_clk signal
  assign w_sck_r_edge  = ~r_sck_sync[2] & r_sck_sync[1];
  assign w_sck_f_edge  = r_sck_sync[2] & ~r_sck_sync[1];
  assign w_sck_edge    = (CPHA^CPOL) ? w_sck_f_edge : w_sck_r_edge;
  assign w_sck_edge_op = (CPHA^CPOL) ? w_sck_r_edge : w_sck_f_edge;
  
// Create transmission bit counter
  always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
      r_transmision_count <= 'h0;
    end else if (!i_enable || r_ss_n_sync[1]) begin
      r_transmision_count <= 'h0;
    end else if (w_sck_edge) begin
      if (r_transmision_count == WIDTH-1) begin
        r_transmision_count <= 'h0;
      end else begin
        r_transmision_count <= r_transmision_count + 1;
      end
    end
  end

// Create o_rx_data bus and valid signals
  always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
      o_rx_data <= 'h0;
    end else if (w_sck_edge) begin
      if (LSB) begin
        o_rx_data <= {i_mosi, o_rx_data[WIDTH-1:1]};
      end else begin
        o_rx_data <= {o_rx_data[WIDTH-2:0], i_mosi};
      end
    end
  end

  always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
      o_rx_data_valid <= 1'b0;
    end else if (r_ss_n_sync[1] || (r_transmision_count == 0 && w_sck_edge)) begin
      o_rx_data_valid <= 1'b0;
    end else if (w_sck_edge && r_transmision_count == WIDTH-1) begin
      o_rx_data_valid <= 1'b1;
    end
  end

// Create o_tx_data_hold signal
  assign o_tx_data_hold = (~CPHA & r_ss_n_sync[2] & ~r_ss_n_sync[1]) | (r_transmision_count == 0 & w_sck_edge_op);

// Create o_miso and OE signals
  always @(posedge i_clk or negedge i_rst_n) begin
    if (!i_rst_n) begin
      r_miso_data <= 'h0;
    end else if (o_tx_data_hold) begin
      r_miso_data <= i_tx_data;
    end else if (w_sck_edge_op) begin
      if (LSB) begin
        r_miso_data <= r_miso_data >> 1;
      end else begin
        r_miso_data <= r_miso_data << 1;
      end
    end
  end

	assign o_miso    = (LSB) ? r_miso_data[0] : r_miso_data[WIDTH-1];
  	assign o_miso_oe = ~i_ss_n;
endmodule