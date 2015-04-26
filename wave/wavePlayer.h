/*
 * wavePlayer.h
 */

#ifndef WAVEPLAYER_H_
#define WAVEPLAYER_H_
#ifdef __cplusplus
extern "C" {
#endif

#include "ch.h"
#include "hal.h"

#define PLAYER_PRIO		(NORMALPRIO+1)
extern Thread* playerThread;
extern EventSource player_evsrc;

void playFile(char* fpath);
void stopPlay(void);

#ifdef __cplusplus
}
#endif
#endif /* WAVEPLAYER_H_ */
