/*
 *
 *  patch_intelhdmi.c - Patch for Intel HDMI codecs
 *
 *  Copyright(c) 2008 Intel Corporation. All rights reserved.
 *
 *  Authors:
 *  			Jiang Zhe <zhe.jiang@intel.com>
 *  			Wu Fengguang <wfg@linux.intel.com>
 *
 *  Maintained by:
 *  			Wu Fengguang <wfg@linux.intel.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software Foundation,
 *  Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#include <linux/init.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <sound/core.h>
#include "hda_codec.h"
#include "hda_local.h"

static hda_nid_t cvt_nid;	/* audio converter */
static hda_nid_t pin_nid;	/* HDMI output pin */

#define INTEL_HDMI_EVENT_TAG		0x08

struct intel_hdmi_spec {
	struct hda_multi_out multiout;
	struct hda_pcm pcm_rec;
	struct hdmi_eld sink_eld;
};

struct hdmi_audio_infoframe {
	u8 type; /* 0x84 */
	u8 ver;  /* 0x01 */
	u8 len;  /* 0x0a */

	u8 checksum;	/* PB0 */
	u8 CC02_CT47;	/* CC in bits 0:2, CT in 4:7 */
	u8 SS01_SF24;
	u8 CXT04;
	u8 CA;
	u8 LFEPBL01_LSV36_DM_INH7;
	u8 reserved[5];	/* PB6 - PB10 */
};

/*
 * CEA speaker placement:
 *
 *        FLH       FCH        FRH
 *  FLW    FL  FLC   FC   FRC   FR   FRW
 *
 *                                  LFE
 *                     TC
 *
 *          RL  RLC   RC   RRC   RR
 *
 * The Left/Right Surround channel _notions_ LS/RS in SMPTE 320M corresponds to
 * CEA RL/RR; The SMPTE channel _assignment_ C/LFE is swapped to CEA LFE/FC.
 */
enum cea_speaker_placement {
	FL  = (1 <<  0),	/* Front Left           */
	FC  = (1 <<  1),	/* Front Center         */
	FR  = (1 <<  2),	/* Front Right          */
	FLC = (1 <<  3),	/* Front Left Center    */
	FRC = (1 <<  4),	/* Front Right Center   */
	RL  = (1 <<  5),	/* Rear Left            */
	RC  = (1 <<  6),	/* Rear Center          */
	RR  = (1 <<  7),	/* Rear Right           */
	RLC = (1 <<  8),	/* Rear Left Center     */
	RRC = (1 <<  9),	/* Rear Right Center    */
	LFE = (1 << 10),	/* Low Frequency Effect */
	FLW = (1 << 11),	/* Front Left Wide      */
	FRW = (1 << 12),	/* Front Right Wide     */
	FLH = (1 << 13),	/* Front Left High      */
	FCH = (1 << 14),	/* Front Center High    */
	FRH = (1 << 15),	/* Front Right High     */
	TC  = (1 << 16),	/* Top Center           */
};

/*
 * ELD SA bits in the CEA Speaker Allocation data block
 */
static int eld_speaker_allocation_bits[] = {
	[0] = FL | FR,
	[1] = LFE,
	[2] = FC,
	[3] = RL | RR,
	[4] = RC,
	[5] = FLC | FRC,
	[6] = RLC | RRC,
	/* the following are not defined in ELD yet */
	[7] = FLW | FRW,
	[8] = FLH | FRH,
	[9] = TC,
	[10] = FCH,
};

struct cea_channel_speaker_allocation {
	int ca_index;
	int speakers[8];

	/* derived values, just for convenience */
	int channels;
	int spk_mask;
};

/*
 * This is an ordered list!
 *
 * The preceding ones have better chances to be selected by
 * hdmi_setup_channel_allocation().
 */
