/*-
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2006 Uwe Stuehler <uwe@openbsd.org>
 * Copyright (c) 2012 Stefan Sperling <stsp@openbsd.org>
 * Copyright (c) 2020 Henri Hennebert <hlh@restart.be>
 * Copyright (c) 2020 Gary Jennejohn <gj@freebsd.org>
 * Copyright (c) 2020 Jesper Schmitz Mouridsen <jsm@FreeBSD.org>
 * All rights reserved.
 *
 * Patch from:
 * - Lutz Bichler <Lutz.Bichler@gmail.com>
 *
 * Base on OpenBSD /sys/dev/pci/rtsx_pci.c & /dev/ic/rtsx.c
 *      on Linux   /drivers/mmc/host/rtsx_pci_sdmmc.c,
 *                 /include/linux/rtsx_pci.h &
 *                 /drivers/misc/cardreader/rtsx_pcr.c
 *      on NetBSD  /sys/dev/ic/rtsx.c
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */ 

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/module.h>
#include <sys/systm.h> /* For FreeBSD 11 */
#include <sys/types.h> /* For FreeBSD 11 */
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <machine/bus.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/sysctl.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcireg.h>
#include <dev/mmc/bridge.h>
#include <dev/mmc/mmcreg.h>
#include <dev/mmc/mmcbrvar.h>

#include "rtsxreg.h"

/* rtsx_flags values */
#define	RTSX_F_DEFAULT		0x0000
#define	RTSX_F_CARD_PRESENT	0x0001
#define	RTSX_F_SDIO_SUPPORT	0x0002
#define	RTSX_F_5209		0x0004
#define	RTSX_F_5227		0x0008
#define	RTSX_F_5229		0x0010
#define	RTSX_F_5229_TYPE_C	0x0020
#define	RTSX_F_522A		0x0040
#define	RTSX_F_522A_TYPE_A	0x0080
#define	RTSX_F_525A		0x0100
#define	RTSX_F_525A_TYPE_A	0x0200
#define	RTSX_F_5249		0x0400
#define	RTSX_F_8402		0x0800
#define	RTSX_F_8411		0x1000
#define	RTSX_F_8411B		0x2000
#define	RTSX_F_8411B_QFN48	0x4000
#define	RTSX_REVERSE_SOCKET	0x8000

#define	RTSX_NREG ((0xFDAE - 0xFDA0) + (0xFD69 - 0xFD52) + (0xFE34 - 0xFE20))
#define	SDMMC_MAXNSEGS	((MAXPHYS / PAGE_SIZE) + 1)

/* The softc holds our per-instance data. */
struct rtsx_softc {
	struct mtx	rtsx_mtx;		/* device mutex */
	device_t	rtsx_dev;		/* device */
	uint16_t	rtsx_flags;		/* device flags */
	device_t	rtsx_mmc_dev;		/* device of mmc bus */
	struct task	rtsx_card_task;		/* card presence check task */
	struct timeout_task
			rtsx_card_delayed_task;	/* card insert delayed task */
	uint32_t 	rtsx_intr_status;	/* soft interrupt status */
	int		rtsx_irq_res_id;	/* bus IRQ resource id */
	struct resource *rtsx_irq_res;		/* bus IRQ resource */
	void		*rtsx_irq_cookie;	/* bus IRQ resource cookie */
	int		rtsx_res_id;		/* bus memory resource id */
	struct resource *rtsx_res;		/* bus memory resource */
	int		rtsx_res_type;		/* bus memory resource type */
	bus_space_tag_t	rtsx_btag;		/* host register set tag */
	bus_space_handle_t rtsx_bhandle;	/* host register set handle */
	int		rtsx_timeout;		/* timeout value */     

	bus_dma_tag_t	rtsx_cmd_dma_tag;	/* DMA tag for command transfer */
	bus_dmamap_t	rtsx_cmd_dmamap;	/* DMA map for command transfer */
	void		*rtsx_cmd_dmamem;	/* DMA mem for command transfer */
	bus_addr_t	rtsx_cmd_buffer;	/* device visible address of the DMA segment */
	int		rtsx_cmd_index;		/* index in rtsx_cmd_buffer */

	bus_dma_tag_t	rtsx_data_dma_tag;	/* DMA tag for data transfer */
	bus_dmamap_t	rtsx_data_dmamap;	/* DMA map for data transfer */
	void		*rtsx_data_dmamem;	/* DMA mem for data transfer */
	bus_addr_t	rtsx_data_buffer;	/* device visible address of the DMA segment */

	u_char		rtsx_bus_busy;		/* bus busy status */
	struct mmc_host rtsx_host;		/* host parameters */
	int8_t		rtsx_ios_bus_width;	/* current host.ios.bus_width */
	int32_t		rtsx_ios_clock;		/* current host.ios.clock */
	int8_t		rtsx_ios_power_mode;	/* current host.ios.power mode */
	int8_t		rtsx_ios_timing;	/* current host.ios.timing */	
	uint8_t		rtsx_read_only;		/* card read only status */
	uint8_t		rtsx_card_drive_sel;	/* value for RTSX_CARD_DRIVE_SEL */
	uint8_t		rtsx_sd30_drive_sel_3v3;/* value for RTSX_SD30_DRIVE_SEL */
	struct mmc_request *rtsx_req;		/* MMC request */
};

static const struct rtsx_device {
	uint16_t	vendor;
	uint16_t	device;
	int		flags;	
	const char	*desc;
} rtsx_devices[] = {
#ifndef RTSX_INVERSION
	{ 0x10ec,	0x5209,	RTSX_F_5209,	"Realtek RTS5209 PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5227,	RTSX_F_5227,	"Realtek RTS5227 PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5229,	RTSX_F_5229,	"Realtek RTS5229 PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x522a,	RTSX_F_522A,	"Realtek RTS522A PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x525a,	RTSX_F_525A,	"Realtek RTS525A PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5249,	RTSX_F_5249,	"Realtek RTS5249 PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5286,	RTSX_F_8402,	"Realtek RTL8402 PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5289,	RTSX_F_8411,	"Realtek RTL8411 PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5287,	RTSX_F_8411B,	"Realtek RTL8411B PCI MMC/SD Card Reader"},
	{ 0, 		0,	0,		NULL}
#else
	{ 0x10ec,	0x5209,	RTSX_F_5209,	"Realtek RTS5209! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5227,	RTSX_F_5227,	"Realtek RTS5227! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5229,	RTSX_F_5229,	"Realtek RTS5229! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x522a,	RTSX_F_522A,	"Realtek RTS522A! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x525a,	RTSX_F_525A,	"Realtek RTS525A! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5249,	RTSX_F_5249,	"Realtek RTS5249! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5286,	RTSX_F_8402,	"Realtek RTL8402! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5289,	RTSX_F_8411,	"Realtek RTL8411! PCI MMC/SD Card Reader"},
	{ 0x10ec,	0x5287,	RTSX_F_8411B,	"Realtek RTL8411B! PCI MMC/SD Card Reader"},
	{ 0, 		0,	0,		NULL}
#endif
};

static int	rtsx_dma_alloc(struct rtsx_softc *sc);
static void	rtsx_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error);
static void	rtsx_dma_free(struct rtsx_softc *sc);
static void	rtsx_intr(void *arg);
static int	rtsx_wait_intr(struct rtsx_softc *sc, int mask, int timeout);
static void	rtsx_handle_card_present(struct rtsx_softc *sc);
static void	rtsx_card_task(void *arg, int pending __unused);
static bool	rtsx_is_card_present(struct rtsx_softc *sc);
#if 0  /* For led */
static int	rtsx_led_enable(struct rtsx_softc *sc);
static int	rtsx_led_disable(struct rtsx_softc *sc);
#endif /* For led */
static int	rtsx_init(struct rtsx_softc *sc);
static int	rtsx_map_sd_drive(int index);
static int	rtsx_rts5227_fill_driving(struct rtsx_softc *sc);
static int	rtsx_rts5249_fill_driving(struct rtsx_softc *sc);
static int	rtsx_read(struct rtsx_softc *, uint16_t, uint8_t *);
static int	rtsx_read_cfg(struct rtsx_softc *sc, uint8_t func, uint16_t addr, uint32_t *val);
static int	rtsx_write(struct rtsx_softc *sc, uint16_t addr, uint8_t mask, uint8_t val);
static int	rtsx_read_phy(struct rtsx_softc *sc, uint8_t addr, uint16_t *val);
static int	rtsx_write_phy(struct rtsx_softc *sc, uint8_t addr, uint16_t val);
static int	rtsx_set_sd_timing(struct rtsx_softc *sc, enum mmc_bus_timing timing);
static int	rtsx_set_sd_clock(struct rtsx_softc *sc, uint32_t freq);
static int	rtsx_stop_sd_clock(struct rtsx_softc *sc);
static int	rtsx_switch_sd_clock(struct rtsx_softc *sc, uint8_t n, int div, int mcu);
static int	rtsx_bus_power_off(struct rtsx_softc *sc);
static int	rtsx_bus_power_on(struct rtsx_softc *sc);
static uint8_t	rtsx_response_type(uint16_t mmc_rsp);
static void	rtsx_init_cmd(struct rtsx_softc *sc, struct mmc_command *cmd);
static void	rtsx_push_cmd(struct rtsx_softc *sc, uint8_t cmd, uint16_t reg,
			      uint8_t mask, uint8_t data);
static int	rtsx_send_cmd(struct rtsx_softc *sc, struct mmc_command *cmd);
static void	rtsx_send_cmd_nowait(struct rtsx_softc *sc, struct mmc_command *cmd);
static void	rtsx_req_done(struct rtsx_softc *sc);
static void	rtsx_soft_reset(struct rtsx_softc *sc);
static int	rtsx_send_req_get_resp(struct rtsx_softc *sc, struct mmc_command *cmd);
static int	rtsx_xfer_short(struct rtsx_softc *sc, struct mmc_command *cmd);
static int	rtsx_read_ppbuf(struct rtsx_softc *sc, struct mmc_command *cmd);
static int	rtsx_write_ppbuf(struct rtsx_softc *sc, struct mmc_command *cmd);
static int	rtsx_xfer(struct rtsx_softc *sc, struct mmc_command *cmd);

static int	rtsx_read_ivar(device_t bus, device_t child, int which, uintptr_t *result);
static int	rtsx_write_ivar(device_t bus, device_t child, int which, uintptr_t value);

static int	rtsx_probe(device_t dev);
static int	rtsx_attach(device_t dev);
static int	rtsx_detach(device_t dev);
static int	rtsx_shutdown(device_t dev);
static int	rtsx_suspend(device_t dev);
static int	rtsx_resume(device_t dev);

static int	rtsx_mmcbr_update_ios(device_t bus, device_t child __unused);
static int	rtsx_mmcbr_switch_vccq(device_t bus, device_t child __unused);
static int	rtsx_mmcbr_tune(device_t bus, device_t child __unused, bool hs400 __unused);
static int	rtsx_mmcbr_retune(device_t bus, device_t child __unused, bool reset __unused);
static int	rtsx_mmcbr_request(device_t bus, device_t child __unused, struct mmc_request *req);
static int	rtsx_mmcbr_get_ro(device_t bus, device_t child __unused);
static int	rtsx_mmcbr_acquire_host(device_t bus, device_t child __unused);
static int	rtsx_mmcbr_release_host(device_t bus, device_t child __unused);

#define	RTSX_LOCK_INIT(_sc)	mtx_init(&(_sc)->rtsx_mtx,	\
					 device_get_nameunit(sc->rtsx_dev), "rtsx", MTX_DEF)
#define	RTSX_LOCK(_sc)		mtx_lock(&(_sc)->rtsx_mtx)
#define	RTSX_UNLOCK(_sc)	mtx_unlock(&(_sc)->rtsx_mtx)
#define	RTSX_LOCK_DESTROY(_sc)	mtx_destroy(&(_sc)->rtsx_mtx)

#define	RTSX_SDCLK_OFF			0
#define	RTSX_SDCLK_250KHZ	   250000
#define	RTSX_SDCLK_400KHZ	   400000
#define	RTSX_SDCLK_25MHZ	 25000000
#define	RTSX_SDCLK_50MHZ	 50000000
#define	RTSX_SDCLK_100MHZ	100000000
#define	RTSX_SDCLK_208MHZ	208000000

#define	RTSX_MAX_DATA_BLKLEN	512

#define	RTSX_DMA_ALIGN		4
#define	RTSX_HOSTCMD_MAX	256
#define	RTSX_DMA_CMD_BIFSIZE	(sizeof(uint32_t) * RTSX_HOSTCMD_MAX)
#define	RTSX_DMA_DATA_BUFSIZE	MAXPHYS

#define	ISSET(t, f) ((t) & (f))

#define	READ4(sc, reg)						\
	(bus_space_read_4((sc)->rtsx_btag, (sc)->rtsx_bhandle, (reg)))
#define	WRITE4(sc, reg, val)					\
	(bus_space_write_4((sc)->rtsx_btag, (sc)->rtsx_bhandle, (reg), (val)))

#define	RTSX_READ(sc, reg, val) 				\
	do { 							\
		int err = rtsx_read((sc), (reg), (val)); 	\
		if (err) 					\
			return (err);				\
	} while (0)

#define	RTSX_WRITE(sc, reg, val) 				\
	do { 							\
		int err = rtsx_write((sc), (reg), 0xff, (val));	\
		if (err) 					\
			return (err);				\
	} while (0)
#define	RTSX_CLR(sc, reg, bits)					\
	do { 							\
		int err = rtsx_write((sc), (reg), (bits), 0); 	\
		if (err) 					\
			return (err);				\
	} while (0)

#define	RTSX_SET(sc, reg, bits)					\
	do { 							\
		int err = rtsx_write((sc), (reg), (bits), 0xff);\
		if (err) 					\
			return (err);				\
	} while (0)

#define	RTSX_BITOP(sc, reg, mask, bits)				\
	do {							\
        	int err = rtsx_write((sc), (reg), (mask), (bits));\
		if (err)					\
			return (err);				\
	} while (0)

/* 
 * We use two DMA buffers: a command buffer and a data buffer.
 *
 * The command buffer contains a command queue for the host controller,
 * which describes SD/MMC commands to run, and other parameters. The chip
 * runs the command queue when a special bit in the RTSX_HCBAR register is
 * set and signals completion with the RTSX_TRANS_OK_INT interrupt.
 * Each command is encoded as a 4 byte sequence containing command number
 * (read, write, or check a host controller register), a register address,
 * and a data bit-mask and value.
 * SD/MMC commands which do not transfer any data from/to the card only use
 * the command buffer.
 *
 * The data buffer is used for transfer longer than 512. Data transfer is
 * controlled via the RTSX_HDBAR register and completion is signalled by
 * the RTSX_TRANS_OK_INT interrupt.
 *
 * The chip is unable to perform DMA above 4GB.
 */

