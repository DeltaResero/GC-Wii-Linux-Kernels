/*
 * sound/ppc/gcn-ai.c
 *
 * Nintendo GameCube audio interface driver
 * Copyright (C) 2004-2007 The GameCube Linux Team
 * Copyright (C) 2007 Albert Herranz
 *
 * Based on work from mist, kirin, groepaz, Steve_-, isobel and others.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 */

/* #define AI_DEBUG */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <asm/io.h>
#include <asm/cacheflush.h>
#include <sound/core.h>
#include <sound/pcm.h>
#define SNDRV_GET_ID
#include <sound/initval.h>

#ifdef AI_DEBUG
#  define DPRINTK(fmt, args...) \
          printk(KERN_ERR "%s: " fmt, __FUNCTION__ , ## args)
#else
#  define DPRINTK(fmt, args...)
#endif

#define DRV_MODULE_NAME  "gcn-ai"
#define DRV_DESCRIPTION  "Nintendo GameCube Audio Interface driver"
#define DRV_AUTHOR       "me!"

MODULE_DESCRIPTION(DRV_DESCRIPTION);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_LICENSE("GPL");

#define PFX DRV_MODULE_NAME ": "
#define ai_printk(level, format, arg...) \
        printk(level PFX format , ## arg)


#define DSP_IRQ 6

#define AI_DSP_CSR          (void __iomem*)0xCC00500A
#define  AI_CSR_RES         (1<<0)
#define  AI_CSR_PIINT       (1<<1)
#define  AI_CSR_HALT        (1<<2)
#define  AI_CSR_AIDINT      (1<<3)
#define  AI_CSR_AIDINTMASK  (1<<4)
#define  AI_CSR_ARINT       (1<<5)
#define  AI_CSR_ARINTMASK   (1<<6)
#define  AI_CSR_DSPINT      (1<<7)
#define  AI_CSR_DSPINTMASK  (1<<8)
#define  AI_CSR_DSPDMA      (1<<9)
#define  AI_CSR_RESETXXX    (1<<11)
#define AUDIO_IRQ_CAUSE     *(volatile u_int16_t *)(0xCC005010)

#define AUDIO_DMA_STARTH    *(volatile u_int16_t *)(0xCC005030)
#define AUDIO_DMA_STARTL    *(volatile u_int16_t *)(0xCC005032)

#define AUDIO_DMA_LENGTH    *(volatile u_int16_t *)(0xCC005036)
#define  AI_DCL_PLAY        (1<<15)

#define AUDIO_DMA_LEFT      *(volatile u_int16_t *)(0xCC00503A)
#define AUDIO_STREAM_STATUS *(volatile u_int32_t *)(0xCC006C00)
#define  AI_AICR_RATE       (1<<6)

#define LoadSample(addr, len) \
	  AUDIO_DMA_STARTH = (addr >> 16) & 0xffff; \
	  AUDIO_DMA_STARTL = addr & 0xffff; \
	  AUDIO_DMA_LENGTH = (AUDIO_DMA_LENGTH & AI_DCL_PLAY) | (len >> 5)
#define StartSample()  AUDIO_DMA_LENGTH |= AI_DCL_PLAY
#define StopSample()   AUDIO_DMA_LENGTH &= ~AI_DCL_PLAY
#define SetFreq32KHz() AUDIO_STREAM_STATUS |= AI_AICR_RATE
#define SetFreq48KHz() AUDIO_STREAM_STATUS &= ~AI_AICR_RATE

static int index = SNDRV_DEFAULT_IDX1;	/* Index 0-MAX */
static char *id = SNDRV_DEFAULT_STR1;	/* ID for this card */

struct snd_gcn {
	struct snd_card *card;
	struct snd_pcm *pcm;
	struct snd_pcm_substream *playback_substream;
	struct snd_pcm_substream *capture_substream;
	spinlock_t reg_lock;
	int dma_size;
	int period_size;
	int nperiods;
	volatile int cur_period;
	volatile int start_play;
	volatile int stop_play;
};

static struct snd_gcn *gcn_audio = NULL;

static struct snd_pcm_hardware snd_gcn_playback = {
	.info = (SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_INTERLEAVED |
		 SNDRV_PCM_INFO_BLOCK_TRANSFER | SNDRV_PCM_INFO_MMAP_VALID),
	.formats = SNDRV_PCM_FMTBIT_S16_BE,
	.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_48000,
	.rate_min = 32000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = 32768,
	.period_bytes_min = 32,
	.period_bytes_max = 32768,
	.periods_min = 1,
	.periods_max = 1024,
};

static int snd_gcn_open(struct snd_pcm_substream * substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	DPRINTK("pcm open\n");
	chip->playback_substream = substream;
	runtime->hw = snd_gcn_playback;

	/* align to 32 bytes */
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_BUFFER_BYTES,
				   32);
	snd_pcm_hw_constraint_step(runtime, 0, SNDRV_PCM_HW_PARAM_PERIOD_BYTES,
				   32);

	return 0;
}

static int snd_gcn_close(struct snd_pcm_substream * substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);

	DPRINTK("pcm close\n");
	chip->playback_substream = NULL;
	return 0;
}

