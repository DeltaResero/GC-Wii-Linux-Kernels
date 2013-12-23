/*
 * C-Media CMI8788 driver for Asus Xonar cards
 *
 * Copyright (c) Clemens Ladisch <clemens@ladisch.de>
 *
 *
 *  This driver is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License, version 2.
 *
 *  This driver is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this driver; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

/*
 * Xonar D2/D2X
 * ------------
 *
 * CMI8788:
 *
 * SPI 0 -> 1st PCM1796 (front)
 * SPI 1 -> 2nd PCM1796 (surround)
 * SPI 2 -> 3rd PCM1796 (center/LFE)
 * SPI 4 -> 4th PCM1796 (back)
 *
 * GPIO 2 -> M0 of CS5381
 * GPIO 3 -> M1 of CS5381
 * GPIO 5 <- external power present (D2X only)
 * GPIO 7 -> ALT
 * GPIO 8 -> enable output to speakers
 */

/*
 * Xonar D1/DX
 * -----------
 *
 * CMI8788:
 *
 * I²C <-> CS4398 (front)
 *     <-> CS4362A (surround, center/LFE, back)
 *
 * GPI 0 <- external power present (DX only)
 *
 * GPIO 0 -> enable output to speakers
 * GPIO 1 -> enable front panel I/O
 * GPIO 2 -> M0 of CS5361
 * GPIO 3 -> M1 of CS5361
 * GPIO 8 -> route input jack to line-in (0) or mic-in (1)
 *
 * CS4398:
 *
 * AD0 <- 1
 * AD1 <- 1
 *
 * CS4362A:
 *
 * AD0 <- 0
 */

/*
 * Xonar HDAV1.3 (Deluxe)
 * ----------------------
 *
 * CMI8788:
 *
 * I²C <-> PCM1796 (front)
 *
 * GPI 0 <- external power present
 *
 * GPIO 0 -> enable output to speakers
 * GPIO 2 -> M0 of CS5381
 * GPIO 3 -> M1 of CS5381
 * GPIO 8 -> route input jack to line-in (0) or mic-in (1)
 *
 * TXD -> HDMI controller
 * RXD <- HDMI controller
 *
 * PCM1796 front: AD1,0 <- 0,0
 *
 * no daughterboard
 * ----------------
 *
 * GPIO 4 <- 1
 *
 * H6 daughterboard
 * ----------------
 *
 * GPIO 4 <- 0
 * GPIO 5 <- 0
 *
 * I²C <-> PCM1796 (surround)
 *     <-> PCM1796 (center/LFE)
 *     <-> PCM1796 (back)
 *
 * PCM1796 surround:   AD1,0 <- 0,1
 * PCM1796 center/LFE: AD1,0 <- 1,0
 * PCM1796 back:       AD1,0 <- 1,1
 *
 * unknown daughterboard
 * ---------------------
 *
 * GPIO 4 <- 0
 * GPIO 5 <- 1
 *
 * I²C <-> CS4362A (surround, center/LFE, back)
 *
 * CS4362A: AD0 <- 0
 */

/*
 * Xonar Essence STX
 * -----------------
 *
 * CMI8788:
 *
 * I²C <-> PCM1792A
 *
 * GPI 0 <- external power present
 *
 * GPIO 0 -> enable output to speakers
 * GPIO 1 -> route HP to front panel (0) or rear jack (1)
 * GPIO 2 -> M0 of CS5381
 * GPIO 3 -> M1 of CS5381
 * GPIO 7 -> route output to speaker jacks (0) or HP (1)
 * GPIO 8 -> route input jack to line-in (0) or mic-in (1)
 *
 * PCM1792A:
 *
 * AD0 <- 0
 *
 * H6 daughterboard
 * ----------------
 *
 * GPIO 4 <- 0
 * GPIO 5 <- 0
 */

#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <sound/ac97_codec.h>
#include <sound/asoundef.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/tlv.h>
#include "oxygen.h"
#include "cm9780.h"
#include "pcm1796.h"
#include "cs4398.h"
#include "cs4362a.h"

MODULE_AUTHOR("Clemens Ladisch <clemens@ladisch.de>");
MODULE_DESCRIPTION("Asus AVx00 driver");
MODULE_LICENSE("GPL v2");
MODULE_SUPPORTED_DEVICE("{{Asus,AV100},{Asus,AV200}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE_PNP;

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "card index");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "enable card");

enum {
	MODEL_D2,
	MODEL_D2X,
	MODEL_D1,
	MODEL_DX,
	MODEL_HDAV,	/* without daughterboard */
	MODEL_HDAV_H6,	/* with H6 daughterboard */
	MODEL_STX,
};

static struct pci_device_id xonar_ids[] __devinitdata = {
	{ OXYGEN_PCI_SUBID(0x1043, 0x8269), .driver_data = MODEL_D2 },
	{ OXYGEN_PCI_SUBID(0x1043, 0x8275), .driver_data = MODEL_DX },
	{ OXYGEN_PCI_SUBID(0x1043, 0x82b7), .driver_data = MODEL_D2X },
	{ OXYGEN_PCI_SUBID(0x1043, 0x8314), .driver_data = MODEL_HDAV },
	{ OXYGEN_PCI_SUBID(0x1043, 0x834f), .driver_data = MODEL_D1 },
	{ OXYGEN_PCI_SUBID(0x1043, 0x835c), .driver_data = MODEL_STX },
	{ OXYGEN_PCI_SUBID_BROKEN_EEPROM },
	{ }
};
MODULE_DEVICE_TABLE(pci, xonar_ids);


