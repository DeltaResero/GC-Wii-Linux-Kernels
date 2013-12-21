/*
 * Universal Interface for Intel High Definition Audio Codec
 *
 * HD audio interface patch for ALC 260/880/882 codecs
 *
 * Copyright (c) 2004 Kailang Yang <kailang@realtek.com.tw>
 *                    PeiSen Hou <pshou@realtek.com.tw>
 *                    Takashi Iwai <tiwai@suse.de>
 *
 *  This driver is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This driver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <sound/core.h>
#include "hda_codec.h"
#include "hda_local.h"


/* ALC880 board config type */
enum {
	ALC880_3ST,
	ALC880_3ST_DIG,
	ALC880_5ST,
	ALC880_5ST_DIG,
	ALC880_W810,
	ALC880_Z71V,
	ALC880_6ST,
	ALC880_6ST_DIG,
	ALC880_F1734,
	ALC880_ASUS,
	ALC880_ASUS_DIG,
	ALC880_ASUS_W1V,
	ALC880_ASUS_DIG2,
	ALC880_UNIWILL_DIG,
	ALC880_CLEVO,
	ALC880_TCL_S700,
#ifdef CONFIG_SND_DEBUG
	ALC880_TEST,
#endif
	ALC880_AUTO,
	ALC880_MODEL_LAST /* last tag */
};

/* ALC260 models */
enum {
	ALC260_BASIC,
	ALC260_HP,
	ALC260_HP_3013,
	ALC260_FUJITSU_S702X,
	ALC260_AUTO,
	ALC260_MODEL_LAST /* last tag */
};

/* ALC262 models */
enum {
	ALC262_BASIC,
	ALC262_AUTO,
	ALC262_MODEL_LAST /* last tag */
};

/* ALC861 models */
enum {
	ALC861_3ST,
	ALC861_3ST_DIG,
	ALC861_6ST_DIG,
	ALC861_AUTO,
	ALC861_MODEL_LAST,
};

/* ALC882 models */
enum {
	ALC882_3ST_DIG,
	ALC882_6ST_DIG,
	ALC882_AUTO,
	ALC882_MODEL_LAST,
};

/* for GPIO Poll */
#define GPIO_MASK	0x03

struct alc_spec {
	/* codec parameterization */
	struct snd_kcontrol_new *mixers[5];	/* mixer arrays */
	unsigned int num_mixers;

	const struct hda_verb *init_verbs[5];	/* initialization verbs
						 * don't forget NULL termination!
						 */
	unsigned int num_init_verbs;

	char *stream_name_analog;	/* analog PCM stream */
	struct hda_pcm_stream *stream_analog_playback;
	struct hda_pcm_stream *stream_analog_capture;

	char *stream_name_digital;	/* digital PCM stream */ 
	struct hda_pcm_stream *stream_digital_playback;
	struct hda_pcm_stream *stream_digital_capture;

	/* playback */
	struct hda_multi_out multiout;	/* playback set-up
					 * max_channels, dacs must be set
					 * dig_out_nid and hp_nid are optional
					 */

	/* capture */
	unsigned int num_adc_nids;
	hda_nid_t *adc_nids;
	hda_nid_t dig_in_nid;		/* digital-in NID; optional */

	/* capture source */
	const struct hda_input_mux *input_mux;
	unsigned int cur_mux[3];

	/* channel model */
	const struct hda_channel_mode *channel_mode;
	int num_channel_mode;

	/* PCM information */
	struct hda_pcm pcm_rec[2];	/* used in alc_build_pcms() */

	/* dynamic controls, init_verbs and input_mux */
	struct auto_pin_cfg autocfg;
	unsigned int num_kctl_alloc, num_kctl_used;
	struct snd_kcontrol_new *kctl_alloc;
	struct hda_input_mux private_imux;
	hda_nid_t private_dac_nids[5];
};

/*
 * configuration template - to be copied to the spec instance
 */
struct alc_config_preset {
	struct snd_kcontrol_new *mixers[5]; /* should be identical size with spec */
	const struct hda_verb *init_verbs[5];
	unsigned int num_dacs;
	hda_nid_t *dac_nids;
	hda_nid_t dig_out_nid;		/* optional */
	hda_nid_t hp_nid;		/* optional */
	unsigned int num_adc_nids;
	hda_nid_t *adc_nids;
	hda_nid_t dig_in_nid;
	unsigned int num_channel_mode;
	const struct hda_channel_mode *channel_mode;
	const struct hda_input_mux *input_mux;
};


/*
 * input MUX handling
 */
static int alc_mux_enum_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	return snd_hda_input_mux_info(spec->input_mux, uinfo);
}

static int alc_mux_enum_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	unsigned int adc_idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);

	ucontrol->value.enumerated.item[0] = spec->cur_mux[adc_idx];
	return 0;
}

static int alc_mux_enum_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	unsigned int adc_idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	return snd_hda_input_mux_put(codec, spec->input_mux, ucontrol,
				     spec->adc_nids[adc_idx], &spec->cur_mux[adc_idx]);
}


/*
 * channel mode setting
 */
static int alc_ch_mode_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	return snd_hda_ch_mode_info(codec, uinfo, spec->channel_mode,
				    spec->num_channel_mode);
}

static int alc_ch_mode_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	return snd_hda_ch_mode_get(codec, ucontrol, spec->channel_mode,
				   spec->num_channel_mode, spec->multiout.max_channels);
}

static int alc_ch_mode_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	return snd_hda_ch_mode_put(codec, ucontrol, spec->channel_mode,
				   spec->num_channel_mode, &spec->multiout.max_channels);
}


/*
 * Control of pin widget settings via the mixer.  Only boolean settings are
 * supported, so VrefEn can't be controlled using these functions as they
 * stand.
 */
static int alc_pinctl_switch_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_BOOLEAN;
	uinfo->count = 1;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = 1;
	return 0;
}

static int alc_pinctl_switch_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = kcontrol->private_value & 0xffff;
	long mask = (kcontrol->private_value >> 16) & 0xff;
	long *valp = ucontrol->value.integer.value;

	*valp = 0;
	if (snd_hda_codec_read(codec,nid,0,AC_VERB_GET_PIN_WIDGET_CONTROL,0x00) & mask)
		*valp = 1;
	return 0;
}

static int alc_pinctl_switch_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = kcontrol->private_value & 0xffff;
	long mask = (kcontrol->private_value >> 16) & 0xff;
	long *valp = ucontrol->value.integer.value;
	unsigned int pinctl = snd_hda_codec_read(codec,nid,0,AC_VERB_GET_PIN_WIDGET_CONTROL,0x00);
	int change = ((pinctl & mask)!=0) != *valp;

	if (change)
		snd_hda_codec_write(codec,nid,0,AC_VERB_SET_PIN_WIDGET_CONTROL,
			*valp?(pinctl|mask):(pinctl&~mask));
	return change;
}

#define ALC_PINCTL_SWITCH(xname, nid, mask) \
	{ .iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = xname, .index = 0,  \
	  .info = alc_pinctl_switch_info, \
	  .get = alc_pinctl_switch_get, \
	  .put = alc_pinctl_switch_put, \
	  .private_value = (nid) | (mask<<16) }


/*
 * set up from the preset table
 */
static void setup_preset(struct alc_spec *spec, const struct alc_config_preset *preset)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(preset->mixers) && preset->mixers[i]; i++)
		spec->mixers[spec->num_mixers++] = preset->mixers[i];
	for (i = 0; i < ARRAY_SIZE(preset->init_verbs) && preset->init_verbs[i]; i++)
		spec->init_verbs[spec->num_init_verbs++] = preset->init_verbs[i];
	
	spec->channel_mode = preset->channel_mode;
	spec->num_channel_mode = preset->num_channel_mode;

	spec->multiout.max_channels = spec->channel_mode[0].channels;

	spec->multiout.num_dacs = preset->num_dacs;
	spec->multiout.dac_nids = preset->dac_nids;
	spec->multiout.dig_out_nid = preset->dig_out_nid;
	spec->multiout.hp_nid = preset->hp_nid;
	
	spec->input_mux = preset->input_mux;

	spec->num_adc_nids = preset->num_adc_nids;
	spec->adc_nids = preset->adc_nids;
	spec->dig_in_nid = preset->dig_in_nid;
}

/*
 * ALC880 3-stack model
 *
 * DAC: Front = 0x02 (0x0c), Surr = 0x05 (0x0f), CLFE = 0x04 (0x0e)
 * Pin assignment: Front = 0x14, Line-In/Surr = 0x1a, Mic/CLFE = 0x18, F-Mic = 0x1b
 *                 HP = 0x19
 */

static hda_nid_t alc880_dac_nids[4] = {
	/* front, rear, clfe, rear_surr */
	0x02, 0x05, 0x04, 0x03
};

static hda_nid_t alc880_adc_nids[3] = {
	/* ADC0-2 */
	0x07, 0x08, 0x09,
};

/* The datasheet says the node 0x07 is connected from inputs,
 * but it shows zero connection in the real implementation on some devices.
 * Note: this is a 915GAV bug, fixed on 915GLV
 */
static hda_nid_t alc880_adc_nids_alt[2] = {
	/* ADC1-2 */
	0x08, 0x09,
};

#define ALC880_DIGOUT_NID	0x06
#define ALC880_DIGIN_NID	0x0a

static struct hda_input_mux alc880_capture_source = {
	.num_items = 4,
	.items = {
		{ "Mic", 0x0 },
		{ "Front Mic", 0x3 },
		{ "Line", 0x2 },
		{ "CD", 0x4 },
	},
};

/* channel source setting (2/6 channel selection for 3-stack) */
/* 2ch mode */
static struct hda_verb alc880_threestack_ch2_init[] = {
	/* set line-in to input, mute it */
	{ 0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN },
	{ 0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE },
	/* set mic-in to input vref 80%, mute it */
	{ 0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80 },
	{ 0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE },
	{ } /* end */
};

/* 6ch mode */
static struct hda_verb alc880_threestack_ch6_init[] = {
	/* set line-in to output, unmute it */
	{ 0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE },
	/* set mic-in to output, unmute it */
	{ 0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE },
	{ } /* end */
};

static struct hda_channel_mode alc880_threestack_modes[2] = {
	{ 2, alc880_threestack_ch2_init },
	{ 6, alc880_threestack_ch6_init },
};

static struct snd_kcontrol_new alc880_three_stack_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Surround Playback Volume", 0x0f, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Surround Playback Switch", 0x0f, 2, HDA_INPUT),
	HDA_CODEC_VOLUME_MONO("Center Playback Volume", 0x0e, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("LFE Playback Volume", 0x0e, 2, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE_MONO("Center Playback Switch", 0x0e, 1, 2, HDA_INPUT),
	HDA_BIND_MUTE_MONO("LFE Playback Switch", 0x0e, 2, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Front Mic Playback Volume", 0x0b, 0x3, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x0b, 0x3, HDA_INPUT),
	HDA_CODEC_VOLUME("PC Speaker Playback Volume", 0x0b, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("PC Speaker Playback Switch", 0x0b, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x19, 0x0, HDA_OUTPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Channel Mode",
		.info = alc_ch_mode_info,
		.get = alc_ch_mode_get,
		.put = alc_ch_mode_put,
	},
	{ } /* end */
};

/* capture mixer elements */
static struct snd_kcontrol_new alc880_capture_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 1, 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 1, 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 2, 0x09, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 2, 0x09, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 3,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};

/* capture mixer elements (in case NID 0x07 not available) */
static struct snd_kcontrol_new alc880_capture_alt_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 1, 0x09, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 1, 0x09, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 2,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};



/*
 * ALC880 5-stack model
 *
 * DAC: Front = 0x02 (0x0c), Surr = 0x05 (0x0f), CLFE = 0x04 (0x0d), Side = 0x02 (0xd)
 * Pin assignment: Front = 0x14, Surr = 0x17, CLFE = 0x16
 *                 Line-In/Side = 0x1a, Mic = 0x18, F-Mic = 0x1b, HP = 0x19
 */

/* additional mixers to alc880_three_stack_mixer */
static struct snd_kcontrol_new alc880_five_stack_mixer[] = {
	HDA_CODEC_VOLUME("Side Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Side Playback Switch", 0x0d, 2, HDA_INPUT),
	{ } /* end */
};

/* channel source setting (6/8 channel selection for 5-stack) */
/* 6ch mode */
static struct hda_verb alc880_fivestack_ch6_init[] = {
	/* set line-in to input, mute it */
	{ 0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN },
	{ 0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE },
	{ } /* end */
};

/* 8ch mode */
static struct hda_verb alc880_fivestack_ch8_init[] = {
	/* set line-in to output, unmute it */
	{ 0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE },
	{ } /* end */
};

static struct hda_channel_mode alc880_fivestack_modes[2] = {
	{ 6, alc880_fivestack_ch6_init },
	{ 8, alc880_fivestack_ch8_init },
};


/*
 * ALC880 6-stack model
 *
 * DAC: Front = 0x02 (0x0c), Surr = 0x03 (0x0d), CLFE = 0x04 (0x0e), Side = 0x05 (0x0f)
 * Pin assignment: Front = 0x14, Surr = 0x15, CLFE = 0x16, Side = 0x17,
 *   Mic = 0x18, F-Mic = 0x19, Line = 0x1a, HP = 0x1b
 */

static hda_nid_t alc880_6st_dac_nids[4] = {
	/* front, rear, clfe, rear_surr */
	0x02, 0x03, 0x04, 0x05
};	

static struct hda_input_mux alc880_6stack_capture_source = {
	.num_items = 4,
	.items = {
		{ "Mic", 0x0 },
		{ "Front Mic", 0x1 },
		{ "Line", 0x2 },
		{ "CD", 0x4 },
	},
};

/* fixed 8-channels */
static struct hda_channel_mode alc880_sixstack_modes[1] = {
	{ 8, NULL },
};

static struct snd_kcontrol_new alc880_six_stack_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Surround Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Surround Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_CODEC_VOLUME_MONO("Center Playback Volume", 0x0e, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("LFE Playback Volume", 0x0e, 2, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE_MONO("Center Playback Switch", 0x0e, 1, 2, HDA_INPUT),
	HDA_BIND_MUTE_MONO("LFE Playback Switch", 0x0e, 2, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Side Playback Volume", 0x0f, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Side Playback Switch", 0x0f, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Front Mic Playback Volume", 0x0b, 0x1, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x0b, 0x1, HDA_INPUT),
	HDA_CODEC_VOLUME("PC Speaker Playback Volume", 0x0b, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("PC Speaker Playback Switch", 0x0b, 0x05, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Channel Mode",
		.info = alc_ch_mode_info,
		.get = alc_ch_mode_get,
		.put = alc_ch_mode_put,
	},
	{ } /* end */
};


/*
 * ALC880 W810 model
 *
 * W810 has rear IO for:
 * Front (DAC 02)
 * Surround (DAC 03)
 * Center/LFE (DAC 04)
 * Digital out (06)
 *
 * The system also has a pair of internal speakers, and a headphone jack.
 * These are both connected to Line2 on the codec, hence to DAC 02.
 * 
 * There is a variable resistor to control the speaker or headphone
 * volume. This is a hardware-only device without a software API.
 *
 * Plugging headphones in will disable the internal speakers. This is
 * implemented in hardware, not via the driver using jack sense. In
 * a similar fashion, plugging into the rear socket marked "front" will
 * disable both the speakers and headphones.
 *
 * For input, there's a microphone jack, and an "audio in" jack.
 * These may not do anything useful with this driver yet, because I
 * haven't setup any initialization verbs for these yet...
 */

static hda_nid_t alc880_w810_dac_nids[3] = {
	/* front, rear/surround, clfe */
	0x02, 0x03, 0x04
};

/* fixed 6 channels */
static struct hda_channel_mode alc880_w810_modes[1] = {
	{ 6, NULL }
};

/* Pin assignment: Front = 0x14, Surr = 0x15, CLFE = 0x16, HP = 0x1b */
static struct snd_kcontrol_new alc880_w810_base_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Surround Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Surround Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_CODEC_VOLUME_MONO("Center Playback Volume", 0x0e, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("LFE Playback Volume", 0x0e, 2, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE_MONO("Center Playback Switch", 0x0e, 1, 2, HDA_INPUT),
	HDA_BIND_MUTE_MONO("LFE Playback Switch", 0x0e, 2, 2, HDA_INPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x1b, 0x0, HDA_OUTPUT),
	{ } /* end */
};


/*
 * Z710V model
 *
 * DAC: Front = 0x02 (0x0c), HP = 0x03 (0x0d)
 * Pin assignment: Front = 0x14, HP = 0x15, Mic = 0x18, Mic2 = 0x19(?), Line = 0x1a
 */

static hda_nid_t alc880_z71v_dac_nids[1] = {
	0x02
};
#define ALC880_Z71V_HP_DAC	0x03

/* fixed 2 channels */
static struct hda_channel_mode alc880_2_jack_modes[1] = {
	{ 2, NULL }
};

static struct snd_kcontrol_new alc880_z71v_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Headphone Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Headphone Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	{ } /* end */
};


/* FIXME! */
/*
 * ALC880 F1734 model
 *
 * DAC: HP = 0x02 (0x0c), Front = 0x03 (0x0d)
 * Pin assignment: HP = 0x14, Front = 0x15, Mic = 0x18
 */

static hda_nid_t alc880_f1734_dac_nids[1] = {
	0x03
};
#define ALC880_F1734_HP_DAC	0x02

static struct snd_kcontrol_new alc880_f1734_mixer[] = {
	HDA_CODEC_VOLUME("Headphone Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Headphone Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Internal Speaker Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Internal Speaker Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	{ } /* end */
};


/* FIXME! */
/*
 * ALC880 ASUS model
 *
 * DAC: HP/Front = 0x02 (0x0c), Surr = 0x03 (0x0d), CLFE = 0x04 (0x0e)
 * Pin assignment: HP/Front = 0x14, Surr = 0x15, CLFE = 0x16,
 *  Mic = 0x18, Line = 0x1a
 */

#define alc880_asus_dac_nids	alc880_w810_dac_nids	/* identical with w810 */
#define alc880_asus_modes	alc880_threestack_modes	/* 2/6 channel mode */

static struct snd_kcontrol_new alc880_asus_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Surround Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Surround Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_CODEC_VOLUME_MONO("Center Playback Volume", 0x0e, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("LFE Playback Volume", 0x0e, 2, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE_MONO("Center Playback Switch", 0x0e, 1, 2, HDA_INPUT),
	HDA_BIND_MUTE_MONO("LFE Playback Switch", 0x0e, 2, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Channel Mode",
		.info = alc_ch_mode_info,
		.get = alc_ch_mode_get,
		.put = alc_ch_mode_put,
	},
	{ } /* end */
};

/* FIXME! */
/*
 * ALC880 ASUS W1V model
 *
 * DAC: HP/Front = 0x02 (0x0c), Surr = 0x03 (0x0d), CLFE = 0x04 (0x0e)
 * Pin assignment: HP/Front = 0x14, Surr = 0x15, CLFE = 0x16,
 *  Mic = 0x18, Line = 0x1a, Line2 = 0x1b
 */

/* additional mixers to alc880_asus_mixer */
static struct snd_kcontrol_new alc880_asus_w1v_mixer[] = {
	HDA_CODEC_VOLUME("Line2 Playback Volume", 0x0b, 0x03, HDA_INPUT),
	HDA_CODEC_MUTE("Line2 Playback Switch", 0x0b, 0x03, HDA_INPUT),
	{ } /* end */
};

/* additional mixers to alc880_asus_mixer */
static struct snd_kcontrol_new alc880_pcbeep_mixer[] = {
	HDA_CODEC_VOLUME("PC Speaker Playback Volume", 0x0b, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("PC Speaker Playback Switch", 0x0b, 0x05, HDA_INPUT),
	{ } /* end */
};

/* TCL S700 */
static struct snd_kcontrol_new alc880_tcl_s700_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Front Playback Switch", 0x1b, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x14, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0B, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0B, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0B, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0B, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 1,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};

/*
 * build control elements
 */
static int alc_build_controls(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int err;
	int i;

	for (i = 0; i < spec->num_mixers; i++) {
		err = snd_hda_add_new_ctls(codec, spec->mixers[i]);
		if (err < 0)
			return err;
	}

	if (spec->multiout.dig_out_nid) {
		err = snd_hda_create_spdif_out_ctls(codec, spec->multiout.dig_out_nid);
		if (err < 0)
			return err;
	}
	if (spec->dig_in_nid) {
		err = snd_hda_create_spdif_in_ctls(codec, spec->dig_in_nid);
		if (err < 0)
			return err;
	}
	return 0;
}


/*
 * initialize the codec volumes, etc
 */

/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc880_volume_init_verbs[] = {
	/*
	 * Unmute ADC0-2 and set the default input to mic-in
	 */
	{0x07, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x09, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},

	/* Unmute input amps (CD, Line In, Mic 1 & Mic 2) of the analog-loopback
	 * mixer widget
	 * Note: PASD motherboards uses the Line In 2 as the input for front panel
	 * mic (mic 2)
	 */
	/* Amp Indices: Mic1 = 0, Mic2 = 1, Line1 = 2, Line2 = 3, CD = 4 */
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(4)},

	/*
	 * Set up output mixers (0x0c - 0x0f)
	 */
	/* set vol=0 to output mixers */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* set up input amps for analog loopback */
	/* Amp Indices: DAC = 0, mixer = 1 */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},

	{ }
};

/*
 * 3-stack pin configuration:
 * front = 0x14, mic/clfe = 0x18, HP = 0x19, line/surr = 0x1a, f-mic = 0x1b
 */
static struct hda_verb alc880_pin_3stack_init_verbs[] = {
	/*
	 * preset connection lists of input pins
	 * 0 = front, 1 = rear_surr, 2 = CLFE, 3 = surround
	 */
	{0x10, AC_VERB_SET_CONNECT_SEL, 0x02}, /* mic/clfe */
	{0x11, AC_VERB_SET_CONNECT_SEL, 0x00}, /* HP */
	{0x12, AC_VERB_SET_CONNECT_SEL, 0x03}, /* line/surround */

	/*
	 * Set pin mode and muting
	 */
	/* set front pin widgets 0x14 for output */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Mic1 (rear panel) pin widget for input and vref at 80% */
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Mic2 (as headphone out) for HP output */
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Line In pin widget for input */
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Line2 (as front mic) pin widget for input and vref at 80% */
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* CD pin widget for input */
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},

	{ }
};

/*
 * 5-stack pin configuration:
 * front = 0x14, surround = 0x17, clfe = 0x16, mic = 0x18, HP = 0x19,
 * line-in/side = 0x1a, f-mic = 0x1b
 */
static struct hda_verb alc880_pin_5stack_init_verbs[] = {
	/*
	 * preset connection lists of input pins
	 * 0 = front, 1 = rear_surr, 2 = CLFE, 3 = surround
	 */
	{0x11, AC_VERB_SET_CONNECT_SEL, 0x00}, /* HP */
	{0x12, AC_VERB_SET_CONNECT_SEL, 0x01}, /* line/side */

	/*
	 * Set pin mode and muting
	 */
	/* set pin widgets 0x14-0x17 for output */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	/* unmute pins for output (no gain on this amp) */
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},

	/* Mic1 (rear panel) pin widget for input and vref at 80% */
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Mic2 (as headphone out) for HP output */
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Line In pin widget for input */
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Line2 (as front mic) pin widget for input and vref at 80% */
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* CD pin widget for input */
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},

	{ }
};

/*
 * W810 pin configuration:
 * front = 0x14, surround = 0x15, clfe = 0x16, HP = 0x1b
 */
static struct hda_verb alc880_pin_w810_init_verbs[] = {
	/* hphone/speaker input selector: front DAC */
	{0x13, AC_VERB_SET_CONNECT_SEL, 0x0},

	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},

	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},

	{ }
};

