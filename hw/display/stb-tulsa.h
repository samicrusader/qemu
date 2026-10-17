/*
 * STB Systems "Tulsa" video/graphics card.
 *
 * Reverse-engineered from the Hollywood (Prevue / TV Guide Channel) units.
 * Every constant here is derived from stbtulsa.sys, STBTCtl.dll, STBTdde.dll,
 * the i960 firmware image and the vendor SDK; see the project notes for the
 * derivation of each one.
 *
 * Topology, as stbtulsa.sys discovers it:
 *
 *   primary bus   dev N fn 0   10B4:0100   i960RP PCI-to-PCI bridge
 *                 dev N fn 1   10B4:0101   i960RP Address Translation Unit
 *   secondary bus dev 0 fn 0   1131:7146   Philips SAA7146
 *                 dev 4 fn 0   1163:2000   Rendition Verite V2200, RGB
 *                 dev 8 fn 0   1163:2000   Rendition Verite V2200, YUV
 *
 * The RGB/YUV roles are decided purely by secondary-bus device number, so
 * those numbers are fixed, not configurable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_STB_TULSA_H
#define HW_DISPLAY_STB_TULSA_H

#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bridge.h"
#include "qom/object.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "stb-v2200.h"
#include "stb-v2200-pix.h"

/* None of these are in pci_ids.h. */
#define PCI_VENDOR_ID_STB               0x10b4
#define PCI_DEVICE_ID_STB_TULSA_BRIDGE  0x0100
#define PCI_DEVICE_ID_STB_TULSA_ATU     0x0101

#define PCI_VENDOR_ID_RENDITION         0x1163
#define PCI_DEVICE_ID_RENDITION_V2200   0x2000

#define PCI_VENDOR_ID_PHILIPS_SEMI      0x1131
#define PCI_DEVICE_ID_PHILIPS_SAA7146   0x7146

#define STB_TULSA_DEVFN_SAA7146  PCI_DEVFN(0, 0)
#define STB_TULSA_DEVFN_V2200RGB PCI_DEVFN(4, 0)
#define STB_TULSA_DEVFN_V2200YUV PCI_DEVFN(8, 0)

#define STB_TULSA_MAX_CARDS 4    /* MAX_STB_TULSA_CARDS */
#define STB_TULSA_CHANNELS  2    /* two stream channels */

/* ------------------------------------------------------------------ *
 * ATU aperture
 *
 * One 4 MiB memory BAR.  The bottom 0x40 bytes are the i960RP messaging
 * unit, mirroring the PMMR block at 0x1300 one-for-one; everything above is
 * board-side shared memory that the host reads and writes directly.
 *
 * The aperture is 4 MiB, not 64 KiB.  stbtulsa.sys maps only 0x10000 of it
 * for its own use - HwFindAdapter's MmMapIoSpace length is a literal
 * 0x10000, which is all the kernel side ever touches - but IOCTL 0x22202c
 * hands user mode the *whole* BAR: the handler copies the HAL's
 * CM_PARTIAL_RESOURCE_DESCRIPTOR straight across, Memory.Start into
 * pCardDevExt+0x14 and Memory.Length into pCardDevExt+0x18, and MapToUser()
 * ZwMapViewOfSection's that length out of \Device\PhysicalMemory.  The
 * length therefore comes from the BAR's own size encoding, and the firmware
 * downloader in STBTCtl.dll proves it has to be at least 0x380000:
 * STBT960.hex is one contiguous image from 0xa02d8000 to 0xa0380000, the
 * only window-slide path (sub_100016fe) is never called during the download,
 * and the write is a flat aperture + (recordAddr - 0xa0000000).  Next power
 * of two up is 4 MiB.  See TULSA.md section 44.
 * ------------------------------------------------------------------ */

#define STB_I960_DRAM_BASE       0xa0000000u  /* i960 local bus base of board DRAM */
#define STB_TULSA_ATU_BAR_SIZE   0x400000     /* 4 MiB; see above */
#define STB_TULSA_ATU_TRACE_LIMIT 0x10000     /* only trace the low 64 KiB   */
#define STB_TULSA_MU_SIZE        0x40

#define STB_MU_IMR0  0x10   /* inbound  message 0 - host->i960 doorbell     */
#define STB_MU_IMR1  0x14   /* inbound  message 1 - entry point on download */
#define STB_MU_OMR0  0x18   /* outbound message 0 - i960->host              */
#define STB_MU_OMR1  0x1c   /* outbound message 1                           */
#define STB_MU_IDR   0x20   /* inbound  doorbell                            */
#define STB_MU_IISR  0x24   /* inbound  interrupt status                    */
#define STB_MU_IIMR  0x28   /* inbound  interrupt mask                      */
#define STB_MU_ODR   0x2c   /* outbound doorbell, write-1-to-clear          */
#define STB_MU_OISR  0x30   /* outbound interrupt status                    */
#define STB_MU_OIMR  0x34   /* outbound interrupt mask, 1 = masked          */

/* OISR bits, as decoded by TulsaDPC. */
#define STB_OISR_OMR0     (1u << 0)
#define STB_OISR_OMR1     (1u << 1)
#define STB_OISR_DOORBELL (1u << 2)