#define GPIO_CS53x1_M_MASK	0x000c
#define GPIO_CS53x1_M_SINGLE	0x0000
#define GPIO_CS53x1_M_DOUBLE	0x0004
#define GPIO_CS53x1_M_QUAD	0x0008

#define GPIO_D2X_EXT_POWER	0x0020
#define GPIO_D2_ALT		0x0080
#define GPIO_D2_OUTPUT_ENABLE	0x0100

#define GPI_DX_EXT_POWER	0x01
#define GPIO_DX_OUTPUT_ENABLE	0x0001
#define GPIO_DX_FRONT_PANEL	0x0002
#define GPIO_DX_INPUT_ROUTE	0x0100

#define GPIO_HDAV_DB_MASK	0x0030
#define GPIO_HDAV_DB_H6		0x0000
#define GPIO_HDAV_DB_XX		0x0020

#define GPIO_ST_HP_REAR		0x0002
#define GPIO_ST_HP		0x0080

#define I2C_DEVICE_PCM1796(i)	(0x98 + ((i) << 1))	/* 10011, ADx=i, /W=0 */
#define I2C_DEVICE_CS4398	0x9e	/* 10011, AD1=1, AD0=1, /W=0 */
#define I2C_DEVICE_CS4362A	0x30	/* 001100, AD0=0, /W=0 */

struct xonar_data {
	unsigned int anti_pop_delay;
	unsigned int dacs;
	u16 output_enable_bit;
	u8 ext_power_reg;
	u8 ext_power_int_reg;
	u8 ext_power_bit;
	u8 has_power;
	u8 pcm1796_oversampling;
	u8 cs4398_fm;
	u8 cs4362a_fm;
	u8 hdmi_params[5];
};

static void xonar_gpio_changed(struct oxygen *chip);

static inline void pcm1796_write_spi(struct oxygen *chip, unsigned int codec,
				     u8 reg, u8 value)
{
	/* maps ALSA channel pair number to SPI output */
	static const u8 codec_map[4] = {
		0, 1, 2, 4
	};
	oxygen_write_spi(chip, OXYGEN_SPI_TRIGGER  |
			 OXYGEN_SPI_DATA_LENGTH_2 |
			 OXYGEN_SPI_CLOCK_160 |
			 (codec_map[codec] << OXYGEN_SPI_CODEC_SHIFT) |
			 OXYGEN_SPI_CEN_LATCH_CLOCK_HI,
			 (reg << 8) | value);
}

static inline void pcm1796_write_i2c(struct oxygen *chip, unsigned int codec,
				     u8 reg, u8 value)
{
	oxygen_write_i2c(chip, I2C_DEVICE_PCM1796(codec), reg, value);
}

static void pcm1796_write(struct oxygen *chip, unsigned int codec,
			  u8 reg, u8 value)
{
	if ((chip->model.function_flags & OXYGEN_FUNCTION_2WIRE_SPI_MASK) ==
	    OXYGEN_FUNCTION_SPI)
		pcm1796_write_spi(chip, codec, reg, value);
	else
		pcm1796_write_i2c(chip, codec, reg, value);
}

static void cs4398_write(struct oxygen *chip, u8 reg, u8 value)
{
	oxygen_write_i2c(chip, I2C_DEVICE_CS4398, reg, value);
}

static void cs4362a_write(struct oxygen *chip, u8 reg, u8 value)
{
	oxygen_write_i2c(chip, I2C_DEVICE_CS4362A, reg, value);
}

static void hdmi_write_command(struct oxygen *chip, u8 command,
			       unsigned int count, const u8 *params)
{
	unsigned int i;
	u8 checksum;

	oxygen_write_uart(chip, 0xfb);
	oxygen_write_uart(chip, 0xef);
	oxygen_write_uart(chip, command);
	oxygen_write_uart(chip, count);
	for (i = 0; i < count; ++i)
		oxygen_write_uart(chip, params[i]);
	checksum = 0xfb + 0xef + command + count;
	for (i = 0; i < count; ++i)
		checksum += params[i];
	oxygen_write_uart(chip, checksum);
}

static void xonar_enable_output(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	msleep(data->anti_pop_delay);
	oxygen_set_bits16(chip, OXYGEN_GPIO_DATA, data->output_enable_bit);
}

static void xonar_common_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	if (data->ext_power_reg) {
		oxygen_set_bits8(chip, data->ext_power_int_reg,
				 data->ext_power_bit);
		chip->interrupt_mask |= OXYGEN_INT_GPIO;
		chip->model.gpio_changed = xonar_gpio_changed;
		data->has_power = !!(oxygen_read8(chip, data->ext_power_reg)
				     & data->ext_power_bit);
	}
	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL,
			  GPIO_CS53x1_M_MASK | data->output_enable_bit);
	oxygen_write16_masked(chip, OXYGEN_GPIO_DATA,
			      GPIO_CS53x1_M_SINGLE, GPIO_CS53x1_M_MASK);
	oxygen_ac97_set_bits(chip, 0, CM9780_JACK, CM9780_FMIC2MIC);
	xonar_enable_output(chip);
}

