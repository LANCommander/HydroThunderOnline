#pragma once
#include <stdint.h>

/* Per-thread emulated interrupt flag (shared by EtsSetInterruptFlag and the CLI/STI emulation). */
int hw_set_interrupt_flag(int enable); /* returns the previous state, 1 = enabled */

/* The IRQ number the host reports for the sound chip (written to ymf's IRQ variable). */
#define HW_AUDIO_IRQ 5
/* Mask/unmask the audio IRQ: a recursive lock shared by EtsPicEnable and the audio thread. */
void hw_audio_irq_lock(int lock);

/* At exit: write the in-game volume through to the CMOS file. */
void hw_cmos_flush_volume(void);

uint32_t hw_port_in(uint32_t port, int size);
void hw_port_out(uint32_t port, uint32_t v, int size);