/*
 * Outbound doorbell bits, read straight off STBTCtl.dll's decoder
 * (sub_100022A9), which tests the ODR snapshot byte by byte:
 *
 *   bit  chan      -> sub_1000359F, the stream state machine
 *   bit  6 + chan  -> sub_100037D6
 *   bit  8 + chan  -> sub_10002F73
 *   byte 5 bit 2   -> sub_10001D29, the vertical callback dispatcher
 *   byte 7 bit 7   -> ++dev[183], the DLL's own vertical counter
 *
 * The last two are what ISTBTBoardControl::CallOnBoardVertical arms against.
 * That call is entirely host-side - it only records the call type and the
 * ISTBTVerticalCallback object - so the board has no idea whether anything is
 * listening and rings both bits on every field unconditionally.
 */
#define STB_ODR_CHAN(c)   (1u << (c))
#define STB_ODR_VERTICAL  (1u << 10)
#define STB_ODR_FIELDTICK (1u << 31)

/*
 * Shared-memory layout within the ATU aperture.  Offsets are host-visible
 * byte offsets from the base of the BAR.
 */
#define STB_SHM_OVL_FLAG     0x0050  /* host -> i960: overlay-active boolean */
#define STB_SHM_VSYNC_TRIM   0x0054  /* host -> i960, from Verite memory 0x54dc */
#define STB_SHM_LINECOUNT_TRIM 0x0058 /* i960 -> host, feeds SetVerticalLineCount */
#define STB_SHM_READY        0x0080  /* TULSA_TEST_STATUS.bDiagsFinished    */

/*
 * TULSA_TEST_STATUS, 44 bytes at SHM +0x80 (IStbTulsaCtl.h:318-332).
 *
 * STBTCtl.dll!GetSelfTestResults is a straight qmemcpy of 0x2C bytes from
 * here, and returns ERR 2062 unless the first dword is non-zero.  The same
 * dword is what GetBoardStatus tests to promote its host-side state from
 * BOARD_STAT_INITIALIZING (4) to BOARD_STAT_RUNNING (3):
 *
 *     if (dev->state == 4 && *(DWORD *)shm_0x80) dev->state = 3;
 *
 * so it is bDiagsFinished and the board-ready flag at once.  The sample
 * app's Self-Test Results dialog renders each flag as "Ok"/"Bad", and shows
 * "?" for the four tests gated behind bAVDecoderRegTestPassed.
 */
/*
 * Board field counter.  STBTCtl.dll!GetBoardVertical is
 *
 *     *out = *(DWORD *)(chan_state_base(dev) + 8);
 *
 * and chan_state_base is SHM +0x150 (section 15.2), so the counter is at
 * +0x158.  Section 25 measured a real card advancing its scroll 1 pixel per
 * 2 verticals over 15 seconds with no drift, which is NTSC's 59.94 Hz field
 * rate -- verticals are fields, not frames.
 */
#define STB_SHM_VERTICAL     0x0158
#define STB_FIELD_NUM        1001     /* NTSC field period = 1001/60000 s */
#define STB_FIELD_DEN        60000

#define STB_SHM_TEST_ENCODER_ID  0x0084  /* VidEncoderID                     */
#define STB_SHM_TEST_FIRST       0x0088  /* bAVDecoderRegTestPassed          */
#define STB_SHM_TEST_COUNT       9       /* through bMixerTestPassed, +0xa8  */

/* The encoder answers 0xCD from its own ID register; see section 19.2. */
#define STB_TEST_ENCODER_ID      0xcd
#define STB_SHM_CHANSTATE    0x0150  /* per-channel stream state, stride    */
#define STB_SHM_CHANSTATE_SZ 0x0068
/*
 * The per-channel stream state block, section 47.2 and section 65.6.
 *
 * STBTCtl.dll's sub_100022A9 walks `for (i = 0; i < 208; i += 104)` over
 * sub_100017E4()'s base -- 104 = 0x68 = the stride, 208 = both channels --
 * and on outbound-doorbell bit `1 << chan` calls
 *
 *     sub_1000359F(chan, *(base + chan*0x68 + 0x10),
 *                        *(base + chan*0x68 + 0x14));
 *
 * so the notification payload is two words of ordinary shared memory that
 * the board writes before it rings.  +0x14 must echo the stream handle the
 * host assigned, or sub_1000359F returns without doing anything.
 */
#define STB_SHM_CHAN_CODE    0x0010  /* within the per-channel block */
#define STB_SHM_CHAN_HANDLE  0x0014

/*
 * The codes sub_1000359F's transition table accepts, named for what they do
 * to the host's own per-channel state word (section 15.6: 1 idle, 4 after
 * download, 6/7/8/9 set when commands 2/3/4/5 are issued).
 *
 *   a3 == 1          -> state 1, unconditionally
 *   a3 == 4          -> state 4, unconditionally
 *   from 6: 2 -> 2,  5 -> 5,  3 -> 3
 *   from 2: 3 -> 3,  5 -> 5
 *   from 3: 2 -> 2        from 5: 2 -> 2
 *   from 8: 3 -> 3        from 9: 2 -> 2
 *
 * Reaching a *different* state is what matters: on a change the DLL fills a
 * 52-byte status block and calls the COM notification sink, which is what
 * UVSTBCard's 500 ms NOTIFY_WAIT (section 65.2) is waiting on.
 */
