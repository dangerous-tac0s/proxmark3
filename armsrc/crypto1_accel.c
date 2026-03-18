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
// CRYPTO-1 FPGA acceleration driver
//
// Drives the crypto1_engine FPGA module for hardware-accelerated MIFARE
// Classic key search. Loads attack parameters via SPI, streams candidate
// keys via SSC, and detects hits.
//
// FPGA SPI parameter protocol:
//   The crypto1_engine uses spi_param_sel = shift_reg[14:12], which maps
//   SPI commands 4-7 to param_sel values 4-7:
//
//   FPGA_CMD_CRYPTO1_PARAMS     (4<<12) -> param_sel 4: shift 12 bits into
//                                          accumulator (3 writes for 32-bit value)
//   FPGA_CMD_CRYPTO1_LATCH_UID  (5<<12) -> param_sel 5: latch accumulator -> uid
//   FPGA_CMD_CRYPTO1_LATCH_NT   (6<<12) -> param_sel 6: latch accumulator -> nt
//   FPGA_CMD_CRYPTO1_LATCH_MISC (7<<12) -> param_sel 7: latch or trigger:
//     data[2:0] = 0: latch accumulator -> nr
//     data[2:0] = 1: latch accumulator -> ar
//     data[2:0] = 2: latch accumulator -> check_data
//     data[3]   = 1: start trigger (begin key trial)
//
// SSC protocol:
//   The engine generates ssp_clk at 6 MHz (pck0/4). After trigger:
//   1. Engine enters LOAD_KEY: clocks in 48 bits from ssp_dout (ARM TX)
//   2. Engine processes: init LFSR, feed UID^NT, verify against check_data
//   3. On hit: asserts ssp_frame for 1 bit, shifts out 48-bit key on ssp_din
//   4. Returns to IDLE (ready=1)
//-----------------------------------------------------------------------------

#include "crypto1_accel.h"

#include "proxmark3_arm.h"
#include "fpgaloader.h"
#include "ticks.h"
#include "dbprint.h"
#include "util.h"
#include "BigBuf.h"
#include "appmain.h"

// ============================================================================
// SPI parameter loading helpers
// ============================================================================

// Shift 32-bit value into the FPGA param_shift accumulator (3 x 12-bit writes)
static void crypto1_shift_param(uint32_t value) {
    FpgaSendCommand(FPGA_CMD_CRYPTO1_PARAMS, (value >> 20) & 0xFFF);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_PARAMS, (value >> 8) & 0xFFF);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_PARAMS, value & 0xFF);
}

// Load UID: shift in value, then latch to uid register
static void crypto1_load_uid(uint32_t uid) {
    crypto1_shift_param(uid);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_LATCH_UID, 0);
}

// Load NT: shift in value, then latch to nt register
static void crypto1_load_nt(uint32_t nt) {
    crypto1_shift_param(nt);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_LATCH_NT, 0);
}

// Load NR: shift in value, then latch via MISC with sel=0
static void crypto1_load_nr(uint32_t nr) {
    crypto1_shift_param(nr);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_LATCH_MISC, 0);
}

// Load AR: shift in value, then latch via MISC with sel=1
static void crypto1_load_ar(uint32_t ar) {
    crypto1_shift_param(ar);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_LATCH_MISC, 1);
}

// Load check_data: shift in value, then latch via MISC with sel=2
static void crypto1_load_check(uint32_t check) {
    crypto1_shift_param(check);
    FpgaSendCommand(FPGA_CMD_CRYPTO1_LATCH_MISC, 2);
}

// Trigger the engine to start accepting a key
static void crypto1_trigger(void) {
    FpgaSendCommand(FPGA_CMD_CRYPTO1_LATCH_MISC, 8); // data[3] = 1
}