static int snd_gcn_hw_params(struct snd_pcm_substream * substream,
				  struct snd_pcm_hw_params * hw_params)
{
	DPRINTK("snd_gcn_hw_params\n");
	return snd_pcm_lib_malloc_pages(substream,
					params_buffer_bytes(hw_params));
}

static int snd_gcn_hw_free(struct snd_pcm_substream * substream)
{
	DPRINTK("snd_gcn_hw_free\n");
	return snd_pcm_lib_free_pages(substream);
}

static int snd_gcn_prepare(struct snd_pcm_substream * substream)
{
	/* struct snd_gcn *chip = snd_pcm_substream_chip(substream); */
	struct snd_pcm_runtime *runtime = substream->runtime;

	DPRINTK("snd_gcn_prepare\n");
	DPRINTK("prepare: rate=%i, channels=%i, sample_bits=%i\n",
		runtime->rate, runtime->channels, runtime->sample_bits);
	DPRINTK("prepare: format=%i, access=%i\n",
		runtime->format, runtime->access);

	/* set requested samplerate */
	switch (runtime->rate) {
	case 32000:
		SetFreq32KHz();
		break;
	case 48000:
		SetFreq48KHz();
		break;
	default:
		DPRINTK("unsupported rate: %i!\n", runtime->rate);
		return -EINVAL;
	}

	return 0;
}

static int snd_gcn_trigger(struct snd_pcm_substream * substream, int cmd)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;

	DPRINTK("snd_gcn_trigger\n");
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		/* do something to start the PCM engine */
		DPRINTK("PCM_TRIGGER_START\n");
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
			chip->dma_size = snd_pcm_lib_buffer_bytes(substream);
			chip->period_size = snd_pcm_lib_period_bytes(substream);
			chip->nperiods = chip->dma_size / chip->period_size;
			chip->cur_period = 0;
			chip->stop_play = 0;
			chip->start_play = 1;

			DPRINTK("stream is PCM_PLAYBACK,"
				" dma_area=0x%p dma_size=%i\n",
				runtime->dma_area, chip->dma_size);
			DPRINTK("%i periods of %i bytes\n", chip->nperiods,
				chip->period_size);

			flush_dcache_range((unsigned long)runtime->dma_area,
					   (unsigned long)(runtime->dma_area +
							   chip->period_size));
			LoadSample((u_int32_t) runtime->dma_area,
				   chip->period_size);
			StartSample();
		}
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		/* do something to stop the PCM engine */
		DPRINTK("PCM_TRIGGER_STOP\n");

		chip->stop_play = 1;
		/* StopSample(); */
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static snd_pcm_uframes_t snd_gcn_pointer(struct snd_pcm_substream * substream)
{
	struct snd_gcn *chip = snd_pcm_substream_chip(substream);
	struct snd_pcm_runtime *runtime = substream->runtime;
	int left, bytes;

	DPRINTK("snd_gcn_pointer\n");
	left = AUDIO_DMA_LEFT << 5;
	bytes = chip->period_size * (chip->cur_period + 1);
	/* bytes = snd_pcm_lib_buffer_bytes(substream); */

	DPRINTK("pointer: %i of %i(%i) bytes left, period #%i\n", left,
		chip->period_size, bytes, chip->cur_period);

	return bytes_to_frames(runtime, bytes - left);
}

static irqreturn_t snd_gcn_interrupt(int irq, void *dev)
{
	struct snd_gcn *chip = dev;
	unsigned long flags;
	u16 tmp;

	if (in_be16(AI_DSP_CSR) & AI_CSR_AIDINT) {
		u_int32_t addr;

		DPRINTK("DSP interrupt! period #%i\n", chip->cur_period);

		if (chip->start_play) {
			chip->start_play = 0;
		} else if (chip->stop_play) {
			StopSample();
		} else {
			StopSample();

			if (chip->cur_period < (chip->nperiods - 1)) {
				chip->cur_period++;
			} else
				chip->cur_period = 0;

			addr =
			    (u_int32_t) chip->playback_substream->runtime->
			    dma_area + (chip->cur_period * chip->period_size);

			flush_dcache_range(addr, addr + chip->period_size);
			LoadSample(addr, chip->period_size);

			StartSample();
			/* chip->start_play = 1; */

			snd_pcm_period_elapsed(chip->playback_substream);
		}
		/* ack AI DMA interrupt, go through lengths to only ack
		   the audio part */
		local_irq_save(flags);
		tmp = in_be16(AI_DSP_CSR);
		tmp &= ~(AI_CSR_PIINT | AI_CSR_ARINT | AI_CSR_DSPINT);
		out_be16(AI_DSP_CSR, tmp);
		local_irq_restore(flags);
		
		return IRQ_HANDLED;
	}

	return IRQ_NONE;
}

