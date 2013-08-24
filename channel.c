/* CHANNEL.C    (c) Copyright Roger Bowler, 1999-2012                */
/*              (c) Copyright Jan Jaeger,   1999-2012                */
/*              ESA/390 Channel Emulator                             */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */

/* z/Architecture support - (c) Copyright Jan Jaeger, 1999-2012      */

/*-------------------------------------------------------------------*/
/* This module contains the channel subsystem functions for the      */
/* Hercules S/370 and ESA/390 emulator.                              */
/*-------------------------------------------------------------------*/

/*-------------------------------------------------------------------*/
/* Additional credits:                                               */
/*      Measurement block support by Jan Jaeger                      */
/*      Fix program check on NOP due to addressing - Jan Jaeger      */
/*      Fix program check on TIC as first ccw on RSCH - Jan Jaeger   */
/*      Fix PCI intermediate status flags             - Jan Jaeger   */
/*      64-bit IDAW support - Roger Bowler v209                  @IWZ*/
/*      Incorrect-length-indication-suppression - Jan Jaeger         */
/*      Read backward support contributed by Hackules   13jun2002    */
/*      Read backward fixes contributed by Jay Jaeger   16sep2003    */
/*      MIDAW support - Roger Bowler                    03aug2005 @MW*/
/*-------------------------------------------------------------------*/

#include "hstdinc.h"

#define _CHANNEL_C_
#define _HENGINE_DLL_

#include "hercules.h"
#include "devtype.h"
#include "opcode.h"
#include "chsc.h"


/* Debug Switches ---------------------------------------------------*/

#if !defined(CHANNEL_DEBUG_SWITCHES)
#define CHANNEL_DEBUG_SWITCHES
#define DEBUG_SCSW              0
#define DEBUG_PREFETCH          0
#define DEBUG_DUMP              0
#endif

/*-------------------------------------------------------------------*/


/*-------------------------------------------------------------------*/
/* Internal function definitions                                     */
/*-------------------------------------------------------------------*/

void                call_execute_ccw_chain (int arch_mode, void* pDevBlk);
DLL_EXPORT  void*   device_thread (void *arg);
static int          schedule_ioq (const REGS* regs, DEVBLK* dev);
static INLINE void  subchannel_interrupt_queue_cleanup (DEVBLK*);
int                 test_subchan_locked (REGS*, DEVBLK*, IRB*, IOINT**, SCSW**);


#if !defined(CHANNEL_INLINES)
#define CHANNEL_INLINES
static INLINE void
set_subchannel_busy(DEVBLK* dev)
{
    dev->busy = 1;
    dev->scsw.flag3 |= SCSW3_AC_SCHAC | SCSW3_SC_INTER;
}

static INLINE void
set_device_busy(DEVBLK* dev)
{
    set_subchannel_busy(dev);
    dev->scsw.flag3 |= SCSW3_AC_DEVAC;
}

static INLINE void
clear_subchannel_busy_scsw(SCSW* scsw)
{
    scsw->flag3 &= ~(SCSW3_AC_SCHAC |
                     SCSW3_AC_DEVAC |
                     SCSW3_SC_INTER);
}

static INLINE void
clear_subchannel_busy(DEVBLK* dev)
{
    clear_subchannel_busy_scsw(&dev->scsw);
    dev->startpending = 0;
    dev->busy = 0;
}

static INLINE void
clear_device_busy_scsw(SCSW* scsw)
{
    scsw->flag3 &= ~SCSW3_AC_DEVAC;
}

static INLINE void
clear_device_busy(DEVBLK* dev)
{
    clear_device_busy_scsw(&dev->scsw);
}


#endif /*CHANNEL_INLINES*/


/*-------------------------------------------------------------------*/
/* Define I/O Buffer Structure and Inline Support Routines           */
/*-------------------------------------------------------------------*/
#if !defined(IOBUF_STRUCT)
#define IOBUF_STRUCT
typedef struct
{
    union
    {
        struct
        {
            u_int   size;               /* Buffer size               */
            BYTE    *start;             /* Start of data address     */
            BYTE    *end;               /* Last byte address         */

            /* Note: Remaining space from 4K page is reserved for
             *       debugging and future use. No mapping of the space
             *       is presently done.
             */
         };
        BYTE    pad[4096];              /* Pad to a 4K page boundary */
    };
    BYTE    data[65536];                /* Channel I/O buffer        */
} IOBUF;

static INLINE void
iobuf_initialize (IOBUF *iobuf, const u_int size)
{
    iobuf->size = size;
    iobuf->start = (BYTE *)iobuf->data;
    iobuf->end = (BYTE *)iobuf->start + (size - 1);
}

static INLINE IOBUF *
iobuf_create (u_int size)
{
    IOBUF *iobuf;
    size  = (MIN(size, 1048576) + 4095) & ~0x0FFF;
    iobuf = (IOBUF*)malloc_aligned(size + 4096, 4096);
    if (iobuf != NULL)
        iobuf_initialize(iobuf, size);
    return iobuf;
}

static INLINE u_int
iobuf_validate (IOBUF *iobuf)
{
    return
    ((iobuf == NULL                         ||
      iobuf->size < 65536                   ||
      iobuf->start != (BYTE *)iobuf->data   ||
      iobuf->end != (BYTE *)iobuf->start + (iobuf->size - 1 )) ?
     0 : 1);
}

static INLINE void
iobuf_destroy (IOBUF *iobuf)
{
    if (iobuf == NULL)
        return;
    if (!iobuf_validate(iobuf))
        raise(SIGSEGV);
    if (iobuf->size > 65536)
        free_aligned(iobuf);
}

static INLINE IOBUF *
iobuf_reallocate (IOBUF *iobuf, const u_int size)
{
    IOBUF *iobufnew;
    if (iobuf == NULL)
        return NULL;
    if (!iobuf_validate(iobuf))
        raise(SIGSEGV);
    iobufnew = iobuf_create(size);
    if (iobufnew == NULL)
        return NULL;
    memcpy(iobufnew->start, iobuf->start, MIN(iobuf->size, size));
    if (iobuf->size > 65536)
        free_aligned(iobuf);
    return iobufnew;
}
#endif


#if !defined(PREFETCH_STRUCT)
#define PREFETCH_STRUCT

/* Define 256 entries in the prefetch table */
#define PF_SIZE 256

/* IDAW Types */
enum PF_IDATYPE
{
   PF_NO_IDAW = 0,
   PF_IDAW1   = 1,
   PF_IDAW2   = 2,
   PF_MIDAW   = 3
};

typedef struct                          /* Prefetch data structure   */
{
    u_int   seq;                        /* Counter                   */
    u_int   pos;                        /* Highest valid position    */
    U32     reqcount;                   /* Requested count           */
    U8      prevcode;                   /* Previous CCW opcode       */
    U8      opcode;                     /* CCW opcode                */
    U16     reserved;
    BYTE    chanstat[PF_SIZE];          /* Channel status            */
    U32     ccwaddr[PF_SIZE];           /* CCW address (SCSW CCW)    */
    U32     datalen[PF_SIZE];           /* Count (length)            */
    RADR    dataaddr[PF_SIZE];          /* Address                   */
    U8      ccwflags[PF_SIZE];          /* CCW flags                 */
    U16     ccwcount[PF_SIZE];          /* Count (length)            */
    U32     idawaddr[PF_SIZE];          /* IDAW address              */
    U8      idawtype[PF_SIZE];          /* IDAW type                 */
    U8      idawflag[PF_SIZE];          /* IDAW flags                */
} PREFETCH;

#endif

#ifdef OPTION_IODELAY_KLUDGE
#define IODELAY(_dev) \
do { \
  if (sysblk.iodelay > 0 && (_dev)->devchar[10] == 0x20) \
    usleep(sysblk.iodelay); \
} while(0)
#else
#define IODELAY(_dev)
#endif

#undef CHADDRCHK
#if defined(FEATURE_ADDRESS_LIMIT_CHECKING)
#define CHADDRCHK(_addr,_dev)                   \
  (   ((_addr) > (_dev)->mainlim)               \
    || (((_dev)->orb.flag5 & ORB5_A)            \
      && ((((_dev)->pmcw.flag5 & PMCW5_LM_LOW)  \
        && ((_addr) < sysblk.addrlimval))       \
      || (((_dev)->pmcw.flag5 & PMCW5_LM_HIGH)  \
        && ((_addr) >= sysblk.addrlimval)) ) ))
#else /*!defined(FEATURE_ADDRESS_LIMIT_CHECKING)*/
#define CHADDRCHK(_addr,_dev) \
        ((_addr) > (_dev)->mainlim)
#endif /*!defined(FEATURE_ADDRESS_LIMIT_CHECKING)*/

#if !defined(_CHANNEL_C)
#define _CHANNEL_C


/*--------------------------------------------------------------------*/
/*  Inline routines for Conditions and Indications Cleared at the     */
/*  Subchannel by TEST SUBCHANNEL; use of the routines keep the       */
/*  logic flow through the table of conditions clean.                 */
/*                                                                    */
/*  SA22-7832-09, Figure 14-2, Conditions and Indications Cleared at  */
/*                the Subchannel by TEST SUBCHANNEL, p. 14-21.        */
/*                                                                    */
/*--------------------------------------------------------------------*/

static INLINE void
scsw_clear_n_C (SCSW* scsw)
{
    scsw->flag1 &= ~SCSW1_N;
}

static INLINE void
scsw_clear_q_C (SCSW* scsw)
{
    scsw->flag2 &= ~SCSW2_Q;
}

static INLINE void
scsw_clear_fc_C (SCSW* scsw)
{
    scsw->flag2 &= ~SCSW2_FC;
}

static INLINE void
scsw_clear_fc_Nc (SCSW* scsw)
{
    if (scsw->flag2 & SCSW2_FC_HALT &&
        scsw->flag3 & SCSW3_AC_SUSP)
        scsw_clear_fc_C(scsw);
}

static INLINE void
scsw_clear_ac_Cp (SCSW* scsw)
{
    scsw->flag2 &= ~(SCSW2_AC_RESUM |
                         SCSW2_AC_START |
                         SCSW2_AC_HALT  |
                         SCSW2_AC_CLEAR);
    scsw->flag3 &= ~SCSW3_AC_SUSP;
}

static INLINE void
scsw_clear_ac_Nr (SCSW* scsw)
{
    if (scsw->flag2 & SCSW2_FC_START &&
        scsw->flag3 & SCSW3_AC_SUSP)
    {
        if (scsw->flag2 & SCSW2_FC_HALT)
            scsw_clear_ac_Cp(scsw);
        else
        {
            scsw->flag2 &= ~SCSW2_AC_RESUM;
            scsw_clear_n_C(scsw);
        }
    }
}

static INLINE void
scsw_clear_sc_Cs (SCSW* scsw)
{
    scsw->flag3 &= ~(SCSW3_SC_ALERT |
                     SCSW3_SC_INTER |
                     SCSW3_SC_PRI   |
                     SCSW3_SC_SEC   |
                     SCSW3_SC_PEND);
}


/*--------------------------------------------------------------------*/
/*  AIPSX (dev)                                                       */
/*                                                                    */
/*  Generates the AIPSX indication for Figure 16-5, the Deferred-     */
/*  Condition-Code Meaning for Status-Pending Subchannel, Figure      */
/*  16-5.                                                             */
/*--------------------------------------------------------------------*/
static INLINE U8
AIPSX (const SCSW* scsw)
{
    U8  result =  (scsw->flag1 & SCSW1_A)           /* >> 4 << 4  */
               | ((scsw->flag1 & SCSW1_I) >> 2      /* >> 5 << 3  */
               |  (scsw->flag0 & SCSW0_S) >> 2)     /* >> 3 << 1  */
               | ((scsw->flag1 & SCSW1_P) >> 4)     /* >> 6 << 2  */
               |  (scsw->flag3 & SCSW3_SC_PEND);    /* >> 0 << 0  */
    return (result);
}


/*--------------------------------------------------------------------*/
/*  Queue I/O interrupt and update status (locked)                    */
/*                                                                    */
/*  Locks Held on Entry                                               */
/*    dev->lock                                                       */
/*    sysblk.intlock                                                  */
/*  Locks Held on Return                                              */
/*    dev->lock                                                       */
/*    sysblk.intlock                                                  */
/*  Locks Used                                                        */
/*    sysblk.iointqlk                                                 */
/*--------------------------------------------------------------------*/
static INLINE void
queue_io_interrupt_and_update_status_locked(DEVBLK* dev)
{
    /* Get the I/O interrupt queue lock */
    obtain_lock(&sysblk.iointqlk);

    /* Ensure the interrupt is queued/dequeued per pending flag */
    if (dev->scsw.flag3 & SCSW3_SC_PEND)
        QUEUE_IO_INTERRUPT_QLOCKED(&dev->ioint);
    else
        DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->ioint);

    /* Perform cleanup for DEVBLK flags being deprecated */
    subchannel_interrupt_queue_cleanup(dev);

    /* Update interrupts */
    UPDATE_IC_IOPENDING_QLOCKED();

    /* Release the I/O interrupt queue lock */
    release_lock(&sysblk.iointqlk);

    /* Wake up any waiters if the device isn't active or reserved */
    if (!(dev->scsw.flag3 & (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC)) &&
        !dev->reserved)
    {
        dev->ioactive = DEV_SYS_NONE;
        if (dev->iowaiters)
            signal_condition (&dev->iocond);
    }
}


/*--------------------------------------------------------------------*/
/*  Queue I/O interrupt and update status                             */
/*                                                                    */
/*  Locks Held on Entry                                               */
/*    None                                                            */
/*  Locks Held on Return                                              */
/*    None                                                            */
/*  Locks Used                                                        */
/*    dev->lock                                                       */
/*    sysblk.intlock                                                  */
/*    sysblk.iointqlk                                                 */
/*--------------------------------------------------------------------*/
static INLINE void
queue_io_interrupt_and_update_status(DEVBLK* dev)
{
    if (likely(dev->scsw.flag3 & SCSW3_SC_PEND))
    {
        OBTAIN_INTLOCK(NULL);
        obtain_lock(&dev->lock);
        queue_io_interrupt_and_update_status_locked(dev);

        /* Finish releasing locks */
        release_lock(&dev->lock);
        RELEASE_INTLOCK(NULL);
    }
    else    /* No interrupt pending */
    {
        /* Wake up any waiters if the device isn't active or reserved */
        if (!(dev->scsw.flag3 & (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC)) &&
            !dev->reserved)
        {
            obtain_lock(&dev->lock);
            dev->ioactive = DEV_SYS_NONE;
            if (dev->iowaiters)
                signal_condition (&dev->iocond);
            release_lock(&dev->lock);
        }
    }
}


/*-------------------------------------------------------------------*/
/* Copy memory backwards for READ BACKWARDS                          */
/*                                                                   */
/* Note: Routines exist that use READ BACKWARDS and PCI, processing  */
/*       data as copied. Consequently, memory copying must be done   */
/*       backwards to ensure that these routines have little chance  */
/*       of failure.                                                 */
/*                                                                   */
/*-------------------------------------------------------------------*/
static INLINE void
memcpy_backwards ( BYTE *to, BYTE *from, int length )
{
    if (length)
    {
        for (to += length, from += length; length; --length)
            *(--to) = *(--from);
    }
}

/*-------------------------------------------------------------------*/
/* FORMAT DATA                                                       */
/*-------------------------------------------------------------------*/
#if !defined(format_data)
#   define format_data(_buffer,_buflen,_data,_datalen)                 \
          _format_data((BYTE *)(_buffer),(u_int)(_buflen),             \
                       (BYTE *)(_data),(u_int)(_datalen))
#endif
static INLINE void
_format_data ( BYTE *buffer,  const u_int buflen,
               const BYTE *a, const u_int len )
{
u_int   i, k;                           /* Array subscripts          */
int     j;

    k = MIN(len, 16);

    if (k)
    {
        j = snprintf((char *)buffer, buflen,
                "%2.2X%2.2X%2.2X%2.2X %2.2X%2.2X%2.2X%2.2X "
                "%2.2X%2.2X%2.2X%2.2X %2.2X%2.2X%2.2X%2.2X",
                a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
                a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
        if (j < 0)
            *buffer = 0;
        else
        {
            /* Blank out unused data */
            if (k != 16)
            {
                i = (k << 1) + (k >> 2);
                memset(buffer + i, ' ', j - i);
            }

            /* If space is available, add translation */
            if ((u_int)j < buflen)
            {
                /* Insert blank separator */
                buffer[j++] = ' ';

                /* If space still available, complete translation */
                if ((u_int)j < buflen)
                {
                    /* Adjust data display size if necessary */
                    if ((j + k) > buflen)
                        k = buflen - j;

                    /* Append translation */
                    prt_guest_to_host(a, buffer + j, k);
                }
            }
        }

    }
    else
        *buffer = 0;

} /* end function format_data */


/*-------------------------------------------------------------------*/
/* FORMAT I/O BUFFER DATA                                            */
/*-------------------------------------------------------------------*/
static INLINE void
format_iobuf_data ( const RADR addr, BYTE *dest, const DEVBLK *dev,
                    const u_int len )
{
u_int   k;                              /* Array subscripts          */
BYTE    workarea[17];                   /* Character string work     */

    k = MIN(len, sizeof(workarea)-1);
    k = MIN(k, (addr < dev->mainlim) ? (dev->mainlim - addr) : 0);

    if (k)
    {
        memcpy(workarea, dev->mainstor + addr, k);
        memcpy(dest, "=>", 2);
        format_data(dest + 2, 52, workarea,  k);
    }
    else
        *dest = 0;

} /* end function format_iobuf_data */


/*-------------------------------------------------------------------*/
/* Dump data block                                                   */
/*-------------------------------------------------------------------*/
#define dump(_desc,_addr,_len)                                         \
       _dump((char *)(_desc),(BYTE *)(_addr),(u_int)(_len))
static void
_dump ( const char *description, BYTE *addr, const u_int len )
{
int     i;
int     k = MIN(len, 65536);
BYTE   *limit;
char    msgbuf[133];

    if (k)
    {
        /* Set limit */
        limit = addr + k;

        if (description &&
            description != NULL &&
            description[0])
            WRMSG(HHC90000, "I", description);
        for (i = 0; addr < limit; addr += 16, i += 16, k -= 16)
        {
            MSGBUF(msgbuf, "%4.4X  => ", i);
            format_data(msgbuf+9, sizeof(msgbuf)-9, addr,  k);
            WRMSG(HHC90000, "I", msgbuf);
        }
    }

} /* end function dump */


/*-------------------------------------------------------------------*/
/* Dump Storage                                                      */
/*-------------------------------------------------------------------*/
#define dump_storage(_desc,_addr,_len)                                 \
       _dump_storage                                                   \
       ((char *)(_desc),(RADR)(_addr),(u_int)(_len))
static void
_dump_storage ( const char *description,
                RADR addr, const u_int len )
{
u_int   k = MIN(MIN(len,
                    (addr < sysblk.mainsize) ?
                        (sysblk.mainsize - addr) : 0),
                65536);
BYTE   *storage;                        /* Real storage address      */
BYTE   *limit;                          /* Display limit             */
char    msgbuf[133];                    /* Message buffer            */

    /* Set length to fully reside within defined storage */
    k = MIN(len,
            (addr < sysblk.mainsize) ? (sysblk.mainsize - addr) : 0);
    if (k)
    {
        k = MIN(k, 65536);
        storage = sysblk.mainstor + addr;
        limit = storage + k;
        if (description &&
            description != NULL &&
            description[0])
            WRMSG(HHC90000, "I", description);
        if (sysblk.mainsize > 2*ONE_GIGABYTE)
            for (; storage < limit; addr += 16, storage += 16, k -= 16)
            {
                MSGBUF(msgbuf, I64_FMTX" => ", addr);
                format_data(msgbuf+20, sizeof(msgbuf)-20, storage, k);
                WRMSG(HHC90000, "I", msgbuf);
            }
        else
            for (; storage < limit; addr += 16, storage += 16, k -= 16)
            {
                MSGBUF(msgbuf, "%8.8X => ", (u_int)addr);
                format_data(msgbuf+12, sizeof(msgbuf)-12, storage, k);
                WRMSG(HHC90000, "I", msgbuf);
            }
    }

} /* end function dump_storage */


/*-------------------------------------------------------------------*/
/* DISPLAY CHANNEL COMMAND WORD AND DATA                             */
/*-------------------------------------------------------------------*/
static void
display_ccw ( const DEVBLK *dev, const BYTE ccw[], const U32 addr,
              const U32 count, const U8 flags )
{
BYTE    area[64];                       /* Data display area         */

    UNREFERENCED(flags);

    if (IS_CCW_READ(ccw[0])             ||
        IS_CCW_IMMEDIATE(dev, ccw[0])   ||
        IS_CCW_TIC(ccw[0])              ||
        IS_CCW_SENSE(ccw[0]))
        area[0] = 0;
    else
        format_iobuf_data (addr, area, dev, count);

    WRMSG (HHC01315, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
            ccw[0], ccw[1], ccw[2], ccw[3],
            ccw[4], ccw[5], ccw[6], ccw[7], area);

} /* end function display_ccw */


/*-------------------------------------------------------------------*/
/* Display IDAW and Data                                             */
/*-------------------------------------------------------------------*/
static void
display_idaw ( const DEVBLK *dev, const BYTE type, const BYTE flag,
               const RADR addr, const U16 count )
{
BYTE    area[64];                       /* Data display area         */

    format_iobuf_data( addr, area, dev, count );

    switch(type)
    {
        case PF_IDAW1:
            WRMSG (HHC01302, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
                                  (U32)addr, count, area);
            break;
        case PF_IDAW2:
            WRMSG (HHC01303, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
                                  (U64)addr, count, area);
            break;
        case PF_MIDAW:
            WRMSG (HHC01301, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
                                  flag, count, (U64)addr, area);
    }
}


/*-------------------------------------------------------------------*/
/* DISPLAY CHANNEL STATUS WORD                                       */
/*-------------------------------------------------------------------*/
static void
display_csw (const DEVBLK *dev, const BYTE csw[])
{
    WRMSG (HHC01316, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
            csw[0],
            csw[4], csw[5], csw[6], csw[7],
            csw[1], csw[2], csw[3]);

} /* end function display_csw */


/*-------------------------------------------------------------------*/
/* DISPLAY SUBCHANNEL STATUS WORD                                    */
/*-------------------------------------------------------------------*/
static void
display_scsw (const DEVBLK *dev, const SCSW scsw)
{
    switch (sysblk.arch_mode)
    {
        case ARCH_370:
        {
            U8  csw[8];

            scsw2csw(&scsw, csw);
            display_csw (dev, csw);
            break;
        }

        default:
            WRMSG (HHC01317, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
                   scsw.flag0, scsw.flag1, scsw.flag2, scsw.flag3,
                   scsw.unitstat, scsw.chanstat,
                   scsw.count[0], scsw.count[1],
                   scsw.ccwaddr[0], scsw.ccwaddr[1],
                   scsw.ccwaddr[2], scsw.ccwaddr[3]);
            break;
    }
} /* end function display_scsw */


/*-------------------------------------------------------------------*/
/*  Display prefetch table                                           */
/*-------------------------------------------------------------------*/
#if DEBUG_PREFETCH
static void
display_prefetch (PREFETCH *prefetch, const u_int ps,
                  const U32 count, const U32 residual,
                  const u_int more)
{
u_int   limit = MIN(prefetch->seq, PF_SIZE);
u_int   ts;
U32     used = count - residual;
char    msgbuf[133];

    MSGBUF(msgbuf, "Prefetch ps=%d count=%llu (%08.8LLX) "
                       "residual=%llu (%08.8LLX) more=%d "
                       "used=%llu (%08.8LLX)",
                   ps, count, count, residual, residual, more,
                   used, used);
    WRMSG(HHC90000, "I", msgbuf);

    MSGBUF(msgbuf, "Prefetch seq=%d req=%d (%04.4X) pos=%d (%04.4X) "
                            "prevcode=%02.2X code=%02.2X",
                   prefetch->seq, prefetch->reqcount,
                   prefetch->reqcount, prefetch->pos, prefetch->pos,
                   prefetch->prevcode, prefetch->opcode);
    WRMSG(HHC90000, "I", msgbuf);

    MSGBUF(msgbuf,
           "Prefetch "
           "  n "               /* ts                                */
           "  "                 /* Select pointer                    */
           "cs "                /* Channel Status                    */
           "scsw-ccw "          /* CCW Address                       */
           "data-len "          /* Data length                       */
           "    data-address "  /* Data address                      */
           "fl "                /* CCW Flags                         */
           "   count "          /* CCW Count                         */
           "idawaddr "          /* IDAW Address                      */
           "it "                /* IDAW Type                         */
           "if"                 /* IDAW Flag                         */
           );
    WRMSG(HHC90000, "I", msgbuf);

    for (ts = 0; ts < limit; ts++)
    {
        MSGBUF(msgbuf,
               "Prefetch "
               "%3d "           /* ts                                */
               "%2s"            /* Select pointer (ps)               */
               "%2.2X "         /* Channel Status                    */
               "%8.8X "         /* CCW Address (SCSW CCW Address)    */
               "%8.8X "         /* Data length                       */
               I64_FMTX" "      /* Data address                      */
               "%2.2X "         /* CCW Flags                         */
               "%8.8X "         /* CCW Count                         */
               "%8.8X "         /* IDAW Address                      */
               "%2d "           /* IDAW Type                         */
               "%2.2X",         /* IDAW Flag                         */
               ts, ((ps == ts) ? "->" : "  "),
               prefetch->chanstat[ts], prefetch->ccwaddr[ts],
               prefetch->datalen[ts], prefetch->dataaddr[ts],
               prefetch->ccwflags[ts], prefetch->ccwcount[ts],
               prefetch->idawaddr[ts], prefetch->idawtype[ts],
               prefetch->idawflag[ts]);
        WRMSG(HHC90000, "I", msgbuf);
    }
}
#else
#define display_prefetch(_prefetch, _ps, _count, _residual, _more)
#endif


/*-------------------------------------------------------------------*/
/* STORE CHANNEL ID                                                  */
/*-------------------------------------------------------------------*/
int
stchan_id (REGS *regs, U16 chan)
{
U32     chanid;                         /* Channel identifier word   */
int     devcount = 0;                   /* #of devices on channel    */
DEVBLK *dev;                            /* -> Device control block   */
PSA_3XX *psa;                           /* -> Prefixed storage area  */

    /* Find a device on specified channel */
    for (dev = sysblk.firstdev; dev != NULL; dev = dev->nextdev)
    {
        /* Skip the device if not on specified channel */
        if ((dev->devnum & 0xFF00) != chan
         || (dev->pmcw.flag5 & PMCW5_V) == 0
#if defined(FEATURE_CHANNEL_SWITCHING)
         || regs->chanset != dev->chanset
#endif /*defined(FEATURE_CHANNEL_SWITCHING)*/
                                            )
            continue;

        /* Found device on the channel */
        devcount++;
        break;
    } /* end for(dev) */

    /* Exit with condition code 3 if no devices on channel */
    if (devcount == 0)
        return 3;

    /* Construct the channel id word
     *
     * According to GA22-7000-4, Page 192, 2nd paragraph, channel 0 is a
     * Byte Multiplexor.. Return STIDC data accordingly.
     * ISW 20080313
     */
    if(!chan)
    {
        chanid = CHANNEL_MPX;
    }
    else
    {
        /* Translate channel type back to S/370 terms */
        switch (dev->chptype[0])
        {
            case CHP_TYPE_UNDEF:
                chanid = CHANNEL_SEL;
                break;
            case CHP_TYPE_BYTE:
                chanid = CHANNEL_MPX;
                break;
            default:
                chanid = CHANNEL_BMX;
                break;
        }
    }

    /* Store the channel id word at PSA+X'A8' */
    psa = (PSA_3XX*)(regs->mainstor + regs->PX);
    STORE_FW(psa->chanid, chanid);

    /* Exit with condition code 0 indicating channel id stored */
    return 0;

} /* end function stchan_id */


/*-------------------------------------------------------------------*/
/* TEST CHANNEL                                                      */
/*-------------------------------------------------------------------*/
int
testch (REGS *regs, U16 chan)
{
DEVBLK *dev;                            /* -> Device control block   */
int     devcount = 0;                   /* Number of devices found   */
int     cc = 0;                         /* Returned condition code   */

    /* Scan devices on the channel */
    for (dev = sysblk.firstdev; dev != NULL; dev = dev->nextdev)
    {
        /* Skip the device if not on specified channel */
        if ((dev->devnum & 0xFF00) != chan
         || (dev->pmcw.flag5 & PMCW5_V) == 0
#if defined(FEATURE_CHANNEL_SWITCHING)
         || regs->chanset != dev->chanset
#endif /*defined(FEATURE_CHANNEL_SWITCHING)*/
                                          )
            continue;

        /* Increment device count on this channel */
        devcount++;

        /* Test for pending interrupt */
        if (IOPENDING(dev))
        {
            cc = 1;
            break;
        }
    }

    /* Set condition code 3 if no devices found on the channel */
    if (devcount == 0)
        cc = 3;

    return cc;

} /* end function testch */


/*-------------------------------------------------------------------*/
/* TEST I/O                                                          */
/*-------------------------------------------------------------------*/
int
testio (REGS *regs, DEVBLK *dev, BYTE ibyte)
{
    int     cc;                         /* Condition code            */
    IRB     irb;                        /* Interrupt request block   */
    IOINT*  ioint;                      /* -> I/O interrupt          */
    SCSW*   scsw;                       /* -> SCSW                   */

    UNREFERENCED(ibyte);

    OBTAIN_INTLOCK(regs);
    obtain_lock(&dev->lock);

    /* Test device status and set condition code */
    if ((dev->busy && dev->ioactive == DEV_SYS_LOCAL)
     || dev->startpending)
    {
        /* Set condition code 2 for device busy */
        cc = 2;
    }
    else if (IOPENDING(dev))
    {
        /* Issue TEST SUBCHANNEL */
        cc = test_subchan_locked (regs, dev, &irb, &ioint, &scsw);

        if (cc)         /* Status incomplete */
            cc = 2;
        else            /* Status pending */
        {
            /* Obtain I/O interrupt queue lock */
            obtain_lock(&sysblk.iointqlk);

            /* Dequeue the interrupt */
            DEQUEUE_IO_INTERRUPT_QLOCKED(ioint);

            /* Store the channel status word at PSA+X'40' */
            store_scsw_as_csw(regs, &irb.scsw);

            /* Set condition code 1 for CSW stored */
            cc = 1;

            /* Cleanup after SCSW */
            subchannel_interrupt_queue_cleanup(dev);
            UPDATE_IC_IOPENDING_QLOCKED();

            /* Release I/O interrupt queue lock */
            release_lock(&sysblk.iointqlk);
        }
    }
    else
        cc = 0;

    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01318, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, cc);

    /* Complete unlock sequence */
    release_lock(&dev->lock);
    RELEASE_INTLOCK(regs);

    /* Return the condition code */
    return (cc);

} /* end function testio */


