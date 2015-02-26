/*
 * Driver for Broadcom BCM2835 SPI Controllers using DMA-FRAGMENTS
 *
 * Copyright (C) 2012 Chris Boot
 * Copyright (C) 2013 Stephen Warren
 * Copyright (C) 2014 Martin Sperl
 *
 * This driver is inspired by:
 * spi-bcm2835.c, Copyright (C) 2012 Chris Boot, 2013 Stephen Warren
 * spi-ath79.c,   Copyright (C) 2009-2011 Gabor Juhos <juhosg@openwrt.org>
 * spi-atmel.c,   Copyright (C) 2006 Atmel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */
#include "spi-bcm2835dma.h"
#include <linux/spi/spi-dmafragment.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>

struct dma_pool *bcm2835_dma_pool=NULL;

/*************************************************************************
 * the function creating dma_fragments - mostly used by dma_fragment_cache
 ************************************************************************/

/*------------------------------------------------------------------------
 * Helpers - to reduce code size
 *----------------------------------------------------------------------*/

#define MASTER_BS_VARS(device)						\
	struct spi_master *master = (struct spi_master *)device;	\
	struct bcm2835dma_spi *bs = spi_master_get_devdata(master)

/**
 * START_CREATE_FRAGMENT_ALLOCATE - macro that contains repetitive code
 *   and variable definitions used by most allocate functions
 * note: no semicolon for the last define, as we run into compiler
 *   warnings otherwise when it sees ";;" and then some more variable
 *   definitions and then complains about "mixed declaration and code"
 * @struct_name: name of structure to allocate as frag
 */
#define START_CREATE_FRAGMENT_ALLOCATE(struct_name,link_func)		\
	struct dma_link *link;						\
	struct struct_name *frag =					\
		(struct struct_name *) dma_fragment_alloc(		\
			sizeof(struct struct_name),			\
			link_func,					\
			gfp);						\
	if (!frag)							\
		return NULL;						\
	frag->dma_fragment.desc = #struct_name;

/**
 * END_CREATE_FRAGMENT_ALLOCATE - macro that contains repetitive code
 * used by all alloc functions on exit of function - including error
 * handling.
 */

#define END_CREATE_FRAGMENT_ALLOCATE()				\
	return &frag->dma_fragment;				\
error:								\
dma_fragment_release((struct dma_fragment *)frag);		\
return NULL;

#ifdef SPI_HAVE_OPTIMIZE
#define VARY_VALUE(xfer) 0 /*(xfer->vary)*/
#else
#define VARY_VALUE(xfer) 0
#endif

#define DO_VARY(xfer,bitmap,func)		\
	if (VARY_VALUE(xfer)|bitmap) {		\
		/* toto */			\
	} else {				\
		err = func();			\
	}

/*------------------------------------------------------------------------
 * structures/function for setting up spi
 *----------------------------------------------------------------------*/
struct dma_fragment_config_spi {
	struct dma_fragment dma_fragment;
	/* cs_set/cs_clear register for dma and the mask*/
	dma_addr_t *cs_gpio_pin_reg;
	u32        *cs_gpio_pin_mask;
	/* reset SPI flags - these depend on polarity */
	u32        *spi_clear_fifo;
	u32        *spi_config;
	/* length */
	u32        *length_ptr;
        u32        length; /* length that is not varied */
	/* speed */
	u32        *clk_ptr;
	/* and the link to start tx_dma */
	struct dma_link *tx_link;
};


int bcm2835dma_spi_link_fragment_config_spi_calc_speed(void)
{
	return -EPERM;
}

int bcm2835dma_spi_fragment_config_spi_link(
	struct dma_fragment *compound,
	struct dma_fragment *tolink)
{
	int err = 0;
	struct dma_fragment_config_spi *frag   = (typeof(frag))   tolink;
	struct spi_merged_dma_fragment *merged = (typeof(merged)) compound;
	struct spi_message *msg                = merged->message;
	//struct spi_message *xfer               = merged->transfer;
	struct spi_device *spi                 = msg->spi;
	struct bcm2835dma_spi_device_data *bs  = spi_get_ctldata(spi);

	/* set up the transfer parts */
	/*
	merged->txdma_link_to_here = frag->tx_link;
	merged->total_length       = frag->length_ptr;
	*/

	/* set the things from the spi device */
	*frag->cs_gpio_pin_reg  = bs->cs_select_gpio_reg;
	*frag->cs_gpio_pin_mask = bs->cs_bitfield;
	*frag->spi_clear_fifo   = bs->spi_reset_fifo;
	*frag->spi_config       = bs->spi_config;