static struct cea_channel_speaker_allocation channel_allocations[] = {
/* 			  channel:   8     7    6    5    4     3    2    1  */
{ .ca_index = 0x00,  .speakers = {   0,    0,   0,   0,   0,    0,  FR,  FL } },
				 /* 2.1 */
{ .ca_index = 0x01,  .speakers = {   0,    0,   0,   0,   0,  LFE,  FR,  FL } },
				 /* Dolby Surround */
{ .ca_index = 0x02,  .speakers = {   0,    0,   0,   0,  FC,    0,  FR,  FL } },
{ .ca_index = 0x03,  .speakers = {   0,    0,   0,   0,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x04,  .speakers = {   0,    0,   0,  RC,   0,    0,  FR,  FL } },
{ .ca_index = 0x05,  .speakers = {   0,    0,   0,  RC,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x06,  .speakers = {   0,    0,   0,  RC,  FC,    0,  FR,  FL } },
{ .ca_index = 0x07,  .speakers = {   0,    0,   0,  RC,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x08,  .speakers = {   0,    0,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x09,  .speakers = {   0,    0,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x0a,  .speakers = {   0,    0,  RR,  RL,  FC,    0,  FR,  FL } },
				 /* 5.1 */
{ .ca_index = 0x0b,  .speakers = {   0,    0,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x0c,  .speakers = {   0,   RC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x0d,  .speakers = {   0,   RC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x0e,  .speakers = {   0,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
				 /* 6.1 */
{ .ca_index = 0x0f,  .speakers = {   0,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x10,  .speakers = { RRC,  RLC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x11,  .speakers = { RRC,  RLC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x12,  .speakers = { RRC,  RLC,  RR,  RL,  FC,    0,  FR,  FL } },
				 /* 7.1 */
{ .ca_index = 0x13,  .speakers = { RRC,  RLC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x14,  .speakers = { FRC,  FLC,   0,   0,   0,    0,  FR,  FL } },
{ .ca_index = 0x15,  .speakers = { FRC,  FLC,   0,   0,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x16,  .speakers = { FRC,  FLC,   0,   0,  FC,    0,  FR,  FL } },
{ .ca_index = 0x17,  .speakers = { FRC,  FLC,   0,   0,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x18,  .speakers = { FRC,  FLC,   0,  RC,   0,    0,  FR,  FL } },
{ .ca_index = 0x19,  .speakers = { FRC,  FLC,   0,  RC,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x1a,  .speakers = { FRC,  FLC,   0,  RC,  FC,    0,  FR,  FL } },
{ .ca_index = 0x1b,  .speakers = { FRC,  FLC,   0,  RC,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x1c,  .speakers = { FRC,  FLC,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x1d,  .speakers = { FRC,  FLC,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x1e,  .speakers = { FRC,  FLC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x1f,  .speakers = { FRC,  FLC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x20,  .speakers = {   0,  FCH,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x21,  .speakers = {   0,  FCH,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x22,  .speakers = {  TC,    0,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x23,  .speakers = {  TC,    0,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x24,  .speakers = { FRH,  FLH,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x25,  .speakers = { FRH,  FLH,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x26,  .speakers = { FRW,  FLW,  RR,  RL,   0,    0,  FR,  FL } },
{ .ca_index = 0x27,  .speakers = { FRW,  FLW,  RR,  RL,   0,  LFE,  FR,  FL } },
{ .ca_index = 0x28,  .speakers = {  TC,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x29,  .speakers = {  TC,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x2a,  .speakers = { FCH,   RC,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x2b,  .speakers = { FCH,   RC,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x2c,  .speakers = {  TC,  FCH,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x2d,  .speakers = {  TC,  FCH,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x2e,  .speakers = { FRH,  FLH,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x2f,  .speakers = { FRH,  FLH,  RR,  RL,  FC,  LFE,  FR,  FL } },
{ .ca_index = 0x30,  .speakers = { FRW,  FLW,  RR,  RL,  FC,    0,  FR,  FL } },
{ .ca_index = 0x31,  .speakers = { FRW,  FLW,  RR,  RL,  FC,  LFE,  FR,  FL } },
};

/*
 * HDMI routines
 */

#ifdef BE_PARANOID
static void hdmi_get_dip_index(struct hda_codec *codec, hda_nid_t nid,
				int *packet_index, int *byte_index)
{
	int val;

	val = snd_hda_codec_read(codec, nid, 0, AC_VERB_GET_HDMI_DIP_INDEX, 0);

	*packet_index = val >> 5;
	*byte_index = val & 0x1f;
}
#endif

static void hdmi_set_dip_index(struct hda_codec *codec, hda_nid_t nid,
				int packet_index, int byte_index)
{
	int val;

	val = (packet_index << 5) | (byte_index & 0x1f);

	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_HDMI_DIP_INDEX, val);
}

static void hdmi_write_dip_byte(struct hda_codec *codec, hda_nid_t nid,
				unsigned char val)
{
	snd_hda_codec_write(codec, nid, 0, AC_VERB_SET_HDMI_DIP_DATA, val);
}

static void hdmi_enable_output(struct hda_codec *codec)
{
	/* Unmute */
	if (get_wcaps(codec, pin_nid) & AC_WCAP_OUT_AMP)
		snd_hda_codec_write(codec, pin_nid, 0,
				AC_VERB_SET_AMP_GAIN_MUTE, AMP_OUT_UNMUTE);
	/* Enable pin out */
	snd_hda_codec_write(codec, pin_nid, 0,
			    AC_VERB_SET_PIN_WIDGET_CONTROL, PIN_OUT);
}

/*
 * Enable Audio InfoFrame Transmission
 */
static void hdmi_start_infoframe_trans(struct hda_codec *codec)
{
	hdmi_set_dip_index(codec, pin_nid, 0x0, 0x0);
	snd_hda_codec_write(codec, pin_nid, 0, AC_VERB_SET_HDMI_DIP_XMIT,
						AC_DIPXMIT_BEST);
}

/*
 * Disable Audio InfoFrame Transmission
 */
static void hdmi_stop_infoframe_trans(struct hda_codec *codec)
{
	hdmi_set_dip_index(codec, pin_nid, 0x0, 0x0);
	snd_hda_codec_write(codec, pin_nid, 0, AC_VERB_SET_HDMI_DIP_XMIT,
						AC_DIPXMIT_DISABLE);
}

static int hdmi_get_channel_count(struct hda_codec *codec)
{
	return 1 + snd_hda_codec_read(codec, cvt_nid, 0,
					AC_VERB_GET_CVT_CHAN_COUNT, 0);
}

static void hdmi_set_channel_count(struct hda_codec *codec, int chs)
{
	snd_hda_codec_write(codec, cvt_nid, 0,
					AC_VERB_SET_CVT_CHAN_COUNT, chs - 1);

	if (chs != hdmi_get_channel_count(codec))
		snd_printd(KERN_INFO "HDMI channel count: expect %d, get %d\n",
					chs, hdmi_get_channel_count(codec));
}

static void hdmi_debug_channel_mapping(struct hda_codec *codec)
{
#ifdef CONFIG_SND_DEBUG_VERBOSE
	int i;
	int slot;

	for (i = 0; i < 8; i++) {
		slot = snd_hda_codec_read(codec, cvt_nid, 0,
						AC_VERB_GET_HDMI_CHAN_SLOT, i);
		printk(KERN_DEBUG "HDMI: ASP channel %d => slot %d\n",
						slot >> 4, slot & 0x7);
	}
#endif
}

static void hdmi_parse_eld(struct hda_codec *codec)
{
	struct intel_hdmi_spec *spec = codec->spec;
	struct hdmi_eld *eld = &spec->sink_eld;

	if (!snd_hdmi_get_eld(eld, codec, pin_nid))
		snd_hdmi_show_eld(eld);
}


/*
 * Audio InfoFrame routines
 */

static void hdmi_debug_dip_size(struct hda_codec *codec)
{
#ifdef CONFIG_SND_DEBUG_VERBOSE
	int i;
	int size;

	size = snd_hdmi_get_eld_size(codec, pin_nid);
	printk(KERN_DEBUG "HDMI: ELD buf size is %d\n", size);

	for (i = 0; i < 8; i++) {
		size = snd_hda_codec_read(codec, pin_nid, 0,
						AC_VERB_GET_HDMI_DIP_SIZE, i);
		printk(KERN_DEBUG "HDMI: DIP GP[%d] buf size is %d\n", i, size);
	}
#endif
}

static void hdmi_clear_dip_buffers(struct hda_codec *codec)
{
#ifdef BE_PARANOID
	int i, j;
	int size;
	int pi, bi;
	for (i = 0; i < 8; i++) {
		size = snd_hda_codec_read(codec, pin_nid, 0,
						AC_VERB_GET_HDMI_DIP_SIZE, i);
		if (size == 0)
			continue;

		hdmi_set_dip_index(codec, pin_nid, i, 0x0);
		for (j = 1; j < 1000; j++) {
			hdmi_write_dip_byte(codec, pin_nid, 0x0);
			hdmi_get_dip_index(codec, pin_nid, &pi, &bi);
			if (pi != i)
				snd_printd(KERN_INFO "dip index %d: %d != %d\n",
						bi, pi, i);
			if (bi == 0) /* byte index wrapped around */
				break;
		}
		snd_printd(KERN_INFO
			"HDMI: DIP GP[%d] buf reported size=%d, written=%d\n",
			i, size, j);
	}
#endif
}

static void hdmi_fill_audio_infoframe(struct hda_codec *codec,
					struct hdmi_audio_infoframe *ai)
{
	u8 *params = (u8 *)ai;
	u8 sum = 0;
	int i;

	hdmi_debug_dip_size(codec);
	hdmi_clear_dip_buffers(codec); /* be paranoid */

	for (i = 0; i < sizeof(ai); i++)
		sum += params[i];
	ai->checksum = - sum;

	hdmi_set_dip_index(codec, pin_nid, 0x0, 0x0);
	for (i = 0; i < sizeof(ai); i++)
		hdmi_write_dip_byte(codec, pin_nid, params[i]);
}

/*
 * Compute derived values in channel_allocations[].
 */
static void init_channel_allocations(void)
{
	int i, j;
	struct cea_channel_speaker_allocation *p;

	for (i = 0; i < ARRAY_SIZE(channel_allocations); i++) {
		p = channel_allocations + i;
		p->channels = 0;
		p->spk_mask = 0;
		for (j = 0; j < ARRAY_SIZE(p->speakers); j++)
			if (p->speakers[j]) {
				p->channels++;
				p->spk_mask |= p->speakers[j];
			}
	}
}

/*
 * The transformation takes two steps:
 *
 * 	eld->spk_alloc => (eld_speaker_allocation_bits[]) => spk_mask
 * 	      spk_mask => (channel_allocations[])         => ai->CA
 *
 * TODO: it could select the wrong CA from multiple candidates.
*/
static int hdmi_setup_channel_allocation(struct hda_codec *codec,
					 struct hdmi_audio_infoframe *ai)
{
	struct intel_hdmi_spec *spec = codec->spec;
	struct hdmi_eld *eld = &spec->sink_eld;
	int i;
	int spk_mask = 0;
	int channels = 1 + (ai->CC02_CT47 & 0x7);
	char buf[SND_PRINT_CHANNEL_ALLOCATION_ADVISED_BUFSIZE];

	/*
	 * CA defaults to 0 for basic stereo audio
	 */
	if (channels <= 2)
		return 0;

	/*
	 * HDMI sink's ELD info cannot always be retrieved for now, e.g.
	 * in console or for audio devices. Assume the highest speakers
	 * configuration, to _not_ prohibit multi-channel audio playback.
	 */
	if (!eld->spk_alloc)
		eld->spk_alloc = 0xffff;

	/*
	 * expand ELD's speaker allocation mask
	 *
	 * ELD tells the speaker mask in a compact(paired) form,
	 * expand ELD's notions to match the ones used by Audio InfoFrame.
	 */
	for (i = 0; i < ARRAY_SIZE(eld_speaker_allocation_bits); i++) {
		if (eld->spk_alloc & (1 << i))
			spk_mask |= eld_speaker_allocation_bits[i];
	}

	/* search for the first working match in the CA table */
	for (i = 0; i < ARRAY_SIZE(channel_allocations); i++) {
		if (channels == channel_allocations[i].channels &&
		    (spk_mask & channel_allocations[i].spk_mask) ==
				channel_allocations[i].spk_mask) {
			ai->CA = channel_allocations[i].ca_index;
			break;
		}
	}

	snd_print_channel_allocation(eld->spk_alloc, buf, sizeof(buf));
	snd_printdd(KERN_INFO
			"HDMI: select CA 0x%x for %d-channel allocation: %s\n",
			ai->CA, channels, buf);

	return ai->CA;
}

static void hdmi_setup_channel_mapping(struct hda_codec *codec,
					struct hdmi_audio_infoframe *ai)
{
	int i;

	if (!ai->CA)
		return;

	/*
	 * TODO: adjust channel mapping if necessary
	 * ALSA sequence is front/surr/clfe/side?
	 */

	for (i = 0; i < 8; i++)
		snd_hda_codec_write(codec, cvt_nid, 0,
				    AC_VERB_SET_HDMI_CHAN_SLOT,
				    (i << 4) | i);

	hdmi_debug_channel_mapping(codec);
}


static void hdmi_setup_audio_infoframe(struct hda_codec *codec,
					struct snd_pcm_substream *substream)
{
	struct hdmi_audio_infoframe ai = {
		.type		= 0x84,
		.ver		= 0x01,
		.len		= 0x0a,
		.CC02_CT47	= substream->runtime->channels - 1,
	};

	hdmi_setup_channel_allocation(codec, &ai);
	hdmi_setup_channel_mapping(codec, &ai);

	hdmi_fill_audio_infoframe(codec, &ai);
	hdmi_start_infoframe_trans(codec);
}


/*
 * Unsolicited events
 */

static void hdmi_intrinsic_event(struct hda_codec *codec, unsigned int res)
{
	int pind = !!(res & AC_UNSOL_RES_PD);
	int eldv = !!(res & AC_UNSOL_RES_ELDV);

	printk(KERN_INFO
		"HDMI hot plug event: Presence_Detect=%d ELD_Valid=%d\n",
		pind, eldv);

	if (pind && eldv) {
		hdmi_parse_eld(codec);
		/* TODO: do real things about ELD */
	}
}

static void hdmi_non_intrinsic_event(struct hda_codec *codec, unsigned int res)
{
	int subtag = (res & AC_UNSOL_RES_SUBTAG) >> AC_UNSOL_RES_SUBTAG_SHIFT;
	int cp_state = !!(res & AC_UNSOL_RES_CP_STATE);
	int cp_ready = !!(res & AC_UNSOL_RES_CP_READY);

	printk(KERN_INFO
		"HDMI content protection event: SUBTAG=0x%x CP_STATE=%d CP_READY=%d\n",
		subtag,
		cp_state,
		cp_ready);

	/* TODO */
	if (cp_state)
		;
	if (cp_ready)
		;
}


static void intel_hdmi_unsol_event(struct hda_codec *codec, unsigned int res)
{
	int tag = res >> AC_UNSOL_RES_TAG_SHIFT;
	int subtag = (res & AC_UNSOL_RES_SUBTAG) >> AC_UNSOL_RES_SUBTAG_SHIFT;

	if (tag != INTEL_HDMI_EVENT_TAG) {
		snd_printd(KERN_INFO "Unexpected HDMI event tag 0x%x\n", tag);
		return;
	}

	if (subtag == 0)
		hdmi_intrinsic_event(codec, res);
	else
		hdmi_non_intrinsic_event(codec, res);
}

/*
 * Callbacks
 */

static int intel_hdmi_playback_pcm_open(struct hda_pcm_stream *hinfo,
					struct hda_codec *codec,
					struct snd_pcm_substream *substream)
{
	struct intel_hdmi_spec *spec = codec->spec;

	return snd_hda_multi_out_dig_open(codec, &spec->multiout);
}

static int intel_hdmi_playback_pcm_close(struct hda_pcm_stream *hinfo,
					 struct hda_codec *codec,
					 struct snd_pcm_substream *substream)
{
	struct intel_hdmi_spec *spec = codec->spec;

	hdmi_stop_infoframe_trans(codec);

	return snd_hda_multi_out_dig_close(codec, &spec->multiout);
}

static int intel_hdmi_playback_pcm_prepare(struct hda_pcm_stream *hinfo,
					   struct hda_codec *codec,
					   unsigned int stream_tag,
					   unsigned int format,
					   struct snd_pcm_substream *substream)
{
	struct intel_hdmi_spec *spec = codec->spec;

	snd_hda_multi_out_dig_prepare(codec, &spec->multiout, stream_tag,
					     format, substream);

	hdmi_set_channel_count(codec, substream->runtime->channels);

	hdmi_setup_audio_infoframe(codec, substream);

	return 0;
}

static struct hda_pcm_stream intel_hdmi_pcm_playback = {
	.substreams = 1,
	.channels_min = 2,
	.channels_max = 8,
	.ops = {
		.open    = intel_hdmi_playback_pcm_open,
		.close   = intel_hdmi_playback_pcm_close,
		.prepare = intel_hdmi_playback_pcm_prepare
	},
};

static int intel_hdmi_build_pcms(struct hda_codec *codec)
{
	struct intel_hdmi_spec *spec = codec->spec;
	struct hda_pcm *info = &spec->pcm_rec;

	codec->num_pcms = 1;
	codec->pcm_info = info;

	/* NID to query formats and rates and setup streams */
	intel_hdmi_pcm_playback.nid = cvt_nid;

	info->name = "INTEL HDMI";
	info->pcm_type = HDA_PCM_TYPE_HDMI;
	info->stream[SNDRV_PCM_STREAM_PLAYBACK] = intel_hdmi_pcm_playback;

	return 0;
}

static int intel_hdmi_build_controls(struct hda_codec *codec)
{
	struct intel_hdmi_spec *spec = codec->spec;
	int err;

	err = snd_hda_create_spdif_out_ctls(codec, spec->multiout.dig_out_nid);
	if (err < 0)
		return err;

	return 0;
}

static int intel_hdmi_init(struct hda_codec *codec)
{
	hdmi_enable_output(codec);

	snd_hda_codec_write(codec, pin_nid, 0,
			    AC_VERB_SET_UNSOLICITED_ENABLE,
			    AC_USRSP_EN | INTEL_HDMI_EVENT_TAG);
	return 0;
}

static void intel_hdmi_free(struct hda_codec *codec)
{
	struct intel_hdmi_spec *spec = codec->spec;

	snd_hda_eld_proc_free(codec, &spec->sink_eld);
	kfree(spec);
}

static struct hda_codec_ops intel_hdmi_patch_ops = {
	.init			= intel_hdmi_init,
	.free			= intel_hdmi_free,
	.build_pcms		= intel_hdmi_build_pcms,
	.build_controls 	= intel_hdmi_build_controls,
	.unsol_event		= intel_hdmi_unsol_event,
};

static int do_patch_intel_hdmi(struct hda_codec *codec)
{
	struct intel_hdmi_spec *spec;

	spec = kzalloc(sizeof(*spec), GFP_KERNEL);
	if (spec == NULL)
		return -ENOMEM;

	spec->multiout.num_dacs = 0;	  /* no analog */
	spec->multiout.max_channels = 8;
	spec->multiout.dig_out_nid = cvt_nid;

	codec->spec = spec;
	codec->patch_ops = intel_hdmi_patch_ops;

	snd_hda_eld_proc_new(codec, &spec->sink_eld);

	init_channel_allocations();

	return 0;
}

static int patch_intel_hdmi(struct hda_codec *codec)
{
	cvt_nid = 0x02;
	pin_nid = 0x03;
	return do_patch_intel_hdmi(codec);
}

static int patch_intel_hdmi_ibexpeak(struct hda_codec *codec)
{
	cvt_nid = 0x02;
	pin_nid = 0x04;
	return do_patch_intel_hdmi(codec);
}

static struct hda_codec_preset snd_hda_preset_intelhdmi[] = {
	{ .id = 0x808629fb, .name = "G45 DEVCL",  .patch = patch_intel_hdmi },
	{ .id = 0x80862801, .name = "G45 DEVBLC", .patch = patch_intel_hdmi },
	{ .id = 0x80862802, .name = "G45 DEVCTG", .patch = patch_intel_hdmi },
	{ .id = 0x80862803, .name = "G45 DEVELK", .patch = patch_intel_hdmi },
	{ .id = 0x80862804, .name = "G45 DEVIBX", .patch = patch_intel_hdmi_ibexpeak },
	{ .id = 0x80860054, .name = "Q57 DEVIBX", .patch = patch_intel_hdmi_ibexpeak },
	{ .id = 0x10951392, .name = "SiI1392 HDMI",     .patch = patch_intel_hdmi },
	{} /* terminator */
};

MODULE_ALIAS("snd-hda-codec-id:808629fb");
MODULE_ALIAS("snd-hda-codec-id:80862801");
MODULE_ALIAS("snd-hda-codec-id:80862802");
MODULE_ALIAS("snd-hda-codec-id:80862803");
MODULE_ALIAS("snd-hda-codec-id:80862804");
MODULE_ALIAS("snd-hda-codec-id:80860054");
MODULE_ALIAS("snd-hda-codec-id:10951392");

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Intel HDMI HD-audio codec");

static struct hda_codec_preset_list intel_list = {
	.preset = snd_hda_preset_intelhdmi,
	.owner = THIS_MODULE,
};

static int __init patch_intelhdmi_init(void)
{
	return snd_hda_add_codec_preset(&intel_list);
}

static void __exit patch_intelhdmi_exit(void)
{
	snd_hda_delete_codec_preset(&intel_list);
}

module_init(patch_intelhdmi_init)
module_exit(patch_intelhdmi_exit)
