/*
 * Simple synchronous userspace interface to SSC device
 *
 * Copyright (C) 2017 Develer Srl <pietro@develer.com>
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/interrupt.h>
#include <linux/semaphore.h>

#include <linux/atmel-ssc.h>
#include <linux/uaccess.h>

/* We'll try to allocate a device major number, store it here */
static int major;
#define N_SSC_MINORS			32	/* up to 256 */

static DECLARE_BITMAP(minors, N_SSC_MINORS);

struct sscdev_data {
	dev_t			devt;
	struct ssc_device	*ssc;
	struct list_head	device_entry;
	unsigned long		buf_size;
	unsigned long		period_len;
	bool			use_external_clk;

	unsigned		users;

	u8			*rx_buffer;

	size_t			tx_head;
	size_t			tx_tail;
	size_t			tx_queued;
	struct semaphore	tx_sem;

	size_t			rx_avail;
	size_t			rx_offset;
	s64			rx_ts;
};

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_list_lock);

/* cyclic DMA periods */
#define DMA_PERIODS	2

/*-------------------------------------------------------------------------*/

static void atmel_ssc_lock(struct ssc_device *ssc) __acquires(&ssc->lock)
{
	spin_lock_irqsave(&ssc->lock, ssc->flags);
}

static void atmel_ssc_unlock(struct ssc_device *ssc) __releases(&ssc->lock)
{
	spin_unlock_irqrestore(&ssc->lock, ssc->flags);
}

static int atmel_ssc_dma_slave_config(struct ssc_device *ssc,
				      struct dma_slave_config *slave_config)
{
	int width, err = 0;

	switch (ssc->word_size) {
	case 1:
		width = DMA_SLAVE_BUSWIDTH_1_BYTE;
		break;
	case 2:
		width = DMA_SLAVE_BUSWIDTH_2_BYTES;
		break;
	case 3:
	case 4:
		width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		break;
	default:
		dev_err(&ssc->pdev->dev, "unsupported sample format: %d\n",
			ssc->word_size);
		return -EINVAL;
	}

	slave_config->dst_addr_width = width;
	slave_config->src_addr_width = width;
	slave_config->dst_addr = (dma_addr_t)ssc->phybase + SSC_THR;
	slave_config->src_addr = (dma_addr_t)ssc->phybase + SSC_RHR;
	slave_config->dst_maxburst = 1;
	slave_config->src_maxburst = 1;
	slave_config->device_fc = false;

	slave_config->direction = DMA_MEM_TO_DEV;
	if (dmaengine_slave_config(ssc->dma.chan_tx, slave_config)) {
		dev_err(&ssc->pdev->dev,
			"failed to configure tx dma channel\n");
		err = -EINVAL;
	}

	slave_config->direction = DMA_DEV_TO_MEM;
	if (dmaengine_slave_config(ssc->dma.chan_rx, slave_config)) {
		dev_err(&ssc->pdev->dev,
			"failed to configure rx dma channel\n");
		err = -EINVAL;
	}

	return err;
}

static int atmel_ssc_configure_dma(struct ssc_device *ssc)
{
	struct dma_slave_config	slave_config;
	struct device *dev = &ssc->pdev->dev;
	int err;

	dma_cap_mask_t mask;
	dma_cap_zero(mask);
	dma_cap_set(DMA_CYCLIC, mask);

	ssc->dma.chan_tx = dma_request_slave_channel_reason(dev, "tx");
	if (IS_ERR(ssc->dma.chan_tx)) {
		err = PTR_ERR(ssc->dma.chan_tx);
		if (err == -EPROBE_DEFER) {
			dev_warn(dev, "no DMA channel available at the moment\n");
			return err;
		}
		dev_err(dev,
			"DMA TX channel not available, SSC unable to use DMA\n");
		err = -EBUSY;
		goto error;
	}

	/*
	 * No reason to check EPROBE_DEFER here since we have already requested
	 * tx channel. If it fails here, it's for another reason.
	 */
	ssc->dma.chan_rx = dma_request_slave_channel(dev, "rx");

	if (!ssc->dma.chan_rx) {
		dev_err(dev,
			"DMA RX channel not available, SSC unable to use DMA\n");
		err = -EBUSY;
		goto error;
	}

	err = atmel_ssc_dma_slave_config(ssc, &slave_config);
	if (err)
		goto error;

	dev_info(&ssc->pdev->dev,
			"Using %s (tx) and %s (rx) for DMA transfers\n",
			dma_chan_name(ssc->dma.chan_tx),
			dma_chan_name(ssc->dma.chan_rx));
	return 0;
error:
	if (ssc->dma.chan_rx)
		dma_release_channel(ssc->dma.chan_rx);
	if (!IS_ERR(ssc->dma.chan_tx))
		dma_release_channel(ssc->dma.chan_tx);
	return err;
}

