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
// HF fingerprint capture — repeatedly sends WUPA and records raw ADC envelope
// samples of the tag response into BigBuf for clone-detection analysis.
//-----------------------------------------------------------------------------

#include "hf_fingerprint.h"
#include "proxmark3_arm.h"
#include "BigBuf.h"
#include "fpgaloader.h"
#include "iso14443a.h"
#include "cmd.h"
#include "dbprint.h"
#include "commonutil.h"
#include "ticks.h"
#include "string.h"
#include "util.h"

#define HF_FP_DEFAULT_CAPTURES   64
#define HF_FP_DEFAULT_SAMPLES    1536
#define HF_FP_HEADROOM           2048
#define HF_FP_BASELINE_MS        5
#define HF_FP_SAMPLE_TIMEOUT_MS  10

void HfFingerprint14a(const hf_fingerprint_req_t *req) {

    uint16_t captures    = req->captures    ? req->captures    : HF_FP_DEFAULT_CAPTURES;
    uint16_t samples_per = req->samples_per ? req->samples_per : HF_FP_DEFAULT_SAMPLES;

    // Ensure total capture fits in BigBuf with headroom
    uint32_t total = (uint32_t)captures * samples_per;
    uint32_t avail = BigBuf_get_size() - HF_FP_HEADROOM;
    if (total > avail) {
        captures = avail / samples_per;
        if (captures == 0) {
            Dbprintf("HF fingerprint: buffer too small");
            reply_ng(CMD_HF_FINGERPRINT, PM3_EOVFLOW, NULL, 0);
            return;
        }
        total = (uint32_t)captures * samples_per;
    }

    BigBuf_free();
    BigBuf_Clear_ext(false);
    uint8_t *buf = BigBuf_get_addr();

    // Activate field and ISO14443A reader
    iso14443a_setup(FPGA_HF_ISO14443A_READER_LISTEN);

    // Measure baseline: average raw ADC over a quiet period
    uint32_t baseline_sum = 0;
    uint32_t baseline_cnt = 0;
    uint32_t timer = GetTickCount();
    while (GetTickCountDelta(timer) < HF_FP_BASELINE_MS) {
        if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY) {
            baseline_sum += (uint8_t)(AT91C_BASE_SSC->SSC_RHR);
            baseline_cnt++;
        }
    }
    uint16_t baseline = baseline_cnt ? (baseline_sum / baseline_cnt) : 0;

    Dbprintf("HF fingerprint: captures=%u samples=%u baseline=%u", captures, samples_per, baseline);

    uint16_t completed = 0;

    for (uint16_t n = 0; n < captures; n++) {

        if (BUTTON_PRESS() || data_available()) {
            Dbprintf("HF fingerprint: aborted after %u captures", n);
            break;
        }

        WDT_HIT();

        // Send WUPA (0x52, 7-bit short frame)
        uint8_t wupa[] = { 0x52 };
        ReaderTransmitBitsPar(wupa, 7, NULL, NULL);

        // Switch FPGA to listen mode and capture raw ADC samples
        FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_ISO14443A | FPGA_HF_ISO14443A_READER_LISTEN);

        uint8_t *dst = buf + ((uint32_t)n * samples_per);
        uint16_t cnt = 0;
        timer = GetTickCount();

        while (cnt < samples_per && GetTickCountDelta(timer) < HF_FP_SAMPLE_TIMEOUT_MS) {
            if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_RXRDY) {
                dst[cnt++] = (uint8_t)(AT91C_BASE_SSC->SSC_RHR);
            }
        }

        // Zero-fill if we timed out before collecting all samples
        if (cnt < samples_per) {
            memset(dst + cnt, 0, samples_per - cnt);
        }

        // Send HLTA to reset the tag
        uint8_t hlta[] = { 0x50, 0x00 };
        ReaderTransmit(hlta, sizeof(hlta), NULL);

        SpinDelayUs(1000);
        completed++;
    }

    // Turn off field
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();

    hf_fingerprint_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.protocol    = 1;  // ISO14443A
    resp.captures    = completed;
    resp.samples_per = samples_per;
    resp.baseline    = baseline;

    reply_ng(CMD_HF_FINGERPRINT, PM3_SUCCESS, (uint8_t *)&resp, sizeof(resp));
}