/*
 * Z71V pin configuration:
 * Speaker-out = 0x14, HP = 0x15, Mic = 0x18, Line-in = 0x1a, Mic2 = 0x1b (?)
 */
static struct hda_verb alc880_pin_z71v_init_verbs[] = {
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},

	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},

	{ }
};

/*
 * 6-stack pin configuration:
 * front = 0x14, surr = 0x15, clfe = 0x16, side = 0x17, mic = 0x18, f-mic = 0x19,
 * line = 0x1a, HP = 0x1b
 */
static struct hda_verb alc880_pin_6stack_init_verbs[] = {
	{0x13, AC_VERB_SET_CONNECT_SEL, 0x00}, /* HP */

	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},

	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	
	{ }
};

/* FIXME! */
/*
 * F1734 pin configuration:
 * HP = 0x14, speaker-out = 0x15, mic = 0x18
 */
static struct hda_verb alc880_pin_f1734_init_verbs[] = {
	{0x10, AC_VERB_SET_CONNECT_SEL, 0x02},
	{0x11, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x12, AC_VERB_SET_CONNECT_SEL, 0x01},
	{0x13, AC_VERB_SET_CONNECT_SEL, 0x00},

	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},

	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},

	{ }
};

/* FIXME! */
/*
 * ASUS pin configuration:
 * HP/front = 0x14, surr = 0x15, clfe = 0x16, mic = 0x18, line = 0x1a
 */
static struct hda_verb alc880_pin_asus_init_verbs[] = {
	{0x10, AC_VERB_SET_CONNECT_SEL, 0x02},
	{0x11, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x12, AC_VERB_SET_CONNECT_SEL, 0x01},
	{0x13, AC_VERB_SET_CONNECT_SEL, 0x00},

	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},

	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	
	{ }
};

/* Enable GPIO mask and set output */
static struct hda_verb alc880_gpio1_init_verbs[] = {
	{0x01, AC_VERB_SET_GPIO_MASK, 0x01},
	{0x01, AC_VERB_SET_GPIO_DIRECTION, 0x01},
	{0x01, AC_VERB_SET_GPIO_DATA, 0x01},

	{ }
};

/* Enable GPIO mask and set output */
static struct hda_verb alc880_gpio2_init_verbs[] = {
	{0x01, AC_VERB_SET_GPIO_MASK, 0x02},
	{0x01, AC_VERB_SET_GPIO_DIRECTION, 0x02},
	{0x01, AC_VERB_SET_GPIO_DATA, 0x02},

	{ }
};

/* Clevo m520g init */
static struct hda_verb alc880_pin_clevo_init_verbs[] = {
	/* headphone output */
	{0x11, AC_VERB_SET_CONNECT_SEL, 0x01},
	/* line-out */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Line-in */
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* CD */
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Mic1 (rear panel) */
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Mic2 (front panel) */
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* headphone */
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
        /* change to EAPD mode */
	{0x20, AC_VERB_SET_COEF_INDEX, 0x07},
	{0x20, AC_VERB_SET_PROC_COEF,  0x3060},

	{ }
};

static struct hda_verb alc880_pin_tcl_S700_init_verbs[] = {
	/* Headphone output */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	/* Front output*/
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x1b, AC_VERB_SET_CONNECT_SEL, 0x00},

	/* Line In pin widget for input */
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	/* CD pin widget for input */
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	/* Mic1 (rear panel) pin widget for input and vref at 80% */
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},

	/* change to EAPD mode */
	{0x20, AC_VERB_SET_COEF_INDEX, 0x07},
	{0x20, AC_VERB_SET_PROC_COEF,  0x3070},

	{ }
};

/*
 */

static int alc_init(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	unsigned int i;

	for (i = 0; i < spec->num_init_verbs; i++)
		snd_hda_sequence_write(codec, spec->init_verbs[i]);
	return 0;
}

#ifdef CONFIG_PM
/*
 * resume
 */
static int alc_resume(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	alc_init(codec);
	for (i = 0; i < spec->num_mixers; i++)
		snd_hda_resume_ctls(codec, spec->mixers[i]);
	if (spec->multiout.dig_out_nid)
		snd_hda_resume_spdif_out(codec);
	if (spec->dig_in_nid)
		snd_hda_resume_spdif_in(codec);

	return 0;
}
#endif

/*
 * Analog playback callbacks
 */
static int alc880_playback_pcm_open(struct hda_pcm_stream *hinfo,
				    struct hda_codec *codec,
				    struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;
	return snd_hda_multi_out_analog_open(codec, &spec->multiout, substream);
}

static int alc880_playback_pcm_prepare(struct hda_pcm_stream *hinfo,
				       struct hda_codec *codec,
				       unsigned int stream_tag,
				       unsigned int format,
				       struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;
	return snd_hda_multi_out_analog_prepare(codec, &spec->multiout, stream_tag,
						format, substream);
}

static int alc880_playback_pcm_cleanup(struct hda_pcm_stream *hinfo,
				       struct hda_codec *codec,
				       struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;
	return snd_hda_multi_out_analog_cleanup(codec, &spec->multiout);
}

/*
 * Digital out
 */
static int alc880_dig_playback_pcm_open(struct hda_pcm_stream *hinfo,
					struct hda_codec *codec,
					struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;
	return snd_hda_multi_out_dig_open(codec, &spec->multiout);
}

static int alc880_dig_playback_pcm_close(struct hda_pcm_stream *hinfo,
					 struct hda_codec *codec,
					 struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;
	return snd_hda_multi_out_dig_close(codec, &spec->multiout);
}

/*
 * Analog capture
 */
static int alc880_capture_pcm_prepare(struct hda_pcm_stream *hinfo,
				      struct hda_codec *codec,
				      unsigned int stream_tag,
				      unsigned int format,
				      struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;

	snd_hda_codec_setup_stream(codec, spec->adc_nids[substream->number],
				   stream_tag, 0, format);
	return 0;
}

static int alc880_capture_pcm_cleanup(struct hda_pcm_stream *hinfo,
				      struct hda_codec *codec,
				      struct snd_pcm_substream *substream)
{
	struct alc_spec *spec = codec->spec;

	snd_hda_codec_setup_stream(codec, spec->adc_nids[substream->number], 0, 0, 0);
	return 0;
}


/*
 */
static struct hda_pcm_stream alc880_pcm_analog_playback = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 8,
	/* NID is set in alc_build_pcms */
	.ops = {
		.open = alc880_playback_pcm_open,
		.prepare = alc880_playback_pcm_prepare,
		.cleanup = alc880_playback_pcm_cleanup
	},
};

static struct hda_pcm_stream alc880_pcm_analog_capture = {
	.substreams = 2,
	.channels_min = 2,
	.channels_max = 2,
	/* NID is set in alc_build_pcms */
	.ops = {
		.prepare = alc880_capture_pcm_prepare,
		.cleanup = alc880_capture_pcm_cleanup
	},
};

static struct hda_pcm_stream alc880_pcm_digital_playback = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 2,
	/* NID is set in alc_build_pcms */
	.ops = {
		.open = alc880_dig_playback_pcm_open,
		.close = alc880_dig_playback_pcm_close
	},
};

static struct hda_pcm_stream alc880_pcm_digital_capture = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 2,
	/* NID is set in alc_build_pcms */
};

static int alc_build_pcms(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	struct hda_pcm *info = spec->pcm_rec;
	int i;

	codec->num_pcms = 1;
	codec->pcm_info = info;

	info->name = spec->stream_name_analog;
	if (spec->stream_analog_playback) {
		snd_assert(spec->multiout.dac_nids, return -EINVAL);
		info->stream[SNDRV_PCM_STREAM_PLAYBACK] = *(spec->stream_analog_playback);
		info->stream[SNDRV_PCM_STREAM_PLAYBACK].nid = spec->multiout.dac_nids[0];
	}
	if (spec->stream_analog_capture) {
		snd_assert(spec->adc_nids, return -EINVAL);
		info->stream[SNDRV_PCM_STREAM_CAPTURE] = *(spec->stream_analog_capture);
		info->stream[SNDRV_PCM_STREAM_CAPTURE].nid = spec->adc_nids[0];
	}

	if (spec->channel_mode) {
		info->stream[SNDRV_PCM_STREAM_PLAYBACK].channels_max = 0;
		for (i = 0; i < spec->num_channel_mode; i++) {
			if (spec->channel_mode[i].channels > info->stream[SNDRV_PCM_STREAM_PLAYBACK].channels_max) {
				info->stream[SNDRV_PCM_STREAM_PLAYBACK].channels_max = spec->channel_mode[i].channels;
			}
		}
	}

	if (spec->multiout.dig_out_nid || spec->dig_in_nid) {
		codec->num_pcms++;
		info++;
		info->name = spec->stream_name_digital;
		if (spec->multiout.dig_out_nid &&
		    spec->stream_digital_playback) {
			info->stream[SNDRV_PCM_STREAM_PLAYBACK] = *(spec->stream_digital_playback);
			info->stream[SNDRV_PCM_STREAM_PLAYBACK].nid = spec->multiout.dig_out_nid;
		}
		if (spec->dig_in_nid &&
		    spec->stream_digital_capture) {
			info->stream[SNDRV_PCM_STREAM_CAPTURE] = *(spec->stream_digital_capture);
			info->stream[SNDRV_PCM_STREAM_CAPTURE].nid = spec->dig_in_nid;
		}
	}

	return 0;
}

static void alc_free(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	unsigned int i;

	if (! spec)
		return;

	if (spec->kctl_alloc) {
		for (i = 0; i < spec->num_kctl_used; i++)
			kfree(spec->kctl_alloc[i].name);
		kfree(spec->kctl_alloc);
	}
	kfree(spec);
}

/*
 */
static struct hda_codec_ops alc_patch_ops = {
	.build_controls = alc_build_controls,
	.build_pcms = alc_build_pcms,
	.init = alc_init,
	.free = alc_free,
#ifdef CONFIG_PM
	.resume = alc_resume,
#endif
};


/*
 * Test configuration for debugging
 *
 * Almost all inputs/outputs are enabled.  I/O pins can be configured via
 * enum controls.
 */
#ifdef CONFIG_SND_DEBUG
static hda_nid_t alc880_test_dac_nids[4] = {
	0x02, 0x03, 0x04, 0x05
};

static struct hda_input_mux alc880_test_capture_source = {
	.num_items = 5,
	.items = {
		{ "In-1", 0x0 },
		{ "In-2", 0x1 },
		{ "In-3", 0x2 },
		{ "In-4", 0x3 },
		{ "CD", 0x4 },
	},
};

static struct hda_channel_mode alc880_test_modes[4] = {
	{ 2, NULL },
	{ 4, NULL },
	{ 6, NULL },
	{ 8, NULL },
};

static int alc_test_pin_ctl_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	static char *texts[] = {
		"N/A", "Line Out", "HP Out",
		"In Hi-Z", "In 50%", "In Grd", "In 80%", "In 100%"
	};
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 8;
	if (uinfo->value.enumerated.item >= 8)
		uinfo->value.enumerated.item = 7;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int alc_test_pin_ctl_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = (hda_nid_t)kcontrol->private_value;
	unsigned int pin_ctl, item = 0;

	pin_ctl = snd_hda_codec_read(codec, nid, 0,
				     AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	if (pin_ctl & AC_PINCTL_OUT_EN) {
		if (pin_ctl & AC_PINCTL_HP_EN)
			item = 2;
		else
			item = 1;
	} else if (pin_ctl & AC_PINCTL_IN_EN) {
		switch (pin_ctl & AC_PINCTL_VREFEN) {
		case AC_PINCTL_VREF_HIZ: item = 3; break;
		case AC_PINCTL_VREF_50:  item = 4; break;
		case AC_PINCTL_VREF_GRD: item = 5; break;
		case AC_PINCTL_VREF_80:  item = 6; break;
		case AC_PINCTL_VREF_100: item = 7; break;
		}
	}
	ucontrol->value.enumerated.item[0] = item;
	return 0;
}

