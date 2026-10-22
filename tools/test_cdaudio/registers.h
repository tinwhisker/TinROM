/* tinrom - (C) 2026 TinWhisker - MIT licensed
 *
 * Stands in for ps1-bare-metal's ps1/registers.h in tools/test_cdaudio: the
 * real header for every name and flag, then the CD-ROM, SPU and interrupt
 * registers that src/hal/cdrom.c and spu.c touch, redirected to a fake
 * drive (test_cdaudio.c).
 *
 * Each register becomes a call that returns a byte to read or write through,
 * so the fake can tell COMMAND from RESULT although they share an address,
 * and acts on a write the next time any register is touched.
 */
#pragma once

#include_next "ps1/registers.h"

#include <stdint.h>

#undef CDROM_ADDRESS
#undef CDROM_COMMAND
#undef CDROM_PARAMETER
#undef CDROM_HCHPCTL
#undef CDROM_HINTMSK_W
#undef CDROM_HCLRCTL
#undef CDROM_HSTS
#undef CDROM_RESULT
#undef CDROM_RDDATA
#undef CDROM_HINTSTS
#undef CDROM_ATV0
#undef CDROM_ATV1
#undef CDROM_ATV2
#undef CDROM_ATV3
#undef CDROM_ADPCTL
#undef IRQ_STAT
#undef IRQ_MASK
#undef BIU_DEV4_DELAY
#undef BIU_DEV5_DELAY
#undef SPU_MVOLL
#undef SPU_MVOLR
#undef SPU_EVOLL
#undef SPU_EVOLR
#undef SPU_KOF0
#undef SPU_KOF1
#undef SPU_EON0
#undef SPU_EON1
#undef SPU_ATTR
#undef SPU_STATX
#undef SPU_AVOLL
#undef SPU_AVOLR
#undef SPU_ESA
#undef SPU_TSA
#undef SPU_DATA
#undef SPU_RAM_CTRL
#undef SPU_REVERB_dAPF1
#undef DMA_MADR
#undef DMA_BCR
#undef DMA_CHCR
#undef DMA_DPCR

typedef enum {
	FAKE_ADDRESS, FAKE_COMMAND, FAKE_PARAMETER, FAKE_HCHPCTL, FAKE_HINTMSK,
	FAKE_HCLRCTL, FAKE_HSTS, FAKE_RESULT, FAKE_RDDATA, FAKE_HINTSTS,
	FAKE_ATV0, FAKE_ATV1, FAKE_ATV2, FAKE_ATV3, FAKE_ADPCTL
} FakeRegister;

volatile uint8_t *fakeCdRegister(FakeRegister which);
volatile uint16_t *fakeSpuStat(void);
volatile uint16_t *fakeSpuData(void);

extern volatile uint16_t fakeIrqStat, fakeIrqMask;
extern volatile uint32_t fakeDev4, fakeDev5;
extern volatile uint16_t fakeSpu[32];
extern volatile uint16_t fakeReverb[32];
extern volatile uint16_t fakeSpuEsa, fakeSpuTsa, fakeSpuRamCtrl;

#define CDROM_ADDRESS   (*fakeCdRegister(FAKE_ADDRESS))
#define CDROM_COMMAND   (*fakeCdRegister(FAKE_COMMAND))
#define CDROM_PARAMETER (*fakeCdRegister(FAKE_PARAMETER))
#define CDROM_HCHPCTL   (*fakeCdRegister(FAKE_HCHPCTL))
#define CDROM_HINTMSK_W (*fakeCdRegister(FAKE_HINTMSK))
#define CDROM_HCLRCTL   (*fakeCdRegister(FAKE_HCLRCTL))
#define CDROM_HSTS      (*fakeCdRegister(FAKE_HSTS))
#define CDROM_RESULT    (*fakeCdRegister(FAKE_RESULT))
#define CDROM_RDDATA    (*fakeCdRegister(FAKE_RDDATA))
#define CDROM_HINTSTS   (*fakeCdRegister(FAKE_HINTSTS))
#define CDROM_ATV0      (*fakeCdRegister(FAKE_ATV0))
#define CDROM_ATV1      (*fakeCdRegister(FAKE_ATV1))
#define CDROM_ATV2      (*fakeCdRegister(FAKE_ATV2))
#define CDROM_ATV3      (*fakeCdRegister(FAKE_ATV3))
#define CDROM_ADPCTL    (*fakeCdRegister(FAKE_ADPCTL))

#define IRQ_STAT        fakeIrqStat
#define IRQ_MASK        fakeIrqMask
#define BIU_DEV4_DELAY  fakeDev4
#define BIU_DEV5_DELAY  fakeDev5

enum {
	FAKE_SPU_MVOLL, FAKE_SPU_MVOLR, FAKE_SPU_EVOLL, FAKE_SPU_EVOLR,
	FAKE_SPU_KOF0, FAKE_SPU_KOF1, FAKE_SPU_EON0, FAKE_SPU_EON1,
	FAKE_SPU_ATTR, FAKE_SPU_AVOLL, FAKE_SPU_AVOLR
};

#define SPU_MVOLL fakeSpu[FAKE_SPU_MVOLL]
#define SPU_MVOLR fakeSpu[FAKE_SPU_MVOLR]
#define SPU_EVOLL fakeSpu[FAKE_SPU_EVOLL]
#define SPU_EVOLR fakeSpu[FAKE_SPU_EVOLR]
#define SPU_KOF0  fakeSpu[FAKE_SPU_KOF0]
#define SPU_KOF1  fakeSpu[FAKE_SPU_KOF1]
#define SPU_EON0  fakeSpu[FAKE_SPU_EON0]
#define SPU_EON1  fakeSpu[FAKE_SPU_EON1]
#define SPU_ATTR  fakeSpu[FAKE_SPU_ATTR]
#define SPU_AVOLL fakeSpu[FAKE_SPU_AVOLL]
#define SPU_AVOLR fakeSpu[FAKE_SPU_AVOLR]
#define SPU_STATX (*fakeSpuStat())

/*
 * Sound RAM transfers: SPU_DATA fills a FIFO, and the fake sends it to RAM at
 * the transfer address when SPUCNT is next seen in manual-write mode.
 */
#define SPU_ESA          fakeSpuEsa
#define SPU_TSA          fakeSpuTsa
#define SPU_RAM_CTRL     fakeSpuRamCtrl
#define SPU_DATA         (*fakeSpuData())
#define SPU_REVERB_dAPF1 fakeReverb[0]

/*
 * DMA, for sound RAM reads: only channel 4 is used, so the channel number
 * is ignored. A host pointer does not fit the real 32-bit MADR, so it is a
 * uintptr_t here. The transfer happens the next time CHCR is looked at.
 */
extern volatile uintptr_t fakeDmaMadr;
extern volatile uint32_t  fakeDmaBcr, fakeDmaDpcr;
volatile uint32_t *fakeDmaChcr(void);

#define DMA_MADR(N) fakeDmaMadr
#define DMA_BCR(N)  fakeDmaBcr
#define DMA_CHCR(N) (*fakeDmaChcr())
#define DMA_DPCR    fakeDmaDpcr