// ============================================================================
// SSC setup for crypto1 mode
//
// The FPGA crypto1_engine generates ssp_clk at 6 MHz. We configure the SSC
// for 8-bit transfers, TX and RX clocked by the external clock (TK/RK pins),
// with continuous start mode so data flows as soon as we write to THR.
// ============================================================================
static void crypto1_setup_ssc(void) {
    // Assign SSC pins to peripheral
    AT91C_BASE_PIOA->PIO_ASR =
        GPIO_SSC_FRAME |
        GPIO_SSC_DIN   |
        GPIO_SSC_DOUT  |
        GPIO_SSC_CLK;
    AT91C_BASE_PIOA->PIO_PDR = GPIO_SSC_DOUT;

    AT91C_BASE_PMC->PMC_PCER = (1 << AT91C_ID_SSC);

    // Reset SSC
    AT91C_BASE_SSC->SSC_CR = AT91C_SSC_SWRST;

    // TX: clock from TK pin (external, ssp_clk from FPGA), continuous start
    AT91C_BASE_SSC->SSC_TCMR = SSC_CLOCK_MODE_SELECT(2) // TK pin
                              | SSC_CLOCK_MODE_START(0); // continuous

    // TX frame: 8 bits per word, MSB first, 1 word per transfer
    AT91C_BASE_SSC->SSC_TFMR = SSC_FRAME_MODE_BITS_IN_WORD(8)
                              | AT91C_SSC_MSBF
                              | SSC_FRAME_MODE_WORDS_PER_TRANSFER(0);

    // RX: clock from TX clock (internal loopback), continuous start
    AT91C_BASE_SSC->SSC_RCMR = SSC_CLOCK_MODE_SELECT(1) // from TX clock
                              | SSC_CLOCK_MODE_START(0); // continuous

    // RX frame: same as TX
    AT91C_BASE_SSC->SSC_RFMR = SSC_FRAME_MODE_BITS_IN_WORD(8)
                              | AT91C_SSC_MSBF
                              | SSC_FRAME_MODE_WORDS_PER_TRANSFER(0);

    // Enable TX and RX
    AT91C_BASE_SSC->SSC_CR = AT91C_SSC_RXEN | AT91C_SSC_TXEN;
}

// ============================================================================
// Send a 48-bit key via SSC and check for a hit response.
//
// The engine clocks in 48 bits on ssp_dout after trigger, then processes.
// If a hit occurs, the engine asserts ssp_frame and shifts out the 48-bit
// matching key on ssp_din.
//
// We send 6 bytes (48 bits, MSB first) and read back 6 bytes. During key
// loading the engine outputs 0 on ssp_din. After processing, if there's a
// hit, the engine outputs the key on ssp_din with ssp_frame asserted.
//
// Returns true if hit detected, with the key in *found_key.
// ============================================================================
static bool crypto1_try_key(uint64_t key, uint64_t *found_key) {
    uint8_t tx_buf[6];
    uint8_t rx_buf[6];

    // Pack 48-bit key into 6 bytes, MSB first
    tx_buf[0] = (key >> 40) & 0xFF;
    tx_buf[1] = (key >> 32) & 0xFF;
    tx_buf[2] = (key >> 24) & 0xFF;
    tx_buf[3] = (key >> 16) & 0xFF;
    tx_buf[4] = (key >> 8) & 0xFF;
    tx_buf[5] = key & 0xFF;

    // Trigger the engine
    crypto1_trigger();

    // Send the key and receive response via SSC
    // The engine will clock in 48 bits on ssp_dout during LOAD_KEY state,
    // then process. After processing, if hit, it shifts out the key on
    // ssp_din during REPORT_HIT state.
    //
    // We need to send 6 bytes for the key, then continue clocking to
    // receive the potential hit response (another 6 bytes).
    // Total: 12 bytes of SSC transfer (6 TX key + 6 RX padding for response).

    // First, drain any stale RX data
    while (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY) {
        (void)AT91C_BASE_SSC->SSC_RHR;
    }

    // Phase 1: Send the 48-bit key (6 bytes)
    for (int i = 0; i < 6; i++) {
        // Wait for TX ready
        uint32_t timeout = 10000;
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY)) {
            if (--timeout == 0) return false;
        }
        AT91C_BASE_SSC->SSC_THR = tx_buf[i];

        // Read corresponding RX byte (will be 0 during key loading)
        timeout = 10000;
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY)) {
            if (--timeout == 0) return false;
        }
        (void)AT91C_BASE_SSC->SSC_RHR;
    }

    // The engine now has the key and is processing it through the LFSR.
    // Processing takes: 24 (init) + 32 (uid^nt) + up to 32 (verify) = 88 cycles
    // at full clock speed (24 MHz). That's about 3.7 us.
    //
    // If there's a hit, the engine enters REPORT_HIT and clocks out
    // 48 bits on ssp_din at 6 MHz (ssp_clk rate). That's 8 us.
    //
    // If no hit, the engine returns to IDLE and ssp_clk keeps running
    // but ssp_din stays 0 and ssp_frame stays 0.
    //
    // We need to wait for processing to complete and then check for hit
    // by reading more SSC data. The engine generates ssp_clk continuously
    // (the divider runs always), so we can keep reading.
    //
    // Wait for engine processing by sending/receiving padding bytes.
    // Processing at 24 MHz takes ~88 cycles = ~3.7 us.
    // At 6 MHz SSC clock, that's about 22 bit periods = ~3 bytes.
    // Send 4 padding bytes to cover processing time.

    for (int i = 0; i < 4; i++) {
        uint32_t timeout = 10000;
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY)) {
            if (--timeout == 0) return false;
        }
        AT91C_BASE_SSC->SSC_THR = 0x00; // padding

        timeout = 10000;
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY)) {
            if (--timeout == 0) return false;
        }
        (void)AT91C_BASE_SSC->SSC_RHR;
    }

    // Phase 2: Read back the potential hit response (6 bytes = 48 bits)
    // If the engine is in REPORT_HIT, it will assert ssp_frame on bit 0
    // and shift out the matched key MSB first.
    // If no hit, we'll read all zeros.
    bool got_hit = false;
    for (int i = 0; i < 6; i++) {
        uint32_t timeout = 10000;
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY)) {
            if (--timeout == 0) return false;
        }
        AT91C_BASE_SSC->SSC_THR = 0x00; // dummy TX to generate clock

        timeout = 10000;
        while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY)) {
            if (--timeout == 0) return false;
        }
        rx_buf[i] = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
    }

    // Check if we received a non-zero response (hit)
    uint64_t rx_key = 0;
    for (int i = 0; i < 6; i++) {
        rx_key = (rx_key << 8) | rx_buf[i];
    }

    if (rx_key != 0) {
        got_hit = true;
        *found_key = rx_key;
    }

    return got_hit;
}