	/* and either schedule as pre-dma transform or now */
	DO_VARY(xfer,SPI_OPTIMIZE_VARY_SPEED_HZ,
		bcm2835dma_spi_link_fragment_config_spi_calc_speed);

	/* and return OK */
	return err;
}

struct dma_fragment *bcm2835dma_spi_fragment_config_spi_create(
	struct device *device, gfp_t gfp)
{
	MASTER_BS_VARS(device);
	START_CREATE_FRAGMENT_ALLOCATE(
		dma_fragment_config_spi,
		bcm2835dma_spi_fragment_config_spi_link);
	/* start by poking the register */

	/* "POKE" the CS active - the register addresses are set during
	 * link time, when we know the spi device and hence the CS-GPIO
	 */
	if (!(link = bcm2835_addDMAPoke(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				0,0,
				gfp)))
		goto error;
	frag->cs_gpio_pin_reg   = bcm2835_getDMAPokeReg(link);
	frag->cs_gpio_pin_mask  = bcm2835_getDMAPokeVal(link);

	/* reset FIFO - the value needs to get set during link time
	 * as it requires knowledge of spi polarity not to disrupt things
	 */
	if (!(link = bcm2835_addDMAPoke(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				(BCM2835_SPI_BASE_BUS + BCM2835_SPI_CS),
				0,
				gfp)))
		goto error;
	frag->spi_clear_fifo = bcm2835_getDMAPokeVal(link);

	/* set clock and length - both are set during link time (or vary) */
	if (!(link = bcm2835_addDMAPoke2(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				(BCM2835_SPI_BASE_BUS + BCM2835_SPI_CLK),
				0,0,
				gfp)))
		goto error;
	frag->clk_ptr    = bcm2835_getDMAPokeVal(link);
	frag->length_ptr = &frag->clk_ptr[1];

	/* and start spi */
	if (!(link = bcm2835_addDMAPoke(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				(BCM2835_SPI_BASE_BUS + BCM2835_SPI_CS),
				0,
				gfp)))
		goto error;
	frag->spi_config = bcm2835_getDMAPokeVal(link);

	/* and start TX DMA */
	if (!(link = bcm2835_addDMAStart(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				bs->dma_tx.chan,
				gfp)))
		goto error;
	frag->tx_link = link;

	/* add the link time function */
	dma_fragment_dump(&frag->dma_fragment,
			device, 0, bcm2835_dma_link_dump);

	printk(KERN_INFO "YYY\n");
	/* and finish */
	END_CREATE_FRAGMENT_ALLOCATE();
}

/*------------------------------------------------------------------------
 * allocator for transfers
 *----------------------------------------------------------------------*/

struct dma_fragment_transfer {
	struct dma_fragment           dma_fragment;
	struct dma_link               *rx_link;
	struct dma_link               *tx_link;
};

struct dma_fragment *bcm2835dma_spi_fragment_transfer_create(
	struct device *device, gfp_t gfp)
{
	START_CREATE_FRAGMENT_ALLOCATE(dma_fragment_transfer,
		NULL);

	/* the rx transfer - keep linked */
	if (!(link = bcm2835_addDMATransfer(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				0, 0, 0,
				gfp)))
		goto error;
	frag->rx_link = link;
	/* the tx transfer - but keep it unlinked */
	if (!(link = bcm2835_addDMATransfer(
				bcm2835_dma_pool,
				&frag->dma_fragment, 0,
				0, 0, 0,
				gfp)))
		goto error;
	frag->tx_link = link;

	/* and finish */
	END_CREATE_FRAGMENT_ALLOCATE();
}

/*------------------------------------------------------------------------
 * allocator for cs_deselect and delay
 *----------------------------------------------------------------------*/
struct dma_fragment_cs_deselect_delay {
	struct dma_fragment           dma_fragment;
	struct dma_link     *delay_us_link;

	dma_addr_t *cs_gpio_pin_clear;
	u32        *cs_gpio_pin_mask;

	struct dma_link     *delay_half_cycle_link;
};

struct dma_fragment *bcm2835dma_spi_fragment_cs_deselect_delay_create(
	struct device *device, gfp_t gfp)
{
	START_CREATE_FRAGMENT_ALLOCATE(dma_fragment_cs_deselect_delay,
		NULL);
	/* delay half a clock cycle or whatever is requested */
	if (!(link = bcm2835_addDMADelay(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				0,
				gfp)))
		goto error;
	frag->delay_us_link = link;