static int alc_test_pin_ctl_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = (hda_nid_t)kcontrol->private_value;
	static unsigned int ctls[] = {
		0, AC_PINCTL_OUT_EN, AC_PINCTL_OUT_EN | AC_PINCTL_HP_EN,
		AC_PINCTL_IN_EN | AC_PINCTL_VREF_HIZ,
		AC_PINCTL_IN_EN | AC_PINCTL_VREF_50,
		AC_PINCTL_IN_EN | AC_PINCTL_VREF_GRD,
		AC_PINCTL_IN_EN | AC_PINCTL_VREF_80,
		AC_PINCTL_IN_EN | AC_PINCTL_VREF_100,
	};
	unsigned int old_ctl, new_ctl;

	old_ctl = snd_hda_codec_read(codec, nid, 0,
				     AC_VERB_GET_PIN_WIDGET_CONTROL, 0);
	new_ctl = ctls[ucontrol->value.enumerated.item[0]];
	if (old_ctl != new_ctl) {
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL, new_ctl);
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE,
				    ucontrol->value.enumerated.item[0] >= 3 ? 0xb080 : 0xb000);
		return 1;
	}
	return 0;
}

static int alc_test_pin_src_info(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_info *uinfo)
{
	static char *texts[] = {
		"Front", "Surround", "CLFE", "Side"
	};
	uinfo->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	uinfo->count = 1;
	uinfo->value.enumerated.items = 4;
	if (uinfo->value.enumerated.item >= 4)
		uinfo->value.enumerated.item = 3;
	strcpy(uinfo->value.enumerated.name, texts[uinfo->value.enumerated.item]);
	return 0;
}

static int alc_test_pin_src_get(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = (hda_nid_t)kcontrol->private_value;
	unsigned int sel;

	sel = snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_CONNECT_SEL, 0);
	ucontrol->value.enumerated.item[0] = sel & 3;
	return 0;
}

static int alc_test_pin_src_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	hda_nid_t nid = (hda_nid_t)kcontrol->private_value;
	unsigned int sel;

	sel = snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_CONNECT_SEL, 0) & 3;
	if (ucontrol->value.enumerated.item[0] != sel) {
		sel = ucontrol->value.enumerated.item[0] & 3;
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_CONNECT_SEL, sel);
		return 1;
	}
	return 0;
}

#define PIN_CTL_TEST(xname,nid) {			\
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,	\
			.name = xname,		       \
			.info = alc_test_pin_ctl_info, \
			.get = alc_test_pin_ctl_get,   \
			.put = alc_test_pin_ctl_put,   \
			.private_value = nid	       \
			}

#define PIN_SRC_TEST(xname,nid) {			\
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,	\
			.name = xname,		       \
			.info = alc_test_pin_src_info, \
			.get = alc_test_pin_src_get,   \
			.put = alc_test_pin_src_put,   \
			.private_value = nid	       \
			}

static struct snd_kcontrol_new alc880_test_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("Surround Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("CLFE Playback Volume", 0x0e, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("Side Playback Volume", 0x0f, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_BIND_MUTE("Surround Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_BIND_MUTE("CLFE Playback Switch", 0x0e, 2, HDA_INPUT),
	HDA_BIND_MUTE("Side Playback Switch", 0x0f, 2, HDA_INPUT),
	PIN_CTL_TEST("Front Pin Mode", 0x14),
	PIN_CTL_TEST("Surround Pin Mode", 0x15),
	PIN_CTL_TEST("CLFE Pin Mode", 0x16),
	PIN_CTL_TEST("Side Pin Mode", 0x17),
	PIN_CTL_TEST("In-1 Pin Mode", 0x18),
	PIN_CTL_TEST("In-2 Pin Mode", 0x19),
	PIN_CTL_TEST("In-3 Pin Mode", 0x1a),
	PIN_CTL_TEST("In-4 Pin Mode", 0x1b),
	PIN_SRC_TEST("In-1 Pin Source", 0x18),
	PIN_SRC_TEST("In-2 Pin Source", 0x19),
	PIN_SRC_TEST("In-3 Pin Source", 0x1a),
	PIN_SRC_TEST("In-4 Pin Source", 0x1b),
	HDA_CODEC_VOLUME("In-1 Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("In-1 Playback Switch", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("In-2 Playback Volume", 0x0b, 0x1, HDA_INPUT),
	HDA_CODEC_MUTE("In-2 Playback Switch", 0x0b, 0x1, HDA_INPUT),
	HDA_CODEC_VOLUME("In-3 Playback Volume", 0x0b, 0x2, HDA_INPUT),
	HDA_CODEC_MUTE("In-3 Playback Switch", 0x0b, 0x2, HDA_INPUT),
	HDA_CODEC_VOLUME("In-4 Playback Volume", 0x0b, 0x3, HDA_INPUT),
	HDA_CODEC_MUTE("In-4 Playback Switch", 0x0b, 0x3, HDA_INPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x4, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x4, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Channel Mode",
		.info = alc_ch_mode_info,
		.get = alc_ch_mode_get,
		.put = alc_ch_mode_put,
	},
	{ } /* end */
};

static struct hda_verb alc880_test_init_verbs[] = {
	/* Unmute inputs of 0x0c - 0x0f */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	/* Vol output for 0x0c-0x0f */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* Set output pins 0x14-0x17 */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	/* Unmute output pins 0x14-0x17 */
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Set input pins 0x18-0x1c */
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	/* Mute input pins 0x18-0x1b */
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* ADC set up */
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x07, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x09, AC_VERB_SET_CONNECT_SEL, 0x00},
	/* Analog input/passthru */
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(2)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(3)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(4)},
	{ }
};
#endif

/*
 */

static struct hda_board_config alc880_cfg_tbl[] = {
	/* Back 3 jack, front 2 jack */
	{ .modelname = "3stack", .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe200, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe201, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe202, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe203, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe204, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe205, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe206, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe207, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe208, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe209, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe20a, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe20b, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe20c, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe20d, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe20e, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe20f, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe210, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe211, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe214, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe302, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe303, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe304, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe306, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe307, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe404, .config = ALC880_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xa101, .config = ALC880_3ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x3031, .config = ALC880_3ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x4036, .config = ALC880_3ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x4037, .config = ALC880_3ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x4038, .config = ALC880_3ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x4040, .config = ALC880_3ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x4041, .config = ALC880_3ST },
	/* TCL S700 */
	{ .pci_subvendor = 0x19db, .pci_subdevice = 0x4188, .config = ALC880_TCL_S700 },

	/* Back 3 jack, front 2 jack (Internal add Aux-In) */
	{ .pci_subvendor = 0x1025, .pci_subdevice = 0xe310, .config = ALC880_3ST },
	{ .pci_subvendor = 0x104d, .pci_subdevice = 0x81d6, .config = ALC880_3ST }, 
	{ .pci_subvendor = 0x104d, .pci_subdevice = 0x81a0, .config = ALC880_3ST },

	/* Back 3 jack plus 1 SPDIF out jack, front 2 jack */
	{ .modelname = "3stack-digout", .config = ALC880_3ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe308, .config = ALC880_3ST_DIG },
	{ .pci_subvendor = 0x1025, .pci_subdevice = 0x0070, .config = ALC880_3ST_DIG },
	/* Clevo m520G NB */
	{ .pci_subvendor = 0x1558, .pci_subdevice = 0x0520, .config = ALC880_CLEVO },

	/* Back 3 jack plus 1 SPDIF out jack, front 2 jack (Internal add Aux-In)*/
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe305, .config = ALC880_3ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xd402, .config = ALC880_3ST_DIG },
	{ .pci_subvendor = 0x1025, .pci_subdevice = 0xe309, .config = ALC880_3ST_DIG },

	/* Back 5 jack, front 2 jack */
	{ .modelname = "5stack", .config = ALC880_5ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x3033, .config = ALC880_5ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x4039, .config = ALC880_5ST },
	{ .pci_subvendor = 0x107b, .pci_subdevice = 0x3032, .config = ALC880_5ST },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x2a09, .config = ALC880_5ST },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x814e, .config = ALC880_5ST },

	/* Back 5 jack plus 1 SPDIF out jack, front 2 jack */
	{ .modelname = "5stack-digout", .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe224, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe400, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe401, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xe402, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xd400, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xd401, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xa100, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x1565, .pci_subdevice = 0x8202, .config = ALC880_5ST_DIG },
	{ .pci_subvendor = 0x1019, .pci_subdevice = 0xa880, .config = ALC880_5ST_DIG },
	/* { .pci_subvendor = 0x1019, .pci_subdevice = 0xa884, .config = ALC880_5ST_DIG }, */ /* conflict with 6stack */
	{ .pci_subvendor = 0x1695, .pci_subdevice = 0x400d, .config = ALC880_5ST_DIG },
	/* note subvendor = 0 below */
	/* { .pci_subvendor = 0x0000, .pci_subdevice = 0x8086, .config = ALC880_5ST_DIG }, */

	{ .modelname = "w810", .config = ALC880_W810 },
	{ .pci_subvendor = 0x161f, .pci_subdevice = 0x203d, .config = ALC880_W810 },

	{ .modelname = "z71v", .config = ALC880_Z71V },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1964, .config = ALC880_Z71V },

	{ .modelname = "6stack", .config = ALC880_6ST },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x8196, .config = ALC880_6ST }, /* ASUS P5GD1-HVM */
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x81b4, .config = ALC880_6ST },
	{ .pci_subvendor = 0x1019, .pci_subdevice = 0xa884, .config = ALC880_6ST }, /* Acer APFV */
	{ .pci_subvendor = 0x1458, .pci_subdevice = 0xa102, .config = ALC880_6ST }, /* Gigabyte K8N51 */

	{ .modelname = "6stack-digout", .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x2668, .pci_subdevice = 0x8086, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0x2668, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x1462, .pci_subdevice = 0x1150, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0xe803, .pci_subdevice = 0x1019, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x1039, .pci_subdevice = 0x1234, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x1025, .pci_subdevice = 0x0077, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x1025, .pci_subdevice = 0x0078, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x1025, .pci_subdevice = 0x0087, .config = ALC880_6ST_DIG },
	{ .pci_subvendor = 0x1297, .pci_subdevice = 0xc790, .config = ALC880_6ST_DIG }, /* Shuttle ST20G5 */

	{ .modelname = "asus", .config = ALC880_ASUS },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1964, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1973, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x19b3, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1113, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1173, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1993, .config = ALC880_ASUS },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x10c3, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1133, .config = ALC880_ASUS },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1123, .config = ALC880_ASUS_DIG },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x1143, .config = ALC880_ASUS },
	{ .pci_subvendor = 0x1043, .pci_subdevice = 0x10b3, .config = ALC880_ASUS_W1V },
	{ .pci_subvendor = 0x1558, .pci_subdevice = 0x5401, .config = ALC880_ASUS_DIG2 },

	{ .modelname = "uniwill", .config = ALC880_UNIWILL_DIG },
	{ .pci_subvendor = 0x1584, .pci_subdevice = 0x9050, .config = ALC880_UNIWILL_DIG },	

	{ .modelname = "F1734", .config = ALC880_F1734 },
	{ .pci_subvendor = 0x1734, .pci_subdevice = 0x107c, .config = ALC880_F1734 },
	{ .pci_subvendor = 0x1584, .pci_subdevice = 0x9054, .config = ALC880_F1734 },

#ifdef CONFIG_SND_DEBUG
	{ .modelname = "test", .config = ALC880_TEST },
#endif
	{ .modelname = "auto", .config = ALC880_AUTO },

	{}
};

/*
 * ALC880 codec presets
 */
static struct alc_config_preset alc880_presets[] = {
	[ALC880_3ST] = {
		.mixers = { alc880_three_stack_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_3stack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_dac_nids),
		.dac_nids = alc880_dac_nids,
		.num_channel_mode = ARRAY_SIZE(alc880_threestack_modes),
		.channel_mode = alc880_threestack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_3ST_DIG] = {
		.mixers = { alc880_three_stack_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_3stack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_dac_nids),
		.dac_nids = alc880_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_threestack_modes),
		.channel_mode = alc880_threestack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_TCL_S700] = {
		.mixers = { alc880_tcl_s700_mixer },
		.init_verbs = { alc880_volume_init_verbs,
				alc880_pin_tcl_S700_init_verbs,
				alc880_gpio2_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_dac_nids),
		.dac_nids = alc880_dac_nids,
		.hp_nid = 0x03,
		.num_channel_mode = ARRAY_SIZE(alc880_2_jack_modes),
		.channel_mode = alc880_2_jack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_5ST] = {
		.mixers = { alc880_three_stack_mixer, alc880_five_stack_mixer},
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_5stack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_dac_nids),
		.dac_nids = alc880_dac_nids,
		.num_channel_mode = ARRAY_SIZE(alc880_fivestack_modes),
		.channel_mode = alc880_fivestack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_5ST_DIG] = {
		.mixers = { alc880_three_stack_mixer, alc880_five_stack_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_5stack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_dac_nids),
		.dac_nids = alc880_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_fivestack_modes),
		.channel_mode = alc880_fivestack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_6ST] = {
		.mixers = { alc880_six_stack_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_6stack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_6st_dac_nids),
		.dac_nids = alc880_6st_dac_nids,
		.num_channel_mode = ARRAY_SIZE(alc880_sixstack_modes),
		.channel_mode = alc880_sixstack_modes,
		.input_mux = &alc880_6stack_capture_source,
	},
	[ALC880_6ST_DIG] = {
		.mixers = { alc880_six_stack_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_6stack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_6st_dac_nids),
		.dac_nids = alc880_6st_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_sixstack_modes),
		.channel_mode = alc880_sixstack_modes,
		.input_mux = &alc880_6stack_capture_source,
	},
	[ALC880_W810] = {
		.mixers = { alc880_w810_base_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_w810_init_verbs,
				alc880_gpio2_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_w810_dac_nids),
		.dac_nids = alc880_w810_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_w810_modes),
		.channel_mode = alc880_w810_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_Z71V] = {
		.mixers = { alc880_z71v_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_z71v_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_z71v_dac_nids),
		.dac_nids = alc880_z71v_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.hp_nid = 0x03,
		.num_channel_mode = ARRAY_SIZE(alc880_2_jack_modes),
		.channel_mode = alc880_2_jack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_F1734] = {
		.mixers = { alc880_f1734_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_f1734_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_f1734_dac_nids),
		.dac_nids = alc880_f1734_dac_nids,
		.hp_nid = 0x02,
		.num_channel_mode = ARRAY_SIZE(alc880_2_jack_modes),
		.channel_mode = alc880_2_jack_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_ASUS] = {
		.mixers = { alc880_asus_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_asus_init_verbs,
				alc880_gpio1_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_asus_dac_nids),
		.dac_nids = alc880_asus_dac_nids,
		.num_channel_mode = ARRAY_SIZE(alc880_asus_modes),
		.channel_mode = alc880_asus_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_ASUS_DIG] = {
		.mixers = { alc880_asus_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_asus_init_verbs,
				alc880_gpio1_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_asus_dac_nids),
		.dac_nids = alc880_asus_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_asus_modes),
		.channel_mode = alc880_asus_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_ASUS_DIG2] = {
		.mixers = { alc880_asus_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_asus_init_verbs,
				alc880_gpio2_init_verbs }, /* use GPIO2 */
		.num_dacs = ARRAY_SIZE(alc880_asus_dac_nids),
		.dac_nids = alc880_asus_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_asus_modes),
		.channel_mode = alc880_asus_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_ASUS_W1V] = {
		.mixers = { alc880_asus_mixer, alc880_asus_w1v_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_asus_init_verbs,
				alc880_gpio1_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_asus_dac_nids),
		.dac_nids = alc880_asus_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_asus_modes),
		.channel_mode = alc880_asus_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_UNIWILL_DIG] = {
		.mixers = { alc880_asus_mixer, alc880_pcbeep_mixer },
		.init_verbs = { alc880_volume_init_verbs, alc880_pin_asus_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_asus_dac_nids),
		.dac_nids = alc880_asus_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_asus_modes),
		.channel_mode = alc880_asus_modes,
		.input_mux = &alc880_capture_source,
	},
	[ALC880_CLEVO] = {
		.mixers = { alc880_three_stack_mixer },
		.init_verbs = { alc880_volume_init_verbs,
				alc880_pin_clevo_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_dac_nids),
		.dac_nids = alc880_dac_nids,
		.hp_nid = 0x03,
		.num_channel_mode = ARRAY_SIZE(alc880_threestack_modes),
		.channel_mode = alc880_threestack_modes,
		.input_mux = &alc880_capture_source,
	},
#ifdef CONFIG_SND_DEBUG
	[ALC880_TEST] = {
		.mixers = { alc880_test_mixer },
		.init_verbs = { alc880_test_init_verbs },
		.num_dacs = ARRAY_SIZE(alc880_test_dac_nids),
		.dac_nids = alc880_test_dac_nids,
		.dig_out_nid = ALC880_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc880_test_modes),
		.channel_mode = alc880_test_modes,
		.input_mux = &alc880_test_capture_source,
	},
#endif
};

/*
 * Automatic parse of I/O pins from the BIOS configuration
 */

#define NUM_CONTROL_ALLOC	32
#define NUM_VERB_ALLOC		32

enum {
	ALC_CTL_WIDGET_VOL,
	ALC_CTL_WIDGET_MUTE,
	ALC_CTL_BIND_MUTE,
};
static struct snd_kcontrol_new alc880_control_templates[] = {
	HDA_CODEC_VOLUME(NULL, 0, 0, 0),
	HDA_CODEC_MUTE(NULL, 0, 0, 0),
	HDA_BIND_MUTE(NULL, 0, 0, 0),
};

