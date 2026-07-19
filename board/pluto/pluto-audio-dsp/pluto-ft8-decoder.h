#ifndef PLUTO_FT8_DECODER_H
#define PLUTO_FT8_DECODER_H

#include <stdio.h>

struct pluto_ft8_decoder;

struct pluto_ft8_decoder *pluto_ft8_decoder_create(unsigned sample_rate);
void pluto_ft8_decoder_destroy(struct pluto_ft8_decoder *decoder);
void pluto_ft8_decoder_add_audio(struct pluto_ft8_decoder *decoder, float sample);
void pluto_ft8_decoder_write_json(struct pluto_ft8_decoder *decoder, FILE *out);

#endif