	/* bring CS up */
	if (!(link = bcm2835_addDMAPoke(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				0, 0,
				gfp)))
		goto error;
	frag->cs_gpio_pin_clear = bcm2835_getDMAPokeReg(link);
	frag->cs_gpio_pin_mask  = bcm2835_getDMAPokeVal(link);

	/* delay half a clock cycle */
	if (!(link = bcm2835_addDMADelay(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				0,
				gfp)))
		goto error;
	frag->delay_half_cycle_link = link;

	/* and finish */
	END_CREATE_FRAGMENT_ALLOCATE();
}

/*------------------------------------------------------------------------
 * allocator for delay
 *----------------------------------------------------------------------*/
struct dma_fragment_delay {
	struct dma_fragment           dma_fragment;
	struct dma_link     *delay_us_link;
};

struct dma_fragment *bcm2835dma_spi_fragment_delay_create(
	struct device *device, gfp_t gfp)
{
	START_CREATE_FRAGMENT_ALLOCATE(dma_fragment_delay,
		NULL);

	/* delay whatever is requested */
	if (!(link = bcm2835_addDMADelay(
				bcm2835_dma_pool,
				&frag->dma_fragment, 1,
				0,
				gfp)))
		goto error;
	frag->delay_us_link = link;

	/* and finish */
	END_CREATE_FRAGMENT_ALLOCATE();
}

/*************************************************************************
 * the release and initialization of dma_fragment caches
 * and function pointers
 ************************************************************************/
void bcm2835dma_release_dmafragment_components(
	struct spi_master *master)
{
	struct bcm2835dma_spi *bs = spi_master_get_devdata(master);
	printk(KERN_INFO "RELEASE_DMAFRAGMENT_COMPONENTS\n");
	dma_fragment_cache_free(bs->fragment_merged);
	bs->fragment_merged = NULL;
	dma_fragment_cache_free(bs->fragment_setup_spi);
	bs->fragment_setup_spi = NULL;
	dma_fragment_cache_free(bs->fragment_transfer);
	bs->fragment_transfer = NULL;
	dma_fragment_cache_free(bs->fragment_cs_deselect_delay);
	bs->fragment_cs_deselect_delay = NULL;
	dma_fragment_cache_free(bs->fragment_delay);
	bs->fragment_delay = NULL;
}

static struct dma_fragment *bcm2835dma_merged_dma_fragments_alloc(
	struct device *device, gfp_t gfp)
{
	return &spi_merged_dma_fragment_alloc(
		sizeof(struct bcm2835dma_spi_merged_dma_fragment),
		gfp)->dma_fragment;
}

/* register all the stuff needed to control dmafragments
   note that the below requires that master has already been registered
   otherwise you get an oops...
 */

#define PREPARE 1 /* prepare the caches with a typical 10 messages */
int bcm2835dma_register_dmafragment_components(
	struct spi_master *master)
{
	struct bcm2835dma_spi *bs = spi_master_get_devdata(master);
	int err=-1;

	printk(KERN_INFO "HERE\n");
	/* initialize DMA Fragment pools */
	bs->fragment_merged = dma_fragment_cache_alloc(
		&master->dev,
		"fragment_merged",
		&bcm2835dma_merged_dma_fragments_alloc,
		PREPARE*1
		);
	if (!bs->fragment_merged)
		goto error;

	bs->fragment_setup_spi = dma_fragment_cache_alloc(
		&master->dev,
		"config_spi",
		&bcm2835dma_spi_fragment_config_spi_create,
		PREPARE*2
		);
	if (!bs->fragment_setup_spi)
		goto error;

	bs->fragment_transfer = dma_fragment_cache_alloc(
		&master->dev,
		"transfer",
		&bcm2835dma_spi_fragment_transfer_create,
		PREPARE*3
		);
	if (!bs->fragment_transfer)
		goto error;

	bs->fragment_cs_deselect_delay = dma_fragment_cache_alloc(
		&master->dev,
		"fragment_cs_deselect",
		&bcm2835dma_spi_fragment_cs_deselect_delay_create,
		PREPARE*1
		);
	if (!bs->fragment_cs_deselect_delay)
		goto error;

	bs->fragment_delay = dma_fragment_cache_alloc(
		&master->dev,
		"fragment_delay",
		&bcm2835dma_spi_fragment_delay_create,
		PREPARE/2
		);
	if (!&bs->fragment_delay)
		goto error;

	return 0;
error:
	printk(KERN_INFO "WHERE\n");
	bcm2835dma_release_dmafragment_components(master);
	return err;
}