/* add dynamic controls */
static int add_control(struct alc_spec *spec, int type, const char *name, unsigned long val)
{
	struct snd_kcontrol_new *knew;

	if (spec->num_kctl_used >= spec->num_kctl_alloc) {
		int num = spec->num_kctl_alloc + NUM_CONTROL_ALLOC;

		knew = kcalloc(num + 1, sizeof(*knew), GFP_KERNEL); /* array + terminator */
		if (! knew)
			return -ENOMEM;
		if (spec->kctl_alloc) {
			memcpy(knew, spec->kctl_alloc, sizeof(*knew) * spec->num_kctl_alloc);
			kfree(spec->kctl_alloc);
		}
		spec->kctl_alloc = knew;
		spec->num_kctl_alloc = num;
	}

	knew = &spec->kctl_alloc[spec->num_kctl_used];
	*knew = alc880_control_templates[type];
	knew->name = kstrdup(name, GFP_KERNEL);
	if (! knew->name)
		return -ENOMEM;
	knew->private_value = val;
	spec->num_kctl_used++;
	return 0;
}

#define alc880_is_fixed_pin(nid)	((nid) >= 0x14 && (nid) <= 0x17)
#define alc880_fixed_pin_idx(nid)	((nid) - 0x14)
#define alc880_is_multi_pin(nid)	((nid) >= 0x18)
#define alc880_multi_pin_idx(nid)	((nid) - 0x18)
#define alc880_is_input_pin(nid)	((nid) >= 0x18)
#define alc880_input_pin_idx(nid)	((nid) - 0x18)
#define alc880_idx_to_dac(nid)		((nid) + 0x02)
#define alc880_dac_to_idx(nid)		((nid) - 0x02)
#define alc880_idx_to_mixer(nid)	((nid) + 0x0c)
#define alc880_idx_to_selector(nid)	((nid) + 0x10)
#define ALC880_PIN_CD_NID		0x1c

/* fill in the dac_nids table from the parsed pin configuration */
static int alc880_auto_fill_dac_nids(struct alc_spec *spec, const struct auto_pin_cfg *cfg)
{
	hda_nid_t nid;
	int assigned[4];
	int i, j;

	memset(assigned, 0, sizeof(assigned));
	spec->multiout.dac_nids = spec->private_dac_nids;

	/* check the pins hardwired to audio widget */
	for (i = 0; i < cfg->line_outs; i++) {
		nid = cfg->line_out_pins[i];
		if (alc880_is_fixed_pin(nid)) {
			int idx = alc880_fixed_pin_idx(nid);
			spec->multiout.dac_nids[i] = alc880_idx_to_dac(idx);
			assigned[idx] = 1;
		}
	}
	/* left pins can be connect to any audio widget */
	for (i = 0; i < cfg->line_outs; i++) {
		nid = cfg->line_out_pins[i];
		if (alc880_is_fixed_pin(nid))
			continue;
		/* search for an empty channel */
		for (j = 0; j < cfg->line_outs; j++) {
			if (! assigned[j]) {
				spec->multiout.dac_nids[i] = alc880_idx_to_dac(j);
				assigned[j] = 1;
				break;
			}
		}
	}
	spec->multiout.num_dacs = cfg->line_outs;
	return 0;
}

/* add playback controls from the parsed DAC table */
static int alc880_auto_create_multi_out_ctls(struct alc_spec *spec,
					     const struct auto_pin_cfg *cfg)
{
	char name[32];
	static const char *chname[4] = { "Front", "Surround", NULL /*CLFE*/, "Side" };
	hda_nid_t nid;
	int i, err;

	for (i = 0; i < cfg->line_outs; i++) {
		if (! spec->multiout.dac_nids[i])
			continue;
		nid = alc880_idx_to_mixer(alc880_dac_to_idx(spec->multiout.dac_nids[i]));
		if (i == 2) {
			/* Center/LFE */
			if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "Center Playback Volume",
					       HDA_COMPOSE_AMP_VAL(nid, 1, 0, HDA_OUTPUT))) < 0)
				return err;
			if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "LFE Playback Volume",
					       HDA_COMPOSE_AMP_VAL(nid, 2, 0, HDA_OUTPUT))) < 0)
				return err;
			if ((err = add_control(spec, ALC_CTL_BIND_MUTE, "Center Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 1, 2, HDA_INPUT))) < 0)
				return err;
			if ((err = add_control(spec, ALC_CTL_BIND_MUTE, "LFE Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 2, 2, HDA_INPUT))) < 0)
				return err;
		} else {
			sprintf(name, "%s Playback Volume", chname[i]);
			if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, name,
					       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
				return err;
			sprintf(name, "%s Playback Switch", chname[i]);
			if ((err = add_control(spec, ALC_CTL_BIND_MUTE, name,
					       HDA_COMPOSE_AMP_VAL(nid, 3, 2, HDA_INPUT))) < 0)
				return err;
		}
	}
	return 0;
}

/* add playback controls for speaker and HP outputs */
static int alc880_auto_create_extra_out(struct alc_spec *spec, hda_nid_t pin,
					const char *pfx)
{
	hda_nid_t nid;
	int err;
	char name[32];

	if (! pin)
		return 0;

	if (alc880_is_fixed_pin(pin)) {
		nid = alc880_idx_to_dac(alc880_fixed_pin_idx(pin));
		if (! spec->multiout.dac_nids[0]) {
			/* use this as the primary output */
			spec->multiout.dac_nids[0] = nid;
			if (! spec->multiout.num_dacs)
				spec->multiout.num_dacs = 1;
		} else 
			/* specify the DAC as the extra output */
			spec->multiout.hp_nid = nid;
		/* control HP volume/switch on the output mixer amp */
		nid = alc880_idx_to_mixer(alc880_fixed_pin_idx(pin));
		sprintf(name, "%s Playback Volume", pfx);
		if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, name,
				       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
			return err;
		sprintf(name, "%s Playback Switch", pfx);
		if ((err = add_control(spec, ALC_CTL_BIND_MUTE, name,
				       HDA_COMPOSE_AMP_VAL(nid, 3, 2, HDA_INPUT))) < 0)
			return err;
	} else if (alc880_is_multi_pin(pin)) {
		/* set manual connection */
		if (! spec->multiout.dac_nids[0]) {
			/* use this as the primary output */
			spec->multiout.dac_nids[0] = alc880_idx_to_dac(alc880_multi_pin_idx(pin));
			if (! spec->multiout.num_dacs)
				spec->multiout.num_dacs = 1;
		}
		/* we have only a switch on HP-out PIN */
		sprintf(name, "%s Playback Switch", pfx);
		if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, name,
				       HDA_COMPOSE_AMP_VAL(pin, 3, 0, HDA_OUTPUT))) < 0)
			return err;
	}
	return 0;
}

/* create input playback/capture controls for the given pin */
static int new_analog_input(struct alc_spec *spec, hda_nid_t pin, const char *ctlname,
			    int idx, hda_nid_t mix_nid)
{
	char name[32];
	int err;

	sprintf(name, "%s Playback Volume", ctlname);
	if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, name,
			       HDA_COMPOSE_AMP_VAL(mix_nid, 3, idx, HDA_INPUT))) < 0)
		return err;
	sprintf(name, "%s Playback Switch", ctlname);
	if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, name,
			       HDA_COMPOSE_AMP_VAL(mix_nid, 3, idx, HDA_INPUT))) < 0)
		return err;
	return 0;
}

/* create playback/capture controls for input pins */
static int alc880_auto_create_analog_input_ctls(struct alc_spec *spec,
						const struct auto_pin_cfg *cfg)
{
	struct hda_input_mux *imux = &spec->private_imux;
	int i, err, idx;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		if (alc880_is_input_pin(cfg->input_pins[i])) {
			idx = alc880_input_pin_idx(cfg->input_pins[i]);
			err = new_analog_input(spec, cfg->input_pins[i],
					       auto_pin_cfg_labels[i],
					       idx, 0x0b);
			if (err < 0)
				return err;
			imux->items[imux->num_items].label = auto_pin_cfg_labels[i];
			imux->items[imux->num_items].index = alc880_input_pin_idx(cfg->input_pins[i]);
			imux->num_items++;
		}
	}
	return 0;
}

static void alc880_auto_set_output_and_unmute(struct hda_codec *codec,
					      hda_nid_t nid, int pin_type,
					      int dac_idx)
{
	/* set as output */
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL, pin_type);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);
	/* need the manual connection? */
	if (alc880_is_multi_pin(nid)) {
		struct alc_spec *spec = codec->spec;
		int idx = alc880_multi_pin_idx(nid);
		snd_hda_codec_write(codec, alc880_idx_to_selector(idx), 0,
				    AC_VERB_SET_CONNECT_SEL,
				    alc880_dac_to_idx(spec->multiout.dac_nids[dac_idx]));
	}
}

static void alc880_auto_init_multi_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i < spec->autocfg.line_outs; i++) {
		hda_nid_t nid = spec->autocfg.line_out_pins[i];
		alc880_auto_set_output_and_unmute(codec, nid, PIN_OUT, i);
	}
}

static void alc880_auto_init_extra_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	hda_nid_t pin;

	pin = spec->autocfg.speaker_pin;
	if (pin) /* connect to front */
		alc880_auto_set_output_and_unmute(codec, pin, PIN_OUT, 0);
	pin = spec->autocfg.hp_pin;
	if (pin) /* connect to front */
		alc880_auto_set_output_and_unmute(codec, pin, PIN_HP, 0);
}

static void alc880_auto_init_analog_input(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		hda_nid_t nid = spec->autocfg.input_pins[i];
		if (alc880_is_input_pin(nid)) {
			snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL,
					    i <= AUTO_PIN_FRONT_MIC ? PIN_VREF80 : PIN_IN);
			if (nid != ALC880_PIN_CD_NID)
				snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE,
						    AMP_OUT_MUTE);
		}
	}
}

/* parse the BIOS configuration and set up the alc_spec */
/* return 1 if successful, 0 if the proper config is not found, or a negative error code */
static int alc880_parse_auto_config(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int err;
	static hda_nid_t alc880_ignore[] = { 0x1d, 0 };

	if ((err = snd_hda_parse_pin_def_config(codec, &spec->autocfg,
						alc880_ignore)) < 0)
		return err;
	if (! spec->autocfg.line_outs && ! spec->autocfg.speaker_pin &&
	    ! spec->autocfg.hp_pin)
		return 0; /* can't find valid BIOS pin config */

	if ((err = alc880_auto_fill_dac_nids(spec, &spec->autocfg)) < 0 ||
	    (err = alc880_auto_create_multi_out_ctls(spec, &spec->autocfg)) < 0 ||
	    (err = alc880_auto_create_extra_out(spec, spec->autocfg.speaker_pin,
						"Speaker")) < 0 ||
	    (err = alc880_auto_create_extra_out(spec, spec->autocfg.speaker_pin,
						"Headphone")) < 0 ||
	    (err = alc880_auto_create_analog_input_ctls(spec, &spec->autocfg)) < 0)
		return err;

	spec->multiout.max_channels = spec->multiout.num_dacs * 2;

	if (spec->autocfg.dig_out_pin)
		spec->multiout.dig_out_nid = ALC880_DIGOUT_NID;
	if (spec->autocfg.dig_in_pin)
		spec->dig_in_nid = ALC880_DIGIN_NID;

	if (spec->kctl_alloc)
		spec->mixers[spec->num_mixers++] = spec->kctl_alloc;

	spec->init_verbs[spec->num_init_verbs++] = alc880_volume_init_verbs;

	spec->input_mux = &spec->private_imux;

	return 1;
}

/* init callback for auto-configuration model -- overriding the default init */
static int alc880_auto_init(struct hda_codec *codec)
{
	alc_init(codec);
	alc880_auto_init_multi_out(codec);
	alc880_auto_init_extra_out(codec);
	alc880_auto_init_analog_input(codec);
	return 0;
}

/*
 * OK, here we have finally the patch for ALC880
 */

static int patch_alc880(struct hda_codec *codec)
{
	struct alc_spec *spec;
	int board_config;
	int err;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	codec->spec = spec;

	board_config = snd_hda_check_board_config(codec, alc880_cfg_tbl);
	if (board_config < 0 || board_config >= ALC880_MODEL_LAST) {
		printk(KERN_INFO "hda_codec: Unknown model for ALC880, trying auto-probe from BIOS...\n");
		board_config = ALC880_AUTO;
	}

	if (board_config == ALC880_AUTO) {
		/* automatic parse from the BIOS config */
		err = alc880_parse_auto_config(codec);
		if (err < 0) {
			alc_free(codec);
			return err;
		} else if (! err) {
			printk(KERN_INFO "hda_codec: Cannot set up configuration from BIOS.  Using 3-stack mode...\n");
			board_config = ALC880_3ST;
		}
	}

	if (board_config != ALC880_AUTO)
		setup_preset(spec, &alc880_presets[board_config]);

	spec->stream_name_analog = "ALC880 Analog";
	spec->stream_analog_playback = &alc880_pcm_analog_playback;
	spec->stream_analog_capture = &alc880_pcm_analog_capture;

	spec->stream_name_digital = "ALC880 Digital";
	spec->stream_digital_playback = &alc880_pcm_digital_playback;
	spec->stream_digital_capture = &alc880_pcm_digital_capture;

	if (! spec->adc_nids && spec->input_mux) {
		/* check whether NID 0x07 is valid */
		unsigned int wcap = get_wcaps(codec, alc880_adc_nids[0]);
		wcap = (wcap & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT; /* get type */
		if (wcap != AC_WID_AUD_IN) {
			spec->adc_nids = alc880_adc_nids_alt;
			spec->num_adc_nids = ARRAY_SIZE(alc880_adc_nids_alt);
			spec->mixers[spec->num_mixers] = alc880_capture_alt_mixer;
			spec->num_mixers++;
		} else {
			spec->adc_nids = alc880_adc_nids;
			spec->num_adc_nids = ARRAY_SIZE(alc880_adc_nids);
			spec->mixers[spec->num_mixers] = alc880_capture_mixer;
			spec->num_mixers++;
		}
	}

	codec->patch_ops = alc_patch_ops;
	if (board_config == ALC880_AUTO)
		codec->patch_ops.init = alc880_auto_init;

	return 0;
}


/*
 * ALC260 support
 */

static hda_nid_t alc260_dac_nids[1] = {
	/* front */
	0x02,
};

static hda_nid_t alc260_adc_nids[1] = {
	/* ADC0 */
	0x04,
};

static hda_nid_t alc260_adc_nids_alt[1] = {
	/* ADC1 */
	0x05,
};

static hda_nid_t alc260_hp_adc_nids[2] = {
	/* ADC1, 0 */
	0x05, 0x04
};

#define ALC260_DIGOUT_NID	0x03
#define ALC260_DIGIN_NID	0x06

static struct hda_input_mux alc260_capture_source = {
	.num_items = 4,
	.items = {
		{ "Mic", 0x0 },
		{ "Front Mic", 0x1 },
		{ "Line", 0x2 },
		{ "CD", 0x4 },
	},
};

/* On Fujitsu S702x laptops capture only makes sense from Mic/LineIn jack
 * and the internal CD lines.
 */
static struct hda_input_mux alc260_fujitsu_capture_source = {
	.num_items = 2,
	.items = {
		{ "Mic/Line", 0x0 },
		{ "CD", 0x4 },
	},
};

/*
 * This is just place-holder, so there's something for alc_build_pcms to look
 * at when it calculates the maximum number of channels. ALC260 has no mixer
 * element which allows changing the channel mode, so the verb list is
 * never used.
 */
static struct hda_channel_mode alc260_modes[1] = {
	{ 2, NULL },
};


/* Mixer combinations
 *
 * basic: base_output + input + pc_beep + capture
 * HP: base_output + input + capture_alt
 * HP_3013: hp_3013 + input + capture
 * fujitsu: fujitsu + capture
 */

static struct snd_kcontrol_new alc260_base_output_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x08, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x08, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Headphone Playback Volume", 0x09, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Headphone Playback Switch", 0x09, 2, HDA_INPUT),
	HDA_CODEC_VOLUME_MONO("Mono Playback Volume", 0x0a, 1, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE_MONO("Mono Playback Switch", 0x0a, 1, 2, HDA_INPUT),
	{ } /* end */
};	