static void update_pcm1796_volume(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	unsigned int i;

	for (i = 0; i < data->dacs; ++i) {
		pcm1796_write(chip, i, 16, chip->dac_volume[i * 2]);
		pcm1796_write(chip, i, 17, chip->dac_volume[i * 2 + 1]);
	}
}

static void update_pcm1796_mute(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	unsigned int i;
	u8 value;

	value = PCM1796_DMF_DISABLED | PCM1796_FMT_24_LJUST | PCM1796_ATLD;
	if (chip->dac_mute)
		value |= PCM1796_MUTE;
	for (i = 0; i < data->dacs; ++i)
		pcm1796_write(chip, i, 18, value);
}

static void pcm1796_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	unsigned int i;

	for (i = 0; i < data->dacs; ++i) {
		pcm1796_write(chip, i, 19, PCM1796_FLT_SHARP | PCM1796_ATS_1);
		pcm1796_write(chip, i, 20, data->pcm1796_oversampling);
		pcm1796_write(chip, i, 21, 0);
	}
	update_pcm1796_mute(chip); /* set ATLD before ATL/ATR */
	update_pcm1796_volume(chip);
}

static void xonar_d2_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	data->anti_pop_delay = 300;
	data->dacs = 4;
	data->output_enable_bit = GPIO_D2_OUTPUT_ENABLE;
	data->pcm1796_oversampling = PCM1796_OS_64;

	pcm1796_init(chip);

	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL, GPIO_D2_ALT);
	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA, GPIO_D2_ALT);

	xonar_common_init(chip);

	snd_component_add(chip->card, "PCM1796");
	snd_component_add(chip->card, "CS5381");
}

static void xonar_d2x_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	data->ext_power_reg = OXYGEN_GPIO_DATA;
	data->ext_power_int_reg = OXYGEN_GPIO_INTERRUPT_MASK;
	data->ext_power_bit = GPIO_D2X_EXT_POWER;
	oxygen_clear_bits16(chip, OXYGEN_GPIO_CONTROL, GPIO_D2X_EXT_POWER);

	xonar_d2_init(chip);
}

static void update_cs4362a_volumes(struct oxygen *chip)
{
	u8 mute;

	mute = chip->dac_mute ? CS4362A_MUTE : 0;
	cs4362a_write(chip, 7, (127 - chip->dac_volume[2]) | mute);
	cs4362a_write(chip, 8, (127 - chip->dac_volume[3]) | mute);
	cs4362a_write(chip, 10, (127 - chip->dac_volume[4]) | mute);
	cs4362a_write(chip, 11, (127 - chip->dac_volume[5]) | mute);
	cs4362a_write(chip, 13, (127 - chip->dac_volume[6]) | mute);
	cs4362a_write(chip, 14, (127 - chip->dac_volume[7]) | mute);
}

static void update_cs43xx_volume(struct oxygen *chip)
{
	cs4398_write(chip, 5, (127 - chip->dac_volume[0]) * 2);
	cs4398_write(chip, 6, (127 - chip->dac_volume[1]) * 2);
	update_cs4362a_volumes(chip);
}

static void update_cs43xx_mute(struct oxygen *chip)
{
	u8 reg;

	reg = CS4398_MUTEP_LOW | CS4398_PAMUTE;
	if (chip->dac_mute)
		reg |= CS4398_MUTE_B | CS4398_MUTE_A;
	cs4398_write(chip, 4, reg);
	update_cs4362a_volumes(chip);
}

static void cs43xx_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	/* set CPEN (control port mode) and power down */
	cs4398_write(chip, 8, CS4398_CPEN | CS4398_PDN);
	cs4362a_write(chip, 0x01, CS4362A_PDN | CS4362A_CPEN);
	/* configure */
	cs4398_write(chip, 2, data->cs4398_fm);
	cs4398_write(chip, 3, CS4398_ATAPI_B_R | CS4398_ATAPI_A_L);
	cs4398_write(chip, 7, CS4398_RMP_DN | CS4398_RMP_UP |
		     CS4398_ZERO_CROSS | CS4398_SOFT_RAMP);
	cs4362a_write(chip, 0x02, CS4362A_DIF_LJUST);
	cs4362a_write(chip, 0x03, CS4362A_MUTEC_6 | CS4362A_AMUTE |
		      CS4362A_RMP_UP | CS4362A_ZERO_CROSS | CS4362A_SOFT_RAMP);
	cs4362a_write(chip, 0x04, CS4362A_RMP_DN | CS4362A_DEM_NONE);
	cs4362a_write(chip, 0x05, 0);
	cs4362a_write(chip, 0x06, data->cs4362a_fm);
	cs4362a_write(chip, 0x09, data->cs4362a_fm);
	cs4362a_write(chip, 0x0c, data->cs4362a_fm);
	update_cs43xx_volume(chip);
	update_cs43xx_mute(chip);
	/* clear power down */
	cs4398_write(chip, 8, CS4398_CPEN);
	cs4362a_write(chip, 0x01, CS4362A_CPEN);
}

