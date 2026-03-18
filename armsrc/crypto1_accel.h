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
//-----------------------------------------------------------------------------

#ifndef CRYPTO1_ACCEL_H__
#define CRYPTO1_ACCEL_H__

#include "common.h"
#include "pm3_cmd.h"

// Search key range using FPGA CRYPTO-1 accelerator.
// Returns PM3_SUCCESS if key found (written to *found_key).
// Returns PM3_ESOFT if range exhausted with no match.
// Returns PM3_EOPABORTED on button press.
int crypto1_accel_search(crypto1_accel_params_t *params, uint64_t *found_key);

#endif // CRYPTO1_ACCEL_H__
