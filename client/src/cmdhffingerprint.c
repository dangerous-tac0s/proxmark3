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
#include "cmdhffingerprint.h"
#include "cliparser.h"
#include "ui.h"
#include "fileutils.h"
#include "comms.h"
#include "cmdparser.h"
#include "pm3_cmd.h"
#include "jansson.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static int CmdHelp(const char *Cmd);

// ---------------------------------------------------------------------------
// average_captures: compute per-sample mean and stddev from N captures,
// normalize by removing baseline DC offset and scaling peak to 1.0
// ---------------------------------------------------------------------------
int fingerprint_average_captures(const uint8_t *raw, uint16_t captures, uint16_t samples_per,
                                 uint16_t baseline, fingerprint_segment_t *seg) {
    if (raw == NULL || seg == NULL || captures == 0 || samples_per == 0) {
        return PM3_EINVARG;
    }

    float *mean = calloc(samples_per, sizeof(float));
    float *sd = calloc(samples_per, sizeof(float));
    if (mean == NULL || sd == NULL) {
        free(mean);
        free(sd);
        return PM3_EMALLOC;
    }

    // accumulate sums
    for (uint16_t c = 0; c < captures; c++) {
        const uint8_t *row = raw + (uint32_t)c * samples_per;
        for (uint16_t s = 0; s < samples_per; s++) {
            mean[s] += (float)row[s];
        }
    }

    // compute mean
    for (uint16_t s = 0; s < samples_per; s++) {
        mean[s] /= (float)captures;
    }

    // accumulate variance
    for (uint16_t c = 0; c < captures; c++) {
        const uint8_t *row = raw + (uint32_t)c * samples_per;
        for (uint16_t s = 0; s < samples_per; s++) {
            float diff = (float)row[s] - mean[s];
            sd[s] += diff * diff;
        }
    }

    // finalize stddev
    for (uint16_t s = 0; s < samples_per; s++) {
        sd[s] = sqrtf(sd[s] / (float)captures);
    }

    // remove baseline DC offset
    float dc = (float)baseline;
    for (uint16_t s = 0; s < samples_per; s++) {
        mean[s] -= dc;
    }

    // find peak absolute value for normalization
    float peak = 0.0f;
    for (uint16_t s = 0; s < samples_per; s++) {
        float a = fabsf(mean[s]);
        if (a > peak) {
            peak = a;
        }
    }

    // normalize so peak == 1.0
    if (peak > 0.0f) {
        for (uint16_t s = 0; s < samples_per; s++) {
            mean[s] /= peak;
            sd[s] /= peak;
        }
    }

    seg->waveform = mean;
    seg->stddev = sd;
    seg->length = samples_per;
    return PM3_SUCCESS;
}

// ---------------------------------------------------------------------------
// fingerprint_correlate: normalized cross-correlation between two segments
// returns value in [-1.0, 1.0], where 1.0 = identical shape
// ---------------------------------------------------------------------------
float fingerprint_correlate(const fingerprint_segment_t *ref, const fingerprint_segment_t *test) {
    if (ref == NULL || test == NULL || ref->waveform == NULL || test->waveform == NULL) {
        return 0.0f;
    }

    uint16_t len = ref->length;
    if (test->length < len) {
        len = test->length;
    }
    if (len == 0) {
        return 0.0f;
    }

    // compute means
    double mean_ref = 0.0, mean_test = 0.0;
    for (uint16_t i = 0; i < len; i++) {
        mean_ref += ref->waveform[i];
        mean_test += test->waveform[i];
    }
    mean_ref /= len;
    mean_test /= len;

    // compute normalized cross-correlation
    double num = 0.0, den_ref = 0.0, den_test = 0.0;
    for (uint16_t i = 0; i < len; i++) {
        double dr = ref->waveform[i] - mean_ref;
        double dt = test->waveform[i] - mean_test;
        num += dr * dt;
        den_ref += dr * dr;
        den_test += dt * dt;
    }

    double denom = sqrt(den_ref * den_test);
    if (denom < 1e-12) {
        return 0.0f;
    }

    return (float)(num / denom);
}