static int
rtsx_dma_alloc(struct rtsx_softc *sc) {
	int	error = 0;

	error = bus_dma_tag_create(bus_get_dma_tag(sc->rtsx_dev), /* inherit from parent */
	    RTSX_DMA_ALIGN, 0,		/* alignment, boundary */
	    BUS_SPACE_MAXADDR_32BIT,	/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    RTSX_DMA_CMD_BIFSIZE, 1,	/* maxsize, nsegments */
	    RTSX_DMA_CMD_BIFSIZE,	/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->rtsx_cmd_dma_tag);
	if (error) {
                device_printf(sc->rtsx_dev,
			      "Can't create cmd parent DMA tag\n");
		return (error);
	}
	error = bus_dmamem_alloc(sc->rtsx_cmd_dma_tag,		/* DMA tag */
	    &sc->rtsx_cmd_dmamem,				/* will hold the KVA pointer */
	    BUS_DMA_COHERENT | BUS_DMA_WAITOK | BUS_DMA_ZERO,	/* flags */
	    &sc->rtsx_cmd_dmamap); 				/* DMA map */
	if (error) {
                device_printf(sc->rtsx_dev,
			      "Can't create DMA map for command transfer\n");
		goto destroy_cmd_dma_tag;

	}
	error = bus_dmamap_load(sc->rtsx_cmd_dma_tag,	/* DMA tag */
	    sc->rtsx_cmd_dmamap,	/* DMA map */
	    sc->rtsx_cmd_dmamem,	/* KVA pointer to be mapped */
	    RTSX_DMA_CMD_BIFSIZE,	/* size of buffer */
	    rtsx_dmamap_cb,		/* callback */
	    &sc->rtsx_cmd_buffer,	/* first arg of callback */
	    0);				/* flags */
	if (error || sc->rtsx_cmd_buffer == 0) {
                device_printf(sc->rtsx_dev,
			      "Can't load DMA memory for command transfer\n");
                error = (error) ? error : EFAULT;
		goto destroy_cmd_dmamem_alloc;
        }

	error = bus_dma_tag_create(bus_get_dma_tag(sc->rtsx_dev),	/* inherit from parent */
	    RTSX_DMA_DATA_BUFSIZE, 0,	/* alignment, boundary */
	    BUS_SPACE_MAXADDR_32BIT,	/* lowaddr */
	    BUS_SPACE_MAXADDR,		/* highaddr */
	    NULL, NULL,			/* filter, filterarg */
	    RTSX_DMA_DATA_BUFSIZE, 1,	/* maxsize, nsegments */
	    RTSX_DMA_DATA_BUFSIZE,	/* maxsegsize */
	    0,				/* flags */
	    NULL, NULL,			/* lockfunc, lockarg */
	    &sc->rtsx_data_dma_tag);
	if (error) {
                device_printf(sc->rtsx_dev,
			      "Can't create data parent DMA tag\n");
		goto destroy_cmd_dmamap_load;
	}
	error = bus_dmamem_alloc(sc->rtsx_data_dma_tag,		/* DMA tag */
	    &sc->rtsx_data_dmamem,				/* will hold the KVA pointer */
	    BUS_DMA_WAITOK | BUS_DMA_ZERO,			/* flags */
	    &sc->rtsx_data_dmamap); 				/* DMA map */
	if (error) {
                device_printf(sc->rtsx_dev,
			      "Can't create DMA map for data transfer\n");
		goto destroy_data_dma_tag;
	}
	error = bus_dmamap_load(sc->rtsx_data_dma_tag,	/* DMA tag */
	    sc->rtsx_data_dmamap,	/* DMA map */
	    sc->rtsx_data_dmamem,	/* KVA pointer to be mapped */
	    RTSX_DMA_DATA_BUFSIZE,	/* size of buffer */
	    rtsx_dmamap_cb,		/* callback */
	    &sc->rtsx_data_buffer,	/* first arg of callback */
	    0);				/* flags */
	if (error || sc->rtsx_data_buffer == 0) {
                device_printf(sc->rtsx_dev,
			      "Can't load DMA memory for data transfer\n");
                error = (error) ? error : EFAULT;
		goto destroy_data_dmamem_alloc;
        }
	return (error);

 destroy_data_dmamem_alloc:
	bus_dmamem_free(sc->rtsx_data_dma_tag, sc->rtsx_data_dmamem, sc->rtsx_data_dmamap);
 destroy_data_dma_tag:
	bus_dma_tag_destroy(sc->rtsx_data_dma_tag);
 destroy_cmd_dmamap_load:
	bus_dmamap_unload(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap);
 destroy_cmd_dmamem_alloc:
	bus_dmamem_free(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamem, sc->rtsx_cmd_dmamap);
 destroy_cmd_dma_tag:
	bus_dma_tag_destroy(sc->rtsx_cmd_dma_tag);

	return (error);
}

static void
rtsx_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nsegs, int error)
{
        if (error) {
                printf("rtsx_dmamap_cb: error %d\n", error);
                return;
        }
        *(bus_addr_t *)arg = segs[0].ds_addr;
}

static void
rtsx_dma_free(struct rtsx_softc *sc) {

	if (sc->rtsx_cmd_dma_tag != NULL) {
		if (sc->rtsx_cmd_dmamap != NULL)
                        bus_dmamap_unload(sc->rtsx_cmd_dma_tag,
					  sc->rtsx_cmd_dmamap);
                if (sc->rtsx_cmd_dmamem != NULL)
                        bus_dmamem_free(sc->rtsx_cmd_dma_tag,
					sc->rtsx_cmd_dmamem,
					sc->rtsx_cmd_dmamap);
		sc->rtsx_cmd_dmamap = NULL;
		sc->rtsx_cmd_dmamem = NULL;
                sc->rtsx_cmd_buffer = 0;
                bus_dma_tag_destroy(sc->rtsx_cmd_dma_tag);
                sc->rtsx_cmd_dma_tag = NULL;
	}
	if (sc->rtsx_data_dma_tag != NULL) {
		if (sc->rtsx_data_dmamap != NULL)
                        bus_dmamap_unload(sc->rtsx_data_dma_tag,
					  sc->rtsx_data_dmamap);
                if (sc->rtsx_data_dmamem != NULL)
                        bus_dmamem_free(sc->rtsx_data_dma_tag,
					sc->rtsx_data_dmamem,
					sc->rtsx_data_dmamap);
		sc->rtsx_data_dmamap = NULL;
		sc->rtsx_data_dmamem = NULL;
                sc->rtsx_data_buffer = 0;
                bus_dma_tag_destroy(sc->rtsx_data_dma_tag);
                sc->rtsx_data_dma_tag = NULL;
	}
}
	
static void
rtsx_intr(void *arg)
{
	struct rtsx_softc *sc = arg;
	uint32_t enabled, status;

	RTSX_LOCK(sc);
	enabled = READ4(sc, RTSX_BIER);	/* read Bus Interrupt Enable Register */
	status = READ4(sc, RTSX_BIPR);	/* read Bus Interrupt Pending Register */

	if (bootverbose)
		device_printf(sc->rtsx_dev, "Interrupt handler - enabled: %#x, status: %#x\n", enabled, status);

	/* Ack interrupts. */
	WRITE4(sc, RTSX_BIPR, status);

	if (((enabled & status) == 0) || status == 0xffffffff) {
		device_printf(sc->rtsx_dev, "Spurious interrupt\n");
		RTSX_UNLOCK(sc);
		return;
	}
	if (status & RTSX_SD_WRITE_PROTECT)
		sc->rtsx_read_only = 1;
	else
		sc->rtsx_read_only = 0;

	/* start task to handle SD card status change. */
	/* from dwmmc.c */
	if (status & RTSX_SD_INT) {
		device_printf(sc->rtsx_dev, "Interrupt card inserted/removed\n");
		rtsx_handle_card_present(sc);
	}
	if (sc->rtsx_req == NULL) {
		RTSX_UNLOCK(sc);
		return;
	}
	if (status & (RTSX_TRANS_OK_INT | RTSX_TRANS_FAIL_INT)) {
		sc->rtsx_intr_status |= status;
		wakeup(&sc->rtsx_intr_status);
	}
	RTSX_UNLOCK(sc);
}

static int
rtsx_wait_intr(struct rtsx_softc *sc, int mask, int timeout)
{
	int status;
	int error = 0;

	mask |= RTSX_TRANS_FAIL_INT;

	status = sc->rtsx_intr_status & mask;
	while (status == 0) {
		if (msleep(&sc->rtsx_intr_status, &sc->rtsx_mtx, 0, "rtsxintr", timeout)
		    == EWOULDBLOCK) {
			if (sc->rtsx_req != NULL)
				device_printf(sc->rtsx_dev, "Controller timeout for CMD%u\n",
					      sc->rtsx_req->cmd->opcode);
			else
				device_printf(sc->rtsx_dev, "Controller timeout!\n");
			error = MMC_ERR_TIMEOUT;
			break;
		}
		status = sc->rtsx_intr_status & mask;
	}

	RTSX_LOCK(sc);

	sc->rtsx_intr_status &= ~status;

	/* Has the card disappeared? */
	if (!ISSET(sc->rtsx_flags, RTSX_F_CARD_PRESENT))
		error = MMC_ERR_INVALID;

	/* Does transfer fail? */
	if (error == 0 && (status & RTSX_TRANS_FAIL_INT))
		error = MMC_ERR_FAILED;

	RTSX_UNLOCK(sc);

	return (error);
}

/*
 * Function called from the IRQ handler (from dwmmc.c).
 */
static void
rtsx_handle_card_present(struct rtsx_softc *sc)
{
	bool was_present;
	bool is_present;

	was_present = sc->rtsx_mmc_dev != NULL;
	is_present = rtsx_is_card_present(sc);
	if (is_present)
		device_printf(sc->rtsx_dev, "Card present\n");
	else
		device_printf(sc->rtsx_dev, "Card absent\n");

	if (!was_present && is_present) {
		/*
		 * The delay is to debounce the card insert
		 * (sometimes the card detect pin stabilizes
		 * before the other pins have made good contact).
		 */
		taskqueue_enqueue_timeout(taskqueue_swi_giant,
					  &sc->rtsx_card_delayed_task, -hz);
	} else if (was_present && !is_present) {
		taskqueue_enqueue(taskqueue_swi_giant, &sc->rtsx_card_task);
	}
}

/*
 * This funtion is called at startup.
 */
static void
rtsx_card_task(void *arg, int pending __unused)
{
	struct rtsx_softc *sc = arg;

	RTSX_LOCK(sc);

	if (rtsx_is_card_present(sc)) {
		sc->rtsx_flags |= RTSX_F_CARD_PRESENT;
		/* Card is present, attach if necessary. */
		if (sc->rtsx_mmc_dev == NULL) {
			if (bootverbose)
				device_printf(sc->rtsx_dev, "Card inserted\n");

			sc->rtsx_mmc_dev = device_add_child(sc->rtsx_dev, "mmc", -1);
			RTSX_UNLOCK(sc);
			if (sc->rtsx_mmc_dev == NULL) {
				device_printf(sc->rtsx_dev, "Adding MMC bus failed\n");
			} else {
				device_set_ivars(sc->rtsx_mmc_dev, sc);
				(void)device_probe_and_attach(sc->rtsx_mmc_dev);
			}
		} else
			RTSX_UNLOCK(sc);
	} else {
		sc->rtsx_flags &= ~RTSX_F_CARD_PRESENT;
		/* Card isn't present, detach if necessary. */
		if (sc->rtsx_mmc_dev != NULL) {
			if (bootverbose)
				device_printf(sc->rtsx_dev, "Card removed\n");

			RTSX_UNLOCK(sc);
			if (device_delete_child(sc->rtsx_dev, sc->rtsx_mmc_dev))
				device_printf(sc->rtsx_dev, "Detaching MMC bus failed\n");
			sc->rtsx_mmc_dev = NULL;
		} else
			RTSX_UNLOCK(sc);
	}
}

static bool
rtsx_is_card_present(struct rtsx_softc *sc)
{
	uint32_t status;

	status = READ4(sc, RTSX_BIPR);
#ifndef RTSX_INVERSION
	return (status & RTSX_SD_EXIST);
#else
	return !(status & RTSX_SD_EXIST);
#endif
}

static int
rtsx_init(struct rtsx_softc *sc)
{
	uint32_t status;
	uint8_t version;
	int error;

	sc->rtsx_host.host_ocr = RTSX_SUPPORTED_VOLTAGE;
	sc->rtsx_host.f_min = RTSX_SDCLK_250KHZ;
	sc->rtsx_host.f_max = RTSX_SDCLK_208MHZ;
	sc->rtsx_read_only = 0;
	sc->rtsx_host.caps = MMC_CAP_4_BIT_DATA | MMC_CAP_HSPEED |
		MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25;

	sc->rtsx_host.caps |= MMC_CAP_UHS_SDR50 | MMC_CAP_UHS_SDR104;
	if (sc->rtsx_flags & RTSX_F_5209)
		sc->rtsx_host.caps |= MMC_CAP_8_BIT_DATA;

	/* check IC version. */
	if (sc->rtsx_flags & RTSX_F_5229) {
		/* Read IC version from dummy register. */
		RTSX_READ(sc, RTSX_DUMMY_REG, &version);
		if ((version & 0x0F) == RTSX_IC_VERSION_C)
			sc->rtsx_flags |= RTSX_F_5229_TYPE_C;
	} else if (sc->rtsx_flags & RTSX_F_522A) {
		/* Read IC version from dummy register. */
		RTSX_READ(sc, RTSX_DUMMY_REG, &version);
		if ((version & 0x0F) == RTSX_IC_VERSION_A)
			sc->rtsx_flags |= RTSX_F_522A_TYPE_A;
	} else if (sc->rtsx_flags & RTSX_F_525A) {
		/* Read IC version from dummy register. */
		RTSX_READ(sc, RTSX_DUMMY_REG, &version);
		if ((version & 0x0F) == RTSX_IC_VERSION_A)
			sc->rtsx_flags |= RTSX_F_525A_TYPE_A;
	} else if (sc->rtsx_flags & RTSX_F_8411B) {
		RTSX_READ(sc, RTSX_RTL8411B_PACKAGE, &version);
		if (version & RTSX_RTL8411B_QFN48)
			sc->rtsx_flags |= RTSX_F_8411B_QFN48;
	}

	/* Fetch vendor settings. */
	sc->rtsx_card_drive_sel = RTSX_CARD_DRIVE_DEFAULT;
	if (sc->rtsx_flags & RTSX_F_5209) {
		uint32_t reg;

		sc->rtsx_card_drive_sel = RTSX_RTS5209_CARD_DRIVE_DEFAULT;
		sc->rtsx_sd30_drive_sel_3v3 = RTSX_DRIVER_TYPE_D;
		reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG2, 4);
		if (!(reg & 0x80)) {
			sc->rtsx_card_drive_sel = (reg >> 8) & 0x3F;
			sc->rtsx_sd30_drive_sel_3v3 = reg & 0x07;
//!!!			if (bootverbose)
			device_printf(sc->rtsx_dev, "card_drive_sel = 0x%02x, sd30_drive_sel_3v3 = 0x%02x\n",
				      sc->rtsx_card_drive_sel, sc->rtsx_sd30_drive_sel_3v3);
		} else {
			device_printf(sc->rtsx_dev, "pci_read_config() error\n");
		}
	} else if (sc->rtsx_flags & (RTSX_F_5227 | RTSX_F_522A)) {
		uint32_t reg;

		sc->rtsx_sd30_drive_sel_3v3 = RTSX_CFG_DRIVER_TYPE_B;
		reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG1, 4);
		if (!(reg & 0x1000000)) {
			sc->rtsx_card_drive_sel &= 0x3F;
			sc->rtsx_card_drive_sel |= ((reg >> 25) & 0x01) << 6;
			reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG2, 4);
			sc->rtsx_sd30_drive_sel_3v3 = (reg >> 5) & 0x03;
			if (reg & 0x4000)
				sc->rtsx_flags |= RTSX_REVERSE_SOCKET;
//!!!			if (bootverbose)
			device_printf(sc->rtsx_dev,
				      "card_drive_sel = 0x%02x, sd30_drive_sel_3v3 = 0x%02x, reverse_socket is %s\n",
				      sc->rtsx_card_drive_sel, sc->rtsx_sd30_drive_sel_3v3,
				      (sc->rtsx_flags & RTSX_REVERSE_SOCKET) ? "true" : "false");
		} else {
			device_printf(sc->rtsx_dev, "pci_read_config() error\n");
		}
	} else if (sc->rtsx_flags & RTSX_F_5229) {
		uint32_t reg;

		sc->rtsx_sd30_drive_sel_3v3 = RTSX_DRIVER_TYPE_D;
		reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG1, 4);
		if (!(reg & 0x1000000)) {
			sc->rtsx_card_drive_sel &= 0x3F;
			sc->rtsx_card_drive_sel |= ((reg >> 25) & 0x01) << 6;
			reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG2, 4);
			sc->rtsx_sd30_drive_sel_3v3 = rtsx_map_sd_drive((reg >> 5) & 0x03);