/*-------------------------------------------------------------------*/
/* HALT I/O                                                          */
/*-------------------------------------------------------------------*/
int
haltio (REGS *regs, DEVBLK *dev, BYTE ibyte)
{
int      cc;                            /* Condition code            */
PSA_3XX *psa;                           /* -> Prefixed storage area  */

    UNREFERENCED(ibyte);

    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01329, "I", SSID_TO_LCSS(dev->ssid), dev->devnum);

    OBTAIN_INTLOCK(regs);
    obtain_lock (&dev->lock);

    /* Test device status and set condition code */
    if (dev->busy)
    {
        /* Invoke the provided halt device routine @ISW */
        /* if it has been provided by the handler  @ISW */
        /* code at init                            @ISW */
        if(dev->hnd->halt!=NULL)                /* @ISW */
        {                                       /* @ISW */
            dev->hnd->halt(dev);                /* @ISW */
            cc=0;                               /* @ISW */
        }                                       /* @ISW */
        else
        {
            /* Set condition code 2 if device is busy */
            cc = 2;

            /* Tell channel and device to halt */
            dev->scsw.flag2 |= SCSW2_AC_HALT;

            /* Clear pending interrupts */
            DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->pciioint);
            DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->ioint);
            DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->attnioint);
            dev->pciscsw.flag3  &= ~(SCSW3_SC_ALERT |
                                     SCSW3_SC_PRI   |
                                     SCSW3_SC_SEC   |
                                     SCSW3_SC_PEND);
            dev->scsw.flag3     &= ~(SCSW3_SC_ALERT |
                                     SCSW3_SC_PRI   |
                                     SCSW3_SC_SEC   |
                                     SCSW3_SC_PEND);
            dev->attnscsw.flag3 &= ~(SCSW3_SC_ALERT |
                                     SCSW3_SC_PRI   |
                                     SCSW3_SC_SEC   |
                                     SCSW3_SC_PEND);
            subchannel_interrupt_queue_cleanup(dev);
        }
    }
    else if (!IOPENDING(dev) && dev->ctctype != CTC_LCS)
    {
        /* Set condition code 1 */
        cc = 1;

        /* Store the channel status word at PSA+X'40' */
        store_scsw_as_csw(regs, &dev->scsw);

        if (dev->ccwtrace || dev->ccwstep)
        {
            psa = (PSA_3XX*)(regs->mainstor + regs->PX);
            display_csw (dev, psa->csw);
        }
    }
    else if (dev->ctctype == CTC_LCS)
    {
        /* Set cc 1 if interrupt is not pending and LCS CTC */
        cc = 1;

        /* Store the channel status word at PSA+X'40' */
        store_scsw_as_csw(regs, &dev->scsw);

        if (dev->ccwtrace)
        {
            psa = (PSA_3XX*)(regs->mainstor + regs->PX);
            WRMSG (HHC01330, "I", SSID_TO_LCSS(dev->ssid), dev->devnum);
            display_csw (dev, psa->csw);
        }
    }
    else
    {
        /* Set condition code 0 if interrupt is pending */
        cc = 0;
    }

    /* Update interrupt status */
    subchannel_interrupt_queue_cleanup(dev);
    UPDATE_IC_IOPENDING_QLOCKED();

    /* Release locks */
    release_lock (&dev->lock);
    RELEASE_INTLOCK(regs);

    /* Return the condition code */
    return cc;

} /* end function haltio */

/*-------------------------------------------------------------------*/
/* CANCEL SUBCHANNEL                                                 */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      regs    -> CPU register context                              */
/*      dev     -> Device control block                              */
/* Return value                                                      */
/*      The return value is the condition code for the XSCH          */
/*      0=start function cancelled (not yet implemented)             */
/*      1=status pending (no action taken)                           */
/*      2=function not applicable                                    */
/*-------------------------------------------------------------------*/
int
cancel_subchan (REGS *regs, DEVBLK *dev)
{
int     cc;                             /* Condition code            */

    UNREFERENCED(regs);

    obtain_lock (&dev->lock);

#if defined(_FEATURE_IO_ASSIST)
    if(SIE_MODE(regs)
      && (regs->siebk->zone != dev->pmcw.zone
        || !(dev->pmcw.flag27 & PMCW27_I)))
    {
        release_lock (&dev->lock);
        longjmp(regs->progjmp,SIE_INTERCEPT_INST);
    }
#endif

    /* Check pending status */
    if ((dev->pciscsw.flag3 & SCSW3_SC_PEND)
     || (dev->scsw.flag3    & SCSW3_SC_PEND)
     || (dev->attnscsw.flag3    & SCSW3_SC_PEND))
        cc = 1;
    else
    {
        cc = 1;
        obtain_lock(&sysblk.ioqlock);
        if(sysblk.ioq != NULL)
        {
         DEVBLK *tmp;

            /* special case for head of queue */
            if(sysblk.ioq == dev)
            {
                /* Remove device from the i/o queue */
                sysblk.ioq = dev->nextioq;
                cc = 0;
            }
            else
            {
                /* Search for device on i/o queue */
                for(tmp = sysblk.ioq;
                    tmp->nextioq != NULL && tmp->nextioq != dev;
                    tmp = tmp->nextioq);

                /* Remove from queue if found */
                if(tmp->nextioq == dev)
                {
                    tmp->nextioq = tmp->nextioq->nextioq;
                    sysblk.devtunavail = MAX(0, sysblk.devtunavail -1);
                    cc = 0;
                }
            }
        }
        release_lock(&sysblk.ioqlock);

        /* Reset the device */
        if(!cc)
        {
            /* Terminate suspended channel program */
            if (dev->scsw.flag3 & SCSW3_AC_SUSP)
            {
                dev->suspended = 0;
                schedule_ioq(NULL, dev);
            }
            else
            {
                /* Reset the scsw */
                dev->scsw.flag2 &= ~(SCSW2_AC_RESUM |
                                     SCSW2_FC_START |
                                     SCSW2_AC_START);
                dev->scsw.flag3 &= ~(SCSW3_AC_SCHAC |
                                     SCSW3_AC_DEVAC |
                                     SCSW3_AC_SUSP);

                /* Reset the device busy indicator */
                clear_subchannel_busy(dev);
            }
        }
    }

    release_lock (&dev->lock);

    /* Return the condition code */
    return cc;

} /* end function cancel_subchan */


/*-------------------------------------------------------------------*/
/*  Perform DEVBLK cleanup following queueing/dequeueing of          */
/*  interrupt                                                        */
/*-------------------------------------------------------------------*/
static INLINE void
subchannel_interrupt_queue_cleanup(DEVBLK* dev)
{
    /* Begin the deprecation of the pending status bits in the
     * DEVBLK; the pending bits are now set from the corresponding
     * SCSWs.
     *
     * Implementation Note
     * -------------------
     * While SCSW3_SC_PEND is bit 31 of the SCSW, the old pending flags
     * are at "unknown" bit positions. Ensure that *ALL* C compilers
     * set the target bit fields properly.
     */
    dev->pcipending  = (dev->pciscsw.flag3  & SCSW3_SC_PEND) ? 1 : 0;
    dev->pending     = (dev->scsw.flag3     & SCSW3_SC_PEND) ? 1 : 0;
    dev->attnpending = (dev->attnscsw.flag3 & SCSW3_SC_PEND) ? 1 : 0;
}


/*-------------------------------------------------------------------*/
/* Perform clear operation for TEST SUBCHANNEL                       */
/*                                                                   */
/* SA22-7832-09, Figure 14-2, Conditions and Indications Cleared at  */
/*               the Subchannel by TEST SUBCHANNEL, p. 14-21.        */
/*                                                                   */
/* Locks on Entry                                                    */
/*   dev->lock                                                       */
/*                                                                   */
/*-------------------------------------------------------------------*/
static INLINE int
test_subchan_clear(DEVBLK* dev, SCSW* scsw)
{
    int cc = 1;                         /* Status not cleared        */

    /* Remove pending condition and requirement for TEST SUBCHANNEL */
    dev->tschpending = 0;

    /* Clearing only occurs if status is pending */
    if (scsw != NULL && scsw->flag3 & SCSW3_SC_PEND)
    {
        if (scsw->flag3 & SCSW3_SC_INTER)
        {
                                        /* SA22-7832-09, page 16-22  */
            scsw->flag1 &= ~SCSW1_Z;    /* Reset Zero Condition Code */

            scsw_clear_fc_Nc(scsw);
            scsw_clear_ac_Nr(scsw);
            scsw_clear_sc_Cs(scsw);
            /*          n_Nr(scsw); Implied by ac_NR */
            scsw_clear_q_C(scsw);
            cc = 0;
        }
        else if ((scsw->flag3 & (SCSW3_SC_ALERT  |
                                 SCSW3_SC_PRI    |
                                 SCSW3_SC_SEC))  ||
                 (scsw->flag3 & SCSW3_SC) == SCSW3_SC_PEND)
        {
            /* SA22-7832-09, page 16-17, TEST SUBCHANNEL clears
             * device suspended indication if suppressed
             */
            if (scsw == &dev->scsw          &&
                scsw->flag1 & SCSW1_U       &&
                scsw->flag3 & SCSW3_AC_SUSP)
                dev->busy = dev->suspended = 0;

            scsw_clear_fc_C(scsw);
            scsw_clear_ac_Cp(scsw);
            scsw_clear_sc_Cs(scsw);
            scsw_clear_n_C(scsw);
            scsw_clear_q_C(scsw);
            cc = 0;
        }
    }

    /* Ensure old DEVBLK status bits are maintained */
    subchannel_interrupt_queue_cleanup(dev);

    /* Return completion code */
    return (cc);
}


/*-------------------------------------------------------------------*/
/* TEST SUBCHANNEL (LOCKED)                                          */
/*-------------------------------------------------------------------*/
/* Core routine for TEST SUBCHANNEL; also used by the                */
/* present_io_interrupt routine for S/360 and S/370 channel support. */
/*                                                                   */
/* Input                                                             */
/*      regs    -> CPU register context                              */
/*      dev     -> Device control block                              */
/* Output                                                            */
/*      irb     -> Interruption response block                       */
/*      ioint   -> Interruption                                      */
/*      scsw    -> SCSW                                              */
/* Return value                                                      */
/*      The return value is the condition code for the TSCH          */
/*      instruction:  0=status was pending and is now cleared,       */
/*      1=no status was pending.  The IRB is updated in both cases.  */
/* Locks held on entry                                               */
/*      INTLOCK         Held by caller                               */
/*      dev->lock       Held by caller                               */
/*      iointqlk        Held by caller                               */
/*-------------------------------------------------------------------*/
int
test_subchan_locked (REGS* regs, DEVBLK* dev,
                     IRB* irb, IOINT** ioint, SCSW** scsw)
{
    enum                                /* Status types              */
    {                                   /* ...                       */
        pci,                            /* PCI Status                */
        normal,                         /* Normal Status             */
        attn                            /* Attention Status          */
    } status;                           /* ...                       */

    int cc = 1;                         /* No status pending         */

    UNREFERENCED( regs );

    if (unlikely((dev->pciscsw.flag3 & SCSW3_SC_PEND)))
    {
        status = pci;
        *ioint = &dev->pciioint;
        *scsw  = &dev->pciscsw;
    }
    else if (likely( (dev->scsw.flag3     & SCSW3_SC_PEND)  ||
                    !(dev->attnscsw.flag3 & SCSW3_SC_PEND)))
    {
        status = normal;
        *ioint = &dev->ioint;
        *scsw  = &dev->scsw;
    }
    else
    {
        status = attn;
        *ioint = &dev->attnioint;
        *scsw  = &dev->attnscsw;
    }

    /* Ensure status removed from interrupt queue */
    DEQUEUE_IO_INTERRUPT_QLOCKED(*ioint);

    /* Display the subchannel status word */
    if (dev->ccwtrace || dev->ccwstep)
    {
        display_scsw (dev, **scsw);
    }

    /* Copy the PCI SCSW to the IRB */
    irb->scsw = **scsw;

    /* Clear the ESW and ECW in the IRB */
    switch (status)
    {
        case normal:

            /* Copy the extended status word to the IRB */
            irb->esw = dev->esw;

            /* Copy the extended control word to the IRB */
            memcpy (irb->ecw, dev->ecw, sizeof(irb->ecw));
            break;

        default:
            /* Clear the ESW and ECW in the IRB */
            memset (&irb->esw, 0, sizeof(ESW));
            irb->esw.lpum = 0x80;
            memset (irb->ecw, 0, sizeof(irb->ecw));
            break;
    }

    /* Clear the subchannel and set condition code */
    cc = test_subchan_clear(dev, *scsw);

    /* Update pending interrupts */
    UPDATE_IC_IOPENDING_QLOCKED();

    /* Return condition code */
    return (cc);
}


/*-------------------------------------------------------------------*/
/* TEST SUBCHANNEL                                                   */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      regs    -> CPU register context                              */
/*      dev     -> Device control block                              */
/* Output                                                            */
/*      irb     -> Interruption response block                       */
/* Return value                                                      */
/*      The return value is the condition code for the TSCH          */
/*      instruction:  0=status was pending and is now cleared,       */
/*      1=no status was pending.  The IRB is updated in both cases.  */
/*-------------------------------------------------------------------*/
int
test_subchan (REGS *regs, DEVBLK *dev, IRB *irb)
{
    int     cc;                         /* Condition code            */
    IOINT*  ioint;                      /* I/O interrupt structure   */
    SCSW*   scsw;                       /* SCSW structure            */

    OBTAIN_INTLOCK(regs);
    obtain_lock (&dev->lock);

#if defined(_FEATURE_IO_ASSIST)
    if(SIE_MODE(regs))
    {
        if (regs->siebk->zone != dev->pmcw.zone
            || !(dev->pmcw.flag27 & PMCW27_I))
        {
            release_lock (&dev->lock);
            RELEASE_INTLOCK(regs);
            longjmp(regs->progjmp,SIE_INTERCEPT_INST);
        }

        /* For I/O assisted devices we must intercept if type B
           status is present on the subchannel */
        if (dev->pciscsw.flag3 & SCSW3_SC_PEND &&
            (regs->siebk->tschds & dev->pciscsw.unitstat  ||
             regs->siebk->tschsc & dev->pciscsw.chanstat))
        {
            dev->pmcw.flag27 &= ~PMCW27_I;
            release_lock (&dev->lock);
            RELEASE_INTLOCK(regs);
            longjmp(regs->progjmp,SIE_INTERCEPT_IOINST);
        }
    }
#endif

    /* Obtain the I/O interrupt queue lock */
    obtain_lock(&sysblk.iointqlk);

    /* Perform core of TEST SUBCHANNEL work */
    cc = test_subchan_locked (regs, dev, irb, &ioint, &scsw);

    /* Release the I/O interrupt queue lock */
    release_lock(&sysblk.iointqlk);

    /* Display the condition code */
    if (dev->ccwtrace || dev->ccwstep)
    {
        WRMSG (HHC01318, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, cc);
    }

    /* Release remaining locks */
    release_lock(&dev->lock);
    RELEASE_INTLOCK(regs);

    /* Return the condition code */
    return (cc);

} /* end function test_subchan */


/*-------------------------------------------------------------------*/
/* Perform CLEAR SUBCHANNEL operation                                */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      dev     -> Device control block                              */
/* Locks on Entry                                                    */
/*      INTLOCK(NULL)   Held by caller                               */
/*      dev->lock       Held by caller                               */
/* Locks on Return                                                   */
/*      INTLOCK(NULL)   Held by caller                               */
/*      dev->lock       Held by caller                               */
/* Locks Used                                                        */
/*      sysblk.iointqlk                                             */
/*-------------------------------------------------------------------*/
void
perform_clear_subchan (DEVBLK *dev)
{
    /* Dequeue pending interrupts */
    obtain_lock(&sysblk.iointqlk);
    DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->ioint);
    dev->scsw.flag3 &= ~SCSW3_SC_PEND;
    dev->pending = 0;
    clear_device_busy(dev);
    clear_subchannel_busy(dev);
    DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->pciioint);
    dev->pciscsw.flag3 &= ~SCSW3_SC_PEND;
    dev->pcipending = 0;
    clear_subchannel_busy_scsw(&dev->pciscsw);
    DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->attnioint);
    dev->attnscsw.flag3 &= ~SCSW3_SC_PEND;
    dev->attnpending = 0;
    dev->tschpending = 0;

    /* [15.3.2] Perform clear function subchannel modification */
    dev->pmcw.pom = 0xFF;
    dev->pmcw.lpum = 0x00;
    dev->pmcw.pnom = 0x00;

    /* [15.3.3] Perform clear function signaling and completion */
    dev->scsw.flag0 = 0;
    dev->scsw.flag1 = 0;
    dev->scsw.flag2 &= ~(SCSW2_FC | SCSW2_AC);
    dev->scsw.flag2 |= SCSW2_FC_CLEAR;
    /* Shortcut setting of flag3 due to clear and setting of just the
     * status pending flag:
     *   scsw.flag3 &= ~(SCSW3_AC | SCSW3_SC);
     *   scsw.flag3 |= SCSW3_SC_PEND;
     */
    dev->scsw.flag3 = SCSW3_SC_PEND;
    store_fw (dev->scsw.ccwaddr, 0);
    dev->scsw.chanstat = 0;
    dev->scsw.unitstat = 0;
    store_hw (dev->scsw.count, 0);

    /* Queue the pending interrupt and update status */
    QUEUE_IO_INTERRUPT_QLOCKED(&dev->ioint);
    subchannel_interrupt_queue_cleanup(dev);
    UPDATE_IC_IOPENDING_QLOCKED();
    release_lock(&sysblk.iointqlk);

    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01308, "I", SSID_TO_LCSS(dev->ssid),
                              dev->devnum);

    /* Wake up any waiters if the device isn't reserved */
    if (!dev->reserved)
    {
        dev->ioactive = DEV_SYS_NONE;
        if (dev->iowaiters)
           signal_condition (&dev->iocond);
    }
}


/*-------------------------------------------------------------------*/
/* CLEAR SUBCHANNEL                                                  */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      regs    -> CPU register context                              */
/*      dev     -> Device control block                              */
/*-------------------------------------------------------------------*/
void
clear_subchan (REGS *regs, DEVBLK *dev)
{
    UNREFERENCED(regs);

    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01331, "I", SSID_TO_LCSS(dev->ssid), dev->devnum);

#if defined(_FEATURE_IO_ASSIST)
    if(SIE_MODE(regs)
      && (regs->siebk->zone != dev->pmcw.zone
        || !(dev->pmcw.flag27 & PMCW27_I)))
    {
        longjmp(regs->progjmp,SIE_INTERCEPT_INST);
    }
#endif

    OBTAIN_INTLOCK(NULL);
    obtain_lock(&dev->lock);

    /* If the device is busy then signal the device to clear */
    if ((dev->busy  && dev->ioactive == DEV_SYS_LOCAL)
     || dev->startpending)
    {
        /* Set clear pending condition */
        dev->scsw.flag2 |= SCSW2_FC_CLEAR | SCSW2_AC_CLEAR;

        /* Signal the subchannel to resume if it is suspended */
        if (dev->scsw.flag3 & SCSW3_AC_SUSP)
        {
            dev->scsw.flag2 |= SCSW2_AC_RESUM;
            schedule_ioq(NULL, dev);
        }

        /* Invoke the provided halt device routine if it has been
         * provided by the handler code at init
         */
        else if(dev->hnd->halt!=NULL)
        {
            /* Revert to just holding dev->lock */
            release_lock(&dev->lock);
            RELEASE_INTLOCK(NULL);
            obtain_lock(&dev->lock);

            /* Call the device's halt routine */
            dev->hnd->halt(dev);

            /* Release dev->lock and return */
            release_lock(&dev->lock);
            return;
        }

#if !defined(NO_SIGABEND_HANDLER)
        else if( dev->ctctype )
        {
            signal_thread(dev->tid, SIGUSR2);
        }
#endif /*!defined(NO_SIGABEND_HANDLER)*/

    }
    else
        perform_clear_subchan(dev);

    release_lock (&dev->lock);
    RELEASE_INTLOCK(NULL);


} /* end function clear_subchan */


