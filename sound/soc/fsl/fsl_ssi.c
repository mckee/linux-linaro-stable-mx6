/*
 * Freescale SSI ALSA SoC Digital Audio Interface (DAI) driver
 *
 * Author: Timur Tabi <timur@freescale.com>
 *
 * Copyright (C) 2007-2013 Freescale Semiconductor, Inc.
 *
 * This file is licensed under the terms of the GNU General Public License
 * version 2.  This program is licensed "as is" without any warranty of any
 * kind, whether express or implied.
 */

#include <linux/busfreq-imx6.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>

#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>

#include "fsl_ssi.h"
#include "imx-pcm.h"

#ifdef PPC
#define read_ssi(addr)			 in_be32(addr)
#define write_ssi(val, addr)		 out_be32(addr, val)
#define write_ssi_mask(addr, clear, set) clrsetbits_be32(addr, clear, set)
#elif defined ARM
#define read_ssi(addr)			 readl(addr)
#define write_ssi(val, addr)		 writel(val, addr)
/*
 * FIXME: Proper locking should be added at write_ssi_mask caller level
 * to ensure this register read/modify/write sequence is race free.
 */
static inline void write_ssi_mask(u32 __iomem *addr, u32 clear, u32 set)
{
	u32 val = readl(addr);
	val = (val & ~clear) | set;
	writel(val, addr);
}
#endif

#ifdef DEBUG
#define NUM_OF_SSI_REG (sizeof(struct ccsr_ssi) / sizeof(__be32))

void dump_reg(struct ccsr_ssi __iomem *ssi)
{
	u32 val, i;

	for (i = 0; i < NUM_OF_SSI_REG; i++) {
		if (&ssi->stx0 + i == NULL)
			continue;
		val = read_ssi(&ssi->stx0 + i);
		pr_debug("REG %x = %x\n", (u32)(&ssi->stx0 + i) & 0xff, val);
	}
}
#else
void dump_reg(struct ccsr_ssi __iomem *ssi) {}
#endif

/**
 * FSLSSI_I2S_RATES: sample rates supported by the I2S
 *
 * This driver currently only supports the SSI running in I2S slave mode,
 * which means the codec determines the sample rate.  Therefore, we tell
 * ALSA that we support all rates and let the codec driver decide what rates
 * are really supported.
 */
#define FSLSSI_I2S_RATES (SNDRV_PCM_RATE_5512 | SNDRV_PCM_RATE_8000_192000 | \
			  SNDRV_PCM_RATE_CONTINUOUS)

/**
 * FSLSSI_I2S_FORMATS: audio formats supported by the SSI
 *
 * This driver currently only supports the SSI running in I2S slave mode.
 *
 * The SSI has a limitation in that the samples must be in the same byte
 * order as the host CPU.  This is because when multiple bytes are written
 * to the STX register, the bytes and bits must be written in the same
 * order.  The STX is a shift register, so all the bits need to be aligned
 * (bit-endianness must match byte-endianness).  Processors typically write
 * the bits within a byte in the same order that the bytes of a word are
 * written in.  So if the host CPU is big-endian, then only big-endian
 * samples will be written to STX properly.
 */
#ifdef __BIG_ENDIAN
#define FSLSSI_I2S_FORMATS (SNDRV_PCM_FMTBIT_S8 | SNDRV_PCM_FMTBIT_S16_BE | \
	 SNDRV_PCM_FMTBIT_S18_3BE | SNDRV_PCM_FMTBIT_S20_3BE | \
	 SNDRV_PCM_FMTBIT_S24_3BE | SNDRV_PCM_FMTBIT_S24_BE)
#else
#define FSLSSI_I2S_FORMATS (SNDRV_PCM_FMTBIT_S8 | SNDRV_PCM_FMTBIT_S16_LE | \
	 SNDRV_PCM_FMTBIT_S18_3LE | SNDRV_PCM_FMTBIT_S20_3LE | \
	 SNDRV_PCM_FMTBIT_S24_3LE | SNDRV_PCM_FMTBIT_S24_LE)
#endif

/* SIER bitflag of interrupts to enable */
#define SIER_FLAGS (CCSR_SSI_SIER_TFRC_EN | CCSR_SSI_SIER_TIE | \
		    CCSR_SSI_SIER_TUE0_EN | CCSR_SSI_SIER_TUE1_EN | \
		    CCSR_SSI_SIER_RFRC_EN | CCSR_SSI_SIER_RIE | \
		    CCSR_SSI_SIER_ROE0_EN | CCSR_SSI_SIER_ROE1_EN)

/**
 * fsl_ssi_private: per-SSI private data
 *
 * @ssi: pointer to the SSI's registers
 * @ssi_phys: physical address of the SSI registers
 * @irq: IRQ of this SSI
 * @first_stream: pointer to the stream that was opened first
 * @second_stream: pointer to second stream
 * @playback: the number of playback streams opened
 * @capture: the number of capture streams opened
 * @cpu_dai: the CPU DAI for this device
 * @dev_attr: the sysfs device attribute structure
 * @stats: SSI statistics
 * @name: name for this device
 */