//!!!			if (bootverbose)
			device_printf(sc->rtsx_dev, "card_drive_sel = 0x%02x, sd30_drive_sel_3v3 = 0x%02x\n",
				      sc->rtsx_card_drive_sel, sc->rtsx_sd30_drive_sel_3v3);
		} else {
			device_printf(sc->rtsx_dev, "pci_read_config() error\n");
		}
	} else if (sc->rtsx_flags & (RTSX_F_525A | RTSX_F_5249)) {
		uint32_t reg;

		sc->rtsx_sd30_drive_sel_3v3 = RTSX_CFG_DRIVER_TYPE_B;
		reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG1, 4);
		if ((reg & 0x1000000)) {
			sc->rtsx_card_drive_sel &= 0x3F;
			sc->rtsx_card_drive_sel |= ((reg >> 25) & 0x01) << 6;
			reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG2, 4);
			sc->rtsx_sd30_drive_sel_3v3 = (reg >> 5) & 0x03;
			if (reg & 0x4000)
				sc->rtsx_flags |= RTSX_REVERSE_SOCKET;
//!!!			if (bootverbose)
			device_printf(sc->rtsx_dev,
				      "card_drive_sel = 0x%02x, sd30_drive_sel_3v3 = 0x%02x, reverse_socket is %s\n",
				      sc->rtsx_card_drive_sel, sc->rtsx_sd30_drive_sel_3v3,
				      (sc->rtsx_flags & RTSX_REVERSE_SOCKET) ? "true" : "false");
		} else {
			device_printf(sc->rtsx_dev, "pci_read_config() error\n");
		}
	} else if (sc->rtsx_flags & (RTSX_F_8402 | RTSX_F_8411)) {
		uint32_t reg1;
		uint8_t  reg3;

		sc->rtsx_card_drive_sel = RTSX_RTL8411_CARD_DRIVE_DEFAULT;
		sc->rtsx_sd30_drive_sel_3v3 = RTSX_DRIVER_TYPE_D;
		reg1 = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG1, 4);
		if (reg1 & 0x1000000) {
			sc->rtsx_card_drive_sel &= 0x3F;
			sc->rtsx_card_drive_sel |= ((reg1 >> 25) & 0x01) << 6;
			reg3 = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG3, 1);
			sc->rtsx_sd30_drive_sel_3v3 = (reg3 >> 5) & 0x07;
//!!!			if (bootverbose)
			device_printf(sc->rtsx_dev,
				      "card_drive_sel = 0x%02x, sd30_drive_sel_3v3 = 0x%02x\n",
				      sc->rtsx_card_drive_sel, sc->rtsx_sd30_drive_sel_3v3);
		} else {
			device_printf(sc->rtsx_dev, "pci_read_config() error\n");
		}
	} else if (sc->rtsx_flags & RTSX_F_8411B) {
		uint32_t reg;

		sc->rtsx_card_drive_sel = RTSX_RTL8411_CARD_DRIVE_DEFAULT;
		sc->rtsx_sd30_drive_sel_3v3 = RTSX_DRIVER_TYPE_D;
		reg = pci_read_config(sc->rtsx_dev, RTSX_PCR_SETTING_REG1, 4);
		if (!(reg & 0x1000000)) {
			sc->rtsx_sd30_drive_sel_3v3 = rtsx_map_sd_drive(reg & 0x03);
//!!!			if (bootverbose)
			device_printf(sc->rtsx_dev, "sd30_drive_sel_3v3 = 0x%02x\n", sc->rtsx_sd30_drive_sel_3v3);
		} else {
			device_printf(sc->rtsx_dev, "pci_read_config() error\n");
		}
	}

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_init() rtsx_flags = 0x%04x\n", sc->rtsx_flags);

	/* Enable interrupt write-clear (default is read-clear). */
	RTSX_CLR(sc, RTSX_NFTS_TX_CTRL, RTSX_INT_READ_CLR);

	/* Clear any pending interrupts. */
	status = READ4(sc, RTSX_BIPR);
	WRITE4(sc, RTSX_BIPR, status);

	/* Enable interrupts. */
	WRITE4(sc, RTSX_BIER,
	       RTSX_TRANS_OK_INT_EN | RTSX_TRANS_FAIL_INT_EN | RTSX_SD_INT_EN);

	/* Power on SSC clock. */
	RTSX_CLR(sc, RTSX_FPDCTL, RTSX_SSC_POWER_DOWN);
	DELAY(200);

	/* Optimize phy. */
	if (sc->rtsx_flags & RTSX_F_5209) {
		/* Some magic numbers from linux driver. */
		if ((error = rtsx_write_phy(sc, 0x00, 0xB966)))
			return (error);
	} else if (sc->rtsx_flags & RTSX_F_5227) {
		/*!!! added */
		RTSX_CLR(sc, RTSX_PM_CTRL3, RTSX_D3_DELINK_MODE_EN);

		/* Optimize RX sensitivity. */
		if ((error = rtsx_write_phy(sc, 0x00, 0xBA42)))
			return (error);
	} else if (sc->rtsx_flags & RTSX_F_5229) {
		/* Some magic numbers from linux driver. */
		if ((error = rtsx_write_phy(sc, 0x00, 0xBA42)))
			return (error);
	} else if (sc->rtsx_flags & RTSX_F_522A) {
		RTSX_CLR(sc, RTSX_RTS522A_PM_CTRL3, RTSX_D3_DELINK_MODE_EN);
		if (sc->rtsx_flags & RTSX_F_522A_TYPE_A) {
			if ((error = rtsx_write_phy(sc, RTSX_PHY_RCR2, RTSX_PHY_RCR2_INIT_27S)))
				return (error);
		}
		if ((error = rtsx_write_phy(sc, RTSX_PHY_RCR1, RTSX_PHY_RCR1_INIT_27S)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_FLD0, RTSX_PHY_FLD0_INIT_27S)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_FLD3, RTSX_PHY_FLD3_INIT_27S)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_FLD4, RTSX_PHY_FLD4_INIT_27S)))
			return (error);
	} else if (sc->rtsx_flags & RTSX_F_525A) {
		if ((error = rtsx_write_phy(sc, RTSX__PHY_FLD0,
					    RTSX__PHY_FLD0_CLK_REQ_20C | RTSX__PHY_FLD0_RX_IDLE_EN |
					    RTSX__PHY_FLD0_BIT_ERR_RSTN | RTSX__PHY_FLD0_BER_COUNT |
					    RTSX__PHY_FLD0_BER_TIMER | RTSX__PHY_FLD0_CHECK_EN)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX__PHY_ANA03,
					    RTSX__PHY_ANA03_TIMER_MAX | RTSX__PHY_ANA03_OOBS_DEB_EN |
					    RTSX__PHY_CMU_DEBUG_EN)))
			return (error);
		if (sc->rtsx_flags & RTSX_F_525A_TYPE_A)
			if ((error = rtsx_write_phy(sc, RTSX__PHY_REV0,
						    RTSX__PHY_REV0_FILTER_OUT | RTSX__PHY_REV0_CDR_BYPASS_PFD |
						    RTSX__PHY_REV0_CDR_RX_IDLE_BYPASS)))
				return (error);
	} else if (sc->rtsx_flags & RTSX_F_5249) {
		RTSX_CLR(sc, RTSX_RTS522A_PM_CTRL3, RTSX_D3_DELINK_MODE_EN);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_REV,
					    RTSX_PHY_REV_RESV | RTSX_PHY_REV_RXIDLE_LATCHED |
					    RTSX_PHY_REV_P1_EN | RTSX_PHY_REV_RXIDLE_EN |
					    RTSX_PHY_REV_CLKREQ_TX_EN | RTSX_PHY_REV_RX_PWST |
					    RTSX_PHY_REV_CLKREQ_DT_1_0 | RTSX_PHY_REV_STOP_CLKRD |
					    RTSX_PHY_REV_STOP_CLKWR)))
			return (error);
		DELAY(10);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_BPCR,
					    RTSX_PHY_BPCR_IBRXSEL | RTSX_PHY_BPCR_IBTXSEL |
					    RTSX_PHY_BPCR_IB_FILTER | RTSX_PHY_BPCR_CMIRROR_EN)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_PCR,
					    RTSX_PHY_PCR_FORCE_CODE | RTSX_PHY_PCR_OOBS_CALI_50 |
					    RTSX_PHY_PCR_OOBS_VCM_08 | RTSX_PHY_PCR_OOBS_SEN_90 |
					    RTSX_PHY_PCR_RSSI_EN | RTSX_PHY_PCR_RX10K)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_RCR2,
					    RTSX_PHY_RCR2_EMPHASE_EN | RTSX_PHY_RCR2_NADJR |
					    RTSX_PHY_RCR2_CDR_SR_2 | RTSX_PHY_RCR2_FREQSEL_12 |
					    RTSX_PHY_RCR2_CDR_SC_12P | RTSX_PHY_RCR2_CALIB_LATE)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_FLD4,
					    RTSX_PHY_FLD4_FLDEN_SEL | RTSX_PHY_FLD4_REQ_REF |
					    RTSX_PHY_FLD4_RXAMP_OFF | RTSX_PHY_FLD4_REQ_ADDA |
					    RTSX_PHY_FLD4_BER_COUNT | RTSX_PHY_FLD4_BER_TIMER |
					    RTSX_PHY_FLD4_BER_CHK_EN)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_RDR,
					    RTSX_PHY_RDR_RXDSEL_1_9 | RTSX_PHY_SSC_AUTO_PWD)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_RCR1,
					    RTSX_PHY_RCR1_ADP_TIME_4 | RTSX_PHY_RCR1_VCO_COARSE)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_FLD3,
					    RTSX_PHY_FLD3_TIMER_4 | RTSX_PHY_FLD3_TIMER_6 |
					    RTSX_PHY_FLD3_RXDELINK)))
			return (error);
		if ((error = rtsx_write_phy(sc, RTSX_PHY_TUNE,
					    RTSX_PHY_TUNE_TUNEREF_1_0 | RTSX_PHY_TUNE_VBGSEL_1252 |
					    RTSX_PHY_TUNE_SDBUS_33 | RTSX_PHY_TUNE_TUNED18 |
					    RTSX_PHY_TUNE_TUNED12 | RTSX_PHY_TUNE_TUNEA12)))
			return (error);
	}

	/* Set mcu_cnt to 7 to ensure data can be sampled properly. */
	RTSX_SET(sc, RTSX_CLK_DIV, 0x07);

	/* Disable sleep mode. */
	RTSX_CLR(sc, RTSX_HOST_SLEEP_STATE,
		 RTSX_HOST_ENTER_S1 | RTSX_HOST_ENTER_S3);

	/* Disable card clock. */
	RTSX_CLR(sc, RTSX_CARD_CLK_EN, RTSX_CARD_CLK_EN_ALL);

	/* Reset delink mode. */
	/*!!! Discrepancy between OpenBSD and Linux. */
//	RTSX_CLR(sc, RTSX_CHANGE_LINK_STATE,
//		 RTSX_FORCE_RST_CORE_EN | RTSX_NON_STICKY_RST_N_DBG | 0x04);	/* OpenBSD */
	RTSX_CLR(sc, RTSX_CHANGE_LINK_STATE,
		 RTSX_FORCE_RST_CORE_EN | RTSX_NON_STICKY_RST_N_DBG);		/* Linux */

	/* Card driving select. */
	/*!!! added */
	RTSX_WRITE(sc, RTSX_CARD_DRIVE_SEL, sc->rtsx_card_drive_sel);

	/* Enable SSC clock. */
	RTSX_WRITE(sc, RTSX_SSC_CTL1, RTSX_SSC_8X_EN | RTSX_SSC_SEL_4M);
	RTSX_WRITE(sc, RTSX_SSC_CTL2, 0x12);

	/* Disable cd_pwr_save. */
	/*!!!*/
//	RTSX_SET(sc, RTSX_CHANGE_LINK_STATE, RTSX_MAC_PHY_RST_N_DBG);
	RTSX_BITOP(sc, RTSX_CHANGE_LINK_STATE, 0x16, RTSX_MAC_PHY_RST_N_DBG);

	/* Clear Link Ready Interrupt. */
	RTSX_SET(sc, RTSX_IRQSTAT0, RTSX_LINK_READY_INT);

	/* Enlarge the estimation window of PERST# glitch
	 * to reduce the chance of invalid card interrupt. */
	RTSX_WRITE(sc, RTSX_PERST_GLITCH_WIDTH, 0x80);

	/* Set RC oscillator to 400K. */
	RTSX_CLR(sc, RTSX_RCCTL, RTSX_RCCTL_F_2M);

	/* Request clock by driving CLKREQ pin to zero. */
	/*!!! only in OpenBSD. */
