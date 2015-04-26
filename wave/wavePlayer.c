/*
 * wavePlayer.c
 */

#include "ch.h"
#include "hal.h"

#include "ff.h"
#include "chprintf.h"
#include "wavePlayer.h"
#include "codec_DAC.h"
#include <string.h>
#include "drivers.h"

#define DEBUG	1
#if DEBUG
#define CONSOLE	SD2
#endif

#define FORMAT_PCM		1
#define RIFF			0x46464952		// 'FIRR' = RIFF in Little-Endian format
#define WAVE			0x45564157		// 'WAVE' in Little-Endian
#define DATA			0x61746164		// 'data' in Little-Endian
#define FMT				0x20746D66		// 'fmt ' in Little-Endian
#define EXTRA_LEN		2				// length of extraBytes field

typedef struct _chunk
{
	uint32_t    id;
	uint32_t    size;
 } chunk;

typedef struct _RIFFHeader
{
	 chunk       descriptor;  // "RIFF"
	 uint32_t    type;        // "WAVE"
} RIFFHeader;

typedef struct _WAVEHeader
{
     chunk       descriptor;
     uint16_t    audioFormat;	// 1 - PCM
     uint16_t    numChannels;	// 1 - mono
     uint32_t    sampleRate;	//
     uint32_t    byteRate;		// byteRate = SampleRate * BlockAlign
     uint16_t    blockAlign;	// BlockAlign = bitsPerSample / 8 * NumChannels
     uint16_t    bitsPerSample;
     // in some case there may be uint16_t extraBytes
} WAVEHeader;

typedef struct _DATAHeader
{
		chunk       descriptor;
} DATAHeader;

typedef struct _FILEHeader
{
     RIFFHeader  riff;
     WAVEHeader  wave;
     DATAHeader  data;
} FILEHeader;

static uint8_t HeaderLength = sizeof(FILEHeader);

EventSource player_evsrc;
Thread* playerThread;

static uint8_t bitsPerSample;
static uint32_t bytesToPlay;
static uint8_t buffer[DAC_BUFFER_SIZE] = {0};

static FIL f;

static void i16_conv(uint16_t *buffer, uint16_t len) {
	static uint16_t i;
	for (i=0; i<len/2; i++) {
		buffer[i] += 0x8000;
	}
}

static WORKING_AREA(waPlayerThread, 256);
static msg_t wavePlayerThread(void *arg) {
	(void) arg;
	chRegSetThreadName("playerThd");

	static UINT btr;
	static uint8_t err;
	struct EventListener player_el;
	static void *pbuffer;

	pbuffer = buffer;
	memset(pbuffer, 0, DAC_BUFFER_SIZE);
	err = f_read(&f, pbuffer, DAC_BUFFER_SIZE, &btr);
	if (err) return 1;

	chEvtInit(&player_evsrc);
	chEvtRegister(&player_evsrc, &player_el, 0);

	if (bitsPerSample == 16) {
		i16_conv((uint16_t*)pbuffer, DAC_BUFFER_SIZE);
		codec_audio_send(pbuffer, DAC_BUFFER_SIZE/2);
	} else {
		codec_audio_send(pbuffer, DAC_BUFFER_SIZE);
	}
	bytesToPlay -= btr;
	while (bytesToPlay) {
		chEvtWaitAny(EVENT_MASK(0));

		chSysLock();
		flagsmask_t flags = chEvtGetAndClearFlagsI(&player_el);
		chSysUnlock();

		if (flags & DAC_CB) {
			memset(pbuffer, 0, DAC_BUFFER_SIZE/2);
			err = f_read(&f, pbuffer, DAC_BUFFER_SIZE/2, &btr);
			if (bitsPerSample == 16) {
				i16_conv(pbuffer, DAC_BUFFER_SIZE/2);
			}
			if (err) break;
			if (pbuffer == &buffer[0])
				pbuffer += DAC_BUFFER_SIZE/2;
			else
				pbuffer = buffer;
			bytesToPlay -= btr;
		}
		if (flags & DAC_ERR) break;
		if (chThdShouldTerminate())	break;
	}

	codec_stop();
	chEvtUnregister(&player_evsrc, &player_el);
	playerThread = NULL;
	f_close(&f);
	return 0;
}

void playFile(char* fpath) {

	static uint8_t err;
	static UINT btr;

	while (playerThread) {
		stopPlay();
		chThdSleepMilliseconds(100);
	}

	err = f_open(&f, fpath, FA_READ);
	if (err) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Failed to open file %s, error=%d\r\n", fpath, err);
#endif
		return;
	}

	err = f_read(&f, &buffer[0], HeaderLength+EXTRA_LEN, &btr);
	if (err) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error read file %s, error=%d\r\n", fpath, err);
#endif
		return;
	}

	static FILEHeader* header;
	header = (FILEHeader*)buffer;
    if (!(header->riff.descriptor.id == RIFF
        && header->riff.type == WAVE
        && header->wave.descriptor.id == FMT
        && header->wave.audioFormat == FORMAT_PCM)) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: this is not PCM file!\r\n");
#endif
		return;
	}

#if DEBUG
	chprintf((BaseSequentialStream*) &CONSOLE,
		"Number of channels=%d\r\nSample Rate=%ld\r\nNumber of Bits=%d\r\n",
		header->wave.numChannels, header->wave.sampleRate, header->wave.bitsPerSample);
#endif

	if (header->wave.numChannels > 1) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: only mono audio supported.\r\n");
#endif
		return;
	}

	if (!(header->wave.bitsPerSample == 8 || header->wave.bitsPerSample == 16)) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: %d bits per sample not supported.\r\n", header->wave.bitsPerSample);
#endif
		return;
	}

	if (header->wave.sampleRate > 44100) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: sample rate %d not supported.\r\n", header->wave.sampleRate);
#endif
		return;
	}

	bitsPerSample = header->wave.bitsPerSample;
	if ((uint32_t) header->data.descriptor.id == DATA)	{
		bytesToPlay = header->data.descriptor.size;
		goto play;
	}
	else {
			static DATAHeader* dataheader;
			dataheader = (DATAHeader*) &buffer[sizeof(RIFFHeader)+sizeof(WAVEHeader)+EXTRA_LEN];
			if ((uint32_t) dataheader->descriptor.id == DATA)	{
				bytesToPlay = dataheader->descriptor.size;
				HeaderLength += EXTRA_LEN;
				goto play;
			}
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: %s data chunk not found\r\n", fpath);
#endif
		return;
	}

play:

#if DEBUG
	chprintf((BaseSequentialStream*) &CONSOLE, "OK, ready to play.\r\n");
#endif

	codec_init(header->wave.sampleRate, bitsPerSample);

#if DEBUG
	chprintf((BaseSequentialStream*) &CONSOLE, "Sample Length:%ld bytes\r\n", bytesToPlay);
#endif

	err = f_lseek(&f, HeaderLength);
	if (err) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error set file position %s, error=%d\r\n", fpath, err);
#endif
		return;
	}

	playerThread = chThdCreateStatic(waPlayerThread, sizeof(waPlayerThread), PLAYER_PRIO, wavePlayerThread, NULL);
}

void stopPlay(void) {
	if (playerThread) {
		chThdTerminate(playerThread);
		chThdWait(playerThread);
		playerThread=NULL;
	}
}