/*-------------------------------------------------------------------*/
/* Perform HALT and release device lock                              */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      dev     -> Device control block                              */
/* Return value                                                      */
/*      None                                                         */
/* Locks                                                             */
/*      INTLOCK   must be held by caller on entry.                   */
/*      dev->lock must be held by caller on entry; lock is released  */
/*                prior to return.                                   */
/*-------------------------------------------------------------------*/
void
perform_halt_and_release_lock (DEVBLK *dev)
{
    /* If status incomplete,
     * [15.4.2] Perform halt function signaling
     */
    if (!(dev->scsw.flag3 & (SCSW3_SC_PRI | SCSW3_SC_SEC) &&
          dev->scsw.flag3 & SCSW3_SC_PEND))
    {

        /* Indicate HALT started */
        set_subchannel_busy(dev);
        dev->scsw.flag2 |= SCSW2_FC_HALT;
        dev->scsw.flag2 &= ~SCSW2_AC_HALT;

        /* If intermediate status pending with subchannel and device
         * active, reset intermediate status pending (HSCH, fifth
         * paragraph).
         */
        if ((dev->scsw.flag3 &
             (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC | SCSW3_SC_INTER | SCSW3_SC_PEND)) ==
             (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC | SCSW3_SC_INTER | SCSW3_SC_PEND))
            dev->scsw.flag3 &= ~(SCSW3_SC_INTER | SCSW3_SC_PEND);

        /* Interim post-facto check for missing deferred condition code
         * Based on the Deferred-Condition-Code Meaning for Status-
         * Pending Subchannel (Figure 16-5).
         */
        if (!(dev->scsw.flag0 & SCSW0_CC) &&
             (dev->scsw.flag2 & (SCSW2_FC_START |
                                 SCSW2_AC_RESUM |
                                 SCSW2_AC_START)))
        {
            switch (AIPSX(&dev->scsw) & 0x0F)
            {
                case 0x0F:
                case 0x0D:
                case 0x09:
                case 0x07:
                case 0x05:
                case 0x03:
                case 0x01:
                    dev->scsw.flag0 &= ~SCSW0_CC;
                    dev->scsw.flag1 &= ~SCSW1_Z;
                    dev->scsw.flag0 |=  SCSW0_CC_1;
                    break;
            }
        }

        /* Invoke the provided halt device routine if it has been
         * provided by the handler code at init; SCSW has not yet been
         * reset to permit device handlers to see HALT condition.
         */
        if(dev->hnd->halt!=NULL)
            dev->hnd->halt(dev);
#if !defined(NO_SIGABEND_HANDLER)
        else if( dev->ctctype && dev->tid)
            signal_thread(dev->tid, SIGUSR2);
#endif /*!defined(NO_SIGABEND_HANDLER)*/

        /* Mark pending interrupt */
        dev->scsw.flag3 |= SCSW3_SC_PEND;
    }

    /* Trace HALT */
    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01300, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, 0);

    /* Queue pending I/O interrupt and update status */
    queue_io_interrupt_and_update_status_locked(dev);

    /* Turn off busy bit */
    clear_subchannel_busy(dev);

    release_lock(&dev->lock);
}


/*-------------------------------------------------------------------*/
/* Perform HALT                                                      */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      dev     -> Device control block                              */
/* Return value                                                      */
/*      None                                                         */
/* Locks on Entry                                                    */
/*      None                                                         */
/* Locks on Return                                                   */
/*      None                                                         */
/* Locks Used                                                        */
/*      dev->lock                                                    */
/*-------------------------------------------------------------------*/
void
perform_halt (DEVBLK *dev)
{
    OBTAIN_INTLOCK(NULL);
    obtain_lock(&dev->lock);
    perform_halt_and_release_lock(dev);
    RELEASE_INTLOCK(NULL);
}

/*-------------------------------------------------------------------*/
/* HALT SUBCHANNEL                                                   */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      regs    -> CPU register context                              */
/*      dev     -> Device control block                              */
/* Return value                                                      */
/*      The return value is the condition code for the HSCH          */
/*      instruction:  0=Halt initiated, 1=Non-intermediate status    */
/*      pending, 2=Busy                                              */
/*-------------------------------------------------------------------*/
int
halt_subchan (REGS *regs, DEVBLK *dev)
{
    UNREFERENCED(regs);

    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01332, "I", SSID_TO_LCSS(dev->ssid), dev->devnum);

    OBTAIN_INTLOCK(regs);
    obtain_lock (&dev->lock);

#if defined(_FEATURE_IO_ASSIST)
    if(SIE_MODE(regs)
      && (regs->siebk->zone != dev->pmcw.zone
        || !(dev->pmcw.flag27 & PMCW27_I)))
    {
        release_lock (&dev->lock);
        RELEASE_INTLOCK(regs);
        longjmp(regs->progjmp,SIE_INTERCEPT_INST);
    }
#endif

    /* Set condition code 1 if subchannel is status pending alone or
       is status pending with alert, primary, or secondary status */
    if ((dev->scsw.flag3 & SCSW3_SC) == SCSW3_SC_PEND
        || ((dev->scsw.flag3 & SCSW3_SC_PEND)
            && (dev->scsw.flag3 &
                    (SCSW3_SC_ALERT | SCSW3_SC_PRI | SCSW3_SC_SEC))))
    {
        if (dev->ccwtrace || dev->ccwstep)
            WRMSG (HHC01300, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, 1);
        release_lock (&dev->lock);
        RELEASE_INTLOCK(regs);
        return 1;
    }

    /* Set condition code 2 if the halt function or the clear
       function is already in progress at the subchannel */
    if (dev->scsw.flag2 & (SCSW2_AC_HALT | SCSW2_AC_CLEAR))
    {
        if (dev->ccwtrace || dev->ccwstep)
            WRMSG (HHC01300, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, 2);
        release_lock (&dev->lock);
        RELEASE_INTLOCK(regs);
        return 2;
    }

    /* Ensure SCSW flags are set from the ORB */
    if (dev->orb.flag4 & ORB4_S)
    {
        dev->scsw.flag0     |= SCSW0_S;
        dev->pciscsw.flag0  |= SCSW0_S;
        dev->attnscsw.flag0 |= SCSW0_S;
    }

    if (dev->orb.flag5 & ORB5_F)
    {
        dev->scsw.flag1     |= SCSW1_F;
        dev->pciscsw.flag1  |= SCSW1_F;
        dev->attnscsw.flag1 |= SCSW1_F;
    }

    if (dev->orb.flag5 & ORB5_P)
    {
        dev->scsw.flag1     |= SCSW1_P;
        dev->pciscsw.flag1  |= SCSW1_P;
        dev->attnscsw.flag1 |= SCSW1_P;
    }

    if (dev->orb.flag5 & ORB5_I)
    {
        dev->scsw.flag1     |= SCSW1_I;
        dev->pciscsw.flag1  |= SCSW1_I;
        dev->attnscsw.flag1 |= SCSW1_I;
    }

    if (dev->orb.flag5 & ORB5_A)
    {
        dev->scsw.flag1     |= SCSW1_A;
        dev->pciscsw.flag1  |= SCSW1_A;
        dev->attnscsw.flag1 |= SCSW1_A;
    }

    if (dev->orb.flag5 & ORB5_U)
    {
        dev->scsw.flag1     |= SCSW1_U;
        dev->pciscsw.flag1  |= SCSW1_U;
        dev->attnscsw.flag1 |= SCSW1_U;
    }

    /* Indicate halt pending to the various SCSWs for device */
    dev->attnscsw.flag2 |= SCSW2_AC_HALT;
    dev->pciscsw.flag2  |= SCSW2_AC_HALT;
    dev->scsw.flag2     |= SCSW2_AC_HALT;

    /* If intermediate status pending with subchannel and device active,
     * reset intermediate status pending (HSCH, fifth paragraph).
     */
    if ((dev->scsw.flag3 &
        (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC | SCSW3_SC_INTER | SCSW3_SC_PEND)) ==
        (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC | SCSW3_SC_INTER | SCSW3_SC_PEND))
        dev->scsw.flag3 &= ~(SCSW3_SC_INTER | SCSW3_SC_PEND);

    /* If the device is busy then signal subchannel to halt */
    if ((dev->busy && dev->ioactive == DEV_SYS_LOCAL)
     || dev->startpending || dev->suspended)
    {
        /* Set halt condition and reset pending condition */
        dev->scsw.flag2 |= (SCSW2_FC_HALT | SCSW2_AC_HALT);
        dev->scsw.flag3 &= ~SCSW3_SC_PEND;

        /* Signal the subchannel to resume if it is suspended */
        if (dev->scsw.flag3 & SCSW3_AC_SUSP)
        {
            dev->scsw.flag2 |= SCSW2_AC_RESUM;
            signal_condition (&dev->resumecond);
        }

        release_lock(&dev->lock);
    }
    else    /* Device not started */
    {
        /* Remove the device from the ioq if startpending and queued;
         * lock required before test to keep from entering queue and
         * becoming active prior to queue manipulation.
         */
        obtain_lock(&sysblk.ioqlock);
        if(dev->startpending)
        {
            if(sysblk.ioq == dev)
                sysblk.ioq = dev->nextioq;
            else if ( sysblk.ioq != NULL )
            {
             DEVBLK *tmp;
                for(tmp = sysblk.ioq;
                    tmp->nextioq != NULL && tmp->nextioq != dev;
                    tmp = tmp->nextioq);
                if(tmp->nextioq == dev)
                {
                    tmp->nextioq = tmp->nextioq->nextioq;
                    sysblk.devtunavail = MAX(0, sysblk.devtunavail -1);
                }
            }
            dev->startpending = 0;
        }
        release_lock(&sysblk.ioqlock);

        /* Halt the device */
        perform_halt_and_release_lock(dev);
    }

    RELEASE_INTLOCK(regs);

    /* Return condition code zero */
    return 0;

} /* end function halt_subchan */

/*-------------------------------------------------------------------*/
/* Reset a device to initialized status                              */
/*                                                                   */
/*   Called by:                                                      */
/*     channelset_reset                                              */
/*     chp_reset                                                     */
/*     io_reset                                                      */
/*                                                                   */
/*   Caller holds `intlock'                                          */
/*-------------------------------------------------------------------*/
static void
device_reset (DEVBLK *dev)
{
    obtain_lock (&dev->lock);

    obtain_lock(&sysblk.iointqlk);
    DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->ioint);
    DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->pciioint);
    DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->attnioint);
    release_lock(&sysblk.iointqlk);

    clear_subchannel_busy(dev);

#if defined(OPTION_SYNCIO)
    dev->syncio = dev->syncio_active = dev->syncio_retry =
#endif

    dev->reserved = dev->pending = dev->pcipending =
    dev->attnpending = dev->startpending = 0;
    dev->ioactive = DEV_SYS_NONE;
    if (dev->suspended)
    {
        dev->suspended = 0;
        schedule_ioq(NULL, dev);
    }
    if (dev->iowaiters) signal_condition (&dev->iocond);
    store_fw (dev->pmcw.intparm, 0);
    dev->pmcw.flag4 &= ~PMCW4_ISC;
    dev->pmcw.flag5 &= ~(PMCW5_E | PMCW5_LM | PMCW5_MM | PMCW5_D);
    dev->pmcw.pnom = 0;
    dev->pmcw.lpum = 0;
    store_hw (dev->pmcw.mbi, 0);
    dev->pmcw.flag27 &= ~PMCW27_S;
#if defined(_FEATURE_IO_ASSIST)
    dev->pmcw.zone = 0;
    dev->pmcw.flag25 &= ~PMCW25_VISC;
    dev->pmcw.flag27 &= ~PMCW27_I;
#endif
    memset (&dev->scsw, 0, sizeof(SCSW));
    memset (&dev->pciscsw, 0, sizeof(SCSW));
    memset (&dev->attnscsw, 0, sizeof(SCSW));

    dev->readpending = 0;
    dev->ckdxtdef = 0;
    dev->ckdsetfm = 0;
    dev->ckdlcount = 0;
    dev->ckdssi = 0;
    memset (dev->sense, 0, sizeof(dev->sense));
    dev->sns_pending = 0;
    memset (dev->pgid, 0, sizeof(dev->pgid));
    /* By Adrian - Reset drive password */
    memset (dev->drvpwd, 0, sizeof(dev->drvpwd));

    dev->mainstor = sysblk.mainstor;
    dev->storkeys = sysblk.storkeys;
    dev->mainlim = sysblk.mainsize - 1;

    dev->s370start = 0;

    dev->pending = 0;
    dev->ioint.dev = dev;
    dev->ioint.pending = 1;
    dev->pciioint.dev = dev;
    dev->pciioint.pcipending = 1;
    dev->attnioint.dev = dev;
    dev->attnioint.attnpending = 1;
    dev->tschpending = 0;

#if defined(FEATURE_VM_BLOCKIO)
    if (dev->vmd250env)
    {
       free(dev->vmd250env);
       dev->vmd250env = 0 ;
    }
#endif /* defined(FEATURE_VM_BLOCKIO) */

    if (dev->hnd->halt!=NULL)
        dev->hnd->halt(dev);

    release_lock (&dev->lock);
} /* end device_reset() */

/*-------------------------------------------------------------------*/
/* Reset all devices on a particular channelset                      */
/*                                                                   */
/*   Called by:                                                      */
/*     SIGP_IMPL    (control.c)                                      */
/*     SIGP_IPR     (control.c)                                      */
/*-------------------------------------------------------------------*/
void
channelset_reset(REGS *regs)
{
DEVBLK *dev;                            /* -> Device control block   */

    /* Reset each device in the configuration */
    for (dev = sysblk.firstdev; dev != NULL; dev = dev->nextdev)
    {
        if( regs->chanset == dev->chanset)
            device_reset(dev);
    }
} /* end function channelset_reset */

/*-------------------------------------------------------------------*/
/* Reset all devices on a particular chpid                           */
/* Called by io.c 'RHCP' Reset Channel Path instruction.             */
/*                                                                   */
/* FIXME: Console code belongs in the device code.                   */
/*                                                                   */
/*-------------------------------------------------------------------*/
int
chp_reset(BYTE chpid, int solicited)
{
DEVBLK *dev;                            /* -> Device control block   */
int i;
int reset = 0;

    /* Reset each device in the configuration with this chpid */
    for (reset=0, dev = sysblk.firstdev; dev != NULL; dev = dev->nextdev)
    {
        if (!IS_DEV(dev))
            continue;

        for (i=0; i < 8; i++)
        {
            if((chpid == dev->pmcw.chpid[i])
              && (dev->pmcw.pim & dev->pmcw.pam & dev->pmcw.pom & (0x80 >> i)) )
            {
                reset = 1;
                device_reset(dev);
            }
        }
    }

    /* Indicate channel path reset completed */
    build_chp_reset_chrpt( chpid, solicited, reset );
    return 0;

} /* end function chp_reset */


/*-------------------------------------------------------------------*/
/* I/O RESET  --  handle the "Channel Subsystem Reset" signal        */
/* Resets status of all devices ready for IPL.  Note that device     */
/* positioning is not affected by I/O reset; thus the system can     */
/* be IPLed from current position in a tape or card reader file.     */
/*                                                                   */
/* Caller holds `intlock'                                            */
/*                                                                   */
/* FIXME: Console references belong in the console device driver     */
/*        code!                                                      */
/*-------------------------------------------------------------------*/
void
io_reset (void)
{
DEVBLK *dev;                            /* -> Device control block   */
int i;

    /* Reset channel subsystem back to default initial non-MSS state */
    sysblk.mss = FALSE;                 /* (not enabled by default)  */
    sysblk.lcssmax = 0;                 /* (default to single lcss)  */

    /* reset sclp interface */
    sclp_reset();

    /* Connect each channel set to its home cpu */
    for (i = 0; i < sysblk.maxcpu; i++)
        if (IS_CPU_ONLINE(i))
            sysblk.regs[i]->chanset = i < FEATURE_LCSS_MAX ? i : 0xFFFF;

    /* Reset each device in the configuration */
    for (dev = sysblk.firstdev; dev != NULL; dev = dev->nextdev)
    {
        device_reset(dev);
    }

    /* No crws pending anymore */
    OBTAIN_CRWLOCK();
    sysblk.crwcount = 0;
    sysblk.crwindex = 0;
    RELEASE_CRWLOCK();
    OFF_IC_CHANRPT;

} /* end function io_reset */


/*-------------------------------------------------------------------*/
/* Set a thread's priority to its proper value                       */
/*-------------------------------------------------------------------*/
static void
adjust_thread_priority (int *newprio)
{
    /* Set root mode in order to set priority */
    SETMODE(ROOT);

#if 0 // BHe: Design must be different than current
    /* Set device thread priority; ignore any errors */
    if(setpriority(PRIO_PROCESS, 0, *newprio))
       WRMSG(HHC00136, "W", "setpriority()", strerror(errno));
#else
    setpriority(PRIO_PROCESS, 0, *newprio);
#endif

    /* Back to user mode */
    SETMODE(USER);
}


/*-------------------------------------------------------------------*/
/* Create a device thread                                            */
/*-------------------------------------------------------------------*/
/*                                                                   */
/* Locks:                                                            */
/*                                                                   */
/* sysblk->ioqlock must be held.                                     */
/*                                                                   */
/*-------------------------------------------------------------------*/
static int
create_device_thread ()
{
int     rc;                             /* Return code               */
TID     tid;                            /* Thread ID                 */

    /* Ensure correct number of ioq entries tracked */
    {
        register int i;
        register DEVBLK  *ioq;

        for (ioq = sysblk.ioq, i = 0;
             ioq;
             ioq = ioq->nextioq, i++);
        sysblk.devtunavail = i;

        /* If no additional work, return */
        if (!i)
            return 0;

    }

    /* If work is waiting and permitted, schedule another device     */
    /* thread to handle                                              */
    if ((sysblk.devtunavail > sysblk.devtwait &&
         (sysblk.devtmax == 0 || sysblk.devtnbr < sysblk.devtmax)) ||
        sysblk.devtmax < 0)
    {
        rc = create_thread (&tid, DETACHED, device_thread, NULL,
                            "idle device thread");
        if (rc)
        {
            WRMSG (HHC00102, "E", strerror(rc));
            return 2;
        }

        /* Update counters */
        sysblk.devtnbr++;
        sysblk.devtwait++;
        if (sysblk.devtnbr > sysblk.devthwm)
            sysblk.devthwm = sysblk.devtnbr;
    }

    /* Signal possible waiting I/O threads */
    signal_condition(&sysblk.ioqcond);

    return 0;
}


/*-------------------------------------------------------------------*/
/* Execute a queued I/O                                              */
/*-------------------------------------------------------------------*/
DLL_EXPORT void *
device_thread (void *arg)
{
DEVBLK *dev;
int     current_priority;               /* Current thread priority   */
int     rc = 0;                         /* Return code               */
u_int   waitcount = 0;                  /* Wait counter              */

    UNREFERENCED(arg);

    current_priority = getpriority(PRIO_PROCESS, 0);
    if (current_priority != sysblk.devprio)
    {
        adjust_thread_priority(&sysblk.devprio);
        current_priority = sysblk.devprio;
    }

    obtain_lock(&sysblk.ioqlock);

    sysblk.devtwait = MAX(0, sysblk.devtwait - 1);

    while (1)
    {
        while ((dev=sysblk.ioq)  &&
               !sysblk.shutdown)
        {
            /* Reset local wait count */
            waitcount = 0;

            /* Set next IOQ entry and zero pointer in current DEVBLK */
            sysblk.ioq = dev->nextioq;
            dev->nextioq = 0;

            /* Decrement waiting IOQ count */
            sysblk.devtunavail = MAX(0, sysblk.devtunavail-1);

            /* Create another device thread if pending work */
            create_device_thread();

            /* Set thread id */
            dev->tid = thread_id();

#if defined(_MSVC_)
            /* Set thread name */
            {
                char    thread_name[32];

                MSGBUF( thread_name,
                        "device %4.4X thread", dev->devnum );
                thread_name[sizeof(thread_name)-1]=0;
                SET_THREAD_NAME(thread_name);
            }
#endif

            release_lock (&sysblk.ioqlock);

            /* Set priority to requested device priority; should not */
            /* have any Hercules locks held                          */
            if (dev->devprio != current_priority)
            {
                adjust_thread_priority(&dev->devprio);
                current_priority = dev->devprio;
            }

            /* Execute requested CCW chain */
            call_execute_ccw_chain(sysblk.arch_mode, dev);

            /* Reset priority back to device default priority */
            if (current_priority != sysblk.devprio)
            {
                adjust_thread_priority(&sysblk.devprio);
                current_priority = sysblk.devprio;
            }

            /* Done. Reset the threadid used by the device and       */
            /* re-obtain ioqlock                                     */
            obtain_lock(&sysblk.ioqlock);
            dev->tid = 0;
        }

        /* Shutdown thread on request, if idle for more than two     */
        /* seconds, or more than four idle threads                   */
        if ((sysblk.devtmax == 0 &&
                waitcount >= 20 &&
                sysblk.devtwait > 3)                                 ||
            (sysblk.devtmax >  0 && sysblk.devtnbr > sysblk.devtmax) ||
            sysblk.devtmax < 0                                       ||
            sysblk.shutdown)
            break;

        /* Show thread as idle */
        waitcount++;
        sysblk.devtwait++;
        SET_THREAD_NAME("idle device thread");  /* If _MSVC_ defined */

        /* Wait for work to arrive */
        rc = timed_wait_condition_relative_usecs (&sysblk.ioqcond,
                                                  &sysblk.ioqlock,
                                                  100000 /* 100 ms */,
                                                  NULL);

        /* Decrement thread waiting count */
        sysblk.devtwait = MAX(0, sysblk.devtwait - 1);

        /* If shutdown requested, terminate the thread after signaling
         * next I/O thread to shutdown.
         */
        if (sysblk.shutdown)
        {
            signal_condition (&sysblk.ioqcond);
            break;
        }
    }

    /* Decrement total number of device threads */
    sysblk.devtnbr = MAX(0, sysblk.devtnbr - 1);

    /* Release queue lock and return */
    release_lock (&sysblk.ioqlock);
    return (NULL);

} /* end function device_thread */


/*-------------------------------------------------------------------*/
/* Schedule I/O Request (second half of Schedule IOQ)                */
/*-------------------------------------------------------------------*/
/*                                                                   */
/* Note: Queue is actually split with resume requests first, then    */
/*       followed by start requests, sorted by priorities.           */
/*                                                                   */
/* Locks held:                                                       */
/*   dev->lock                                                       */
/*                                                                   */
/* Locks used:                                                       */
/*   sysblk.ioqlock                                                  */
/*                                                                   */
/*-------------------------------------------------------------------*/
static int
ScheduleIORequest ( DEVBLK *dev )
{
    int     rc = 0;                     /* Return Code               */
    int     resume = dev->scsw.flag3 & SCSW3_AC_SUSP;   /* Resume?   */
    DEVBLK *previoq, *ioq;              /* Device I/O queue pointers */

    /* Queue the I/O request */
    obtain_lock (&sysblk.ioqlock);

    /* Insert the device into the I/O queue */
    for (previoq = NULL, ioq = sysblk.ioq;
         ioq &&
            /* 1. Resume                                         */
            resume >= (ioq->scsw.flag3 & SCSW3_AC_SUSP) &&
            /* 2. Subchannel priority                            */
            dev->priority >= ioq->priority;
         previoq = ioq, ioq = ioq->nextioq);
    dev->nextioq = ioq;
    if (previoq != NULL)
        previoq->nextioq = dev;
    else
        sysblk.ioq = dev;
    sysblk.devtunavail++;

    rc = create_device_thread();

    /* Release lock */
    release_lock(&sysblk.ioqlock);

    /* Return condition code */
    return rc;
}

#if defined(OPTION_SYNCIO)
/*-------------------------------------------------------------------*/
/*  EXECUTE SYNCIO                                                   */
/*-------------------------------------------------------------------*/
/*                                                                   */
/*  Execute I/O synchronously.                                       */
/*                                                                   */
/*                                                                   */
/*  Format:                                                          */
/*                                                                   */
/*      execute_syncio (regs, dev)                                   */
/*                                                                   */
/*                                                                   */
/*  Entry Conditions:                                                */
/*                                                                   */
/*      dev->lock       must be held                                 */
/*                                                                   */
/*                                                                   */
/*  Locks Used:                                                      */
/*                                                                   */
/*    sysblk.intlock                                                 */
/*                                                                   */
/*                                                                   */
/*  Returns:                                                         */
/*                                                                   */
/*  0 - Success                                                      */
/*  2 - Unable to start I/O operation synchronously                  */
/*                                                                   */
/*-------------------------------------------------------------------*/
int
execute_syncio (REGS* regs, DEVBLK* dev)
{
    int result = 2;

    /* Initiate synchronous I/O */
    dev->s370start = 0;
    dev->syncio_active = 1;
    dev->ioactive = DEV_SYS_LOCAL;
    dev->regs = regs;
    release_lock (&dev->lock);

    /* syncio is set with intlock held.  This allows SYNCHRONIZE_CPUS
     * to consider this CPU waiting while performing synchronous I/O.
     */
    if (regs->cpubit != sysblk.started_mask)
    {
        OBTAIN_INTLOCK(regs);
        regs->hostregs->syncio = 1;
        RELEASE_INTLOCK(regs);
    }

    call_execute_ccw_chain(sysblk.arch_mode, dev);

    if (regs->hostregs->syncio)
    {
        OBTAIN_INTLOCK(regs);
        regs->hostregs->syncio = 0;
        RELEASE_INTLOCK(regs);
    }

    /* Return if retry not required */
    obtain_lock (&dev->lock);
    dev->regs = NULL;
    dev->syncio_active = 0;
    if (!dev->syncio_retry)
        result = 0;


    /* syncio_retry gets turned off after the execute ccw device
     * handler routine is called for the first time.
     */

    return (result);
}
#endif /*defined(OPTION_SYNCIO)*/