struct fsl_ssi_private {
	struct ccsr_ssi __iomem *ssi;
	dma_addr_t ssi_phys;
	unsigned int irq;
	struct snd_pcm_substream *first_stream;
	struct snd_pcm_substream *second_stream;
	unsigned int fifo_depth;
	struct snd_soc_dai_driver cpu_dai_drv;
	struct device_attribute dev_attr;
	struct platform_device *pdev;

	unsigned long sysrate;
	bool new_binding;
	bool ssi_on_imx;
	bool use_dual_fifo;
	u8 i2s_mode;
	struct clk *coreclk;
	struct clk *clk;
	struct snd_dmaengine_dai_dma_data dma_params_tx;
	struct snd_dmaengine_dai_dma_data dma_params_rx;

	struct {
		unsigned int rfrc;
		unsigned int tfrc;
		unsigned int cmdau;
		unsigned int cmddu;
		unsigned int rxt;
		unsigned int rdr1;
		unsigned int rdr0;
		unsigned int tde1;
		unsigned int tde0;
		unsigned int roe1;
		unsigned int roe0;
		unsigned int tue1;
		unsigned int tue0;
		unsigned int tfs;
		unsigned int rfs;
		unsigned int tls;
		unsigned int rls;
		unsigned int rff1;
		unsigned int rff0;
		unsigned int tfe1;
		unsigned int tfe0;
	} stats;

	char name[1];
};

/**
 * fsl_ssi_isr: SSI interrupt handler
 *
 * Although it's possible to use the interrupt handler to send and receive
 * data to/from the SSI, we use the DMA instead.  Programming is more
 * complicated, but the performance is much better.
 *
 * This interrupt handler is used only to gather statistics.
 *
 * @irq: IRQ of the SSI device
 * @dev_id: pointer to the ssi_private structure for this SSI device
 */
static irqreturn_t fsl_ssi_isr(int irq, void *dev_id)
{
	struct fsl_ssi_private *ssi_private = dev_id;
	struct ccsr_ssi __iomem *ssi = ssi_private->ssi;
	irqreturn_t ret = IRQ_NONE;
	__be32 sisr;
	__be32 sisr2 = 0;

	/* We got an interrupt, so read the status register to see what we
	   were interrupted for.  We mask it with the Interrupt Enable register
	   so that we only check for events that we're interested in.
	 */
	sisr = read_ssi(&ssi->sisr) & SIER_FLAGS;

	if (sisr & CCSR_SSI_SISR_RFRC) {
		ssi_private->stats.rfrc++;
		sisr2 |= CCSR_SSI_SISR_RFRC;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TFRC) {
		ssi_private->stats.tfrc++;
		sisr2 |= CCSR_SSI_SISR_TFRC;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_CMDAU) {
		ssi_private->stats.cmdau++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_CMDDU) {
		ssi_private->stats.cmddu++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RXT) {
		ssi_private->stats.rxt++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RDR1) {
		ssi_private->stats.rdr1++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RDR0) {
		ssi_private->stats.rdr0++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TDE1) {
		ssi_private->stats.tde1++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TDE0) {
		ssi_private->stats.tde0++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_ROE1) {
		ssi_private->stats.roe1++;
		sisr2 |= CCSR_SSI_SISR_ROE1;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_ROE0) {
		ssi_private->stats.roe0++;
		sisr2 |= CCSR_SSI_SISR_ROE0;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TUE1) {
		ssi_private->stats.tue1++;
		sisr2 |= CCSR_SSI_SISR_TUE1;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TUE0) {
		ssi_private->stats.tue0++;
		sisr2 |= CCSR_SSI_SISR_TUE0;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TFS) {
		ssi_private->stats.tfs++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RFS) {
		ssi_private->stats.rfs++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TLS) {
		ssi_private->stats.tls++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RLS) {
		ssi_private->stats.rls++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RFF1) {
		ssi_private->stats.rff1++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_RFF0) {
		ssi_private->stats.rff0++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TFE1) {
		ssi_private->stats.tfe1++;
		ret = IRQ_HANDLED;
	}

	if (sisr & CCSR_SSI_SISR_TFE0) {
		ssi_private->stats.tfe0++;
		ret = IRQ_HANDLED;
	}

	/* Clear the bits that we set */
	if (sisr2)
		write_ssi(sisr2, &ssi->sisr);

	return ret;
}

/**
 * fsl_ssi_startup: create a new substream
 *
 * This is the first function called when a stream is opened.
 *
 * If this is the first stream open, then grab the IRQ and program most of
 * the SSI registers.
 */
