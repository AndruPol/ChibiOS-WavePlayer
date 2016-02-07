/*
 * wavePlayer.h
 */

#ifndef WAVEPLAYER_H_
#define WAVEPLAYER_H_

#include "ch.h"

#ifdef __cplusplus
extern "C" {
#endif

extern thread_t* playerThread;

void playFile(char* fpath);
void stopPlay(void);

#ifdef __cplusplus
}
#endif
#endif /* WAVEPLAYER_H_ */
