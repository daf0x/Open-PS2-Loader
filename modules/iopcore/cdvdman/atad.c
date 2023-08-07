/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# Copyright (c) 2003 Marcus R. Brown <mrbrown@0xd6.org>
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
#
# $Id$
# ATA device driver.
# This module provides the low-level ATA support for hard disk drives.  It is
# 100% compatible with its proprietary counterpart called atad.irx.
#
# This module also include support for 48-bit feature set (done by Clement).
# To avoid causing an "emergency park" for some HDDs, shutdown callback 15 of dev9
# is used for issuing the STANDBY IMMEDIATE command prior to DEV9 getting shut down.
*/

#include <types.h>
#include <defs.h>
#include <irx.h>
#include <loadcore.h>
#include <thbase.h>
#include <thevent.h>
#include <thsemap.h>
#include <stdio.h>
#include <sysclib.h>
#include <dev9.h>
#include "atad.h"

#include <speedregs.h>
#include <atahw.h>

//#define NETLOG_DEBUG

#ifdef NETLOG_DEBUG
// !!! netlog exports functions pointers !!!
extern int (*pNetlogSend)(const char *format, ...);
extern int netlog_inited;
#endif

#ifdef DEV9_DEBUG
#define M_PRINTF(format, args...) \
    printf(MODNAME ": " format, ##args)
#else
#define M_PRINTF(format, args...) \
    do {                          \
    } while (0)
#endif

#define BANNER  "ATA device driver %s - Copyright (c) 2003 Marcus R. Brown\n"
#define VERSION "v1.2"

extern char lba_48bit;

static int ata_evflg = -1;

int ata_io_sema = -1;

#define WAITIOSEMA(x)   WaitSema(x)
#define SIGNALIOSEMA(x) SignalSema(x)

#define ATA_EV_TIMEOUT  1
#define ATA_EV_COMPLETE 2

/* Local device info.  */
static ata_devinfo_t atad_devinfo;

/* ATA command info.  */
typedef struct _ata_cmd_info
{
    u8 command;
    u8 type;
} ata_cmd_info_t;

#define ata_cmd_command_mask 0x1f
#define ata_cmd_command_bits(x) ((x) & ata_cmd_command_mask)
#define ata_cmd_flag_write_twice 0x80
#define ata_cmd_flag_use_timeout 0x40
#define ata_cmd_flag_dir         0x20
#define ata_cmd_flag_is_set(x, y) ((x) & (y))
static const ata_cmd_info_t ata_cmd_table[] =
{
      { ATA_C_READ_DMA              , 0x04 | ata_cmd_flag_use_timeout                                               }
    , { ATA_C_IDENTIFY_DEVICE       , 0x02                                                                          }
    , { ATA_C_IDENTIFY_PACKET_DEVICE, 0x02                                                                          }
    , { ATA_C_SET_FEATURES          , 0x01 | ata_cmd_flag_use_timeout                                               }
    , { ATA_C_READ_DMA_EXT          , 0x04 | ata_cmd_flag_use_timeout                    | ata_cmd_flag_write_twice }
    , { ATA_C_WRITE_DMA             , 0x04 | ata_cmd_flag_use_timeout | ata_cmd_flag_dir                            }
    , { ATA_C_IDLE                  , 0x01 | ata_cmd_flag_use_timeout                                               }
    , { ATA_C_WRITE_DMA_EXT         , 0x04 | ata_cmd_flag_use_timeout | ata_cmd_flag_dir | ata_cmd_flag_write_twice }
    , { ATA_C_STANDBY_IMMEDIATE     , 0x01 | ata_cmd_flag_use_timeout                                               }
    , { ATA_C_FLUSH_CACHE           , 0x01 | ata_cmd_flag_use_timeout                                               }
    , { ATA_C_STANDBY_IMMEDIATE     , 0x01 | ata_cmd_flag_use_timeout                                               }
    , { ATA_C_FLUSH_CACHE_EXT       , 0x01 | ata_cmd_flag_use_timeout                                               }
};
#define ATA_CMD_TABLE_SIZE (sizeof ata_cmd_table / sizeof(ata_cmd_info_t))

/* This is the state info tracked between ata_io_start() and ata_io_finish().  */
struct
{
    union
    {
        void* buf;
        u8*   buf8;
        u16*  buf16;
    };
    u16 blkcount; /* The number of 512-byte blocks (sectors) to transfer.  */
    u8  type;     /* The ata_cmd_info_t type field. */
    u8  dir;      /* DMA direction: 0 - to RAM, 1 - from RAM.  */
} static atad_cmd_state;
_Static_assert(sizeof(atad_cmd_state) == 8);

static int ata_intr_cb(int flag);
static unsigned int ata_alarm_cb(void *unused);

static void ata_set_dir(int dir);
static void ata_shutdown_cb(void);

#ifndef ATA_GAMESTAR_WORKAROUND
/* In v1.04, DMA was enabled in ata_set_dir() instead. */
static void ata_pre_dma_cb(int bcr, int dir)
{
    USE_SPD_REGS;

    SPD_REG16(SPD_R_XFR_CTRL) |= 0x80;
}

static void ata_post_dma_cb(int bcr, int dir)
{
    USE_SPD_REGS;

    SPD_REG16(SPD_R_XFR_CTRL) &= ~0x80;
}
#endif // ATA_GAMESTAR_WORKAROUND

static int ata_create_event_flag(void)
{
    iop_event_t event;

    /* In v1.04, EA_MULTI was specified. */
    event.attr = EA_SINGLE;
    event.bits = 0;
    return CreateEventFlag(&event);
}

int atad_start(void)
{
#ifdef DEV9_DEBUG
    USE_SPD_REGS;
#endif
    int res = 1;

    M_PRINTF(BANNER, VERSION);

#ifdef DEV9_DEBUG
    if (!(SPD_REG16(SPD_R_REV_3) & SPD_CAPS_ATA) || !(SPD_REG16(SPD_R_REV_8) & 0x02)) {
        M_PRINTF("HDD is not connected, exiting.\n");
        goto out;
    }
#endif

    if ((ata_evflg = ata_create_event_flag()) < 0) {
        M_PRINTF("Couldn't create event flag, exiting.\n");
        res = 1;
        goto out;
    }

    /* In v1.04, PIO mode 0 was set here. In late versions, it is set in ata_init_devices(). */
    dev9RegisterIntrCb(1, &ata_intr_cb);
    dev9RegisterIntrCb(0, &ata_intr_cb);
    #ifndef ATA_GAMESTAR_WORKAROUND
    dev9RegisterPreDmaCb(0, &ata_pre_dma_cb);
    dev9RegisterPostDmaCb(0, &ata_post_dma_cb);
    #endif // ATA_GAMESTAR_WORKAROUND
    /* Register this at the last position, as it should be the last thing done before shutdown. */
    dev9RegisterShutdownCb(15, &ata_shutdown_cb);

    iop_sema_t smp;
    smp.initial = 1;
    smp.max = 1;
    smp.option = 0;
    smp.attr = SA_THPRI;
    ata_io_sema = CreateSema(&smp);

    res = 0;
    M_PRINTF("Driver loaded.\n");
out:
    return res;
}

static int ata_intr_cb(int flag)
{
    if (flag != 1) { /* New card, invalidate device info.  */
        dev9IntrDisable(SPD_INTR_ATA);
        iSetEventFlag(ata_evflg, ATA_EV_COMPLETE);
    }

    return 1;
}

static unsigned int ata_alarm_cb(void *unused)
{
    iSetEventFlag(ata_evflg, ATA_EV_TIMEOUT);
    return 0;
}

/* Export 8 */
int ata_get_error(void)
{
    USE_ATA_REGS;
    return ata_hwport->r_error & 0xff;
}

/**
 * In the original ATAD, the busy and bus-busy functions were separate, but similar.
 */
#define ATA_WAIT_BUSY    0x80
#define ATA_WAIT_BUSBUSY 0x88

#define ata_wait_busy()     gen_ata_wait_busy(ATA_WAIT_BUSY)
#define ata_wait_bus_busy() gen_ata_wait_busy(ATA_WAIT_BUSBUSY)

static int gen_ata_wait_busy(int bits) {
    USE_ATA_REGS;

    for(unsigned i = 0; i < 56; ++i) {
        if(!(ata_hwport->r_control & bits)) {
            return 0;
        }

        unsigned delay = (i>>3) << (i>>2) << 5;
        if(delay) {
            DelayThread(delay);
        }
    }

    M_PRINTF("Timeout while waiting on busy (0x%02x).\n", bits);
    return ATA_RES_ERR_TIMEOUT;
}

static int ata_device_select(int device)
{
    USE_ATA_REGS;
    int res;

    if ((res = ata_wait_bus_busy()) < 0)
        return res;

    /* If the device was already selected, nothing to do.  */
    if (((ata_hwport->r_select >> 4) & 1) == device)
        return 0;

    /* Select the device.  */
    ata_hwport->r_select = (device & 1) << 4;
    (void)(ata_hwport->r_control);
    (void)(ata_hwport->r_control); //Only done once in v1.04.

    return ata_wait_bus_busy();
}

static unsigned find_ata_cmd(u16 command)
{
    unsigned result = 0;
    for (int i = 0; i < ATA_CMD_TABLE_SIZE; ++i) {
        if ((u8)command == ata_cmd_table[i].command) {
            result = ata_cmd_table[i].type;
        }
    }
    return result;
}

/* Export 6 */
/*
	28-bit LBA:
		sector	(7:0)	-> LBA (7:0)
		lcyl	(7:0)	-> LBA (15:8)
		hcyl	(7:0)	-> LBA (23:16)
		device	(3:0)	-> LBA (27:24)

	48-bit LBA just involves writing the upper 24 bits in the format above into each respective register on the first write pass, before writing the lower 24 bits in the 2nd write pass. The LBA bits within the device field are not used in either write pass.
*/
int ata_io_start(void *buf, u32 blkcount, u16 feature, u16 nsector, u16 sector, u16 lcyl, u16 hcyl, u16 select, u16 command)
{
    USE_ATA_REGS;
    int res;
    int device = (select >> 4) & 1;

    ClearEventFlag(ata_evflg, 0);

    if ((res = ata_device_select(device)) != 0)
        return res;

    const unsigned type = find_ata_cmd(command);
    if (!(atad_cmd_state.type = ata_cmd_command_bits(type))) //Non-SONY: ignore the 48-bit LBA flag.
        return ATA_RES_ERR_NOTREADY;

    atad_cmd_state.buf = buf;
    atad_cmd_state.blkcount = blkcount;
    atad_cmd_state.dir = !!ata_cmd_flag_is_set(type, ata_cmd_flag_dir);

    /* Check that the device is ready if this the appropiate command.  */
    if (!(ata_hwport->r_control & 0x40)) {
        switch (command) {
            case ATA_C_DEVICE_RESET:
            case ATA_C_EXECUTE_DEVICE_DIAGNOSTIC:
            case ATA_C_INITIALIZE_DEVICE_PARAMETERS:
            case ATA_C_PACKET:
            case ATA_C_IDENTIFY_PACKET_DEVICE:
                break;
            default:
                M_PRINTF("Error: Device %d is not ready.\n", device);
                return ATA_RES_ERR_NOTREADY;
        }
    }

    if (ata_cmd_flag_is_set(type, ata_cmd_flag_use_timeout)) {
        iop_sys_clock_t cmd_timeout;
        cmd_timeout.lo = 0x41eb0000;
        cmd_timeout.hi = 0;

        if ((res = SetAlarm(&cmd_timeout, &ata_alarm_cb, NULL)) < 0)
            return res;
    }

    /* Enable the command completion interrupt.  */
    if (ata_cmd_command_bits(type) == 1)
        dev9IntrEnable(SPD_INTR_ATA0);

    /* Finally!  We send off the ATA command with arguments.  */
    ata_hwport->r_control = !ata_cmd_flag_is_set(type, ata_cmd_flag_use_timeout) << 1;

    // 48-bit LBA requires writing to the address registers twice, 24 bits of
    // the LBA address is written each time. Writing to registers twice does not
    // affect 28-bit LBA since only the latest data stored in address registers
    // is used.
    //
    // For the sake of achieving (greatly) improved performance, write the
    // registers twice only if required! This is also required for compatibility
    // with the buggy firmware of certain PSX units.
    if (ata_cmd_flag_is_set(type, ata_cmd_flag_write_twice)) {
        ata_hwport->r_feature = (feature >> 8) & 0xff;
        ata_hwport->r_nsector = (nsector >> 8) & 0xff;
        ata_hwport->r_sector = (sector >> 8) & 0xff;
        ata_hwport->r_lcyl = (lcyl >> 8) & 0xff;
        ata_hwport->r_hcyl = (hcyl >> 8) & 0xff;
    }

    ata_hwport->r_feature = feature & 0xff;
    ata_hwport->r_nsector = nsector & 0xff;
    ata_hwport->r_sector = sector & 0xff;
    ata_hwport->r_lcyl = lcyl & 0xff;
    ata_hwport->r_hcyl = hcyl & 0xff;
    ata_hwport->r_select = (select | ATA_SEL_LBA) & 0xff; //In v1.04, LBA was enabled in the ata_device_sector_io function.
    ata_hwport->r_command = command & 0xff;

    /* Turn on the LED.  */
    dev9LEDCtl(1);

    return 0;
}

/* Complete a DMA transfer, to or from the device.  */
static inline int ata_dma_complete(void *buf, int blkcount, int dir)
{
    USE_ATA_REGS;
    USE_SPD_REGS;
    u32 count, nbytes;
    u32 bits;
    int i, res;
    u16 dma_stat;

    while (blkcount) {
        for (i = 0; i < 20; i++)
            if ((dma_stat = SPD_REG16(0x38) & 0x1f))
                goto next_transfer;

        if (dma_stat)
            goto next_transfer;

        dev9IntrEnable(SPD_INTR_ATA);
        /* Wait for the previous transfer to complete or a timeout.  */
        WaitEventFlag(ata_evflg, ATA_EV_TIMEOUT | ATA_EV_COMPLETE, WEF_CLEAR | WEF_OR, &bits);

        if (bits & ATA_EV_TIMEOUT) { /* Timeout.  */
            M_PRINTF("Error: DMA timeout.\n");
            return ATA_RES_ERR_TIMEOUT;
        }
        /* No DMA completion bit? Spurious interrupt.  */
        if (!(SPD_REG16(SPD_R_INTR_STAT) & 0x02)) {
            if (ata_hwport->r_control & 0x01) {
                M_PRINTF("Error: Command error while doing DMA.\n");
                M_PRINTF("Error: Command error status 0x%02x, error 0x%02x.\n", ata_hwport->r_status, ata_get_error());
#ifdef NETLOG_DEBUG
                pNetlogSend("Error: Command error status 0x%02x, error 0x%02x.\n", ata_hwport->r_status, ata_get_error());
#endif
                /* In v1.04, there was no check for ICRC. */
                return ((ata_get_error() & ATA_ERR_ICRC) ? ATA_RES_ERR_ICRC : ATA_RES_ERR_IO);
            } else {
                M_PRINTF("Warning: Got command interrupt, but not an error.\n");
                continue;
            }
        }

        dma_stat = SPD_REG16(0x38) & 0x1f;

    next_transfer:
        count = (blkcount < dma_stat) ? blkcount : dma_stat;
        nbytes = count * 512;
        if ((res = dev9DmaTransfer(0, buf, (nbytes << 9) | 32, dir)) < 0)
            return res;

        buf = (void *)((u8 *)buf + nbytes);
        blkcount -= count;
    }

    return 0;
}

/* Export 7 */
int ata_io_finish(void)
{
    USE_SPD_REGS;
    USE_ATA_REGS;
    u32 bits;
    int i, res = 0, type = atad_cmd_state.type;
    unsigned short int stat;

    if (type == 1) { /* Non-data commands.  */
        WaitEventFlag(ata_evflg, ATA_EV_TIMEOUT | ATA_EV_COMPLETE, WEF_CLEAR | WEF_OR, &bits);
        if (bits & ATA_EV_TIMEOUT) { /* Timeout.  */
            M_PRINTF("Error: ATA timeout on a non-data command.\n");
            return ATA_RES_ERR_TIMEOUT;
        }
    } else if (type == 4) { /* DMA.  */
        if ((res = ata_dma_complete(
            atad_cmd_state.buf,
            atad_cmd_state.blkcount,
            atad_cmd_state.dir)) < 0)
            goto finish;

        for (i = 0; i < 100; i++)
            if ((stat = SPD_REG16(SPD_R_INTR_STAT) & 0x01))
                break;
        if (!stat) {
            dev9IntrEnable(SPD_INTR_ATA0);
            WaitEventFlag(ata_evflg, ATA_EV_TIMEOUT | ATA_EV_COMPLETE, WEF_CLEAR | WEF_OR, &bits);
            if (bits & ATA_EV_TIMEOUT) {
                M_PRINTF("Error: ATA timeout on DMA completion.\n");
                res = ATA_RES_ERR_TIMEOUT;
            }
        }
    } else { /* PIO transfers.  */
        stat = ata_hwport->r_control;
        if ((res = ata_wait_busy()) < 0)
            goto finish;

        /* Transfer each PIO data block.  */
        while (--atad_cmd_state.blkcount != -1) {
            SPD_REG8(SPD_R_PIO_DATA) = 0;
            if ((res = ata_wait_busy()) < 0)
                goto finish;
        }
    }

    if (res)
        goto finish;

    /* Wait until the device isn't busy.  */
    if (ata_hwport->r_status & ATA_STAT_BUSY)
        res = ata_wait_busy();
    if ((stat = ata_hwport->r_status) & ATA_STAT_ERR) {
        M_PRINTF("Error: Command error: status 0x%02x, error 0x%02x.\n", stat, ata_get_error());
        /* In v1.04, there was no check for ICRC. */
        res = (ata_get_error() & ATA_ERR_ICRC) ? ATA_RES_ERR_ICRC : ATA_RES_ERR_IO;
    }

finish:
    /* The command has completed (with an error or not), so clean things up.  */
    CancelAlarm(&ata_alarm_cb, NULL);
    /* Turn off the LED.  */
    dev9LEDCtl(0);

    if (res)
        M_PRINTF("error: ATA failed, %d\n", res);

    return res;
}

/* Export 17 */
int ata_device_flush_cache(int device)
{
    int res;

    if (!(res = ata_io_start(NULL, 1, 0, 0, 0, 0, 0, (device << 4) & 0xffff, lba_48bit ? ATA_C_FLUSH_CACHE_EXT : ATA_C_FLUSH_CACHE)))
        res = ata_io_finish();

    return res;
}

/* Export 9 */
/* Note: this can only support DMA modes, due to the commands issued. */
int ata_device_sector_io(int device, void *buf, u32 lba, u32 nsectors, int dir)
{
    USE_SPD_REGS;
    int res = 0, retries;
    u16 sector, lcyl, hcyl, select, command, len;

    WAITIOSEMA(ata_io_sema);

    while (res == 0 && nsectors > 0) {
        /* Variable lba is only 32 bits so no change for lcyl and hcyl.  */
        lcyl = (lba >> 8) & 0xff;
        hcyl = (lba >> 16) & 0xff;

        if (lba_48bit) {
            /* Setup for 48-bit LBA. */
            len = (nsectors > 65536) ? 65536 : nsectors;

            /* Combine bits 24-31 and bits 0-7 of lba into sector.  */
            sector = ((lba >> 16) & 0xff00) | (lba & 0xff);
            /* In v1.04, LBA was enabled here.  */
            select = (device << 4) & 0xffff;
            command = (dir == 1) ? ATA_C_WRITE_DMA_EXT : ATA_C_READ_DMA_EXT;
        } else {
            /* Setup for 28-bit LBA.  */
            len = (nsectors > 256) ? 256 : nsectors;
            sector = lba & 0xff;
            /* In v1.04, LBA was enabled here.  */
            select = ((device << 4) | ((lba >> 24) & 0xf)) & 0xffff;
            command = (dir == 1) ? ATA_C_WRITE_DMA : ATA_C_READ_DMA;
        }

        for (retries = 3; retries > 0; retries--) {
            #ifdef ATA_GAMESTAR_WORKAROUND
            /* Due to the retry loop, put this call (for the GameStar workaround) here instead of the old location. */
            ata_set_dir(dir);
            #endif // ATA_GAMESTAR_WORKAROUND

            if ((res = ata_io_start(buf, len, 0, len, sector, lcyl, hcyl, select, command)) != 0)
                break;

            /* Set up (part of) the transfer here. In v1.04, this was called at the top of the outer loop. */
            #ifndef ATA_GAMESTAR_WORKAROUND
            ata_set_dir(dir);
            #endif // ATA_GAMESTAR_WORKAROUND

            res = ata_io_finish();

            /* In v1.04, this was not done. Neither was there a mechanism to retry if a non-permanent error occurs. */
            SPD_REG16(SPD_R_IF_CTRL) &= ~SPD_IF_DMA_ENABLE;

            if (res != ATA_RES_ERR_ICRC)
                break;
        }

        buf = (void *)((u8 *)buf + len * 512);
        lba += len;
        nsectors -= len;
    }

    SIGNALIOSEMA(ata_io_sema);

    return res;
}

/* Export 4 */
ata_devinfo_t *ata_get_devinfo(int device)
{
    return &atad_devinfo;
}

static void ata_set_dir(int dir)
{
    USE_SPD_REGS;
    unsigned short int val;

    SPD_REG16(0x38) = 3;
    val = SPD_REG16(SPD_R_IF_CTRL) & 1;
    val |= (dir == ATA_DIR_WRITE) ? 0x4c : 0x4e;
    SPD_REG16(SPD_R_IF_CTRL) = val;
    #ifdef ATA_GAMESTAR_WORKAROUND
    SPD_REG16(SPD_R_XFR_CTRL) = dir | 0x86;
    #else // ATA_GAMESTAR_WORKAROUND
    SPD_REG16(SPD_R_XFR_CTRL) = dir | 0x06; //In v1.04, DMA was enabled here (0x86 instead of 0x6)
    #endif // ATA_GAMESTAR_WORKAROUND
}

static int ata_device_standby_immediate(int device)
{
    int res;

    if (!(res = ata_io_start(NULL, 1, 0, 0, 0, 0, 0, (device << 4) & 0xFFFF, ATA_C_STANDBY_IMMEDIATE)))
        res = ata_io_finish();

    return res;
}

static void ata_shutdown_cb(void)
{
    if (atad_devinfo.exists)
        ata_device_standby_immediate(0);
}