static struct snd_pcm_ops snd_gcn_playback_ops = {
	.open = snd_gcn_open,
	.close = snd_gcn_close,
	.ioctl = snd_pcm_lib_ioctl,
	.hw_params = snd_gcn_hw_params,
	.hw_free = snd_gcn_hw_free,
	.prepare = snd_gcn_prepare,
	.trigger = snd_gcn_trigger,
	.pointer = snd_gcn_pointer,
};

static int __devinit snd_gcn_new_pcm(struct snd_gcn * chip)
{
	struct snd_pcm *pcm;
	int err;

	if ((err =
	     snd_pcm_new(chip->card, chip->card->shortname, 0, 1, 0, &pcm)) < 0)
		return err;

	snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_PLAYBACK,
			&snd_gcn_playback_ops);
	/* snd_pcm_set_ops(pcm, SNDRV_PCM_STREAM_CAPTURE, &snd_gcn_capture_ops); */

	/* preallocate 64k buffer */
	snd_pcm_lib_preallocate_pages_for_all(pcm, SNDRV_DMA_TYPE_CONTINUOUS,
					      snd_dma_continuous_data
					      (GFP_KERNEL), 64 * 1024,
					      64 * 1024);

	pcm->info_flags = 0;
	pcm->private_data = chip;
	strcpy(pcm->name, chip->card->shortname);

	chip->pcm = pcm;

	return 0;
}

static int __init alsa_card_gcn_init(void)
{
	int err;
	struct snd_card *card;
	unsigned long flags;
/*	if (!is_gamecube())
		return -ENODEV; */

	/* register the soundcard */
	card = snd_card_new(index, id, THIS_MODULE, sizeof(struct snd_gcn));
	if (card == NULL)
		return -ENOMEM;

	gcn_audio = (struct snd_gcn *) card->private_data;
	if (gcn_audio == NULL)
		return -ENOMEM;

	memset(gcn_audio, 0, sizeof(struct snd_gcn));
	gcn_audio->card = card;
	gcn_audio->stop_play = 1;

	strcpy(card->driver, "gcn-ai");
	strcpy(card->shortname, card->driver);
	sprintf(card->longname, "Nintendo GameCube Audio Interface");

	if (request_irq(DSP_IRQ, snd_gcn_interrupt,
			IRQF_DISABLED | IRQF_SHARED, 
			card->shortname,gcn_audio)) {
		snd_printk(KERN_ERR "%s: unable to grab IRQ %d\n",
			   card->shortname, DSP_IRQ);
		return -EBUSY;
	} else {

		/* enable AI DMA and DSP interrupt */
		local_irq_save(flags);
		out_be16(AI_DSP_CSR,
			 in_be16(AI_DSP_CSR) | AI_CSR_AIDINTMASK | AI_CSR_PIINT);
		local_irq_restore(flags);
	}

#if 0
	if (request_region(AUDIO_INTERFACE_ADDR, 0x200, card->shortname) ==
	    NULL) {
		printk("unable to grab memory region 0x%lx-0x%lx\n",
		       AUDIO_INTERFACE_ADDR, AUDIO_INTERFACE_ADDR + 0x200 - 1);
		return -EBUSY;
	}

	if ((iobase = (unsigned long)ioremap(AUDIO_INTERFACE_ADDR, 0x200)) == 0) {
		printk("unable to remap memory region 0x%lx-0x%lx\n",
		       AUDIO_INTERFACE_ADDR, AUDIO_INTERFACE_ADDR + 0x200 - 1);
		return -ENOMEM;
	}

	printk("iobase=0x%lx\n", iobase);
#endif

#if 0
	/* mixer */
	if ((err = snd_gcn_mixer_new(gcn_audio)) < 0)
		goto fail;
#endif
	/* PCM */
	if ((err = snd_gcn_new_pcm(gcn_audio)) < 0)
		goto fail;

	if ((err = snd_card_register(card)) == 0) {
		ai_printk(KERN_INFO, "%s initialized\n", DRV_DESCRIPTION);
		return 0;
	}

      fail:
	snd_card_free(card);
	return err;
}

static void __exit alsa_card_gcn_exit(void)
{
	unsigned long flags;
	DPRINTK("Goodbye, cruel world\n");

	StopSample();
	/* disable interrupts */
	local_irq_save(flags);
	out_be16(AI_DSP_CSR, in_be16(AI_DSP_CSR) & ~AI_CSR_AIDINTMASK);
	local_irq_restore(flags);
	
	free_irq(DSP_IRQ, gcn_audio);
	snd_card_free(gcn_audio->card);
}

module_init(alsa_card_gcn_init);
module_exit(alsa_card_gcn_exit);