static struct snd_kcontrol_new alc260_input_mixer[] = {
	HDA_CODEC_VOLUME("CD Playback Volume", 0x07, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x07, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x07, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x07, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Front Mic Playback Volume", 0x07, 0x01, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x07, 0x01, HDA_INPUT),
	{ } /* end */
};

static struct snd_kcontrol_new alc260_pc_beep_mixer[] = {
	HDA_CODEC_VOLUME("PC Speaker Playback Volume", 0x07, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("PC Speaker Playback Switch", 0x07, 0x05, HDA_INPUT),
	{ } /* end */
};

static struct snd_kcontrol_new alc260_hp_3013_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x09, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Front Playback Switch", 0x10, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("Aux-In Playback Volume", 0x07, 0x06, HDA_INPUT),
	HDA_CODEC_MUTE("Aux-In Playback Switch", 0x07, 0x06, HDA_INPUT),
	HDA_CODEC_VOLUME("Headphone Playback Volume", 0x08, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x15, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("iSpeaker Playback Volume", 0x0a, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE_MONO("iSpeaker Playback Switch", 0x11, 1, 0x0, HDA_OUTPUT),
	{ } /* end */
};

static struct snd_kcontrol_new alc260_fujitsu_mixer[] = {
	HDA_CODEC_VOLUME("Headphone Playback Volume", 0x08, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Headphone Playback Switch", 0x08, 2, HDA_INPUT),
	ALC_PINCTL_SWITCH("Headphone Amp Switch", 0x14, PIN_HP_AMP),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x07, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x07, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic/Line Playback Volume", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic/Line Playback Switch", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Beep Playback Volume", 0x07, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("Beep Playback Switch", 0x07, 0x05, HDA_INPUT),
	HDA_CODEC_VOLUME("Internal Speaker Playback Volume", 0x09, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Internal Speaker Playback Switch", 0x09, 2, HDA_INPUT),
	{ } /* end */
};

/* capture mixer elements */
static struct snd_kcontrol_new alc260_capture_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x04, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x04, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 1, 0x05, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 1, 0x05, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 2,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};

static struct snd_kcontrol_new alc260_capture_alt_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x05, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x05, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 1,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};

/*
 * initialization verbs
 */
static struct hda_verb alc260_init_verbs[] = {
	/* Line In pin widget for input */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	/* CD pin widget for input */
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	/* Mic1 (rear panel) pin widget for input and vref at 80% */
	{0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	/* Mic2 (front panel) pin widget for input and vref at 80% */
	{0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	/* LINE-2 is used for line-out in rear */
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	/* select line-out */
	{0x0e, AC_VERB_SET_CONNECT_SEL, 0x00},
	/* LINE-OUT pin */
	{0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	/* enable HP */
	{0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	/* enable Mono */
	{0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	/* mute capture amp left and right */
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	/* set connection select to line in (default select for this ADC) */
	{0x04, AC_VERB_SET_CONNECT_SEL, 0x02},
	/* mute capture amp left and right */
	{0x05, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	/* set connection select to line in (default select for this ADC) */
	{0x05, AC_VERB_SET_CONNECT_SEL, 0x02},
	/* set vol=0 Line-Out mixer amp left and right */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* unmute pin widget amp left and right (no gain on this amp) */
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* set vol=0 HP mixer amp left and right */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* unmute pin widget amp left and right (no gain on this amp) */
	{0x10, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* set vol=0 Mono mixer amp left and right */
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* unmute pin widget amp left and right (no gain on this amp) */
	{0x11, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* unmute LINE-2 out pin */
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Amp Indexes: CD = 0x04, Line In 1 = 0x02, Mic 1 = 0x00 & Line In 2 = 0x03 */
	/* mute CD */
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(4)},
	/* mute Line In */
	{0x07,  AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	/* mute Mic */
	{0x07,  AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	/* Amp Indexes: DAC = 0x01 & mixer = 0x00 */
	/* mute Front out path */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	/* mute Headphone out path */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	/* mute Mono out path */
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{ }
};

static struct hda_verb alc260_hp_init_verbs[] = {
	/* Headphone and output */
	{0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0xc0},
	/* mono output */
	{0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},
	/* Mic1 (rear panel) pin widget for input and vref at 80% */
	{0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24},
	/* Mic2 (front panel) pin widget for input and vref at 80% */
	{0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24},
	/* Line In pin widget for input */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20},
	/* Line-2 pin widget for output */
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},
	/* CD pin widget for input */
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20},
	/* unmute amp left and right */
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, 0x7000},
	/* set connection select to line in (default select for this ADC) */
	{0x04, AC_VERB_SET_CONNECT_SEL, 0x02},
	/* unmute Line-Out mixer amp left and right (volume = 0) */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000},
	/* mute pin widget amp left and right (no gain on this amp) */
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	/* unmute HP mixer amp left and right (volume = 0) */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000},
	/* mute pin widget amp left and right (no gain on this amp) */
	{0x10, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	/* Amp Indexes: CD = 0x04, Line In 1 = 0x02, Mic 1 = 0x00 & Line In 2 = 0x03 */
	/* unmute CD */
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x04 << 8))},
	/* unmute Line In */
	{0x07,  AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x02 << 8))},
	/* unmute Mic */
	{0x07,  AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	/* Amp Indexes: DAC = 0x01 & mixer = 0x00 */
	/* Unmute Front out path */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8))},
	/* Unmute Headphone out path */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8))},
	/* Unmute Mono out path */
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8))},
	{ }
};

static struct hda_verb alc260_hp_3013_init_verbs[] = {
	/* Line out and output */
	{0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},
	/* mono output */
	{0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},
	/* Mic1 (rear panel) pin widget for input and vref at 80% */
	{0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24},
	/* Mic2 (front panel) pin widget for input and vref at 80% */
	{0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24},
	/* Line In pin widget for input */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20},
	/* Headphone pin widget for output */
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, 0xc0},
	/* CD pin widget for input */
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20},
	/* unmute amp left and right */
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, 0x7000},
	/* set connection select to line in (default select for this ADC) */
	{0x04, AC_VERB_SET_CONNECT_SEL, 0x02},
	/* unmute Line-Out mixer amp left and right (volume = 0) */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000},
	/* mute pin widget amp left and right (no gain on this amp) */
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	/* unmute HP mixer amp left and right (volume = 0) */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, 0xb000},
	/* mute pin widget amp left and right (no gain on this amp) */
	{0x10, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	/* Amp Indexes: CD = 0x04, Line In 1 = 0x02, Mic 1 = 0x00 & Line In 2 = 0x03 */
	/* unmute CD */
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x04 << 8))},
	/* unmute Line In */
	{0x07,  AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x02 << 8))},
	/* unmute Mic */
	{0x07,  AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	/* Amp Indexes: DAC = 0x01 & mixer = 0x00 */
	/* Unmute Front out path */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8))},
	/* Unmute Headphone out path */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8))},
	/* Unmute Mono out path */
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8))},
	{ }
};

/* Initialisation sequence for ALC260 as configured in Fujitsu S702x
 * laptops.
 */
static struct hda_verb alc260_fujitsu_init_verbs[] = {
	/* Disable all GPIOs */
	{0x01, AC_VERB_SET_GPIO_MASK, 0},
	/* Internal speaker is connected to headphone pin */
	{0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	/* Headphone/Line-out jack connects to Line1 pin; make it an output */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
        /* Mic/Line-in jack is connected to mic1 pin, so make it an input */
        {0x12, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
        /* Ensure all other unused pins are disabled and muted.
	 * Note: trying to set widget 0x15 to anything blocks all audio
	 * output for some reason, so just leave that at the default.
	 */
        {0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0},
        {0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0},
        {0x11, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x13, AC_VERB_SET_PIN_WIDGET_CONTROL, 0},
        {0x13, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
        /* Disable digital (SPDIF) pins */
        {0x03, AC_VERB_SET_DIGI_CONVERT_1, 0},
        {0x06, AC_VERB_SET_DIGI_CONVERT_1, 0},

        /* Start with mixer outputs muted */
        {0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
        {0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
        {0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},

        /* Unmute HP pin widget amp left and right (no equiv mixer ctrl) */
        {0x10, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
        /* Unmute Line1 pin widget amp left and right (no equiv mixer ctrl) */
        {0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	/* Unmute pin widget used for Line-in (no equiv mixer ctrl) */
        {0x12, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},

        /* Mute capture amp left and right */
        {0x04, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
        /* Set ADC connection select to line in (on mic1 pin) */
        {0x04, AC_VERB_SET_CONNECT_SEL, 0x00},

        /* Mute all inputs to mixer widget (even unconnected ones) */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)}, /* mic1 pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)}, /* mic2 pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(2)}, /* line1 pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(3)}, /* line2 pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(4)}, /* CD pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(5)}, /* Beep-gen pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(6)}, /* Line-out pin */
        {0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(7)}, /* HP-pin pin */

	{ }
};

static struct hda_pcm_stream alc260_pcm_analog_playback = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 2,
};

static struct hda_pcm_stream alc260_pcm_analog_capture = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 2,
};

#define alc260_pcm_digital_playback	alc880_pcm_digital_playback
#define alc260_pcm_digital_capture	alc880_pcm_digital_capture

/*
 * for BIOS auto-configuration
 */

static int alc260_add_playback_controls(struct alc_spec *spec, hda_nid_t nid,
					const char *pfx)
{
	hda_nid_t nid_vol;
	unsigned long vol_val, sw_val;
	char name[32];
	int err;

	if (nid >= 0x0f && nid < 0x11) {
		nid_vol = nid - 0x7;
		vol_val = HDA_COMPOSE_AMP_VAL(nid_vol, 3, 0, HDA_OUTPUT);
		sw_val = HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT);
	} else if (nid == 0x11) {
		nid_vol = nid - 0x7;
		vol_val = HDA_COMPOSE_AMP_VAL(nid_vol, 2, 0, HDA_OUTPUT);
		sw_val = HDA_COMPOSE_AMP_VAL(nid, 2, 0, HDA_OUTPUT);
	} else if (nid >= 0x12 && nid <= 0x15) {
		nid_vol = 0x08;
		vol_val = HDA_COMPOSE_AMP_VAL(nid_vol, 3, 0, HDA_OUTPUT);
		sw_val = HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT);
	} else
		return 0; /* N/A */
	
	snprintf(name, sizeof(name), "%s Playback Volume", pfx);
	if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, name, vol_val)) < 0)
		return err;
	snprintf(name, sizeof(name), "%s Playback Switch", pfx);
	if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, name, sw_val)) < 0)
		return err;
	return 1;
}

/* add playback controls from the parsed DAC table */
static int alc260_auto_create_multi_out_ctls(struct alc_spec *spec,
					     const struct auto_pin_cfg *cfg)
{
	hda_nid_t nid;
	int err;

	spec->multiout.num_dacs = 1;
	spec->multiout.dac_nids = spec->private_dac_nids;
	spec->multiout.dac_nids[0] = 0x02;

	nid = cfg->line_out_pins[0];
	if (nid) {
		err = alc260_add_playback_controls(spec, nid, "Front");
		if (err < 0)
			return err;
	}

	nid = cfg->speaker_pin;
	if (nid) {
		err = alc260_add_playback_controls(spec, nid, "Speaker");
		if (err < 0)
			return err;
	}

	nid = cfg->hp_pin;
	if (nid) {
		err = alc260_add_playback_controls(spec, nid, "Headphone");
		if (err < 0)
			return err;
	}
	return 0;	
}

/* create playback/capture controls for input pins */
static int alc260_auto_create_analog_input_ctls(struct alc_spec *spec,
						const struct auto_pin_cfg *cfg)
{
	struct hda_input_mux *imux = &spec->private_imux;
	int i, err, idx;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		if (cfg->input_pins[i] >= 0x12) {
			idx = cfg->input_pins[i] - 0x12;
			err = new_analog_input(spec, cfg->input_pins[i],
					       auto_pin_cfg_labels[i], idx, 0x07);
			if (err < 0)
				return err;
			imux->items[imux->num_items].label = auto_pin_cfg_labels[i];
			imux->items[imux->num_items].index = idx;
			imux->num_items++;
		}
		if ((cfg->input_pins[i] >= 0x0f) && (cfg->input_pins[i] <= 0x10)){
			idx = cfg->input_pins[i] - 0x09;
			err = new_analog_input(spec, cfg->input_pins[i],
					       auto_pin_cfg_labels[i], idx, 0x07);
			if (err < 0)
				return err;
			imux->items[imux->num_items].label = auto_pin_cfg_labels[i];
			imux->items[imux->num_items].index = idx;
			imux->num_items++;
		}
	}
	return 0;
}

static void alc260_auto_set_output_and_unmute(struct hda_codec *codec,
					      hda_nid_t nid, int pin_type,
					      int sel_idx)
{
	/* set as output */
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL, pin_type);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);
	/* need the manual connection? */
	if (nid >= 0x12) {
		int idx = nid - 0x12;
		snd_hda_codec_write(codec, idx + 0x0b, 0,
				    AC_VERB_SET_CONNECT_SEL, sel_idx);
				    
	}
}

static void alc260_auto_init_multi_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	hda_nid_t nid;

	nid = spec->autocfg.line_out_pins[0];	
	if (nid)
		alc260_auto_set_output_and_unmute(codec, nid, PIN_OUT, 0);
	
	nid = spec->autocfg.speaker_pin;
	if (nid)
		alc260_auto_set_output_and_unmute(codec, nid, PIN_OUT, 0);

	nid = spec->autocfg.hp_pin;
	if (nid)
		alc260_auto_set_output_and_unmute(codec, nid, PIN_OUT, 0);
}	

#define ALC260_PIN_CD_NID		0x16
static void alc260_auto_init_analog_input(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		hda_nid_t nid = spec->autocfg.input_pins[i];
		if (nid >= 0x12) {
			snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL,
					    i <= AUTO_PIN_FRONT_MIC ? PIN_VREF80 : PIN_IN);
			if (nid != ALC260_PIN_CD_NID)
				snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE,
						    AMP_OUT_MUTE);
		}
	}
}

/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc260_volume_init_verbs[] = {
	/*
	 * Unmute ADC0-1 and set the default input to mic-in
	 */
	{0x04, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x05, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x05, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	
	/* Unmute input amps (CD, Line In, Mic 1 & Mic 2) of the analog-loopback
	 * mixer widget
	 * Note: PASD motherboards uses the Line In 2 as the input for front panel
	 * mic (mic 2)
	 */
	/* Amp Indices: Mic1 = 0, Mic2 = 1, Line1 = 2, Line2 = 3, CD = 4 */
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(4)},

	/*
	 * Set up output mixers (0x08 - 0x0a)
	 */
	/* set vol=0 to output mixers */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* set up input amps for analog loopback */
	/* Amp Indices: DAC = 0, mixer = 1 */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	
	{ }
};

static int alc260_parse_auto_config(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	unsigned int wcap;
	int err;
	static hda_nid_t alc260_ignore[] = { 0x17, 0 };

	if ((err = snd_hda_parse_pin_def_config(codec, &spec->autocfg,
						alc260_ignore)) < 0)
		return err;
	if ((err = alc260_auto_create_multi_out_ctls(spec, &spec->autocfg)) < 0)
		return err;
	if (! spec->kctl_alloc)
		return 0; /* can't find valid BIOS pin config */
	if ((err = alc260_auto_create_analog_input_ctls(spec, &spec->autocfg)) < 0)
		return err;

	spec->multiout.max_channels = 2;

	if (spec->autocfg.dig_out_pin)
		spec->multiout.dig_out_nid = ALC260_DIGOUT_NID;
	if (spec->kctl_alloc)
		spec->mixers[spec->num_mixers++] = spec->kctl_alloc;

	spec->init_verbs[spec->num_init_verbs++] = alc260_volume_init_verbs;

	spec->input_mux = &spec->private_imux;

	/* check whether NID 0x04 is valid */
	wcap = get_wcaps(codec, 0x04);
	wcap = (wcap & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT; /* get type */
	if (wcap != AC_WID_AUD_IN) {
		spec->adc_nids = alc260_adc_nids_alt;
		spec->num_adc_nids = ARRAY_SIZE(alc260_adc_nids_alt);
		spec->mixers[spec->num_mixers] = alc260_capture_alt_mixer;
	} else {
		spec->adc_nids = alc260_adc_nids;
		spec->num_adc_nids = ARRAY_SIZE(alc260_adc_nids);
		spec->mixers[spec->num_mixers] = alc260_capture_mixer;
	}
	spec->num_mixers++;

	return 1;
}

/* init callback for auto-configuration model -- overriding the default init */
static int alc260_auto_init(struct hda_codec *codec)
{
	alc_init(codec);
	alc260_auto_init_multi_out(codec);
	alc260_auto_init_analog_input(codec);
	return 0;
}

/*
 * ALC260 configurations
 */
static struct hda_board_config alc260_cfg_tbl[] = {
	{ .modelname = "basic", .config = ALC260_BASIC },
	{ .pci_subvendor = 0x104d, .pci_subdevice = 0x81bb,
	  .config = ALC260_BASIC }, /* Sony VAIO */
	{ .pci_subvendor = 0x152d, .pci_subdevice = 0x0729,
	  .config = ALC260_BASIC }, /* CTL Travel Master U553W */
	{ .modelname = "hp", .config = ALC260_HP },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3010, .config = ALC260_HP },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3011, .config = ALC260_HP },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3012, .config = ALC260_HP_3013 },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3013, .config = ALC260_HP_3013 },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3014, .config = ALC260_HP },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3015, .config = ALC260_HP },
	{ .pci_subvendor = 0x103c, .pci_subdevice = 0x3016, .config = ALC260_HP },
	{ .modelname = "fujitsu", .config = ALC260_FUJITSU_S702X },
	{ .pci_subvendor = 0x10cf, .pci_subdevice = 0x1326, .config = ALC260_FUJITSU_S702X },
	{ .modelname = "auto", .config = ALC260_AUTO },
	{}
};