//	RTSX_SET(sc, RTSX_PETXCFG, RTSX_PETXCFG_CLKREQ_PIN);

	/* Specific extra init. */
	if (sc->rtsx_flags & RTSX_F_5209) {
		/* Turn off LED. */
		RTSX_WRITE(sc, RTSX_CARD_GPIO, 0x03);
		/* Reset ASPM state to default value. */

		RTSX_CLR(sc, RTSX_ASPM_FORCE_CTL, RTSX_ASPM_FORCE_MASK);
		/* Force CLKREQ# PIN to drive 0 to request clock. */
		RTSX_BITOP(sc, RTSX_PETXCFG, 0x08, 0x08);
		/* Configure GPIO as output. */
		RTSX_WRITE(sc, RTSX_CARD_GPIO_DIR, 0x03);
		/* Configure driving. */
		RTSX_WRITE(sc, RTSX_SD30_CMD_DRIVE_SEL, sc->rtsx_sd30_drive_sel_3v3);
	} else if (sc->rtsx_flags & RTSX_F_5227) {
		int reg;
		uint16_t cap;

		/* Configure GPIO as output. */
		RTSX_BITOP(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON, RTSX_GPIO_LED_ON);
		/* Reset ASPM state to default value. */
		RTSX_BITOP(sc, RTSX_ASPM_FORCE_CTL, RTSX_ASPM_FORCE_MASK, RTSX_FORCE_ASPM_NO_ASPM);
		/* Switch LDO3318 source from DV33 to 3V3. */
		RTSX_CLR(sc, RTSX_LDO_PWR_SEL, RTSX_LDO_PWR_SEL_DV33);
		RTSX_BITOP(sc, RTSX_LDO_PWR_SEL,RTSX_LDO_PWR_SEL_DV33, RTSX_LDO_PWR_SEL_3V3);
		/* Set default OLT blink period. */
		RTSX_BITOP(sc, RTSX_OLT_LED_CTL, 0x0F, RTSX_OLT_LED_PERIOD);
		pci_find_cap(sc->rtsx_dev, PCIY_EXPRESS, &reg);
		cap = pci_read_config(sc->rtsx_dev, reg + RTSX_PCI_EXP_DEVCTL2, 2);
		if (cap & RTSX_PCI_EXP_DEVCTL2_LTR_EN)
			RTSX_WRITE(sc, RTSX_LTR_CTL, 0xa3);
		/* Configure OBFF. */
		RTSX_BITOP(sc, RTSX_OBFF_CFG, RTSX_OBFF_EN_MASK, RTSX_OBFF_ENABLE);
		/* Configure driving. */
		if ((error = rtsx_rts5227_fill_driving(sc)))
			return (error);
		/* Configure force_clock_req. */
		if (sc->rtsx_flags & RTSX_REVERSE_SOCKET)
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB8, 0xB8);
		else
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB8, 0x88);
		RTSX_CLR(sc, RTSX_PM_CTRL3,  0x10);
	} else if (sc->rtsx_flags & RTSX_F_5229) {
		/* Configure GPIO as output. */
		RTSX_BITOP(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON, RTSX_GPIO_LED_ON);
		/* Reset ASPM state to default value. */
		RTSX_BITOP(sc, RTSX_ASPM_FORCE_CTL, RTSX_ASPM_FORCE_MASK, RTSX_FORCE_ASPM_NO_ASPM);
		/* Force CLKREQ# PIN to drive 0 to request clock. */
		RTSX_BITOP(sc, RTSX_PETXCFG, 0x08, 0x08);
		/* Switch LDO3318 source from DV33 to card_3v3. */
		RTSX_CLR(sc, RTSX_LDO_PWR_SEL, RTSX_LDO_PWR_SEL_DV33);
		RTSX_BITOP(sc, RTSX_LDO_PWR_SEL,RTSX_LDO_PWR_SEL_DV33, RTSX_LDO_PWR_SEL_3V3);
		/* Set default OLT blink period. */
		RTSX_BITOP(sc, RTSX_OLT_LED_CTL, 0x0F, RTSX_OLT_LED_PERIOD);
		/* Configure driving. */
		RTSX_WRITE(sc, RTSX_SD30_CMD_DRIVE_SEL, sc->rtsx_sd30_drive_sel_3v3);
	} else if (sc->rtsx_flags & RTSX_F_522A) {
		/* Add specific init from RTS5227. */
		int reg;
		uint16_t cap;

		/* Configure GPIO as output. */
		RTSX_BITOP(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON, RTSX_GPIO_LED_ON);
		/* Reset ASPM state to default value. */
		RTSX_BITOP(sc, RTSX_ASPM_FORCE_CTL, RTSX_ASPM_FORCE_MASK, RTSX_FORCE_ASPM_NO_ASPM);
		/* Switch LDO3318 source from DV33 to 3V3. */
		RTSX_CLR(sc, RTSX_LDO_PWR_SEL, RTSX_LDO_PWR_SEL_DV33);
		RTSX_BITOP(sc, RTSX_LDO_PWR_SEL,RTSX_LDO_PWR_SEL_DV33, RTSX_LDO_PWR_SEL_3V3);
		/* Set default OLT blink period. */
		RTSX_BITOP(sc, RTSX_OLT_LED_CTL, 0x0F, RTSX_OLT_LED_PERIOD);
		pci_find_cap(sc->rtsx_dev, PCIY_EXPRESS, &reg);
		cap = pci_read_config(sc->rtsx_dev, reg + RTSX_PCI_EXP_DEVCTL2, 2);
		if (cap & RTSX_PCI_EXP_DEVCTL2_LTR_EN)
			RTSX_WRITE(sc, RTSX_LTR_CTL, 0xa3);
		/* Configure OBFF. */
		RTSX_BITOP(sc, RTSX_OBFF_CFG, RTSX_OBFF_EN_MASK, RTSX_OBFF_ENABLE);
		/* Configure driving. */
		if ((error = rtsx_rts5227_fill_driving(sc)))
			return (error);
		/* Configure force_clock_req. */
		if (sc->rtsx_flags & RTSX_REVERSE_SOCKET)
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB8, 0xB8);
		else
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB8, 0x88);
		RTSX_CLR(sc, RTSX_PM_CTRL3,  0x10);

		/* specific for RTS522A. */
		RTSX_BITOP(sc, RTSX_FUNC_FORCE_CTL,
			   RTSX_FUNC_FORCE_UPME_XMT_DBG, RTSX_FUNC_FORCE_UPME_XMT_DBG);
		RTSX_BITOP(sc, RTSX_PCLK_CTL, 0x04, 0x04);
		RTSX_BITOP(sc, RTSX_PM_EVENT_DEBUG,
			   RTSX_PME_DEBUG_0, RTSX_PME_DEBUG_0);
		RTSX_WRITE(sc, RTSX_PM_CLK_FORCE_CTL, 0x11);
	} else if (sc->rtsx_flags & RTSX_F_525A) {
		/* Add specific init from RTS5249. */
		/* Rest L1SUB Config. */
		RTSX_CLR(sc, RTSX_L1SUB_CONFIG3, 0xff);
		/* Configure GPIO as output. */
		RTSX_BITOP(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON, RTSX_GPIO_LED_ON);
		/* Reset ASPM state to default value. */
		RTSX_BITOP(sc, RTSX_ASPM_FORCE_CTL, RTSX_ASPM_FORCE_MASK, RTSX_FORCE_ASPM_NO_ASPM);
		/* Switch LDO3318 source from DV33 to 3V3. */
		RTSX_CLR(sc, RTSX_LDO_PWR_SEL, RTSX_LDO_PWR_SEL_DV33);
		RTSX_BITOP(sc, RTSX_LDO_PWR_SEL,RTSX_LDO_PWR_SEL_DV33, RTSX_LDO_PWR_SEL_3V3);
		/* Set default OLT blink period. */
		RTSX_BITOP(sc, RTSX_OLT_LED_CTL, 0x0F, RTSX_OLT_LED_PERIOD);
		/* Configure driving. */
		if ((error = rtsx_rts5249_fill_driving(sc)))
			return (error);
		/* Configure force_clock_req. */
		if (sc->rtsx_flags & RTSX_REVERSE_SOCKET)
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB0, 0xB0);
		else
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB0, 0x80);

		/* Specifc for RTS525A. */
		RTSX_BITOP(sc, RTSX_PCLK_CTL, RTSX_PCLK_MODE_SEL, RTSX_PCLK_MODE_SEL);
		if (sc->rtsx_flags & RTSX_F_525A_TYPE_A) {
			RTSX_WRITE(sc, RTSX_L1SUB_CONFIG2, RTSX_L1SUB_AUTO_CFG);
			RTSX_BITOP(sc, RTSX_RREF_CFG,
				   RTSX_RREF_VBGSEL_MASK, RTSX_RREF_VBGSEL_1V25);
			RTSX_BITOP(sc, RTSX_LDO_VIO_CFG,
				   RTSX_LDO_VIO_TUNE_MASK, RTSX_LDO_VIO_1V7);
			RTSX_BITOP(sc, RTSX_LDO_DV12S_CFG,
				   RTSX_LDO_D12_TUNE_MASK, RTSX_LDO_D12_TUNE_DF);
			RTSX_BITOP(sc, RTSX_LDO_AV12S_CFG,
				   RTSX_LDO_AV12S_TUNE_MASK, RTSX_LDO_AV12S_TUNE_DF);
			RTSX_BITOP(sc, RTSX_LDO_VCC_CFG0,
				   RTSX_LDO_VCC_LMTVTH_MASK, RTSX_LDO_VCC_LMTVTH_2A);
			RTSX_BITOP(sc, RTSX_OOBS_CONFIG,
				   RTSX_OOBS_AUTOK_DIS | RTSX_OOBS_VAL_MASK, 0x89);
		}
	} else  if (sc->rtsx_flags & RTSX_F_5249) {
		/* Rest L1SUB Config. */
		RTSX_CLR(sc, RTSX_L1SUB_CONFIG3, 0xff);
		/* Configure GPIO as output. */
		RTSX_BITOP(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON, RTSX_GPIO_LED_ON);
		/* Reset ASPM state to default value. */
		RTSX_BITOP(sc, RTSX_ASPM_FORCE_CTL, RTSX_ASPM_FORCE_MASK, RTSX_FORCE_ASPM_NO_ASPM);
		/* Switch LDO3318 source from DV33 to 3V3. */
		RTSX_CLR(sc, RTSX_LDO_PWR_SEL, RTSX_LDO_PWR_SEL_DV33);
		RTSX_BITOP(sc, RTSX_LDO_PWR_SEL,RTSX_LDO_PWR_SEL_DV33, RTSX_LDO_PWR_SEL_3V3);
		/* Set default OLT blink period. */
		RTSX_BITOP(sc, RTSX_OLT_LED_CTL, 0x0F, RTSX_OLT_LED_PERIOD);
		/* Configure driving. */
		if ((error = rtsx_rts5249_fill_driving(sc)))
			return (error);
		/* Configure force_clock_req. */
		if (sc->rtsx_flags & RTSX_REVERSE_SOCKET)
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB0, 0xB0);
		else
			RTSX_BITOP(sc, RTSX_PETXCFG, 0xB0, 0x80);
	} else  if (sc->rtsx_flags & (RTSX_F_8402 | RTSX_F_8411)) {
		RTSX_WRITE(sc, RTSX_SD30_CMD_DRIVE_SEL, sc->rtsx_sd30_drive_sel_3v3);
		RTSX_BITOP(sc, RTSX_CARD_PAD_CTL, RTSX_CD_DISABLE_MASK | RTSX_CD_AUTO_DISABLE,
			   RTSX_CD_ENABLE);
	} else 	if (sc->rtsx_flags & RTSX_F_8411B) {
		if (sc->rtsx_flags & RTSX_F_8411B_QFN48)
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0xf5);
		RTSX_WRITE(sc, RTSX_SD30_CMD_DRIVE_SEL, sc->rtsx_sd30_drive_sel_3v3);
		/* Enable SD interrupt. */
		RTSX_BITOP(sc, RTSX_CARD_PAD_CTL, RTSX_CD_DISABLE_MASK | RTSX_CD_AUTO_DISABLE,
			   RTSX_CD_ENABLE);
		RTSX_BITOP(sc, RTSX_FUNC_FORCE_CTL, 0x06, 0x00);
	}

	return (0);
}

static int
rtsx_map_sd_drive(int index)
{
	uint8_t sd_drive[4] =
		{
		 0x01,	/* Type D */
		 0x02,	/* Type C */
		 0x05,	/* Type A */
		 0x03	/* Type B */
		};
	return (sd_drive[index]);
}

/* For voltage 3v3. */
static int
rtsx_rts5227_fill_driving(struct rtsx_softc *sc)
{
	u_char driving_3v3[4][3] = {
				    {0x13, 0x13, 0x13},
				    {0x96, 0x96, 0x96},
				    {0x7F, 0x7F, 0x7F},
				    {0x96, 0x96, 0x96},
	};
	RTSX_WRITE(sc, RTSX_SD30_CLK_DRIVE_SEL, driving_3v3[sc->rtsx_sd30_drive_sel_3v3][0]);
	RTSX_WRITE(sc, RTSX_SD30_CMD_DRIVE_SEL, driving_3v3[sc->rtsx_sd30_drive_sel_3v3][1]);
	RTSX_WRITE(sc, RTSX_SD30_DAT_DRIVE_SEL, driving_3v3[sc->rtsx_sd30_drive_sel_3v3][2]);

	return (0);
}

/* For voltage 3v3. */
static int
rtsx_rts5249_fill_driving(struct rtsx_softc *sc)
{
	u_char driving_3v3[4][3] = {
				    {0x11, 0x11, 0x18},
				    {0x55, 0x55, 0x5C},
				    {0xFF, 0xFF, 0xFF},
				    {0x96, 0x96, 0x96},
	};
	RTSX_WRITE(sc, RTSX_SD30_CLK_DRIVE_SEL, driving_3v3[sc->rtsx_sd30_drive_sel_3v3][0]);
	RTSX_WRITE(sc, RTSX_SD30_CMD_DRIVE_SEL, driving_3v3[sc->rtsx_sd30_drive_sel_3v3][1]);
	RTSX_WRITE(sc, RTSX_SD30_DAT_DRIVE_SEL, driving_3v3[sc->rtsx_sd30_drive_sel_3v3][2]);

	return (0);
}

static int
rtsx_read(struct rtsx_softc *sc, uint16_t addr, uint8_t *val)
{
	int tries = 1024;
	uint32_t reg;

	WRITE4(sc, RTSX_HAIMR, RTSX_HAIMR_BUSY |
	    (uint32_t)((addr & 0x3FFF) << 16));

	while (tries--) {
		reg = READ4(sc, RTSX_HAIMR);
		if (!(reg & RTSX_HAIMR_BUSY))
			break;
	}
	*val = (reg & 0xff);

	return ((tries == 0) ? ETIMEDOUT : 0);
}

static int
rtsx_read_cfg(struct rtsx_softc *sc, uint8_t func, uint16_t addr, uint32_t *val)
{
	int tries = 1024;
	uint8_t data0, data1, data2, data3, rwctl;

	RTSX_WRITE(sc, RTSX_CFGADDR0, addr);
	RTSX_WRITE(sc, RTSX_CFGADDR1, addr >> 8);
	RTSX_WRITE(sc, RTSX_CFGRWCTL, RTSX_CFG_BUSY | (func & 0x03 << 4));

	while (tries--) {
		RTSX_READ(sc, RTSX_CFGRWCTL, &rwctl);
		if (!(rwctl & RTSX_CFG_BUSY))
			break;
	}

	if (tries == 0)
		return (ETIMEDOUT);

	RTSX_READ(sc, RTSX_CFGDATA0, &data0);
	RTSX_READ(sc, RTSX_CFGDATA1, &data1);
	RTSX_READ(sc, RTSX_CFGDATA2, &data2);
	RTSX_READ(sc, RTSX_CFGDATA3, &data3);

	*val = (data3 << 24) | (data2 << 16) | (data1 << 8) | data0;

	return (0);
}

static int
rtsx_write(struct rtsx_softc *sc, uint16_t addr, uint8_t mask, uint8_t val)
{
	int tries = 1024;
	uint32_t reg;

	WRITE4(sc, RTSX_HAIMR,
	    RTSX_HAIMR_BUSY | RTSX_HAIMR_WRITE |
	    (uint32_t)(((addr & 0x3FFF) << 16) |
	    (mask << 8) | val));

	while (tries--) {
		reg = READ4(sc, RTSX_HAIMR);
		if (!(reg & RTSX_HAIMR_BUSY)) {
			if (val != (reg & 0xff))
				return (EIO);
			return (0);
		}
	}

	return (ETIMEDOUT);
}

static int
rtsx_read_phy(struct rtsx_softc *sc, uint8_t addr, uint16_t *val)
{
	int tries = 100000;
	uint8_t data0, data1, rwctl;

	RTSX_WRITE(sc, RTSX_PHY_ADDR, addr);
	RTSX_WRITE(sc, RTSX_PHY_RWCTL, RTSX_PHY_BUSY | RTSX_PHY_READ);

	while (tries--) {
		RTSX_READ(sc, RTSX_PHY_RWCTL, &rwctl);
		if (!(rwctl & RTSX_PHY_BUSY))
			break;
	}
	if (tries == 0)
		return (ETIMEDOUT);

	RTSX_READ(sc, RTSX_PHY_DATA0, &data0);
	RTSX_READ(sc, RTSX_PHY_DATA1, &data1);
	*val = data1 << 8 | data0;

	return (0);
}

static int
rtsx_write_phy(struct rtsx_softc *sc, uint8_t addr, uint16_t val)
{
	int tries = 100000;
	uint8_t rwctl;

	RTSX_WRITE(sc, RTSX_PHY_DATA0, val);
	RTSX_WRITE(sc, RTSX_PHY_DATA1, val >> 8);
	RTSX_WRITE(sc, RTSX_PHY_ADDR, addr);
	RTSX_WRITE(sc, RTSX_PHY_RWCTL, RTSX_PHY_BUSY | RTSX_PHY_WRITE);

	while (tries--) {
		RTSX_READ(sc, RTSX_PHY_RWCTL, &rwctl);
		if (!(rwctl & RTSX_PHY_BUSY))
			break;
	}

	return ((tries == 0) ? ETIMEDOUT : 0);
}

static int
rtsx_set_sd_timing(struct rtsx_softc *sc, enum mmc_bus_timing timing)
{

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_set_sd_timing(%u)\n", timing);

	switch (timing) {
	case bus_timing_hs:
		RTSX_BITOP(sc, RTSX_SD_CFG1, 0x0C, RTSX_SD20_MODE);
		RTSX_BITOP(sc, RTSX_CLK_CTL, RTSX_CLK_LOW_FREQ, RTSX_CLK_LOW_FREQ);
		RTSX_BITOP(sc, RTSX_CARD_CLK_SOURCE, 0xff,
			   RTSX_CRC_FIX_CLK | RTSX_SD30_VAR_CLK0 | RTSX_SAMPLE_VAR_CLK1);
		RTSX_BITOP(sc, RTSX_CLK_CTL, RTSX_CLK_LOW_FREQ, 0x00);
		RTSX_BITOP(sc, RTSX_SD_PUSH_POINT_CTL,
			   RTSX_SD20_TX_SEL_MASK, RTSX_SD20_TX_14_AHEAD);
		RTSX_BITOP(sc, RTSX_SD_SAMPLE_POINT_CTL,
			   RTSX_SD20_RX_SEL_MASK, RTSX_SD20_RX_14_DELAY);
		break;
	default:
		RTSX_BITOP(sc, RTSX_SD_CFG1, 0x0C, RTSX_SD20_MODE);
		RTSX_BITOP(sc, RTSX_CLK_CTL, RTSX_CLK_LOW_FREQ, RTSX_CLK_LOW_FREQ);
		RTSX_BITOP(sc, RTSX_CARD_CLK_SOURCE, 0xff,
			   RTSX_CRC_FIX_CLK | RTSX_SD30_VAR_CLK0 | RTSX_SAMPLE_VAR_CLK1);
		RTSX_BITOP(sc, RTSX_CLK_CTL, RTSX_CLK_LOW_FREQ, 0x00);
		RTSX_BITOP(sc, RTSX_SD_PUSH_POINT_CTL, 0xFF, RTSX_SD20_TX_NEG_EDGE);
		RTSX_BITOP(sc, RTSX_SD_SAMPLE_POINT_CTL, RTSX_SD20_RX_SEL_MASK, RTSX_SD20_RX_POS_EDGE);
	}

	return (0);
}