static int fsl_ssi_startup(struct snd_pcm_substream *substream,
			   struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct fsl_ssi_private *ssi_private =
		snd_soc_dai_get_drvdata(rtd->cpu_dai);
	int synchronous = ssi_private->cpu_dai_drv.symmetric_rates;

	if (ssi_private->ssi_on_imx) {
		pm_runtime_get_sync(dai->dev);

		clk_prepare_enable(ssi_private->coreclk);
		clk_prepare_enable(ssi_private->clk);

		/* When using dual fifo mode, it would be safer if we ensure
		 * its period size to be an even number. If appearing to an
		 * odd number, the 2nd fifo might be neglected by SDMA sciprt
		 * at the end of each period.
		 */
		if (ssi_private->use_dual_fifo)
			snd_pcm_hw_constraint_step(substream->runtime, 0,
					SNDRV_PCM_HW_PARAM_PERIOD_SIZE, 2);
	}

	/*
	 * If this is the first stream opened, then request the IRQ
	 * and initialize the SSI registers.
	 */
	if (!ssi_private->first_stream) {
		struct ccsr_ssi __iomem *ssi = ssi_private->ssi;

		ssi_private->first_stream = substream;

		/*
		 * Section 16.5 of the MPC8610 reference manual says that the
		 * SSI needs to be disabled before updating the registers we set
		 * here.
		 */
		write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_SSIEN, 0);

		/*
		 * Program the SSI into I2S Slave Non-Network Synchronous mode.
		 * Also enable the transmit and receive FIFO.
		 *
		 * FIXME: Little-endian samples require a different shift dir
		 */
		ssi_private->i2s_mode = CCSR_SSI_SCR_I2S_MODE_SLAVE;
		write_ssi_mask(&ssi->scr,
			CCSR_SSI_SCR_I2S_MODE_MASK | CCSR_SSI_SCR_SYN,
			CCSR_SSI_SCR_TFR_CLK_DIS | ssi_private->i2s_mode
			| (synchronous ? CCSR_SSI_SCR_SYN : 0));

		write_ssi(CCSR_SSI_STCR_TXBIT0 | CCSR_SSI_STCR_TFEN0 |
			 CCSR_SSI_STCR_TFSI | CCSR_SSI_STCR_TEFS |
			 CCSR_SSI_STCR_TSCKP, &ssi->stcr);

		write_ssi(CCSR_SSI_SRCR_RXBIT0 | CCSR_SSI_SRCR_RFEN0 |
			 CCSR_SSI_SRCR_RFSI | CCSR_SSI_SRCR_REFS |
			 CCSR_SSI_SRCR_RSCKP, &ssi->srcr);

		/*
		 * The DC and PM bits are only used if the SSI is the clock
		 * master.
		 */

		/* Enable the interrupts and DMA requests */
		write_ssi(SIER_FLAGS, &ssi->sier);

		/*
		 * Set the watermark for transmit FIFI 0 and receive FIFO 0. We
		 * don't use FIFO 1.  We program the transmit water to signal a
		 * DMA transfer if there are only two (or fewer) elements left
		 * in the FIFO.  Two elements equals one frame (left channel,
		 * right channel).  This value, however, depends on the depth of
		 * the transmit buffer.
		 *
		 * We program the receive FIFO to notify us if at least two
		 * elements (one frame) have been written to the FIFO.  We could
		 * make this value larger (and maybe we should), but this way
		 * data will be written to memory as soon as it's available.
		 */
		write_ssi(CCSR_SSI_SFCSR_TFWM0(ssi_private->fifo_depth - 2) |
			CCSR_SSI_SFCSR_RFWM0(ssi_private->fifo_depth - 2) |
			CCSR_SSI_SFCSR_TFWM1(ssi_private->fifo_depth - 2) |
			CCSR_SSI_SFCSR_RFWM1(ssi_private->fifo_depth - 2),
			&ssi->sfcsr);

		/* Select Single/Dual fifo mode */
		if (ssi_private->use_dual_fifo) {
			write_ssi_mask(&ssi->srcr, 0, CCSR_SSI_SRCR_RFEN1);
			write_ssi_mask(&ssi->stcr, 0, CCSR_SSI_STCR_TFEN1);
			write_ssi_mask(&ssi->scr, 0, CCSR_SSI_SCR_TCH_EN);
		} else {
			write_ssi_mask(&ssi->srcr, CCSR_SSI_SRCR_RFEN1, 0);
			write_ssi_mask(&ssi->stcr, CCSR_SSI_STCR_TFEN1, 0);
			write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_TCH_EN, 0);
		}

		/*
		 * We keep the SSI disabled because if we enable it, then the
		 * DMA controller will start.  It's not supposed to start until
		 * the SCR.TE (or SCR.RE) bit is set, but it does anyway.  The
		 * DMA controller will transfer one "BWC" of data (i.e. the
		 * amount of data that the MR.BWC bits are set to).  The reason
		 * this is bad is because at this point, the PCM driver has not
		 * finished initializing the DMA controller.
		 */
	} else {
		ssi_private->second_stream = substream;
	}

	return 0;
}

/**
 * fsl_ssi_hw_params - program the sample size
 *
 * Most of the SSI registers have been programmed in the startup function,
 * but the word length must be programmed here.  Unfortunately, programming
 * the SxCCR.WL bits requires the SSI to be temporarily disabled.  This can
 * cause a problem with supporting simultaneous playback and capture.  If
 * the SSI is already playing a stream, then that stream may be temporarily
 * stopped when you start capture.
 *
 * Note: The SxCCR.DC and SxCCR.PM bits are only used if the SSI is the
 * clock master.
 */
