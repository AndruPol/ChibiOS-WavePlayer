/*
 * codec_DAC.h
 *
 *  Created on: Jun 5, 2013
 *      Author: Andrey Polyakov
 */

#ifndef CODEC_H_
#define CODEC_H_

#define EVT_DAC_TC			(1<<0)	// DAC half/full transmission complete
#define EVT_DAC_ERR			(1<<1)	// DAC error
#define DAC_BUFFER_SIZE 	1024	// size = sizeof(dacsample_t) * DAC_BUFFER_SIZE

#ifdef __cplusplus
extern "C" {
#endif

void codec_init(uint8_t numBits);
void codec_stop(void);
void codec_audio_send(uint16_t sampleRate, dacsample_t *txbuf, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* CODEC_H_ */
