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
// Testbench for crypto1_engine.v
//
// Verifies the Verilog CRYPTO-1 engine against C reference test vectors
// generated from common/crapto1/crypto1.c using BEBIT ordering.
//
// Reference test vectors (from C crypto1_word with BEBIT ordering):
//   uid=0xDEADBEEF  nt=0x12345678  nr=0xAAAAAAAA
//   uid^nt = 0xCC99E897
//
//   Key 0xFFFFFFFFFFFF:
//     init: odd=0xFFFFFF  even=0xFFFFFF
//     check_data (sequential keystream): 0xE5FCD738
//
//   Key 0x000000000000:
//     init: odd=0x000000  even=0x000000
//     keystream: 0x04D86264 (first byte 0x04 != 0xE5, early termination)
//-----------------------------------------------------------------------------

`timescale 1ns / 1ps

module tb_crypto1_engine;

    // ========================================================================
    // DUT signals
    // ========================================================================
    reg         clk;
    reg         rst;
    reg         ssp_dout;
    wire        ssp_din;
    wire        ssp_frame;
    wire        ssp_clk;
    reg         spi_load;
    reg  [2:0]  spi_param_sel;
    reg  [11:0] spi_data;
    wire        hit;
    wire        ready;

    // ========================================================================
    // DUT instantiation
    // ========================================================================
    crypto1_engine dut (
        .clk          (clk),
        .rst          (rst),
        .ssp_dout     (ssp_dout),
        .ssp_din      (ssp_din),
        .ssp_frame    (ssp_frame),
        .ssp_clk      (ssp_clk),
        .spi_load     (spi_load),
        .spi_param_sel(spi_param_sel),
        .spi_data     (spi_data),
        .hit          (hit),
        .ready        (ready)
    );

    // ========================================================================
    // Test parameters
    // ========================================================================
    localparam [31:0] UID        = 32'hDEADBEEF;
    localparam [31:0] NT         = 32'h12345678;
    localparam [31:0] NR         = 32'hAAAAAAAA;
    localparam [31:0] AR         = 32'h00000000;
    localparam [31:0] CHECK_DATA = 32'hE5FCD738;

    localparam [47:0] KEY_CORRECT = 48'hFFFFFFFFFFFF;
    localparam [47:0] KEY_WRONG   = 48'h000000000000;

    // ========================================================================
    // Clock: 24 MHz -> 42 ns period
    // ========================================================================
    localparam CLK_PERIOD = 42;
    initial clk = 0;
    always #(CLK_PERIOD/2) clk = ~clk;

    // ========================================================================
    // Hit latch: captures if hit was ever asserted during a trial
    // ========================================================================
    reg hit_seen;
    always @(posedge clk) begin
        if (rst)
            hit_seen <= 1'b0;
        else if (hit)
            hit_seen <= 1'b1;
    end

    // ========================================================================
    // Test tracking
    // ========================================================================
    integer pass_count = 0;
    integer fail_count = 0;
    integer test_num   = 0;

    // ========================================================================
    // Helper tasks
    // ========================================================================

    task wait_clks;
        input integer n;
        integer i;
        begin
            for (i = 0; i < n; i = i + 1)
                @(posedge clk);
        end
    endtask

    // Shift 12 bits into the engine's param_shift register (param_sel=4)
    task spi_shift12;
        input [11:0] value;
        begin
            @(posedge clk);
            spi_param_sel <= 3'd4;
            spi_data      <= value;
            spi_load      <= 1'b1;
            @(posedge clk);
            spi_load      <= 1'b0;
            @(posedge clk);
        end
    endtask

    // Load a 32-bit parameter: 3 shifts into param_shift, then latch.
    // After 3 writes of d1, d2, d3 to param_sel=4:
    //   param_shift = {d1[7:0], d2[11:0], d3[11:0]} = 32 bits
    // Then latch_sel latches param_shift into the target register.
    task spi_load_param;
        input [2:0]  latch_sel;   // 5=UID, 6=NT, 7=NR/AR/check (with data sub-select)
        input [11:0] latch_data;  // sub-select for param_sel=7
        input [31:0] value;
        begin
            spi_shift12({4'b0, value[31:24]});
            spi_shift12(value[23:12]);
            spi_shift12(value[11:0]);

            // Latch
            @(posedge clk);
            spi_param_sel <= latch_sel;
            spi_data      <= latch_data;
            spi_load      <= 1'b1;
            @(posedge clk);
            spi_load      <= 1'b0;
            @(posedge clk);
        end
    endtask

    task spi_trigger;
        begin
            @(posedge clk);
            spi_param_sel <= 3'd7;
            spi_data      <= 12'h008;  // data[3]=1 triggers
            spi_load      <= 1'b1;
            @(posedge clk);
            spi_load      <= 1'b0;
            @(posedge clk);
        end
    endtask

    // Send a 48-bit key via SSC, MSB first.
    //
    // The engine samples ssp_dout on each ssc_clk_rising (ssc_div == 1)
    // while in S_LOAD_KEY, for bit_counter 0..47.  We synchronize by
    // monitoring the engine's bit_counter: for each expected sample, we
    // set ssp_dout before the sample edge arrives.
    task send_key;
        input [47:0] key;
        integer i;
        begin
            // Pre-set MSB so it's available from the very first sample edge
            ssp_dout <= key[47];

            // Wait for engine to enter S_LOAD_KEY
            while (dut.state != 3'd1)
                @(posedge clk);

            // Feed 48 bits: for each bit_counter value 0..47, ensure
            // ssp_dout holds key[47-bc] before the sample edge.
            // We detect each sample by watching bit_counter increment.
            for (i = 47; i >= 0; i = i - 1) begin
                // Set up data for this bit
                ssp_dout <= key[i];
                // Wait until the engine has consumed this bit
                // (bit_counter advances from 47-i to 47-i+1, or state changes)
                if (i > 0) begin
                    while (dut.bit_counter == (47 - i) && dut.state == 3'd1)
                        @(posedge clk);
                end else begin
                    // Last bit: wait for state to change out of S_LOAD_KEY
                    while (dut.state == 3'd1)
                        @(posedge clk);
                end
            end
        end
    endtask

    // Wait for engine to return to IDLE (ready goes high), with timeout.
    task wait_engine_done;
        input integer timeout_clks;
        integer count;
        begin
            count = 0;
            while (!ready && count < timeout_clks) begin
                @(posedge clk);
                count = count + 1;
            end
            if (count >= timeout_clks)
                $display("  WARNING: engine did not become ready within %0d clocks", timeout_clks);
        end
    endtask

    // Clear the hit_seen latch (call before each trial)
    task clear_hit_latch;
        begin
            @(posedge clk);
            // Force clear via blocking assignment trick: wait until hit is low
            while (hit) @(posedge clk);
            hit_seen = 1'b0;
            @(posedge clk);
        end
    endtask

    task check;
        input [511:0] msg;
        input         cond;
        begin
            test_num = test_num + 1;
            if (cond) begin
                $display("  [PASS] %0d: %0s", test_num, msg);
                pass_count = pass_count + 1;
            end else begin
                $display("  [FAIL] %0d: %0s", test_num, msg);
                fail_count = fail_count + 1;
            end
        end
    endtask

    // ========================================================================
    // Main test sequence
    // ========================================================================
    initial begin
        rst           = 1;
        ssp_dout      = 0;
        spi_load      = 0;
        spi_param_sel = 0;
        spi_data      = 0;

        $display("");
        $display("===========================================================");
        $display(" crypto1_engine testbench");
        $display("===========================================================");

        wait_clks(10);
        rst = 0;
        wait_clks(5);

        // ================================================================
        // Load parameters
        // ================================================================
        $display("");
        $display("--- Loading parameters via SPI ---");
        spi_load_param(3'd5, 12'h000, UID);         // latch UID
        spi_load_param(3'd6, 12'h000, NT);          // latch NT
        spi_load_param(3'd7, 12'h000, NR);          // latch NR  (data[2:0]=0)
        spi_load_param(3'd7, 12'h001, AR);          // latch AR  (data[2:0]=1)
        spi_load_param(3'd7, 12'h002, CHECK_DATA);  // latch check (data[2:0]=2)
        wait_clks(5);

        check("UID loaded correctly", dut.uid == UID);
        check("NT loaded correctly", dut.nt == NT);
        check("NR loaded correctly", dut.nr == NR);
        check("CHECK_DATA loaded correctly", dut.check_data == CHECK_DATA);
        $display("  uid=0x%08X nt=0x%08X nr=0x%08X check=0x%08X",
                 dut.uid, dut.nt, dut.nr, dut.check_data);

        check("Ready before trigger", ready == 1'b1);
        check("No hit before trigger", hit == 1'b0);

        // ================================================================
        // Test A: Correct key -> hit
        // ================================================================
        $display("");
        $display("--- Test A: Correct key (0xFFFFFFFFFFFF) ---");

        clear_hit_latch;
        spi_trigger;
        send_key(KEY_CORRECT);

        // Wait for INIT_LFSR to complete
        while (dut.state == 3'd1) @(posedge clk);
        while (dut.state == 3'd2) @(posedge clk);

        if (dut.state == 3'd3) begin
            $display("  LFSR after init: odd=0x%06X even=0x%06X (expect FFFFFF, FFFFFF)",
                     dut.lfsr_odd, dut.lfsr_even);
            check("LFSR init odd=0xFFFFFF", dut.lfsr_odd == 24'hFFFFFF);
            check("LFSR init even=0xFFFFFF", dut.lfsr_even == 24'hFFFFFF);
        end

        // Wait for engine to finish (including hit report if any)
        wait_engine_done(5000);

        // Check hit via latch (hit was high during S_REPORT_HIT)
        $display("  hit_seen=%b ready=%b", hit_seen, ready);
        check("Correct key -> HIT (hit_seen)", hit_seen == 1'b1);
        check("Engine returns to IDLE", ready == 1'b1);

        // ================================================================
        // Test B: Wrong key -> no hit
        // ================================================================
        $display("");
        $display("--- Test B: Wrong key (0x000000000000) ---");

        clear_hit_latch;
        spi_trigger;
        send_key(KEY_WRONG);

        while (dut.state == 3'd1) @(posedge clk);
        while (dut.state == 3'd2) @(posedge clk);

        if (dut.state == 3'd3) begin
            $display("  LFSR after init: odd=0x%06X even=0x%06X (expect 000000, 000000)",
                     dut.lfsr_odd, dut.lfsr_even);
            check("Zero key LFSR init odd=0x000000", dut.lfsr_odd == 24'h000000);
            check("Zero key LFSR init even=0x000000", dut.lfsr_even == 24'h000000);
        end

        wait_engine_done(5000);

        $display("  hit_seen=%b ready=%b", hit_seen, ready);
        check("Wrong key -> NO hit", hit_seen == 1'b0);
        check("Engine IDLE after wrong key", ready == 1'b1);

        // ================================================================
        // Test C: Correct key again (reuse after miss)
        // ================================================================
        $display("");
        $display("--- Test C: Correct key again (reuse test) ---");

        clear_hit_latch;
        spi_trigger;
        send_key(KEY_CORRECT);
        wait_engine_done(5000);

        $display("  hit_seen=%b ready=%b", hit_seen, ready);
        check("Second correct key -> HIT", hit_seen == 1'b1);
        check("Engine IDLE after second hit", ready == 1'b1);

        // ================================================================
        // Test D: Verify early termination for wrong key
        // ================================================================
        $display("");
        $display("--- Test D: Early termination timing ---");

        clear_hit_latch;
        spi_trigger;
        send_key(KEY_WRONG);

        // Wait for verify state
        while (dut.state != 3'd4 && dut.state != 3'd0)
            @(posedge clk);

        if (dut.state == 3'd4) begin
            // Count cycles in verify state
            begin : count_block
                integer verify_cycles;
                verify_cycles = 0;
                while (dut.state == 3'd4) begin
                    @(posedge clk);
                    verify_cycles = verify_cycles + 1;
                end
                $display("  Verify lasted %0d cycles (expected <=8 for first-byte mismatch)",
                         verify_cycles);
                check("Early termination (verify < 32 cycles)", verify_cycles < 32);
            end
        end else begin
            $display("  Skipped verify without entering VERIFY state");
        end

        wait_engine_done(5000);

        // ================================================================
        // Test E: Verify key readback via SSC on hit
        // ================================================================
        $display("");
        $display("--- Test E: Key readback on hit ---");

        clear_hit_latch;
        spi_trigger;
        send_key(KEY_CORRECT);

        // Wait for hit
        while (!hit && dut.state != 3'd0) @(posedge clk);

        if (hit) begin
            // Engine is in S_REPORT_HIT, shifting out key via ssp_din
            // Capture 48 bits synced to ssp_clk
            begin : readback_block
                reg [47:0] readback_key;
                integer k;
                readback_key = 48'd0;
                for (k = 0; k < 48; k = k + 1) begin
                    @(posedge ssp_clk);
                    readback_key = {readback_key[46:0], ssp_din};
                end
                $display("  Readback key: 0x%012X (expect 0x%012X)",
                         readback_key, KEY_CORRECT);
                check("Key readback matches", readback_key == KEY_CORRECT);
            end
        end else begin
            $display("  No hit for key readback test");
            check("Key readback (no hit)", 1'b0);
        end

        wait_engine_done(5000);

        // ================================================================
        // Summary
        // ================================================================
        $display("");
        $display("===========================================================");
        $display(" Results: %0d passed, %0d failed out of %0d tests",
                 pass_count, fail_count, test_num);
        if (fail_count == 0)
            $display(" ALL TESTS PASSED");
        else
            $display(" SOME TESTS FAILED");
        $display("===========================================================");
        $display("");

        $finish;
    end

    // Timeout watchdog
    initial begin
        #3000000;
        $display("ERROR: Simulation timed out at 3 ms!");
        $finish;
    end

    // Waveform dump
    initial begin
        $dumpfile("testbench/tb_crypto1_engine.vcd");
        $dumpvars(0, tb_crypto1_engine);
    end

endmodule