/*
 * Set or change SDCLK frequency or disable the SD clock.
 * Return zero on success.
 */
static int
rtsx_set_sd_clock(struct rtsx_softc *sc, uint32_t freq)
{
	uint8_t n;
	int div;
	int mcu;
	int error = 0;

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_set_sd_clock(%u)\n", freq);

	if (freq == RTSX_SDCLK_OFF) {
		error = rtsx_stop_sd_clock(sc);
		goto done;
	}

	/* Round down to a supported frequency. */
	if (freq >= RTSX_SDCLK_50MHZ)
		freq = RTSX_SDCLK_50MHZ;
	else if (freq >= RTSX_SDCLK_25MHZ)
		freq = RTSX_SDCLK_25MHZ;
	else
		freq = RTSX_SDCLK_400KHZ;

	/*
	 * Configure the clock frequency.
	 */
	switch (freq) {
	case RTSX_SDCLK_400KHZ:
		n = 80; /* minimum */
		div = RTSX_CLK_DIV_8;
		mcu = 7;
		/*!!!*/
//		RTSX_BITOP(sc, RTSX_SD_CFG1, RTSX_CLK_DIVIDE_MASK, RTSX_CLK_DIVIDE_128);
		RTSX_CLR(sc, RTSX_SD_CFG1, RTSX_CLK_DIVIDE_MASK);
		break;
	case RTSX_SDCLK_25MHZ:
		n = 100;
		div = RTSX_CLK_DIV_4;
		mcu = 7;
		RTSX_CLR(sc, RTSX_SD_CFG1, RTSX_CLK_DIVIDE_MASK);
		break;
	case RTSX_SDCLK_50MHZ:
		n = 100;
		div = RTSX_CLK_DIV_2;
		mcu = 7;
		RTSX_CLR(sc, RTSX_SD_CFG1, RTSX_CLK_DIVIDE_MASK);
		break;
	default:
		error = EINVAL;
		goto done;
	}

	/* Enable SD clock. */
	error = rtsx_switch_sd_clock(sc, n, div, mcu);

 done:
	return (error);
}

static int
rtsx_stop_sd_clock(struct rtsx_softc *sc)
{
	RTSX_CLR(sc, RTSX_CARD_CLK_EN, RTSX_CARD_CLK_EN_ALL);
	RTSX_SET(sc, RTSX_SD_BUS_STAT, RTSX_SD_CLK_FORCE_STOP);
	return (0);
}

static int
rtsx_switch_sd_clock(struct rtsx_softc *sc, uint8_t n, int div, int mcu)
{
	/* Enable SD 2.0 mode. */
	RTSX_CLR(sc, RTSX_SD_CFG1, RTSX_SD_MODE_MASK);

	RTSX_SET(sc, RTSX_CLK_CTL, RTSX_CLK_LOW_FREQ);

	RTSX_WRITE(sc, RTSX_CARD_CLK_SOURCE,
		   RTSX_CRC_FIX_CLK | RTSX_SD30_VAR_CLK0 | RTSX_SAMPLE_VAR_CLK1);
	RTSX_CLR(sc, RTSX_SD_SAMPLE_POINT_CTL, RTSX_SD20_RX_SEL_MASK);
	RTSX_WRITE(sc, RTSX_SD_PUSH_POINT_CTL, RTSX_SD20_TX_NEG_EDGE);
	RTSX_WRITE(sc, RTSX_CLK_DIV, (div << 4) | mcu);
	RTSX_CLR(sc, RTSX_SSC_CTL1, RTSX_RSTB);
	RTSX_CLR(sc, RTSX_SSC_CTL2, RTSX_SSC_DEPTH_MASK);
	RTSX_WRITE(sc, RTSX_SSC_DIV_N_0, n);
	RTSX_SET(sc, RTSX_SSC_CTL1, RTSX_RSTB);

	DELAY(200);

	RTSX_CLR(sc, RTSX_CLK_CTL, RTSX_CLK_LOW_FREQ);
	return (0);
}

/*
 * Notice that the meaning of RTSX_PWR_GATE_CTRL changes between RTS5209 and
 * RTS5229. In RTS5209 it is a mask of disabled power gates, while in RTS5229
 * it is a mask of *enabled* gates.
 */
static int
rtsx_bus_power_off(struct rtsx_softc *sc)
{
	int error;

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_bus_power_off()\n");

	if ((error = rtsx_stop_sd_clock(sc)))
		return (error);

	/* Disable SD clock. */
	RTSX_CLR(sc, RTSX_CARD_CLK_EN, RTSX_SD_CLK_EN);

	/* Disable SD output. */
	RTSX_CLR(sc, RTSX_CARD_OE, RTSX_CARD_OUTPUT_EN);

	/* Turn off power. */
	if (sc->rtsx_flags & RTSX_F_5209) {
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_SD_PWR_MASK | RTSX_PMOS_STRG_MASK,
			   RTSX_SD_PWR_OFF | RTSX_PMOS_STRG_400mA);
		RTSX_SET(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_OFF);
	} else if (sc->rtsx_flags & (RTSX_F_5227 | RTSX_F_5229 | RTSX_F_522A)) {
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_SD_PWR_MASK | RTSX_PMOS_STRG_MASK,
			   RTSX_SD_PWR_OFF | RTSX_PMOS_STRG_400mA);
		RTSX_CLR(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK);
	} else if (sc->rtsx_flags & (RTSX_F_8402 | RTSX_F_8411 | RTSX_F_8411B)) {
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_BPP_POWER_MASK,
			   RTSX_BPP_POWER_OFF);
		RTSX_BITOP(sc, RTSX_LDO_CTL, RTSX_BPP_LDO_POWB,
			   RTSX_BPP_LDO_SUSPEND);
  	} else {
		RTSX_CLR(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK);
		RTSX_SET(sc, RTSX_CARD_PWR_CTL, RTSX_SD_PWR_OFF);
		RTSX_CLR(sc, RTSX_CARD_PWR_CTL, RTSX_PMOS_STRG_800mA);
	}

	/* Disable pull control. */
	if (sc->rtsx_flags & RTSX_F_5209) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, RTSX_PULL_CTL_DISABLE12);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, RTSX_PULL_CTL_DISABLE12);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_DISABLE3);
	} else if (sc->rtsx_flags & (RTSX_F_5227 | RTSX_F_5229 | RTSX_F_522A)) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, RTSX_PULL_CTL_DISABLE12);
		if (sc->rtsx_flags & RTSX_F_5229_TYPE_C)
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_DISABLE3_TYPE_C);
		else
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_DISABLE3);
	} else if (sc->rtsx_flags & (RTSX_F_525A | RTSX_F_5249)) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, 0x66);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, RTSX_PULL_CTL_DISABLE12);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_DISABLE3);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL4, 0x55);
	} else if (sc->rtsx_flags & (RTSX_F_8402 | RTSX_F_8411)) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, 0x65);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, 0x55);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0x95);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL4, 0x09);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL5, 0x05);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL6, 0x04);
	} else if (sc->rtsx_flags & RTSX_F_8411B) {
		if (sc->rtsx_flags & RTSX_F_8411B_QFN48) {
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, 0x55);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0xf5);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL6, 0x15);
		} else {
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, 0x65);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, 0x55);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0xd5);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL4, 0x59);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL5, 0x55);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL6, 0x15);
		}
	}

	return (0);
}

static int
rtsx_bus_power_on(struct rtsx_softc *sc)
{
	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_bus_power_on()\n");

	/* Select SD card. */
	RTSX_WRITE(sc, RTSX_CARD_SELECT, RTSX_SD_MOD_SEL);
	RTSX_WRITE(sc, RTSX_CARD_SHARE_MODE, RTSX_CARD_SHARE_48_SD);

	/* Enable SD clock. */
	RTSX_SET(sc, RTSX_CARD_CLK_EN, RTSX_SD_CLK_EN);

	/* Enable pull control. */
	if (sc->rtsx_flags & RTSX_F_5209) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, RTSX_PULL_CTL_ENABLE12);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, RTSX_PULL_CTL_ENABLE12);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_ENABLE3);
	} else if (sc->rtsx_flags & (RTSX_F_5227 | RTSX_F_5229 | RTSX_F_522A)) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, RTSX_PULL_CTL_ENABLE12);
		if (sc->rtsx_flags & RTSX_F_5229_TYPE_C)
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_ENABLE3_TYPE_C);
		else
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_ENABLE3);
	} else if (sc->rtsx_flags & (RTSX_F_525A | RTSX_F_5249)) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, 0x66);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, RTSX_PULL_CTL_ENABLE12);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, RTSX_PULL_CTL_ENABLE3);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL4, 0xaa);
	} if (sc->rtsx_flags & (RTSX_F_8402 | RTSX_F_8411)) {
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, 0xaa);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, 0xaa);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0xa9);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL4, 0x09);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL5, 0x09);
		RTSX_WRITE(sc, RTSX_CARD_PULL_CTL6, 0x04);
	} else if (sc->rtsx_flags & RTSX_F_8411B) {
		if (sc->rtsx_flags & RTSX_F_8411B_QFN48) {
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, 0xaa);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0xf9);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL6, 0x19);
		} else {
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL1, 0xaa);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL2, 0xaa);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL3, 0xd9);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL4, 0x59);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL5, 0x55);
			RTSX_WRITE(sc, RTSX_CARD_PULL_CTL6, 0x15);
		}
	}

	/*
	 * To avoid a current peak, enable card power in two phases
	 * with a delay in between.
	 */
	if (sc->rtsx_flags & (RTSX_F_8402 | RTSX_F_8411 | RTSX_F_8411B)) {
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_BPP_POWER_MASK,
			   RTSX_BPP_POWER_5_PERCENT_ON);
		RTSX_BITOP(sc, RTSX_LDO_CTL, RTSX_BPP_LDO_POWB,
			   RTSX_BPP_LDO_SUSPEND);
		DELAY(150);
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_BPP_POWER_MASK,
			   RTSX_BPP_POWER_10_PERCENT_ON);
		DELAY(150);
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_BPP_POWER_MASK,
			   RTSX_BPP_POWER_15_PERCENT_ON);
		DELAY(150);
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_BPP_POWER_MASK,
			   RTSX_BPP_POWER_ON);
		RTSX_BITOP(sc, RTSX_LDO_CTL, RTSX_BPP_LDO_POWB,
			   RTSX_BPP_LDO_ON);
	} else {
		if (sc->rtsx_flags & RTSX_F_525A)
			RTSX_BITOP(sc, RTSX_LDO_VCC_CFG1, RTSX_LDO_VCC_TUNE_MASK, RTSX_LDO_VCC_3V3);

		/* Partial power. */
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_SD_PWR_MASK, RTSX_SD_PARTIAL_PWR_ON);
		if (sc->rtsx_flags & RTSX_F_5209)
			RTSX_BITOP(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK, RTSX_LDO3318_VCC2);
		else
			RTSX_BITOP(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK, RTSX_LDO3318_VCC1);

		DELAY(200);

		/* Full power. */
		RTSX_BITOP(sc, RTSX_CARD_PWR_CTL, RTSX_SD_PWR_MASK, RTSX_SD_PWR_ON);
		if (sc->rtsx_flags & RTSX_F_5209)
			RTSX_BITOP(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK, RTSX_LDO3318_ON);
		else if (sc->rtsx_flags & (RTSX_F_5227 | RTSX_F_5229 | RTSX_F_522A | RTSX_F_525A | RTSX_F_5249))
			RTSX_BITOP(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK,
				   RTSX_LDO3318_VCC1 | RTSX_LDO3318_VCC2);
		else
			RTSX_BITOP(sc, RTSX_PWR_GATE_CTRL, RTSX_LDO3318_PWR_MASK, RTSX_LDO3318_VCC2);
	}

	/* Enable SD card output. */
	RTSX_WRITE(sc, RTSX_CARD_OE, RTSX_SD_OUTPUT_EN);

	DELAY(200);

	return (0);
}

#if 0  /* For led usage */
static int
rtsx_led_enable(struct rtsx_softc *sc)
{
	if (sc->rtsx_flags & RTSX_F_5209) {
		RTSX_CLR(sc, RTSX_CARD_GPIO, RTSX_CARD_GPIO_LED_OFF);
		RTSX_WRITE(sc, RTSX_CARD_AUTO_BLINK,
			   RTSX_LED_BLINK_EN | RTSX_LED_BLINK_SPEED);
	} else if (sc->rtsx_flags & RTSX_F_8411B) {
		RTSX_CLR(sc, RTSX_GPIO_CTL, 0x01);
		RTSX_WRITE(sc, RTSX_CARD_AUTO_BLINK,
			   RTSX_LED_BLINK_EN | RTSX_LED_BLINK_SPEED);	
	} else {
		RTSX_SET(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON);
		RTSX_SET(sc, RTSX_OLT_LED_CTL, RTSX_OLT_LED_AUTOBLINK);
	}

	return (0);
}

static int
rtsx_led_disable(struct rtsx_softc *sc)
{
	if (sc->rtsx_flags & RTSX_F_5209) {
		RTSX_CLR(sc, RTSX_CARD_AUTO_BLINK, RTSX_LED_BLINK_EN);
		RTSX_WRITE(sc, RTSX_CARD_GPIO, RTSX_CARD_GPIO_LED_OFF);
	} else if (sc->rtsx_flags & RTSX_F_8411B) {
		RTSX_CLR(sc, RTSX_CARD_AUTO_BLINK, RTSX_LED_BLINK_EN);
		RTSX_SET(sc, RTSX_GPIO_CTL, 0x01);
	} else {
		RTSX_CLR(sc, RTSX_OLT_LED_CTL, RTSX_OLT_LED_AUTOBLINK);
		RTSX_CLR(sc, RTSX_GPIO_CTL, RTSX_GPIO_LED_ON);
	}

	return (0);
}
#endif /* For led usage */

static uint8_t
rtsx_response_type(uint16_t mmc_rsp)
{
	int i;
	struct rsp_type {
		uint16_t mmc_rsp;
		uint8_t  rtsx_rsp;
	} rsp_types[] = {
		{ MMC_RSP_NONE,	RTSX_SD_RSP_TYPE_R0 },
		{ MMC_RSP_R1,	RTSX_SD_RSP_TYPE_R1 },
		{ MMC_RSP_R1B,	RTSX_SD_RSP_TYPE_R1B },
		{ MMC_RSP_R2,	RTSX_SD_RSP_TYPE_R2 },
		{ MMC_RSP_R3,	RTSX_SD_RSP_TYPE_R3 },
		{ MMC_RSP_R4,	RTSX_SD_RSP_TYPE_R4 },
		{ MMC_RSP_R5,	RTSX_SD_RSP_TYPE_R5 },
		{ MMC_RSP_R6,	RTSX_SD_RSP_TYPE_R6 },
		{ MMC_RSP_R7,	RTSX_SD_RSP_TYPE_R7 }
	};

	for (i = 0; i < nitems(rsp_types); i++) {
		if (mmc_rsp == rsp_types[i].mmc_rsp)
			return (rsp_types[i].rtsx_rsp);
	}

	return (0);
}

/*
 * Init command buffer with SD command index and argument.
 */
static void
rtsx_init_cmd(struct rtsx_softc *sc, struct mmc_command *cmd)
{
	sc->rtsx_cmd_index = 0;
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CMD0,
		      0xff, RTSX_SD_CMD_START  | cmd->opcode); 
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CMD1,
		     0xff, cmd->arg >> 24);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CMD2,
		      0xff, cmd->arg >> 16);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CMD3,
		     0xff, cmd->arg >> 8);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CMD4,
		     0xff, cmd->arg);
}

/*
 * Append a properly encoded host command to the host command buffer.
 */