static int fsl_ssi_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *hw_params, struct snd_soc_dai *cpu_dai)
{
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(cpu_dai);
	struct ccsr_ssi __iomem *ssi = ssi_private->ssi;
	unsigned int sample_size =
		snd_pcm_format_width(params_format(hw_params));
	u32 wl = CCSR_SSI_SxCCR_WL(sample_size);
	int enabled = read_ssi(&ssi->scr) & CCSR_SSI_SCR_SSIEN;
	unsigned int channels = params_channels(hw_params);
	int ret;

	/*
	 * If we're in synchronous mode, and the SSI is already enabled,
	 * then STCCR is already set properly.
	 */
	if (enabled && ssi_private->cpu_dai_drv.symmetric_rates)
		return 0;

	if (ssi_private->sysrate) {
		ret = clk_set_rate(ssi_private->clk, ssi_private->sysrate);
		if (ret) {
			dev_err(cpu_dai->dev, "failed to set clock rate\n");
			return ret;
		}
	}

	/*
	 * FIXME: The documentation says that SxCCR[WL] should not be
	 * modified while the SSI is enabled.  The only time this can
	 * happen is if we're trying to do simultaneous playback and
	 * capture in asynchronous mode.  Unfortunately, I have been enable
	 * to get that to work at all on the P1022DS.  Therefore, we don't
	 * bother to disable/enable the SSI when setting SxCCR[WL], because
	 * the SSI will stop anyway.  Maybe one day, this will get fixed.
	 */

	/* In synchronous mode, the SSI uses STCCR for capture */
	if ((substream->stream == SNDRV_PCM_STREAM_PLAYBACK) ||
	    ssi_private->cpu_dai_drv.symmetric_rates)
		write_ssi_mask(&ssi->stccr, CCSR_SSI_SxCCR_WL_MASK, wl);
	else
		write_ssi_mask(&ssi->srccr, CCSR_SSI_SxCCR_WL_MASK, wl);

	write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_NET | CCSR_SSI_SCR_I2S_MODE_MASK,
			channels == 1 ? 0 : ssi_private->i2s_mode);

	return 0;
}

/**
 * fsl_ssi_trigger: start and stop the DMA transfer.
 *
 * This function is called by ALSA to start, stop, pause, and resume the DMA
 * transfer of data.
 *
 * The DMA channel is in external master start and pause mode, which
 * means the SSI completely controls the flow of data.
 */
static int fsl_ssi_trigger(struct snd_pcm_substream *substream, int cmd,
			   struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(rtd->cpu_dai);
	struct ccsr_ssi __iomem *ssi = ssi_private->ssi;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			write_ssi_mask(&ssi->scr, 0,
				CCSR_SSI_SCR_SSIEN | CCSR_SSI_SCR_TE);
			write_ssi_mask(&ssi->sier, 0, CCSR_SSI_SIER_TDMAE);
		} else {
			write_ssi_mask(&ssi->scr, 0,
				CCSR_SSI_SCR_SSIEN | CCSR_SSI_SCR_RE);
			write_ssi_mask(&ssi->sier, 0, CCSR_SSI_SIER_RDMAE);
		}
		dump_reg(ssi);
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_TE, 0);
			write_ssi_mask(&ssi->sier, CCSR_SSI_SIER_TDMAE, 0);
		} else {
			write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_RE, 0);
			write_ssi_mask(&ssi->sier, CCSR_SSI_SIER_RDMAE, 0);
		}
		if ((read_ssi(&ssi->scr) & (CCSR_SSI_SCR_TE | CCSR_SSI_SCR_RE)) == 0)
			write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_SSIEN, 0);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int fsl_ssi_set_dai_fmt(struct snd_soc_dai *cpu_dai, unsigned int fmt)
{
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(cpu_dai);
	struct ccsr_ssi __iomem *ssi = ssi_private->ssi;
	u32 strcr = 0, stcr, srcr, scr, mask;

	scr = read_ssi(&ssi->scr) & ~(CCSR_SSI_SCR_SYN | CCSR_SSI_SCR_NET);

	mask = CCSR_SSI_STCR_TXBIT0 | CCSR_SSI_STCR_TFDIR | CCSR_SSI_STCR_TXDIR
		| CCSR_SSI_STCR_TSCKP | CCSR_SSI_STCR_TFSI | CCSR_SSI_STCR_TFSL
		| CCSR_SSI_STCR_TEFS;
	stcr = read_ssi(&ssi->stcr) & ~mask;
	srcr = read_ssi(&ssi->srcr) & ~mask;

	/* DAI format */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		scr |= CCSR_SSI_SCR_NET;

		/* Pre-set SSI I2S mode */
		switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
		case SND_SOC_DAIFMT_CBS_CFS:
			ssi_private->i2s_mode = CCSR_SSI_SCR_I2S_MODE_MASTER;
			break;
		case SND_SOC_DAIFMT_CBM_CFM:
			ssi_private->i2s_mode = CCSR_SSI_SCR_I2S_MODE_SLAVE;
			break;
		default:
			dev_err(cpu_dai->dev, "unsupported SND_SOC_DAIFMT_MASTER: %d",
					fmt & SND_SOC_DAIFMT_MASTER_MASK);
			return -EINVAL;
		}
		scr &= ~CCSR_SSI_SCR_I2S_MODE_MASK;
		scr |= ssi_private->i2s_mode;

		/* Data on rising edge of bclk, frame low, 1clk before data */
		strcr |= CCSR_SSI_STCR_TFSI | CCSR_SSI_STCR_TSCKP
			| CCSR_SSI_STCR_TXBIT0 | CCSR_SSI_STCR_TEFS;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		/* Data on rising edge of bclk, frame high */
		strcr |= CCSR_SSI_STCR_TXBIT0 | CCSR_SSI_STCR_TSCKP;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		/* Data on rising edge of bclk, frame high, 1clk before data */
		strcr |= CCSR_SSI_STCR_TFSL | CCSR_SSI_STCR_TSCKP
			| CCSR_SSI_STCR_TXBIT0 | CCSR_SSI_STCR_TEFS;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		/* Data on rising edge of bclk, frame high */
		strcr |= CCSR_SSI_STCR_TFSL | CCSR_SSI_STCR_TSCKP
			| CCSR_SSI_STCR_TXBIT0;
		break;
	default:
		dev_err(cpu_dai->dev, "unsupported SND_SOC_DAIFMT: %d",
				fmt & SND_SOC_DAIFMT_FORMAT_MASK);
		return -EINVAL;
	}

	/* DAI clock inversion */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		/* Nothing to do for both normal cases */
		break;
	case SND_SOC_DAIFMT_IB_NF:
		/* Invert bit clock */
		strcr ^= CCSR_SSI_STCR_TSCKP;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		/* Invert frame clock */
		strcr ^= CCSR_SSI_STCR_TFSI;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		/* Invert both clocks */
		strcr ^= CCSR_SSI_STCR_TSCKP;
		strcr ^= CCSR_SSI_STCR_TFSI;
		break;
	default:
		dev_err(cpu_dai->dev, "unsupported SND_SOC_DAIFMT_INV: %d",
				fmt & SND_SOC_DAIFMT_INV_MASK);
		return -EINVAL;
	}

	/* DAI clock master masks */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		scr |= CCSR_SSI_SCR_SYS_CLK_EN;
		strcr |= CCSR_SSI_STCR_TFDIR | CCSR_SSI_STCR_TXDIR;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		scr &= ~CCSR_SSI_SCR_SYS_CLK_EN;
		break;
	default:
		dev_err(cpu_dai->dev, "unsupported SND_SOC_DAIFMT_MASTER: %d",
				fmt & SND_SOC_DAIFMT_MASTER_MASK);
		return -EINVAL;
	}

	stcr |= strcr;
	srcr |= strcr;

	if (ssi_private->cpu_dai_drv.symmetric_rates) {
		scr |= CCSR_SSI_SCR_SYN;

		/* Need to clear RXDIR when using SYNC mode */
		srcr &= ~CCSR_SSI_SRCR_RXDIR;
	}

	write_ssi(stcr, &ssi->stcr);
	write_ssi(srcr, &ssi->srcr);
	write_ssi(scr, &ssi->scr);

	return 0;
}

