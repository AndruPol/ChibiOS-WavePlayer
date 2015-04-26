/*
 * codec_DAC.h
 *
 *  Created on: Jun 5, 2013
 *      Author: Andrey Polyakov
 */

#ifndef CODEC_H_
#define CODEC_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "ch.h"
#include "hal.h"

#define DAC_CB				(1<<1)	// DAC callback event flag
#define DAC_ERR				(1<<2)	// DAC error event flag
#define DAC_BUFFER_SIZE 	1024

extern void codec_init(uint32_t sampleRate, uint8_t numBits);
extern void codec_stop(void);
extern void codec_audio_send(void* txbuf, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* CODEC_H_ */