/*
 * Section 68.8.  These are not arbitrary "codes": they are the SDK's
 * DECODE_STATE enum from inc/IStbTulsaCtl.h, and the host's own state word is
 * a value from the same enum.  That makes the whole table in sub_1000359F
 * read as plain English - PLAY_REQUESTED accepts PLAYING, PAUSE_REQUESTED
 * accepts PAUSED, and so on.
 */
#define STB_DEC_UNKNOWN          0
#define STB_DEC_STOPPED          1
#define STB_DEC_PLAYING          2
#define STB_DEC_PAUSED           3
#define STB_DEC_ERROR            4
#define STB_DEC_PLAY_PENDING     5
#define STB_DEC_PLAY_REQUESTED   6
#define STB_DEC_STOP_REQUESTED   7
#define STB_DEC_PAUSE_REQUESTED  8
#define STB_DEC_RESUME_REQUESTED 9

#define STB_SHM_MAILBOX      0x0450  /* cmd, chan, arg, then <=124B payload */
#define STB_SHM_DEVCTX       0x04c0  /* device context                      */
#define STB_SHM_BUFDESC      0x8000  /* per-channel buffer descriptors      */
#define STB_SHM_BUFDESC_SZ   0x00c0
#define STB_SHM_DRAMSIZE     0xc000  /* board DRAM size in bytes            */
#define STB_SHM_TMC_IMAGES   0xf080  /* 3 x 0x410 analog register images    */
#define STB_SHM_TMC_STRIDE   0x0410
#define STB_SHM_TMC_SCALAR   0xfcb0  /* TMC_IMAGES + 0xc30                  */
#define STB_SHM_TMC_READBACK 0xfcb8  /* mirror of the active TMC config     */

/* Mailbox word indices. */
#define STB_MBOX_CMD     0
#define STB_MBOX_CHAN    1
#define STB_MBOX_ARG     2
#define STB_MBOX_PAYLOAD 3
#define STB_MBOX_PAYLOAD_MAX 124

/*
 * Command dispatch.  The Control task subtracts 2 and bounds-checks against
 * 0x18, so 2..26 inclusive are valid; anything else falls through to the
 * tail.  A channel number above 2 hits a self-loop in the firmware and hangs
 * the i960 outright, which we model as a latched fault rather than a hang.
 *
 * Individual command numbers within that range are NOT yet established -
 * only the range, the 0x50-byte InitBoard block and the 56-byte payload of
 * command 2 are.  Everything else is logged for identification against a
 * real driver trace.
 */
/*
 * Section 68.  The stream commands are no longer guesses: STBTCtl.dll's
 * ISTBTMPEGDecoderControl vtable at 0x1000e1b0+0x30 lists its methods in
 * the header's (alphabetical) order, and each one is a two-line wrapper
 * around SendCommand, so the method and the command number pair up exactly:
 *
 *   GetStatus  NotifyAt  Pause=5  Play=2  Resume=4  Stop=3
 *
 * There is no separate "load": Play() opens the file, sends command 2 and
 * starts the feeder thread, and the board is expected to report *twice* -
 * code 2 when the stream is accepted and code 3 once it is actually
 * decoding.  Only the second one satisfies UVSTBCard's NOTIFY_WAIT.
 *
 * Command 17 on the same vtable (index 5) is ISTBTBoardControl::EnableBypass
 * and its arg is the BOOL, not a stream handle.
 */
#define STB_CMD_STREAM_PLAY    2   /* 56-byte payload + filename, arg=handle */
#define STB_CMD_STREAM_STOP    3
#define STB_CMD_STREAM_PAUSE   4
#define STB_CMD_STREAM_RESUME  5
#define STB_CMD_ENABLE_BYPASS  17

/*
 * The device context at STB_SHM_DEVCTX **is a live TULSA_CONFIG** - the same
 * 0x50-byte block InitBoard takes, kept in shared memory and edited in place.
 *
 * Section 77: the firmware handlers for 11-16, 18-20 and 23 all begin
 * `call sub_A02E4EF0`, two instructions returning the constant 0xA00004C0,
 * and then read their parameter out of it at a fixed offset.  The mailbox
 * payload for them is empty; the host writes the field and rings a doorbell
 * meaning "apply this one".  Every offset those handlers read lands exactly
 * on a TULSA_CONFIG member, which is what identifies the commands:
 *
 *   cmd 14  +0x08  audio_delay        ISTBTAudioControl::SetDelay
 *   cmd 15  +0x0c  audio_source       ISTBTAudioControl::SetSource
 *   cmd 16  +0x10  audio_volume[3]    ISTBTAudioControl::SetVolume
 *   cmd 18  +0x1c  line21_passthru    ISTBTVBIControl::EnableLine21PassThru
 *   cmd 19  +0x24  video_source[3]    ISTBTVideoControl::SetVideoPortSource
 *   cmd 23  +0x3c  audio_phase[3]     ISTBTAudioControl::SetPhaseOffset
 *   cmd 11  +0x20, +0x30   video_mode, disable_genlock
 *   cmd 12  +0x20          video_mode (with the TMC scalar at cfg+0xc30)
 *   cmd 13  +0x20, +0x34   video_mode, genlock_source
 *   cmd 20  +0x34          genlock_source  (shares 13's handler)
 *
 * The three "per-index setters" of section 68.3 are the three arrays: the
 * wrapper writes one element and the command applies all three, which is why
 * command 19's host side range-checks the value against 3 - that is the
 * source count, not an array bound.
 */