static struct alc_config_preset alc260_presets[] = {
	[ALC260_BASIC] = {
		.mixers = { alc260_base_output_mixer,
			    alc260_input_mixer,
			    alc260_pc_beep_mixer,
			    alc260_capture_mixer },
		.init_verbs = { alc260_init_verbs },
		.num_dacs = ARRAY_SIZE(alc260_dac_nids),
		.dac_nids = alc260_dac_nids,
		.num_adc_nids = ARRAY_SIZE(alc260_adc_nids),
		.adc_nids = alc260_adc_nids,
		.num_channel_mode = ARRAY_SIZE(alc260_modes),
		.channel_mode = alc260_modes,
		.input_mux = &alc260_capture_source,
	},
	[ALC260_HP] = {
		.mixers = { alc260_base_output_mixer,
			    alc260_input_mixer,
			    alc260_capture_alt_mixer },
		.init_verbs = { alc260_hp_init_verbs },
		.num_dacs = ARRAY_SIZE(alc260_dac_nids),
		.dac_nids = alc260_dac_nids,
		.num_adc_nids = ARRAY_SIZE(alc260_hp_adc_nids),
		.adc_nids = alc260_hp_adc_nids,
		.num_channel_mode = ARRAY_SIZE(alc260_modes),
		.channel_mode = alc260_modes,
		.input_mux = &alc260_capture_source,
	},
	[ALC260_HP_3013] = {
		.mixers = { alc260_hp_3013_mixer,
			    alc260_input_mixer,
			    alc260_capture_alt_mixer },
		.init_verbs = { alc260_hp_3013_init_verbs },
		.num_dacs = ARRAY_SIZE(alc260_dac_nids),
		.dac_nids = alc260_dac_nids,
		.num_adc_nids = ARRAY_SIZE(alc260_hp_adc_nids),
		.adc_nids = alc260_hp_adc_nids,
		.num_channel_mode = ARRAY_SIZE(alc260_modes),
		.channel_mode = alc260_modes,
		.input_mux = &alc260_capture_source,
	},
	[ALC260_FUJITSU_S702X] = {
		.mixers = { alc260_fujitsu_mixer,
			    alc260_capture_mixer },
		.init_verbs = { alc260_fujitsu_init_verbs },
		.num_dacs = ARRAY_SIZE(alc260_dac_nids),
		.dac_nids = alc260_dac_nids,
		.num_adc_nids = ARRAY_SIZE(alc260_adc_nids),
		.adc_nids = alc260_adc_nids,
		.num_channel_mode = ARRAY_SIZE(alc260_modes),
		.channel_mode = alc260_modes,
		.input_mux = &alc260_fujitsu_capture_source,
	},
};

static int patch_alc260(struct hda_codec *codec)
{
	struct alc_spec *spec;
	int err, board_config;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	codec->spec = spec;

	board_config = snd_hda_check_board_config(codec, alc260_cfg_tbl);
	if (board_config < 0 || board_config >= ALC260_MODEL_LAST) {
		snd_printd(KERN_INFO "hda_codec: Unknown model for ALC260\n");
		board_config = ALC260_AUTO;
	}

	if (board_config == ALC260_AUTO) {
		/* automatic parse from the BIOS config */
		err = alc260_parse_auto_config(codec);
		if (err < 0) {
			alc_free(codec);
			return err;
		} else if (! err) {
			printk(KERN_INFO "hda_codec: Cannot set up configuration from BIOS.  Using base mode...\n");
			board_config = ALC260_BASIC;
		}
	}

	if (board_config != ALC260_AUTO)
		setup_preset(spec, &alc260_presets[board_config]);

	spec->stream_name_analog = "ALC260 Analog";
	spec->stream_analog_playback = &alc260_pcm_analog_playback;
	spec->stream_analog_capture = &alc260_pcm_analog_capture;

	spec->stream_name_digital = "ALC260 Digital";
	spec->stream_digital_playback = &alc260_pcm_digital_playback;
	spec->stream_digital_capture = &alc260_pcm_digital_capture;

	codec->patch_ops = alc_patch_ops;
	if (board_config == ALC260_AUTO)
		codec->patch_ops.init = alc260_auto_init;

	return 0;
}


/*
 * ALC882 support
 *
 * ALC882 is almost identical with ALC880 but has cleaner and more flexible
 * configuration.  Each pin widget can choose any input DACs and a mixer.
 * Each ADC is connected from a mixer of all inputs.  This makes possible
 * 6-channel independent captures.
 *
 * In addition, an independent DAC for the multi-playback (not used in this
 * driver yet).
 */
#define ALC882_DIGOUT_NID	0x06
#define ALC882_DIGIN_NID	0x0a

static struct hda_channel_mode alc882_ch_modes[1] = {
	{ 8, NULL }
};

static hda_nid_t alc882_dac_nids[4] = {
	/* front, rear, clfe, rear_surr */
	0x02, 0x03, 0x04, 0x05
};

/* identical with ALC880 */
#define alc882_adc_nids		alc880_adc_nids
#define alc882_adc_nids_alt	alc880_adc_nids_alt

/* input MUX */
/* FIXME: should be a matrix-type input source selection */

static struct hda_input_mux alc882_capture_source = {
	.num_items = 4,
	.items = {
		{ "Mic", 0x0 },
		{ "Front Mic", 0x1 },
		{ "Line", 0x2 },
		{ "CD", 0x4 },
	},
};

#define alc882_mux_enum_info alc_mux_enum_info
#define alc882_mux_enum_get alc_mux_enum_get

static int alc882_mux_enum_put(struct snd_kcontrol *kcontrol, struct snd_ctl_elem_value *ucontrol)
{
	struct hda_codec *codec = snd_kcontrol_chip(kcontrol);
	struct alc_spec *spec = codec->spec;
	const struct hda_input_mux *imux = spec->input_mux;
	unsigned int adc_idx = snd_ctl_get_ioffidx(kcontrol, &ucontrol->id);
	static hda_nid_t capture_mixers[3] = { 0x24, 0x23, 0x22 };
	hda_nid_t nid = capture_mixers[adc_idx];
	unsigned int *cur_val = &spec->cur_mux[adc_idx];
	unsigned int i, idx;

	idx = ucontrol->value.enumerated.item[0];
	if (idx >= imux->num_items)
		idx = imux->num_items - 1;
	if (*cur_val == idx && ! codec->in_resume)
		return 0;
	for (i = 0; i < imux->num_items; i++) {
		unsigned int v = (i == idx) ? 0x7000 : 0x7080;
		snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE,
				    v | (imux->items[i].index << 8));
	}
	*cur_val = idx;
	return 1;
}

/*
 * 6ch mode
 */
static struct hda_verb alc882_sixstack_ch6_init[] = {
	{ 0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00 },
	{ 0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ } /* end */
};

/*
 * 8ch mode
 */
static struct hda_verb alc882_sixstack_ch8_init[] = {
	{ 0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ 0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT },
	{ } /* end */
};

static struct hda_channel_mode alc882_sixstack_modes[2] = {
	{ 6, alc882_sixstack_ch6_init },
	{ 8, alc882_sixstack_ch8_init },
};

/* Pin assignment: Front=0x14, Rear=0x15, CLFE=0x16, Side=0x17
 *                 Mic=0x18, Front Mic=0x19, Line-In=0x1a, HP=0x1b
 */
static struct snd_kcontrol_new alc882_base_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Front Playback Switch", 0x0c, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Surround Playback Volume", 0x0d, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Surround Playback Switch", 0x0d, 2, HDA_INPUT),
	HDA_CODEC_VOLUME_MONO("Center Playback Volume", 0x0e, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("LFE Playback Volume", 0x0e, 2, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE_MONO("Center Playback Switch", 0x0e, 1, 2, HDA_INPUT),
	HDA_BIND_MUTE_MONO("LFE Playback Switch", 0x0e, 2, 2, HDA_INPUT),
	HDA_CODEC_VOLUME("Side Playback Volume", 0x0f, 0x0, HDA_OUTPUT),
	HDA_BIND_MUTE("Side Playback Switch", 0x0f, 2, HDA_INPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x1b, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Front Mic Playback Volume", 0x0b, 0x1, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x0b, 0x1, HDA_INPUT),
	HDA_CODEC_VOLUME("PC Speaker Playback Volume", 0x0b, 0x05, HDA_INPUT),
	HDA_CODEC_MUTE("PC Speaker Playback Switch", 0x0b, 0x05, HDA_INPUT),
	HDA_CODEC_VOLUME("Capture Volume", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 1, 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 1, 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 2, 0x09, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 2, 0x09, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 3,
		.info = alc882_mux_enum_info,
		.get = alc882_mux_enum_get,
		.put = alc882_mux_enum_put,
	},
	{ } /* end */
};

static struct snd_kcontrol_new alc882_chmode_mixer[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Channel Mode",
		.info = alc_ch_mode_info,
		.get = alc_ch_mode_get,
		.put = alc_ch_mode_put,
	},
	{ } /* end */
};

static struct hda_verb alc882_init_verbs[] = {
	/* Front mixer: unmute input/output amp left and right (volume = 0) */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	/* Rear mixer */
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	/* CLFE mixer */
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	/* Side mixer */
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},

	/* Front Pin: output 0 (0x0c) */
	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x14, AC_VERB_SET_CONNECT_SEL, 0x00},
	/* Rear Pin: output 1 (0x0d) */
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x15, AC_VERB_SET_CONNECT_SEL, 0x01},
	/* CLFE Pin: output 2 (0x0e) */
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x16, AC_VERB_SET_CONNECT_SEL, 0x02},
	/* Side Pin: output 3 (0x0f) */
	{0x17, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x17, AC_VERB_SET_CONNECT_SEL, 0x03},
	/* Mic (rear) pin: input vref at 80% */
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Front Mic pin: input vref at 80% */
	{0x19, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_VREF80},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Line In pin: input */
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	/* Line-2 In: Headphone output (output 0 - 0x0c) */
	{0x1b, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_HP},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x1b, AC_VERB_SET_CONNECT_SEL, 0x00},
	/* CD pin widget for input */
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_IN},

	/* FIXME: use matrix-type input source selection */
	/* Mixer elements: 0x18, 19, 1a, 1b, 1c, 1d, 14, 15, 16, 17, 0b */
	/* Input mixer1: unmute Mic, F-Mic, Line, CD inputs */
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(3)},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(2)},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(4)},
	/* Input mixer2 */
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(3)},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(2)},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(4)},
	/* Input mixer3 */
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(3)},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(2)},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(4)},
	/* ADC1: mute amp left and right */
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x07, AC_VERB_SET_CONNECT_SEL, 0x00},
	/* ADC2: mute amp left and right */
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	/* ADC3: mute amp left and right */
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x09, AC_VERB_SET_CONNECT_SEL, 0x00},

	{ }
};

/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc882_auto_init_verbs[] = {
	/*
	 * Unmute ADC0-2 and set the default input to mic-in
	 */
	{0x07, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x09, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},

	/* Unmute input amps (CD, Line In, Mic 1 & Mic 2) of the analog-loopback
	 * mixer widget
	 * Note: PASD motherboards uses the Line In 2 as the input for front panel
	 * mic (mic 2)
	 */
	/* Amp Indices: Mic1 = 0, Mic2 = 1, Line1 = 2, Line2 = 3, CD = 4 */
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(4)},

	/*
	 * Set up output mixers (0x0c - 0x0f)
	 */
	/* set vol=0 to output mixers */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* set up input amps for analog loopback */
	/* Amp Indices: DAC = 0, mixer = 1 */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0f, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x26, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x26, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},

	/* FIXME: use matrix-type input source selection */
	/* Mixer elements: 0x18, 19, 1a, 1b, 1c, 1d, 14, 15, 16, 17, 0b */
	/* Input mixer1: unmute Mic, F-Mic, Line, CD inputs */
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},
	/* Input mixer2 */
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},
	/* Input mixer3 */
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},

	{ }
};

/* capture mixer elements */
static struct snd_kcontrol_new alc882_capture_alt_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 1, 0x09, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 1, 0x09, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 2,
		.info = alc882_mux_enum_info,
		.get = alc882_mux_enum_get,
		.put = alc882_mux_enum_put,
	},
	{ } /* end */
};

static struct snd_kcontrol_new alc882_capture_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x07, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 1, 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 1, 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME_IDX("Capture Volume", 2, 0x09, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE_IDX("Capture Switch", 2, 0x09, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 * FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 3,
		.info = alc882_mux_enum_info,
		.get = alc882_mux_enum_get,
		.put = alc882_mux_enum_put,
	},
	{ } /* end */
};

/* pcm configuration: identiacal with ALC880 */
#define alc882_pcm_analog_playback	alc880_pcm_analog_playback
#define alc882_pcm_analog_capture	alc880_pcm_analog_capture
#define alc882_pcm_digital_playback	alc880_pcm_digital_playback
#define alc882_pcm_digital_capture	alc880_pcm_digital_capture

/*
 * configuration and preset
 */
static struct hda_board_config alc882_cfg_tbl[] = {
	{ .modelname = "3stack-dig", .config = ALC882_3ST_DIG },
	{ .modelname = "6stack-dig", .config = ALC882_6ST_DIG },
	{ .pci_subvendor = 0x1462, .pci_subdevice = 0x6668, .config = ALC882_6ST_DIG }, /* MSI  */
	{ .pci_subvendor = 0x105b, .pci_subdevice = 0x6668, .config = ALC882_6ST_DIG }, /* Foxconn */
	{ .pci_subvendor = 0x1019, .pci_subdevice = 0x6668, .config = ALC882_6ST_DIG }, /* ECS */
	{ .modelname = "auto", .config = ALC882_AUTO },
	{}
};

static struct alc_config_preset alc882_presets[] = {
	[ALC882_3ST_DIG] = {
		.mixers = { alc882_base_mixer },
		.init_verbs = { alc882_init_verbs },
		.num_dacs = ARRAY_SIZE(alc882_dac_nids),
		.dac_nids = alc882_dac_nids,
		.dig_out_nid = ALC882_DIGOUT_NID,
		.num_adc_nids = ARRAY_SIZE(alc882_adc_nids),
		.adc_nids = alc882_adc_nids,
		.dig_in_nid = ALC882_DIGIN_NID,
		.num_channel_mode = ARRAY_SIZE(alc882_ch_modes),
		.channel_mode = alc882_ch_modes,
		.input_mux = &alc882_capture_source,
	},
	[ALC882_6ST_DIG] = {
		.mixers = { alc882_base_mixer, alc882_chmode_mixer },
		.init_verbs = { alc882_init_verbs },
		.num_dacs = ARRAY_SIZE(alc882_dac_nids),
		.dac_nids = alc882_dac_nids,
		.dig_out_nid = ALC882_DIGOUT_NID,
		.num_adc_nids = ARRAY_SIZE(alc882_adc_nids),
		.adc_nids = alc882_adc_nids,
		.dig_in_nid = ALC882_DIGIN_NID,
		.num_channel_mode = ARRAY_SIZE(alc882_sixstack_modes),
		.channel_mode = alc882_sixstack_modes,
		.input_mux = &alc882_capture_source,
	},
};


/*
 * BIOS auto configuration
 */
static void alc882_auto_set_output_and_unmute(struct hda_codec *codec,
					      hda_nid_t nid, int pin_type,
					      int dac_idx)
{
	/* set as output */
	struct alc_spec *spec = codec->spec;
	int idx; 
	
	if (spec->multiout.dac_nids[dac_idx] == 0x25)
		idx = 4;
	else
		idx = spec->multiout.dac_nids[dac_idx] - 2;

	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL, pin_type);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_CONNECT_SEL, idx);

}

static void alc882_auto_init_multi_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i <= HDA_SIDE; i++) {
		hda_nid_t nid = spec->autocfg.line_out_pins[i];	
		if (nid)
			alc882_auto_set_output_and_unmute(codec, nid, PIN_OUT, i);
	}
}

static void alc882_auto_init_hp_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	hda_nid_t pin;

	pin = spec->autocfg.hp_pin;
	if (pin) /* connect to front */
		alc882_auto_set_output_and_unmute(codec, pin, PIN_HP, 0); /* use dac 0 */
}

#define alc882_is_input_pin(nid)	alc880_is_input_pin(nid)
#define ALC882_PIN_CD_NID		ALC880_PIN_CD_NID

static void alc882_auto_init_analog_input(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		hda_nid_t nid = spec->autocfg.input_pins[i];
		if (alc882_is_input_pin(nid)) {
			snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL,
					    i <= AUTO_PIN_FRONT_MIC ? PIN_VREF80 : PIN_IN);
			if (nid != ALC882_PIN_CD_NID)
				snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_AMP_GAIN_MUTE,
						    AMP_OUT_MUTE);
		}
	}
}

/* almost identical with ALC880 parser... */
static int alc882_parse_auto_config(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int err = alc880_parse_auto_config(codec);

	if (err < 0)
		return err;
	else if (err > 0)
		/* hack - override the init verbs */
		spec->init_verbs[0] = alc882_auto_init_verbs;
	return err;
}

/* init callback for auto-configuration model -- overriding the default init */
static int alc882_auto_init(struct hda_codec *codec)
{
	alc_init(codec);
	alc882_auto_init_multi_out(codec);
	alc882_auto_init_hp_out(codec);
	alc882_auto_init_analog_input(codec);
	return 0;
}

/*
 *  ALC882 Headphone poll in 3.5.1a or 3.5.2
 */