static void xonar_d1_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	data->anti_pop_delay = 800;
	data->output_enable_bit = GPIO_DX_OUTPUT_ENABLE;
	data->cs4398_fm = CS4398_FM_SINGLE | CS4398_DEM_NONE | CS4398_DIF_LJUST;
	data->cs4362a_fm = CS4362A_FM_SINGLE |
		CS4362A_ATAPI_B_R | CS4362A_ATAPI_A_L;

	oxygen_write16(chip, OXYGEN_2WIRE_BUS_STATUS,
		       OXYGEN_2WIRE_LENGTH_8 |
		       OXYGEN_2WIRE_INTERRUPT_MASK |
		       OXYGEN_2WIRE_SPEED_FAST);

	cs43xx_init(chip);

	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL,
			  GPIO_DX_FRONT_PANEL | GPIO_DX_INPUT_ROUTE);
	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA,
			    GPIO_DX_FRONT_PANEL | GPIO_DX_INPUT_ROUTE);

	xonar_common_init(chip);

	snd_component_add(chip->card, "CS4398");
	snd_component_add(chip->card, "CS4362A");
	snd_component_add(chip->card, "CS5361");
}

static void xonar_dx_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	data->ext_power_reg = OXYGEN_GPI_DATA;
	data->ext_power_int_reg = OXYGEN_GPI_INTERRUPT_MASK;
	data->ext_power_bit = GPI_DX_EXT_POWER;

	xonar_d1_init(chip);
}

static void xonar_hdav_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	u8 param;

	oxygen_write16(chip, OXYGEN_2WIRE_BUS_STATUS,
		       OXYGEN_2WIRE_LENGTH_8 |
		       OXYGEN_2WIRE_INTERRUPT_MASK |
		       OXYGEN_2WIRE_SPEED_FAST);

	data->anti_pop_delay = 100;
	data->dacs = chip->model.private_data == MODEL_HDAV_H6 ? 4 : 1;
	data->output_enable_bit = GPIO_DX_OUTPUT_ENABLE;
	data->ext_power_reg = OXYGEN_GPI_DATA;
	data->ext_power_int_reg = OXYGEN_GPI_INTERRUPT_MASK;
	data->ext_power_bit = GPI_DX_EXT_POWER;
	data->pcm1796_oversampling = PCM1796_OS_64;

	pcm1796_init(chip);

	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL, GPIO_DX_INPUT_ROUTE);
	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA, GPIO_DX_INPUT_ROUTE);

	oxygen_reset_uart(chip);
	param = 0;
	hdmi_write_command(chip, 0x61, 1, &param);
	param = 1;
	hdmi_write_command(chip, 0x74, 1, &param);
	data->hdmi_params[1] = IEC958_AES3_CON_FS_48000;
	data->hdmi_params[4] = 1;
	hdmi_write_command(chip, 0x54, 5, data->hdmi_params);

	xonar_common_init(chip);

	snd_component_add(chip->card, "PCM1796");
	snd_component_add(chip->card, "CS5381");
}

static void xonar_stx_init(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	oxygen_write16(chip, OXYGEN_2WIRE_BUS_STATUS,
		       OXYGEN_2WIRE_LENGTH_8 |
		       OXYGEN_2WIRE_INTERRUPT_MASK |
		       OXYGEN_2WIRE_SPEED_FAST);

	data->anti_pop_delay = 100;
	data->dacs = 1;
	data->output_enable_bit = GPIO_DX_OUTPUT_ENABLE;
	data->ext_power_reg = OXYGEN_GPI_DATA;
	data->ext_power_int_reg = OXYGEN_GPI_INTERRUPT_MASK;
	data->ext_power_bit = GPI_DX_EXT_POWER;
	data->pcm1796_oversampling = PCM1796_OS_64;

	pcm1796_init(chip);

	oxygen_set_bits16(chip, OXYGEN_GPIO_CONTROL,
			  GPIO_DX_INPUT_ROUTE | GPIO_ST_HP_REAR | GPIO_ST_HP);
	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA,
			    GPIO_DX_INPUT_ROUTE | GPIO_ST_HP_REAR | GPIO_ST_HP);

	xonar_common_init(chip);

	snd_component_add(chip->card, "PCM1792A");
	snd_component_add(chip->card, "CS5381");
}

static void xonar_disable_output(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;

	oxygen_clear_bits16(chip, OXYGEN_GPIO_DATA, data->output_enable_bit);
}

static void xonar_d2_cleanup(struct oxygen *chip)
{
	xonar_disable_output(chip);
}

static void xonar_d1_cleanup(struct oxygen *chip)
{
	xonar_disable_output(chip);
	cs4362a_write(chip, 0x01, CS4362A_PDN | CS4362A_CPEN);
	oxygen_clear_bits8(chip, OXYGEN_FUNCTION, OXYGEN_FUNCTION_RESET_CODEC);
}