static int fsl_ssi_set_dai_sysclk(struct snd_soc_dai *cpu_dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(cpu_dai);
	struct ccsr_ssi __iomem *ssi = ssi_private->ssi;
	int synchronous = ssi_private->cpu_dai_drv.symmetric_rates;
	u32 pm = 999, div2, psr, stccr, mask, afreq, factor, i;
	unsigned long clkrate, sysrate = 0;
	u64 sub, savesub = 100000;

	/*
	 * It should be already enough to divide clock by setting pm.
	 * So we here keep psr and div2 to 0.
	 */
	psr = 0;
	div2 = 0;

	factor = (div2 + 1) * (7 * psr + 1) * 2;

	for (i = 0; i < 255; i++) {
		/* The bclk rate must be smaller than 1/5 sysclk rate */
		if (factor * (i + 1) < 5)
			continue;

		sysrate = freq * factor * (i + 2);
		clkrate = clk_round_rate(ssi_private->clk, sysrate);

		do_div(clkrate, factor);
		afreq = (u32)clkrate / (i + 1);

		if (freq == afreq)
			sub = 0;
		else if (freq / afreq == 1)
			sub = freq - afreq;
		else if (afreq / freq == 1)
			sub = afreq - freq;
		else
			continue;

		/* Calculate the fraction */
		sub *= 100000;
		do_div(sub, freq);

		if (sub < savesub) {
			ssi_private->sysrate = sysrate;
			savesub = sub;
			pm = i;
		}

		/* We are lucky */
		if (savesub == 0)
			break;
	}

	/* No proper pm found if it is still remaining the initial value */
	if (pm == 999) {
		dev_err(cpu_dai->dev, "failed to handle the required sysclk\n");
		return -EINVAL;
	}

	stccr = CCSR_SSI_SxCCR_PM(pm + 1) | (div2 ? CCSR_SSI_SxCCR_DIV2 : 0)
		| (psr ? CCSR_SSI_SxCCR_PSR : 0);
	mask = CCSR_SSI_SxCCR_PM_MASK | CCSR_SSI_SxCCR_DIV2_MASK
		| CCSR_SSI_SxCCR_PSR_MASK;

	if (dir == SND_SOC_CLOCK_OUT || synchronous)
		write_ssi_mask(&ssi->stccr, mask, stccr);
	else
		write_ssi_mask(&ssi->srccr, mask, stccr);

	return 0;
}

