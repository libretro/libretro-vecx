/*
MIT License

Copyright (c) 2020-2022 Rupert Carmichael

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// General Instrument AY-3-8912 Programmable Sound Generator

#include <stddef.h>
#include <stdint.h>

#include "vecx_psg.h"

static const int16_t vtable[16] = { // Volume Table
    0,       40,      60,     86,    124,    186,    264,    440,
    518,    840,    1196,   1526,   2016,   2602,   3300,   4096,
};

static psg_t psg; // PSG Context
static int16_t *psgbuf = NULL; // Buffer for raw PSG output samples
static size_t bufpos = 0; // Keep track of the position in the PSG output buffer

// Reset the Envelope step and volume depending on the currently selected shape
static void vecx_psg_env_reset(void) {
    psg.estep = 0; // Reset the step counter

    if (psg.eseg) { // Segment 1
        switch (psg.reg[13]) {
            case 8: case 11: case 13: case 14: { // Start from the top
                psg.evol = 15;
                break;
            }
            default: { // Start from the bottom
                psg.evol = 0;
                break;
            }
        }
    }
    else { // Segment 0
        // If the Attack bit is set, start from the bottom. Otherwise, the top.
        psg.evol = psg.reg[13] & 0x04 ? 0 : 15;
    }
}


void vecx_psg_set_buffer(int16_t *ptr) {
    psgbuf = ptr;
}

// Grab the pointer to the PSG's buffer
void vecx_psg_reset_buffer(void) {
    bufpos = 0;
}

// Set initial values
void vecx_psg_init(void) {
    int i = 0;

    // Registers
    for (i = 0; i < 16; ++i)
        psg.reg[i] = 0x00;

    // Set the IO Register to all 1s
    psg.reg[14] = 0xff;

    // Latched Register
    psg.rlatch = 0x00;

    // Tone Periods, Tone Counters, Amplitude, Sign bits
    for (i = 0; i < 3; ++i) {
        psg.tperiod[i] = 0x0000;
        psg.tcounter[i] = 0x0000;
        psg.amplitude[i] = 0x00;
        psg.sign[i] = 0x00;
    }

    // Noise Period, Noise Counter
    psg.nperiod = 0x00;
    psg.ncounter = 0x0000;

    // Seed the Noise RNG Shift Register
    psg.nshift = 1;

    // Envelope Period, Counter, Segment, Step, and Volume
    psg.eperiod = 0x0000;
    psg.ecounter = 0x0000;
    psg.eseg = 0x00;
    psg.estep = 0x00;
    psg.evol = 0x00;

    // Enable bits for Tone, Noise, and Envelope
    for (i = 0; i < 3; ++i) {
        psg.tdisable[i] = 0x00;
        psg.ndisable[i] = 0x00;
        psg.emode[i] = 0x00;
    }
}

// Read from the currently latched Control Register
uint8_t vecx_psg_rd(void) {
    return psg.reg[psg.rlatch];
}

// Write to the currently latched Control Register
void vecx_psg_wr(uint8_t data) {
    // Masks to avoid writing "Don't Care" bits
    uint8_t dcmask[16] = {
        0xff, 0x0f, 0xff, 0x0f, 0xff, 0x0f, 0x1f, 0xff,
        0x1f, 0x1f, 0x1f, 0xff, 0xff, 0x0f, 0xff, 0xff,
    };

    // Write data to the latched register
    psg.reg[psg.rlatch] = data & dcmask[psg.rlatch];

    switch (psg.rlatch) {
        case 0: case 1: { // Channel A Tone Period
            psg.tperiod[0] = psg.reg[0] | (psg.reg[1] << 8);
            if (psg.tperiod[0] == 0)
                psg.tperiod[0] = 1;
            break;
        }
        case 2: case 3: { // Channel B Tone Period
            psg.tperiod[1] = psg.reg[2] | (psg.reg[3] << 8);
            if (psg.tperiod[1] == 0)
                psg.tperiod[1] = 1;
            break;
        }
        case 4: case 5: { // Channel C Tone Period
            psg.tperiod[2] = psg.reg[4] | (psg.reg[5] << 8);
            if (psg.tperiod[2] == 0)
                psg.tperiod[2] = 1;
            break;
        }
        case 6: { // Noise Period
            psg.nperiod = psg.reg[6];
            if (psg.nperiod == 0) // As with Tones, lowest period value is 1.
                psg.nperiod = 1;
            break;
        }
        case 7: { // Enable IO/Noise/Tone
            // Register 7's Enable bits are actually Disable bits.
            psg.tdisable[0] = (psg.reg[7] >> 0) & 0x01;
            psg.tdisable[1] = (psg.reg[7] >> 1) & 0x01;
            psg.tdisable[2] = (psg.reg[7] >> 2) & 0x01;
            psg.ndisable[0] = (psg.reg[7] >> 3) & 0x01;
            psg.ndisable[1] = (psg.reg[7] >> 4) & 0x01;
            psg.ndisable[2] = (psg.reg[7] >> 5) & 0x01;
            break;
        }
        case 8: { // Channel A Amplitude
            psg.amplitude[0] = data & 0x0f;
            psg.emode[0] = (data >> 4) & 0x01;
            break;
        }
        case 9: { // Channel B Amplitude
            psg.amplitude[1] = data & 0x0f;
            psg.emode[1] = (data >> 4) & 0x01;
            break;
        }
        case 10: { // Channel C Amplitude
            psg.amplitude[2] = data & 0x0f;
            psg.emode[2] = (data >> 4) & 0x01;
            break;
        }
        case 11: case 12: { // Envelope Period
            psg.eperiod = psg.reg[11] | (psg.reg[12] << 8);
            break;
        }
        case 13: { // Envelope Shape
            // Reset all Envelope related variables when Register 13 is written
            psg.ecounter = 0;
            psg.eseg = 0;
            vecx_psg_env_reset();
            break;
        }
        default: {
            break;
        }
    }
}

// Write to the IO Register
void vecx_psg_io_wr(uint8_t data) {
    psg.reg[14] = data;
}

// Get the latched Control Register
uint8_t vecx_psg_get_reg(void) {
    return psg.rlatch;
}

// Set the latched Control Register
void vecx_psg_set_reg(uint8_t r) {
    psg.rlatch = r;
}

// Execute a PSG cycle
size_t vecx_psg_exec(void) {
    int i = 0;
    int16_t vol = 0; // Initial output volume of this sample

    // Clock Tone Counters for Channels A, B, and C
    for (i = 0; i < 3; ++i) {
        if (++psg.tcounter[i] >= psg.tperiod[i]) {
            psg.tcounter[i] = 0;
            psg.sign[i] ^= 1;
        }
    }

    // Clock Noise Counter
    if (++psg.ncounter >= (psg.nperiod << 1)) {
        psg.ncounter = 0;
        psg.nshift = (psg.nshift >> 1) |
            (((psg.nshift ^ (psg.nshift >> 3)) & 0x01) << 16);
    }

    // Clock Envelope Counter
    if (++psg.ecounter >= (psg.eperiod << 1)) {
        psg.ecounter = 0;

        if (psg.estep) { // Do not change the envelope's volume for the 0th step
            if (psg.eseg) { // Second half of the envelope shape
                if ((psg.reg[13] == 10) || (psg.reg[13] == 12))
                    ++psg.evol; // Count Up
                else if ((psg.reg[13] == 8) || (psg.reg[13] == 14))
                    --psg.evol; // Count Down
                // Otherwise, simply hold the current value (no else statement)
            }
            else { // First half of the envelope shape
                if (psg.reg[13] & 0x04) // Attack is set - Count Up
                    ++psg.evol;
                else // Count Down
                    --psg.evol;
            }
        }

        // Reset and start the new Segment if this is the last Envelope Step
        if (++psg.estep >= 16) {
            if ((psg.reg[13] & 0x09) == 0x08) // Switch Envelope Segment
                psg.eseg ^= 1;
            else // Hold the current Segment for 0-7, 9, 11, 13, 15
                psg.eseg = 1;
            vecx_psg_env_reset();
        }
    }

    for (i = 0; i < 3; ++i) {
        uint8_t out = (psg.tdisable[i] | psg.sign[i]) &
            (psg.ndisable[i] | (psg.nshift & 0x01));

        if (out)
            vol += psg.emode[i] ? vtable[psg.evol] : vtable[psg.amplitude[i]];
    }

    // Add the mixed sample to the output buffer and increment the position
    psgbuf[bufpos++] = vol;

    return 1; // Return 1, signifying that a sample has been generated
}

void vecx_psg_state_load(psg_t *st_psg) {
    psg = *st_psg;
}

void vecx_psg_state_save(psg_t *st_psg) {
    *st_psg = psg;
}