static void xonar_hdav_cleanup(struct oxygen *chip)
{
	u8 param = 0;

	hdmi_write_command(chip, 0x74, 1, &param);
	xonar_disable_output(chip);
}

static void xonar_st_cleanup(struct oxygen *chip)
{
	xonar_disable_output(chip);
}

static void xonar_d2_suspend(struct oxygen *chip)
{
	xonar_d2_cleanup(chip);
}

static void xonar_d1_suspend(struct oxygen *chip)
{
	xonar_d1_cleanup(chip);
}

static void xonar_hdav_suspend(struct oxygen *chip)
{
	xonar_hdav_cleanup(chip);
	msleep(2);
}

static void xonar_st_suspend(struct oxygen *chip)
{
	xonar_st_cleanup(chip);
}

static void xonar_d2_resume(struct oxygen *chip)
{
	pcm1796_init(chip);
	xonar_enable_output(chip);
}

static void xonar_d1_resume(struct oxygen *chip)
{
	oxygen_set_bits8(chip, OXYGEN_FUNCTION, OXYGEN_FUNCTION_RESET_CODEC);
	msleep(1);
	cs43xx_init(chip);
	xonar_enable_output(chip);
}

static void xonar_hdav_resume(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	u8 param;

	oxygen_reset_uart(chip);
	param = 0;
	hdmi_write_command(chip, 0x61, 1, &param);
	param = 1;
	hdmi_write_command(chip, 0x74, 1, &param);
	hdmi_write_command(chip, 0x54, 5, data->hdmi_params);
	pcm1796_init(chip);
	xonar_enable_output(chip);
}

static void xonar_st_resume(struct oxygen *chip)
{
	pcm1796_init(chip);
	xonar_enable_output(chip);
}

static void xonar_hdav_pcm_hardware_filter(unsigned int channel,
					   struct snd_pcm_hardware *hardware)
{
	if (channel == PCM_MULTICH) {
		hardware->rates = SNDRV_PCM_RATE_44100 |
				  SNDRV_PCM_RATE_48000 |
				  SNDRV_PCM_RATE_96000 |
				  SNDRV_PCM_RATE_192000;
		hardware->rate_min = 44100;
	}
}

static void set_pcm1796_params(struct oxygen *chip,
			       struct snd_pcm_hw_params *params)
{
	struct xonar_data *data = chip->model_data;
	unsigned int i;

	data->pcm1796_oversampling =
		params_rate(params) >= 96000 ? PCM1796_OS_32 : PCM1796_OS_64;
	for (i = 0; i < data->dacs; ++i)
		pcm1796_write(chip, i, 20, data->pcm1796_oversampling);
}

static void set_cs53x1_params(struct oxygen *chip,
			      struct snd_pcm_hw_params *params)
{
	unsigned int value;

	if (params_rate(params) <= 54000)
		value = GPIO_CS53x1_M_SINGLE;
	else if (params_rate(params) <= 108000)
		value = GPIO_CS53x1_M_DOUBLE;
	else
		value = GPIO_CS53x1_M_QUAD;
	oxygen_write16_masked(chip, OXYGEN_GPIO_DATA,
			      value, GPIO_CS53x1_M_MASK);
}

static void set_cs43xx_params(struct oxygen *chip,
			      struct snd_pcm_hw_params *params)
{
	struct xonar_data *data = chip->model_data;

	data->cs4398_fm = CS4398_DEM_NONE | CS4398_DIF_LJUST;
	data->cs4362a_fm = CS4362A_ATAPI_B_R | CS4362A_ATAPI_A_L;
	if (params_rate(params) <= 50000) {
		data->cs4398_fm |= CS4398_FM_SINGLE;
		data->cs4362a_fm |= CS4362A_FM_SINGLE;
	} else if (params_rate(params) <= 100000) {
		data->cs4398_fm |= CS4398_FM_DOUBLE;
		data->cs4362a_fm |= CS4362A_FM_DOUBLE;
	} else {
		data->cs4398_fm |= CS4398_FM_QUAD;
		data->cs4362a_fm |= CS4362A_FM_QUAD;
	}
	cs4398_write(chip, 2, data->cs4398_fm);
	cs4362a_write(chip, 0x06, data->cs4362a_fm);
	cs4362a_write(chip, 0x09, data->cs4362a_fm);
	cs4362a_write(chip, 0x0c, data->cs4362a_fm);
}

static void set_hdmi_params(struct oxygen *chip,
			    struct snd_pcm_hw_params *params)
{
	struct xonar_data *data = chip->model_data;

	data->hdmi_params[0] = 0; /* 1 = non-audio */
	switch (params_rate(params)) {
	case 44100:
		data->hdmi_params[1] = IEC958_AES3_CON_FS_44100;
		break;
	case 48000:
		data->hdmi_params[1] = IEC958_AES3_CON_FS_48000;
		break;
	default: /* 96000 */
		data->hdmi_params[1] = IEC958_AES3_CON_FS_96000;
		break;
	case 192000:
		data->hdmi_params[1] = IEC958_AES3_CON_FS_192000;
		break;
	}
	data->hdmi_params[2] = params_channels(params) / 2 - 1;
	if (params_format(params) == SNDRV_PCM_FORMAT_S16_LE)
		data->hdmi_params[3] = 0;
	else
		data->hdmi_params[3] = 0xc0;
	data->hdmi_params[4] = 1; /* ? */
	hdmi_write_command(chip, 0x54, 5, data->hdmi_params);
}

