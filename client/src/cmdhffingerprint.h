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
// High frequency RF fingerprint commands
//-----------------------------------------------------------------------------

#ifndef CMDHF_FINGERPRINT_H__
#define CMDHF_FINGERPRINT_H__

#include "common.h"

typedef struct {
    float *waveform;
    float *stddev;
    uint16_t length;
} fingerprint_segment_t;

typedef struct {
    uint8_t version;
    uint8_t protocol;
    fingerprint_segment_t atqa;
    uint16_t captures;
    uint16_t baseline;
    char created[32];
} fingerprint_profile_t;

int fingerprint_average_captures(const uint8_t *raw, uint16_t captures, uint16_t samples_per,
                                 uint16_t baseline, fingerprint_segment_t *seg);
float fingerprint_correlate(const fingerprint_segment_t *ref, const fingerprint_segment_t *test);
int fingerprint_save(const char *name, const fingerprint_profile_t *fp);
int fingerprint_load(const char *name, fingerprint_profile_t *fp);
void fingerprint_free(fingerprint_profile_t *fp);

int CmdHFFingerprint(const char *Cmd);

#endif