/*-------------------------------------------------------------------*/
/*  SCHEDULE IOQ                                                     */
/*-------------------------------------------------------------------*/
/*                                                                   */
/*  Schedule I/O work for an IOP to dispatch to channel              */
/*                                                                   */
/*                                                                   */
/*  Format:                                                          */
/*                                                                   */
/*    schedule_ioq (regs, dev)                                       */
/*                                                                   */
/*                                                                   */
/*  Entry Conditions:                                                */
/*                                                                   */
/*    dev->lock     must be held.                                    */
/*                                                                   */
/*                                                                   */
/*  Locks Used:                                                      */
/*                                                                   */
/*    None                                                           */
/*                                                                   */
/*                                                                   */
/*  Returns:                                                         */
/*                                                                   */
/*  0 - Success                                                      */
/*  2 - Unable to schedule channel thread (this RC belongs in IOP)   */
/*                                                                   */
/*-------------------------------------------------------------------*/

static int
schedule_ioq (const REGS *regs, DEVBLK *dev)
{
    int result = 2;                     /* 0=Thread scheduled        */
                                        /* 2=Unable to schedule      */

    /* If shutdown in progress, don't schedule or perform I/O
     * operation.
     */
    if (sysblk.shutdown)
    {
        signal_condition(&sysblk.ioqcond);
        return (result);
    }

#if defined(OPTION_SYNCIO)
    {
        int syncio;                     /* 1=Do synchronous I/O      */

        /* Schedule the I/O.  The various methods are a direct
         * correlation to the interest in the subject:
         * [1] Synchronous I/O.  Attempts to complete the channel
         *     program in the cpu thread to avoid any threads overhead.
         * [2] Device threads.  Queue the I/O and signal a device
         *     thread. Eliminates the overhead of thead creation and
         *     termination.
         * [3] Original.  Create a thread to execute this I/O.
         */

        /* Determine if we can do synchronous I/O */
        if (!regs)
            syncio = 0;
        else if (dev->syncio == 1)
            syncio = 1;
        else if (dev->syncio == 2 &&
                 fetch_fw(dev->orb.ccwaddr) < dev->mainlim)
        {
            dev->code = dev->mainstor[fetch_fw(dev->orb.ccwaddr)];
            syncio = IS_CCW_TIC(dev->code)      ||
                     IS_CCW_SENSE(dev->code)    ||
                     IS_CCW_IMMEDIATE(dev, dev->code);
        }
        else
            syncio = 0;

        if (syncio && dev->ioactive == DEV_SYS_NONE
#ifdef OPTION_IODELAY_KLUDGE
         && sysblk.iodelay < 1
#endif /*OPTION_IODELAY_KLUDGE*/
           )
        {
            result = execute_syncio((REGS*)regs, dev);
            if (result == 0)
                return (result);
        }
    }

    /* Otherwise, schedule normally */
#endif /*defined(OPTION_SYNCIO)*/

    if (dev->s370start && regs != NULL)
    {
        release_lock(&dev->lock);
        call_execute_ccw_chain(sysblk.arch_mode, dev);
        obtain_lock(&dev->lock);
        result = 0;
    }
    else
        result = ScheduleIORequest(dev);

    return (result);
}


/*-------------------------------------------------------------------*/
/* RESUME SUBCHANNEL                                                 */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      regs    -> CPU register context                              */
/*      dev     -> Device control block                              */
/* Return value                                                      */
/*      The return value is the condition code for the RSCH          */
/*      instruction:  0=subchannel has been made resume pending,     */
/*      1=status was pending, 2=resume not allowed                   */
/*-------------------------------------------------------------------*/
int
resume_subchan (REGS *regs, DEVBLK *dev)
{
int cc;                                 /* Return code               */

    obtain_lock (&dev->lock);

#if defined(_FEATURE_IO_ASSIST)
    if(SIE_MODE(regs)
      && (regs->siebk->zone != dev->pmcw.zone
        || !(dev->pmcw.flag27 & PMCW27_I)))
    {
        release_lock (&dev->lock);
        longjmp(regs->progjmp,SIE_INTERCEPT_INST);
    }
#endif

    /* Set condition code 1 if subchannel has status pending
     *
     * SA22-7832-09, RESUME SUBCHANNEL, Condition Code 1, p. 14-11
     */
    if (dev->scsw.flag3     & SCSW3_SC_PEND ||
        dev->pciscsw.flag3  & SCSW3_SC_PEND ||
        dev->attnscsw.flag3 & SCSW3_SC_PEND)
    {
        cc = 1;
    }

    /* Set condition code 2 if subchannel has any function active, is
     * already resume pending, or the ORB for the SSCH did not specify
     * suspend control.
     *
     * SA22-7832-09, RESUME SUBCHANNEL, Condition Code 2, p. 14-11
     */
    else if (unlikely(!(dev->orb.flag4  & ORB4_S)           ||
                      !(dev->scsw.flag2 & SCSW2_FC)         ||
                      !(dev->scsw.flag3 & SCSW3_AC_SUSP)    ||
                      (dev->scsw.flag2 &
                       (SCSW2_FC_HALT  | SCSW2_FC_CLEAR |
                        SCSW2_AC_RESUM | SCSW2_AC_START |
                        SCSW2_AC_HALT  | SCSW2_AC_CLEAR))   ||
                      (dev->scsw.flag3 &
                       (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC))))
    {
        cc = 2;
    }

    /* Otherwise, schedule the resume request */
    else
    {
        /* Clear the path not-operational mask if in suspend state */
        if (dev->scsw.flag3 & SCSW3_AC_SUSP)
            dev->pmcw.pnom = 0x00;

        /* Set the resume pending flag and signal the subchannel */
        dev->scsw.flag2 |= SCSW2_AC_RESUM;
        cc = schedule_ioq(NULL, dev);
    }

    /* If tracing, write trace message */
    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01333, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, cc);

    release_lock (&dev->lock);

    /* Return the condition code */
    return cc;

} /* end function resume_subchan */

#endif /*!defined(_CHANNEL_C)*/


/*-------------------------------------------------------------------*/
/* RAISE A PCI INTERRUPT                                             */
/* This function is called during execution of a channel program     */
/* whenever a CCW is fetched which has the CCW_FLAGS_PCI flag set    */
/*                                                                   */
/* Note: PCI interrupts are incorrectly presented as a separate      */
/*       interrupt. The PCI interrupt should be ORed in to the       */
/*       next status read.                                           */
/*                                                                   */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      dev     -> Device control block                              */
/*      ccwkey  =  Key in which channel program is executing         */
/*      ccwfmt  =  CCW format (0 or 1)                               */
/*      ccwaddr =  Address of next CCW                               */
/* Output                                                            */
/*      The PCI CSW or SCSW is saved in the device block, and the    */
/*      pcipending flag is set, and an I/O interrupt is scheduled.   */
/*-------------------------------------------------------------------*/
static void
ARCH_DEP(raise_pci) (DEVBLK *dev,       /* -> Device block           */
                     BYTE ccwkey,       /* Bits 0-3=key, 4-7=zeroes  */
                     BYTE ccwfmt,       /* CCW format (0 or 1)       */
                     U32 ccwaddr)       /* Main storage addr of CCW  */
{
#if !defined(FEATURE_CHANNEL_SUBSYSTEM)
    UNREFERENCED(ccwfmt);
#endif

    IODELAY(dev);

    OBTAIN_INTLOCK(NULL);
    obtain_lock (&dev->lock);

    /* Save the PCI SCSW replacing any previous pending PCI; always
     * track the channel in channel subsystem mode
     */
    dev->pciscsw.flag0 = ccwkey & SCSW0_KEY;
    dev->pciscsw.flag1 = (ccwfmt == 1 ? SCSW1_F : 0);
    dev->pciscsw.flag2 = SCSW2_FC_START;
    dev->pciscsw.flag3 = SCSW3_AC_SCHAC | SCSW3_AC_DEVAC
                       | SCSW3_SC_INTER | SCSW3_SC_PEND;
    STORE_FW(dev->pciscsw.ccwaddr,ccwaddr);
    dev->pciscsw.unitstat = 0;
    dev->pciscsw.chanstat = CSW_PCI;
    store_hw (dev->pciscsw.count, 0);

    /* Queue the PCI pending interrupt */
    obtain_lock(&sysblk.iointqlk);
    QUEUE_IO_INTERRUPT_QLOCKED(&dev->pciioint);

    /* Update interrupt status */
    subchannel_interrupt_queue_cleanup(dev);
    UPDATE_IC_IOPENDING_QLOCKED();
    release_lock(&sysblk.iointqlk);
    release_lock(&dev->lock);
    RELEASE_INTLOCK(NULL);

} /* end function raise_pci */


/*-------------------------------------------------------------------*/
/* FETCH A CHANNEL COMMAND WORD FROM MAIN STORAGE                    */
/*-------------------------------------------------------------------*/
static INLINE void
ARCH_DEP(fetch_ccw) (DEVBLK *dev,       /* -> Device block           */
                     BYTE ccwkey,       /* Bits 0-3=key, 4-7=zeroes  */
                     BYTE ccwfmt,       /* CCW format (0 or 1)   @IWZ*/
                     U32 ccwaddr,       /* Main storage addr of CCW  */
                     BYTE *code,        /* Returned operation code   */
                     U32 *addr,         /* Returned data address     */
                     BYTE *flags,       /* Returned flags            */
                     U32 *count,        /* Returned data count       */
                     BYTE *chanstat)    /* Returned channel status   */
{
BYTE    storkey;                        /* Storage key               */
BYTE   *ccw;                            /* CCW pointer               */

    UNREFERENCED_370(dev);
    *code=0;
    *count=0;
    *flags=0;
    *addr=0;

    /* Channel program check if CCW is not on a doubleword
       boundary or is outside limit of main storage */
    if ( (ccwaddr & 0x00000007) || CHADDRCHK(ccwaddr, dev) )
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel protection check if CCW is fetch protected */
    storkey = STORAGE_KEY(ccwaddr, dev);
    if (ccwkey != 0 && (storkey & STORKEY_FETCH)
        && (storkey & STORKEY_KEY) != ccwkey)
    {
        *chanstat = CSW_PROTC;
        return;
    }

    /* Set the main storage reference bit for the CCW location */
    STORAGE_KEY(ccwaddr, dev) |= STORKEY_REF;

    /* Point to the CCW in main storage */
    ccw = dev->mainstor + ccwaddr;

    /* Extract CCW opcode, flags, byte count, and data address */
    if (ccwfmt == 0)
    {
        *code = ccw[0];
        *addr = ((U32)(ccw[1]) << 16) | ((U32)(ccw[2]) << 8)
                    | ccw[3];
        *flags = ccw[4];
        *count = ((U16)(ccw[6]) << 8) | ccw[7];
    }
    else
    {
        *code = ccw[0];
        *flags = ccw[1];
        *count = ((U16)(ccw[2]) << 8) | ccw[3];
        *addr = ((U32)(ccw[4]) << 24) | ((U32)(ccw[5]) << 16)
                    | ((U32)(ccw[6]) << 8) | ccw[7];
    }
} /* end function fetch_ccw */

/*-------------------------------------------------------------------*/
/* FETCH AN INDIRECT DATA ADDRESS WORD FROM MAIN STORAGE             */
/*-------------------------------------------------------------------*/
static INLINE void
ARCH_DEP(fetch_idaw) (DEVBLK *dev,      /* -> Device block           */
                      BYTE code,        /* CCW operation code        */
                      BYTE ccwkey,      /* Bits 0-3=key, 4-7=zeroes  */
                      BYTE idawfmt,     /* IDAW format (1 or 2)  @IWZ*/
                      U16 idapmask,     /* IDA page size - 1     @IWZ*/
                      int idaseq,       /* 0=1st IDAW                */
                      U32 idawaddr,     /* Main storage addr of IDAW */
                      RADR *addr,       /* Returned IDAW content @IWZ*/
                      U16 *len,         /* Returned IDA data length  */
                      BYTE *chanstat)   /* Returned channel status   */
{
RADR    idaw;                           /* Contents of IDAW      @IWZ*/
U32     idaw1;                          /* Format-1 IDAW         @IWZ*/
U64     idaw2;                          /* Format-2 IDAW         @IWZ*/
RADR    idapage;                        /* Addr of next IDA page @IWZ*/
U16     idalen;                         /* #of bytes until next page */
BYTE    storkey;                        /* Storage key               */

    UNREFERENCED_370(dev);
    *addr = 0;
    *len = 0;

    /* Channel program check if IDAW is not on correct           @IWZ
       boundary or is outside limit of main storage */
    if ((idawaddr & ((idawfmt == 2) ? 0x07 : 0x03))            /*@IWZ*/
        || CHADDRCHK(idawaddr, dev)
        /* Program check if Format-0 CCW and IDAW address > 16M      */
        /* SA22-7201-05:                                             */
        /*  p. 16-25, Invalid IDAW Address                           */
        || (!(dev->orb.flag5 & ORB5_F) && (idawaddr & 0xFF000000)))
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel protection check if IDAW is fetch protected */
    storkey = STORAGE_KEY(idawaddr, dev);
    if (ccwkey != 0 && (storkey & STORKEY_FETCH)
        && (storkey & STORKEY_KEY) != ccwkey)
    {
        *chanstat = CSW_PROTC;
        return;
    }

    /* Set the main storage reference bit for the IDAW location */
    STORAGE_KEY(idawaddr, dev) |= STORKEY_REF;

    /* Fetch IDAW from main storage */
    if (idawfmt == 2)                                          /*@IWZ*/
    {                                                          /*@IWZ*/
        /* Fetch format-2 IDAW */                              /*@IWZ*/
        FETCH_DW(idaw2, dev->mainstor + idawaddr);             /*@IWZ*/

       #ifndef FEATURE_ESAME                                   /*@IWZ*/
        /* Channel program check in ESA/390 mode
           if the format-2 IDAW exceeds 2GB-1 */               /*@IWZ*/
        if (idaw2 > 0x7FFFFFFF)                                /*@IWZ*/
        {                                                      /*@IWZ*/
            *chanstat = CSW_PROGC;                             /*@IWZ*/
            return;                                            /*@IWZ*/
        }                                                      /*@IWZ*/
       #endif /*!FEATURE_ESAME*/                               /*@IWZ*/

        /* Save contents of format-2 IDAW */                   /*@IWZ*/
        idaw = idaw2;                                          /*@IWZ*/
    }                                                          /*@IWZ*/
    else                                                       /*@IWZ*/
    {                                                          /*@IWZ*/
        /* Fetch format-1 IDAW */                              /*@IWZ*/
        FETCH_FW(idaw1, dev->mainstor + idawaddr);             /*@IWZ*/

        /* Channel program check if bit 0 of
           the format-1 IDAW is not zero */                    /*@IWZ*/
        if (idaw1 & 0x80000000)                                /*@IWZ*/
        {                                                      /*@IWZ*/
            *chanstat = CSW_PROGC;                             /*@IWZ*/
            return;                                            /*@IWZ*/
        }                                                      /*@IWZ*/

        /* Save contents of format-1 IDAW */                   /*@IWZ*/
        idaw = idaw1;                                          /*@IWZ*/
    }                                                          /*@IWZ*/

    /* Channel program check if IDAW data
       location is outside main storage */
    if ( CHADDRCHK(idaw, dev) )
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel program check if IDAW data location is not
       on a page boundary, except for the first IDAW */        /*@IWZ*/
    if (IS_CCW_RDBACK (code))
    {
        if (idaseq > 0 && ((idaw+1) & idapmask) != 0)
        {
            *chanstat = CSW_PROGC;
            return;
        }

        /* Calculate address of next page boundary */
        idapage = (idaw & ~idapmask);
        idalen = (idaw - idapage) + 1;

        /* Return the address and length for this IDAW */
        *addr = idaw;
        *len = idalen;
    }
    else
    {
        if (idaseq > 0 && (idaw & idapmask) != 0)              /*@IWZ*/
        {
            *chanstat = CSW_PROGC;
            return;
        }

        /* Calculate address of next page boundary */          /*@IWZ*/
        idapage = (idaw + idapmask + 1) & ~idapmask;           /*@IWZ*/
        idalen = idapage - idaw;

        /* Return the address and length for this IDAW */
        *addr = idaw;
        *len = idalen;
    }

} /* end function fetch_idaw */

#if defined(FEATURE_MIDAW)                                      /*@MW*/
/*-------------------------------------------------------------------*/
/* FETCH A MODIFIED INDIRECT DATA ADDRESS WORD FROM MAIN STORAGE  @MW*/
/*-------------------------------------------------------------------*/
INLINE void
ARCH_DEP(fetch_midaw) (DEVBLK *dev,     /* -> Device block           */
                       BYTE code,       /* CCW operation code        */
                       BYTE ccwkey,     /* Bits 0-3=key, 4-7=zeroes  */
                       int midawseq,    /* 0=1st MIDAW               */
                       U32 midawadr,    /* Main storage addr of MIDAW*/
                       RADR *addr,      /* Returned MIDAW content    */
                       U16 *len,        /* Returned MIDAW data length*/
                       BYTE *flags,     /* Returned MIDAW flags      */
                       BYTE *chanstat)  /* Returned channel status   */
{
U64     mword1, mword2;                 /* MIDAW high and low words  */
RADR    mdaddr;                         /* Data address from MIDAW   */
U16     mcount;                         /* Count field from MIDAW    */
BYTE    mflags;                         /* Flags byte from MIDAW     */
BYTE    storkey;                        /* Storage key               */
U16     maxlen;                         /* Maximum allowable length  */

    UNREFERENCED_370(dev);

    /* Channel program check if MIDAW is not on quadword
       boundary or is outside limit of main storage */
    if ((midawadr & 0x0F)
        || CHADDRCHK(midawadr, dev) )
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel program check if MIDAW list crosses a page boundary */
    if (midawseq > 0 && (midawadr & PAGEFRAME_BYTEMASK) == 0)
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel protection check if MIDAW is fetch protected */
    storkey = STORAGE_KEY(midawadr, dev);
    if (ccwkey != 0 && (storkey & STORKEY_FETCH)
        && (storkey & STORKEY_KEY) != ccwkey)
    {
        *chanstat = CSW_PROTC;
        return;
    }

    /* Set the main storage reference bit for the MIDAW location */
    STORAGE_KEY(midawadr, dev) |= STORKEY_REF;

    /* Fetch MIDAW from main storage (MIDAW is quadword
       aligned and so cannot cross a page boundary) */
    FETCH_DW(mword1, dev->mainstor + midawadr);
    FETCH_DW(mword2, dev->mainstor + midawadr + 8);

    /* Channel program check in reserved bits are non-zero */
    if (mword1 & 0xFFFFFFFFFF000000ULL)
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Extract fields from MIDAW */
    mflags = mword1 >> 16;
    mcount = mword1 & 0xFFFF;
    mdaddr = (RADR)mword2;

    /* Channel program check if data transfer interrupt flag is set */
    if (mflags & MIDAW_DTI)
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel program check if MIDAW count is zero */
    if (mcount == 0)
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel program check if MIDAW data
       location is outside main storage */
    if ( CHADDRCHK(mdaddr, dev) )
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Channel program check if skipping not in effect
       and the MIDAW data area crosses a page boundary */
    maxlen = (IS_CCW_RDBACK(code)) ?
                mdaddr - (mdaddr & PAGEFRAME_PAGEMASK) + 1 :
                (mdaddr | PAGEFRAME_BYTEMASK) - mdaddr + 1 ;

    if ((mflags & MIDAW_SKIP) == 0 && mcount > maxlen)
    {
        *chanstat = CSW_PROGC;
        return;
    }

    /* Return the data address, length, flags for this MIDAW */
    *addr = mdaddr;
    *len = mcount;
    *flags = mflags;

} /* end function fetch_midaw */                                /*@MW*/
#endif /*defined(FEATURE_MIDAW)*/                               /*@MW*/