#define STB_CTX_AUDIO_DELAY    0x08   /* cmd 14 */
#define STB_CTX_AUDIO_SOURCE   0x0c   /* cmd 15 */
#define STB_CTX_AUDIO_VOLUME   0x10   /* cmd 16, 3 dwords */
#define STB_CTX_LINE21         0x1c   /* cmd 18 - no-op in this firmware */
#define STB_CTX_VIDEO_MODE     0x20   /* cmds 11, 12, 13 */
#define STB_CTX_VIDEO_SOURCE   0x24   /* cmd 19, 3 dwords, each < 3 */
#define STB_CTX_DISABLE_GENLOCK 0x30  /* cmd 11 */
#define STB_CTX_GENLOCK_SOURCE 0x34   /* cmds 13 and 20 */
#define STB_CTX_AUDIO_PHASE    0x3c   /* cmd 23, 3 dwords */

#define STB_CMD_MIN  2
#define STB_CMD_MAX  26
#define STB_CMD_INITBOARD_LEN 0x50  /* sizeof(TULSA_CONFIG) */

/* Board status, as GetBoardStatus reports it. */
typedef enum {
    STB_BOARD_RESET = 0,
    STB_BOARD_INIT,
    STB_BOARD_RUNNING,
    STB_BOARD_FAULT,
} STBBoardStatus;

/* Ready-flag bits at STB_SHM_READY. */
#define STB_READY_PRESENT (1u << 0)
#define STB_READY_RUNNING (1u << 1)

/*
 * TULSA_CONFIG, the 0x50-byte block InitBoard takes.  Field order and
 * offsets are from the vendor SDK header; the two BYTE members each force
 * three bytes of padding before the next enum.
 */
typedef struct QEMU_PACKED {
    uint32_t disk_buffer_count;
    uint32_t disk_buffer_size;
    uint32_t audio_delay;          /* milliseconds */
    uint32_t audio_source;         /* 0 MPEG, 1 analog in, 2 ext digital   */
    uint32_t audio_volume[3];      /* dB, per source                       */
    uint32_t line21_passthru;
    uint32_t video_mode;           /* 0 NTSC, 1 PAL                        */
    uint32_t video_source[3];      /* YUV primary, YUV secondary, RGB      */
    uint8_t  disable_genlock;
    uint8_t  pad0[3];
    uint32_t genlock_source;       /* 0 video in, 1 genlock in             */
    uint8_t  disable_avsync;
    uint8_t  pad1[3];
    int32_t  audio_phase[3];       /* -3..+3, 20.83 us per step            */
    uint32_t acc_enable;
    int32_t  thread_priority;      /* host-side only, but occupies 0x4c    */
} STBTulsaConfig;

/* Verite apertures. */
#define STB_V2200_MEM_SIZE 0x1000000  /* BAR0, 16 MiB */
#define STB_V2200_REG_SIZE 0x40000    /* BAR2, 256 KiB */

/*
 * Overlay command-list machinery, at these offsets in Verite memory.  The
 * three lists are 0x808 bytes apart - 514 dwords, being two header dwords
 * plus sixteen 32-dword slots.  A slot's first dword is a jump address into
 * the Verite microcode, not an enumerated opcode.
 */
#define STB_V2200_CMDLIST0    0x54e0
#define STB_V2200_CMDLIST1    0x5ce8
#define STB_V2200_CMDLIST2    0x64f0
#define STB_V2200_CMDLIST_SZ  0x808
/*
 * Microcode-load handshake, from v1kload_uc_file in STBTdde.dll: it stores the
 * byte 0x81 at +0x20060, then spins up to 0xFFFFFF times reading +0x20064
 * until it equals 0x800 - the value _v1kboard_init and _v2kboard_init put in
 * the object as the expected reply - and only then parses STBT2200.uc.
 */
/*
 * risc_forcestep in STBTdde.dll shows what these really are: the host stops
 * the Vérité's RISC core, writes 0x80 to +0x20060 and spins until it reads
 * back, writes an instruction word to +0x20064 and spins until *that* reads
 * back, then sets bits 1 and 2 of the control byte +0x20048 and waits for bit
 * 2 to clear.  They are latches that the software verifies by reading, not
 * status registers - so the aperture has to behave like memory.
 */
#define STB_V2200_FIFO_STAT   0x20040   /* low 5 bits: free slots in the command FIFO */
#define STB_V2200_FIFO_FREE   0x1f      /* we consume instantly, so always wide open   */
#define STB_V2200_RISC_CTRL   0x20048   /* bit 1 stop, bit 2 step (self-clearing) */
#define STB_V2200_RISC_STAT   0x2004a   /* bits 3:2 ready, bits 1:0 stopped       */
#define STB_V2200_RISC_MODE   0x20060   /* 0x80 instruction injection, 0x81 load  */
#define STB_V2200_RISC_DATA   0x20064   /* instruction / data latch               */
#define STB_V2200_RISC_STOPPED 0x0f     /* ready | stopped, for a core we do not run */
#define STB_V2200_RISC_MODE_STEP 0x80   /* 0x20064 is the instruction latch       */
#define STB_V2200_RISC_MODE_PC   0x81   /* 0x20064 reads the core's current address */
#define STB_V2200_RISC_MODE_RF   0x82   /* 0x20064 reads the selected RF register  */
#define STB_V2200_RF_SELECT   0x10000000  /* OR'd into the latch to pick a register */
#define STB_V2200_RF_REGS     256
#define STB_V2200_RF_ADDR     0xfe      /* risc_readmem/writemem address register  */
#define STB_V2200_RF_DATA     0xfc      /* risc_readmem result register            */
#define STB_V2200_RF_VALUE    0xff      /* risc_writemem value register            */

