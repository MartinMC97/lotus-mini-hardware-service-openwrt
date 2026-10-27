// SPDX-License-Identifier: GPL-3.0-only
// Generates test PCM, never opens a sound device. 48kHz/stereo/S16_LE.
#include <cmath>
#include <cstdint>
#include <cstdio>
int main() {
    // 1s warm-up silence, 2s low-amplitude tone, 1s trailing silence.
    for (unsigned i=0;i<192000;++i) {
        double gain=0;
        if (i>=48000 && i<144000) {
            gain=0.08;
            if (i<48960) gain*=static_cast<double>(i-48000)/960;
            if (i>=143040) gain*=static_cast<double>(143999-i)/960;
        }
        const auto sample=static_cast<int16_t>(std::lround(32767*gain*
            std::sin(6.283185307179586*440*i/48000)));
        const uint16_t bits=static_cast<uint16_t>(sample);
        for (unsigned channel=0;channel<2;++channel) {
            if (std::putchar(bits&255)==EOF || std::putchar(bits>>8)==EOF) return 1;
        }
    }
    return std::fflush(stdout)==0?0:1;
}