// ============================================================================
// Main search function
// ============================================================================
int crypto1_accel_search(crypto1_accel_params_t *params, uint64_t *found_key) {

    // Load the crypto1 FPGA bitstream
    FpgaDownloadAndGo(FPGA_BITSTREAM_CRYPTO1);

    // Load attack parameters via SPI
    crypto1_load_uid(params->uid);
    crypto1_load_nt(params->nt);
    crypto1_load_nr(params->nr);
    crypto1_load_ar(params->ar);
    crypto1_load_check(params->check);

    // Select crypto1 major mode
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_CRYPTO1);

    // Configure SSC for crypto1 operation
    crypto1_setup_ssc();

    // Small delay to let FPGA settle
    SpinDelay(1);

    uint64_t key_start = params->key_start;
    uint64_t key_end = params->key_end;

    Dbprintf("crypto1_accel: searching keys %012llx - %012llx",
             (unsigned long long)key_start, (unsigned long long)key_end);

    uint64_t count = 0;
    for (uint64_t key = key_start; key <= key_end; key++) {

        if (crypto1_try_key(key, found_key)) {
            Dbprintf("crypto1_accel: HIT! key = %012llx (after %llu trials)",
                     (unsigned long long)*found_key, (unsigned long long)(count + 1));
            FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
            return PM3_SUCCESS;
        }

        count++;

        // Check for abort every 256 keys
        if ((count & 0xFF) == 0) {
            if (BUTTON_PRESS() || data_available()) {
                Dbprintf("crypto1_accel: aborted after %llu trials",
                         (unsigned long long)count);
                FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
                return PM3_EOPABORTED;
            }
            WDT_HIT();
        }

        // Progress report every 65536 keys
        if ((count & 0xFFFF) == 0) {
            Dbprintf("crypto1_accel: %llu keys tested...",
                     (unsigned long long)count);
        }
    }

    Dbprintf("crypto1_accel: range exhausted after %llu trials, no match",
             (unsigned long long)count);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    return PM3_ESOFT;
}