// ---------------------------------------------------------------------------
// JSON save / load helpers
// ---------------------------------------------------------------------------
int fingerprint_save(const char *name, const fingerprint_profile_t *fp) {
    if (name == NULL || fp == NULL) {
        return PM3_EINVARG;
    }

    json_t *root = json_object();
    if (root == NULL) {
        return PM3_EMALLOC;
    }

    json_object_set_new(root, "version", json_integer(fp->version));
    json_object_set_new(root, "protocol", json_integer(fp->protocol));
    json_object_set_new(root, "captures", json_integer(fp->captures));
    json_object_set_new(root, "baseline", json_integer(fp->baseline));
    json_object_set_new(root, "created", json_string(fp->created));
    json_object_set_new(root, "samples", json_integer(fp->atqa.length));

    // waveform array
    json_t *waveform_arr = json_array();
    json_t *stddev_arr = json_array();
    if (waveform_arr == NULL || stddev_arr == NULL) {
        json_decref(root);
        return PM3_EMALLOC;
    }

    for (uint16_t i = 0; i < fp->atqa.length; i++) {
        json_array_append_new(waveform_arr, json_real(fp->atqa.waveform[i]));
        json_array_append_new(stddev_arr, json_real(fp->atqa.stddev[i]));
    }
    json_object_set_new(root, "atqa_waveform", waveform_arr);
    json_object_set_new(root, "atqa_stddev", stddev_arr);

    // build filename
    char fname[256];
    snprintf(fname, sizeof(fname), "hf_fingerprint_%s.json", name);

    int res = json_dump_file(root, fname, JSON_INDENT(2));
    json_decref(root);

    if (res != 0) {
        PrintAndLogEx(ERR, "failed to write " _RED_("%s"), fname);
        return PM3_EFILE;
    }

    PrintAndLogEx(SUCCESS, "saved fingerprint to " _YELLOW_("%s"), fname);
    return PM3_SUCCESS;
}

int fingerprint_load(const char *name, fingerprint_profile_t *fp) {
    if (name == NULL || fp == NULL) {
        return PM3_EINVARG;
    }

    char fname[256];
    snprintf(fname, sizeof(fname), "hf_fingerprint_%s.json", name);

    json_error_t error;
    json_t *root = json_load_file(fname, 0, &error);
    if (root == NULL) {
        PrintAndLogEx(ERR, "failed to load " _RED_("%s") " (%s)", fname, error.text);
        return PM3_EFILE;
    }

    memset(fp, 0, sizeof(fingerprint_profile_t));

    fp->version  = (uint8_t)json_integer_value(json_object_get(root, "version"));
    fp->protocol = (uint8_t)json_integer_value(json_object_get(root, "protocol"));
    fp->captures = (uint16_t)json_integer_value(json_object_get(root, "captures"));
    fp->baseline = (uint16_t)json_integer_value(json_object_get(root, "baseline"));

    const char *created = json_string_value(json_object_get(root, "created"));
    if (created) {
        snprintf(fp->created, sizeof(fp->created), "%s", created);
    }

    uint16_t samples = (uint16_t)json_integer_value(json_object_get(root, "samples"));
    json_t *waveform_arr = json_object_get(root, "atqa_waveform");
    json_t *stddev_arr = json_object_get(root, "atqa_stddev");

    if (!json_is_array(waveform_arr) || !json_is_array(stddev_arr) || samples == 0) {
        PrintAndLogEx(ERR, "invalid fingerprint data in " _RED_("%s"), fname);
        json_decref(root);
        return PM3_EFILE;
    }

    fp->atqa.length = samples;
    fp->atqa.waveform = calloc(samples, sizeof(float));
    fp->atqa.stddev = calloc(samples, sizeof(float));

    if (fp->atqa.waveform == NULL || fp->atqa.stddev == NULL) {
        free(fp->atqa.waveform);
        free(fp->atqa.stddev);
        fp->atqa.waveform = NULL;
        fp->atqa.stddev = NULL;
        json_decref(root);
        return PM3_EMALLOC;
    }

    for (uint16_t i = 0; i < samples; i++) {
        fp->atqa.waveform[i] = (float)json_real_value(json_array_get(waveform_arr, i));
        fp->atqa.stddev[i] = (float)json_real_value(json_array_get(stddev_arr, i));
    }

    json_decref(root);
    PrintAndLogEx(SUCCESS, "loaded fingerprint from " _YELLOW_("%s"), fname);
    return PM3_SUCCESS;
}