static void set_hdav_params(struct oxygen *chip,
			    struct snd_pcm_hw_params *params)
{
	set_pcm1796_params(chip, params);
	set_hdmi_params(chip, params);
}

static void xonar_gpio_changed(struct oxygen *chip)
{
	struct xonar_data *data = chip->model_data;
	u8 has_power;

	has_power = !!(oxygen_read8(chip, data->ext_power_reg)
		       & data->ext_power_bit);
	if (has_power != data->has_power) {
		data->has_power = has_power;
		if (has_power) {
			snd_printk(KERN_NOTICE "power restored\n");
		} else {
			snd_printk(KERN_CRIT
				   "Hey! Don't unplug the power cable!\n");
			/* TODO: stop PCMs */
		}
	}
}

static void xonar_hdav_uart_input(struct oxygen *chip)
{
	if (chip->uart_input_count >= 2 &&
	    chip->uart_input[chip->uart_input_count - 2] == 'O' &&
	    chip->uart_input[chip->uart_input_count - 1] == 'K') {
		printk(KERN_DEBUG "message from Xonar HDAV HDMI chip received:\n");
		print_hex_dump_bytes("", DUMP_PREFIX_OFFSET,
				     chip->uart_input, chip->uart_input_count);
		chip->uart_input_count = 0;
	}
}

static int gpio_bit_switch_get(struct snd_kcontrol *ctl,
			       struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;
	u16 bit = ctl->private_value;

	value->value.integer.value[0] =
		!!(oxygen_read16(chip, OXYGEN_GPIO_DATA) & bit);
	return 0;
}

static int gpio_bit_switch_put(struct snd_kcontrol *ctl,
			       struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;
	u16 bit = ctl->private_value;
	u16 old_bits, new_bits;
	int changed;

	spin_lock_irq(&chip->reg_lock);
	old_bits = oxygen_read16(chip, OXYGEN_GPIO_DATA);
	if (value->value.integer.value[0])
		new_bits = old_bits | bit;
	else
		new_bits = old_bits & ~bit;
	changed = new_bits != old_bits;
	if (changed)
		oxygen_write16(chip, OXYGEN_GPIO_DATA, new_bits);
	spin_unlock_irq(&chip->reg_lock);
	return changed;
}

static const struct snd_kcontrol_new alt_switch = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Analog Loopback Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = gpio_bit_switch_get,
	.put = gpio_bit_switch_put,
	.private_value = GPIO_D2_ALT,
};

static const struct snd_kcontrol_new front_panel_switch = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Front Panel Switch",
	.info = snd_ctl_boolean_mono_info,
	.get = gpio_bit_switch_get,
	.put = gpio_bit_switch_put,
	.private_value = GPIO_DX_FRONT_PANEL,
};

static int st_output_switch_info(struct snd_kcontrol *ctl,
				 struct snd_ctl_elem_info *info)
{
	static const char *const names[3] = {
		"Speakers", "Headphones", "FP Headphones"
	};

	info->type = SNDRV_CTL_ELEM_TYPE_ENUMERATED;
	info->count = 1;
	info->value.enumerated.items = 3;
	if (info->value.enumerated.item >= 3)
		info->value.enumerated.item = 2;
	strcpy(info->value.enumerated.name, names[info->value.enumerated.item]);
	return 0;
}

static int st_output_switch_get(struct snd_kcontrol *ctl,
				struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;
	u16 gpio;

	gpio = oxygen_read16(chip, OXYGEN_GPIO_DATA);
	if (!(gpio & GPIO_ST_HP))
		value->value.enumerated.item[0] = 0;
	else if (gpio & GPIO_ST_HP_REAR)
		value->value.enumerated.item[0] = 1;
	else
		value->value.enumerated.item[0] = 2;
	return 0;
}


static int st_output_switch_put(struct snd_kcontrol *ctl,
				struct snd_ctl_elem_value *value)
{
	struct oxygen *chip = ctl->private_data;
	u16 gpio_old, gpio;

	mutex_lock(&chip->mutex);
	gpio_old = oxygen_read16(chip, OXYGEN_GPIO_DATA);
	gpio = gpio_old;
	switch (value->value.enumerated.item[0]) {
	case 0:
		gpio &= ~(GPIO_ST_HP | GPIO_ST_HP_REAR);
		break;
	case 1:
		gpio |= GPIO_ST_HP | GPIO_ST_HP_REAR;
		break;
	case 2:
		gpio = (gpio | GPIO_ST_HP) & ~GPIO_ST_HP_REAR;
		break;
	}
	oxygen_write16(chip, OXYGEN_GPIO_DATA, gpio);
	mutex_unlock(&chip->mutex);
	return gpio != gpio_old;
}

static const struct snd_kcontrol_new st_output_switch = {
	.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
	.name = "Analog Output",
	.info = st_output_switch_info,
	.get = st_output_switch_get,
	.put = st_output_switch_put,
};