/*
 * The subset of the Vérité's instruction set that STBTdde.dll force-steps to
 * bootstrap the board, recovered from writeRF, risc_readmem and risc_writemem.
 * Encoding is OP:RD:RS:RA, one byte each, or OP:RD:IMM16 for the loads.
 */
#define STB_V2200_OP_LDI      0x76      /* rD = imm16                   */
#define STB_V2200_OP_LDI_HI   0x77      /* rD[31:16] = imm16            */
#define STB_V2200_OP_LOAD8    0x70      /* rD = mem[rA], 1/2/4 bytes    */
#define STB_V2200_OP_STORE8   0x78      /* mem[rA] = rS, 1/2/4 bytes    */
#define STB_V2200_OP_JUMP     0x6c      /* pc = imm24 << 2              */

/*
 * The microcode's own data lives in the low part of the Verite aperture:
 * .data at 0x5000-0x71d0 and .bss to 0x790c (section 24.1/24.2).  That range
 * holds the three overlay command lists, the submit/retire pair, the flicker
 * filter tables and the screen geometry -- every structure the host and the
 * microcode use to talk to each other.  A tracing overlay across it makes
 * that conversation visible without trapping framebuffer writes, which are
 * allocated above it.
 */
/* The card's raster: 720x480 NTSC (section 5d, SetDisplayMode). */
/*
 * The YUV overlay plane's coordinate origin includes horizontal and vertical
 * blanking; the visible raster starts here within it.  Measured, not derived
 * - see section 99.16.  The RGB plane's base is already the visible origin,
 * so this applies to the YUV plane alone.
 */
#define STB_V2200_BLANK_X     82
#define STB_V2200_BLANK_Y     26

#define STB_V2200_DISP_W      720
#define STB_V2200_DISP_H      480

/* Microcode entry addresses used as display-list slot handlers (section 24.1). */
#define STB_V2200_UC_SETFRAMEBASE 0x3c3c
#define STB_V2200_UC_FILL         0x3cac

/* Payload +10, the source pixel format (section 21.3). */
#define STB_V2200_FMT_16BPP   4
#define STB_V2200_FMT_32BPP   12

/*
 * SCROLLSTATE, in Verite memory, decoded in section 21.4.  Every field is
 * 16.16 fixed point -- which is how the crawl was smooth on 1999 hardware,
 * and why advancing an integer pixel per field looks wrong in a way that is
 * obvious on screen and hard to diagnose later.
 */
#define STB_SS_POSITION      0x00
#define STB_SS_VERTICALS     0x08   /* verticals between steps */
#define STB_SS_PIXELS        0x0c   /* pixels per step, signed */
#define STB_SS_PAUSE         0x10   /* park here; out of range = never */
/*
 * +0x14 is the live countdown and +0x18 the reload, not the other way round.
 * CSurfaceControl::UpdateScrollStateNotifCounter() -- which SDE_SCROLL_NOTIFY_RESET
 * calls -- writes +0x14 alone, while UpdateScrollState() rewrites +0x18 along with
 * the other rate parameters and never touches +0x14.  So +0x18 is a parameter the
 * host owns and +0x14 is state the board owns, same split as the position at +0x00.
 */
/* Payload +12 blend selector, section 21.3 */
#define STB_V2200_BLT_ALPHA      0x80000
#define STB_V2200_BLT_ALPHA_NEG  0x230   /* 0x320 is the normal-alpha twin */

#define STB_SS_NOTIFY_CNT    0x14   /* live countdown, board-owned     */
#define STB_SS_NOTIFY_INC    0x18   /* reload value, host-owned        */
#define STB_SS_NOTIFY_FLAG   0x1c   /* board sets, ClearScrollStateNotifFlag clears */
#define STB_SS_NOTIFY_POS    0x20

#define STB_V2200_SCROLL_VERT 1
#define STB_V2200_SCROLL_HORZ 2
#define STB_V2200_MAX_SCROLL  8

#define STB_V2200_CTRL_BASE   0x5000
#define STB_V2200_CTRL_SIZE   0x3000

/*
 * Flicker-filter command block, 14 dwords, built by BuildFFCommandList and
 * read back by CSurfaceControl::CheckSurfaceForCompleteFFCommand:
 *
 *   [0] active flag      [2] source base   [4] width    [5] height
 *   [6] width  << 16     [7] lines remaining, 16.16     [13] -1
 *
 * Completion is word 7 reaching zero; the microcode decrements it per field.
 */
#define STB_V2200_FFCMD_ACTIVE 0x00
#define STB_V2200_FFCMD_LINES  0x1c

#define STB_V2200_CMD_CURRENT 0x6cf8  /* host writes the list it submitted */

/*
 * The microcode's ELF entry point, and how many instructions one
 * submitted list is allowed before the model gives up on it.  A field's
 * worth of real work is a few thousand; the ceiling is only here so a
 * microcode that spins cannot wedge the whole VM.
 */