static int fsl_ssi_set_dai_tdm_slot(struct snd_soc_dai *cpu_dai, u32 tx_mask,
				u32 rx_mask, int slots, int slot_width)
{
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(cpu_dai);
	struct ccsr_ssi __iomem *ssi = ssi_private->ssi;

	write_ssi_mask(&ssi->stccr, CCSR_SSI_SxCCR_DC_MASK,
			CCSR_SSI_SxCCR_DC(slots));
	write_ssi_mask(&ssi->srccr, CCSR_SSI_SxCCR_DC_MASK,
			CCSR_SSI_SxCCR_DC(slots));

	/* The register SxMSKs need SSI to provide essential clock due to
	 * hardware design. So we here temporarily enable SSI to set them.
	 */
	write_ssi_mask(&ssi->scr, 0, CCSR_SSI_SCR_SSIEN);
	write_ssi(tx_mask, &ssi->stmsk);
	write_ssi(rx_mask, &ssi->srmsk);
	write_ssi_mask(&ssi->scr, CCSR_SSI_SCR_SSIEN, 0);

	return 0;
}

/**
 * fsl_ssi_shutdown: shutdown the SSI
 *
 * Shutdown the SSI if there are no other substreams open.
 */
static void fsl_ssi_shutdown(struct snd_pcm_substream *substream,
			     struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(rtd->cpu_dai);

	if (ssi_private->first_stream == substream)
		ssi_private->first_stream = ssi_private->second_stream;

	ssi_private->second_stream = NULL;

	/* If this is the last active substream, disable the interrupts. */
	if (!ssi_private->first_stream) {
		struct ccsr_ssi __iomem *ssi = ssi_private->ssi;

		write_ssi_mask(&ssi->sier, SIER_FLAGS, 0);

		ssi_private->sysrate = 0;
	}

	if (ssi_private->ssi_on_imx) {
		clk_disable_unprepare(ssi_private->clk);
		clk_disable_unprepare(ssi_private->coreclk);

		pm_runtime_put_sync(dai->dev);
	}
}

static int fsl_ssi_dai_probe(struct snd_soc_dai *dai)
{
	struct fsl_ssi_private *ssi_private = snd_soc_dai_get_drvdata(dai);

	if (ssi_private->ssi_on_imx) {
		dai->playback_dma_data = &ssi_private->dma_params_tx;
		dai->capture_dma_data = &ssi_private->dma_params_rx;
	}

	return 0;
}

static const struct snd_soc_dai_ops fsl_ssi_dai_ops = {
	.startup	= fsl_ssi_startup,
	.hw_params	= fsl_ssi_hw_params,
	.set_fmt	= fsl_ssi_set_dai_fmt,
	.set_sysclk	= fsl_ssi_set_dai_sysclk,
	.set_tdm_slot	= fsl_ssi_set_dai_tdm_slot,
	.shutdown	= fsl_ssi_shutdown,
	.trigger	= fsl_ssi_trigger,
};

/* Template for the CPU dai driver structure */
static struct snd_soc_dai_driver fsl_ssi_dai_template = {
	.probe = fsl_ssi_dai_probe,
	.playback = {
		.channels_min = 1,
		.channels_max = 2,
		.rates = FSLSSI_I2S_RATES,
		.formats = FSLSSI_I2S_FORMATS,
	},
	.capture = {
		.channels_min = 1,
		.channels_max = 2,
		.rates = FSLSSI_I2S_RATES,
		.formats = FSLSSI_I2S_FORMATS,
	},
	.ops = &fsl_ssi_dai_ops,
};

static const struct snd_soc_component_driver fsl_ssi_component = {
	.name		= "fsl-ssi",
};

/* Show the statistics of a flag only if its interrupt is enabled.  The
 * compiler will optimze this code to a no-op if the interrupt is not
 * enabled.
 */
#define SIER_SHOW(flag, name) \
	do { \
		if (SIER_FLAGS & CCSR_SSI_SIER_##flag) \
			length += sprintf(buf + length, #name "=%u\n", \
				ssi_private->stats.name); \
	} while (0)


/**
 * fsl_sysfs_ssi_show: display SSI statistics
 *
 * Display the statistics for the current SSI device.  To avoid confusion,
 * we only show those counts that are enabled.
 */
static ssize_t fsl_sysfs_ssi_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct fsl_ssi_private *ssi_private =
		container_of(attr, struct fsl_ssi_private, dev_attr);
	ssize_t length = 0;

	SIER_SHOW(RFRC_EN, rfrc);
	SIER_SHOW(TFRC_EN, tfrc);
	SIER_SHOW(CMDAU_EN, cmdau);
	SIER_SHOW(CMDDU_EN, cmddu);
	SIER_SHOW(RXT_EN, rxt);
	SIER_SHOW(RDR1_EN, rdr1);
	SIER_SHOW(RDR0_EN, rdr0);
	SIER_SHOW(TDE1_EN, tde1);
	SIER_SHOW(TDE0_EN, tde0);
	SIER_SHOW(ROE1_EN, roe1);
	SIER_SHOW(ROE0_EN, roe0);
	SIER_SHOW(TUE1_EN, tue1);
	SIER_SHOW(TUE0_EN, tue0);
	SIER_SHOW(TFS_EN, tfs);
	SIER_SHOW(RFS_EN, rfs);
	SIER_SHOW(TLS_EN, tls);
	SIER_SHOW(RLS_EN, rls);
	SIER_SHOW(RFF1_EN, rff1);
	SIER_SHOW(RFF0_EN, rff0);
	SIER_SHOW(TFE1_EN, tfe1);
	SIER_SHOW(TFE0_EN, tfe0);

	return length;
}