static void xonar_line_mic_ac97_switch(struct oxygen *chip,
				       unsigned int reg, unsigned int mute)
{
	if (reg == AC97_LINE) {
		spin_lock_irq(&chip->reg_lock);
		oxygen_write16_masked(chip, OXYGEN_GPIO_DATA,
				      mute ? GPIO_DX_INPUT_ROUTE : 0,
				      GPIO_DX_INPUT_ROUTE);
		spin_unlock_irq(&chip->reg_lock);
	}
}

static const DECLARE_TLV_DB_SCALE(pcm1796_db_scale, -6000, 50, 0);
static const DECLARE_TLV_DB_SCALE(cs4362a_db_scale, -6000, 100, 0);

static int xonar_d2_control_filter(struct snd_kcontrol_new *template)
{
	if (!strncmp(template->name, "CD Capture ", 11))
		/* CD in is actually connected to the video in pin */
		template->private_value ^= AC97_CD ^ AC97_VIDEO;
	return 0;
}

static int xonar_d1_control_filter(struct snd_kcontrol_new *template)
{
	if (!strncmp(template->name, "CD Capture ", 11))
		return 1; /* no CD input */
	return 0;
}

static int xonar_st_control_filter(struct snd_kcontrol_new *template)
{
	if (!strncmp(template->name, "CD Capture ", 11))
		return 1; /* no CD input */
	if (!strcmp(template->name, "Stereo Upmixing"))
		return 1; /* stereo only - we don't need upmixing */
	return 0;
}

static int xonar_d2_mixer_init(struct oxygen *chip)
{
	return snd_ctl_add(chip->card, snd_ctl_new1(&alt_switch, chip));
}

static int xonar_d1_mixer_init(struct oxygen *chip)
{
	return snd_ctl_add(chip->card, snd_ctl_new1(&front_panel_switch, chip));
}

static int xonar_st_mixer_init(struct oxygen *chip)
{
	return snd_ctl_add(chip->card, snd_ctl_new1(&st_output_switch, chip));
}

static const struct oxygen_model model_xonar_d2 = {
	.longname = "Asus Virtuoso 200",
	.chip = "AV200",
	.init = xonar_d2_init,
	.control_filter = xonar_d2_control_filter,
	.mixer_init = xonar_d2_mixer_init,
	.cleanup = xonar_d2_cleanup,
	.suspend = xonar_d2_suspend,
	.resume = xonar_d2_resume,
	.set_dac_params = set_pcm1796_params,
	.set_adc_params = set_cs53x1_params,
	.update_dac_volume = update_pcm1796_volume,
	.update_dac_mute = update_pcm1796_mute,
	.dac_tlv = pcm1796_db_scale,
	.model_data_size = sizeof(struct xonar_data),
	.device_config = PLAYBACK_0_TO_I2S |
			 PLAYBACK_1_TO_SPDIF |
			 CAPTURE_0_FROM_I2S_2 |
			 CAPTURE_1_FROM_SPDIF |
			 MIDI_OUTPUT |
			 MIDI_INPUT,
	.dac_channels = 8,
	.dac_volume_min = 255 - 2*60,
	.dac_volume_max = 255,
	.misc_flags = OXYGEN_MISC_MIDI,
	.function_flags = OXYGEN_FUNCTION_SPI |
			  OXYGEN_FUNCTION_ENABLE_SPI_4_5,
	.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
};

static const struct oxygen_model model_xonar_d1 = {
	.longname = "Asus Virtuoso 100",
	.chip = "AV200",
	.init = xonar_d1_init,
	.control_filter = xonar_d1_control_filter,
	.mixer_init = xonar_d1_mixer_init,
	.cleanup = xonar_d1_cleanup,
	.suspend = xonar_d1_suspend,
	.resume = xonar_d1_resume,
	.set_dac_params = set_cs43xx_params,
	.set_adc_params = set_cs53x1_params,
	.update_dac_volume = update_cs43xx_volume,
	.update_dac_mute = update_cs43xx_mute,
	.ac97_switch = xonar_line_mic_ac97_switch,
	.dac_tlv = cs4362a_db_scale,
	.model_data_size = sizeof(struct xonar_data),
	.device_config = PLAYBACK_0_TO_I2S |
			 PLAYBACK_1_TO_SPDIF |
			 CAPTURE_0_FROM_I2S_2,
	.dac_channels = 8,
	.dac_volume_min = 127 - 60,
	.dac_volume_max = 127,
	.function_flags = OXYGEN_FUNCTION_2WIRE,
	.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
};

static const struct oxygen_model model_xonar_hdav = {
	.longname = "Asus Virtuoso 200",
	.chip = "AV200",
	.init = xonar_hdav_init,
	.cleanup = xonar_hdav_cleanup,
	.suspend = xonar_hdav_suspend,
	.resume = xonar_hdav_resume,
	.pcm_hardware_filter = xonar_hdav_pcm_hardware_filter,
	.set_dac_params = set_hdav_params,
	.set_adc_params = set_cs53x1_params,
	.update_dac_volume = update_pcm1796_volume,
	.update_dac_mute = update_pcm1796_mute,
	.uart_input = xonar_hdav_uart_input,
	.ac97_switch = xonar_line_mic_ac97_switch,
	.dac_tlv = pcm1796_db_scale,
	.model_data_size = sizeof(struct xonar_data),
	.device_config = PLAYBACK_0_TO_I2S |
			 PLAYBACK_1_TO_SPDIF |
			 CAPTURE_0_FROM_I2S_2,
	.dac_channels = 8,
	.dac_volume_min = 255 - 2*60,
	.dac_volume_max = 255,
	.misc_flags = OXYGEN_MISC_MIDI,
	.function_flags = OXYGEN_FUNCTION_2WIRE,
	.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
};