static void atmel_ssc_stop_dma(struct ssc_device *ssc)
{
	if (ssc->dma.chan_rx) {
		dmaengine_terminate_all(ssc->dma.chan_rx);
		dma_release_channel(ssc->dma.chan_rx);
		dma_unmap_sg(&ssc->pdev->dev, &ssc->dma.sgrx, 1,
				DMA_FROM_DEVICE);

		ssc->dma.desc_rx = NULL;
		ssc->dma.chan_rx = NULL;
		ssc->dma.cookie_rx = -EINVAL;
	}

	if (ssc->dma.chan_tx) {
		dmaengine_terminate_all(ssc->dma.chan_tx);
		dma_release_channel(ssc->dma.chan_tx);
		dma_unmap_sg(&ssc->pdev->dev, &ssc->dma.sgtx, 1,
				DMA_TO_DEVICE);

		ssc->dma.desc_tx = NULL;
		ssc->dma.chan_tx = NULL;
		ssc->dma.cookie_tx = -EINVAL;
	}
}

/* This function is called by the DMA driver from tasklet context */
static void dma_callback_rx(void *data)
{
	struct sscdev_data *sscdev = data;
	struct ssc_device *ssc = sscdev->ssc;
	char *buf = sscdev->rx_buffer;
	struct dma_chan *chan = ssc->dma.chan_rx;
	struct circ_buf *ring = &ssc->rx_ring;
	struct dma_tx_state state;
	struct timeval tv;
	size_t count;

	/* get timestamp as early as possible */
	do_gettimeofday(&tv);
	sscdev->rx_ts = timeval_to_ns(&tv);

	dmaengine_tx_status(chan, ssc->dma.cookie_rx, &state);

	/* CPU claims ownership of RX DMA buffer */
	dma_sync_sg_for_cpu(&ssc->pdev->dev,
			    &ssc->dma.sgrx,
			    1,
			    DMA_FROM_DEVICE);

	ring->head = sg_dma_len(&ssc->dma.sgrx) - state.residue;
	BUG_ON(ring->head > sg_dma_len(&ssc->dma.sgrx));

	/* reset the number of bytes in the buffer */
	sscdev->rx_avail = sscdev->rx_offset = 0;

	if (ring->head < ring->tail) {
		count = sg_dma_len(&ssc->dma.sgrx) - ring->tail;

		memcpy(buf, ring->buf + ring->tail, count);
		buf += count;

		sscdev->rx_avail = count;
		ring->tail = 0;
	}

	if (ring->tail < ring->head) {
		count = ring->head - ring->tail;

		memcpy(buf, ring->buf + ring->tail, count);
		buf += count;

		/* Wrap ring->head if needed */
		if (ring->head >= sg_dma_len(&ssc->dma.sgrx))
			ring->head = 0;

		sscdev->rx_avail += count;
		ring->tail = ring->head;
	}

	/* SSC retreives ownership of RX DMA buffer */
	dma_sync_sg_for_device(&ssc->pdev->dev,
			       &ssc->dma.sgrx,
			       1,
			       DMA_FROM_DEVICE);
}