/*-------------------------------------------------------------------*/
/* COPY DATA BETWEEN CHANNEL I/O BUFFER AND MAIN STORAGE             */
/*-------------------------------------------------------------------*/
static void
ARCH_DEP(copy_iobuf) (DEVBLK *dev,      /* -> Device block           */
                      BYTE code,        /* CCW operation code        */
                      BYTE flags,       /* CCW flags                 */
                      U32 addr,         /* Data address              */
                      U32 count,        /* Data count                */
                      BYTE ccwkey,      /* Protection key            */
                      BYTE idawfmt,     /* IDAW format (1 or 2)  @IWZ*/
                      U16 idapmask,     /* IDA page size - 1     @IWZ*/
                      BYTE *iobuf,      /* -> Channel I/O buffer     */
                      BYTE *iobufstart, /* -> First byte of buffer   */
                      BYTE *iobufend,   /* -> Last byte of buffer    */
                      BYTE *chanstat,   /* Returned channel status   */
                      U32 *residual,    /* Residual data count       */
                      PREFETCH *prefetch)  /* Prefetch CCW buffer    */
{
BYTE    *iobufptr = 0;                  /* Working I/O buffer addr   */
u_int   ps = 0;                         /* Prefetch entry            */
U32     idawaddr;                       /* Main storage addr of IDAW */
U16     idacount;                       /* IDA bytes remaining       */
int     idaseq;                         /* IDA sequence number       */
RADR    idadata;                        /* IDA data address      @IWZ*/
U16     idalen;                         /* IDA data length           */
int     idasize;                        /* IDAW Size                 */
BYTE    storkey;                        /* Storage key               */
RADR    page,startpage,endpage;         /* Storage key pages         */
BYTE    to_iobuf;                       /* 1=READ, SENSE, or RDBACK  */
BYTE    to_memory;                      /* 1=READ, SENSE, or RDBACK  */
BYTE    readbackwards;                  /* 1=RDBACK                  */
#if defined(FEATURE_MIDAW)                                      /*@MW*/
int     midawseq;                       /* MIDAW counter (0=1st)  @MW*/
U32     midawptr;                       /* Real addr of MIDAW     @MW*/
U16     midawrem;                       /* CCW bytes remaining    @MW*/
U16     midawlen=0;                     /* MIDAW data length      @MW*/
RADR    midawdat=0;                     /* MIDAW data area addr   @MW*/
BYTE    midawflg;                       /* MIDAW flags            @MW*/
#endif /*defined(FEATURE_MIDAW)*/                               /*@MW*/

#if !defined(set_chanstat)
#define set_chanstat(_status)                                          \
do {                                                                   \
    if (prefetch->seq)                                                 \
        prefetch->chanstat[ps] = (_status);                            \
    else                                                               \
        *chanstat = (_status);                                         \
} while(0)
#endif

#if !defined(get_new_prefetch_entry)
#define get_new_prefetch_entry(_idawtype,_idawaddr)                    \
do {                                                                   \
    if (prefetch->seq)                                                 \
    {                                                                  \
        ps = prefetch->seq++;                                          \
        if (prefetch->seq > PF_SIZE)                                   \
        {                                                              \
            *chanstat = CSW_CDC;                                       \
            break;                                                     \
        }                                                              \
        prefetch->ccwaddr[ps] = prefetch->ccwaddr[ps-1];               \
        if ((_idawtype) != PF_NO_IDAW)                                 \
        {                                                              \
            prefetch->idawtype[ps] = (_idawtype);                      \
            prefetch->idawaddr[ps] = (_idawaddr);                      \
        }                                                              \
    }                                                                  \
    *chanstat = 0;                                                     \
} while(0)
#endif

    /* Set current prefetch sequence */
    if (prefetch->seq)
        ps = prefetch->seq - 1;

    /* Channel Data Check if invalid I/O buffer */
    if ((size_t)iobufend < 131072   ||  /* Low host OS storage reference */
        iobuf < iobufstart          ||
        iobuf > iobufend)
    {
        set_chanstat(CSW_CDC);
        return;
    }

    /* Exit if no bytes are to be copied */
    if (count == 0 || dev->is_immed)
    {
        set_chanstat(0);
        return;
    }

    /* Set flags indicating direction of data movement */
    if (IS_CCW_RDBACK(code))
    {
        readbackwards = to_memory = 1;
        to_iobuf = 0;
    }
    else
    {
        readbackwards = 0;
        to_iobuf = code & 0x01;
        to_memory = !to_iobuf;
    }


#if defined(FEATURE_MIDAW)                                      /*@MW*/
    /* Move data when modified indirect data addressing is used */
    if (flags & CCW_FLAGS_MIDAW)
    {
        if (prefetch->seq)
            prefetch->datalen[ps] = 0;

        midawptr = addr;
        midawrem = count;
        midawflg = 0;

        for (midawseq = 0;
             midawrem > 0 &&
                (midawflg & MIDAW_LAST) == 0 &&
                chanstat != 0 &&
                !(prefetch->seq &&
                    (prefetch->chanstat[ps] ||
                        ((ps+1) < prefetch->seq &&
                            prefetch->chanstat[ps+1])));
             midawseq++)
        {
            /* Get new prefetch entry */
            get_new_prefetch_entry(PF_MIDAW, midawptr);
            if (*chanstat != 0)
                break;

            /* Fetch MIDAW and set data address, length, flags */
            ARCH_DEP(fetch_midaw) (dev, code, ccwkey,
                    midawseq, midawptr,
                    &midawdat, &midawlen, &midawflg, chanstat);

            /* Exit if fetch_midaw detected channel program check */
            if (prefetch->seq)
            {
                prefetch->idawflag[ps] = midawflg;
                prefetch->dataaddr[ps] = midawdat;
                prefetch->datalen[ps] = midawlen;
                if (*chanstat != 0)
                {
                    prefetch->chanstat[ps] = *chanstat;
                    *chanstat = 0;
                    break;
                }
            }
            else if (*chanstat != 0)
                break;

            /* Channel program check if MIDAW length
               exceeds the remaining CCW count */
            if (midawlen > midawrem)
            {
                set_chanstat(CSW_PROGC);
                return;
            }

            /* MIDAW length may be zero during prefetch operations */
            if (!prefetch->seq || (prefetch->seq && midawlen))
            {

                /* Perform data movement unless SKIP flag is set in
                   MIDAW */
                if ((midawflg & MIDAW_SKIP) ==0)
                {
                    /* Note: MIDAW data area cannot cross a page
                       boundary. The fetch_midaw function enforces this
                       restriction */

                    /* Channel protection check if MIDAW data location
                       is fetch protected, or if location is store
                       protected and command is READ, READ BACKWARD, or
                       SENSE */
                    storkey = STORAGE_KEY(midawdat, dev);
                    if (ccwkey != 0
                        && (storkey & STORKEY_KEY) != ccwkey
                        && ((storkey & STORKEY_FETCH) || to_memory))
                    {
                        set_chanstat(CSW_PROTC);
                        return;
                    }

                    /* Ensure memcpy will stay within buffer         */
                    /* Channel data check if outside buffer          */
                    /* SA22-7201-05:                                 */
                    /*  p. 16-27, Channel-Data Check                 */
                    if (readbackwards)
                    {
                        iobufptr = iobuf + dev->curblkrem + midawrem -
                                   midawlen;
                        if (!midawlen                        ||
                            (iobufptr + midawlen) > iobufend ||
                            iobufptr < iobufstart)
                        {
                            *chanstat = CSW_CDC;
                            return;
                        }
                    }
                    else if (!midawlen                     ||
                             (iobuf + midawlen) > iobufend ||
                             iobuf < iobufstart)
                    {
                            set_chanstat(CSW_CDC);
                            return;
                    }

                    /* Set the main storage reference and change
                       bits */
                    STORAGE_KEY(midawdat, dev) |= (to_memory ?
                            (STORKEY_REF|STORKEY_CHANGE) :
                             STORKEY_REF);

                    /* Copy data between main storage and channel
                       buffer */
                    if (readbackwards)
                    {
                        midawdat = (midawdat - midawlen) + 1;
                        memcpy_backwards (dev->mainstor + midawdat,
                                          iobufptr,
                                          midawlen);

                        /* Decrement buffer pointer */
                        iobuf -= midawlen;
                    }
                    else
                    {
                        if (to_iobuf)
                        {
                            memcpy (iobuf,
                                    dev->mainstor + midawdat,
                                    midawlen);
                            prefetch->pos += midawlen;
                            if (prefetch->seq)
                                prefetch->datalen[ps] = midawlen;
                        }
                        else
                            memcpy (dev->mainstor + midawdat,
                                    iobuf,
                                    midawlen);

                        /* Increment buffer pointer */
                        iobuf += midawlen;
                    }

                } /* end if(!MIDAW_FLAG_SKIP) */

                /* Display the MIDAW if CCW tracing is on */
                if (!prefetch->seq && (dev->ccwtrace || dev->ccwstep))
                {
                    display_idaw (dev, PF_MIDAW, midawflg, midawdat,
                                  midawlen);
                }

                #if DEBUG_DUMP
                if (dev->ccwtrace)
                {
                    if (to_memory)
                        dump("iobuf:", iobuf, midawlen);
                    dump_storage("Storage:", midawdat, midawlen);
                    if (to_iobuf)
                        dump("iobuf:", iobuf, midawlen);
                }
                #endif

                /* Decrement remaining count */
                midawrem -= midawlen;

            }

            /* Increment to next MIDAW address */
            midawptr += 16;

        } /* end for(midawseq) */

        /* Channel program check if sum of MIDAW lengths
           did not exhaust the CCW count and no pending status */
        if (midawrem > 0    &&
            *chanstat != 0  &&
            !(prefetch->seq && prefetch->chanstat[ps]))
            set_chanstat(CSW_PROGC);

    } /* end if(CCW_FLAGS_MIDAW) */                             /*@MW*/
    else                                                        /*@MW*/
#endif /*defined(FEATURE_MIDAW)*/                               /*@MW*/
    /* Move data when indirect data addressing is used */
    if (flags & CCW_FLAGS_IDA)
    {
        if (prefetch->seq)
            prefetch->datalen[ps] = 0;

        idawaddr = addr;
        idacount = count;
        idasize = (idawfmt == 1) ? 4 : 8;

        for (idaseq = 0;
             idacount > 0 &&
                chanstat != 0 &&
                !(prefetch->seq &&
                    (prefetch->chanstat[ps] ||
                        ((ps+1) < prefetch->seq &&
                            prefetch->chanstat[ps+1])));
             idaseq++)
        {
            /* Get new prefetch entry */
            get_new_prefetch_entry(idawfmt, idawaddr);
            if (*chanstat != 0)
                break;

            /* Fetch the IDAW and set IDA pointer and length */
            ARCH_DEP(fetch_idaw) (dev, code, ccwkey, idawfmt,  /*@IWZ*/
                        idapmask, idaseq, idawaddr,            /*@IWZ*/
                        &idadata, &idalen, chanstat);

            /* Exit if fetch_idaw detected channel program check */
            if (prefetch->seq)
            {
                prefetch->dataaddr[ps] = idadata;
                prefetch->datalen[ps] = idalen;
                if (*chanstat != 0)
                {
                    prefetch->chanstat[ps] = *chanstat;
                    *chanstat = 0;
                    break;
                }
            }
            else if (*chanstat != 0)
                break;

            /* Channel protection check if IDAW data location is
               fetch protected, or if location is store protected
               and command is READ, READ BACKWARD, or SENSE */
            storkey = STORAGE_KEY(idadata, dev);
            if (ccwkey != 0 && (storkey & STORKEY_KEY) != ccwkey
                && ((storkey & STORKEY_FETCH) || to_memory))
            {
                set_chanstat(CSW_PROTC);
                break;
            }

            /* Reduce length if less than one page remaining */
            if (idalen > idacount)
            {
                idalen = idacount;
                if (prefetch->seq)
                   prefetch->datalen[ps] = idacount;
            }

            /* Ensure memcpy will stay within buffer         */
            /* Channel data check if outside buffer          */
            /* SA22-7201-05:                                 */
            /*  p. 16-27, Channel-Data Check                 */
            if (readbackwards)
            {
                iobufptr = iobuf + dev->curblkrem + idacount -
                           idalen;
                if ((iobufptr + idalen) > (iobufend + 1) ||
                    (iobufptr + idalen) <= iobufstart)
                {
                    *chanstat = CSW_CDC;
                    return;
                }
                if (iobufptr < iobufstart)
                {
                    *chanstat = CSW_CDC;

                    /* Reset length to copy to buffer */
                    idalen = iobufptr + idalen - iobufstart;
                }
            }
            else if (iobuf < iobufstart || iobuf > iobufend)
            {
                set_chanstat(CSW_CDC);
                break;
            }
            else if ((iobuf + idalen) > (iobufend + 1))
            {
                /* Reset length to copy to buffer */
                idalen = iobufend - iobuf + 1;
                if (prefetch->seq)
                {
                    prefetch->datalen[ps] = idalen;

                    /* Get new prefetch entry for channel data
                       check */
                    get_new_prefetch_entry(idawfmt, idawaddr);
                    if (*chanstat != 0)
                        break;
                    prefetch->dataaddr[ps] = idadata + idalen;
                    prefetch->datalen[ps] = 1;
                    prefetch->chanstat[ps] = *chanstat = CSW_CDC;
                    ps -= 1;
                }

                /* Set channel data check and permit copy to/from
                   end-of-buffer */
                else
                    set_chanstat(CSW_CDC);
            }

            /* Copy to I/O buffer */
            if (idalen)
            {
                /* Set the main storage reference and change bits */
                STORAGE_KEY(idadata, dev) |=
                    (to_memory ?
                     (STORKEY_REF|STORKEY_CHANGE) : STORKEY_REF);

                /* Copy data between main storage and channel buffer */
                if (readbackwards)
                {
                    idadata = (idadata - idalen) + 1;
                    memcpy_backwards (dev->mainstor + idadata,
                                      iobuf + dev->curblkrem +
                                        idacount - idalen,
                                     idalen);

                    /* Decrement buffer pointer for next IDAW*/
                    iobuf -= idalen;

                }
                else
                {
                    if (to_iobuf)
                    {
                        memcpy (iobuf,
                                dev->mainstor + idadata,
                                idalen);
                    }
                    else
                        memcpy (dev->mainstor + idadata,
                                iobuf,
                                idalen);

                    /* Increment buffer pointer for next IDAW*/
                    iobuf += idalen;
                }

                /* Update prefetch completed bytes */
                prefetch->pos += idalen;
            }

            /* If not prefetch, display the IDAW if CCW tracing */
            else if (dev->ccwtrace || dev->ccwstep)
                display_idaw (dev, idawfmt, 0, idadata, idalen);

            #if DEBUG_DUMP
            if (dev->ccwtrace)
            {
                if (to_memory)
                    dump("iobuf:", iobuf-idalen, idalen);
                dump_storage("Storage:", idadata, idalen);
                if (to_iobuf)
                    dump("iobuf:", iobuf-idalen, idalen);
            }
            #endif

            /* Decrement remaining count, increment buffer pointer */
            idacount -= idalen;

            /* Increment to next IDAW address */
            idawaddr += idasize;

        } /* end for(idaseq) */

    }

    else                              /* Non-IDA data addressing */
    {

        /* Point to start of data for read backward command */
        if (readbackwards)
            addr = addr - (count - 1);

        /* Channel protection check if any data is fetch protected,
           or if location is store protected and command is READ,
           READ BACKWARD, or SENSE */
        startpage = addr;
        endpage = addr + (count - 1);
        for (page = startpage & STORAGE_KEY_PAGEMASK;
             page <= (endpage | STORAGE_KEY_BYTEMASK);
             page += STORAGE_KEY_PAGESIZE)
        {
            /* Channel program check if data is outside main storage */
            if ( CHADDRCHK(page, dev) )
            {
                if (prefetch->seq)
                {
                    prefetch->chanstat[ps] = CSW_PROGC;
                    break;
                }
                *chanstat = CSW_PROGC;
                if (readbackwards)
                    return;
                break;
            }

            storkey = STORAGE_KEY(page, dev);
            if (ccwkey != 0 && (storkey & STORKEY_KEY) != ccwkey
                && ((storkey & STORKEY_FETCH) || to_memory))
            {
                if (readbackwards)
                {
                    *residual = MAX(page, addr) - addr;
                    *chanstat = CSW_PROTC;
                    break;
                }

                /* Calculate residual */
                *residual = count - (MAX(page, addr) - addr);

                /* Handle prefetch */
                if (prefetch->seq)
                {
                    prefetch->datalen[ps] = MAX(page, addr) - addr;
                    if (*residual)
                    {
                        /* Split entry */
                        get_new_prefetch_entry(PF_NO_IDAW, page);
                        if (*chanstat != 0)
                            break;
                        prefetch->dataaddr[ps] = page;
                        prefetch->datalen[ps] = *residual;
                    }
                    prefetch->chanstat[ps] = CSW_PROTC;
                    break;
                }

                *chanstat = CSW_PROTC;
                break;
            }

        } /* end for(page) */

        /* Adjust local count for copy to main storage */
        count = MIN(count, MAX(page, addr) - addr);

        /* Count may be zero during prefetch operations */
        if (count)
        {
            /* Set the main storage reference and change bits */
            for (page = startpage & STORAGE_KEY_PAGEMASK;
                 page <= (endpage | STORAGE_KEY_BYTEMASK);
                 page += STORAGE_KEY_PAGESIZE)
            {
                STORAGE_KEY(page, dev) |=
                    (to_memory ?
                     (STORKEY_REF|STORKEY_CHANGE) : STORKEY_REF);
            } /* end for(page) */

            if (DEBUG_PREFETCH && dev->ccwtrace)
            {
                char msgbuf[133];
                MSGBUF(msgbuf,
                       "CCW %2.2X %2.2X %4.4X %8.8X "
                       "to_memory=%d "
                       "to_iobuf=%d "
                       "readbackwards=%d",
                       (U8)code, (U8)flags, (U16)count, (U32)addr,
                       to_memory, to_iobuf, readbackwards);
                WRMSG(HHC90000, "I", msgbuf);
            }

            /* Copy data between main storage and channel buffer */
            if (readbackwards)
            {
                BYTE *iobufptr = iobuf + dev->curblkrem;

                /* Channel check if outside buffer                   */
                /* SA22-7201-05:                                     */
                /*  p. 16-27, Channel-Data Check                     */
                if (!count                              ||
                    (iobufptr + count) > (iobufend + 1) ||
                    iobufptr < iobufstart)
                    *chanstat = CSW_CDC;
                else
                    /* read backward  - use END of buffer */
                    memcpy_backwards(dev->mainstor + addr, iobufptr,
                                     count);
            }

            /* Channel check if outside buffer                       */
            /* SA22-7201-05:                                         */
            /*  p. 16-27, Channel-Data Check                         */
            else if (!count                           ||
                     (iobuf + count) > (iobufend + 1) ||
                     iobuf < iobufstart)
                set_chanstat(CSW_CDC);

            /* Handle Write and Control transfer to I/O buffer */
            else if (to_iobuf)
            {
                memcpy (iobuf, dev->mainstor + addr, count);
                prefetch->pos += count;
                if (prefetch->seq)
                    prefetch->datalen[ps] = count;

            }

            /* Handle Read transfer from I/O buffer */
            else
            {
                memcpy (dev->mainstor + addr, iobuf, count);
            }
            #if DEBUG_DUMP
            if (dev->ccwtrace)
            {
                char msgbuf[133];
                MSGBUF(msgbuf,
                       "iobuf->%8.8X.%4.4X",
                       addr, count);
                WRMSG(HHC90000, "I", msgbuf);
                MSGBUF(msgbuf,
                       "addr=%8.8X "
                       "count=%d residual=%d copy=%d",
                       addr, count, *residual, count);
                WRMSG(HHC90000, "I", msgbuf);
                if (to_memory)
                    dump("iobuf:", iobuf, count);
                dump_storage("Storage:", addr, count);
                if (to_iobuf)
                    dump("iobuf:", iobuf, count);
            }
            #endif

        }

    } /* end if(!IDA) */

#undef get_new_prefetch_entry
#undef set_chanstat

} /* end function copy_iobuf */


/*-------------------------------------------------------------------*/
/* DEVICE ATTENTION                                                  */
/* Raises an unsolicited interrupt condition for a specified device. */
/* Return value is 0 if successful, 1 if device is busy or pending   */
/* or 3 if subchannel is not valid or not enabled                    */
/*                                                                   */
/* SA22-7204-00:                                                     */
/*  p. 4-1, Attention                                                */
/* GA22-6974-09:                                                     */
/*  pp. 2-13 -- 2-14, Attention                                      */
/*                                                                   */
/* Note:  An unsolicited interrupt does not always have the          */
/*        attention bit set, and the software differentiates between */
/*        an unsolicited interrupt with the attention bit set and    */
/*        not set. A good example of this are the 3270 device types  */
/*        where the attention bit set on an unsolicited interrupt    */
/*        indicates "data available". Consequently, it is the        */
/*        calling routine's responsibility to set the attention bit, */
/*        when required.                                             */
/*                                                                   */
/*        For further reference, refer to GA23-0218-11, IBM          */
/*        3174 Function Description, 3.1.3.2.3 Unsolicited           */
/*        Status, Table  5-9. Unsolicited Status Conditions          */
/*        (Non-SNA).                                                 */
/*-------------------------------------------------------------------*/
DLL_EXPORT int
ARCH_DEP(device_attention) (DEVBLK *dev, BYTE unitstat)
{
    obtain_lock (&dev->lock);

    if (dev->hnd->attention) (dev->hnd->attention) (dev);

#ifdef FEATURE_CHANNEL_SUBSYSTEM
    /* If subchannel not valid and enabled, do not present interrupt */
    if ((dev->pmcw.flag5 & PMCW5_V) == 0
     || (dev->pmcw.flag5 & PMCW5_E) == 0)
    {
        release_lock (&dev->lock);
        return 3;
    }
#endif /*FEATURE_CHANNEL_SUBSYSTEM*/


    /* If device is already busy or interrupt pending */
    if (dev->busy       ||
        IOPENDING(dev)  ||
        (dev->scsw.flag3 & SCSW3_SC_PEND))
    {
        int rc;                         /* Return code               */

        /* Resume the suspended device with attention set            */
        /* SA22-7204-00:                                             */
        /*  p. 4-1, Attention                                        */
        if(dev->scsw.flag3 & SCSW3_AC_SUSP)
        {
            dev->scsw.flag3 |= SCSW3_SC_ALERT |
                               SCSW3_SC_PEND;
            dev->scsw.unitstat |= unitstat |= CSW_ATTN;
            dev->scsw.flag2 |= SCSW2_AC_RESUM;
            schedule_ioq(NULL, dev);
            if (dev->ccwtrace || dev->ccwstep)
                WRMSG (HHC01304, "I", SSID_TO_LCSS(dev->ssid),
                                      dev->devnum);
            rc = 0;
        }
        else
            rc = 1;

        release_lock (&dev->lock);

        return (rc);
    }

    if (dev->ccwtrace || dev->ccwstep)
        WRMSG (HHC01305, "I", SSID_TO_LCSS(dev->ssid), dev->devnum);

    /* Switch to INTLOCK context for remainder of attention */
    release_lock(&dev->lock);
    OBTAIN_INTLOCK(NULL);
    obtain_lock(&dev->lock);
    obtain_lock(&sysblk.iointqlk);

    /* Set SCSW for attention interrupt                              */
    /* SA22-7201-05:                                                 */
    /*  p. 16-3, Unsolicited Interuption Condition                   */
    /*           Solicited Interuption Condition                     */
    /*           Figure 16-1, Interruption Condition for Status-     */
    /*                        Control-Bit Combinations               */
    /*  p. 16-4, Alert Interruption Condition                        */
    /*  p. 16-16 -- 16-17, Alert Status (Bit 27)                     */
    /*  p. 16-18, Status-Pending (Bit 31)                            */
    /*                                                               */
    /*  Hercules maintains for tracking purposes regardless of       */
    /*  architecture.                                                */
    /*                                                               */
    /* Set CSW for attention interrupt when in S/360 or S/370 mode,  */
    /* CSW will be derived from the SCSW when interrupt is issued    */
    /* SA22-7204-00:                                                 */
    /*  p. 4-1, Attention                                            */
    /* GA22-6974-09:                                                 */
    /*  pp. 2-13 -- 2-14, Attention                                  */
    dev->attnscsw.flag3 = SCSW3_SC_ALERT | SCSW3_SC_PEND;
    store_fw (dev->attnscsw.ccwaddr, 0);
    dev->attnscsw.unitstat = unitstat;
    dev->attnscsw.chanstat = 0;
    store_hw (dev->attnscsw.count, 0);

    /* Queue the attention interrupt */
    QUEUE_IO_INTERRUPT_QLOCKED(&dev->attnioint);

    /* Update interrupt status */
    subchannel_interrupt_queue_cleanup(dev);
    UPDATE_IC_IOPENDING_QLOCKED();
    release_lock(&sysblk.iointqlk);
    release_lock(&dev->lock);
    RELEASE_INTLOCK(NULL);

    return 0;
} /* end function device_attention */


/*-------------------------------------------------------------------*/
/* START A CHANNEL PROGRAM                                           */
/* This function is called by the SIO and SSCH instructions          */
/*-------------------------------------------------------------------*/
/* Input                                                             */
/*      dev     -> Device control block                              */
/*      orb     -> Operation request block                       @IWZ*/
/* Output                                                            */
/*      The I/O parameters are stored in the device block, and a     */
/*      thread is created to execute the CCW chain asynchronously.   */
/*      The return value is the condition code for the SIO or        */
/*      SSCH instruction.                                            */
/* Note                                                              */
/*      For S/370 SIO, only the protect key and CCW address are      */
/*      valid, all other ORB parameters are set to zero.             */
/*-------------------------------------------------------------------*/
int
ARCH_DEP(startio) (REGS *regs, DEVBLK *dev, ORB *orb)          /*@IWZ*/
{
int     rc;                             /* Return code               */

    obtain_lock (&dev->lock);

#ifdef OPTION_SYNCIO
    dev->regs = NULL;
    dev->syncio_active = dev->syncio_retry = 0;
#endif // OPTION_SYNCIO

#if defined(_FEATURE_IO_ASSIST)
    if(SIE_MODE(regs)
      && (regs->siebk->zone != dev->pmcw.zone
        || !(dev->pmcw.flag27 & PMCW27_I)))
    {
        release_lock (&dev->lock);
        longjmp(regs->progjmp,SIE_INTERCEPT_INST);
    }
#endif

    /* Return condition code 1 if status pending */
    if (unlikely((dev->scsw.flag3     & SCSW3_SC_PEND)  ||
                 (dev->pciscsw.flag3  & SCSW3_SC_PEND)  ||
                 (dev->attnscsw.flag3 & SCSW3_SC_PEND)  ||
                 dev->tschpending))
    {
        release_lock (&dev->lock);
        return 1;
    }

    /* Return condition code 2 if device is busy */
    if (unlikely((dev->busy &&
                  dev->ioactive == DEV_SYS_LOCAL)   ||
                 dev->startpending))
    {
        release_lock (&dev->lock);
        return 2;
    }

    /* Ensure clean status flag bits */
    dev->suspended          = 0;
    dev->pending            = 0;
    dev->pcipending         = 0;
    dev->attnpending        = 0;
    dev->startpending       = 0;
    dev->resumesuspended    = 0;
    dev->tschpending        = 0;

    /* Initialize the subchannel status word */
    memset (&dev->scsw,     0, sizeof(SCSW));
    dev->scsw.flag0 = (orb->flag4 & (SCSW0_KEY |
                                     SCSW0_S));
    dev->scsw.flag1 = (orb->flag5 & (SCSW1_F   |
                                     SCSW1_P   |
                                     SCSW1_I   |
                                     SCSW1_A   |
                                     SCSW1_U));

    /* Set the device busy indicator */
    set_subchannel_busy(dev);

    /* Initialize shadow SCSWs */
    memcpy(&dev->pciscsw,  &dev->scsw, sizeof(SCSW));
    memcpy(&dev->attnscsw, &dev->scsw, sizeof(SCSW));

    /* Make the subchannel start-pending */
    dev->scsw.flag2 |= SCSW2_FC_START | SCSW2_AC_START;
    dev->startpending = 1;

    /* Copy the I/O parameter to the path management control word */
    memcpy (dev->pmcw.intparm, orb->intparm,                   /*@IWZ*/
                        sizeof(dev->pmcw.intparm));            /*@IWZ*/

    /* Store the start I/O parameters in the device block */
    if (orb->flag7 & ORB7_X)
    {
        /* Extended ORB */
        memcpy(&dev->orb, orb, sizeof(ORB));
    }
    else
    {
        /* Original ORB size */
        memcpy(&dev->orb, orb, 12);
        memset(&dev->orb.csspriority, 0, sizeof(ORB) - 12);
    }

    /* Set I/O priority */
    dev->priority &= 0x00FF0000ULL;
    dev->priority |= dev->orb.csspriority << 8;
    dev->priority |= dev->orb.cupriority;

    /* Schedule the I/O for execution */
    rc = schedule_ioq((sysblk.arch_mode == ARCH_370) ? regs : NULL,
                      dev);

    /* Done; release locks and return */
    release_lock (&dev->lock);
    return (rc);

} /* end function startio */