static int patch_alc882(struct hda_codec *codec)
{
	struct alc_spec *spec;
	int err, board_config;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	codec->spec = spec;

	board_config = snd_hda_check_board_config(codec, alc882_cfg_tbl);

	if (board_config < 0 || board_config >= ALC882_MODEL_LAST) {
		printk(KERN_INFO "hda_codec: Unknown model for ALC882, trying auto-probe from BIOS...\n");
		board_config = ALC882_AUTO;
	}

	if (board_config == ALC882_AUTO) {
		/* automatic parse from the BIOS config */
		err = alc882_parse_auto_config(codec);
		if (err < 0) {
			alc_free(codec);
			return err;
		} else if (! err) {
			printk(KERN_INFO "hda_codec: Cannot set up configuration from BIOS.  Using base mode...\n");
			board_config = ALC882_3ST_DIG;
		}
	}

	if (board_config != ALC882_AUTO)
		setup_preset(spec, &alc882_presets[board_config]);

	spec->stream_name_analog = "ALC882 Analog";
	spec->stream_analog_playback = &alc882_pcm_analog_playback;
	spec->stream_analog_capture = &alc882_pcm_analog_capture;

	spec->stream_name_digital = "ALC882 Digital";
	spec->stream_digital_playback = &alc882_pcm_digital_playback;
	spec->stream_digital_capture = &alc882_pcm_digital_capture;

	if (! spec->adc_nids && spec->input_mux) {
		/* check whether NID 0x07 is valid */
		unsigned int wcap = get_wcaps(codec, 0x07);
		wcap = (wcap & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT; /* get type */
		if (wcap != AC_WID_AUD_IN) {
			spec->adc_nids = alc882_adc_nids_alt;
			spec->num_adc_nids = ARRAY_SIZE(alc882_adc_nids_alt);
			spec->mixers[spec->num_mixers] = alc882_capture_alt_mixer;
			spec->num_mixers++;
		} else {
			spec->adc_nids = alc882_adc_nids;
			spec->num_adc_nids = ARRAY_SIZE(alc882_adc_nids);
			spec->mixers[spec->num_mixers] = alc882_capture_mixer;
			spec->num_mixers++;
		}
	}

	codec->patch_ops = alc_patch_ops;
	if (board_config == ALC882_AUTO)
		codec->patch_ops.init = alc882_auto_init;

	return 0;
}

/*
 * ALC262 support
 */

#define ALC262_DIGOUT_NID	ALC880_DIGOUT_NID
#define ALC262_DIGIN_NID	ALC880_DIGIN_NID

#define alc262_dac_nids		alc260_dac_nids
#define alc262_adc_nids		alc882_adc_nids
#define alc262_adc_nids_alt	alc882_adc_nids_alt

#define alc262_modes		alc260_modes
#define alc262_capture_source	alc882_capture_source

static struct snd_kcontrol_new alc262_base_mixer[] = {
	HDA_CODEC_VOLUME("Front Playback Volume", 0x0c, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Front Playback Switch", 0x14, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("CD Playback Volume", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x0b, 0x04, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x0b, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x0b, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Front Mic Playback Volume", 0x0b, 0x01, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x0b, 0x01, HDA_INPUT),
	/* HDA_CODEC_VOLUME("PC Beep Playback Volume", 0x0b, 0x05, HDA_INPUT),
	   HDA_CODEC_MUTE("PC Beelp Playback Switch", 0x0b, 0x05, HDA_INPUT), */
	HDA_CODEC_VOLUME("Headphone Playback Volume", 0x0D, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x15, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME_MONO("Mono Playback Volume", 0x0e, 2, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE_MONO("Mono Playback Switch", 0x16, 2, 0x0, HDA_OUTPUT),
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Source",
		.count = 1,
		.info = alc882_mux_enum_info,
		.get = alc882_mux_enum_get,
		.put = alc882_mux_enum_put,
	},
	{ } /* end */
};			
	
#define alc262_capture_mixer		alc882_capture_mixer
#define alc262_capture_alt_mixer	alc882_capture_alt_mixer

/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc262_init_verbs[] = {
	/*
	 * Unmute ADC0-2 and set the default input to mic-in
	 */
	{0x07, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x09, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},

	/* Unmute input amps (CD, Line In, Mic 1 & Mic 2) of the analog-loopback
	 * mixer widget
	 * Note: PASD motherboards uses the Line In 2 as the input for front panel
	 * mic (mic 2)
	 */
	/* Amp Indices: Mic1 = 0, Mic2 = 1, Line1 = 2, Line2 = 3, CD = 4 */
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(4)},

	/*
	 * Set up output mixers (0x0c - 0x0e)
	 */
	/* set vol=0 to output mixers */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	/* set up input amps for analog loopback */
	/* Amp Indices: DAC = 0, mixer = 1 */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},

	{0x14, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},
	{0x15, AC_VERB_SET_PIN_WIDGET_CONTROL, 0xc0},
	{0x16, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40},
	{0x18, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24},
	{0x1a, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20},
	{0x1c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20},

	{0x14, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, 0x0000},
	
	{0x14, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x15, AC_VERB_SET_CONNECT_SEL, 0x01},
	
	/* FIXME: use matrix-type input source selection */
	/* Mixer elements: 0x18, 19, 1a, 1b, 1c, 1d, 14, 15, 16, 17, 0b */
	/* Input mixer1: unmute Mic, F-Mic, Line, CD inputs */
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},
	/* Input mixer2 */
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},
	/* Input mixer3 */
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},	

	{ }
};

/* add playback controls from the parsed DAC table */
static int alc262_auto_create_multi_out_ctls(struct alc_spec *spec, const struct auto_pin_cfg *cfg)
{
	hda_nid_t nid;
	int err;

	spec->multiout.num_dacs = 1;	/* only use one dac */
	spec->multiout.dac_nids = spec->private_dac_nids;
	spec->multiout.dac_nids[0] = 2;

	nid = cfg->line_out_pins[0];
	if (nid) {
		if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "Front Playback Volume",
				       HDA_COMPOSE_AMP_VAL(0x0c, 3, 0, HDA_OUTPUT))) < 0)
			return err;
		if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, "Front Playback Switch",
				       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
			return err;
	}

	nid = cfg->speaker_pin;
	if (nid) {
		if (nid == 0x16) {
			if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "Speaker Playback Volume",
					       HDA_COMPOSE_AMP_VAL(0x0e, 2, 0, HDA_OUTPUT))) < 0)
				return err;
			if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, "Speaker Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 2, 0, HDA_OUTPUT))) < 0)
				return err;
		} else {
			if (! cfg->line_out_pins[0])
				if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "Speaker Playback Volume",
					       HDA_COMPOSE_AMP_VAL(0x0c, 3, 0, HDA_OUTPUT))) < 0)
					return err;
			if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, "Speaker Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
				return err;
		}
	}
	nid = cfg->hp_pin;
	if (nid) {
		/* spec->multiout.hp_nid = 2; */
		if (nid == 0x16) {
			if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "Headphone Playback Volume",
					       HDA_COMPOSE_AMP_VAL(0x0e, 2, 0, HDA_OUTPUT))) < 0)
				return err;
			if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, "Headphone Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 2, 0, HDA_OUTPUT))) < 0)
				return err;
		} else {
			if (! cfg->line_out_pins[0])
				if ((err = add_control(spec, ALC_CTL_WIDGET_VOL, "Headphone Playback Volume",
					       HDA_COMPOSE_AMP_VAL(0x0c, 3, 0, HDA_OUTPUT))) < 0)
					return err;
			if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, "Headphone Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
				return err;
		}
	}
	return 0;	
}

/* identical with ALC880 */
#define alc262_auto_create_analog_input_ctls alc880_auto_create_analog_input_ctls

/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc262_volume_init_verbs[] = {
	/*
	 * Unmute ADC0-2 and set the default input to mic-in
	 */
	{0x07, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x09, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x09, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},

	/* Unmute input amps (CD, Line In, Mic 1 & Mic 2) of the analog-loopback
	 * mixer widget
	 * Note: PASD motherboards uses the Line In 2 as the input for front panel
	 * mic (mic 2)
	 */
	/* Amp Indices: Mic1 = 0, Mic2 = 1, Line1 = 2, Line2 = 3, CD = 4 */
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},
	{0x0b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(4)},

	/*
	 * Set up output mixers (0x0c - 0x0f)
	 */
	/* set vol=0 to output mixers */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_ZERO},
	
	/* set up input amps for analog loopback */
	/* Amp Indices: DAC = 0, mixer = 1 */
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0d, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x0e, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},

	/* FIXME: use matrix-type input source selection */
	/* Mixer elements: 0x18, 19, 1a, 1b, 1c, 1d, 14, 15, 16, 17, 0b */
	/* Input mixer1: unmute Mic, F-Mic, Line, CD inputs */
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x24, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},
	/* Input mixer2 */
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x23, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},
	/* Input mixer3 */
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x00 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x03 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8))},
	{0x22, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x04 << 8))},

	{ }
};

/* pcm configuration: identiacal with ALC880 */
#define alc262_pcm_analog_playback	alc880_pcm_analog_playback
#define alc262_pcm_analog_capture	alc880_pcm_analog_capture
#define alc262_pcm_digital_playback	alc880_pcm_digital_playback
#define alc262_pcm_digital_capture	alc880_pcm_digital_capture

/*
 * BIOS auto configuration
 */
static int alc262_parse_auto_config(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int err;
	static hda_nid_t alc262_ignore[] = { 0x1d, 0 };

	if ((err = snd_hda_parse_pin_def_config(codec, &spec->autocfg,
						alc262_ignore)) < 0)
		return err;
	if (! spec->autocfg.line_outs && ! spec->autocfg.speaker_pin &&
	    ! spec->autocfg.hp_pin)
		return 0; /* can't find valid BIOS pin config */
	if ((err = alc262_auto_create_multi_out_ctls(spec, &spec->autocfg)) < 0 ||
	    (err = alc262_auto_create_analog_input_ctls(spec, &spec->autocfg)) < 0)
		return err;

	spec->multiout.max_channels = spec->multiout.num_dacs * 2;

	if (spec->autocfg.dig_out_pin)
		spec->multiout.dig_out_nid = ALC262_DIGOUT_NID;
	if (spec->autocfg.dig_in_pin)
		spec->dig_in_nid = ALC262_DIGIN_NID;

	if (spec->kctl_alloc)
		spec->mixers[spec->num_mixers++] = spec->kctl_alloc;

	spec->init_verbs[spec->num_init_verbs++] = alc262_volume_init_verbs;
	spec->input_mux = &spec->private_imux;

	return 1;
}

#define alc262_auto_init_multi_out	alc882_auto_init_multi_out
#define alc262_auto_init_hp_out		alc882_auto_init_hp_out
#define alc262_auto_init_analog_input	alc882_auto_init_analog_input


/* init callback for auto-configuration model -- overriding the default init */
static int alc262_auto_init(struct hda_codec *codec)
{
	alc_init(codec);
	alc262_auto_init_multi_out(codec);
	alc262_auto_init_hp_out(codec);
	alc262_auto_init_analog_input(codec);
	return 0;
}

/*
 * configuration and preset
 */
static struct hda_board_config alc262_cfg_tbl[] = {
	{ .modelname = "basic", .config = ALC262_BASIC },
	{ .modelname = "auto", .config = ALC262_AUTO },
	{}
};

static struct alc_config_preset alc262_presets[] = {
	[ALC262_BASIC] = {
		.mixers = { alc262_base_mixer },
		.init_verbs = { alc262_init_verbs },
		.num_dacs = ARRAY_SIZE(alc262_dac_nids),
		.dac_nids = alc262_dac_nids,
		.hp_nid = 0x03,
		.num_channel_mode = ARRAY_SIZE(alc262_modes),
		.channel_mode = alc262_modes,
		.input_mux = &alc262_capture_source,
	},
};

static int patch_alc262(struct hda_codec *codec)
{
	struct alc_spec *spec;
	int board_config;
	int err;

	spec = kcalloc(1, sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	codec->spec = spec;
#if 0
	/* pshou 07/11/05  set a zero PCM sample to DAC when FIFO is under-run */
	{
	int tmp;
	snd_hda_codec_write(codec, 0x1a, 0, AC_VERB_SET_COEF_INDEX, 7);
	tmp = snd_hda_codec_read(codec, 0x20, 0, AC_VERB_GET_PROC_COEF, 0);
	snd_hda_codec_write(codec, 0x1a, 0, AC_VERB_SET_COEF_INDEX, 7);
	snd_hda_codec_write(codec, 0x1a, 0, AC_VERB_SET_PROC_COEF, tmp | 0x80);
	}
#endif

	board_config = snd_hda_check_board_config(codec, alc262_cfg_tbl);
	if (board_config < 0 || board_config >= ALC262_MODEL_LAST) {
		printk(KERN_INFO "hda_codec: Unknown model for ALC262, trying auto-probe from BIOS...\n");
		board_config = ALC262_AUTO;
	}

	if (board_config == ALC262_AUTO) {
		/* automatic parse from the BIOS config */
		err = alc262_parse_auto_config(codec);
		if (err < 0) {
			alc_free(codec);
			return err;
		} else if (! err) {
			printk(KERN_INFO "hda_codec: Cannot set up configuration from BIOS.  Using base mode...\n");
			board_config = ALC262_BASIC;
		}
	}

	if (board_config != ALC262_AUTO)
		setup_preset(spec, &alc262_presets[board_config]);

	spec->stream_name_analog = "ALC262 Analog";
	spec->stream_analog_playback = &alc262_pcm_analog_playback;
	spec->stream_analog_capture = &alc262_pcm_analog_capture;
		
	spec->stream_name_digital = "ALC262 Digital";
	spec->stream_digital_playback = &alc262_pcm_digital_playback;
	spec->stream_digital_capture = &alc262_pcm_digital_capture;

	if (! spec->adc_nids && spec->input_mux) {
		/* check whether NID 0x07 is valid */
		unsigned int wcap = get_wcaps(codec, 0x07);

		wcap = (wcap & AC_WCAP_TYPE) >> AC_WCAP_TYPE_SHIFT; /* get type */
		if (wcap != AC_WID_AUD_IN) {
			spec->adc_nids = alc262_adc_nids_alt;
			spec->num_adc_nids = ARRAY_SIZE(alc262_adc_nids_alt);
			spec->mixers[spec->num_mixers] = alc262_capture_alt_mixer;
			spec->num_mixers++;
		} else {
			spec->adc_nids = alc262_adc_nids;
			spec->num_adc_nids = ARRAY_SIZE(alc262_adc_nids);
			spec->mixers[spec->num_mixers] = alc262_capture_mixer;
			spec->num_mixers++;
		}
	}

	codec->patch_ops = alc_patch_ops;
	if (board_config == ALC262_AUTO)
		codec->patch_ops.init = alc262_auto_init;
	
	return 0;
}


/*
 *  ALC861 channel source setting (2/6 channel selection for 3-stack)
 */

/*
 * set the path ways for 2 channel output
 * need to set the codec line out and mic 1 pin widgets to inputs
 */
static struct hda_verb alc861_threestack_ch2_init[] = {
	/* set pin widget 1Ah (line in) for input */
	{ 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20 },
	/* set pin widget 18h (mic1/2) for input, for mic also enable the vref */
	{ 0x0d, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24 },

        { 0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb00c },
        { 0x15, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x01 << 8)) }, //mic
        { 0x15, AC_VERB_SET_AMP_GAIN_MUTE, (0x7000 | (0x02 << 8)) }, //line in
	{ } /* end */
};
/*
 * 6ch mode
 * need to set the codec line out and mic 1 pin widgets to outputs
 */
static struct hda_verb alc861_threestack_ch6_init[] = {
	/* set pin widget 1Ah (line in) for output (Back Surround)*/
	{ 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },
	/* set pin widget 18h (mic1) for output (CLFE)*/
	{ 0x0d, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },

	{ 0x0c, AC_VERB_SET_CONNECT_SEL, 0x00 },
        { 0x0d, AC_VERB_SET_CONNECT_SEL, 0x00 },

        { 0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb080 },
        { 0x15, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x01 << 8)) }, //mic
        { 0x15, AC_VERB_SET_AMP_GAIN_MUTE, (0x7080 | (0x02 << 8)) }, //line in
	{ } /* end */
};

static struct hda_channel_mode alc861_threestack_modes[2] = {
	{ 2, alc861_threestack_ch2_init },
	{ 6, alc861_threestack_ch6_init },
};

/* patch-ALC861 */

static struct snd_kcontrol_new alc861_base_mixer[] = {
        /* output mixer control */
	HDA_CODEC_MUTE("Front Playback Switch", 0x03, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Surround Playback Switch", 0x06, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE_MONO("Center Playback Switch", 0x05, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE_MONO("LFE Playback Switch", 0x05, 2, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Side Playback Switch", 0x04, 0x0, HDA_OUTPUT),

        /*Input mixer control */
	/* HDA_CODEC_VOLUME("Input Playback Volume", 0x15, 0x0, HDA_OUTPUT),
	   HDA_CODEC_MUTE("Input Playback Switch", 0x15, 0x0, HDA_OUTPUT), */
	HDA_CODEC_VOLUME("CD Playback Volume", 0x15, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x15, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x15, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x15, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x15, 0x01, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x15, 0x01, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x10, 0x01, HDA_OUTPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x1a, 0x03, HDA_INPUT),
 
        /* Capture mixer control */
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Source",
		.count = 1,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};