/* This function is called by the DMA driver from tasklet context */
static void dma_callback_tx(void *data)
{
	struct sscdev_data *sscdev = data;
	struct ssc_device *ssc = sscdev->ssc;
	unsigned long head, tail, cnt;

	atmel_ssc_lock(ssc);

	head = sscdev->tx_head;
	tail = sscdev->tx_tail;
	cnt  = sscdev->tx_queued;

	if (cnt > 0) {
		sscdev->tx_tail = (tail + 1) & (DMA_PERIODS - 1);
		sscdev->tx_queued = cnt - 1;

		up(&sscdev->tx_sem);
	} else {
		/* buffer underrun, move everything forward by a slot */
		sscdev->tx_head = (head + 1) & (DMA_PERIODS - 1);
		sscdev->tx_tail = (tail + 1) & (DMA_PERIODS - 1);
	}

	atmel_ssc_unlock(ssc);
}

/*
 * Start cyclic DMA operations
 */
static int atmel_ssc_start_dma(struct sscdev_data *sscdev)
{
	struct ssc_device *ssc = sscdev->ssc;
	struct dma_chan *rxchan = ssc->dma.chan_rx;
	struct dma_chan *txchan = ssc->dma.chan_tx;
	struct circ_buf *rxring = &ssc->rx_ring;
	struct circ_buf *txring = &ssc->tx_ring;
	struct dma_async_tx_descriptor *rxdesc;
	struct dma_async_tx_descriptor *txdesc;
	struct dma_slave_config	slave_config;
	int nent;

	dev_vdbg(&ssc->pdev->dev, "atmel_ssc_start_dma\n");

	//BUG_ON((int)rxring->buf & ~PAGE_MASK);
	//BUG_ON((int)txring->buf & ~PAGE_MASK);

	/* periods must be power-of-2 */
	BUILD_BUG_ON(DMA_PERIODS & (DMA_PERIODS - 1));

	/* Check that the channels are available */
	if (!rxchan || !txchan)
		return -ENODEV;

	/* release lock for DMA operations */
	atmel_ssc_unlock(ssc);

	/* prepare the RX dma transfer */
	sg_init_table(&ssc->dma.sgrx, 1);
	sg_set_page(&ssc->dma.sgrx,
		virt_to_page(rxring->buf),
		sscdev->buf_size,
		(int)rxring->buf & ~PAGE_MASK);

	nent = dma_map_sg(&ssc->pdev->dev, &ssc->dma.sgrx, 1, DMA_FROM_DEVICE);
	if (!nent) {
		dev_err(&ssc->pdev->dev, "RX channel not available\n");
		return -EINVAL;
	} else {
		dev_dbg(&ssc->pdev->dev, "%s: mapped %d@%p to %x\n", __func__,
			sg_dma_len(&ssc->dma.sgrx),
			rxring->buf,
			sg_dma_address(&ssc->dma.sgrx));
	}

	/* prepare the TX dma transfer */
	sg_init_table(&ssc->dma.sgtx, 1);
	sg_set_page(&ssc->dma.sgtx,
		virt_to_page(txring->buf),
		sscdev->buf_size,
		(int)txring->buf & ~PAGE_MASK);

	nent = dma_map_sg(&ssc->pdev->dev, &ssc->dma.sgtx, 1, DMA_TO_DEVICE);
	if (!nent) {
		dev_err(&ssc->pdev->dev, "TX channel not available\n");
		return -EINVAL;
	} else {
		dev_dbg(&ssc->pdev->dev, "%s: mapped %d@%p to %x\n", __func__,
			sg_dma_len(&ssc->dma.sgtx),
			txring->buf,
			sg_dma_address(&ssc->dma.sgtx));
	}

	/* configure DMA slave */
	if (atmel_ssc_dma_slave_config(ssc, &slave_config))
		goto err_exit;

	/* Send both scatterlists */
	rxdesc = dmaengine_prep_dma_cyclic(rxchan,
					sg_dma_address(&ssc->dma.sgrx),
					sg_dma_len(&ssc->dma.sgrx),
					sg_dma_len(&ssc->dma.sgrx)/DMA_PERIODS,
					DMA_DEV_TO_MEM,
					DMA_PREP_INTERRUPT);
	if (!rxdesc)
		goto err_dma;

	txdesc = dmaengine_prep_dma_cyclic(txchan,
					sg_dma_address(&ssc->dma.sgtx),
					sg_dma_len(&ssc->dma.sgtx),
					sg_dma_len(&ssc->dma.sgtx)/DMA_PERIODS,
					DMA_MEM_TO_DEV,
					DMA_PREP_INTERRUPT);
	if (!txdesc)
		goto err_dma;

	/* fire DMA transfer */
	rxdesc->callback = dma_callback_rx;
	rxdesc->callback_param = sscdev;
	ssc->dma.desc_rx = rxdesc;

	txdesc->callback = dma_callback_tx;
	txdesc->callback_param = sscdev;
	ssc->dma.desc_tx = txdesc;

	ssc->dma.cookie_rx = dmaengine_submit(rxdesc);
	ssc->dma.cookie_tx = dmaengine_submit(txdesc);

	dev_dbg(&ssc->pdev->dev, "DMA transfer started\n");

	/* take back lock */
	atmel_ssc_lock(ssc);
	return 0;

err_dma:
	atmel_ssc_stop_dma(ssc);
err_exit:
	atmel_ssc_lock(ssc);
	return -ENOMEM;
}