static void
rtsx_push_cmd(struct rtsx_softc *sc, uint8_t cmd, uint16_t reg,
	      uint8_t mask, uint8_t data)
{
	KASSERT(sc->rtsx_cmd_index < RTSX_HOSTCMD_MAX,
		("rtsx: Too many host commands (%d)\n", sc->rtsx_cmd_index));

	uint32_t *cmd_buffer = (uint32_t *)(sc->rtsx_cmd_dmamem);
	cmd_buffer[sc->rtsx_cmd_index++] =
		htole32((uint32_t)(cmd & 0x3) << 30) |
		((uint32_t)(reg & 0x3fff) << 16) |
		((uint32_t)(mask) << 8) |
		((uint32_t)data);
}

/*
 * Run the command queue and wait for completion.
 */
static int
rtsx_send_cmd(struct rtsx_softc *sc, struct mmc_command *cmd)
{
	int error = 0;

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_send_cmd()\n");

	sc->rtsx_intr_status = 0;

	/* Sync command DMA buffer. */
	bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_PREREAD);
	bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_PREWRITE);

	/* Tell the chip where the command buffer is and run the commands. */
	WRITE4(sc, RTSX_HCBAR, (uint32_t)sc->rtsx_cmd_buffer);
	WRITE4(sc, RTSX_HCBCTLR,
	       ((sc->rtsx_cmd_index * 4) & 0x00ffffff) | RTSX_START_CMD | RTSX_HW_AUTO_RSP);

	if ((error = rtsx_wait_intr(sc, RTSX_TRANS_OK_INT, hz * sc->rtsx_timeout)))
		cmd->error = error;

	return (error);
}

/*
 * Run the command queue and don't wait for completion.
 */
static void
rtsx_send_cmd_nowait(struct rtsx_softc *sc, struct mmc_command *cmd)
{

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_send_cmd_nowait()\n");

	sc->rtsx_intr_status = 0;
	/* Sync command DMA buffer. */
	bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_PREREAD);
	bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_PREWRITE);

	/* Tell the chip where the command buffer is and run the commands. */
	WRITE4(sc, RTSX_HCBAR, (uint32_t)sc->rtsx_cmd_buffer);
	WRITE4(sc, RTSX_HCBCTLR,
	       ((sc->rtsx_cmd_index * 4) & 0x00ffffff) | RTSX_START_CMD | RTSX_HW_AUTO_RSP);
}

static void
rtsx_req_done(struct rtsx_softc *sc)
{
	struct mmc_request *req;

	req = sc->rtsx_req;
	if (req->cmd->error != MMC_ERR_NONE)
		rtsx_soft_reset(sc);
	sc->rtsx_req = NULL;
	req->done(req);
}

/*
 * Prepare for another command.
 */
static void
rtsx_soft_reset(struct rtsx_softc *sc)
{
	device_printf(sc->rtsx_dev, "Soft reset\n");

	/* Stop command transfer. */
	WRITE4(sc, RTSX_HCBCTLR, RTSX_STOP_CMD);

	/* Stop DMA transfer. */
	WRITE4(sc, RTSX_HDBCTLR, RTSX_STOP_DMA);

	(void)rtsx_write(sc, RTSX_DMACTL, RTSX_DMA_RST, RTSX_DMA_RST);

	(void)rtsx_write(sc, RTSX_RBCTL, RTSX_RB_FLUSH, RTSX_RB_FLUSH);

	/* Clear error. */
	(void)rtsx_write(sc, RTSX_CARD_STOP, RTSX_SD_STOP|RTSX_SD_CLR_ERR,
			 RTSX_SD_STOP|RTSX_SD_CLR_ERR);
}

static int
rtsx_send_req_get_resp(struct rtsx_softc *sc, struct mmc_command *cmd) {
	uint8_t rsp_type;
	uint16_t reg;
	int error = 0;

	/* Convert response type. */
	rsp_type = rtsx_response_type(cmd->flags & MMC_RSP_MASK);
	if (rsp_type == 0) {
		device_printf(sc->rtsx_dev, "Unknown response type 0x%lx\n", (cmd->flags & MMC_RSP_MASK));
		cmd->error = MMC_ERR_INVALID;
		return (MMC_ERR_INVALID);
	}

	/* Select SD card. */
	/*!!! Only in Linux. */
//	RTSX_WRITE(sc, RTSX_CARD_SELECT, RTSX_SD_MOD_SEL);
//	RTSX_WRITE(sc, RTSX_CARD_SHARE_MODE, RTSX_CARD_SHARE_48_SD);

	rtsx_init_cmd(sc, cmd);

	/* Queue command to set response type. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CFG2,
		      0xff, rsp_type);

	/* Use the ping-pong buffer (cmd buffer) for commands which do not transfer data. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_CARD_DATA_SOURCE,
		      0x01, RTSX_PINGPONG_BUFFER);

	/* Queue commands to perform SD transfer. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_TRANSFER,
		      0xff, RTSX_TM_CMD_RSP | RTSX_SD_TRANSFER_START);
	rtsx_push_cmd(sc, RTSX_CHECK_REG_CMD, RTSX_SD_TRANSFER,
		      RTSX_SD_TRANSFER_END|RTSX_SD_STAT_IDLE,
		      RTSX_SD_TRANSFER_END|RTSX_SD_STAT_IDLE);

	/* If needed queue commands to read back card status response. */
	if (rsp_type == RTSX_SD_RSP_TYPE_R2) {
		/* Read data from ping-pong buffer. */
		for (reg = RTSX_PPBUF_BASE2; reg < RTSX_PPBUF_BASE2 + 16; reg++)
			rtsx_push_cmd(sc, RTSX_READ_REG_CMD, reg, 0, 0);
	} else if (rsp_type != RTSX_SD_RSP_TYPE_R0) {
		/* Read data from SD_CMDx registers. */
		for (reg = RTSX_SD_CMD0; reg <= RTSX_SD_CMD4; reg++)
			rtsx_push_cmd(sc, RTSX_READ_REG_CMD, reg, 0, 0);
	}
	rtsx_push_cmd(sc, RTSX_READ_REG_CMD, RTSX_SD_STAT1, 0, 0);

	/* Run the command queue and wait for completion. */
	if ((error = rtsx_send_cmd(sc, cmd)))
		return (error);

	/* Sync command DMA buffer. */
	bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_POSTREAD);
	bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_POSTWRITE);

	/* Copy card response into mmc response buffer. */
	if (ISSET(cmd->flags, MMC_RSP_PRESENT)) {
		uint32_t *cmd_buffer = (uint32_t *)(sc->rtsx_cmd_dmamem);

		if (bootverbose) {
			device_printf(sc->rtsx_dev, "cmd_buffer: 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x\n",
				      cmd_buffer[0], cmd_buffer[1], cmd_buffer[2], cmd_buffer[3], cmd_buffer[4]);
		}

		if (rsp_type == RTSX_SD_RSP_TYPE_R2) {
			/* First byte is CHECK_REG_CMD return value, skip it. */
			unsigned char *ptr = (unsigned char *)cmd_buffer + 1;
			int i;

			/*
			 * The controller offloads the last byte {CRC-7, end bit 1}
			 * of response type R2. Assign dummy CRC, 0, and end bit to this
			 * byte (ptr[16], goes into the LSB of resp[3] later).
			 */
			ptr[16] = 0x01;
			/* The second byte is the status of response, skip it. */
			for (i = 0; i < 4; i++)
				cmd->resp[i] = be32dec(ptr + 1 + i * 4);
		} else {
			/*
			 * First byte is CHECK_REG_CMD return value, second
			 * one is the command op code -- we skip those.
			 */
			cmd->resp[0] =
				((be32toh(cmd_buffer[0]) & 0x0000ffff) << 16) |
				((be32toh(cmd_buffer[1]) & 0xffff0000) >> 16);
		}

		if (bootverbose)
			device_printf(sc->rtsx_dev, "cmd->resp = 0x%08x 0x%08x 0x%08x 0x%08x\n",
				      cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);
		
	}
	return (error);
}

static int
rtsx_xfer_short(struct rtsx_softc *sc, struct mmc_command *cmd)
{
	uint8_t rsp_type;
	int read;
	int error = 0;

	if (cmd->data == NULL || cmd->data->len == 0) {
		cmd->error = MMC_ERR_INVALID;
		return (MMC_ERR_INVALID);
	}
	if (cmd->data->xfer_len == 0)
		cmd->data->xfer_len = (cmd->data->len > RTSX_MAX_DATA_BLKLEN) ?
			RTSX_MAX_DATA_BLKLEN : cmd->data->len;

	read = ISSET(cmd->data->flags, MMC_DATA_READ);

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_xfer_short() - %s xfer: %ld bytes with block size %ld\n",
			      read ? "Read" : "Write",
			      (unsigned long)cmd->data->len, (unsigned long)cmd->data->xfer_len);

	if (cmd->data->len > 512) {
		device_printf(sc->rtsx_dev, "rtsx_xfer_short() length too large: %ld > 512\n",
			      (unsigned long)cmd->data->len);
		cmd->error = MMC_ERR_INVALID;
		return (MMC_ERR_INVALID);
	}

	rsp_type = rtsx_response_type(cmd->flags & MMC_RSP_MASK);
	if (rsp_type == 0) {
		device_printf(sc->rtsx_dev, "Unknown response type 0x%lx\n", (cmd->flags & MMC_RSP_MASK));
		cmd->error = MMC_ERR_INVALID;
		return (MMC_ERR_INVALID);
	}

	read = ISSET(cmd->data->flags, MMC_DATA_READ);

	if (read) {
		rtsx_init_cmd(sc, cmd);

		/* Queue commands to configure data transfer size. */
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BYTE_CNT_L,
			      0xff, (cmd->data->xfer_len & 0xff));
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BYTE_CNT_H,
			      0xff, (cmd->data->xfer_len >> 8));
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BLOCK_CNT_L,
			      0xff, ((cmd->data->len / cmd->data->xfer_len) & 0xff));
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BLOCK_CNT_H,
			      0xff, ((cmd->data->len / cmd->data->xfer_len) >> 8));

		/* from linux: rtsx_pci_sdmmc.c sd_read_data(). */
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CFG2,
			      0xff, RTSX_SD_CALCULATE_CRC7 | RTSX_SD_CHECK_CRC16 |
			      RTSX_SD_NO_WAIT_BUSY_END | RTSX_SD_CHECK_CRC7 | RTSX_SD_RSP_LEN_6);

		/* Use the ping-pong buffer (cmd buffer). */
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_CARD_DATA_SOURCE,
			      0x01, RTSX_PINGPONG_BUFFER);

		/* Queue commands to perform SD transfer. */
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_TRANSFER,
			      0xff, RTSX_TM_NORMAL_READ | RTSX_SD_TRANSFER_START);
		rtsx_push_cmd(sc, RTSX_CHECK_REG_CMD, RTSX_SD_TRANSFER,
			      RTSX_SD_TRANSFER_END, RTSX_SD_TRANSFER_END);

		/* Run the command queue and wait for completion. */
		if ((error = rtsx_send_cmd(sc, cmd)))
			return (error);

		error = rtsx_read_ppbuf(sc, cmd);

		if (bootverbose && error == 0 && cmd->opcode == ACMD_SEND_SCR) {
			uint8_t *ptr = cmd->data->data;
			device_printf(sc->rtsx_dev, "SCR = 0x%02x%02x%02x%02x%02x%02x%02x%02x\n",
				      ptr[0], ptr[1], ptr[2], ptr[3],
				      ptr[4], ptr[5], ptr[6], ptr[7]);
		}
	} else {
		if ((error = rtsx_send_req_get_resp(sc, cmd)))
			return (error);

		if ((error = rtsx_write_ppbuf(sc, cmd)))
			return (error);

		sc->rtsx_cmd_index = 0;

		/* Queue commands to configure data transfer size. */
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BYTE_CNT_L,
			      0xff, (cmd->data->xfer_len & 0xff));
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BYTE_CNT_H,
			      0xff, (cmd->data->xfer_len >> 8));
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BLOCK_CNT_L,
			      0xff, ((cmd->data->len / cmd->data->xfer_len) & 0xff));
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BLOCK_CNT_H,
			      0xff, ((cmd->data->len / cmd->data->xfer_len) >> 8));

		/* from linux: rtsx_pci_sdmmc.c sd_write_data(). */
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CFG2,
			      0xff, RTSX_SD_CALCULATE_CRC7 | RTSX_SD_CHECK_CRC16 |
			      RTSX_SD_NO_WAIT_BUSY_END | RTSX_SD_CHECK_CRC7 | RTSX_SD_RSP_LEN_0);
	
		rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_TRANSFER, 0xff,
			      RTSX_TM_AUTO_WRITE3 | RTSX_SD_TRANSFER_START);
		rtsx_push_cmd(sc, RTSX_CHECK_REG_CMD, RTSX_SD_TRANSFER,
			      RTSX_SD_TRANSFER_END, RTSX_SD_TRANSFER_END);

		error = rtsx_send_cmd(sc, cmd);
	}

	return (error);
}

/*
 * Use the ping-pong buffer (cmd buffer) for transfer <= 512 bytes.
 */
static int
rtsx_read_ppbuf(struct rtsx_softc *sc, struct mmc_command *cmd)
{

	uint16_t reg = RTSX_PPBUF_BASE2;
	uint8_t *ptr = cmd->data->data;
	int remain = cmd->data->len;
	int i, j;
	int error;

	for (j = 0; j < cmd->data->len / RTSX_HOSTCMD_MAX; j++) {
		sc->rtsx_cmd_index = 0;
		for (i = 0; i < RTSX_HOSTCMD_MAX; i++) {
			rtsx_push_cmd(sc, RTSX_READ_REG_CMD, reg++,
				      0, 0);
		}
		if ((error = rtsx_send_cmd(sc, cmd)))
		    return (error);

		/* Sync command DMA buffer. */
		bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_POSTREAD);
		bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_POSTWRITE);

		memcpy(ptr, sc->rtsx_cmd_dmamem, RTSX_HOSTCMD_MAX);
		ptr += RTSX_HOSTCMD_MAX;
		remain -= RTSX_HOSTCMD_MAX;
	}
	if (remain > 0) {
		sc->rtsx_cmd_index = 0;
		for (i = 0; i < remain; i++) {
			rtsx_push_cmd(sc, RTSX_READ_REG_CMD, reg++,
				      0, 0);
		}
		if ((error = rtsx_send_cmd(sc, cmd)))
			return (error);

		/* Sync command DMA buffer. */
		bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_POSTREAD);
		bus_dmamap_sync(sc->rtsx_cmd_dma_tag, sc->rtsx_cmd_dmamap, BUS_DMASYNC_POSTWRITE);

		memcpy(ptr, sc->rtsx_cmd_dmamem, remain);
	}

	return (0);
}

/*
 * Use the ping-pong buffer (cmd buffer) for transfer <= 512 bytes.
 */
static int
rtsx_write_ppbuf(struct rtsx_softc *sc, struct mmc_command *cmd)
{
	uint16_t reg = RTSX_PPBUF_BASE2;
	uint8_t *ptr = cmd->data->data;
	int remain = cmd->data->len;
	int i, j;
	int error;

	for (j = 0; j < cmd->data->len / RTSX_HOSTCMD_MAX; j++) {
		sc->rtsx_cmd_index = 0;
		for (i = 0; i < RTSX_HOSTCMD_MAX; i++) {
			rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, reg++,
				      0xff, *ptr);
			ptr++;
		}
		if ((error = rtsx_send_cmd(sc, cmd)))
		    return (error);

		remain -= RTSX_HOSTCMD_MAX;
	}
	if (remain > 0) {
		sc->rtsx_cmd_index = 0;
		for (i = 0; i < remain; i++) {
			rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, reg++,
				      0xff, *ptr);
			ptr++;
		}
		if ((error = rtsx_send_cmd(sc, cmd)))
			return (error);
	}

	return (0);
}

/*
 * Use the data buffer for transfer > 512 bytes.
 */