#define STB_V2200_PENDING     8
#define STB_V2200_UCODE_ENTRY 0x1800
#define STB_V2200_LLE_BUDGET  (4u * 1000 * 1000)
#define STB_V2200_CMD_ACTIVE  0x6cfc  /* microcode publishes what it retired */
#define STB_V2200_SLOTS       16
#define STB_V2200_SLOT_DWORDS 32

/* SAA7146 register window and the board-ID strap inside it. */
/*
 * The analog capture path.
 *
 * On the card the BNC input is digitised by the SAA7146 and DMAed into
 * Verite memory, where the YUV processor's SetFrameBase points.  That is the
 * "analog in" the device context's audio_source and video_source select
 * (section 77), and in a headend it is the IRD's baseband output.
 *
 * NTSC fixes the raster, so unlike the guide bitmap - which can be any size -
 * these are standard: 720x480 UYVY 4:2:2, 1440 bytes a line, at 29.97 frames
 * a second.
 */
#define STB_VIDEO_BPL   (STB_V2200_DISP_W * 2)
#define STB_VIDEO_BYTES (STB_VIDEO_BPL * STB_V2200_DISP_H)

#define STB_SAA7146_REG_SIZE 0x200
#define STB_SAA7146_BOARDID  0x110   /* bits 4:3, active low */

/*
 * The capture registers, recovered in section 85 from
 * STBTdde.dll!Enable7146VideoPortYUVProc, which programs every one of them
 * and nothing else.  The host keeps a shadow of each block and copies it
 * down in one go: six dwords at 0x00 and eight at 0x5c.
 */
#define STB_SAA7146_BASE_ODD   0x00  /* DMA target, odd field              */
#define STB_SAA7146_BASE_EVEN  0x04  /* DMA target, even field             */
#define STB_SAA7146_PROT_ADDR  0x08  /* end-of-buffer protection           */
#define STB_SAA7146_PITCH      0x0c  /* bytes per line                     */
#define STB_SAA7146_BASE_PAGE  0x10  /* always 2 here                      */
#define STB_SAA7146_NUM_LINE   0x14  /* (lines per field << 16) | 724      */
#define STB_SAA7146_HPS        0x5c  /* the scaler block, 8 dwords         */
#define STB_SAA7146_MC1        0xfc  /* mask/value; bit 0x40 = capture on  */
#define STB_SAA7146_MC2        0x100 /* mask/value                         */

#define STB_SAA7146_MC1_CAPTURE 0x0040

/* Bridge extended control; bit 5 holds the i960 core in reset. */
#define STB_TULSA_EBCR        0x40
#define STB_TULSA_EBCR_RESET  (1u << 5)

#define TYPE_STB_TULSA         "stb-tulsa"
#define TYPE_STB_TULSA_ATU     "stb-tulsa-atu"
#define TYPE_STB_TULSA_V2200   "stb-tulsa-v2200"
#define TYPE_STB_TULSA_SAA7146 "stb-tulsa-saa7146"

OBJECT_DECLARE_SIMPLE_TYPE(STBTulsaATU, STB_TULSA_ATU)
OBJECT_DECLARE_SIMPLE_TYPE(STBTulsaV2200, STB_TULSA_V2200)
OBJECT_DECLARE_SIMPLE_TYPE(STBTulsaSAA7146, STB_TULSA_SAA7146)
OBJECT_DECLARE_SIMPLE_TYPE(STBTulsa, STB_TULSA)

/*
 * How long the board takes to answer a stream command.
 *
 * It must not be zero.  The firmware path is MU HISR -> CntlQ -> the Nucleus
 * `Control` task -> the stream state machine -> ring (sections 14, 15.1,
 * 47.2), so the notify always lands well after the host's SendCommand() has
 * returned.  Answering inside the host's own store to IMR0 instead -- which
 * is what a synchronous model does by default -- delivers the interrupt
 * re-entrantly, and STBTCtl.dll's state machine then runs against a channel
 * the command wrapper has not finished setting up.  Observed: Hollywood died
 * with eip=00000004 in __threadstart@4, a worker thread launched with a
 * procedure address of 4.
 *
 * 10 ms is comfortably longer than the host's return path and far inside
 * both the 100 ms SendCommand timeout and the 500 ms `Load Timeout`.
 */
#define STB_CHAN_NOTIFY_DELAY_NS  (10 * 1000 * 1000)
#define STB_CHAN_NOTIFY_QDEPTH    8

typedef struct STBTulsaChannel {
    bool     active;
    uint32_t state;
    uint32_t handle;        /* the host's stream handle, echoed on notify */
    uint32_t buffers_submitted;
    uint32_t buffers_retired;
    /* Deferred notification, so the board never answers instantaneously. */
    struct STBTulsaATU *atu;
    uint32_t index;
    /*
     * A short FIFO, because Play() produces two notifications in a row
     * (accepted, then decoding) and the host must see them as separate
     * transitions - posting only the second would leave the DLL's state
     * machine in 6 and posting them together would collapse into one.
     */
    uint32_t notify_q[STB_CHAN_NOTIFY_QDEPTH];
    unsigned notify_head;
    unsigned notify_tail;
    QEMUTimer *notify_timer;
} STBTulsaChannel;

struct STBTulsaATU {
    PCIDevice parent_obj;