static struct snd_kcontrol_new alc861_3ST_mixer[] = {
        /* output mixer control */
	HDA_CODEC_MUTE("Front Playback Switch", 0x03, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE("Surround Playback Switch", 0x06, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE_MONO("Center Playback Switch", 0x05, 1, 0x0, HDA_OUTPUT),
	HDA_CODEC_MUTE_MONO("LFE Playback Switch", 0x05, 2, 0x0, HDA_OUTPUT),
	/*HDA_CODEC_MUTE("Side Playback Switch", 0x04, 0x0, HDA_OUTPUT), */

	/* Input mixer control */
	/* HDA_CODEC_VOLUME("Input Playback Volume", 0x15, 0x0, HDA_OUTPUT),
	   HDA_CODEC_MUTE("Input Playback Switch", 0x15, 0x0, HDA_OUTPUT), */
	HDA_CODEC_VOLUME("CD Playback Volume", 0x15, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("CD Playback Switch", 0x15, 0x0, HDA_INPUT),
	HDA_CODEC_VOLUME("Line Playback Volume", 0x15, 0x02, HDA_INPUT),
	HDA_CODEC_MUTE("Line Playback Switch", 0x15, 0x02, HDA_INPUT),
	HDA_CODEC_VOLUME("Mic Playback Volume", 0x15, 0x01, HDA_INPUT),
	HDA_CODEC_MUTE("Mic Playback Switch", 0x15, 0x01, HDA_INPUT),
	HDA_CODEC_MUTE("Front Mic Playback Switch", 0x10, 0x01, HDA_OUTPUT),
	HDA_CODEC_MUTE("Headphone Playback Switch", 0x1a, 0x03, HDA_INPUT),
 
	/* Capture mixer control */
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Source",
		.count = 1,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Channel Mode",
		.info = alc_ch_mode_info,
		.get = alc_ch_mode_get,
		.put = alc_ch_mode_put,
                .private_value = ARRAY_SIZE(alc861_threestack_modes),
	},
	{ } /* end */
};			
	
/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc861_base_init_verbs[] = {
	/*
	 * Unmute ADC0 and set the default input to mic-in
	 */
	/* port-A for surround (rear panel) */
	{ 0x0e, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },
	{ 0x0e, AC_VERB_SET_CONNECT_SEL, 0x00 },
	/* port-B for mic-in (rear panel) with vref */
	{ 0x0d, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24 },
	/* port-C for line-in (rear panel) */
	{ 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20 },
	/* port-D for Front */
	{ 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },
	{ 0x0b, AC_VERB_SET_CONNECT_SEL, 0x00 },
	/* port-E for HP out (front panel) */
	{ 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0xc0 },
	/* route front PCM to HP */
	{ 0x0f, AC_VERB_SET_CONNECT_SEL, 0x01 },
	/* port-F for mic-in (front panel) with vref */
	{ 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24 },
	/* port-G for CLFE (rear panel) */
	{ 0x1f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },
	{ 0x1f, AC_VERB_SET_CONNECT_SEL, 0x00 },
	/* port-H for side (rear panel) */
	{ 0x20, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },
	{ 0x20, AC_VERB_SET_CONNECT_SEL, 0x00 },
	/* CD-in */
	{ 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20 },
	/* route front mic to ADC1*/
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	
	/* Unmute DAC0~3 & spdif out*/
	{0x03, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x05, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x06, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	
	/* Unmute Mixer 14 (mic) 1c (Line in)*/
	{0x014, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
        {0x014, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x01c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
        {0x01c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	
	/* Unmute Stereo Mixer 15 */
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb00c          }, //Output 0~12 step

	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)}, // hp used DAC 3 (Front)
        {0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},

	{ }
};

static struct hda_verb alc861_threestack_init_verbs[] = {
	/*
	 * Unmute ADC0 and set the default input to mic-in
	 */
	/* port-A for surround (rear panel) */
	{ 0x0e, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00 },
	/* port-B for mic-in (rear panel) with vref */
	{ 0x0d, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24 },
	/* port-C for line-in (rear panel) */
	{ 0x0c, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20 },
	/* port-D for Front */
	{ 0x0b, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x40 },
	{ 0x0b, AC_VERB_SET_CONNECT_SEL, 0x00 },
	/* port-E for HP out (front panel) */
	{ 0x0f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0xc0 },
	/* route front PCM to HP */
	{ 0x0f, AC_VERB_SET_CONNECT_SEL, 0x01 },
	/* port-F for mic-in (front panel) with vref */
	{ 0x10, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x24 },
	/* port-G for CLFE (rear panel) */
	{ 0x1f, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00 },
	/* port-H for side (rear panel) */
	{ 0x20, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x00 },
	/* CD-in */
	{ 0x11, AC_VERB_SET_PIN_WIDGET_CONTROL, 0x20 },
	/* route front mic to ADC1*/
	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	/* Unmute DAC0~3 & spdif out*/
	{0x03, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x05, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x06, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	
	/* Unmute Mixer 14 (mic) 1c (Line in)*/
	{0x014, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
        {0x014, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x01c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
        {0x01c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	
	/* Unmute Stereo Mixer 15 */
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb00c          }, //Output 0~12 step

	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)}, // hp used DAC 3 (Front)
        {0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{ }
};
/*
 * generic initialization of ADC, input mixers and output mixers
 */
static struct hda_verb alc861_auto_init_verbs[] = {
	/*
	 * Unmute ADC0 and set the default input to mic-in
	 */
//	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},
	{0x08, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	
	/* Unmute DAC0~3 & spdif out*/
	{0x03, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x04, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x05, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x06, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_MUTE},
	{0x07, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE},
	
	/* Unmute Mixer 14 (mic) 1c (Line in)*/
	{0x014, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x014, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x01c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x01c, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	
	/* Unmute Stereo Mixer 15 */
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},
	{0x15, AC_VERB_SET_AMP_GAIN_MUTE, 0xb00c},

	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x16, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x17, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x18, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(0)},
	{0x19, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(1)},

	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},	
	{0x1a, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},		
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(0)},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_MUTE(1)},
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(2)},	
	{0x1b, AC_VERB_SET_AMP_GAIN_MUTE, AMP_IN_UNMUTE(3)},	

	{0x08, AC_VERB_SET_CONNECT_SEL, 0x00},	// set Mic 1

	{ }
};

/* pcm configuration: identiacal with ALC880 */
#define alc861_pcm_analog_playback	alc880_pcm_analog_playback
#define alc861_pcm_analog_capture	alc880_pcm_analog_capture
#define alc861_pcm_digital_playback	alc880_pcm_digital_playback
#define alc861_pcm_digital_capture	alc880_pcm_digital_capture


#define ALC861_DIGOUT_NID	0x07

static struct hda_channel_mode alc861_8ch_modes[1] = {
	{ 8, NULL }
};

static hda_nid_t alc861_dac_nids[4] = {
	/* front, surround, clfe, side */
	0x03, 0x06, 0x05, 0x04
};

static hda_nid_t alc861_adc_nids[1] = {
	/* ADC0-2 */
	0x08,
};

static struct hda_input_mux alc861_capture_source = {
	.num_items = 5,
	.items = {
		{ "Mic", 0x0 },
		{ "Front Mic", 0x3 },
		{ "Line", 0x1 },
		{ "CD", 0x4 },
		{ "Mixer", 0x5 },
	},
};

/* fill in the dac_nids table from the parsed pin configuration */
static int alc861_auto_fill_dac_nids(struct alc_spec *spec, const struct auto_pin_cfg *cfg)
{
	int i;
	hda_nid_t nid;

	spec->multiout.dac_nids = spec->private_dac_nids;
	for (i = 0; i < cfg->line_outs; i++) {
		nid = cfg->line_out_pins[i];
		if (nid) {
			if (i >= ARRAY_SIZE(alc861_dac_nids))
				continue;
			spec->multiout.dac_nids[i] = alc861_dac_nids[i];
		}
	}
	spec->multiout.num_dacs = cfg->line_outs;
	return 0;
}

/* add playback controls from the parsed DAC table */
static int alc861_auto_create_multi_out_ctls(struct alc_spec *spec,
					     const struct auto_pin_cfg *cfg)
{
	char name[32];
	static const char *chname[4] = { "Front", "Surround", NULL /*CLFE*/, "Side" };
	hda_nid_t nid;
	int i, idx, err;

	for (i = 0; i < cfg->line_outs; i++) {
		nid = spec->multiout.dac_nids[i];
		if (! nid)
			continue;
		if (nid == 0x05) {
			/* Center/LFE */
			if ((err = add_control(spec, ALC_CTL_BIND_MUTE, "Center Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 1, 0, HDA_OUTPUT))) < 0)
				return err;
			if ((err = add_control(spec, ALC_CTL_BIND_MUTE, "LFE Playback Switch",
					       HDA_COMPOSE_AMP_VAL(nid, 2, 0, HDA_OUTPUT))) < 0)
				return err;
		} else {
			for (idx = 0; idx < ARRAY_SIZE(alc861_dac_nids) - 1; idx++)
				if (nid == alc861_dac_nids[idx])
					break;
			sprintf(name, "%s Playback Switch", chname[idx]);
			if ((err = add_control(spec, ALC_CTL_BIND_MUTE, name,
					       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
				return err;
		}
	}
	return 0;
}

static int alc861_auto_create_hp_ctls(struct alc_spec *spec, hda_nid_t pin)
{
	int err;
	hda_nid_t nid;

	if (! pin)
		return 0;

	if ((pin >= 0x0b && pin <= 0x10) || pin == 0x1f || pin == 0x20) {
		nid = 0x03;
		if ((err = add_control(spec, ALC_CTL_WIDGET_MUTE, "Headphone Playback Switch",
				       HDA_COMPOSE_AMP_VAL(nid, 3, 0, HDA_OUTPUT))) < 0)
			return err;
		spec->multiout.hp_nid = nid;
	}
	return 0;
}

/* create playback/capture controls for input pins */
static int alc861_auto_create_analog_input_ctls(struct alc_spec *spec, const struct auto_pin_cfg *cfg)
{
	struct hda_input_mux *imux = &spec->private_imux;
	int i, err, idx, idx1;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		switch(cfg->input_pins[i]) {
		case 0x0c:
			idx1 = 1;
			idx = 2;	// Line In
			break;
		case 0x0f:
			idx1 = 2;
			idx = 2;	// Line In
			break;
		case 0x0d:
			idx1 = 0;
			idx = 1;	// Mic In 
			break;
		case 0x10:	
			idx1 = 3;
			idx = 1;	// Mic In 
			break;
		case 0x11:
			idx1 = 4;
			idx = 0;	// CD
			break;
		default:
			continue;
		}

		err = new_analog_input(spec, cfg->input_pins[i],
				       auto_pin_cfg_labels[i], idx, 0x15);
		if (err < 0)
			return err;

		imux->items[imux->num_items].label = auto_pin_cfg_labels[i];
		imux->items[imux->num_items].index = idx1;
		imux->num_items++;	
	}
	return 0;
}

static struct snd_kcontrol_new alc861_capture_mixer[] = {
	HDA_CODEC_VOLUME("Capture Volume", 0x08, 0x0, HDA_INPUT),
	HDA_CODEC_MUTE("Capture Switch", 0x08, 0x0, HDA_INPUT),

	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		/* The multiple "Capture Source" controls confuse alsamixer
		 * So call somewhat different..
		 *FIXME: the controls appear in the "playback" view!
		 */
		/* .name = "Capture Source", */
		.name = "Input Source",
		.count = 1,
		.info = alc_mux_enum_info,
		.get = alc_mux_enum_get,
		.put = alc_mux_enum_put,
	},
	{ } /* end */
};

static void alc861_auto_set_output_and_unmute(struct hda_codec *codec, hda_nid_t nid,
					      int pin_type, int dac_idx)
{
	/* set as output */

	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL, pin_type);
	snd_hda_codec_write(codec, dac_idx, 0, AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);

}

static void alc861_auto_init_multi_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i < spec->autocfg.line_outs; i++) {
		hda_nid_t nid = spec->autocfg.line_out_pins[i];
		if (nid)
			alc861_auto_set_output_and_unmute(codec, nid, PIN_OUT, spec->multiout.dac_nids[i]);
	}
}

static void alc861_auto_init_hp_out(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	hda_nid_t pin;

	pin = spec->autocfg.hp_pin;
	if (pin) /* connect to front */
		alc861_auto_set_output_and_unmute(codec, pin, PIN_HP, spec->multiout.dac_nids[0]);
}

static void alc861_auto_init_analog_input(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int i;

	for (i = 0; i < AUTO_PIN_LAST; i++) {
		hda_nid_t nid = spec->autocfg.input_pins[i];
		if ((nid>=0x0c) && (nid <=0x11)) {
			snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_PIN_WIDGET_CONTROL,
					    i <= AUTO_PIN_FRONT_MIC ? PIN_VREF80 : PIN_IN);
		}
	}
}

/* parse the BIOS configuration and set up the alc_spec */
/* return 1 if successful, 0 if the proper config is not found, or a negative error code */
static int alc861_parse_auto_config(struct hda_codec *codec)
{
	struct alc_spec *spec = codec->spec;
	int err;
	static hda_nid_t alc861_ignore[] = { 0x1d, 0 };

	if ((err = snd_hda_parse_pin_def_config(codec, &spec->autocfg,
						alc861_ignore)) < 0)
		return err;
	if (! spec->autocfg.line_outs && ! spec->autocfg.speaker_pin &&
	    ! spec->autocfg.hp_pin)
		return 0; /* can't find valid BIOS pin config */

	if ((err = alc861_auto_fill_dac_nids(spec, &spec->autocfg)) < 0 ||
	    (err = alc861_auto_create_multi_out_ctls(spec, &spec->autocfg)) < 0 ||
	    (err = alc861_auto_create_hp_ctls(spec, spec->autocfg.hp_pin)) < 0 ||
	    (err = alc861_auto_create_analog_input_ctls(spec, &spec->autocfg)) < 0)
		return err;

	spec->multiout.max_channels = spec->multiout.num_dacs * 2;

	if (spec->autocfg.dig_out_pin)
		spec->multiout.dig_out_nid = ALC861_DIGOUT_NID;

	if (spec->kctl_alloc)
		spec->mixers[spec->num_mixers++] = spec->kctl_alloc;

	spec->init_verbs[spec->num_init_verbs++] = alc861_auto_init_verbs;

	spec->input_mux = &spec->private_imux;

	spec->adc_nids = alc861_adc_nids;
	spec->num_adc_nids = ARRAY_SIZE(alc861_adc_nids);
	spec->mixers[spec->num_mixers] = alc861_capture_mixer;
	spec->num_mixers++;

	return 1;
}

/* init callback for auto-configuration model -- overriding the default init */
static int alc861_auto_init(struct hda_codec *codec)
{
	alc_init(codec);
	alc861_auto_init_multi_out(codec);
	alc861_auto_init_hp_out(codec);
	alc861_auto_init_analog_input(codec);

	return 0;
}


/*
 * configuration and preset
 */
static struct hda_board_config alc861_cfg_tbl[] = {
	{ .modelname = "3stack", .config = ALC861_3ST },
	{ .pci_subvendor = 0x8086, .pci_subdevice = 0xd600, .config = ALC861_3ST },
	{ .modelname = "3stack-dig", .config = ALC861_3ST_DIG },
	{ .modelname = "6stack-dig", .config = ALC861_6ST_DIG },
	{ .modelname = "auto", .config = ALC861_AUTO },
	{}
};

static struct alc_config_preset alc861_presets[] = {
	[ALC861_3ST] = {
		.mixers = { alc861_3ST_mixer },
		.init_verbs = { alc861_threestack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc861_dac_nids),
		.dac_nids = alc861_dac_nids,
		.num_channel_mode = ARRAY_SIZE(alc861_threestack_modes),
		.channel_mode = alc861_threestack_modes,
		.num_adc_nids = ARRAY_SIZE(alc861_adc_nids),
		.adc_nids = alc861_adc_nids,
		.input_mux = &alc861_capture_source,
	},
	[ALC861_3ST_DIG] = {
		.mixers = { alc861_base_mixer },
		.init_verbs = { alc861_threestack_init_verbs },
		.num_dacs = ARRAY_SIZE(alc861_dac_nids),
		.dac_nids = alc861_dac_nids,
		.dig_out_nid = ALC861_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc861_threestack_modes),
		.channel_mode = alc861_threestack_modes,
		.num_adc_nids = ARRAY_SIZE(alc861_adc_nids),
		.adc_nids = alc861_adc_nids,
		.input_mux = &alc861_capture_source,
	},
	[ALC861_6ST_DIG] = {
		.mixers = { alc861_base_mixer },
		.init_verbs = { alc861_base_init_verbs },
		.num_dacs = ARRAY_SIZE(alc861_dac_nids),
		.dac_nids = alc861_dac_nids,
		.dig_out_nid = ALC861_DIGOUT_NID,
		.num_channel_mode = ARRAY_SIZE(alc861_8ch_modes),
		.channel_mode = alc861_8ch_modes,
		.num_adc_nids = ARRAY_SIZE(alc861_adc_nids),
		.adc_nids = alc861_adc_nids,
		.input_mux = &alc861_capture_source,
	},
};	


static int patch_alc861(struct hda_codec *codec)
{
	struct alc_spec *spec;
	int board_config;
	int err;

	spec = kcalloc(1, sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	codec->spec = spec;	

        board_config = snd_hda_check_board_config(codec, alc861_cfg_tbl);
	if (board_config < 0 || board_config >= ALC861_MODEL_LAST) {
		printk(KERN_INFO "hda_codec: Unknown model for ALC861, trying auto-probe from BIOS...\n");
		board_config = ALC861_AUTO;
	}

	if (board_config == ALC861_AUTO) {
		/* automatic parse from the BIOS config */
		err = alc861_parse_auto_config(codec);
		if (err < 0) {
			alc_free(codec);
			return err;
		} else if (! err) {
			printk(KERN_INFO "hda_codec: Cannot set up configuration from BIOS.  Using base mode...\n");
		   board_config = ALC861_3ST_DIG;
		}
	}

	if (board_config != ALC861_AUTO)
		setup_preset(spec, &alc861_presets[board_config]);

	spec->stream_name_analog = "ALC861 Analog";
	spec->stream_analog_playback = &alc861_pcm_analog_playback;
	spec->stream_analog_capture = &alc861_pcm_analog_capture;

	spec->stream_name_digital = "ALC861 Digital";
	spec->stream_digital_playback = &alc861_pcm_digital_playback;
	spec->stream_digital_capture = &alc861_pcm_digital_capture;

	codec->patch_ops = alc_patch_ops;
	if (board_config == ALC861_AUTO)
		codec->patch_ops.init = alc861_auto_init;
		
	return 0;
}

/*
 * patch entries
 */
struct hda_codec_preset snd_hda_preset_realtek[] = {
	{ .id = 0x10ec0260, .name = "ALC260", .patch = patch_alc260 },
	{ .id = 0x10ec0262, .name = "ALC262", .patch = patch_alc262 },
 	{ .id = 0x10ec0880, .name = "ALC880", .patch = patch_alc880 },
	{ .id = 0x10ec0882, .name = "ALC882", .patch = patch_alc882 },
	{ .id = 0x10ec0883, .name = "ALC883", .patch = patch_alc882 },
	{ .id = 0x10ec0885, .name = "ALC885", .patch = patch_alc882 },
	{ .id = 0x10ec0861, .name = "ALC861", .patch = patch_alc861 },
	{} /* terminator */
};
