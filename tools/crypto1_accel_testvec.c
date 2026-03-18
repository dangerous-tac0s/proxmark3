//-----------------------------------------------------------------------------
// Generate test vectors for FPGA CRYPTO-1 accelerator validation.
//
// Simulates a MIFARE Classic authentication exchange with a known key,
// then prints the parameters needed for "hf mf accel" to recover it.
//
// Build:  gcc -O2 -I../common -o crypto1_accel_testvec crypto1_accel_testvec.c \
//         ../common/crapto1/crypto1.c ../common/crapto1/crapto1.c -lm
//
// Usage:  ./crypto1_accel_testvec [key_hex]
//         Default key: A0A1A2A3A4A5
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "crapto1/crapto1.h"

int main(int argc, char *argv[]) {
    uint64_t key = 0xA0A1A2A3A4A5ULL;
    if (argc > 1) {
        key = strtoull(argv[1], NULL, 16);
        key &= 0xFFFFFFFFFFFFULL;
    }

    // Fixed UID and tag nonce for reproducibility
    uint32_t uid = 0x01020304;
    uint32_t nt  = 0x11223344;
    // Random-ish reader nonce
    uint32_t nr  = 0xDEADBEEF;

    printf("=== CRYPTO-1 Accel Test Vector ===\n");
    printf("Key:  %012llX\n", (unsigned long long)key);
    printf("UID:  %08X\n", uid);
    printf("NT:   %08X\n", nt);
    printf("NR:   %08X\n", nr);

    // Initialize cipher with key
    struct Crypto1State state;
    crypto1_init(&state, key);

    // Feed UID ^ NT (tag nonce authentication step)
    crypto1_word(&state, uid ^ nt, 0);

    // Encrypt reader nonce (nr) and compute reader answer (ar)
    uint32_t nr_enc = crypto1_word(&state, nr, 1);
    uint32_t ar = prng_successor(nt, 64);
    uint32_t ar_enc = crypto1_word(&state, ar, 1);

    // Generate some encrypted "check" bytes (tag answer: suc(nt, 96))
    uint32_t at = prng_successor(nt, 96);
    uint32_t at_enc = crypto1_word(&state, at, 1);

    printf("NR_enc: %08X\n", nr_enc);
    printf("AR:     %08X  (prng_successor(nt, 64))\n", ar);
    printf("AR_enc: %08X\n", ar_enc);
    printf("AT:     %08X  (prng_successor(nt, 96))\n", at);
    printf("AT_enc: %08X\n", at_enc);

    // The FPGA engine expects:
    //   uid, nt           — card parameters
    //   nr (PLAINTEXT)    — the reader nonce we chose
    //   ar (PLAINTEXT)    — prng_successor(nt, 64)
    //   check_data        — the KEYSTREAM from the NR encryption phase
    //                        (= nr_enc XOR nr, the filter outputs during NR feed)
    uint32_t ks_nr = nr_enc ^ nr;  // keystream = encrypted XOR plaintext

    printf("KS(NR): %08X  (keystream = NR_enc ^ NR)\n", ks_nr);

    printf("\n=== hf mf accel command ===\n");

    // Narrow key range to +-16 of the real key for a quick test
    uint64_t start = (key > 16) ? key - 16 : 0;
    uint64_t end   = (key + 16 < 0xFFFFFFFFFFFFULL) ? key + 16 : 0xFFFFFFFFFFFFULL;

    printf("hf mf accel --uid %08X --nt %08X --nr %08X --ar %08X --chk %08X --start %012llX --end %012llX\n",
           uid, nt, nr, ar, ks_nr,
           (unsigned long long)start, (unsigned long long)end);

    printf("\n=== Full key space test (will take ~10 min on FPGA) ===\n");
    printf("hf mf accel --uid %08X --nt %08X --nr %08X --ar %08X --chk %08X\n",
           uid, nt, nr, ar, ks_nr);

    printf("\nExpected result: Found key: %012llX\n", (unsigned long long)key);

    return 0;
}