void fingerprint_free(fingerprint_profile_t *fp) {
    if (fp == NULL) {
        return;
    }
    free(fp->atqa.waveform);
    free(fp->atqa.stddev);
    fp->atqa.waveform = NULL;
    fp->atqa.stddev = NULL;
    fp->atqa.length = 0;
}

// ---------------------------------------------------------------------------
// CLI commands
// ---------------------------------------------------------------------------
static int CmdHFFingerprintList(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf fingerprint list",
                  "List saved RF fingerprint profiles",
                  "hf fingerprint list");

    void *argtable[] = {
        arg_param_begin,
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, true);
    CLIParserFree(ctx);

    PrintAndLogEx(INFO, "use " _YELLOW_("`hf 14a fingerprint --enroll -n <name>`") " to create profiles");
    PrintAndLogEx(INFO, "profiles are saved as " _YELLOW_("hf_fingerprint_<name>.json") " in the current directory");
    return PM3_SUCCESS;
}

static int CmdHFFingerprintRemove(const char *Cmd) {
    CLIParserContext *ctx;
    CLIParserInit(&ctx, "hf fingerprint remove",
                  "Remove a saved RF fingerprint profile",
                  "hf fingerprint remove -n mycard");

    void *argtable[] = {
        arg_param_begin,
        arg_str1("n", "name", "<str>", "Profile name to remove"),
        arg_param_end
    };
    CLIExecWithReturn(ctx, Cmd, argtable, false);

    int nlen = 0;
    char name[64] = {0};
    CLIParamStrToBuf(arg_get_str(ctx, 1), (uint8_t *)name, sizeof(name) - 1, &nlen);
    CLIParserFree(ctx);

    if (nlen == 0) {
        PrintAndLogEx(ERR, "name required");
        return PM3_EINVARG;
    }

    char fname[256];
    snprintf(fname, sizeof(fname), "hf_fingerprint_%s.json", name);

    if (remove(fname) != 0) {
        PrintAndLogEx(ERR, "failed to remove " _RED_("%s"), fname);
        return PM3_EFILE;
    }

    PrintAndLogEx(SUCCESS, "removed " _GREEN_("%s"), fname);
    return PM3_SUCCESS;
}

static command_t CommandTable[] = {
    {"--------", CmdHelp,                 AlwaysAvailable, "----------------------- " _CYAN_("RF Fingerprint") " -----------------------"},
    {"help",     CmdHelp,                 AlwaysAvailable, "This help"},
    {"list",     CmdHFFingerprintList,    AlwaysAvailable, "List saved fingerprint profiles"},
    {"remove",   CmdHFFingerprintRemove,  AlwaysAvailable, "Remove a saved fingerprint profile"},
    {NULL, NULL, NULL, NULL}
};

int CmdHFFingerprint(const char *Cmd) {
    clearCommandBuffer();
    return CmdsParse(CommandTable, Cmd);
}

static int CmdHelp(const char *Cmd) {
    (void)Cmd;
    CmdsHelp(CommandTable);
    return PM3_SUCCESS;
}
