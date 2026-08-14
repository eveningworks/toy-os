// See fixed.h. The sine table and the lookup around it.
#include "fixed.h"

// A QUARTER turn of sine, in Q16.16, plus one extra entry so the
// interpolation below always has a right-hand neighbour without
// wrapping. The other three quadrants are this one mirrored -- storing
// them would be three copies of the same numbers and three chances for
// one of them to be wrong.
//
// Generated, not hand-typed: sin(i / (4*256) * 2pi) * 65536.
#define FX_QUARTER_STEPS 256

static const int32_t SIN_Q[FX_QUARTER_STEPS + 1] = {
    0, 402, 804, 1206, 1608, 2010, 2412, 2814,
    3216, 3617, 4019, 4420, 4821, 5222, 5623, 6023,
    6424, 6824, 7224, 7623, 8022, 8421, 8820, 9218,
    9616, 10014, 10411, 10808, 11204, 11600, 11996, 12391,
    12785, 13180, 13573, 13966, 14359, 14751, 15143, 15534,
    15924, 16314, 16703, 17091, 17479, 17867, 18253, 18639,
    19024, 19409, 19792, 20175, 20557, 20939, 21320, 21699,
    22078, 22457, 22834, 23210, 23586, 23961, 24335, 24708,
    25080, 25451, 25821, 26190, 26558, 26925, 27291, 27656,
    28020, 28383, 28745, 29106, 29466, 29824, 30182, 30538,
    30893, 31248, 31600, 31952, 32303, 32652, 33000, 33347,
    33692, 34037, 34380, 34721, 35062, 35401, 35738, 36075,
    36410, 36744, 37076, 37407, 37736, 38064, 38391, 38716,
    39040, 39362, 39683, 40002, 40320, 40636, 40951, 41264,
    41576, 41886, 42194, 42501, 42806, 43110, 43412, 43713,
    44011, 44308, 44604, 44898, 45190, 45480, 45769, 46056,
    46341, 46624, 46906, 47186, 47464, 47741, 48015, 48288,
    48559, 48828, 49095, 49361, 49624, 49886, 50146, 50404,
    50660, 50914, 51166, 51417, 51665, 51911, 52156, 52398,
    52639, 52878, 53114, 53349, 53581, 53812, 54040, 54267,
    54491, 54714, 54934, 55152, 55368, 55582, 55794, 56004,
    56212, 56418, 56621, 56823, 57022, 57219, 57414, 57607,
    57798, 57986, 58172, 58356, 58538, 58718, 58896, 59071,
    59244, 59415, 59583, 59750, 59914, 60075, 60235, 60392,
    60547, 60700, 60851, 60999, 61145, 61288, 61429, 61568,
    61705, 61839, 61971, 62101, 62228, 62353, 62476, 62596,
    62714, 62830, 62943, 63054, 63162, 63268, 63372, 63473,
    63572, 63668, 63763, 63854, 63944, 64031, 64115, 64197,
    64277, 64354, 64429, 64501, 64571, 64639, 64704, 64766,
    64827, 64884, 64940, 64993, 65043, 65091, 65137, 65180,
    65220, 65259, 65294, 65328, 65358, 65387, 65413, 65436,
    65457, 65476, 65492, 65505, 65516, 65525, 65531, 65535,
    65536,
};

// Sine of an index in [0, 4*FX_QUARTER_STEPS), i.e. a full turn split
// into 1024 steps, with linear interpolation between table entries.
// `frac` is the sub-step position in Q16.16.
static fx_t sin_step(int step, fx_t frac) {
    // Fold the full turn onto the stored quarter. Mirror rules:
    //   quadrant 0:  sin(x)        = table[i]
    //   quadrant 1:  sin(pi/2 + x) = table[N - i]
    //   quadrant 2:  sin(pi + x)   = -table[i]
    //   quadrant 3:  sin(3pi/2 +x) = -table[N - i]
    int quadrant = (step / FX_QUARTER_STEPS) & 3;
    int i = step % FX_QUARTER_STEPS;

    int a_idx, b_idx, sign;
    if (quadrant == 0)      { a_idx = i;                    b_idx = i + 1;                    sign = 1; }
    else if (quadrant == 1) { a_idx = FX_QUARTER_STEPS - i;  b_idx = FX_QUARTER_STEPS - i - 1;  sign = 1; }
    else if (quadrant == 2) { a_idx = i;                    b_idx = i + 1;                    sign = -1; }
    else                    { a_idx = FX_QUARTER_STEPS - i;  b_idx = FX_QUARTER_STEPS - i - 1;  sign = -1; }

    int32_t a = SIN_Q[a_idx];
    int32_t b = SIN_Q[b_idx];
    int32_t v = a + (int32_t)(((int64_t)(b - a) * frac) >> FX_SHIFT);
    return (fx_t)(sign * v);
}

fx_t fx_sin(fx_t turns) {
    // Wrap by masking, which is why angles are in turns: a rotation
    // counter can increase forever and this stays exact.
    uint32_t t = (uint32_t)turns & (FX_ONE - 1);

    // Split the turn into 1024 steps plus a fractional remainder.
    uint32_t scaled = t * (4 * FX_QUARTER_STEPS);   // fits: t < 65536, 1024 * 65536 < 2^26
    int step = (int)(scaled >> FX_SHIFT);
    fx_t frac = (fx_t)(scaled & (FX_ONE - 1));
    return sin_step(step, frac);
}

fx_t fx_cos(fx_t turns) {
    // cos(x) = sin(x + quarter turn). One table, one lookup path.
    return fx_sin(turns + FX_ONE / 4);
}
