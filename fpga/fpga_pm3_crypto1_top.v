//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
//
// Top-level module for the crypto1 FPGA bitstream.
// Includes HF reader, ISO14443-A, and the crypto1 engine.
//-----------------------------------------------------------------------------

`include "define.v"

module fpga_top(
    input ck_1356meg,
    input ck_1356megb,
    input spck,
    input pck0,
    input ncs,
    input [7:0] adc_d,
    input cross_hi,
    input cross_lo,
    input mosi,
    input ssp_dout,

    output ssp_din,
    output ssp_frame,
    output ssp_clk,
    output adc_clk,
    output adc_noe,
    output miso,
    output pwr_lo,
    output pwr_hi,
    output pwr_oe1,
    output pwr_oe2,
    output pwr_oe3,
    output pwr_oe4,
    output dbg
);

// In all modes, let the ADC's outputs be enabled.
assign adc_noe = 1'b0;

// P39 (spck) is a GCLKIOB pad on the XC2S30 — must use IBUFG so the mapper
// can place the signal on a global clock buffer.  Without this explicit
// instantiation ISE may infer a plain IBUF, which is illegal on GCLK pads.
wire spck_bufg;
IBUFG spck_ibufg(.I(spck), .O(spck_bufg));

//-----------------------------------------------------------------------------
// The SPI receiver. This sets up the configuration word, which the rest of
// the logic looks at to determine how to connect the A/D and the coil
// drivers (i.e., which section gets it). Also assign some symbolic names
// to the configuration bits, for use below.
//-----------------------------------------------------------------------------

// Receive 16bits of data from ARM here.
reg [15:0] shift_reg;
always @(posedge spck_bufg) if (~ncs) shift_reg <= {shift_reg[14:0], mosi};

reg trace_enable;

reg [5:0] hf_edge_detect_threshold;
reg [5:0] hf_edge_detect_threshold_high;

reg [8:0] conf_word;

initial
begin
    hf_edge_detect_threshold <= 7;
    hf_edge_detect_threshold_high <= 20;
end

// Crypto1 SPI load pulse synchronization
// Set on posedge ncs when command is 4-7, cleared after one pck0 cycle.
reg crypto1_spi_load_ncs;
reg crypto1_spi_load;
reg crypto1_spi_load_prev;

always @(posedge ncs)
begin
    // 4 bit command
    case (shift_reg[15:12])
        `FPGA_CMD_SET_CONFREG:  conf_word <= shift_reg[8:0];
        `FPGA_CMD_TRACE_ENABLE: trace_enable <= shift_reg[0];
        `FPGA_CMD_SET_EDGE_DETECT_THRESHOLD:
        begin
            hf_edge_detect_threshold <= shift_reg[5:0];
            hf_edge_detect_threshold_high <= shift_reg[11:6];
        end
    endcase

    // Commands 4-7: set flag for crypto1 engine
    if (shift_reg[15:14] == 2'b01)
        crypto1_spi_load_ncs <= 1'b1;
    else
        crypto1_spi_load_ncs <= 1'b0;
end

// Synchronize crypto1_spi_load_ncs into pck0 domain as a single-cycle pulse
always @(posedge pck0)
begin
    crypto1_spi_load_prev <= crypto1_spi_load_ncs;
    crypto1_spi_load      <= crypto1_spi_load_ncs & ~crypto1_spi_load_prev;
end

//-----------------------------------------------------------------------------
// And then we instantiate the modules corresponding to each of the FPGA's
// major modes, and use muxes to connect the outputs of the active mode to
// the output pins.
//-----------------------------------------------------------------------------

//   Mode 0: HF reader
hi_reader hr(
    .ck_1356meg (ck_1356megb),
    .adc_d      (adc_d),
    .subcarrier_frequency (conf_word[5:4]),
    .minor_mode (conf_word[3:0]),
    .ssp_dout   (ssp_dout),

    .ssp_din    (mux0_ssp_din),
    .ssp_frame  (mux0_ssp_frame),
    .ssp_clk    (mux0_ssp_clk),
    .adc_clk    (mux0_adc_clk),
    .pwr_lo     (mux0_pwr_lo),
    .pwr_hi     (mux0_pwr_hi),
    .pwr_oe1    (mux0_pwr_oe1),
    .pwr_oe2    (mux0_pwr_oe2),
    .pwr_oe3    (mux0_pwr_oe3),
    .pwr_oe4    (mux0_pwr_oe4),
    .debug      (mux0_debug)
);

//   Mode 2: HF ISO14443-A
hi_iso14443a hisn(
    .ck_1356meg (ck_1356meg),
    .adc_d      (adc_d),
    .mod_type   (conf_word[3:0]),
    .ssp_dout   (ssp_dout),

    .ssp_din    (mux2_ssp_din),
    .ssp_frame  (mux2_ssp_frame),
    .ssp_clk    (mux2_ssp_clk),
    .adc_clk    (mux2_adc_clk),
    .pwr_lo     (mux2_pwr_lo),
    .pwr_hi     (mux2_pwr_hi),
    .pwr_oe1    (mux2_pwr_oe1),
    .pwr_oe2    (mux2_pwr_oe2),
    .pwr_oe3    (mux2_pwr_oe3),
    .pwr_oe4    (mux2_pwr_oe4),
    .debug      (mux2_debug),
    .edge_detect_threshold (hf_edge_detect_threshold),
    .edge_detect_threshold_high (hf_edge_detect_threshold_high)
);

//   Mode 6: crypto1 engine
crypto1_engine ce(
    .clk        (pck0),
    .rst        (conf_word[8:6] == `FPGA_MAJOR_MODE_OFF),
    .ssp_dout   (ssp_dout),

    .ssp_din    (mux6_ssp_din),
    .ssp_frame  (mux6_ssp_frame),
    .ssp_clk    (mux6_ssp_clk),
    .spi_load   (crypto1_spi_load),
    .spi_param_sel (shift_reg[14:12]),
    .spi_data   (shift_reg[11:0]),
    .hit        (mux6_hit),
    .ready      (mux6_ready)
);

// Unused mux inputs tied to 0
assign mux1_ssp_clk = 1'b0, mux1_ssp_din = 1'b0, mux1_ssp_frame = 1'b0;
assign mux1_pwr_lo  = 1'b0, mux1_pwr_hi  = 1'b0;
assign mux1_pwr_oe1 = 1'b0, mux1_pwr_oe2 = 1'b0, mux1_pwr_oe3  = 1'b0, mux1_pwr_oe4 = 1'b0;
assign mux1_adc_clk = 1'b0, mux1_debug   = 1'b0;

assign mux3_ssp_clk = 1'b0, mux3_ssp_din = 1'b0, mux3_ssp_frame = 1'b0;
assign mux3_pwr_lo  = 1'b0, mux3_pwr_hi  = 1'b0;
assign mux3_pwr_oe1 = 1'b0, mux3_pwr_oe2 = 1'b0, mux3_pwr_oe3  = 1'b0, mux3_pwr_oe4 = 1'b0;
assign mux3_adc_clk = 1'b0, mux3_debug   = 1'b0;

assign mux4_ssp_clk = 1'b0, mux4_ssp_din = 1'b0, mux4_ssp_frame = 1'b0;
assign mux4_pwr_lo  = 1'b0, mux4_pwr_hi  = 1'b0;
assign mux4_pwr_oe1 = 1'b0, mux4_pwr_oe2 = 1'b0, mux4_pwr_oe3  = 1'b0, mux4_pwr_oe4 = 1'b0;
assign mux4_adc_clk = 1'b0, mux4_debug   = 1'b0;

assign mux5_ssp_clk = 1'b0, mux5_ssp_din = 1'b0, mux5_ssp_frame = 1'b0;
assign mux5_pwr_lo  = 1'b0, mux5_pwr_hi  = 1'b0;
assign mux5_pwr_oe1 = 1'b0, mux5_pwr_oe2 = 1'b0, mux5_pwr_oe3  = 1'b0, mux5_pwr_oe4 = 1'b0;
assign mux5_adc_clk = 1'b0, mux5_debug   = 1'b0;

// Mode 6 (crypto1): no antenna drive, no ADC clock
assign mux6_pwr_lo  = 1'b0, mux6_pwr_hi  = 1'b0;
assign mux6_pwr_oe1 = 1'b0, mux6_pwr_oe2 = 1'b0, mux6_pwr_oe3  = 1'b0, mux6_pwr_oe4 = 1'b0;
assign mux6_adc_clk = 1'b0, mux6_debug   = 1'b0;

// Mode 7: OFF (all zero)
assign mux7_ssp_clk = 1'b0, mux7_ssp_din = 1'b0, mux7_ssp_frame = 1'b0;
assign mux7_pwr_lo  = 1'b0, mux7_pwr_hi  = 1'b0;
assign mux7_pwr_oe1 = 1'b0, mux7_pwr_oe2 = 1'b0, mux7_pwr_oe3  = 1'b0, mux7_pwr_oe4 = 1'b0;
assign mux7_adc_clk = 1'b0, mux7_debug   = 1'b0;

// Major modes:
//   mux0 = HF reader
//   mux1 = unused
//   mux2 = HF ISO14443-A
//   mux3 = unused
//   mux4 = unused
//   mux5 = unused
//   mux6 = crypto1 engine
//   mux7 = FPGA_MAJOR_MODE_OFF

mux8 mux_ssp_clk   (.sel(conf_word[8:6]), .y(ssp_clk  ), .x0(mux0_ssp_clk  ), .x1(mux1_ssp_clk  ), .x2(mux2_ssp_clk  ), .x3(mux3_ssp_clk  ), .x4(mux4_ssp_clk  ), .x5(mux5_ssp_clk  ), .x6(mux6_ssp_clk  ), .x7(mux7_ssp_clk  ) );
mux8 mux_ssp_din   (.sel(conf_word[8:6]), .y(ssp_din  ), .x0(mux0_ssp_din  ), .x1(mux1_ssp_din  ), .x2(mux2_ssp_din  ), .x3(mux3_ssp_din  ), .x4(mux4_ssp_din  ), .x5(mux5_ssp_din  ), .x6(mux6_ssp_din  ), .x7(mux7_ssp_din  ) );
mux8 mux_ssp_frame (.sel(conf_word[8:6]), .y(ssp_frame), .x0(mux0_ssp_frame), .x1(mux1_ssp_frame), .x2(mux2_ssp_frame), .x3(mux3_ssp_frame), .x4(mux4_ssp_frame), .x5(mux5_ssp_frame), .x6(mux6_ssp_frame), .x7(mux7_ssp_frame) );
mux8 mux_pwr_oe1   (.sel(conf_word[8:6]), .y(pwr_oe1  ), .x0(mux0_pwr_oe1  ), .x1(mux1_pwr_oe1  ), .x2(mux2_pwr_oe1  ), .x3(mux3_pwr_oe1  ), .x4(mux4_pwr_oe1  ), .x5(mux5_pwr_oe1  ), .x6(mux6_pwr_oe1  ), .x7(mux7_pwr_oe1  ) );
mux8 mux_pwr_oe2   (.sel(conf_word[8:6]), .y(pwr_oe2  ), .x0(mux0_pwr_oe2  ), .x1(mux1_pwr_oe2  ), .x2(mux2_pwr_oe2  ), .x3(mux3_pwr_oe2  ), .x4(mux4_pwr_oe2  ), .x5(mux5_pwr_oe2  ), .x6(mux6_pwr_oe2  ), .x7(mux7_pwr_oe2  ) );
mux8 mux_pwr_oe3   (.sel(conf_word[8:6]), .y(pwr_oe3  ), .x0(mux0_pwr_oe3  ), .x1(mux1_pwr_oe3  ), .x2(mux2_pwr_oe3  ), .x3(mux3_pwr_oe3  ), .x4(mux4_pwr_oe3  ), .x5(mux5_pwr_oe3  ), .x6(mux6_pwr_oe3  ), .x7(mux7_pwr_oe3  ) );
mux8 mux_pwr_oe4   (.sel(conf_word[8:6]), .y(pwr_oe4  ), .x0(mux0_pwr_oe4  ), .x1(mux1_pwr_oe4  ), .x2(mux2_pwr_oe4  ), .x3(mux3_pwr_oe4  ), .x4(mux4_pwr_oe4  ), .x5(mux5_pwr_oe4  ), .x6(mux6_pwr_oe4  ), .x7(mux7_pwr_oe4  ) );
mux8 mux_pwr_lo    (.sel(conf_word[8:6]), .y(pwr_lo   ), .x0(mux0_pwr_lo   ), .x1(mux1_pwr_lo   ), .x2(mux2_pwr_lo   ), .x3(mux3_pwr_lo   ), .x4(mux4_pwr_lo   ), .x5(mux5_pwr_lo   ), .x6(mux6_pwr_lo   ), .x7(mux7_pwr_lo   ) );
mux8 mux_pwr_hi    (.sel(conf_word[8:6]), .y(pwr_hi   ), .x0(mux0_pwr_hi   ), .x1(mux1_pwr_hi   ), .x2(mux2_pwr_hi   ), .x3(mux3_pwr_hi   ), .x4(mux4_pwr_hi   ), .x5(mux5_pwr_hi   ), .x6(mux6_pwr_hi   ), .x7(mux7_pwr_hi   ) );
mux8 mux_adc_clk   (.sel(conf_word[8:6]), .y(adc_clk  ), .x0(mux0_adc_clk  ), .x1(mux1_adc_clk  ), .x2(mux2_adc_clk  ), .x3(mux3_adc_clk  ), .x4(mux4_adc_clk  ), .x5(mux5_adc_clk  ), .x6(mux6_adc_clk  ), .x7(mux7_adc_clk  ) );
mux8 mux_dbg       (.sel(conf_word[8:6]), .y(dbg      ), .x0(mux0_debug    ), .x1(mux1_debug    ), .x2(mux2_debug    ), .x3(mux3_debug    ), .x4(mux4_debug    ), .x5(mux5_debug    ), .x6(mux6_debug    ), .x7(mux7_debug    ) );

endmodule
