/*
 * codec_DAC.c
 *
 *  Created on: Jun 5, 2013
 *      Author: Andrey Polyakov
 */

#include "ch.h"
#include "hal.h"

#include "codec_DAC.h"

#define DACDRIVER			DACD1
#define DACTIMER			GPTD6
#define DAC_GPIO			GPIOA
#define DAC_PIN				GPIOA_PIN4

#define SOUND_EN			FALSE		// use sound control pin

#if defined(SOUND_EN)
#define SNDEN_GPIO			GPIOC
#define SNDEN_PIN			GPIOC_PIN13
#define SOUNDON				palSetPad(SNDEN_GPIO, SNDEN_PIN)
#define SOUNDOFF			palClearPad(SNDEN_GPIO, SNDEN_PIN)
#endif

extern thread_t *playerThread;

/*
 * DMA end of transmission callback.
 */
static void daccb(DACDriver *dacp, const dacsample_t * samples, size_t pos) {
	(void)dacp;
	(void)samples;
	(void)pos;
	if (playerThread) {
		chSysLockFromISR();
		chEvtSignalI(playerThread, EVT_DAC_TC);
		chSysUnlockFromISR();
	}
}

/*
 * DMA errors callback.
 */
static void dacerrcb(DACDriver *dacp, dacerror_t err) {
	(void)dacp;
	(void)err;
	if (playerThread) {
		chSysLockFromISR();
		chEvtSignalI(playerThread, EVT_DAC_ERR);
		chSysUnlockFromISR();
	}
}

/*
 * DAC config
 */
static DACConfig daccfg = {
	init: 		0,
	datamode: 	DAC_DHRM_8BIT_RIGHT,
};

static const DACConversionGroup dacconvgrp = {
	num_channels:	1, 			/* Number of DAC channels */
	end_cb:			daccb, 		/* End of transfer callback */
	error_cb:		dacerrcb, 	/* Error callback */
	trigger:		DAC_TRG(0),	/* */
};

/*
 * GPT6 configuration
 */
static const GPTConfig gptcfg = {
	frequency:	32000000,		/* */
	callback:	NULL,			/* */
	cr2:		TIM_CR2_MMS_1,	/* MMS = 010 = TRGO on update event */
	dier:		0U,
};

void codec_init(uint8_t numBits) {
	daccfg.datamode = DAC_DHRM_8BIT_RIGHT;
	if (numBits == 16) {
		daccfg.datamode = DAC_DHRM_12BIT_LEFT;
	}

	palSetPadMode(DAC_GPIO, DAC_PIN, PAL_MODE_INPUT_ANALOG);
	dacStart(&DACDRIVER, &daccfg);
	gptStart(&DACTIMER, &gptcfg);
}

void codec_stop(void) {
	gptStopTimer(&DACTIMER);
	gptStop(&DACTIMER);
	dacStopConversion(&DACDRIVER);
	dacStop(&DACDRIVER);
#if defined(SOUND_EN)
	SOUNDOFF;
#endif
}

// Send data to codec
void codec_audio_send(uint16_t sampleRate, dacsample_t *txbuf, size_t n) {
#if defined(SOUND_EN)
	SOUNDON;
#endif
	dacStartConversion(&DACDRIVER, &dacconvgrp, txbuf, n);
	gptcnt_t cnt = DACTIMER.clock/sampleRate;
	gptStartContinuous(&DACTIMER, cnt);
}