static int
rtsx_xfer(struct rtsx_softc *sc, struct mmc_command *cmd)
{
	uint8_t cfg2;
	int read = ISSET(cmd->data->flags, MMC_DATA_READ);
	int dma_dir;
	int tmode;
	int error = 0;

	if (cmd->data->xfer_len == 0)
		cmd->data->xfer_len = (cmd->data->len > RTSX_MAX_DATA_BLKLEN) ?
			RTSX_MAX_DATA_BLKLEN : cmd->data->len;

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_xfer() - %s xfer: %ld bytes with block size %ld\n",
			      read ? "Read" : "Write",
			      (unsigned long)cmd->data->len, (unsigned long)cmd->data->xfer_len);

	if (cmd->data->len > RTSX_DMA_DATA_BUFSIZE) {
		device_printf(sc->rtsx_dev, "rtsx_xfer() length too large: %ld > %d\n",
			      (unsigned long)cmd->data->len, RTSX_DMA_DATA_BUFSIZE);
		cmd->error = MMC_ERR_INVALID;
		return (MMC_ERR_INVALID);
	}

	if (!read) {
		if ((error = rtsx_send_req_get_resp(sc, cmd)))
			return (error);
	}

	/* Configure DMA transfer mode parameters. */
	if (cmd->opcode == MMC_READ_MULTIPLE_BLOCK)
		cfg2 = RTSX_SD_CHECK_CRC16 | RTSX_SD_NO_WAIT_BUSY_END | RTSX_SD_RSP_LEN_6;
	else
		cfg2 = RTSX_SD_CHECK_CRC16 | RTSX_SD_NO_WAIT_BUSY_END | RTSX_SD_RSP_LEN_0;
	if (read) {
		dma_dir = RTSX_DMA_DIR_FROM_CARD;
		/*
		 * Use transfer mode AUTO_READ1, which assume we not
		 * already send the read command and don't need to send
		 * CMD 12 manually after read.
		 */
     		tmode = RTSX_TM_AUTO_READ1;
		cfg2 |= RTSX_SD_CALCULATE_CRC7 | RTSX_SD_CHECK_CRC7;

		rtsx_init_cmd(sc, cmd);
	} else {
		dma_dir = RTSX_DMA_DIR_TO_CARD;
		/*
		 * Use transfer mode AUTO_WRITE3, wich assumes we've already
		 * sent the write command and gotten the response, and will
		 * send CMD 12 manually after writing.
		 */
		tmode = RTSX_TM_AUTO_WRITE3;
		cfg2 |= RTSX_SD_NO_CALCULATE_CRC7 | RTSX_SD_NO_CHECK_CRC7;

		sc->rtsx_cmd_index = 0;
	}

	/* Queue commands to configure data transfer size. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BYTE_CNT_L,
		      0xff, (cmd->data->xfer_len & 0xff));
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BYTE_CNT_H,
		      0xff, (cmd->data->xfer_len >> 8));
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BLOCK_CNT_L,
		      0xff, ((cmd->data->len / cmd->data->xfer_len) & 0xff));
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_BLOCK_CNT_H,
		      0xff, ((cmd->data->len / cmd->data->xfer_len) >> 8));

	/* Configure DMA controller. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_IRQSTAT0,
		     RTSX_DMA_DONE_INT, RTSX_DMA_DONE_INT);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_DMATC3,
		     0xff, cmd->data->len >> 24);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_DMATC2,
		     0xff, cmd->data->len >> 16);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_DMATC1,
		     0xff, cmd->data->len >> 8);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_DMATC0,
		     0xff, cmd->data->len);
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_DMACTL,
		     RTSX_DMA_EN | RTSX_DMA_DIR | RTSX_DMA_PACK_SIZE_MASK,
		     RTSX_DMA_EN | dma_dir | RTSX_DMA_512);

	/* Use the DMA ring buffer for commands which transfer data. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_CARD_DATA_SOURCE,
		      0x01, RTSX_RING_BUFFER);

	/* Queue command to set response type. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_CFG2,
		     0xff, cfg2); 

	/* Queue commands to perform SD transfer. */
	rtsx_push_cmd(sc, RTSX_WRITE_REG_CMD, RTSX_SD_TRANSFER,
		      0xff, tmode | RTSX_SD_TRANSFER_START);
	rtsx_push_cmd(sc, RTSX_CHECK_REG_CMD, RTSX_SD_TRANSFER,
		      RTSX_SD_TRANSFER_END, RTSX_SD_TRANSFER_END);

	/* Run the command queue and don't wait for completion. */
	rtsx_send_cmd_nowait(sc, cmd);

	sc->rtsx_intr_status = 0;

	if (!read)
		memcpy(sc->rtsx_data_dmamem, cmd->data->data, cmd->data->len);

	/* Sync data DMA buffer. */
	bus_dmamap_sync(sc->rtsx_data_dma_tag, sc->rtsx_data_dmamap, BUS_DMASYNC_PREREAD);
	bus_dmamap_sync(sc->rtsx_data_dma_tag, sc->rtsx_data_dmamap, BUS_DMASYNC_PREWRITE);

	/* Tell the chip where the data buffer is and run the transfer. */
	WRITE4(sc, RTSX_HDBAR, sc->rtsx_data_buffer);
	WRITE4(sc, RTSX_HDBCTLR, RTSX_TRIG_DMA | (read ? RTSX_DMA_READ : 0) |
	       (cmd->data->len & 0x00ffffff));

	if ((error = rtsx_wait_intr(sc, RTSX_TRANS_OK_INT, hz * sc->rtsx_timeout))) {
		cmd->error = error;
		return (error);
	}

	/* Sync data DMA buffer. */
	bus_dmamap_sync(sc->rtsx_data_dma_tag, sc->rtsx_data_dmamap, BUS_DMASYNC_POSTREAD);
	bus_dmamap_sync(sc->rtsx_data_dma_tag, sc->rtsx_data_dmamap, BUS_DMASYNC_POSTWRITE);

	if (read)
		memcpy(cmd->data->data, sc->rtsx_data_dmamem, cmd->data->len);
	else
		/* Send CMD12 after AUTO_WRITE3 (see mmcsd_rw() in mmcsd.c). */
		error = rtsx_send_req_get_resp(sc, sc->rtsx_req->stop);

	return (error);
}

static int
rtsx_read_ivar(device_t bus, device_t child, int which, uintptr_t *result)
{
	struct rtsx_softc *sc;

	sc = device_get_softc(bus);
	switch (which) {
	case MMCBR_IVAR_BUS_MODE:		/* ivar  0 - 1 = opendrain, 2 = pushpull */
		*result = sc->rtsx_host.ios.bus_mode;
		break;
	case MMCBR_IVAR_BUS_WIDTH:		/* ivar  1 - 0 = 1b   2 = 4b, 3 = 8b */
		*result = sc->rtsx_host.ios.bus_width;
		break;
	case MMCBR_IVAR_CHIP_SELECT:		/* ivar  2 - O = dontcare, 1 = cs_high, 2 = cs_low */
		*result = sc->rtsx_host.ios.chip_select;
		break;
	case MMCBR_IVAR_CLOCK:			/* ivar  3 - clock in Hz */
		*result = sc->rtsx_host.ios.clock;
		break;
	case MMCBR_IVAR_F_MIN:			/* ivar  4 */
		*result = sc->rtsx_host.f_min;
		break;
	case MMCBR_IVAR_F_MAX:			/* ivar  5 */
		*result = sc->rtsx_host.f_max;
		break;
	case MMCBR_IVAR_HOST_OCR: 		/* ivar  6 - host operation conditions register */
		*result = sc->rtsx_host.host_ocr;
		break;
	case MMCBR_IVAR_MODE:			/* ivar  7 - 0 = mode_mmc, 1 = mode_sd */
		*result = sc->rtsx_host.mode;
		break;
	case MMCBR_IVAR_OCR:			/* ivar  8 - operation conditions register */
		*result = sc->rtsx_host.ocr;
		break;
	case MMCBR_IVAR_POWER_MODE:		/* ivar  9 - 0 = off, 1 = up, 2 = on */
		*result = sc->rtsx_host.ios.power_mode;
		break;
	case MMCBR_IVAR_VDD:			/* ivar 11 - voltage power pin */
		*result = sc->rtsx_host.ios.vdd;
		break;
	case MMCBR_IVAR_VCCQ:			/* ivar 12 - signaling: 0 = 1.20V, 1 = 1.80V, 2 = 3.30V */
		*result = sc->rtsx_host.ios.vccq;
		break;
	case MMCBR_IVAR_CAPS:			/* ivar 13 */
		*result = sc->rtsx_host.caps;
		break;
	case MMCBR_IVAR_TIMING:			/* ivar 14 - 0 = normal, 1 = timing_hs, ... */
		*result = sc->rtsx_host.ios.timing;
		break;
	case MMCBR_IVAR_MAX_DATA:		/* ivar 15 */
		*result = MAXPHYS / MMC_SECTOR_SIZE;
		break;
	case MMCBR_IVAR_RETUNE_REQ:		/* ivar 10 */
	case MMCBR_IVAR_MAX_BUSY_TIMEOUT:	/* ivar 16 */
	default:
		return (EINVAL);
	}

	if (bootverbose)
		device_printf(bus, "Read ivar #%d, value %#x / #%d\n",
			      which, *(int *)result, *(int *)result);

	return (0);
}

static int
rtsx_write_ivar(device_t bus, device_t child, int which, uintptr_t value)
{
	struct rtsx_softc *sc;

	if (bootverbose)
		device_printf(bus, "Write ivar #%d, value %#x / #%d\n",
			      which, (int)value, (int)value);

	sc = device_get_softc(bus);
	switch (which) {
	case MMCBR_IVAR_BUS_MODE:		/* ivar  0 - 1 = opendrain, 2 = pushpull */
		sc->rtsx_host.ios.bus_mode = value;
		break;
	case MMCBR_IVAR_BUS_WIDTH:		/* ivar  1 - 0 = 1b   2 = 4b, 3 = 8b */
		sc->rtsx_host.ios.bus_width = value;
		sc->rtsx_ios_bus_width = -1;	/* To be updated on next rtsx_mmcbr_update_ios(). */
		break;
	case MMCBR_IVAR_CHIP_SELECT:		/* ivar  2 - O = dontcare, 1 = cs_high, 2 = cs_low */
		sc->rtsx_host.ios.chip_select = value;
		break;
	case MMCBR_IVAR_CLOCK:			/* ivar  3 - clock in Hz */
		sc->rtsx_host.ios.clock = value;
		sc->rtsx_ios_clock = -1;	/* To be updated on next rtsx_mmcbr_update_ios(). */
		break;
	case MMCBR_IVAR_MODE:			/* ivar  7 - 0 = mode_mmc, 1 = mode_sd */ 
		sc->rtsx_host.mode = value;
		break;
	case MMCBR_IVAR_OCR:			/* ivar  8 - operation conditions register */
		sc->rtsx_host.ocr = value;
		break;
	case MMCBR_IVAR_POWER_MODE:		/* ivar  9 - 0 = off, 1 = up, 2 = on */
		sc->rtsx_host.ios.power_mode = value;
		sc->rtsx_ios_power_mode = -1;	/* To be updated on next rtsx_mmcbr_update_ios(). */
		break;
	case MMCBR_IVAR_VDD:			/* ivar 11 - voltage power pin */ 
		sc->rtsx_host.ios.vdd = value;
		break;
	case MMCBR_IVAR_VCCQ:			/* ivar 12 - signaling: 0 = 1.20V, 1 = 1.80V, 2 = 3.30V */
		sc->rtsx_host.ios.vccq = value; /* rtsx_mmcbr_switch_vccq() will be called by mmc.c. */
		break;
	case MMCBR_IVAR_TIMING:			/* ivar 14 - 0 = normal, 1 = timing_hs, ... */
		sc->rtsx_host.ios.timing = value;
		sc->rtsx_ios_timing = -1;	/* To be updated on next rtsx_mmcbr_update_ios(). */
		break;
	/* These are read-only. */
	case MMCBR_IVAR_F_MIN:			/* ivar  4 */
	case MMCBR_IVAR_F_MAX:			/* ivar  5 */
	case MMCBR_IVAR_HOST_OCR: 		/* ivar  6 - host operation conditions register */
	case MMCBR_IVAR_RETUNE_REQ:		/* ivar 10 */
	case MMCBR_IVAR_CAPS:			/* ivar 13 */
	case MMCBR_IVAR_MAX_DATA:		/* ivar 15 */
	case MMCBR_IVAR_MAX_BUSY_TIMEOUT:	/* ivar 16 */
	default:
		return (EINVAL);
	}

	return (0);
}

static int
rtsx_mmcbr_update_ios(device_t bus, device_t child)
{
	struct rtsx_softc *sc;
	struct mmc_ios *ios;
	int error;

	sc = device_get_softc(bus);
	ios = &sc->rtsx_host.ios;

	if (bootverbose)
		device_printf(bus, "rtsx_mmcbr_update_ios()\n");

	/* if MMCBR_IVAR_BUS_WIDTH updated. */
	if (sc->rtsx_ios_bus_width < 0) {
		uint32_t bus_width;

		sc->rtsx_ios_bus_width = ios->bus_width;
		switch (ios->bus_width) {
		case bus_width_1:
			bus_width = RTSX_BUS_WIDTH_1;
			break;
		case bus_width_4:
			bus_width = RTSX_BUS_WIDTH_4;
			break;
		case bus_width_8:
			bus_width = RTSX_BUS_WIDTH_8;
			break;
		default:
			return (MMC_ERR_INVALID);
		}
		if ((error = rtsx_write(sc, RTSX_SD_CFG1, RTSX_BUS_WIDTH_MASK, bus_width)))
			return (error);

		if (bootverbose) {
			char *busw[] = {
					"1 bit",
					"4 bits",
					"8 bits"
			};
				device_printf(sc->rtsx_dev, "Setting bus width to %s\n", busw[bus_width]);
		}
	}

	/* if MMCBR_IVAR_CLOCK updated. */
	if (sc->rtsx_ios_clock < 0) {
		sc->rtsx_ios_clock = ios->clock;
		if ((error = rtsx_set_sd_clock(sc, ios->clock)))
			return (error);
	}

	/* if MMCBR_IVAR_POWER_MODE updated. */
	if (sc->rtsx_ios_power_mode < 0) {
		sc->rtsx_ios_power_mode = ios->power_mode;
		switch (ios->power_mode) {
		case power_off:
			if ((error = rtsx_bus_power_off(sc)))
				return (error);
			break;
		case power_up:
			if ((error = rtsx_bus_power_on(sc)))
				return (error);
			break;
		case power_on:
			if ((error = rtsx_bus_power_on(sc)))
				return (error);
			break;
		}
	}

	/* if MMCBR_IVAR_TIMING updated. */
	if (sc->rtsx_ios_timing < 0) {
		sc->rtsx_ios_timing = ios->timing;
		if ((error = rtsx_set_sd_timing(sc, ios->timing)))
			return (error);
	}

	return (0);
}

/*
 * Set output stage logic power voltage.
 */
