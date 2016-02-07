/*
    ChibiOS/RT - Copyright (C) 2006-2013 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include "ch.h"
#include "hal.h"

#include "ff.h"
#include "shell.h"
#include "chprintf.h"
#include "wave/wavePlayer.h"

#include <stdio.h>
#include <string.h>

#define CONSOLE			SD1

#if defined(STM32F1XX_HD)
#define LEDGPIO			GPIOC
#define LEDPIN			GPIOC_LED
#endif
#if defined(STM32L1XX_MD)
#define LEDGPIO			GPIOA
#define LEDPIN			GPIOA_PIN15
#endif

/*===========================================================================*/
/* MMC_SPI driver related settings.                                          */
/*===========================================================================*/

/**
 * @brief   Delays insertions.
 * @details If enabled this options inserts delays into the MMC waiting
 *          routines releasing some extra CPU time for the threads with
 *          lower priority, this may slow down the driver a bit however.
 *          This option is recommended also if the SPI driver does not
 *          use a DMA channel and heavily loads the CPU.
 */
#if !defined(MMC_NICE_WAITING) || defined(__DOXYGEN__)
#define MMC_NICE_WAITING            TRUE
#endif

/**
 * @brief   Number of positive insertion queries before generating the
 *          insertion event.
 */
#if !defined(MMC_POLLING_INTERVAL) || defined(__DOXYGEN__)
#define MMC_POLLING_INTERVAL        10
#endif

/**
 * @brief   Interval, in milliseconds, between insertion queries.
 */
#if !defined(MMC_POLLING_DELAY) || defined(__DOXYGEN__)
#define MMC_POLLING_DELAY           100
#endif

/**
 * @brief   Block size for MMC transfers.
 */
#if !defined(MMC_SECTOR_SIZE) || defined(__DOXYGEN__)
#define MMC_SECTOR_SIZE             512
#endif

/**
 * @brief   Card monitor timer.
 */
static virtual_timer_t tmr;

/**
 * @brief   Debounce counter.
 */
static unsigned cnt;

/**
 * @brief   Card event sources.
 */
static EVENTSOURCE_DECL(inserted_event);
static EVENTSOURCE_DECL(removed_event);

/**
 * @brief   Insertion monitor timer callback function.
 *
 * @param[in] p         pointer to the @p BaseBlockDevice object
 *
 * @notapi
 */
static void tmrfunc(void *p) {
  BaseBlockDevice *bbdp = p;

  /* The presence check is performed only while the driver is not in a
     transfer state because it is often performed by changing the mode of
     the pin connected to the CS/D3 contact of the card, this could disturb
     the transfer.*/
  blkstate_t state = blkGetDriverState(bbdp);
  chSysLockFromISR();
  if ((state != BLK_READING) && (state != BLK_WRITING)) {
    /* Safe to perform the check.*/
    if (cnt > 0) {
      if (blkIsInserted(bbdp)) {
        if (--cnt == 0) {
          chEvtBroadcastI(&inserted_event);
        }
      }
      else
        cnt = MMC_POLLING_INTERVAL;
    }
    else {
      if (!blkIsInserted(bbdp)) {
        cnt = MMC_POLLING_INTERVAL;
        chEvtBroadcastI(&removed_event);
      }
    }
  }
  chVTSetI(&tmr, MS2ST(MMC_POLLING_DELAY), tmrfunc, bbdp);
  chSysUnlockFromISR();
}

/**
 * @brief   Polling monitor start.
 *
 * @param[in] p         pointer to an object implementing @p BaseBlockDevice
 *
 * @notapi
 */
static void tmr_init(void *p) {
  chSysLock();
  cnt = MMC_POLLING_INTERVAL;
  chVTSetI(&tmr, MS2ST(MMC_POLLING_DELAY), tmrfunc, p);
  chSysUnlock();
}

/*===========================================================================*/
/* FatFs related.                                                            */
/*===========================================================================*/

/**
 * @brief FS object.
 */
FATFS MMC_FS;

/**
 * MMC driver instance.
 */
MMCDriver MMCD1;

/* FS mounted and ready.*/
bool fs_ready = FALSE;

static const SPIConfig hs_spicfg = {NULL, GPIOB, GPIOB_PIN12, 0};
static const SPIConfig ls_spicfg = {NULL, GPIOB, GPIOB_PIN12, SPI_CR1_BR_2 | SPI_CR1_BR_1};

/* MMC/SD over SPI driver configuration.*/
static const MMCConfig mmccfg = {&SPID2, &ls_spicfg, &hs_spicfg};