static irqreturn_t atmel_ssc_pio_interrupt(int irq, void *dev_id)
{
	return IRQ_HANDLED;
}

/*-------------------------------------------------------------------------*/

static int sscdev_setup_ll(struct ssc_device *ssc, bool ext_clk)
{
	struct platform_device *pdev = ssc->pdev;
	u32 tfmr, rfmr, tcmr, rcmr;
	int wlen;

	dev_dbg(&pdev->dev, "ssc_setup: SSC_SR=0x%u\n",
		ssc_readl(ssc->regs, SR));

	/* Enable peripheral clock for this SSC */
	dev_dbg(&pdev->dev, "ssc_setup: starting clock\n");
	clk_prepare_enable(ssc->clk);

	/* Reset the SSC to keep it at a clean status */
	ssc_writel(ssc->regs, CR, 0);
	ssc_writel(ssc->regs, CR, SSC_BIT(CR_TXDIS) | SSC_BIT(CR_RXDIS));
	ssc_writel(ssc->regs, CR, 0);
	ssc_writel(ssc->regs, CR, SSC_BIT(CR_SWRST));

	/* word length in bits */
	wlen = ssc->word_size * 8;

	/* Prepare peripheral RX and TX mode registers */
	if (ext_clk) {
		/* SIPO configuration */

		rcmr =	  SSC_BF(RCMR_PERIOD, (wlen / 2) - 1)
			| SSC_BF(RCMR_STTDLY, 0)
			| SSC_BF(RCMR_START, SSC_START_FALLING_RF)
			| SSC_BF(RCMR_CKI, SSC_CKI_FALLING)
			| SSC_BF(RCMR_CKO, SSC_CKO_NONE)
			| SSC_BF(RCMR_CKS, SSC_CKS_DIV);

		rfmr =	  SSC_BF(RFMR_FSEDGE, SSC_FSEDGE_NEGATIVE)
			| SSC_BF(RFMR_FSOS, SSC_FSOS_NONE)
			| SSC_BF(RFMR_FSLEN, (wlen-1) % 16)
			| SSC_BF(RFMR_FSLEN_EXT, (wlen-1) / 16)
			| SSC_BF(RFMR_DATNB, 0)
			| SSC_BIT(RFMR_MSBF)
			| SSC_BF(RFMR_LOOP, 0)
			| SSC_BF(RFMR_DATLEN, wlen-1);

		tcmr =	  SSC_BF(TCMR_PERIOD, 0)
			| SSC_BF(TCMR_STTDLY, 0)
			| SSC_BF(TCMR_START, SSC_START_FALLING_RF)
			| SSC_BF(TCMR_CKI, SSC_CKI_FALLING)
			| SSC_BF(TCMR_CKO, SSC_CKO_NONE)
			| SSC_BF(TCMR_CKS, SSC_CKS_PIN);

		tfmr =	  SSC_BF(TFMR_FSEDGE, SSC_FSEDGE_NEGATIVE)
			| SSC_BF(TFMR_FSDEN, 0)
			| SSC_BF(TFMR_FSOS, SSC_FSOS_NONE)
			| SSC_BF(TFMR_FSLEN, 0)
			| SSC_BF(TFMR_FSLEN_EXT, 0)
			| SSC_BF(TFMR_DATNB, 0)
			| SSC_BIT(TFMR_MSBF)
			| SSC_BF(TFMR_DATDEF, 0)
			| SSC_BF(TFMR_DATLEN, wlen-1);
	} else {
		/* ADC configuration */

		rcmr =	  SSC_BF(RCMR_PERIOD, (wlen / 2) - 1)
			| SSC_BF(RCMR_STTDLY, 0)
			| SSC_BF(RCMR_START, SSC_START_FALLING_RF)
			| SSC_BF(RCMR_CKI, SSC_CKI_FALLING)
			| SSC_BF(RCMR_CKO, SSC_CKO_NONE)
			| SSC_BF(RCMR_CKS, SSC_CKS_DIV);

		rfmr =	  SSC_BF(RFMR_FSEDGE, SSC_FSEDGE_NEGATIVE)
			| SSC_BF(RFMR_FSOS, SSC_FSOS_NONE)
			| SSC_BF(RFMR_FSLEN, (wlen-1) % 16)
			| SSC_BF(RFMR_FSLEN_EXT, (wlen-1) / 16)
			| SSC_BF(RFMR_DATNB, 0)
			| SSC_BIT(RFMR_MSBF)
			| SSC_BF(RFMR_LOOP, 0)
			| SSC_BF(RFMR_DATLEN, wlen-1);

		tcmr =	  SSC_BF(TCMR_PERIOD, (wlen / 2) - 1)
			| SSC_BF(TCMR_STTDLY, 0)
			| SSC_BF(TCMR_START, SSC_START_CONTINUOUS)
			| SSC_BF(TCMR_CKI, SSC_CKI_RISING)
			| SSC_BF(TCMR_CKO, SSC_CKO_TRANSFER)
			| SSC_BF(TCMR_CKS, SSC_CKS_DIV);

		tfmr =	  SSC_BF(TFMR_FSEDGE, SSC_FSEDGE_POSITIVE)
			| SSC_BF(TFMR_FSDEN, 0)
			| SSC_BF(TFMR_FSOS, SSC_FSOS_POSITIVE)
			| SSC_BF(TFMR_FSLEN, 1)
			| SSC_BF(TFMR_FSLEN_EXT, 0)
			| SSC_BF(TFMR_DATNB, 0)
			| SSC_BIT(TFMR_MSBF)
			| SSC_BF(TFMR_DATDEF, 0)
			| SSC_BF(TFMR_DATLEN, wlen-1);
	}

	/* set SSC clock mode register */
	/* TODO: change me with the correct div */
	ssc_writel(ssc->regs, CMR, SSC_BF(CMR_DIV, 10));

	/* set receive clock mode and format */
	ssc_writel(ssc->regs, RCMR, rcmr);
	ssc_writel(ssc->regs, RFMR, rfmr);

	/* set transmit clock mode and format */
	ssc_writel(ssc->regs, TCMR, tcmr);
	ssc_writel(ssc->regs, TFMR, tfmr);

	/* ... and go! */
	ssc_writel(ssc->regs, CR, SSC_BIT(CR_TXEN) | SSC_BIT(CR_RXEN));

	dev_dbg(&pdev->dev, "ssc_setup: SSC initialized\n");

	return 0;
}