/**
 * Make every character in a string lower-case
 */
static void make_lowercase(char *s)
{
	char *p = s;
	char c;

	while ((c = *p)) {
		if ((c >= 'A') && (c <= 'Z'))
			*p = c + ('a' - 'A');
		p++;
	}
}

static int fsl_ssi_probe(struct platform_device *pdev)
{
	struct fsl_ssi_private *ssi_private;
	int ret = 0;
	struct device_attribute *dev_attr = NULL;
	struct device_node *np = pdev->dev.of_node;
	const char *p, *sprop;
	const uint32_t *iprop;
	struct resource res;
	char name[64];

	/* SSIs that are not connected on the board should have a
	 *      status = "disabled"
	 * property in their device tree nodes.
	 */
	if (!of_device_is_available(np))
		return -ENODEV;

	/* The DAI name is the last part of the full name of the node. */
	p = strrchr(np->full_name, '/') + 1;
	ssi_private = kzalloc(sizeof(struct fsl_ssi_private) + strlen(p),
			      GFP_KERNEL);
	if (!ssi_private) {
		dev_err(&pdev->dev, "could not allocate DAI object\n");
		return -ENOMEM;
	}

	strcpy(ssi_private->name, p);

	/* Initialize this copy of the CPU DAI driver structure */
	memcpy(&ssi_private->cpu_dai_drv, &fsl_ssi_dai_template,
	       sizeof(fsl_ssi_dai_template));
	ssi_private->cpu_dai_drv.name = ssi_private->name;

	/* Get the addresses and IRQ */
	ret = of_address_to_resource(np, 0, &res);
	if (ret) {
		dev_err(&pdev->dev, "could not determine device resources\n");
		goto error_kmalloc;
	}
	ssi_private->ssi = of_iomap(np, 0);
	if (!ssi_private->ssi) {
		dev_err(&pdev->dev, "could not map device resources\n");
		ret = -ENOMEM;
		goto error_kmalloc;
	}
	ssi_private->ssi_phys = res.start;

	ssi_private->irq = irq_of_parse_and_map(np, 0);
	if (ssi_private->irq == NO_IRQ) {
		dev_err(&pdev->dev, "no irq for node %s\n", np->full_name);
		ret = -ENXIO;
		goto error_iomap;
	}

	/* The 'name' should not have any slashes in it. */
	ret = request_irq(ssi_private->irq, fsl_ssi_isr, 0, ssi_private->name,
			  ssi_private);
	if (ret < 0) {
		dev_err(&pdev->dev, "could not claim irq %u\n", ssi_private->irq);
		goto error_irqmap;
	}

	/* Are the RX and the TX clocks locked? */
	if (!of_find_property(np, "fsl,ssi-asynchronous", NULL))
		ssi_private->cpu_dai_drv.symmetric_rates = 1;

	/* Determine the FIFO depth. */
	iprop = of_get_property(np, "fsl,fifo-depth", NULL);
	if (iprop)
		ssi_private->fifo_depth = be32_to_cpup(iprop);
	else
                /* Older 8610 DTs didn't have the fifo-depth property */
		ssi_private->fifo_depth = 8;

	if (of_device_is_compatible(pdev->dev.of_node, "fsl,imx21-ssi")) {
		ssi_private->ssi_on_imx = true;

		ssi_private->coreclk = devm_clk_get(&pdev->dev, "ipg");
		if (IS_ERR(ssi_private->coreclk)) {
			ret = PTR_ERR(ssi_private->coreclk);
			dev_err(&pdev->dev, "could not get ipg clock: %d\n", ret);
			goto error_irq;
		}
		ssi_private->clk = devm_clk_get(&pdev->dev, "baud");
		if (IS_ERR(ssi_private->clk)) {
			ret = PTR_ERR(ssi_private->clk);
			dev_err(&pdev->dev, "could not get baud clock: %d\n", ret);
			goto error_irq;
		}

		/*
		 * We have burstsize be "fifo_depth - 2" to match the SSI
		 * watermark setting in fsl_ssi_startup().
		 */
		ssi_private->dma_params_tx.maxburst =
			ssi_private->fifo_depth - 2;
		ssi_private->dma_params_rx.maxburst =
			ssi_private->fifo_depth - 2;
		ssi_private->dma_params_tx.addr =
			ssi_private->ssi_phys + offsetof(struct ccsr_ssi, stx0);
		ssi_private->dma_params_rx.addr =
			ssi_private->ssi_phys + offsetof(struct ccsr_ssi, srx0);
	}

	ssi_private->sysrate = 0;

	/* Initialize the the device_attribute structure */
	dev_attr = &ssi_private->dev_attr;
	sysfs_attr_init(&dev_attr->attr);
	dev_attr->attr.name = "statistics";
	dev_attr->attr.mode = S_IRUGO;
	dev_attr->show = fsl_sysfs_ssi_show;

	ret = device_create_file(&pdev->dev, dev_attr);
	if (ret) {
		dev_err(&pdev->dev, "could not create sysfs %s file\n",
			ssi_private->dev_attr.attr.name);
		goto error_irq;
	}

	pm_runtime_enable(&pdev->dev);

	/* Register with ASoC */
	dev_set_drvdata(&pdev->dev, ssi_private);

	ret = snd_soc_register_component(&pdev->dev, &fsl_ssi_component,
					 &ssi_private->cpu_dai_drv, 1);
	if (ret) {
		dev_err(&pdev->dev, "failed to register DAI: %d\n", ret);
		goto error_dev;
	}

	if (ssi_private->ssi_on_imx) {
		ret = imx_pcm_dma_init(pdev, SND_DMAENGINE_PCM_FLAG_NO_RESIDUE,
					IMX_SSI_DMABUF_SIZE);
		if (ret)
			goto error_dev;
	}

	/* Enable dual fifo feature as default */
	ssi_private->use_dual_fifo = true;

	/*
	 * If codec-handle property is missing from SSI node, we assume
	 * that the machine driver uses new binding which does not require
	 * SSI driver to trigger machine driver's probe.
	 */
	if (!of_get_property(np, "codec-handle", NULL)) {
		ssi_private->new_binding = true;
		goto done;
	}

	/* Trigger the machine driver's probe function.  The platform driver
	 * name of the machine driver is taken from /compatible property of the
	 * device tree.  We also pass the address of the CPU DAI driver
	 * structure.
	 */
	sprop = of_get_property(of_find_node_by_path("/"), "compatible", NULL);
	/* Sometimes the compatible name has a "fsl," prefix, so we strip it. */
	p = strrchr(sprop, ',');
	if (p)
		sprop = p + 1;
	snprintf(name, sizeof(name), "snd-soc-%s", sprop);
	make_lowercase(name);

	ssi_private->pdev =
		platform_device_register_data(&pdev->dev, name, 0, NULL, 0);
	if (IS_ERR(ssi_private->pdev)) {
		ret = PTR_ERR(ssi_private->pdev);
		dev_err(&pdev->dev, "failed to register platform: %d\n", ret);
		goto error_dai;
	}

done:
	return 0;

error_dai:
	if (ssi_private->ssi_on_imx)
		imx_pcm_dma_exit(pdev);
	snd_soc_unregister_component(&pdev->dev);

error_dev:
	dev_set_drvdata(&pdev->dev, NULL);
	device_remove_file(&pdev->dev, dev_attr);

error_irq:
	free_irq(ssi_private->irq, ssi_private);

error_irqmap:
	irq_dispose_mapping(ssi_private->irq);

error_iomap:
	iounmap(ssi_private->ssi);

error_kmalloc:
	kfree(ssi_private);

	return ret;
}