static const struct oxygen_model model_xonar_st = {
	.longname = "Asus Virtuoso 100",
	.chip = "AV200",
	.init = xonar_stx_init,
	.control_filter = xonar_st_control_filter,
	.mixer_init = xonar_st_mixer_init,
	.cleanup = xonar_st_cleanup,
	.suspend = xonar_st_suspend,
	.resume = xonar_st_resume,
	.set_dac_params = set_pcm1796_params,
	.set_adc_params = set_cs53x1_params,
	.update_dac_volume = update_pcm1796_volume,
	.update_dac_mute = update_pcm1796_mute,
	.ac97_switch = xonar_line_mic_ac97_switch,
	.dac_tlv = pcm1796_db_scale,
	.model_data_size = sizeof(struct xonar_data),
	.device_config = PLAYBACK_0_TO_I2S |
			 PLAYBACK_1_TO_SPDIF |
			 CAPTURE_0_FROM_I2S_2,
	.dac_channels = 2,
	.dac_volume_min = 255 - 2*60,
	.dac_volume_max = 255,
	.function_flags = OXYGEN_FUNCTION_2WIRE,
	.dac_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
	.adc_i2s_format = OXYGEN_I2S_FORMAT_LJUST,
};

static int __devinit get_xonar_model(struct oxygen *chip,
				     const struct pci_device_id *id)
{
	static const struct oxygen_model *const models[] = {
		[MODEL_D1]	= &model_xonar_d1,
		[MODEL_DX]	= &model_xonar_d1,
		[MODEL_D2]	= &model_xonar_d2,
		[MODEL_D2X]	= &model_xonar_d2,
		[MODEL_HDAV]	= &model_xonar_hdav,
		[MODEL_STX]	= &model_xonar_st,
	};
	static const char *const names[] = {
		[MODEL_D1]	= "Xonar D1",
		[MODEL_DX]	= "Xonar DX",
		[MODEL_D2]	= "Xonar D2",
		[MODEL_D2X]	= "Xonar D2X",
		[MODEL_HDAV]	= "Xonar HDAV1.3",
		[MODEL_HDAV_H6]	= "Xonar HDAV1.3+H6",
		[MODEL_STX]	= "Xonar Essence STX",
	};
	unsigned int model = id->driver_data;

	if (model >= ARRAY_SIZE(models) || !models[model])
		return -EINVAL;
	chip->model = *models[model];

	switch (model) {
	case MODEL_D2X:
		chip->model.init = xonar_d2x_init;
		break;
	case MODEL_DX:
		chip->model.init = xonar_dx_init;
		break;
	case MODEL_HDAV:
		oxygen_clear_bits16(chip, OXYGEN_GPIO_CONTROL,
				    GPIO_HDAV_DB_MASK);
		switch (oxygen_read16(chip, OXYGEN_GPIO_DATA) &
			GPIO_HDAV_DB_MASK) {
		case GPIO_HDAV_DB_H6:
			model = MODEL_HDAV_H6;
			break;
		case GPIO_HDAV_DB_XX:
			snd_printk(KERN_ERR "unknown daughterboard\n");
			return -ENODEV;
		}
		break;
	case MODEL_STX:
		oxygen_clear_bits16(chip, OXYGEN_GPIO_CONTROL,
				    GPIO_HDAV_DB_MASK);
		break;
	}

	chip->model.shortname = names[model];
	chip->model.private_data = model;
	return 0;
}

static int __devinit xonar_probe(struct pci_dev *pci,
				 const struct pci_device_id *pci_id)
{
	static int dev;
	int err;

	if (dev >= SNDRV_CARDS)
		return -ENODEV;
	if (!enable[dev]) {
		++dev;
		return -ENOENT;
	}
	err = oxygen_pci_probe(pci, index[dev], id[dev], THIS_MODULE,
			       xonar_ids, get_xonar_model);
	if (err >= 0)
		++dev;
	return err;
}

static struct pci_driver xonar_driver = {
	.name = "AV200",
	.id_table = xonar_ids,
	.probe = xonar_probe,
	.remove = __devexit_p(oxygen_pci_remove),
#ifdef CONFIG_PM
	.suspend = oxygen_pci_suspend,
	.resume = oxygen_pci_resume,
#endif
};

static int __init alsa_card_xonar_init(void)
{
	return pci_register_driver(&xonar_driver);
}

static void __exit alsa_card_xonar_exit(void)
{
	pci_unregister_driver(&xonar_driver);
}

module_init(alsa_card_xonar_init)
module_exit(alsa_card_xonar_exit)
