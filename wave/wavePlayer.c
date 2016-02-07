/*
 * wavePlayer.c
 */

#include "ch.h"
#include "hal.h"

#include "ff.h"
#include "wavePlayer.h"
#include "codec_DAC.h"
#include <string.h>

#define PLAYER_PRIO		(NORMALPRIO+1)
#define HEADERMAX		128
#define DEBUG			FALSE

#if DEBUG
#define CONSOLE	SD1
#include "chprintf.h"
#endif

#define FORMAT_PCM		1
#define RIFF			0x46464952		// 'FIRR' = RIFF in Little-Endian format
#define WAVE			0x45564157		// 'WAVE' in Little-Endian
#define DATA			0x61746164		// 'data' in Little-Endian
#define FMT				0x20746D66		// 'fmt ' in Little-Endian

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
} WAVEHeader;

typedef struct _DATAHeader
{
	chunk       descriptor;
} DATAHeader;

typedef struct _FILEHeader
{
    RIFFHeader  riff;
    WAVEHeader  wave;
} FILEHeader;

extern bool fs_ready;
static dacsample_t dacbuffer[DAC_BUFFER_SIZE];

uint8_t bitsPerSample;
uint16_t sampleRate;
uint32_t bytesToPlay;

thread_t* playerThread;
static FIL file;

static void i16_conv(uint16_t buf[], uint16_t len) {
	for (uint16_t i=0; i<len; i++) {
		buf[i] += 0x8000;
	}
}

static THD_WORKING_AREA(waPlayerThread, 512);
static THD_FUNCTION(wavePlayerThread, arg) {
	(void) arg;

	UINT btr;
	FRESULT err;
	void *pbuffer;

	chRegSetThreadName("player");

	pbuffer = dacbuffer;
	memset(pbuffer, 0, DAC_BUFFER_SIZE * sizeof(dacsample_t));
	err = f_read(&file, pbuffer, DAC_BUFFER_SIZE * sizeof(dacsample_t), &btr);
	if (err != FR_OK) goto end;

	if (bitsPerSample == 16) {
		i16_conv((uint16_t*) pbuffer, DAC_BUFFER_SIZE);
		codec_audio_send(sampleRate, pbuffer, DAC_BUFFER_SIZE);
	} else {
		codec_audio_send(sampleRate, pbuffer, DAC_BUFFER_SIZE*4);	// don't know why
	}

	bytesToPlay -= btr;

	while (bytesToPlay) {
    	if (chThdShouldTerminateX()) break;
		eventmask_t evt = chEvtWaitAny(ALL_EVENTS);

	    if (evt & EVT_DAC_ERR) break;
	    if (evt & EVT_DAC_TC) {
			err = f_read(&file, pbuffer, DAC_BUFFER_SIZE, &btr);
			if (err != FR_OK) break;
			if (btr < DAC_BUFFER_SIZE)
				memset(pbuffer+btr, 0, DAC_BUFFER_SIZE-btr);
			if (bitsPerSample == 16) {
				i16_conv((uint16_t*) pbuffer, DAC_BUFFER_SIZE/2);
			}
			if (pbuffer == dacbuffer)
				pbuffer += DAC_BUFFER_SIZE;
			else
				pbuffer = dacbuffer;
			bytesToPlay -= btr;
		}

	    if (!btr) break;
	}

end:
	codec_stop();
	f_close(&file);

	playerThread = NULL;
	chThdExit((msg_t) 0);
}

void playFile(char* fpath) {
	uint16_t HeaderLength;
	FRESULT err;
	UINT btr;

	if (!fs_ready) {
#if DEBUG
	    chprintf((BaseSequentialStream*) &CONSOLE, "File System not mounted\r\n");
#endif
	    return;
	}

	while (playerThread) {
		stopPlay();
		chThdSleepMilliseconds(100);
	}

	err = f_open(&file, fpath, FA_READ);
	if (err != FR_OK) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Failed to open file %s, error=%d\r\n", fpath, err);
#endif
		return;
	}

	HeaderLength = sizeof(FILEHeader);
	err = f_read(&file, &dacbuffer, HeaderLength, &btr);
	if (err != FR_OK) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error read file %s, error=%d\r\n", fpath, err);
#endif
		return;
	}

	FILEHeader* header = (FILEHeader*) dacbuffer;
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
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: only mono format supported.\r\n");
#endif
		return;
	}

	if (!(header->wave.bitsPerSample == 8 || header->wave.bitsPerSample == 16)) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: %d bits per sample not supported.\r\n", header->wave.bitsPerSample);
#endif
		return;
	}

	sampleRate = header->wave.sampleRate;
	bitsPerSample = header->wave.bitsPerSample;

	DATAHeader* dataheader = (DATAHeader*) &dacbuffer;

	HeaderLength += sizeof(DATAHeader);
	err = f_read(&file, &dacbuffer, sizeof(DATAHeader), &btr);
	if (err != FR_OK) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error read file %s, error=%d\r\n", fpath, err);
#endif
		return;
	}

	while ((uint32_t) dataheader->descriptor.id != DATA) {
		if (HeaderLength > HEADERMAX) break;
		if (dataheader->descriptor.size > 0) {
			HeaderLength += dataheader->descriptor.size;
			f_lseek(&file, HeaderLength);
		}
		HeaderLength += sizeof(DATAHeader);
		err = f_read(&file, &dacbuffer, sizeof(DATAHeader), &btr);
		if (err != FR_OK) {
#if DEBUG
			chprintf((BaseSequentialStream*) &CONSOLE, "Error read file %s, error=%d\r\n", fpath, err);
#endif
			return;
		}
	}

	if ((uint32_t) dataheader->descriptor.id != DATA) {
#if DEBUG
		chprintf((BaseSequentialStream*) &CONSOLE, "Error: %s data chunk not found\r\n", fpath);
#endif
		return;
	}

#if DEBUG
	chprintf((BaseSequentialStream*) &CONSOLE, "OK, ready to play.\r\n");
#endif

	bytesToPlay = dataheader->descriptor.size;
#if DEBUG
	chprintf((BaseSequentialStream*) &CONSOLE, "Sample Length:%ld bytes\r\n", bytesToPlay);
#endif

	codec_init(bitsPerSample);
	playerThread = chThdCreateStatic(waPlayerThread, sizeof(waPlayerThread), PLAYER_PRIO, wavePlayerThread, NULL);
}

void stopPlay(void) {
	if (playerThread) {
		chThdTerminate(playerThread);
		chThdWait(playerThread);
		if (chThdTerminatedX(playerThread))	playerThread=NULL;
	}
}