/* Generic large buffer.*/
uint8_t fbuff[256];

static FRESULT scan_files(BaseSequentialStream *chp, char *path) {
  FRESULT res;
  FILINFO fno;
  DIR dir;
  int i;
  char *fn;

#if _USE_LFN
  fno.lfname = 0;
  fno.lfsize = 0;
#endif
  res = f_opendir(&dir, path);
  if (res == FR_OK) {
    i = strlen(path);
    for (;;) {
      res = f_readdir(&dir, &fno);
      if (res != FR_OK || fno.fname[0] == 0)
        break;
      if (fno.fname[0] == '.')
        continue;
      fn = fno.fname;
      if (fno.fattrib & AM_DIR) {
        path[i++] = '/';
        strcpy(&path[i], fn);
        res = scan_files(chp, path);
        if (res != FR_OK)
          break;
        path[--i] = 0;
      }
      else {
        chprintf(chp, "%s/%s\r\n", path, fn);
      }
    }
  }
  return res;
}

/*===========================================================================*/
/* Command line related.                                                     */
/*===========================================================================*/
#define SHELL_WA_SIZE   THD_WORKING_AREA_SIZE(2048)

static void cmd_mem(BaseSequentialStream *chp, int argc, char *argv[]) {
  size_t n, size;

  (void)argv;
  if (argc > 0) {
    chprintf(chp, "Usage: mem\r\n");
    return;
  }
  n = chHeapStatus(NULL, &size);
  chprintf(chp, "core free memory : %u bytes\r\n", chCoreGetStatusX());
  chprintf(chp, "heap fragments   : %u\r\n", n);
  chprintf(chp, "heap free total  : %u bytes\r\n", size);
}

static void cmd_threads(BaseSequentialStream *chp, int argc, char *argv[]) {
  static const char *states[] = {CH_STATE_NAMES};
  thread_t *tp;

  (void)argv;
  if (argc > 0) {
    chprintf(chp, "Usage: threads\r\n");
    return;
  }
  chprintf(chp, "    addr    stack prio refs     state time\r\n");
  tp = chRegFirstThread();
  do {
    chprintf(chp, "%08lx %08lx %4lu %4lu %9s\r\n",
            (uint32_t)tp, (uint32_t)tp->p_ctx.r13,
            (uint32_t)tp->p_prio, (uint32_t)(tp->p_refs - 1),
            states[tp->p_state]);
    tp = chRegNextThread(tp);
  } while (tp != NULL);
}

static void cmd_tree(BaseSequentialStream *chp, int argc, char *argv[]) {
  (void)argv;
  if (argc > 0) {
    chprintf(chp, "Usage: tree\r\n");
    return;
  }
  if (!fs_ready) {
    chprintf(chp, "File System not mounted\r\n");
    return;
  }
#if 0
  err = f_getfree("/", &clusters, &fsp);
  if (err != FR_OK) {
    chprintf(chp, "FS: f_getfree() failed\r\n");
    return;
  }
  chprintf(chp,
           "FS: %lu free clusters, %lu sectors per cluster, %lu bytes free\r\n",
           clusters, (uint32_t)MMC_FS.csize,
           clusters * (uint32_t)MMC_FS.csize * (uint32_t)MMC_SECTOR_SIZE);
#endif
  fbuff[0] = 0;
  scan_files(chp, (char *)fbuff);
}

static void cmd_play(BaseSequentialStream *chp, int argc, char *argv[]) {

  (void)argv;
  if (argc == 0) {
    chprintf(chp, "Usage: play filename\r\n");
    return;
  }
  if (!fs_ready) {
    chprintf(chp, "File System not mounted\r\n");
    return;
  }
  if (argc == 1){
      size_t unLen = strlen(argv[0]);
      if(strcmp(argv[0] + unLen - 4, ".wav") == 0){
    	playFile(argv[0]);
      }
  }
}

static const ShellCommand commands[] = {
  {"mem", cmd_mem},
  {"threads", cmd_threads},
  {"tree", cmd_tree},
  {"play", cmd_play},
  {NULL, NULL}
};

static const ShellConfig shell_cfg = {
  (BaseSequentialStream *)&CONSOLE,
  commands
};

/*
 * Red LEDs blinker thread, times are in milliseconds.
 */
static THD_WORKING_AREA(waledThread, 64);
static THD_FUNCTION(ledThread, arg) {
  (void)arg;
  chRegSetThreadName("blinker");
  palSetPadMode(LEDGPIO, LEDPIN, PAL_MODE_OUTPUT_PUSHPULL);

  while (TRUE) {
    palTogglePad(LEDGPIO, LEDPIN);
    chThdSleepMilliseconds(500);
  }
}