static int sscdev_start(struct sscdev_data *sscdev)
{
	int ret = 0;

	atmel_ssc_lock(sscdev->ssc);

	sscdev->rx_buffer = kzalloc(sscdev->buf_size, GFP_KERNEL);
	if (!sscdev->rx_buffer) {
		ret = -ENOMEM;
		goto err_rx_alloc;
	}

	ret = sscdev_setup_ll(sscdev->ssc, sscdev->use_external_clk);
	if (ret)
		goto err_setup;

	ret = atmel_ssc_start_dma(sscdev);
	if (ret)
		goto err_setup;

	atmel_ssc_unlock(sscdev->ssc);
	return 0;

err_setup:
	kfree(sscdev->rx_buffer);
err_rx_alloc:
	atmel_ssc_unlock(sscdev->ssc);
	return ret;
}

static void sscdev_stop(struct sscdev_data *sscdev)
{
	atmel_ssc_lock(sscdev->ssc);
	atmel_ssc_stop_dma(sscdev->ssc);
	atmel_ssc_unlock(sscdev->ssc);
}

static int sscdev_setup_ssc(struct sscdev_data *sscdev)
{
	struct ssc_device *ssc = sscdev->ssc;
	int ret;

	ssc->word_size = 4; /* in bytes */

	memset(&ssc->rx_ring, 0, sizeof(ssc->rx_ring));
	ssc->rx_ring.buf = kzalloc(sscdev->buf_size, GFP_KERNEL);
	if (!ssc->rx_ring.buf) {
		dev_err(&ssc->pdev->dev, "could not allocate RX ring buffer, OOM\n");
		return -ENOMEM;
	}

	memset(&ssc->tx_ring, 0, sizeof(ssc->tx_ring));
	ssc->tx_ring.buf = kzalloc(sscdev->buf_size, GFP_KERNEL);
	if (!ssc->tx_ring.buf) {
		dev_err(&ssc->pdev->dev, "could not allocate TX ring buffer, OOM\n");
		ret = -ENOMEM;
		goto out_free_rx_ring;
	}

	ret = atmel_ssc_configure_dma(ssc);
	if (ret == -EPROBE_DEFER)
		return ret;
	else if (ret)
		goto out_free_tx_ring;

	ret = devm_request_irq(&ssc->pdev->dev, ssc->irq, atmel_ssc_pio_interrupt,
				0, dev_name(&ssc->pdev->dev), ssc);
	if (ret)
		goto out_free_tx_ring;

	return 0;

out_free_tx_ring:
	kfree(ssc->tx_ring.buf);
	ssc->tx_ring.buf = NULL;

out_free_rx_ring:
	kfree(ssc->rx_ring.buf);
	ssc->rx_ring.buf = NULL;

	return ret;
}