/*-------------------------------------------------------------------*/
/* EXECUTE A CHANNEL PROGRAM                                         */
/*-------------------------------------------------------------------*/
void*
ARCH_DEP(execute_ccw_chain) (void *arg)
{
DEVBLK *dev = (DEVBLK*) arg;            /* Device Block pointer      */
IOBUF  *iobuf;                          /* Pointer to I/O buffer     */
int     sysid = DEV_SYS_LOCAL;          /* System Identifier         */
U32     ccwaddr = 0;                    /* Address of CCW        @IWZ*/
U32     ticaddr = 0;                    /* Previous CCW was a TIC    */
U16     ticback = 0;                    /* Backwards TIC counter     */
U16     idapmask = 0;                   /* IDA page size - 1     @IWZ*/
BYTE    idawfmt = 0;                    /* IDAW format (1 or 2)  @IWZ*/
BYTE    ccwfmt = 0;                     /* CCW format (0 or 1)   @IWZ*/
BYTE    ccwkey = 0;                     /* Bits 0-3=key, 4-7=zero@IWZ*/
BYTE    opcode;                         /* CCW operation code        */
BYTE    flags;                          /* CCW flags                 */
U32     addr;                           /* CCW data address          */
#ifdef FEATURE_CHANNEL_SUBSYSTEM
U32     mbaddr;                         /* Measure block address     */
MBK    *mbk;                            /* Measure block             */
U16     mbcount;                        /* Measure block count       */
#endif /*FEATURE_CHANNEL_SUBSYSTEM*/
U32     count;                          /* CCW byte count            */
BYTE   *ccw;                            /* CCW pointer               */
BYTE    unitstat;                       /* Unit status               */
BYTE    chanstat;                       /* Channel status            */
U32     residual = 0;                   /* Residual byte count       */
BYTE    more;                           /* 1=Count exhausted         */
BYTE    chain = 1;                      /* 1=Chain to next CCW       */
BYTE    tracethis = 0;                  /* 1=Trace this CCW only     */
BYTE    firstccw = 1;                   /* 1=First CCW               */
BYTE    area[64];                       /* Message area              */
u_int   bufpos = 0;                     /* Position in I/O buffer    */
u_int   skip_ccws = 0;                  /* Skip ccws                 */
int     cmdretry = 255;                 /* Limit command retry       */
U32     prevccwaddr = 1;                /* Previous CCW address      */
U32     prefetch_remaining;             /* Prefetch bytes remaining  */

u_int       ps = 0;                     /* Local prefetch sequence   */
u_int       ts;                         /* Test prefetch sequence    */

CACHE_ALIGN
PREFETCH    prefetch;                   /* Prefetch buffer           */

ALIGN_4K
IOBUF iobuf_initial;                    /* Channel I/O buffer        */

#if !defined(execute_ccw_chain_return)

#define execute_ccw_chain_fast_return(_result)                         \
do {                                                                   \
    if (iobuf != &iobuf_initial)                                       \
        iobuf_destroy(iobuf);                                          \
    return (void*) (_result);                                          \
} while(0)

#define execute_ccw_chain_unlock_and_return(_result)                   \
do {                                                                   \
    clear_subchannel_busy(dev);                                        \
    release_lock(&dev->lock);                                          \
    execute_ccw_chain_fast_return(_result);                            \
} while(0)

#define execute_ccw_chain_return(_result)                              \
do {                                                                   \
    obtain_lock(&dev->lock);                                           \
    execute_ccw_chain_unlock_and_return(_result);                      \
} while(0)
#endif

    /* Initialize prefetch */
    memset(&prefetch, 0, sizeof(prefetch));

    /* Point to initial I/O buffer and initialize */
    iobuf = &iobuf_initial;
    iobuf_initialize(iobuf, sizeof(iobuf_initial.data));

    /* Wait for the device to become available */
    obtain_lock (&dev->lock);

#ifdef OPTION_SYNCIO
    if (!dev->syncio_active && dev->shared)
#else // OPTION_NOSYNCIO
    if (dev->shared)
#endif // OPTION_SYNCIO
    {
        while (dev->ioactive != DEV_SYS_NONE
            && dev->ioactive != sysid)
        {
            dev->iowaiters++;
            wait_condition(&dev->iocond, &dev->lock);
            dev->iowaiters--;
        }
        set_subchannel_busy(dev);
        dev->ioactive = sysid;
        if (sysid == DEV_SYS_LOCAL)
            dev->startpending = 0;
    }
    else
    {
        set_subchannel_busy(dev);
        dev->ioactive = DEV_SYS_LOCAL;
        dev->startpending = 0;
    }

    /* Increment excp count */
    dev->excps++;

    /* Indicate that we're started */
    dev->scsw.flag2 |= SCSW2_FC_START;
    dev->scsw.flag2 &= ~SCSW2_AC_START;

    /* Handle early clear or halt */
    if (dev->scsw.flag2 & (SCSW2_AC_CLEAR | SCSW2_AC_HALT))
    {
        release_lock(&dev->lock);
        if (dev->scsw.flag2 & SCSW2_AC_CLEAR)
            goto execute_clear;
        goto execute_halt;
    }

    /* For hercules `resume' resume suspended state */
    if (dev->resumesuspended || dev->suspended ||
        (dev->scsw.flag2 & SCSW2_AC_RESUM))
    {
        /* Restore CCW execution variables */
        ccwaddr = dev->ccwaddr;
        idapmask = dev->idapmask;
        idawfmt = dev->idawfmt;
        ccwfmt = dev->ccwfmt;
        ccwkey = dev->ccwkey;


        /* Turn the `suspended' bits off */
        dev->suspended =
        dev->resumesuspended=0;

        /* Wait for the device to become available */
        while (dev->ioactive != DEV_SYS_NONE
            && dev->ioactive != sysid)
        {
            dev->iowaiters++;
            wait_condition(&dev->iocond, &dev->lock);
            dev->iowaiters = MAX(0, dev->iowaiters - 1);
        }
        dev->ioactive = sysid;
        set_device_busy(dev);

        if (dev->ccwtrace || dev->ccwstep || tracethis)
        {
            if (dev->s370start)
            {
                /* State successful conversion from synchronous to
                 * asynchronous for 370 mode.
                 */
                WRMSG (HHC01321, "I", SSID_TO_LCSS(dev->ssid),
                                      dev->devnum);
            }
            else
            {
                /* Trace I/O resumption */
                WRMSG (HHC01311, "I", SSID_TO_LCSS(dev->ssid),
                                      dev->devnum);
            }
        }

        /* Reset the suspended status in the SCSW */
        dev->scsw.flag2 &= ~SCSW2_AC_RESUM;
        dev->scsw.flag3 &= ~(SCSW3_AC_SUSP  |
                             SCSW3_SC_ALERT |
                             SCSW3_SC_INTER |
                             SCSW3_SC_PEND);
        dev->scsw.flag3 |= (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC);

        /* Call the i/o resume exit if not clearing */
        if (!(dev->scsw.flag2 & SCSW2_AC_CLEAR))
        {
            /* Don't execute resume exit if S/370 channel start */
            if (!dev->s370start &&
                dev->hnd->resume)
                (dev->hnd->resume) (dev);

            /* Refetch the suspended CCW */
            ccwaddr -= 8;
        }

        /* Reset S/370 channel SIO resume indicator */
        dev->s370start = 0;

        goto resume_suspend;
    }

    /* Hercules deviation; always zero the SCSW CCW address to start */
    store_fw(dev->scsw.ccwaddr, 0);

    /* Call the i/o start exit */
#ifdef OPTION_SYNCIO
    if (!dev->syncio_retry && dev->hnd->start)
#else // OPTION_NOSYNCIO
    if (dev->hnd->start)
#endif // OPTION_SYNCIO
    {
        release_lock (&dev->lock);
        (dev->hnd->start) (dev);
        obtain_lock (&dev->lock);
    }

    /* Extract the I/O parameters from the ORB */              /*@IWZ*/
    FETCH_FW(ccwaddr, dev->orb.ccwaddr);                       /*@IWZ*/
    dev->ccwaddr = ccwaddr;
    dev->ccwfmt = ccwfmt = (dev->orb.flag5 & ORB5_F) ? 1 : 0;
    dev->ccwkey = ccwkey = dev->orb.flag4 & ORB4_KEY;
    dev->idawfmt = idawfmt = (dev->orb.flag5 & ORB5_H) ? 2 : 1;

    /* Determine IDA page size */                              /*@IWZ*/
    if (idawfmt == 2)                                          /*@IWZ*/
    {                                                          /*@IWZ*/
        /* Page size is 2K or 4K depending on flag bit */      /*@IWZ*/
        idapmask =                                             /*@IWZ*/
            (dev->orb.flag5 & ORB5_T) ? 0x7FF : 0xFFF;         /*@IWZ*/
    } else {                                                   /*@IWZ*/
        /* Page size is always 2K for format-1 IDAW */         /*@IWZ*/
        idapmask = 0x7FF;                                      /*@IWZ*/
    }                                                          /*@IWZ*/


resume_suspend:

    /* Turn off the start pending bit in the SCSW */
    dev->scsw.flag2 &= ~SCSW2_AC_START;

#ifdef OPTION_SYNCIO
    /* Check for retried synchronous I/O */
    if (dev->syncio_retry)
    {
        dev->syncios--; dev->asyncios++;
        ccwaddr = dev->syncio_addr;
        dev->code = dev->prevcode;
        dev->prevcode = 0;
        dev->chained &= ~CCW_FLAGS_CD;
        dev->prev_chained = 0;
        logdevtr (dev, MSG(HHC01334, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, ccwaddr));
    }
    else
#endif // OPTION_SYNCIO
    {
        dev->chained = dev->prev_chained =
        dev->code    = dev->prevcode     = dev->ccwseq = 0;
    }

#ifdef OPTION_SYNCIO
    /* Check for synchronous I/O */
    if (dev->syncio_active)
    {
        dev->syncios++;
        logdevtr (dev, MSG(HHC01335, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, ccwaddr));
    }
#endif // OPTION_SYNCIO

#if defined(_FEATURE_IO_ASSIST)
 #define _IOA_MBO sysblk.zpb[dev->pmcw.zone].mbo
 #define _IOA_MBM sysblk.zpb[dev->pmcw.zone].mbm
 #define _IOA_MBK sysblk.zpb[dev->pmcw.zone].mbk
#else /*defined(_FEATURE_IO_ASSIST)*/
 #define _IOA_MBO sysblk.mbo
 #define _IOA_MBM sysblk.mbm
 #define _IOA_MBK sysblk.mbk
#endif /*defined(_FEATURE_IO_ASSIST)*/

#ifdef FEATURE_CHANNEL_SUBSYSTEM
    /* Update the measurement block if applicable */
    if (_IOA_MBM && (dev->pmcw.flag5 & PMCW5_MM_MBU) &&
        !(dev->scsw.flag2 & (SCSW2_AC_CLEAR | SCSW2_AC_HALT)))
    {
        mbaddr = _IOA_MBO;
        mbaddr += (dev->pmcw.mbi[0] << 8 | dev->pmcw.mbi[1]) << 5;
        if ( !CHADDRCHK(mbaddr, dev)
            && (((STORAGE_KEY(mbaddr, dev) & STORKEY_KEY) == _IOA_MBK)
                || (_IOA_MBK == 0)))
        {
            STORAGE_KEY(mbaddr, dev) |= (STORKEY_REF | STORKEY_CHANGE);
            mbk = (MBK*)&dev->mainstor[mbaddr];
            FETCH_HW(mbcount,mbk->srcount);
            mbcount++;
            STORE_HW(mbk->srcount,mbcount);
        } else {
            /* Generate subchannel logout indicating program
               check or protection check, and set the subchannel
               measurement-block-update-enable to zero */
            dev->pmcw.flag5 &= ~PMCW5_MM_MBU;
            dev->esw.scl0 |= !CHADDRCHK(mbaddr, dev) ?
                                 SCL0_ESF_MBPTK : SCL0_ESF_MBPGK;
            /*INCOMPLETE*/
        }
    }
#endif /*FEATURE_CHANNEL_SUBSYSTEM*/

    release_lock (&dev->lock);

    /* Execute the CCW chain */
    /* On entry : No locks held */
    while ( chain )
    {
        /* Test for clear subchannel request or system shutdown */
        if (dev->scsw.flag2 & SCSW2_AC_CLEAR ||
            sysblk.shutdown)
        {

            /* No I/O delays are to be taken during clear operations */

            /* Call the i/o end exit */
            if (dev->hnd->end) (dev->hnd->end) (dev);

execute_clear:
            /* Get necessary locks */
            OBTAIN_INTLOCK(NULL);
            obtain_lock(&dev->lock);

            /* Execute clear function */
            perform_clear_subchan(dev);

            /* Release locks */
            release_lock(&dev->lock);
            RELEASE_INTLOCK(NULL);

            /* Return */
            execute_ccw_chain_fast_return(NULL);
        } /* end perform clear subchannel */

        /* Test for halt subchannel request */
        if (dev->scsw.flag2 & SCSW2_AC_HALT)
        {
            IODELAY(dev);

            /* Call the i/o end exit */
            if (dev->hnd->end) (dev->hnd->end) (dev);

execute_halt:
            perform_halt(dev);

            if (tracethis && !dev->ccwstep)
                WRMSG (HHC01309, "I", SSID_TO_LCSS(dev->ssid),
                                      dev->devnum);

            execute_ccw_chain_fast_return(NULL);
        } /* end perform halt subchannel */

        /* Test for attention status from device */
        if (dev->scsw.flag3 & SCSW3_SC_ALERT)
        {
            IODELAY(dev);

            /* Call the i/o end exit */
            if (dev->hnd->end) (dev->hnd->end) (dev);

            obtain_lock (&dev->lock);

            /* Turn off busy bit, turn on pending bit */
            clear_subchannel_busy(dev);
//          dev->pending = 1;

            /* Queue the pending interrupt and update status */
            release_lock (&dev->lock);
            queue_io_interrupt_and_update_status(dev);

            if (dev->ccwtrace || dev->ccwstep || tracethis)
                WRMSG (HHC01307, "I", SSID_TO_LCSS(dev->ssid),
                                      dev->devnum);

            execute_ccw_chain_fast_return(NULL);
        } /* end attention processing */

        /* Clear the channel status and unit status, unless skipping
           to end of chain data */
        if (!skip_ccws)
        {
            chanstat = 0;
            unitstat = 0;
        }

        /* Fetch the next CCW */
        ARCH_DEP(fetch_ccw) (dev, ccwkey, ccwfmt, ccwaddr, &opcode,
                             &addr, &flags, &count, &chanstat);

        /*************************************************************/
        /* NOTE: Order of process must not only match the Principles */
        /*       of Operations, but must also match the processing   */
        /*       order of real channels.                             */
        /*************************************************************/

        /* For an invalid CCW address in a TIC we must backup to
           TIC+8 */
        if(ticaddr && (chanstat & CSW_PROGC))
            ccwaddr = ticaddr-8;

        /* Point to the CCW in main storage */
        ccw = dev->mainstor + ccwaddr;

        /* Increment to next CCW address */
#ifdef OPTION_SYNCIO
        if ((dev->chained & CCW_FLAGS_CD) == 0)
            dev->syncio_addr = ccwaddr;
#endif // OPTION_SYNCIO
        ccwaddr += 8;

        /* If prefetch, update prefetch table */
        if (prefetch.seq && (dev->chained & CCW_FLAGS_CD))
        {
            ps = prefetch.seq++;
            if (prefetch.seq > PF_SIZE)
                goto breakchain;
            prefetch.ccwaddr[ps] = ccwaddr;
            prefetch.ccwflags[ps] = flags;
            prefetch.ccwcount[ps] =
            prefetch.datalen[ps] = count;
            prefetch.dataaddr[ps] = addr;

            /* Exit if fetch_ccw detected channel program check */
            if (chanstat)
                goto prefetch;
        }
        else
        {
            /* Update the CCW address in the SCSW */
            STORE_FW(dev->scsw.ccwaddr,ccwaddr);

            /* Exit if fetch_ccw detected channel program check */
            if (chanstat != 0)
                goto breakchain;

            /* Display the CCW */
            if (dev->ccwtrace || dev->ccwstep)
                display_ccw (dev, ccw, addr, count, flags);
        }

        /* Channel program check if invalid Format-1 CCW             */
        /* SA22-7201-05:                                             */
        /*  pp. 15-23 -- 15-24, Channel_Command Word                 */
        /*  p. 15-25, Count                                          */
        /*  p. 16-25, Invalid Count, Format 1                        */
        if (ccwfmt == 1 &&                  /* Validate Format 1     */
            ((opcode != 0x08)              &&
             (dev->chained & CCW_FLAGS_CD) &&
             !count))
            {
                chanstat = CSW_PROGC;
                if (prefetch.seq)
                    goto prefetch;
                goto breakchain;
            }

        /* Channel program check if invalid TIC opcode or invalid    */
        /* TIC Format-1 CCW                                          */
        /* SA22-7201-05:                                             */
        /*  p. 15-25, Figure 15-5. Command Code Assignment           */
        /*  p. 15-36, Figure 15-7. Command Codes and Flags           */
        /*  p. 15-37, Transfer in Channel                            */
        if (opcode == 0x08 ||               /* Validate TIC          */
            (ccwfmt == 0 &&
             ((opcode & 0x0F) == 0x08)))
        {
            if (ticaddr                  || /* No TIC-to-TIC         */
                (addr & 0x03)            || /* Must be aligned DWORD */
                (ccwfmt == 1 &&             /* Validate Format 1     */
                 (flags           ||
                  count))                ||
                ticback > 255               /* Exceeded TIC limit?   */
                )
            {
                chanstat = CSW_PROGC;
                if (prefetch.seq)
                {
                    prefetch.ccwflags[ps] = 0;
                    prefetch.datalen[ps] = 0;
                    goto prefetch;
                }
                goto breakchain;
            }

            /* Reuse prefetch entry for next CCW */
            if (prefetch.seq)
            {
                prefetch.ccwaddr[ps] = 0;
                prefetch.ccwflags[ps] = 0;
                prefetch.ccwcount[ps] = 0;
                prefetch.dataaddr[ps] = 0;
                prefetch.datalen[ps] = 0;
                ps--;
                prefetch.seq--;
            }

            /* Update backwards TIC counter */
            if (addr < ccwaddr)
                ticback++;
            else
                ticback = 0;

            /* Set new CCW address (leaving the values of chained and
               code untouched to allow data-chaining through TIC)    */
            ticaddr = ccwaddr;
            ccwaddr = addr;
            chain = 1;
            continue;
        }
        ticaddr = 0;                    /* Reset the TIC-to-TIC flag */

        /* Reset TIC back counter if Read or Write CCW               */
        if (IS_CCW_WRITE(opcode) ||
            IS_CCW_READ(opcode)  ||
            IS_CCW_RDBACK(opcode))
            ticback = 0;

        /* At this point, the CCW now has "control" of the I/O       */
        /* operation (SA22-7201 p. 15-24, PCI). Signal I/O interrupt */
        /* if PCI flag is set                                        */
        if (
#ifdef OPTION_SYNCIO
            !dev->syncio_retry &&
#endif // OPTION_SYNCIO
            (flags & CCW_FLAGS_PCI) /*-- Debug &&
            !prefetch.seq --*/)
        {
            ARCH_DEP(raise_pci) (dev, ccwkey, ccwfmt, ccwaddr);
        }

        /* Validate basic CCW command                                */
        /* SA22-7201-05:                                             */
        /*  p. 15-25, Figure 15-5. Command Code Assignment           */
        /* Note: TIC validation not included as TIC has already been */
        /*       validated and processed.                            */
        if (!(dev->chained & CCW_FLAGS_CD) &&
            !((opcode & 0x0F) != 0      &&
              (IS_CCW_WRITE(opcode)   ||
               IS_CCW_READ(opcode)    ||
               IS_CCW_RDBACK(opcode)  ||
               IS_CCW_CONTROL(opcode) ||
               IS_CCW_SENSE(opcode))))
        {
            chanstat = CSW_PROGC;
            if (prefetch.seq)
                goto prefetch;
            goto breakchain;
        }

        /* Validate chain data (CD) flag                             */
        /* SA22-7201-05:                                             */
        /*  p. 16-26, program check if suspend specified             */
        /*  p. 15-24, Chain-Data (CD) Flag                           */
        if (flags & CCW_FLAGS_CD)
        {
            if (flags & CCW_FLAGS_SUSP)
            {
                chanstat = CSW_PROGC;
                if (prefetch.seq)
                    goto prefetch;
                goto breakchain;
            }

            /* Turn of chain command and suppress indicator bits     */
            /* for processing purposes as settings ignored.          */
            flags &= ~(CCW_FLAGS_CC | CCW_FLAGS_SLI);
        }

        /* Validate command chain (CC) flag                          */
        /* SA22-7201-05:                                             */
        /*  p. 15-24, Chain-Command (CC) Flag                        */
        /*-------------------------------------------------------------
        //
        // Note: With CD check first, this test will always be false.
        //       Code left here in comment for documentation purposes.
        //
        // if (flags & (CCW_FLAGS_CC | CCW_FLAGS_CD) ==
        //             (CCW_FLAGS_CC | CCW_FLAGS_CD))
        //     flags &= ~CCW_FLAGS_CC;
        -------------------------------------------------------------*/

        /* Validate suppress length indication (SLI) flag            */
        /* SA22-7201-05:                                             */
        /*  p. 15-24, Supress-Length-Indication (SLI) Flag           */
        /*-------------------------------------------------------------
        //
        // Note: With CD check first, this test will always be false.
        //       Code left here in comment for documentation purposes.
        //
        // if (flags & (CCW_FLAGS_CD | CCW_FLAGS_SLI)) ==
        //             (CCW_FLAGS_CD | CCW_FLAGS_SLI)))
        //     flags &= ~CCW_FLAGS_SLI;
        -------------------------------------------------------------*/

        /* Validate skip flag                                        */
        /* SA22-7201-05:                                             */
        /*  p. 15-24, Skip (SKIP) Flag                               */
        /*-------------------------------------------------------------
        //
        // Note: Check against MIDAW down in MIDAW validation section.
        //       Code left here in comment for documentation purposes.
        //
        // if (flags & CCW_FLAGS_SKIP)
        // {}
        -------------------------------------------------------------*/

        /* Validate program controlled interruption (PCI) flag       */
        /* SA22-7201-05:                                             */
        /*  p. 15-24, Program Controlled Interruption (PCI) Flag     */
        /*-------------------------------------------------------------
        //
        // Note: No validation required.
        //       Code left here in comment for documentation purposes.
        //
        // if (flags & CCW_FLAGS_PCI)
        // {}
        -------------------------------------------------------------*/

#if !defined(FEATURE_MIDAW)                                     /*@MW*/
        /* Channel program check if MIDAW not installed */      /*@MW*/
        if (flags & CCW_FLAGS_MIDAW)                            /*@MW*/
        {
            chanstat = CSW_PROGC;
            if (prefetch.seq)
                goto prefetch;
            goto breakchain;
        }
#endif /*!defined(FEATURE_MIDAW)*/                              /*@MW*/

#if defined(FEATURE_MIDAW)                                      /*@MW*/
        /* Channel program check if MIDAW not enabled in ORB, or     */
        /* with SKIP or IDA specified                                */
        if ((flags & CCW_FLAGS_MIDAW) &&                        /*@MW*/
            ((dev->orb.flag7 & ORB7_D) == 0 ||
             (flags & (CCW_FLAGS_SKIP | CCW_FLAGS_IDA))))
        {                                                       /*@MW*/
            chanstat = CSW_PROGC;                               /*@MW*/
            if (prefetch.seq)
                goto prefetch;
            goto breakchain;
        }                                                       /*@MW*/
#endif /*defined(FEATURE_MIDAW)*/                               /*@MW*/

#ifdef OPTION_SYNCIO
        /* If synchronous I/O and a syncio 2 device and not an
           immediate CCW then retry asynchronously */
        if (dev->syncio_active && dev->syncio == 2
         && !dev->is_immed && !IS_CCW_SENSE(dev->code))
        {
            dev->syncio_retry = 1;
            execute_ccw_chain_return(NULL);
        }
#endif // OPTION_SYNCIO

        /* Suspend supported prior to GA22-7000-10 for the S/370     */
        /* Suspend channel program if suspend flag is set */
        if (flags & CCW_FLAGS_SUSP)
        {
            /* Channel program check if the ORB suspend control bit
               was zero, or if this is a data chained CCW */
            if ((dev->orb.flag4 & ORB4_S) == 0
                || (dev->chained & CCW_FLAGS_CD))
            {
                chanstat = CSW_PROGC;
                if (prefetch.seq)
                    goto prefetch;
                goto breakchain;
            }

#ifdef OPTION_SYNCIO
            /* Retry if synchronous I/O */
            if (dev->syncio_active)
            {
                dev->syncio_retry = 1;
                execute_ccw_chain_return(NULL);
            }
#endif // OPTION_SYNCIO

            IODELAY(dev);

            /* If halt or clear, abort suspend operation */
            if (dev->scsw.flag2 & (SCSW2_AC_HALT | SCSW2_AC_CLEAR))
            {
                if (dev->scsw.flag2 & SCSW2_AC_HALT)
                    goto execute_halt;
                goto execute_clear;
            }

            /* Call the i/o suspend exit */
            if (dev->hnd->suspend) (dev->hnd->suspend) (dev);

            OBTAIN_INTLOCK(NULL);
            obtain_lock (&dev->lock);

            /* Suspend the device if not already resume pending */
            if (!(dev->scsw.flag2 & (SCSW2_AC_RESUM)))
            {
                /* Clean up device and complete suspension exit */
                clear_subchannel_busy(dev);

                /* Set the subchannel status word to suspended       */
                /* SA22-7201-05:                                     */
                /*  p. 16-15, Subchannel-Active (Bit 24)             */
                /*  pp. 16-15 -- 16-16, Device-Active (Bit 25)       */
                /*  p. 16-16, Suspended (Bit 26)                     */
                /*  p. 16-16, Alert Status (Bit 27)                  */
                /*  p. 16-17, Intermediate Status (Bit 28)           */
                /*  p. 16-18, Status-Pending (Bit 31)                */
                dev->scsw.flag3 &= ~(SCSW3_AC_SCHAC |
                                     SCSW3_AC_DEVAC);
                dev->scsw.flag3 |= SCSW3_AC_SUSP;
                /* Principles violation. Some operating systems use
                 * CLI to check for suspend, intermediate and pending
                 * status (x'29') instead of the Principles statement
                 * with alert status set (x'39'). This also appears to
                 * be consistent with older machines.
                 * FIXME: Place conformance in user configuration?
                 *        flag3 |= SCSW3_SC_ALERT;
                 */

                dev->scsw.unitstat = 0;

                if (flags & CCW_FLAGS_PCI)
                {
                    dev->scsw.chanstat   = CSW_PCI;
                    dev->scsw.flag3     |= SCSW3_SC_INTER   |
                                           SCSW3_SC_PEND;
                    dev->pciscsw.flag3  &= ~SCSW3_SC_PEND;
                }
                else
                {
                    dev->scsw.chanstat   = 0;
                    if (!(dev->scsw.flag1 & SCSW1_U))
                        dev->scsw.flag3 |= SCSW3_SC_INTER   |
                                           SCSW3_SC_PEND;
                }

                STORE_HW(dev->scsw.count,count);

                /* Update local copy of ORB */
                STORE_FW(dev->orb.ccwaddr, (ccwaddr-8));

                /* Preserve CCW execution variables for validation */
                dev->ccwaddr = ccwaddr;
                dev->idapmask = idapmask;
                dev->idawfmt = idawfmt;
                dev->ccwfmt = ccwfmt;
                dev->ccwkey = ccwkey;

                /* Turn on the "suspended" bit.  This enables remote
                 * systems to use the device while we're waiting
                 */
                dev->suspended = 1;

                /* Trace suspension point */
                if (unlikely(dev->ccwtrace || dev->ccwstep || tracethis))
                    WRMSG (HHC01310, "I", SSID_TO_LCSS(dev->ssid),
                                          dev->devnum);

#ifdef OPTION_SYNCIO
                if (dev->regs) dev->regs->hostregs->syncio = 0;
#endif // OPTION_SYNCIO

                /* Present the interrupt and return */
                if (dev->scsw.flag3 & SCSW3_SC_PEND)
                    queue_io_interrupt_and_update_status_locked(dev);

                release_lock(&dev->lock);
                RELEASE_INTLOCK(NULL);

                if (dev->scsw.flag2 & (SCSW2_AC_CLEAR | SCSW2_AC_HALT))
                {
                    if (dev->scsw.flag2 & SCSW2_AC_CLEAR)
                        goto execute_clear;
                    goto execute_halt;
                }

                execute_ccw_chain_fast_return(NULL);
            }

            release_lock (&dev->lock);
            RELEASE_INTLOCK(NULL);

        } /* end if(CCW_FLAGS_SUSP) */

        /* Update current CCW opcode, unless data chaining */
        if (!(skip_ccws ||
              (dev->chained & CCW_FLAGS_CD)))
        {
            dev->prevcode = dev->code;
            dev->code = opcode;

            /* Allow the device handler to determine whether this is
               an immediate CCW (i.e. CONTROL with no data transfer) */
            dev->is_immed = IS_CCW_IMMEDIATE(dev, opcode);

            /*-- TBD ------------------------------------------------*/
            /*                                                       */
            /*   Initiation and check of opcode with control unit    */
            /*   belongs here.                                       */
            /*                                                       */
            /*-------------------------------------------------------*/

        }

        /* If immediate, chain data and address are ignored */
        if (dev->is_immed)
        {
            flags &= ~CCW_FLAGS_CD;
            dev->chained &= ~CCW_FLAGS_CD;
            addr = 0;
        }

        /* Channel program check if CCW refers to invalid storage    */
        /* SA22-7201-05:                                             */
        /*  p. 15-24, Data Address                                   */
        /*  p. 15-25, Count                                          */
        /*  pp. 15-25 -- 15-27, Designation of Storage Area          */
        if ((count &&
             (!(flags & CCW_FLAGS_SKIP)) &&
             (((flags & CCW_FLAGS_IDA)   &&
               ((addr & 0x03) ||
                CHADDRCHK(addr, dev)))                      ||
#if defined(FEATURE_MIDAW)
              ((flags & CCW_FLAGS_MIDAW) &&
               ((addr & 0x0F) ||
                CHADDRCHK(addr, dev)))                      ||
#endif /*defined(FEATURE_MIDAW)*/
              (!(flags & (CCW_FLAGS_IDA | CCW_FLAGS_MIDAW))     &&
               ((ccwfmt == 0 &&
                 ((addr & ~0x00FFFFFF)                      ||
                  ((addr + (count - 1)) & ~0x00FFFFFF)      ||
                  CHADDRCHK((addr + (count - 1)), dev)))    ||
#if defined(FEATURE_CHANNEL_SUBSYSTEM)
                (ccwfmt == 1 &&
                 ((addr & ~0x7FFFFFFF)                      ||
                  ((addr + count - 1) & ~0x7FFFFFFF)        ||
                  CHADDRCHK((addr + (count - 1)), dev)))        ||
#endif /*defined(FEATURE_CHANNEL_SUBSYSTEM)*/
                 CHADDRCHK(addr, dev)))))
#if defined(FEATURE_CHANNEL_SUBSYSTEM)
         || (!count &&
             (ccwfmt == 1 && (flags & CCW_FLAGS_CD)))
#endif /*defined(FEATURE_CHANNEL_SUBSYSTEM)*/
            )
        {
            chanstat = CSW_PROGC;
            if (prefetch.seq)
                goto prefetch;
            goto breakchain;
        }

        /* Suspend and reschedule I/O at this point if SIO and CPU   */
        /* not yet released; if IDA specified, first IDA must be     */
        /* verified before suspend and reschedule.                   */
        if (dev->s370start)
        {
            /* Note: dev->s370start is reset in resume processing;
             *       dev->suspended is NOT set as it is not the intent
             *       to permit another system to update the device.
             */

            /* Acquire device lock */
            obtain_lock(&dev->lock);

            /* State converting from SIO synchronous to asynchronous */
            if (dev->ccwtrace || dev->ccwstep || tracethis)
                WRMSG (HHC01320, "I", SSID_TO_LCSS(dev->ssid),
                                      dev->devnum);

            /* Update local copy of ORB */
            STORE_FW(dev->orb.ccwaddr, (ccwaddr-8));

            /* Preserve CCW execution variables for validation */
            dev->ccwaddr = ccwaddr;
            dev->idapmask = idapmask;
            dev->idawfmt = idawfmt;
            dev->ccwfmt = ccwfmt;
            dev->ccwkey = ccwkey;

            /* Set the resume pending flag and signal the subchannel;
             * NULL is used for the requeue regs as the execution of
             * the I/O is no longer attached to a specific processor.
             */
            dev->scsw.flag2 |= SCSW2_AC_RESUM;
            schedule_ioq(NULL, dev);

            /* Leave device as busy, unlock device and return */
            release_lock(&dev->lock);
            execute_ccw_chain_fast_return(NULL);
        }

        /* Handle initial status settings on first non-immediate CCW */
        /* to the device                                             */
        if (firstccw && !dev->is_immed)
        {
            /* Reset first CCW indication as we're starting the      */
            /* subchannel                                            */
            firstccw = 0;

            /* Subchannel and device are now active, set bits in     */
            /* SCSW                                                  */
            /* SA22-7201-05:                                         */
            /*  p. 16-14, Subchannel-Active                          */
            /*  pp. 16-14 -- 16-15, Device-Active                    */
            dev->scsw.flag3 |= (SCSW3_AC_SCHAC | SCSW3_AC_DEVAC);

            /* Process Initial-Status-Interruption Request           */
            /* SA22-7201-05:                                         */
            /*  p. 16-11, Zero Condition Code                        */
#ifdef OPTION_SYNCIO
            if ((dev->scsw.flag1 & SCSW1_I) && !dev->syncio_retry)
#else // OPTION_NOSYNCIO
            if (dev->scsw.flag1 & SCSW1_I)
#endif // OPTION_SYNCIO
            {
                obtain_lock (&dev->lock);

                /* Update the CCW address in the SCSW */
                STORE_FW(dev->scsw.ccwaddr,ccwaddr);

                /* Set the zero condition-code flag in the SCSW */
                dev->scsw.flag1 |= SCSW1_Z;

                /* Set intermediate status in the SCSW */
                dev->scsw.flag3 |= (SCSW3_SC_INTER | SCSW3_SC_PEND);

                /* Queue the interrupt and update interrupt status */
                release_lock(&dev->lock);
                queue_io_interrupt_and_update_status(dev);

                if (dev->ccwtrace || dev->ccwstep || tracethis)
                    WRMSG (HHC01306, "I", SSID_TO_LCSS(dev->ssid),
                                          dev->devnum);
            }
        }


        /* For WRITE and non-immediate CONTROL operations,
           copy data from main storage into channel buffer */
        if (!skip_ccws &&
            (prefetch.seq ||
             (!dev->is_immed                 &&
              (IS_CCW_WRITE(dev->code)  ||
               IS_CCW_CONTROL(dev->code)))))
        {
            /* Clear prefetch sequence table and I/O buffer if first
               entry */
            if (!prefetch.seq)
            {
                clear_io_buffer(iobuf->data, iobuf->size);
                clear_io_buffer(&prefetch, sizeof(prefetch));
                ps = 0;
                prefetch.seq = 1;
                prefetch.prevcode = dev->prevcode;
                prefetch.opcode = dev->code;
                prefetch.ccwaddr[ps] = ccwaddr;
                prefetch.ccwflags[ps] = flags;
                prefetch.ccwcount[ps] = count;
                prefetch.chanstat[ps] = chanstat;
                chanstat = 0;
            }


prefetch:

            /* Finish prefetch table entry initialization */
            if (opcode == 0x08 ||
                (ccwfmt == 0 && ((opcode & 0x0f) == 0x08)))
            {
                prefetch.ccwflags[ps] = 0;
                prefetch.ccwcount[ps] = 0;
            }
            else
            {
                prefetch.ccwflags[ps] = flags & ~(CCW_FLAGS_SKIP |
                                                  CCW_FLAGS_SUSP);
                prefetch.reqcount += count;
            }

            /* Copy address to prefetch table */
            prefetch.dataaddr[ps] = addr;

            /* Ignore additional checks if error */
            if (chanstat)
            {
                prefetch.chanstat[ps] = chanstat;
                chanstat = 0;
            }

            /* Don't copy if immediate and zero count */
            else if (dev->is_immed && !count);

            /* Otherwise, copy data into channel buffer */
            else
            {
                U32 newsize = bufpos + count;

                prefetch.datalen[ps] = count;

                /* Extend buffer if overflow */
                if (newsize > iobuf->size)
                {
                    IOBUF *iobufnew;

                    /* Round the new buffer size up to the next
                     * megabyte boundary.
                     */
                    newsize += 1048575;
                    newsize &= 0xFFF00000UL;
                    iobufnew = iobuf_reallocate(iobuf, newsize);

                    /* If new I/O buffer allocation failed, force a
                     * Channel Data Check (CDC). Otherwise, set the
                     * iobuf pointer to the new I/O buffer space.
                     */
                    if (iobufnew == NULL)
                        prefetch.chanstat[ps] = CSW_CDC;
                    else
                        iobuf = iobufnew;
                }

                /* If no errors, prefetch data to I/O buffer */
                if (!prefetch.chanstat[ps])
                {
                    ARCH_DEP(copy_iobuf) (dev, dev->code, flags, addr,
                                count, ccwkey, idawfmt, idapmask,
                                iobuf->data + bufpos,
                                iobuf->start, iobuf->end,
                                &chanstat, &residual, &prefetch);

                    /* Update local copy of prefetch sequence entry */
                    ps = prefetch.seq - 1;

                    /* Update number of bytes in channel buffer */
                    bufpos = prefetch.pos;
                }
            }

            /* If the device handler has requested merging of data
               chained write CCWs, then collect the data from the
               chained-data CCWs in the sequence before passing buffer
               to device handler */

            /* Note: This test is commented out as we prefetch the data;
                     chain data, therefore, must always be handled.

            if ((dev->orb.flag5 & ORB5_P) ||
                dev->cdwmerge)                                        */
            {
                if (dev->code != 0x08 &&
                    !(ccwfmt == 0 && ((dev->code & 0x0F) == 0x08)) &&
                    flags & CCW_FLAGS_CD)
                {
                    /* If this is the first CCW in the data chain,
                       then save the chaining flags from the
                       previous CCW */
                    if ((dev->chained & CCW_FLAGS_CD) == 0)
                        dev->prev_chained = dev->chained;

                    /* Process next CCW in data chain */
                    dev->chained = CCW_FLAGS_CD;
                    chain = 1;
                    continue;
                }

                /* If this is the last CCW in the data chain, then
                   restore the chaining flags from the previous
                   CCW */
                if (dev->chained & CCW_FLAGS_CD)
                    dev->chained = dev->prev_chained;

            } /* end if(dev->cdwmerge) */

            /* Reset pointers */
            ccwaddr = prefetch.ccwaddr[0];
            dev->prevcode = prefetch.prevcode;
            dev->code = prefetch.opcode;
            count = prefetch.reqcount;

        }   /* End prefetch */

        /* Set chaining flag */
        chain = ( flags & (CCW_FLAGS_CD | CCW_FLAGS_CC) ) ? 1 : 0;

        /* If first in sequence, begin execution and channel data
           transfer */
        if (!skip_ccws)
        {
            /* Initialize residual byte count */
            residual = count;
            more = bufpos = unitstat = chanstat = 0;

            /* Pass the CCW to the device handler for execution */
            dev->iobuf.length = iobuf->size;
            dev->iobuf.data = iobuf->data;
            (dev->hnd->exec) (dev, dev->code, flags, dev->chained,
                              count, dev->prevcode, dev->ccwseq,
                              iobuf->data,
                              &more, &unitstat, &residual);
            dev->iobuf.length = 0;
            dev->iobuf.data = 0;

#ifdef OPTION_SYNCIO
            /* Check if synchronous I/O needs to be retried */
            if (dev->syncio_active && dev->syncio_retry)
                execute_ccw_chain_unlock_and_return(NULL);
            /*
             * NOTE: syncio_retry is left on for asynchronous I/O
             * until after the first call to the execute ccw device
             * handler. This allows the device handler to realize that
             * the I/O is being retried asynchronously.
             */
            dev->syncio_retry = 0;
#endif // OPTION_SYNCIO

            /* Check for Command Retry (suggested by Jim Pierson) */
            if ( --cmdretry && unitstat == ( CSW_CE | CSW_DE | CSW_UC | CSW_SM ) )
            {
                chain    = 1;
                if (prefetch.seq)
                {
                    ccwaddr = prefetch.ccwaddr[0];
                    prefetch.seq = 0;
                }
                ccwaddr -= 8;   /* (retry same ccw again) */
                continue;
            }

            /* Handle command reject with no data transfer */
            if (residual == count &&
                (unitstat & CSW_UC) &&
                (dev->sense[0] & SENSE_CR))
            {
                residual = 0;
                if (prefetch.seq)
                {
                    ps = 0;
                    ccwaddr = prefetch.ccwaddr[0];
                }

                goto breakchain;
            }

            /* Handle prefetch (write) conditions */
            else if (prefetch.seq)
            {
                /* Set prefetch remaining byte count                 */
                prefetch_remaining = count - residual;

                /* Determine prefetched CCW limit and raise requested
                   PCI interrupts */
                for (ps = ts = 0,
                        prevccwaddr = 1;
                     ts < prefetch.seq;
                     ts++)
                {
                    /* Set clean index not affected by loop completion
                       */
                    ps = ts;

                    /* ccwaddr and flags match each CCW encountered */
                    if (prevccwaddr != prefetch.ccwaddr[ps])
                    {
                        flags = prefetch.ccwflags[ps];
                        prevccwaddr = prefetch.ccwaddr[ps];
                        residual = count = prefetch.ccwcount[ps];

                        /* Raise PCI interrupt */
                        if (flags & CCW_FLAGS_PCI)
                            ARCH_DEP(raise_pci) (dev, ccwkey, ccwfmt,
                                                 prevccwaddr);
                    }

                    /* Adjust counts */
                    if (prefetch.datalen[ps])
                    {
                        if (residual >= prefetch.datalen[ps])
                            residual -= prefetch.datalen[ps];
                        else
                            residual = 0;

                        if (prefetch_remaining >= prefetch.datalen[ps])
                            prefetch_remaining -= prefetch.datalen[ps];
                        else
                        {
                            prefetch_remaining = 0;
                            break;
                        }
                    }

                    /* Check for prefetch replay completed */
                    if (prefetch.chanstat[ps] ||
                        (!prefetch_remaining &&
                         (ps >= prefetch.seq)))
                        break;

                }

                /* Update SCSW CCW address and channel status */
                ccwaddr  = prefetch.ccwaddr[ps];
                chanstat = prefetch.chanstat[ps];

            } /* End prefetch status update */


            /* For READ, SENSE, and READ BACKWARD operations, copy data
               from channel buffer to main storage, unless SKIP is set
               */
            else if (!dev->is_immed                  &&
                     !skip_ccws                      &&
                     !(flags & CCW_FLAGS_SKIP)       &&
                     ((IS_CCW_READ(dev->code)     ||
                       IS_CCW_SENSE(dev->code)    ||
                       IS_CCW_RDBACK(dev->code))))
            {
                /* Copy data from I/O buffer to main storage */
                ARCH_DEP(copy_iobuf) (dev, dev->code, flags, addr,
                                      count - residual, ccwkey,
                                      idawfmt, idapmask,
                                      iobuf->data,
                                      iobuf->start, iobuf->end,
                                      &chanstat, &residual, &prefetch);

                /* Update number of bytes in channel buffer */
                bufpos = prefetch.pos;

                /* If error during copy, skip remaining CD CCWs */
                if (chanstat && (flags & CCW_FLAGS_CD))
                    skip_ccws = 1;
            }
        }

        /* Check for incorrect length */
        if ((residual || more)
            && !(chanstat & ~(CSW_PCI | CSW_IL)))
        {
            /* Set incorrect length status if not suppressed         */
            /* SA22-7201-05:                                         */
            /*  pp. 16-24 -- 16-25, Incorrect Length                 */
            /* GA22-7000-10:                                         */
            /*  p. 13-70, Incorrect Length                           */
            if (((residual && !(flags & CCW_FLAGS_SLI)) ||
                 ((more || (residual && prefetch.seq)) &&
                  !(flags & (CCW_FLAGS_CD | CCW_FLAGS_SLI))))
                && !(dev->is_immed && (flags & CCW_FLAGS_CC))                   /* DSF Debug */
#if defined(FEATURE_INCORRECT_LENGTH_INDICATION_SUPPRESSION)
                /* Set incorrect length indication if not Format-1   */
                /* CCW and incorrect length suppression is not       */
                /* indicated in the ORB                              */
                /* SA22-7201-05:                                     */
                /*  p. 15-6, Figure 15-6, Subchannel Chaining Action */
                /*  p. 16-25, Incorrect Length                       */
                && !((dev->orb.flag5 & ORB5_F) &&
                     (dev->orb.flag7 & ORB7_L))
#else
                /* S/370 Channel or suppression facility not         */
                /* available                                         */
                /* GA22-7000-10:                                     */
                /*  p. 13-70, Incorrect Length                       */
                && !dev->is_immed
#endif /*defined(FEATURE_INCORRECT_LENGTH_INDICATION_SUPPRESSION)*/
                    )
                chanstat |= CSW_IL;
        }


breakchain:

        if (unlikely((chanstat & (CSW_PROGC | CSW_PROTC | CSW_CDC |
                                  CSW_CCC   | CSW_ICC   | CSW_CHC))
                  || ((unitstat & CSW_UC) && dev->sense[0])))
        {
            /* Trace the CCW if not already done */
            if (!(dev->ccwtrace || dev->ccwstep || tracethis)
              && (CPU_STEPPING_OR_TRACING_ALL || sysblk.pgminttr
                || dev->ccwtrace || dev->ccwstep) )
                display_ccw (dev, ccw, addr, count, flags);

            /* Activate tracing for this CCW chain only
               if any trace is already active */
            if(CPU_STEPPING_OR_TRACING_ALL || sysblk.pgminttr
              || dev->ccwtrace || dev->ccwstep)
            tracethis = 1;
        }

        /* Trace the results of CCW execution */
        if (unlikely(dev->ccwtrace || dev->ccwstep || tracethis))
        {
            #if DEBUG_PREFETCH
                if (!prefetch.seq)
                {
                    char msgbuf[133];
                    MSGBUF(msgbuf, "flags=%2.2X count=%d (%4.4X) "
                                   "residual=%d (%4.4X) "
                                   "more=%d "
                                   "bufpos=%d",
                                   flags,
                                   count, count,
                                   residual, residual,
                                   more, bufpos);
                    WRMSG(HHC90000, "I", msgbuf);
                }
            #endif

            /* Display prefetch data */
            if (prefetch.seq)
            {
                /* Display prefetch table */
                display_prefetch(&prefetch, ps, count, residual, more);

                /* Loop through prefetch table for CCW/IDAW display */
                for (ts = 0, prevccwaddr = 1;
                     ts <= ps && ts < prefetch.seq;
                     ts++)
                {
                    if (prevccwaddr != prefetch.ccwaddr[ts])
                    {
                        prevccwaddr = ccwaddr = prefetch.ccwaddr[ts];
                        if (ts)
                        {
                            /* Display CCW */
                            display_ccw (dev,
                                         dev->mainstor +
                                           (prefetch.ccwaddr[ts] - 8),
                                         prefetch.dataaddr[ts],
                                         prefetch.datalen[ts],
                                         flags);

                        }
                    }
                    if (prefetch.idawtype[ts])
                        display_idaw(dev,
                                     prefetch.idawtype[ts],
                                     prefetch.idawflag[ts],
                                     prefetch.dataaddr[ts],
                                     prefetch.datalen[ts]);
                }

                /* Don't display data area on status */
                area[0] = 0x00;
            }

            /* Format data for READ or SENSE commands only */
            else if (!dev->is_immed              &&
                !(flags & CCW_FLAGS_SKIP)        &&
                count                            &&
                (IS_CCW_READ(dev->code)     ||
                 IS_CCW_SENSE(dev->code)    ||
                 IS_CCW_RDBACK(dev->code)))
            {
                memcpy(area, "=>", 2);
                format_data(area + 2, sizeof(area)-2, iobuf->data, MIN(count, 16));
            }
            else
                area[0] = '\0';

            /* Display status and residual byte count */
            WRMSG (HHC01312, "I", SSID_TO_LCSS(dev->ssid), dev->devnum, unitstat, chanstat, residual, area);

            /* Display sense bytes if unit check is indicated */
            if (unitstat & CSW_UC)
            {
                register BYTE *sense = dev->sense;
                WRMSG (HHC01313, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
                        sense[0], sense[1], sense[2], sense[3],
                        sense[4], sense[5], sense[6], sense[7],
                        sense[8], sense[9], sense[10], sense[11],
                        sense[12], sense[13], sense[14], sense[15],
                        sense[16], sense[17], sense[18], sense[19],
                        sense[20], sense[21], sense[22], sense[23],
                        sense[24], sense[25], sense[26], sense[27],
                        sense[28], sense[29], sense[30], sense[31]);
                if (sense[0] != 0 || sense[1] != 0)
                {
                    WRMSG (HHC01314, "I", SSID_TO_LCSS(dev->ssid), dev->devnum,
                            (sense[0] & SENSE_CR) ? "CMDREJ " : "",
                            (sense[0] & SENSE_IR) ? "INTREQ " : "",
                            (sense[0] & SENSE_BOC) ? "BOC " : "",
                            (sense[0] & SENSE_EC) ? "EQC " : "",
                            (sense[0] & SENSE_DC) ? "DCK " : "",
                            (sense[0] & SENSE_OR) ? "OVR " : "",
                            (sense[0] & SENSE_CC) ? "CCK " : "",
                            (sense[0] & SENSE_OC) ? "OCK " : "",
                            (sense[1] & SENSE1_PER) ? "PER " : "",
                            (sense[1] & SENSE1_ITF) ? "ITF " : "",
                            (sense[1] & SENSE1_EOC) ? "EOC " : "",
                            (sense[1] & SENSE1_MTO) ? "MSG " : "",
                            (sense[1] & SENSE1_NRF) ? "NRF " : "",
                            (sense[1] & SENSE1_FP) ? "FP " : "",
                            (sense[1] & SENSE1_WRI) ? "WRI " : "",
                            (sense[1] & SENSE1_IE) ? "IE " : "");
                }
            }
        }

        /* Terminate the channel program if any unusual status */
        if (chanstat != 0
            || (unitstat & ~CSW_SM) != (CSW_CE | CSW_DE))
        {
            if (firstccw &&
                !dev->is_immed &&
                (dev->scsw.flag1 & SCSW1_I)
#ifdef OPTION_SYNCIO
                && !dev->syncio_retry
#endif // OPTION_SYNCIO
            )
            {
                /* Set the zero condition-code flag in the SCSW */
                dev->scsw.flag1 |= SCSW1_Z;
                firstccw = 0;
            }
            chain = 0;
        }

        /* Increment CCW address if device returned status modifier  */
        /* SA22-7201-05:                                             */
        /*  p. 15-30, Command Chaining                               */
        else if ((unitstat & (CSW_DE | CSW_SM)) == (CSW_DE | CSW_SM))
            ccwaddr += 8;

        /* Update the chaining flags */
        dev->chained = flags & (CCW_FLAGS_CD | CCW_FLAGS_CC);

        /* Update the CCW sequence number unless data chained */
        if ((flags & CCW_FLAGS_CD) == 0)
        {
            dev->ccwseq++;

            /* Reset prefetch table and master skip data */
            dev->is_immed =
            skip_ccws =
            ps =
            prefetch.seq =
            prefetch.pos =
            bufpos = 0;

        }

        /* If Halt or Clear Pending, restart chaining to reset */
        if (dev->scsw.flag2 & (SCSW2_AC_HALT | SCSW2_AC_CLEAR))
            chain = 1;

    } /* end while(chain) */

    IODELAY(dev);

    /* Call the i/o end exit */
    if (dev->hnd->end) (dev->hnd->end) (dev);

    obtain_lock (&dev->lock);

    /* Complete the subchannel status word */
    dev->scsw.flag3 &= ~(SCSW3_AC_SCHAC | SCSW3_AC_DEVAC | SCSW3_SC_INTER);
    dev->scsw.flag3 |= (SCSW3_SC_PRI | SCSW3_SC_SEC | SCSW3_SC_PEND);
    STORE_FW(dev->scsw.ccwaddr,ccwaddr);
    dev->scsw.unitstat = unitstat;
    dev->scsw.chanstat = chanstat;
    STORE_HW(dev->scsw.count,residual);

    /* Set alert status if terminated by any unusual condition */
    if (chanstat != 0 || unitstat != (CSW_CE | CSW_DE))
        dev->scsw.flag3 |= SCSW3_SC_ALERT;

    /* Clear the Extended Status Word (ESW) and set LPUM in Word 0,
       defaulting to a Format-1 ESW if no other action taken */
    memset (&dev->esw, 0,sizeof(ESW));
    dev->esw.lpum = 0x80;

    /* Format-0 if CDC, CCC OR IFCC */
    if (chanstat & (CSW_CDC | CSW_CCC | CSW_ICC))
    {
        /* SA22-7201-5:                                              */
        /*  p. 16-34, Field Validity Flags (FVF)                     */
        /*  p. 16-34 -- 16-35, Termination Code                      */
        /*  p. 16-35 -- 16-36, Sequence Code (SC)                    */
        /*                                                           */
        /*  TBD: Further refinement needed as only CDC is even close */
        /*       to being properly implemented.                      */
        dev->esw.scl2 = 0x78;   /* FVF: LPUM, termination code,      */
                                /*      sequence code and device     */
                                /*      status valid                 */
        /* Set data direction */
        if (!dev->is_immed)
        {
            if (prefetch.seq)
                dev->esw.scl2 |= 0x02;  /* Write operation */
            else if (IS_CCW_RDBACK(dev->code))
                dev->esw.scl2 |= 0x03;  /* Read backward */
            else
                dev->esw.scl2 |= 0x01;  /* Read */
        }
        dev->esw.scl3 = 0x45;   /* Bits 24-25: Termination Code      */
                                /* Bits 29-31: Sequence Code         */
    }

    /* Clear the extended control word */
    memset (dev->ecw, 0, sizeof(dev->ecw));

    /* Return sense information if PMCW allows concurrent sense */
    if ((unitstat & CSW_UC) && (dev->pmcw.flag27 & PMCW27_S))
    {
        dev->scsw.flag1 |= SCSW1_E;
        dev->esw.erw0 |= ERW0_S;
        dev->esw.erw1 = (BYTE)((dev->numsense < (int)sizeof(dev->ecw)) ?
                        dev->numsense : (int)sizeof(dev->ecw));
        memcpy (dev->ecw, dev->sense, dev->esw.erw1 & ERW1_SCNT);
        memset (dev->sense, 0, sizeof(dev->sense));
        dev->sns_pending = 0;
    }

    /* Late notification. If halt or clear in process, go clear the  */
    /* mess.                                                         */
    if (dev->scsw.flag2 & (SCSW2_AC_HALT | SCSW2_AC_CLEAR))
    {
        release_lock(&dev->lock);
        if (dev->scsw.flag2 & SCSW2_AC_HALT)
            goto execute_halt;
        goto execute_clear;
    }

#ifdef OPTION_SYNCIO
    if (dev->regs) dev->regs->hostregs->syncio = 0;
#endif // OPTION_SYNCIO

    /* Present the interrupt and return */
    release_lock (&dev->lock);
    queue_io_interrupt_and_update_status(dev);
    execute_ccw_chain_return(NULL);

    #undef execute_ccw_chain_return
    #undef execute_ccw_chain_fast_return
    #undef execute_ccw_chain_unlock_and_return

    return NULL;
} /* end function execute_ccw_chain */


