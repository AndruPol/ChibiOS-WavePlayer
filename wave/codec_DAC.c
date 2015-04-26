/*
 * codec_DAC.c
 *
 *  Created on: Jun 5, 2013
 *      Author: Andrey Polyakov
 */

#include "ch.h"
#include "hal.h"

#include "codec_DAC.h"
#include "drivers.h"

#define DAC_GPIO		GPIOA
#define DAC_PIN			GPIOA_PIN4

extern EventSource player_evsrc;

/*
 * DMA end of transmission callback.
 */
static void daccb(DACDriver *dacp, const dacsample_t *samples, size_t pos) {
	(void)dacp;
	(void)samples;
	(void)pos;
	chSysLockFromIsr();
	chEvtBroadcastFlagsI(&player_evsrc,	DAC_CB);
	chSysUnlockFromIsr();
}

/*
 * DMA errors callback.
 */
static void dacerrcb(DACDriver *dacp, uint32_t flags) {
	(void)dacp;
	(void)flags;
	chSysLockFromIsr();
	chEvtBroadcastFlagsI(&player_evsrc, DAC_ERR);
	chSysUnlockFromIsr();
}

/*
 * DAC config, with callbacks.
 */
static DACConfig daccfg = {
	DAC_DHRM_8BIT_RIGHT,
	0
};

static DACConversionGroup dacconvgrp = {
	  0,	 	/* Timer frequency in Hz */
	  1, 		/* Number of DAC channels */
	  daccb, 	/* End of transfer callback */
	  dacerrcb, /* Error callback */
	  TRUE		/* Circular buffer */
};


void codec_init(uint32_t sampleRate, uint8_t numBits) {
	palSetPadMode(DAC_GPIO, DAC_PIN, PAL_MODE_INPUT_ANALOG);
	daccfg.dhrm = DAC_DHRM_8BIT_RIGHT;
	if (numBits == 16) {
		daccfg.dhrm = DAC_DHRM_12BIT_LEFT;
	}
	dacconvgrp.frequency = sampleRate;
	dacStart(&DACD1, &daccfg);
}

void codec_stop(void) {
	dacStopConversion(&DACD1);
	dacStop(&DACD1);
}

// Send data to the codec
void codec_audio_send(void* txbuf, size_t n) {
	dacStartConversion(&DACD1, &dacconvgrp, txbuf, n);
}