static void sscdev_remove_ssc(struct ssc_device *ssc)
{
	atmel_ssc_stop_dma(ssc);

	kfree(ssc->rx_ring.buf);
	kfree(ssc->tx_ring.buf);
}

/*-------------------------------------------------------------------------*/

static ssize_t sscdev_write(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	struct sscdev_data *sscdev = filp->private_data;
	struct ssc_device *ssc = sscdev->ssc;
	struct circ_buf *ring = &ssc->tx_ring;
	unsigned long head, tail, cnt;

	if (down_interruptible(&sscdev->tx_sem)) {
		pr_info("sscdev: could not hold semaphore\n");
		return -ERESTARTSYS;
	}

	/* CPU claims ownership of TX DMA buffer */
	dma_sync_sg_for_cpu(&ssc->pdev->dev,
			    &ssc->dma.sgtx,
			    1,
			    DMA_TO_DEVICE);

	atmel_ssc_lock(sscdev->ssc);

	head = sscdev->tx_head;
	tail = sscdev->tx_tail;
	cnt  = sscdev->tx_queued;

	BUG_ON(count != sscdev->period_len); // avoids complicating things!

	if (cnt < (DMA_PERIODS - 1)) {
		BUG_ON(copy_from_user(ring->buf + (head * sscdev->period_len),
				      buf, count));
		*f_pos += count;
		
		sscdev->tx_head = (head + 1) & (DMA_PERIODS - 1);
		sscdev->tx_queued = cnt + 1;
	}

	atmel_ssc_unlock(sscdev->ssc);

	/* SSC retreives ownership of TX DMA buffer */
	dma_sync_sg_for_device(&ssc->pdev->dev,
			       &ssc->dma.sgtx,
			       1,
			       DMA_TO_DEVICE);

	return count;
}