/*-------------------------------------------------------------------*/
/* TEST WHETHER INTERRUPTS ARE ENABLED FOR THE SPECIFIED DEVICE      */
/* When configured for S/370 channels, the PSW system mask and/or    */
/* the channel masks in control register 2 determine whether the     */
/* device is enabled.  When configured for the XA or ESA channel     */
/* subsystem, the interrupt subclass masks in control register 6     */
/* determine eligability; the PSW system mask is not tested, because */
/* the TPI instruction can operate with I/O interrupts masked off.   */
/* Returns non-zero if interrupts enabled, 0 if interrupts disabled. */
/*-------------------------------------------------------------------*/
/* I/O Assist:                                                       */
/* This routine must return:                                         */
/*                                                                   */
/* 0                   - In the case of no pending interrupt         */
/* SIE_NO_INTERCEPT    - For a non-intercepted I/O interrupt         */
/* SIE_INTERCEPT_IOINT - For an intercepted I/O interrupt            */
/* SIE_INTERCEPT_IOINTP- For a pending I/O interception              */
/*                                                                   */
/* SIE_INTERCEPT_IOINT may only be returned to a guest               */
/*-------------------------------------------------------------------*/
static INLINE int
ARCH_DEP(interrupt_enabled) (REGS *regs, DEVBLK *dev)
{
int     i;                              /* Interruption subclass     */

    /* Ignore this device if subchannel not valid */
    if (!(dev->pmcw.flag5 & PMCW5_V))
        return 0;

#if defined(_FEATURE_IO_ASSIST)
    /* For I/O Assist the zone must match the guest zone */
    if(SIE_MODE(regs) && regs->siebk->zone != dev->pmcw.zone)
        return 0;
#endif

#if defined(_FEATURE_IO_ASSIST)
    /* The interrupt interlock control bit must be on
       if not we must intercept */
    if(SIE_MODE(regs) && !(dev->pmcw.flag27 & PMCW27_I))
        return SIE_INTERCEPT_IOINT;
#endif

#ifdef FEATURE_S370_CHANNEL

#if defined(FEATURE_CHANNEL_SWITCHING)
    /* Is this device on a channel connected to this CPU? */
    if(
#if defined(_FEATURE_IO_ASSIST)
       !SIE_MODE(regs) &&
#endif
       regs->chanset != dev->chanset)
        return 0;
#endif /*defined(FEATURE_CHANNEL_SWITCHING)*/

    /* Isolate the channel number */
    i = dev->devnum >> 8;
    if (!ECMODE(&regs->psw) && i < 6)
    {
#if defined(_FEATURE_IO_ASSIST)
        /* We must always intercept in BC mode */
        if(SIE_MODE(regs))
            return SIE_INTERCEPT_IOINT;
#endif
        /* For BC mode channels 0-5, test system mask bits 0-5 */
        if ((regs->psw.sysmask & (0x80 >> i)) == 0)
            return 0;
    }
    else
    {
        /* For EC mode and channels 6-31, test system mask bit 6 */
        if ((regs->psw.sysmask & PSW_IOMASK) == 0)
            return 0;

        /* If I/O mask is enabled, test channel masks in CR2 */
        if (i > 31) i = 31;
        if ((CHANNEL_MASKS(regs) & (0x80000000 >> i)) == 0)
            return
#if defined(_FEATURE_IO_ASSIST)
                   SIE_MODE(regs) ? SIE_INTERCEPT_IOINTP :
#endif
                                                           0;
    }
#endif /*FEATURE_S370_CHANNEL*/

#ifdef FEATURE_CHANNEL_SUBSYSTEM
    /* Ignore this device if subchannel not enabled */
    if (!(dev->pmcw.flag5 & PMCW5_E))
        return 0;

    /* Isolate the interruption subclass */
    i =
#if defined(_FEATURE_IO_ASSIST)
        /* For I/O Assisted devices use the guest (V)ISC */
        SIE_MODE(regs) ? (dev->pmcw.flag25 & PMCW25_VISC) :
#endif
        ((dev->pmcw.flag4 & PMCW4_ISC) >> 3);

    /* Test interruption subclass mask bit in CR6 */
    if ((regs->CR_L(6) & (0x80000000 >> i)) == 0)
        return
#if defined(_FEATURE_IO_ASSIST)
                   SIE_MODE(regs) ? SIE_INTERCEPT_IOINTP :
#endif
                                                           0;
#endif /*FEATURE_CHANNEL_SUBSYSTEM*/

    /* Interrupts are enabled for this device */
    return SIE_NO_INTERCEPT;
} /* end function interrupt_enabled */

