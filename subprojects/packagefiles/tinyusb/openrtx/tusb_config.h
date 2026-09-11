#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------
// COMMON CONFIGURATION
//--------------------------------------------------------------------
   
// Select CFG_TUSB_MCU starting from CPU type passed by the build system
#ifdef STM32F405xx
  #define CFG_TUSB_MCU OPT_MCU_STM32F4
#elif defined(STM32H743xx)
  #define CFG_TUSB_MCU OPT_MCU_STM32H7
#else
  #error CFG_TUSB_MCU must be defined
#endif

// RHPort number used for device can be defined by board.mk, default to port 0
#ifndef BOARD_DEVICE_RHPORT_NUM
  #define BOARD_DEVICE_RHPORT_NUM     0
#endif

// RHPort max operational speed can defined by board.mk
// Default to Highspeed for MCU with internal HighSpeed PHY (can be port specific), otherwise FullSpeed
#ifndef BOARD_DEVICE_RHPORT_SPEED
   #if (CFG_TUSB_MCU == OPT_MCU_LPC18XX || CFG_TUSB_MCU == OPT_MCU_LPC43XX || CFG_TUSB_MCU == OPT_MCU_MIMXRT10XX || \
        CFG_TUSB_MCU == OPT_MCU_NUC505  || CFG_TUSB_MCU == OPT_MCU_CXD56 || CFG_TUSB_MCU == OPT_MCU_SAMX7X)
    #define BOARD_DEVICE_RHPORT_SPEED   OPT_MODE_HIGH_SPEED
  #else
    #define BOARD_DEVICE_RHPORT_SPEED   OPT_MODE_FULL_SPEED
  #endif
#endif

// Device mode with rhport and speed defined by board.mk
#if   BOARD_DEVICE_RHPORT_NUM == 0
  #define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#elif BOARD_DEVICE_RHPORT_NUM == 1
  #define CFG_TUSB_RHPORT1_MODE     (OPT_MODE_DEVICE | BOARD_DEVICE_RHPORT_SPEED)
#else
  #error "Incorrect RHPort configuration"
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS                 OPT_OS_NONE
#endif

// CFG_TUSB_DEBUG is defined by compiler in DEBUG build
#define CFG_TUSB_DEBUG           0

/* USB DMA on some MCUs can only access a specific SRAM region with restriction on alignment.
 * Tinyusb use follows macros to declare transferring memory so that they can be put
 * into those specific section.
 * e.g
 * - CFG_TUSB_MEM SECTION : __attribute__ (( section(".usb_ram") ))
 * - CFG_TUSB_MEM_ALIGN   : __attribute__ ((aligned(4)))
 */

/*
 * On STM32H7 the DWC2 core is used in slave (FIFO) mode: CFG_TUD_DWC2_DMA_ENABLE
 * keeps its default of 0, so the CPU copies every packet to/from the packet
 * FIFO and no bus master ever touches the endpoint buffers. They can therefore
 * live in the ordinary AXI SRAM together with the rest of .bss, and no D-cache
 * maintenance is needed (the only DMA-inaccessible RAM on the H7 is DTCM, which
 * the CS7000P linker script does not use for data anyway).
 *
 * If DMA mode is ever enabled, set CFG_TUD_MEM_DCACHE_ENABLE to 1 and align the
 * buffers to CFG_TUSB_MEM_DCACHE_LINE_SIZE (32 bytes on Cortex-M7); with
 * write-through cacheable RAM (Miosix MPU setup) the clean/invalidate calls in
 * dcd_dwc2.c then keep the buffers coherent.
 */
#ifndef CFG_TUSB_MEM_SECTION
  #define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
  #define CFG_TUSB_MEM_ALIGN  __attribute__((aligned(4)))
#endif

#ifdef STM32H743xx
  #define CFG_TUD_MEM_DCACHE_ENABLE       0
#endif

//--------------------------------------------------------------------
// DEVICE CONFIGURATION
//--------------------------------------------------------------------

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

//------------- CLASS -------------//
#define CFG_TUD_CDC               1
#define CFG_TUD_MSC               0
#define CFG_TUD_HID               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            0

/*
 * Enable CDC SERIAL_STATE notifications (interrupt EP 0x81).
 * Lets the device report DCD/DSR to the host; Linux cdc-acm uses this for
 * carrier so open() on /dev/ttyACMx does not wait indefinitely for modem status.
 */
#define CFG_TUD_CDC_NOTIFY        1

// CDC FIFO size of TX and RX
#define CFG_TUD_CDC_RX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_CDC_TX_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)

// CDC Endpoint transfer buffer size, more is faster
#define CFG_TUD_CDC_EP_BUFSIZE   (TUD_OPT_HIGH_SPEED ? 512 : 64)

#ifdef __cplusplus
 }
#endif

#endif /* _TUSB_CONFIG_H_ */