static ssize_t sscdev_read(struct file *filp, char __user *buf,
		size_t count, loff_t *f_pos)
{
	struct sscdev_data *sscdev = filp->private_data;

	/* No data available, try later */
	if (!sscdev->rx_avail) {
		return 0;
	}

	/* round down to nearest word */
	count &= (~3);

	/* clamp to the actual data available */
	count = min(count, sscdev->rx_avail + sizeof(s64));

	/* put timestamp as first element in the buffer */
	BUG_ON(copy_to_user(buf, (void*)&sscdev->rx_ts, sizeof(sscdev->rx_ts)));
	count -= sizeof(s64);

	if (count > 0 && sscdev->rx_avail > 0) {
		BUG_ON(copy_to_user(buf + sizeof(s64),
				    sscdev->rx_buffer + sscdev->rx_offset,
				    count));

		sscdev->rx_offset += count;
		sscdev->rx_avail -= count;
	} else {
		return 0;
	}

	return count + sizeof(s64);
}

static int sscdev_open(struct inode *inode, struct file *filp)
{
	struct sscdev_data *sscdev;
	int status = -ENXIO;

	mutex_lock(&device_list_lock);

	list_for_each_entry(sscdev, &device_list, device_entry) {
		if (sscdev->devt == inode->i_rdev) {
			status = 0;
			break;
		}
	}

	if (status) {
		pr_debug("sscdev: nothing for minor %d\n", iminor(inode));
		goto err_find_dev;
	}

	sscdev->users++;
	filp->private_data = sscdev;
	nonseekable_open(inode, filp);

err_find_dev:
	mutex_unlock(&device_list_lock);
	return status;
}

static int sscdev_release(struct inode *inode, struct file *filp)
{
	struct sscdev_data *sscdev;
	int status = 0;

	mutex_lock(&device_list_lock);
	sscdev = filp->private_data;
	filp->private_data = NULL;

	sscdev->users--;
	mutex_unlock(&device_list_lock);

	return status;
}

static const struct file_operations sscdev_fops = {
	.owner		= THIS_MODULE,
	.read		= sscdev_read,
	.write		= sscdev_write,
	.open		= sscdev_open,
	.release	= sscdev_release,
	.llseek		= no_llseek,
};

/*-------------------------------------------------------------------------*/

static struct class *sscdev_class;

/*-------------------------------------------------------------------------*/

static int sscdev_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *ssc_np;
	struct sscdev_data *sscdev;
	struct ssc_device *ssc;
	unsigned long minor;
	int id, ret;
	u32 val;

	if (!np) {
		dev_err(&pdev->dev, "only device tree supported\n");
		return -EINVAL;
	}

	sscdev = kzalloc(sizeof(*sscdev), GFP_KERNEL);
	if (!sscdev)
		return -ENOMEM;

	/* Get SSC id from device tree node and grab it */
	ssc_np = of_parse_phandle(np, "develer,ssc", 0);
	if (!ssc_np) {
		dev_err(&pdev->dev, "failed to get SSC info\n");
		ret = -EINVAL;
		goto ssc_failed;
	}
	id = of_alias_get_id(ssc_np, "ssc");
	of_node_put(ssc_np);

	ssc = ssc_request(id);
	if (IS_ERR(ssc)) {
		dev_err(&pdev->dev, "unable to request SSC %d: %ld\n", id,
			PTR_ERR(ssc));
		ret = PTR_ERR(ssc);
		goto ssc_failed;
	}
	sscdev->ssc = ssc;

	/* one slot is always occupied since we are transmitting it */
	sema_init(&sscdev->tx_sem, DMA_PERIODS - 1);

	/* When starting: slot #0 is being sent, slot #1 is free */
	sscdev->tx_head = 1;
	sscdev->tx_tail = 0;

	sscdev->use_external_clk = of_property_read_bool(np, "develer,external-clk");

	ret = of_property_read_u32(np, "develer,bufsize", &val);
	if (ret) {
		dev_err(&pdev->dev, "could not read buffer size: %d\n", ret);
		goto setup_failed;
	}

	sscdev->buf_size = val;
	sscdev->period_len = val / DMA_PERIODS;

	ret = sscdev_setup_ssc(sscdev);
	if (ret) {
		dev_err(&pdev->dev, "could not setup SSC: %d\n", ret);
		goto setup_failed;
	}

	ret = sscdev_start(sscdev);
	if (ret) {
		dev_err(&pdev->dev, "could not start SSC xfer: %d\n", ret);
		goto dev_failed;
	}

	INIT_LIST_HEAD(&sscdev->device_entry);

	/* Allocate a minor. Reusing minors is fine
	 * as long as udev is working.
	 */
	mutex_lock(&device_list_lock);
	minor = find_first_zero_bit(minors, N_SSC_MINORS);
	if (minor < N_SSC_MINORS) {
		struct device *dev;

		sscdev->devt = MKDEV(major, minor);
		dev = device_create(sscdev_class, NULL, sscdev->devt,
				    sscdev, "sscdev%d", id);
		ret = PTR_ERR_OR_ZERO(dev);
	} else {
		dev_warn(&pdev->dev, "no minor number available!\n");
		ret = -ENODEV;
	}
	if (ret == 0) {
		set_bit(minor, minors);
		list_add(&sscdev->device_entry, &device_list);
	}
	mutex_unlock(&device_list_lock);

	if (ret == 0)
		platform_set_drvdata(pdev, sscdev);
	else
		goto alloc_failed;

	return ret;