/*-------------------------------------------------------------------*/
/* PRESENT PENDING I/O INTERRUPT                                     */
/* Finds a device with a pending condition for which an interrupt    */
/* is allowed by the CPU whose regs structure is passed as a         */
/* parameter.  Clears the interrupt condition and returns the        */
/* I/O address and I/O interruption parameter (for channel subsystem)*/
/* or the I/O address and CSW (for S/370 channels).                  */
/* This routine does not perform a PSW switch.                       */
/* The CSW pointer is NULL in the case of TPI.                       */
/* The return value is the condition code for the TPI instruction:   */
/* 0 if no allowable pending interrupt exists, otherwise 1.          */
/* Note: The caller MUST hold the interrupt lock (sysblk.intlock).   */
/*-------------------------------------------------------------------*/
/* I/O Assist:                                                       */
/* This routine must return:                                         */
/*                                                                   */
/* 0                   - In the case of no pending interrupt         */
/* SIE_NO_INTERCEPT    - For a non-intercepted I/O interrupt         */
/* SIE_INTERCEPT_IOINT - For an I/O interrupt which must intercept   */
/* SIE_INTERCEPT_IOINTP- For a pending I/O interception              */
/*                                                                   */
/* SIE_INTERCEPT_IOINT may only be returned to a guest               */
/*-------------------------------------------------------------------*/
int
ARCH_DEP(present_io_interrupt) (REGS *regs, U32 *ioid,
                                U32 *ioparm, U32 *iointid, BYTE *csw)
{
IOINT  *io, *io2;                       /* -> I/O interrupt entry    */
DEVBLK *dev;                            /* -> Device control block   */
int     icode = 0;                      /* Intercept code            */

    UNREFERENCED_370(ioparm);
    UNREFERENCED_370(iointid);
#if defined(_FEATURE_IO_ASSIST)
    UNREFERENCED_390(iointid);
#endif
    UNREFERENCED_390(csw);
    UNREFERENCED_900(csw);

    /* Find a device with pending interrupt */

    /* (N.B. devlock cannot be acquired while iointqlk held;
       iointqlk must be acquired after devlock) */
retry:
    dev = NULL;
    obtain_lock(&sysblk.iointqlk);
    for (io = sysblk.iointq; io != NULL; io = io->next)
    {
        /* Can't present interrupt while TEST SUBCHANNEL required
         * (interrupt already presented for this device)
         */
        if (io->dev->tschpending)
            continue;

        /* Exit loop if enabled for interrupts from this device */
        if ((icode = ARCH_DEP(interrupt_enabled)(regs, io->dev))
#if defined(_FEATURE_IO_ASSIST)
          && icode != SIE_INTERCEPT_IOINTP
#endif
                                          )
        {
            dev = io->dev;
            break;
        }

        /* See if another CPU can take this interrupt */
        {
            REGS *regs;
            CPU_BITMAP mask = sysblk.waiting_mask;
            CPU_BITMAP wake;
            int i;

            /* If any CPUs are waiting, isolate to subgroup enabled for
             * I/O interrupts.
             */
            if (mask)
            {
                wake = mask;

                /* Turn off wake mask bits for waiting CPUs that aren't
                 * enabled for I/O interrupts.
                 */
                for (i = 0; mask; mask >>= 1, ++i)
                {
                    if (mask & 1)
                    {
                        regs = sysblk.regs[i];
                        if (!OPEN_IC_IOPENDING(regs))
                            wake ^= regs->cpubit;
                    }
                }

                /* Wakeup the LRU waiting CPU enabled for I/O
                 * interrupts.
                 */
                WAKEUP_CPU_MASK(wake);
            }
        }

    } /* end for(io) */

#if defined(_FEATURE_IO_ASSIST)
    /* In the case of I/O assist, do a rescan, to see if there are
       any devices with pending subclasses for which we are not
       enabled, if so cause a interception */
    if (io == NULL && SIE_MODE(regs))
    {
        /* Find a device with a pending interrupt, regardless
           of the interrupt subclass mask */
        ASSERT(dev == NULL);
        for (io = sysblk.iointq; io != NULL; io = io->next)
        {
            /* Exit loop if pending interrupts from this device */
            if ((icode = ARCH_DEP(interrupt_enabled)(regs, io->dev)))
            {
                dev = io->dev;
                break;
            }
        } /* end for(io) */
    }
#endif

    /* If no interrupt pending, or no device, exit with
     * condition code 0
     */
    if (io == NULL || dev == NULL)
    {
        if (dev != NULL)
            subchannel_interrupt_queue_cleanup(dev);
        UPDATE_IC_IOPENDING_QLOCKED();
        release_lock(&sysblk.iointqlk);
        return 0;
    }

    release_lock(&sysblk.iointqlk);

    /* Obtain device lock for device with interrupt */
    obtain_lock(&dev->lock);

    /* Verify interrupt for this device still exists and TEST SUBCHANNEL
     * has to be issued to clear an existing interrupt
     */
    obtain_lock(&sysblk.iointqlk);
    for (io2 = sysblk.iointq; io2 != NULL && io2 != io; io2 = io2->next);

    if (io2 == NULL ||
        dev->tschpending)
    {
        /* Our interrupt was dequeued; retry */
        release_lock(&sysblk.iointqlk);
        release_lock(&dev->lock);
        goto retry;
    }


    /* Extract the I/O address and interrupt parameter */
    *ioid = (dev->ssid << 16) | dev->subchan;
    FETCH_FW(*ioparm,dev->pmcw.intparm);
#if defined(FEATURE_ESAME) || defined(_FEATURE_IO_ASSIST)
#if defined(FEATURE_QDIO_THININT)
    if (unlikely(FACILITY_ENABLED(QDIO_THININT, regs)
        && (dev->pciscsw.flag2 & SCSW2_Q) && dev->qdio.thinint) )
    {
        *ioid = *ioparm = 0;

        *iointid = 0x80000000
             | (
#if defined(_FEATURE_IO_ASSIST)
                /* For I/O Assisted devices use (V)ISC */
                (SIE_MODE(regs)) ?
                  (icode == SIE_NO_INTERCEPT) ?
                    ((dev->pmcw.flag25 & PMCW25_VISC) << 27) :
                    ((dev->pmcw.flag25 & PMCW25_VISC) << 27)
                      | (dev->pmcw.zone << 16)
                      | ((dev->pmcw.flag27 & PMCW27_I) << 8) :
#endif
                ((dev->pmcw.flag4 & PMCW4_ISC) << 24)
                  | ((dev->pmcw.flag25 & PMCW25_TYPE) << 7)
#if defined(_FEATURE_IO_ASSIST)
                  | (dev->pmcw.zone << 16)
                  | ((dev->pmcw.flag27 & PMCW27_I) << 8)
#endif
                                                                  ) ;

        if(!SIE_MODE(regs) || icode != SIE_INTERCEPT_IOINTP)
        {
            /* Clear the pending PCI status */
            dev->pciscsw.flag2 &= ~(SCSW2_FC | SCSW2_AC);
            dev->pciscsw.flag3 &= ~(SCSW3_SC);

            /* Dequeue the interrupt */
            DEQUEUE_IO_INTERRUPT_QLOCKED(&dev->pciioint);
        }
    }
    else
#endif /*defined(FEATURE_QDIO_THININT)*/
        *iointid = (
#if defined(_FEATURE_IO_ASSIST)
                    /* For I/O Assisted devices use (V)ISC */
                    (SIE_MODE(regs)) ?
                      (icode == SIE_NO_INTERCEPT) ?
                        ((dev->pmcw.flag25 & PMCW25_VISC) << 27) :
                        ((dev->pmcw.flag25 & PMCW25_VISC) << 27)
                          | (dev->pmcw.zone << 16)
                          | ((dev->pmcw.flag27 & PMCW27_I) << 8) :
#endif
                     ((dev->pmcw.flag4 & PMCW4_ISC) << 24)
                       | ((dev->pmcw.flag25 & PMCW25_TYPE) << 7)
#if defined(_FEATURE_IO_ASSIST)
                       | (dev->pmcw.zone << 16)
                       | ((dev->pmcw.flag27 & PMCW27_I) << 8)
#endif
                                                                  );
#endif /*defined(FEATURE_ESAME) || defined(_FEATURE_IO_ASSIST)*/

#if defined(_FEATURE_IO_ASSIST)
    /* Do not drain pending interrupts on intercept due to zero ISC
     * mask
     */
    if(!SIE_MODE(regs) || icode != SIE_INTERCEPT_IOINTP)
#endif /*_FEATURE_IO_ASSIST)*/
    {
        if (!SIE_MODE(regs) || icode != SIE_NO_INTERCEPT)
            dev->pmcw.flag27 &= ~PMCW27_I;

        /* Dequeue the interrupt */
        DEQUEUE_IO_INTERRUPT_QLOCKED(io);
    }

    /* TEST SUBCHANNEL is now required to clear the interrupt */
    dev->tschpending = 1;

    /* Perform additional architecture post processing and cleanup */
    switch (sysblk.arch_mode)
    {
        case ARCH_370:
        {
            IOINT*  ioint;              /* -> I/O interrupt          */
            IRB     irb;                /* -> IRB                    */
            SCSW*   scsw;               /* -> SCSW                   */
            int     cc;                 /* Condition code ignored    */

            /* Extract the I/O address and CSW */
            *ioid = dev->devnum;

            /* Perform core of TEST SUBCHANNEL work and store CSW */
            cc = test_subchan_locked(regs, dev, &irb, &ioint, &scsw);
            store_scsw_as_csw(regs, scsw);
            break;
        }
    }

    subchannel_interrupt_queue_cleanup(dev);
    UPDATE_IC_IOPENDING_QLOCKED();
    release_lock(&sysblk.iointqlk);

#if DEBUG_SCSW
    if (unlikely(dev->ccwtrace))
    {
        SCSW*   scsw;                   /* SCSW to validate          */

        /* Select SCSW */
        if (dev->pciscsw.flag3 & SCSW3_SC_PEND)
            scsw = &dev->pciscsw;
        else if (dev->scsw.flag3 & SCSW3_SC_PEND)
            scsw = &dev->scsw;
     /* else if (dev->attnscsw.flag3 & SCSW3_SC_PEND)   */
     /*     scsw = &dev->attnscsw;                      */
        else
            scsw = NULL;

        /* Check interrupt validity */
        if (scsw != NULL                            &&
            !(scsw->flag2 & (SCSW2_FC | SCSW2_AC))  &&
            !(scsw->flag3 & SCSW3_AC))
        {
            WRMSG(HHC90000, "E", "  CHAN: Invalid SCSW presentation");
            display_scsw(dev, *scsw);
        }
    }
#endif /*DEBUG_SCSW*/

    release_lock(&dev->lock);

    /* Exit with condition code indicating queued interrupt cleared */
    return icode;

} /* end function present_io_interrupt */


#if defined(_FEATURE_IO_ASSIST)
int
ARCH_DEP(present_zone_io_interrupt) (U32 *ioid, U32 *ioparm,
                                     U32 *iointid, BYTE zone)
{
IOINT  *io;                             /* -> I/O interrupt entry    */
DEVBLK *dev;                            /* -> Device control block   */
typedef struct _DEVLIST {               /* list of device block ptrs */
    struct _DEVLIST *next;              /* next list entry or NULL   */
    DEVBLK          *dev;               /* DEVBLK in requested zone  */
    U16              ssid;              /* Subsystem ID incl. lcssid */
    U16              subchan;           /* Subchannel number         */
    FWORD            intparm;           /* Interruption parameter    */
    int              visc;              /* Guest Interrupt Subclass  */
} DEVLIST;
DEVLIST *pDEVLIST, *pPrevDEVLIST = NULL;/* (work)                    */
DEVLIST *pZoneDevs = NULL;              /* devices in requested zone */

    /* Gather devices within our zone with pending interrupt flagged */
    for (dev = sysblk.firstdev; dev; dev = dev->nextdev)
    {
        obtain_lock (&dev->lock);

        if (1
            /* Subchannel valid and enabled */
            && ((dev->pmcw.flag5 & (PMCW5_E | PMCW5_V)) == (PMCW5_E | PMCW5_V))
            /* Within requested zone */
            && (dev->pmcw.zone == zone)
            /* Pending interrupt flagged */
            && ((dev->scsw.flag3 | dev->pciscsw.flag3) & SCSW3_SC_PEND)
        )
        {
            /* (save this device for further scrutiny) */
            pDEVLIST          = (DEVLIST *)malloc( sizeof(DEVLIST) );
            pDEVLIST->next    = NULL;
            pDEVLIST->dev     = dev;
            pDEVLIST->ssid    = dev->ssid;
            pDEVLIST->subchan = dev->subchan;
//          pDEVLIST->intparm = dev->pmcw.intparm;
            memcpy( pDEVLIST->intparm, dev->pmcw.intparm, sizeof(pDEVLIST->intparm) );
            pDEVLIST->visc    = (dev->pmcw.flag25 & PMCW25_VISC);

            if (!pZoneDevs)
                pZoneDevs = pDEVLIST;

            if (pPrevDEVLIST)
                pPrevDEVLIST->next = pDEVLIST;

            pPrevDEVLIST = pDEVLIST;
        }

        release_lock (&dev->lock);
    }

    /* Exit with condition code 0 if no devices
       within our zone with a pending interrupt */
    if (!pZoneDevs)
        return 0;

    /* Remove from our list those devices
       without a pending interrupt queued */
    obtain_lock(&sysblk.iointqlk);
    for (pDEVLIST = pZoneDevs, pPrevDEVLIST = NULL; pDEVLIST;)
    {
        /* Search interrupt queue for this device */
        for (io = sysblk.iointq; io != NULL && io->dev != pDEVLIST->dev; io = io->next);

        /* Is interrupt queued for this device? */
        if (io == NULL)
        {
            /* No, remove it from our list */
            if (!pPrevDEVLIST)
            {
                ASSERT(pDEVLIST == pZoneDevs);
                pZoneDevs = pDEVLIST->next;
                free(pDEVLIST);
                pDEVLIST = pZoneDevs;
            }
            else
            {
                pPrevDEVLIST->next = pDEVLIST->next;
                free(pDEVLIST);
                pDEVLIST = pPrevDEVLIST->next;
            }
        }
        else
        {
            /* Yes, go on to next list entry */
            pPrevDEVLIST = pDEVLIST;
            pDEVLIST = pDEVLIST->next;
        }
    }
    release_lock(&sysblk.iointqlk);

    /* If no devices remain, exit with condition code 0 */
    if (!pZoneDevs)
        return 0;

    /* Extract the I/O address and interrupt parameter
       for the first pending subchannel */
    dev = pZoneDevs->dev;
    *ioid = (pZoneDevs->ssid << 16) | pZoneDevs->subchan;
    FETCH_FW(*ioparm,pZoneDevs->intparm);
    *iointid = (0x80000000 >> pZoneDevs->visc) | (zone << 16);
    pDEVLIST = pZoneDevs->next;
    free (pZoneDevs);

    /* Find all other pending subclasses */
    while (pDEVLIST)
    {
        *iointid |= (0x80000000 >> pDEVLIST->visc);
        pPrevDEVLIST = pDEVLIST;
        pDEVLIST = pDEVLIST->next;
        free (pPrevDEVLIST);
    }

    /* Exit with condition code indicating interrupt pending */
    return 1;

} /* end function present_zone_io_interrupt */
#endif

#if !defined(_GEN_ARCH)

#if defined(_ARCHMODE2)
 #define  _GEN_ARCH _ARCHMODE2
 #include "channel.c"
#endif

#if defined(_ARCHMODE3)
 #undef   _GEN_ARCH
 #define  _GEN_ARCH _ARCHMODE3
 #include "channel.c"
#endif


DLL_EXPORT int
device_attention (DEVBLK *dev, BYTE unitstat)
{
    switch(sysblk.arch_mode) {
#if defined(_370)
        case ARCH_370:
            /* Do NOT raise if initial power-on state */
            if (!INITIAL_POWERON_370())
                return s370_device_attention(dev, unitstat);
#endif
#if defined(_390)
        case ARCH_390: return s390_device_attention(dev, unitstat);
#endif
#if defined(_900)
        case ARCH_900: return z900_device_attention(dev, unitstat);
#endif
    }
    return 3;
}

void
call_execute_ccw_chain (int arch_mode, void* pDevBlk)
{
    switch (arch_mode)
    {
#if defined(_370)
        case ARCH_370: s370_execute_ccw_chain((DEVBLK*)pDevBlk); break;
#endif
#if defined(_390)
        case ARCH_390: s390_execute_ccw_chain((DEVBLK*)pDevBlk); break;
#endif
#if defined(_900)
        case ARCH_900: z900_execute_ccw_chain((DEVBLK*)pDevBlk); break;
#endif
    }
}

#endif /*!defined(_GEN_ARCH)*/