    MemoryRegion bar0;      /* 64 KiB container                      */
    MemoryRegion shmem;     /* RAM backing the whole aperture        */
    MemoryRegion mu;        /* MMIO messaging unit, overlaid at 0    */
    MemoryRegion shmtrace;  /* debug: traps + traces every aperture access */
    uint8_t     *shm;       /* board DRAM; the aperture is a window into it */
    uint8_t     *dram;      /* owning pointer for the above                 */

    /* Messaging unit. */
    uint32_t imr[2];
    uint32_t omr[2];
    uint32_t idr, iisr, iimr;
    uint32_t odr, oisr, oimr;

    /*
     * IMR0 is not a doorbell: STBTCtl.dll uses it to select which part of the
     * i960's local address space the aperture above the MU registers looks at,
     * and spins until the i960 echoes the value back in OMR1.  See
     * sub_100016FE in STBTCtl.dll.
     */
    uint32_t        window;
    bool            window_valid;
    /*
     * Set when the host writes the firmware entry point to IMR1.  Before
     * that, IMR0 is the window-select register; after it, the running i960
     * firmware uses IMR0 as the mailbox doorbell and clears it on
     * completion.  See TULSA.md section 45.
     */
    bool            fw_started;
    uint64_t        last_code_pc;   /* last guest PC whose bytes were dumped */
    QEMUTimer      *field_timer;    /* NTSC field clock, drives the vertical */
    int64_t        field_deadline; /* absolute, so the clock cannot drift     */
    uint32_t        vertical;       /* board field counter published at +0x158 */

    /* Modelled i960 state. */
    uint8_t         status;         /* STBBoardStatus */
    bool            configured;
    STBTulsaConfig  config;
    /*
     * Everything the device-context commands have applied.  Nothing consumes
     * these yet; latching them is what turns a blind ACK into a record of
     * what the host actually asked the board to do, which is the only way to
     * find out what the three 3-element arrays are.
     */
    struct {
        uint32_t audio_delay, audio_source, line21_passthru;
        uint32_t video_mode, disable_genlock, genlock_source;
        uint32_t audio_volume[3], video_source[3];
        int32_t  audio_phase[3];
        bool     bypass;              /* cmd 17, EnableBypass */
    }               ctx;
    STBTulsaChannel chan[STB_TULSA_CHANNELS];
    uint32_t        commands_seen;
    uint32_t        last_command;

    /* Properties. */
    uint32_t dram_size;
    uint32_t notify_delay_ms;     /* board latency before a stream notify */
    bool     vertical_doorbell;   /* ring the per-field ODR bits (68.7)   */
};

/*
 * Host wall-clock accounting, in buckets.  Measured on
 * QEMU_CLOCK_REALTIME rather than VIRTUAL, because the question is how
 * much host time was spent, not how much guest time passed - and under
 * -icount those are deliberately unrelated.
 */
enum {
    STB_PROF_SUBMIT,    /* the MMIO write itself: pure guest stall */
    STB_PROF_LIST,      /* a display list, now off the vCPU thread */
    STB_PROF_FIELD,     /* the field timer, in the main loop   */
    STB_PROF_COMPOSITE, /* the mixer and console upload        */
    STB_PROF_REG,       /* every other MMIO access             */
    STB_PROF_MAX,
};

typedef struct STBTulsaProf {
    uint64_t ns;        /* total host time in this window      */
    uint64_t max_ns;    /* the worst single call in it         */
    uint64_t calls;
    uint64_t ns_total;  /* and the same since reset, unwindowed */
    uint64_t calls_total;
} STBTulsaProf;

struct STBTulsaV2200 {
    PCIDevice parent_obj;

    MemoryRegion mem;
    MemoryRegion reg;
    MemoryRegion cmdptr;    /* submit/retire pair, overlaid on mem */
    MemoryRegion ctrltrace; /* tracing overlay over the microcode's data */
    uint64_t     last_mem_pc;
    bool         mmio_trace;
    /*
     * Mixer polarity on the primary.  Section 64 read it off the SDK
     * preset, where `sde.bInvertAlpha = TRUE` makes an alpha of 0xff mean
     * *show video*; Hollywood's own driver blits every guide surface with
     * alpha 0xff, which under that rule keys the whole frame to a black YUV
     * input.  The polarity is a property of how the host set the primary up,
     * not of the card, so it is a knob rather than a constant.
     */
    bool         alpha_invert;
    QemuConsole *con;              /* the card's own output, section 52.3 */
    struct STBTulsaSAA7146 *saa;   /* so a capture slot can report its size */
    uint32_t     frame_base[2];    /* SetFrameBase: even and odd field    */
    uint32_t     shown_base;       /* what the surface currently points at */
    uint32_t     shown_stride;     /* and at what stride                   */
    /*
     * Accumulated damage for the console, in display coordinates, empty
     * when x1 <= x0 (section 87.7).  The mixer used to walk all 345,600
     * pixels and re-upload the whole surface on every refresh whether or
     * not anything had moved; the blits and fills that write a displayed
     * framebuffer record their destination rectangles here instead.
     */
    uint32_t     dirty_x0, dirty_y0, dirty_x1, dirty_y1;
    /*
     * The video mixer (section 2's block diagram, section 60.3).  The RGB
     * processor's framebuffer is not the card's output: it is one input to a
     * mixer whose other input is the YUV processor and whose key is the RGB
     * surface's own alpha channel, per pixel.  So the console shows a
     * composite built here rather than Verite memory directly, and the RGB
     * instance needs a way to reach the YUV instance's framebuffer.
     */
    struct STBTulsaV2200 *peer;    /* the other processor, RGB <-> YUV     */
    uint8_t     *mix;              /* composited output behind the console */
    struct {
        uint32_t addr;             /* SCROLLSTATE block in Verite memory   */
        uint32_t extent;           /* wrap modulus: source w or h          */
    }            scroll[STB_V2200_MAX_SCROLL];
    int          n_scroll;
    QEMUTimer   *field_timer;      /* advances the scroll, section 25       */
    int64_t     field_deadline;   /* absolute, so the clock cannot drift   */
    uint8_t     *vram;
    char        *role;