static int
rtsx_mmcbr_switch_vccq(device_t bus, device_t child __unused)
{
	struct rtsx_softc *sc;
	int vccq = 0;
	int error;

	sc = device_get_softc(bus);

	switch (sc->rtsx_host.ios.vccq) {
	case vccq_120:
		vccq = 120;
		break;
	case vccq_180:
		vccq = 180;
		break;
	case vccq_330:
		vccq = 330;
		break;
	};
	/* It seems it is always vccq_330. */
	if (vccq == 330) {
		if (sc->rtsx_flags & RTSX_F_5227) {
			/*!!!*/
			if ((error = rtsx_write_phy(sc, 0x08, 0x4FE4)))
				return (error);
			if ((error = rtsx_rts5227_fill_driving(sc)))
				return (error);
		} else if (sc->rtsx_flags & RTSX_F_5229) {
			RTSX_BITOP(sc, RTSX_SD30_CMD_DRIVE_SEL, RTSX_SD30_DRIVE_SEL_MASK, sc->rtsx_sd30_drive_sel_3v3);
			if ((error = rtsx_write_phy(sc, 0x08, 0x4FE4)))
				return (error);
		} else if (sc->rtsx_flags & RTSX_F_522A) {
			if ((error = rtsx_write_phy(sc, 0x08, 0x57E4)))
				return (error);
			if ((error = rtsx_rts5227_fill_driving(sc)))
				return (error);
		} else if (sc->rtsx_flags & RTSX_F_525A) {
			RTSX_BITOP(sc, RTSX_LDO_CONFIG2, RTSX_LDO_D3318_MASK, RTSX_LDO_D3318_33V);
			RTSX_BITOP(sc, RTSX_SD_PAD_CTL, RTSX_SD_IO_USING_1V8, 0);
			if ((error = rtsx_rts5249_fill_driving(sc)))
				return (error);
		} else if (sc->rtsx_flags & RTSX_F_5249) {
			uint16_t val;

			if ((error = rtsx_read_phy(sc, RTSX_PHY_TUNE, &val)))
				return (error);
			if ((error = rtsx_write_phy(sc, RTSX_PHY_TUNE,
						    (val & RTSX_PHY_TUNE_VOLTAGE_MASK) | RTSX_PHY_TUNE_VOLTAGE_3V3)))
				return (error);
			if ((error = rtsx_rts5249_fill_driving(sc)))
				return (error);
		} else if (sc->rtsx_flags & RTSX_F_8402) {
			RTSX_BITOP(sc, RTSX_SD30_CMD_DRIVE_SEL, RTSX_SD30_DRIVE_SEL_MASK, sc->rtsx_sd30_drive_sel_3v3);
			RTSX_BITOP(sc, RTSX_LDO_CTL,
				   (RTSX_BPP_ASIC_MASK << RTSX_BPP_SHIFT_8402) | RTSX_BPP_PAD_MASK,
				   (RTSX_BPP_ASIC_3V3 << RTSX_BPP_SHIFT_8402) | RTSX_BPP_PAD_3V3);
		} else if (sc->rtsx_flags & (RTSX_F_8411 | RTSX_F_8411B)) {
			RTSX_BITOP(sc, RTSX_SD30_CMD_DRIVE_SEL, RTSX_SD30_DRIVE_SEL_MASK, sc->rtsx_sd30_drive_sel_3v3);
			RTSX_BITOP(sc, RTSX_LDO_CTL,
				   (RTSX_BPP_ASIC_MASK << RTSX_BPP_SHIFT_8411) | RTSX_BPP_PAD_MASK,
				   (RTSX_BPP_ASIC_3V3 << RTSX_BPP_SHIFT_8411) | RTSX_BPP_PAD_3V3);
		}
		DELAY(300);
	}

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_mmcbr_switch_vccq(%d)\n", vccq);

	return (0);
}

static int
rtsx_mmcbr_tune(device_t bus, device_t child __unused, bool hs400)
{
	struct rtsx_softc *sc;

	sc = device_get_softc(bus);

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_mmcbr_tune() - hs400 = %s\n",
			      (hs400) ? "true" : "false");

	return (0);
}

static int
rtsx_mmcbr_retune(device_t bus, device_t child __unused, bool reset __unused)
{
	struct rtsx_softc *sc;

	sc = device_get_softc(bus);

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_mmcbr_retune()\n");

	return (0);
}

static int
rtsx_mmcbr_request(device_t bus, device_t child __unused, struct mmc_request *req)
{
	struct rtsx_softc *sc;
	struct mmc_command *cmd;
	int error = 0;

	sc = device_get_softc(bus);

	RTSX_LOCK(sc);
	if (sc->rtsx_req != NULL) {
		RTSX_UNLOCK(sc);
                return (MMC_ERR_MAX);
        }
	sc->rtsx_req = req;
	sc->rtsx_intr_status = 0;
	cmd = req->cmd;
	cmd->error = MMC_ERR_NONE;

	if (bootverbose)
		device_printf(sc->rtsx_dev, "rtsx_mmcbr_request(CMD%u arg %#x flags %#x dlen %u dflags %#x)\n",
			      cmd->opcode, cmd->arg, cmd->flags,
			      cmd->data != NULL ? (unsigned int)cmd->data->len : 0,
			      cmd->data != NULL ? cmd->data->flags : 0);

	/* Check if card present. */
	if (!ISSET(sc->rtsx_flags, RTSX_F_CARD_PRESENT)) {
		cmd->error = MMC_ERR_INVALID;
		error = MMC_ERR_INVALID;
		goto done;
	}

	/* Refuse SDIO probe if the chip doesn't support SDIO. */
	if (cmd->opcode == IO_SEND_OP_COND &&
	    !ISSET(sc->rtsx_flags, RTSX_F_SDIO_SUPPORT)) {
		cmd->error = MMC_ERR_INVALID;
		error = MMC_ERR_INVALID;
		goto done;
	}

	if (cmd->data == NULL) {
		/*!!!*/
		DELAY(200);
		error = rtsx_send_req_get_resp(sc, cmd);
	} else if (cmd->data->len <= 512) {
		if ((error = rtsx_xfer_short(sc, cmd))) {
			uint8_t stat1;
			if (rtsx_read(sc, RTSX_SD_STAT1, &stat1) == 0 &&
			    (stat1 & RTSX_SD_CRC_ERR)) {
				device_printf(sc->rtsx_dev, "CRC error\n");
				cmd->error = MMC_ERR_BADCRC;
			}
		}
	} else {
		if ((error = rtsx_xfer(sc, cmd))) {
			uint8_t stat1;
			if (rtsx_read(sc, RTSX_SD_STAT1, &stat1) == 0 &&
			    (stat1 & RTSX_SD_CRC_ERR)) {
				device_printf(sc->rtsx_dev, "CRC error\n");
				cmd->error = MMC_ERR_BADCRC;
			}
		}
	}

 done:
	rtsx_req_done(sc);
	RTSX_UNLOCK(sc);
	return (error);
}

static int
rtsx_mmcbr_get_ro(device_t bus, device_t child __unused)
{
	struct rtsx_softc *sc;

	sc = device_get_softc(bus);

#ifndef RTSX_INVERSION
	return (sc->rtsx_read_only);
#else
	return !(sc->rtsx_read_only);
#endif	
}

static int
rtsx_mmcbr_acquire_host(device_t bus, device_t child __unused)
{
	struct rtsx_softc *sc;

	if (bootverbose)
		device_printf(bus, "rtsx_mmcbr_acquire_host()\n");

	sc = device_get_softc(bus);
	RTSX_LOCK(sc);
	while (sc->rtsx_bus_busy)
                msleep(sc, &sc->rtsx_mtx, 0, "rtsxah", 0);
	sc->rtsx_bus_busy++;
	RTSX_UNLOCK(sc);

	return (0);
}
	       
static int
rtsx_mmcbr_release_host(device_t bus, device_t child __unused)
{
	struct rtsx_softc *sc;

	if (bootverbose)
		device_printf(bus, "rtsx_mmcbr_release_host()\n");

	sc = device_get_softc(bus);
	RTSX_LOCK(sc);
	sc->rtsx_bus_busy--;
	RTSX_UNLOCK(sc);
	wakeup(sc);

	return (0);
}

/*
 *
 * PCI Support Functions
 *
 */

/*
 * Compare the device ID (chip) of this device against the IDs that this driver
 * supports. If there is a match, set the description and return success.
 */
static int
rtsx_probe(device_t dev)
{
	uint16_t vendor;
	uint16_t device;
	struct rtsx_softc *sc;
	int i, result;

	vendor = pci_get_vendor(dev);
	device = pci_get_device(dev);

	result = ENXIO;
	for (i = 0; rtsx_devices[i].vendor != 0; i++) {
		if (rtsx_devices[i].vendor == vendor &&
		    rtsx_devices[i].device == device) {
		        device_set_desc(dev, rtsx_devices[i].desc);
			sc = device_get_softc(dev);
			sc->rtsx_flags = rtsx_devices[i].flags;
			result = BUS_PROBE_DEFAULT;
			break;
		}
	}

	return (result);
}

/*
 * Attach function is only called if the probe is successful.
 */
static int
rtsx_attach(device_t dev)
{
	struct rtsx_softc 	*sc = device_get_softc(dev);
	struct sysctl_ctx_list	*ctx;
	struct sysctl_oid_list	*tree;
	int			msi_count = 1;
	uint32_t		sdio_cfg;
	int			error;

	if (bootverbose)
		device_printf(dev, "Attach - Vendor ID: 0x%x - Device ID: 0x%x\n",
			      pci_get_vendor(dev), pci_get_device(dev));

	sc->rtsx_dev = dev;
	RTSX_LOCK_INIT(sc);

	/* timeout parameter for rtsx_wait_intr(). */
	sc->rtsx_timeout = 2;
	ctx = device_get_sysctl_ctx(dev);
	tree = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));
	SYSCTL_ADD_INT(ctx, tree, OID_AUTO, "req_timeout", CTLFLAG_RW,
		       &sc->rtsx_timeout, 0, "Request timeout in seconds");

	/* Allocate IRQ. */
	sc->rtsx_irq_res_id = 0;
	if (pci_alloc_msi(dev, &msi_count) == 0)
		sc->rtsx_irq_res_id = 1;
	sc->rtsx_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->rtsx_irq_res_id,
						  RF_ACTIVE | (sc->rtsx_irq_res_id != 0 ? 0 : RF_SHAREABLE));
	if (sc->rtsx_irq_res == NULL) {
		device_printf(dev, "Can't allocate IRQ resources for %d\n", sc->rtsx_irq_res_id);
		pci_release_msi(dev);
		return (ENXIO);
	}

	/* Allocate memory resource. */
	if (sc->rtsx_flags & RTSX_F_525A)
		sc->rtsx_res_id = PCIR_BAR(1);
	else
		sc->rtsx_res_id = PCIR_BAR(0);
	sc->rtsx_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->rtsx_res_id, RF_ACTIVE);
	if (sc->rtsx_res == NULL) {
		device_printf(dev, "Can't allocate memory resource for %d\n", sc->rtsx_res_id);
		goto destroy_rtsx_irq_res;
	}

	if (bootverbose)
		device_printf(dev, "rtsx_irq_res_id: %d - rtsx_res_id: %d\n",
			      sc->rtsx_irq_res_id, sc->rtsx_res_id);

	sc->rtsx_btag = rman_get_bustag(sc->rtsx_res);
	sc->rtsx_bhandle = rman_get_bushandle(sc->rtsx_res);

	/* Activate the interrupt. */
	error = bus_setup_intr(dev, sc->rtsx_irq_res, INTR_TYPE_MISC | INTR_MPSAFE,
			       NULL, rtsx_intr, sc, &sc->rtsx_irq_cookie);
	if (error) {
		device_printf(dev, "Can't set up irq [0x%x]!\n", error);
		goto destroy_rtsx_res;
	}
	pci_enable_busmaster(dev);

	if (rtsx_read_cfg(sc, 0, RTSX_SDIOCFG_REG, &sdio_cfg) == 0) {
		if ((sdio_cfg & RTSX_SDIOCFG_SDIO_ONLY) ||
		    (sdio_cfg & RTSX_SDIOCFG_HAVE_SDIO))
			sc->rtsx_flags |= RTSX_F_SDIO_SUPPORT;
	}

	/* Allocate two DMA buffers: a command buffer and a data buffer. */
	error = rtsx_dma_alloc(sc);
	if (error) {
		goto destroy_rtsx_irq;
	}

	/* from dwmmc.c. */
	TASK_INIT(&sc->rtsx_card_task, 0, rtsx_card_task, sc);
	/* really giant?. */
	TIMEOUT_TASK_INIT(taskqueue_swi_giant, &sc->rtsx_card_delayed_task, 0,
			  rtsx_card_task, sc);

	/* Initialize device. */
	if (rtsx_init(sc)) {
		device_printf(dev, "Error during rtsx_init()\n");
		goto destroy_rtsx_irq;
	}

	/* 
	 * Schedule a card detection as we won't get an interrupt
	 * if the card is inserted when we attach
	 */
	DELAY(500);
	if (rtsx_is_card_present(sc))
		device_printf(sc->rtsx_dev, "Card present\n");
	else
		device_printf(sc->rtsx_dev, "Card absent\n");
	rtsx_card_task(sc, 0);

	if (bootverbose)
		device_printf(dev, "Device attached\n");
	
	return (0);

 destroy_rtsx_irq:
	bus_teardown_intr(dev, sc->rtsx_irq_res, sc->rtsx_irq_cookie);	
 destroy_rtsx_res:
	bus_release_resource(dev, SYS_RES_MEMORY, sc->rtsx_res_id,
			     sc->rtsx_res);
 destroy_rtsx_irq_res:
	bus_release_resource(dev, SYS_RES_IRQ, sc->rtsx_irq_res_id,
			     sc->rtsx_irq_res);
	pci_release_msi(dev);
	RTSX_LOCK_DESTROY(sc);
	return (ENXIO);
}

	
static int
rtsx_detach(device_t dev)
{
	struct rtsx_softc *sc = device_get_softc(dev);
	int error;

	if (bootverbose)
		device_printf(dev, "Detach - Vendor ID: 0x%x - Device ID: 0x%x\n",
			      pci_get_vendor(dev), pci_get_device(dev));

	/* Stop device. */
	error = device_delete_children(sc->rtsx_dev);
	sc->rtsx_mmc_dev = NULL;
	if (error)
		return (error);

	taskqueue_drain(taskqueue_swi_giant, &sc->rtsx_card_task);
	taskqueue_drain_timeout(taskqueue_swi_giant, &sc->rtsx_card_delayed_task);

	/* Teardown the state in our softc created in our attach routine. */
	rtsx_dma_free(sc);
        if (sc->rtsx_res != NULL)
                bus_release_resource(dev, SYS_RES_MEMORY, sc->rtsx_res_id,
				     sc->rtsx_res);	
	if (sc->rtsx_irq_cookie != NULL)
                bus_teardown_intr(dev, sc->rtsx_irq_res, sc->rtsx_irq_cookie);	
        if (sc->rtsx_irq_res != NULL) {
	        bus_release_resource(dev, SYS_RES_IRQ, sc->rtsx_irq_res_id,
				     sc->rtsx_irq_res);
		pci_release_msi(dev);
	}
	RTSX_LOCK_DESTROY(sc);

	return (0);
}

static int
rtsx_shutdown(device_t dev)
{

	if (bootverbose)
		device_printf(dev, "Shutdown\n");

	return (0);
}

/*
 * Device suspend routine.
 */
static int
rtsx_suspend(device_t dev)
{
	struct rtsx_softc *sc = device_get_softc(dev);

//	if (bootverbose)
		device_printf(dev, "Suspend\n");

	if (sc->rtsx_req != NULL) {
		device_printf(dev, "Request in progress: CMD%u, rtsr_intr_status=0x%08x\n",
			      sc->rtsx_req->cmd->opcode, sc->rtsx_intr_status);
	}

	bus_generic_suspend(dev);

	return (0);
}

/*
 * Device resume routine.
 */
static int
rtsx_resume(device_t dev)
{
//	if (bootverbose)
		device_printf(dev, "Resume\n");

	bus_generic_resume(dev);

	return (0);
}

static device_method_t rtsx_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		rtsx_probe),
	DEVMETHOD(device_attach,	rtsx_attach),
	DEVMETHOD(device_detach,	rtsx_detach),
	DEVMETHOD(device_shutdown,	rtsx_shutdown),
	DEVMETHOD(device_suspend,	rtsx_suspend),
	DEVMETHOD(device_resume,	rtsx_resume),

	/* Bus interface */
	DEVMETHOD(bus_read_ivar,	rtsx_read_ivar),
	DEVMETHOD(bus_write_ivar,	rtsx_write_ivar),

	/* MMC bridge interface */
     	DEVMETHOD(mmcbr_update_ios,	rtsx_mmcbr_update_ios),
	DEVMETHOD(mmcbr_switch_vccq,	rtsx_mmcbr_switch_vccq),
	DEVMETHOD(mmcbr_tune,		rtsx_mmcbr_tune),
	DEVMETHOD(mmcbr_retune,		rtsx_mmcbr_retune),
	DEVMETHOD(mmcbr_request,	rtsx_mmcbr_request),
	DEVMETHOD(mmcbr_get_ro,		rtsx_mmcbr_get_ro),
	DEVMETHOD(mmcbr_acquire_host,	rtsx_mmcbr_acquire_host),
	DEVMETHOD(mmcbr_release_host,	rtsx_mmcbr_release_host),

	DEVMETHOD_END
};

static devclass_t rtsx_devclass;

DEFINE_CLASS_0(rtsx, rtsx_driver, rtsx_methods, sizeof(struct rtsx_softc));
DRIVER_MODULE(rtsx, pci, rtsx_driver, rtsx_devclass, NULL, NULL);
MMC_DECLARE_BRIDGE(rtsx);