static int fsl_ssi_remove(struct platform_device *pdev)
{
	struct fsl_ssi_private *ssi_private = dev_get_drvdata(&pdev->dev);

	if (!ssi_private->new_binding)
		platform_device_unregister(ssi_private->pdev);
	if (ssi_private->ssi_on_imx)
		imx_pcm_dma_exit(pdev);

	snd_soc_unregister_component(&pdev->dev);
	device_remove_file(&pdev->dev, &ssi_private->dev_attr);

	free_irq(ssi_private->irq, ssi_private);
	irq_dispose_mapping(ssi_private->irq);

	kfree(ssi_private);
	dev_set_drvdata(&pdev->dev, NULL);

	return 0;
}

#ifdef CONFIG_PM_RUNTIME
static int fsl_ssi_runtime_resume(struct device *dev)
{
	request_bus_freq(BUS_FREQ_AUDIO);
	return 0;
}

static int fsl_ssi_runtime_suspend(struct device *dev)
{
	release_bus_freq(BUS_FREQ_AUDIO);
	return 0;
}
#endif

static const struct dev_pm_ops fsl_ssi_pm = {
	SET_RUNTIME_PM_OPS(fsl_ssi_runtime_suspend,
			fsl_ssi_runtime_resume,
			NULL)
};

static const struct of_device_id fsl_ssi_ids[] = {
	{ .compatible = "fsl,mpc8610-ssi", },
	{ .compatible = "fsl,imx21-ssi", },
	{}
};
MODULE_DEVICE_TABLE(of, fsl_ssi_ids);

static struct platform_driver fsl_ssi_driver = {
	.driver = {
		.name = "fsl-ssi-dai",
		.owner = THIS_MODULE,
		.of_match_table = fsl_ssi_ids,
		.pm = &fsl_ssi_pm,
	},
	.probe = fsl_ssi_probe,
	.remove = fsl_ssi_remove,
};

module_platform_driver(fsl_ssi_driver);

MODULE_ALIAS("platform:fsl-ssi-dai");
MODULE_AUTHOR("Timur Tabi <timur@freescale.com>");
MODULE_DESCRIPTION("Freescale Synchronous Serial Interface (SSI) ASoC Driver");
MODULE_LICENSE("GPL v2");