    uint8_t  regfile[STB_V2200_REG_SIZE];   /* the register aperture, as latches */
    uint32_t risc_steps;    /* instructions force-fed into the RISC core */
    uint32_t rf[STB_V2200_RF_REGS];     /* the core's register file   */
    uint32_t risc_pc;                   /* last jump target           */
    uint8_t  rf_sel;                    /* register selected for readRF */
    /*
     * The low-level cores.  `lle` picks between the HLE display-list
     * interpreter above - which reads the slots in C and is what has been
     * driving the console - and running the card's own microcode through
     * the V2200 RISC interpreter.  HLE is the default because it works;
     * the LLE path is the one that will eventually make the HLE path
     * unnecessary.
     */
    bool         lle;
    bool         lle_running;      /* the microcode has been entered      */

    /*
     * Where the host's time goes (section 146).  Nothing here runs in
     * parallel: a display list executes inside the vCPU thread, with the
     * guest stopped on the store that submitted it, while the field tick
     * and the mixer composite run in the main loop - and all three hold
     * the BQL, so each one blocks the other two.  These buckets are the
     * only way to tell which of them is eating a field.
     */
    STBTulsaProf prof[STB_PROF_MAX];
    int64_t      prof_window;      /* start of the current report window  */

    /*
     * Submitted lists waiting to run.  The board executes a list on its
     * own RISC and only then stores its address to the retire pointer at
     * 0x6CFC (section 21), and the driver picks its next buffer by
     * polling for one that is neither current nor active - so running
     * the list out of line and retiring it afterwards is what the
     * hardware does, not a liberty taken for speed.  Section 148.
     */
    uint32_t     pending[STB_V2200_PENDING];
    unsigned     pend_head, pend_tail;
    QEMUBH      *list_bh;
    V2200Core    core;
    V2200Pix     pix;

    uint32_t cmd_current;   /* shadow of the submit doorbell  */
    uint32_t cmd_active;    /* shadow of the retire pointer   */
    uint32_t lists_submitted;
};

struct STBTulsaSAA7146 {
    PCIDevice parent_obj;

    MemoryRegion reg;
    uint8_t  board_id;
    uint32_t regs[STB_SAA7146_REG_SIZE / 4];

    /*
     * The frame source.  `video-file` is read as raw UYVY 720x480 frames -
     * a plain file is looped, a FIFO or socket is read as it arrives, which
     * is how a live decode is piped in:
     *
     *   ffmpeg -i feed.ts -pix_fmt uyvy422 -s 720x480 -f rawvideo feed.uyvy
     *
     * Reads are non-blocking and a partial frame is carried across ticks, so
     * a producer that falls behind stalls the picture rather than the VM.
     */
    char            *video_file;    /* raw UYVY frames, looped            */
    char            *video_listen;  /* "host:port" - one producer at a time */
    int              video_fd;      /* the live source, file or socket     */
    int              video_listen_fd;
    bool             video_is_pipe;
    bool             video_is_socket;   /* a SOCKET, not a CRT fd: use recv() */
    uint32_t         mc1;            /* MC1 after mask/value resolution     */
    uint32_t         cap_w, cap_h;   /* what the display list reads back    */
    /*
     * The geometry the capture buffer was last painted with.  Section 99:
     * the buffer is wider than the picture written into it, and nothing used
     * to clear the difference - so the untouched margin stayed all-zero
     * (saturated green once blitted) and, after the window narrowed, held
     * bars from the previous, wider geometry.  Tracking the last geometry
     * lets the whole extent be reset to UYVY black the moment it changes.
     */
    uint32_t         last_w, last_h, last_pitch, last_odd, last_even;
    uint8_t         *frame;
    uint32_t         fill;
    uint64_t         frames_in;
    QEMUTimer       *frame_timer;
    int64_t          frame_deadline;
    STBTulsaV2200   *yuv;        /* where the captured frame lands */
};

struct STBTulsa {
    PCIBridge parent_obj;


    STBTulsaATU     atu;
    STBTulsaSAA7146 saa;
    STBTulsaV2200   rgb;
    STBTulsaV2200   yuv;

    uint8_t  board_id;
    uint32_t dram_size;
    uint32_t notify_delay_ms;
    bool     vertical_doorbell;
    bool     lle;                 /* propagated to both V2200 children    */
    char    *video_file;        /* forwarded to the SAA7146, section 82 */
    char    *video_listen;      /* likewise, the live socket             */
};

#endif /* HW_DISPLAY_STB_TULSA_H */
