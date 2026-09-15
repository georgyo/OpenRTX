# DMR bring-up checklist

Nothing of the DMR series has been run on a radio. This document lists, in
the order in which they should be checked, every assumption that comes from
the HR_C6000 manual, the CS7000-M17 Plus schematic or the observation of other
firmwares but has not been confirmed on a CS7000 or CS7000-M17 Plus. Each
entry names the code that carries the matching `UNVERIFIED on hardware`
comment, what to observe, and what to change if the observation disagrees.

The list grows with the stages of the series (see [dmr_mode.md](dmr_mode.md));
an entry is removed, and the comment in the code with it, once somebody has
checked it on a radio and recorded the result here.

## 1. GPIO external interrupts (`platform/drivers/GPIO/gpio_irq_stm32.cpp`)

The HR_C6000 signals the slot timing and the layer-2 events with ~300 ns
active-low pulses on TIME_SLOT_INTER, SYS_INTER and RF_TX_INTER (manual: "the
interrupt low pulse is valid, the pulse width is 3 system working clocks" of
9.8304 MHz). They can only be caught with a falling-edge EXTI interrupt;
polling is not viable. `gpioIrq_attach()` programs the STM32 EXTI for one
callback per line, the interrupt handlers latch flags and wake a thread, and
nothing touches the SPI bus in interrupt context.

### 1.1 PC2_C / PC3_C reach the EXTI logic with SYSCFG_PMCR at reset

- Where: header comment of `gpio_irq_stm32.cpp`.
- Assumption: on the STM32H743 of the CS7000-M17 Plus, the schematic (V01)
  routes RF_TX_INTER and SYS_INTER to the pads PC2_C and PC3_C. The datasheet
  (DS12110, pin table note 6) says the `_C` pads reach the PC2/PC3 digital
  logic through analogue switches controlled by `SYSCFG_PMCR` bits `PC2SO` and
  `PC3SO`, which are closed at reset. Nothing in the tree opens them, and the
  driver deliberately never writes `SYSCFG_PMCR`, so a falling-edge EXTI on
  PC2 or PC3 should see the pulses.
- Check: in DMR receive near an active repeater or hotspot, the per-line
  counters of the DMR monitor screen (stage 3, part B) must show PC0 ticking
  at about 33 Hz (TIME_SLOT_INTER) and PC3 or PC1 pulsing roughly every 60 ms
  (SYS_INTER), PC2 staying idle in receive.
- If wrong: if PC2/PC3 never fire while PC0 does, the switches are open on
  this board (an option byte or the bootloader may have set `PC2SO`/`PC3SO`).
  Clear them once at init in `platform.c` of the CS7000-PLUS target, with a
  comment, before attaching the lines. Do not clear them blindly beforehand:
  with the switches closed the ADC3 inputs on PC2/PC3 and the digital pads
  share the pin, and `radio_CS7000.cpp` notes that ADC3 inputs on this package
  are PC0-PC3.

### 1.2 Which pin carries SYS_INTER (PC1 or PC3)

- Where: `platform/targets/CS7000/pinmap.h` and
  `platform/targets/CS7000-PLUS/pinmap.h` (`DMR_SYS_INT`), attached by the
  stage-3 modem driver (part B).
- Assumption: the pinmaps say TS = PC0, SYS = PC1, TX = PC2 (from the reverse
  engineering of the F405 CS7000 firmware); the CS7000-M17 Plus schematic says
  TS = PC0, RF_TX = PC2_C, SYS = PC3_C and puts the 2T/5T analogue tone
  detector on PC1. The two boards may differ.
- Check: the counters of check 1.1 tell which line pulses every 60 ms.
- If wrong: fix `DMR_SYS_INT` per target. The EXTI shim attaches PC0..PC3 all
  four during bring-up, so no rebuild is needed to see the answer.

### 1.3 NVIC priority 14 for the EXTI lines

- Where: `EXTI_IRQ_PRIORITY` in `gpio_irq_stm32.cpp`.
- Assumption: priority 14 (one above the Miosix tick at 15, same as the USB
  and GPS drivers) is enough for the ~300 ns pulses, since the EXTI latches
  the edge in hardware and the handler only records it; the timing budget of
  the slot (t2 = 27 ms to program the next slot) tolerates the latency of the
  SPI DMA interrupts and of the audio streams.
- Check: the TS counter must not skip values while the display refreshes and
  the OpenMusic DAC stream runs (stage 4); the monitor screen shows a "TS lost"
  count once the modem driver tracks the 30 ms period.
- If wrong: lower the number (higher priority), keeping it above the SPI DMA
  streams so that a callback can never pre-empt a bus transfer that the rtx
  thread will read right after.

### 1.4 The handler symbols

- Where: bottom of `gpio_irq_stm32.cpp`.
- Constraint, not a hardware unknown: the file defines
  `EXTI0..EXTI4_IRQHandler`, `EXTI9_5_IRQHandler` and `EXTI15_10_IRQHandler`
  with C++ linkage, as the vector tables in `platform/mcu/STM32{F4,H7}xx/boot/
  startup.cpp` declare them. `chSelector_UV3x0.c` and `chSelector_MD9600.c`
  define `EXTI15_10_IRQHandler` for the MD-UV3x0, DM-1701 and MD-9600, so the
  shim is listed only in the CS7000 and CS7000-PLUS source lists of
  `meson.build`. A target that needs both must split the handler.
- Check: the CS7000 and CS7000-PLUS firmwares link (done in CI); the vector
  table entry for EXTI0 must point into `gpio_irq_stm32.cpp` (`arm-none-eabi-nm
  openrtx_cs7000p | grep EXTI0`).