/*
 * MMC card insertion event.
 */
static void InsertHandler(eventid_t id) {
  FRESULT err;
  char path = 0;

  (void)id;
  /*
   * On insertion MMC initialization and FS mount.
   */
  if (mmcConnect(&MMCD1)) {
    return;
  }
  err = f_mount(&MMC_FS, &path, 1);
  if (err != FR_OK) {
    mmcDisconnect(&MMCD1);
    return;
  }
  fs_ready = TRUE;
}

/*
 * MMC card removal event.
 */
static void RemoveHandler(eventid_t id) {

  (void)id;
  mmcDisconnect(&MMCD1);
  fs_ready = FALSE;
}

/*
 * Application entry point.
 */
int main(void) {
	evhandler_t evhndl[] = {
			  InsertHandler,
			  RemoveHandler
	};

	struct event_listener el0, el1;
	thread_t *shelltp = NULL;

   /*
   * System initializations.
   * - HAL initialization, this also initializes the configured device drivers
   *   and performs the board-specific initializations.
   * - Kernel initialization, the main() function becomes a thread and the
   *   RTOS is active.
   */
  halInit();
  chSysInit();

  /*
   * Activates the serial driver 1 using the driver default configuration.
   */
  sdStart(&CONSOLE, NULL);
#if defined(STM32F1XX_HD)
  palSetPadMode(GPIOA, GPIOA_PIN9, PAL_MODE_STM32_ALTERNATE_PUSHPULL);
  palSetPadMode(GPIOA, GPIOA_PIN10, PAL_MODE_INPUT);
#endif
#if defined(STM32L1XX_MD)
  palSetPadMode(GPIOA, GPIOA_PIN9, PAL_MODE_ALTERNATE(7));
  palSetPadMode(GPIOA, GPIOA_PIN10, PAL_MODE_ALTERNATE(7));
#endif
  /*
   * Initializes the MMC driver to work with SPI2.
   */
  // setup pads to SPI2 function (connect these pads to your SD card accordingly)
#if defined(STM32F1XX_HD)
  palSetPadMode(GPIOB, GPIOB_PIN12, PAL_MODE_OUTPUT_PUSHPULL); // CS
  palSetPadMode(GPIOB, GPIOB_PIN13, PAL_MODE_STM32_ALTERNATE_PUSHPULL); // SCK
  palSetPadMode(GPIOB, GPIOB_PIN14, PAL_MODE_STM32_ALTERNATE_PUSHPULL); // MISO
  palSetPadMode(GPIOB, GPIOB_PIN15, PAL_MODE_STM32_ALTERNATE_PUSHPULL); // MOSI
#endif
#if defined(STM32L1XX_MD)
  palSetPadMode(GPIOB, GPIOB_PIN12, PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST); // CS
  palSetPadMode(GPIOB, GPIOB_PIN13, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_HIGHEST); // SCK
  palSetPadMode(GPIOB, GPIOB_PIN14, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_HIGHEST); // MISO
  palSetPadMode(GPIOB, GPIOB_PIN15, PAL_MODE_ALTERNATE(5) | PAL_STM32_OSPEED_HIGHEST); // MOSI
#endif
  palSetPad(GPIOB, GPIOB_PIN12); // set CS high

  mmcObjectInit(&MMCD1);
  mmcStart(&MMCD1, &mmccfg);

  /*
   * Activates the card insertion monitor.
   */
  tmr_init(&MMCD1);

  /*
   * Creates the blinker thread.
   */
  chThdCreateStatic(waledThread, sizeof(waledThread), NORMALPRIO, ledThread, NULL);

  /*
   * Shell manager initialization.
   */
  shellInit();
  shelltp = shellCreate(&shell_cfg, SHELL_WA_SIZE, NORMALPRIO);
  /*
   * Normal main() thread activity, in this demo it does nothing except
   * sleeping in a loop and listen for events.
   */
  chEvtRegister(&inserted_event, &el0, 0);
  chEvtRegister(&removed_event, &el1, 1);
  while (TRUE) {
    if (!shelltp)
      shelltp = shellCreate(&shell_cfg, SHELL_WA_SIZE, NORMALPRIO);
    else if (chThdTerminatedX(shelltp)) {
      chThdRelease(shelltp);    /* Recovers memory of the previous shell.   */
      shelltp = NULL;           /* Triggers spawning of a new shell.        */
    }
    chEvtDispatch(evhndl, chEvtWaitOneTimeout(ALL_EVENTS, MS2ST(500)));
  }
}