alloc_failed:
	sscdev_stop(sscdev);
dev_failed:
	sscdev_remove_ssc(ssc);
setup_failed:
	ssc_free(ssc);
ssc_failed:
	kfree(sscdev);
	return ret;
}

static int sscdev_remove(struct platform_device *pdev)
{
	struct sscdev_data *sscdev = platform_get_drvdata(pdev);

	sscdev_stop(sscdev);
	sscdev_remove_ssc(sscdev->ssc);
	sscdev->ssc = NULL;

	/* prevent new opens */
	mutex_lock(&device_list_lock);
	list_del(&sscdev->device_entry);
	device_destroy(sscdev_class, sscdev->devt);
	clear_bit(MINOR(sscdev->devt), minors);
	if (sscdev->users == 0)
		kfree(sscdev);
	mutex_unlock(&device_list_lock);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id sscdev_dt_ids[] = {
	{
		.compatible = "develer,sscdev",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, sscdev_dt_ids);
#endif

static struct platform_driver sscdev_driver = {
	.driver		= {
		.name		= "sscdev",
		.owner		= THIS_MODULE,
		.of_match_table	= of_match_ptr(sscdev_dt_ids),
	},
	.probe		= sscdev_probe,
	.remove		= sscdev_remove,
};

/*-------------------------------------------------------------------------*/

static int __init sscdev_init(void)
{
	int status;

	/* Claim our 256 reserved device numbers.  Then register a class
	 * that will key udev/mdev to add/remove /dev nodes.  Last, register
	 * the driver which manages those device numbers.
	 */
	BUILD_BUG_ON(N_SSC_MINORS > 256);

	major = register_chrdev(0, "ssc", &sscdev_fops);
	if (major < 0)
		return major;

	sscdev_class = class_create(THIS_MODULE, "sscdev");
	if (IS_ERR(sscdev_class)) {
		unregister_chrdev(major, sscdev_driver.driver.name);
		return PTR_ERR(sscdev_class);
	}

	status = platform_driver_register(&sscdev_driver);
	if (status < 0) {
		class_destroy(sscdev_class);
		unregister_chrdev(major, sscdev_driver.driver.name);
	}
	return status;
}
module_init(sscdev_init);

static void __exit sscdev_exit(void)
{
	platform_driver_unregister(&sscdev_driver);
	class_destroy(sscdev_class);
	unregister_chrdev(major, sscdev_driver.driver.name);
}
module_exit(sscdev_exit);

MODULE_AUTHOR("Pietro Lorefice <pietro@develer.com>");
MODULE_DESCRIPTION("User mode SSC device interface");
MODULE_LICENSE("GPL");
MODULE_ALIAS("ssc:sscdev");
