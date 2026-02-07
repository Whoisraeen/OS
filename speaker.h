#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>

// Play a tone at the given frequency (Hz) for the given duration (ms)
void speaker_beep(uint32_t frequency, uint32_t duration_ms);

// Play a short click sound
void speaker_click(void);

// Play an error beep
void speaker_error(void);

// Play a success sound
void speaker_success(void);

#endif
