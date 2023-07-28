// SPDX-License-Identifier: GPL-2.0
//
// cs35l41.c -- CS35l41 ALSA SoC audio driver
//
// Copyright 2017-2021 Cirrus Logic, Inc.
//
// Author: David Rhodes <david.rhodes@cirrus.com>

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

#include "cs35l41.h"

static const char * const cs35l41_supplies[CS35L41_NUM_SUPPLIES] = {
	"VA",
	"VP",
};

struct cs35l41_pll_sysclk_config {
	int freq;
	int clk_cfg;
};

static const struct cs35l41_pll_sysclk_config cs35l41_pll_sysclk[] = {
	{ 32768,	0x00 },
	{ 8000,		0x01 },
	{ 11025,	0x02 },
	{ 12000,	0x03 },
	{ 16000,	0x04 },
	{ 22050,	0x05 },
	{ 24000,	0x06 },
	{ 32000,	0x07 },
	{ 44100,	0x08 },
	{ 48000,	0x09 },
	{ 88200,	0x0A },
	{ 96000,	0x0B },
	{ 128000,	0x0C },
	{ 176400,	0x0D },
	{ 192000,	0x0E },
	{ 256000,	0x0F },
	{ 352800,	0x10 },
	{ 384000,	0x11 },
	{ 512000,	0x12 },
	{ 705600,	0x13 },
	{ 750000,	0x14 },
	{ 768000,	0x15 },
	{ 1000000,	0x16 },
	{ 1024000,	0x17 },
	{ 1200000,	0x18 },
	{ 1411200,	0x19 },
	{ 1500000,	0x1A },
	{ 1536000,	0x1B },
	{ 2000000,	0x1C },
	{ 2048000,	0x1D },
	{ 2400000,	0x1E },
	{ 2822400,	0x1F },
	{ 3000000,	0x20 },
	{ 3072000,	0x21 },
	{ 3200000,	0x22 },
	{ 4000000,	0x23 },
	{ 4096000,	0x24 },
	{ 4800000,	0x25 },
	{ 5644800,	0x26 },
	{ 6000000,	0x27 },
	{ 6144000,	0x28 },
	{ 6250000,	0x29 },
	{ 6400000,	0x2A },
	{ 6500000,	0x2B },
	{ 6750000,	0x2C },
	{ 7526400,	0x2D },
	{ 8000000,	0x2E },
	{ 8192000,	0x2F },
	{ 9600000,	0x30 },
	{ 11289600,	0x31 },
	{ 12000000,	0x32 },
	{ 12288000,	0x33 },
	{ 12500000,	0x34 },
	{ 12800000,	0x35 },
	{ 13000000,	0x36 },
	{ 13500000,	0x37 },
	{ 19200000,	0x38 },
	{ 22579200,	0x39 },
	{ 24000000,	0x3A },
	{ 24576000,	0x3B },
	{ 25000000,	0x3C },
	{ 25600000,	0x3D },
	{ 26000000,	0x3E },
	{ 27000000,	0x3F },
};

struct cs35l41_fs_mon_config {
	int freq;
	unsigned int fs1;
	unsigned int fs2;
};

static const struct cs35l41_fs_mon_config cs35l41_fs_mon[] = {
	{ 32768,	2254,	3754 },
	{ 8000,		9220,	15364 },
	{ 11025,	6148,	10244 },
	{ 12000,	6148,	10244 },
	{ 16000,	4612,	7684 },
	{ 22050,	3076,	5124 },
	{ 24000,	3076,	5124 },
	{ 32000,	2308,	3844 },
	{ 44100,	1540,	2564 },
	{ 48000,	1540,	2564 },
	{ 88200,	772,	1284 },
	{ 96000,	772,	1284 },
	{ 128000,	580,	964 },
	{ 176400,	388,	644 },
	{ 192000,	388,	644 },
	{ 256000,	292,	484 },
	{ 352800,	196,	324 },
	{ 384000,	196,	324 },
	{ 512000,	148,	244 },
	{ 705600,	100,	164 },
	{ 750000,	100,	164 },
	{ 768000,	100,	164 },
	{ 1000000,	76,	124 },
	{ 1024000,	76,	124 },
	{ 1200000,	64,	104 },
	{ 1411200,	52,	84 },
	{ 1500000,	52,	84 },
	{ 1536000,	52,	84 },
	{ 2000000,	40,	64 },
	{ 2048000,	40,	64 },
	{ 2400000,	34,	54 },
	{ 2822400,	28,	44 },
	{ 3000000,	28,	44 },
	{ 3072000,	28,	44 },
	{ 3200000,	27,	42 },
	{ 4000000,	22,	34 },
	{ 4096000,	22,	34 },
	{ 4800000,	19,	29 },
	{ 5644800,	16,	24 },
	{ 6000000,	16,	24 },
	{ 6144000,	16,	24 },
};

static int cs35l41_get_fs_mon_config_index(int freq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs35l41_fs_mon); i++) {
		if (cs35l41_fs_mon[i].freq == freq)
			return i;
	}

	return -EINVAL;
}

static const DECLARE_TLV_DB_RANGE(dig_vol_tlv,
		0, 0, TLV_DB_SCALE_ITEM(TLV_DB_GAIN_MUTE, 0, 1),
		1, 913, TLV_DB_MINMAX_ITEM(-10200, 1200));
static DECLARE_TLV_DB_SCALE(amp_gain_tlv, 0, 1, 1);

static const struct snd_kcontrol_new dre_ctrl =
	SOC_DAPM_SINGLE("Switch", CS35L41_PWR_CTRL3, 20, 1, 0);

static const char * const cs35l41_pcm_sftramp_text[] =  {
	"Off", ".5ms", "1ms", "2ms", "4ms", "8ms", "15ms", "30ms"
};

static SOC_ENUM_SINGLE_DECL(pcm_sft_ramp,
			    CS35L41_AMP_DIG_VOL_CTRL, 0,
			    cs35l41_pcm_sftramp_text);

static int cs35l41_dsp_preload_ev(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(component);
	int ret;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		if (cs35l41->dsp.booted)
			return 0;

		return wm_adsp_early_event(w, kcontrol, event);
	case SND_SOC_DAPM_PRE_PMD:
		if (cs35l41->dsp.preloaded)
			return 0;

		if (cs35l41->dsp.running) {
			ret = wm_adsp_event(w, kcontrol, event);
			if (ret)
				return ret;
		}

		return wm_adsp_early_event(w, kcontrol, event);
	default:
		return 0;
	}
}

static bool cs35l41_check_cspl_mbox_sts(enum cs35l41_cspl_mbox_cmd cmd,
					enum cs35l41_cspl_mbox_status sts)
{
	switch (cmd) {
	case CSPL_MBOX_CMD_NONE:
	case CSPL_MBOX_CMD_UNKNOWN_CMD:
		return true;
	case CSPL_MBOX_CMD_PAUSE:
	case CSPL_MBOX_CMD_OUT_OF_HIBERNATE:
		return (sts == CSPL_MBOX_STS_PAUSED);
	case CSPL_MBOX_CMD_RESUME:
		return (sts == CSPL_MBOX_STS_RUNNING);
	case CSPL_MBOX_CMD_REINIT:
		return (sts == CSPL_MBOX_STS_RUNNING);
	case CSPL_MBOX_CMD_STOP_PRE_REINIT:
		return (sts == CSPL_MBOX_STS_RDY_FOR_REINIT);
	default:
		return false;
	}
}

static int cs35l41_set_cspl_mbox_cmd(struct cs35l41_private *cs35l41,
				     enum cs35l41_cspl_mbox_cmd cmd)
{
	unsigned int sts = 0, i;
	int ret;

	// Set mailbox cmd
	ret = regmap_write(cs35l41->regmap, CS35L41_DSP_VIRT1_MBOX_1, cmd);
	if (ret < 0) {
		if (cmd != CSPL_MBOX_CMD_OUT_OF_HIBERNATE)
			dev_err(cs35l41->dev, "Failed to write MBOX: %d\n", ret);
		return ret;
	}

	// Read mailbox status and verify it is appropriate for the given cmd
	for (i = 0; i < 5; i++) {
		usleep_range(1000, 1100);

		ret = regmap_read(cs35l41->regmap, CS35L41_DSP_MBOX_2, &sts);
		if (ret < 0) {
			dev_err(cs35l41->dev, "Failed to read MBOX STS: %d\n", ret);
			continue;
		}

		if (!cs35l41_check_cspl_mbox_sts(cmd, sts)) {
			dev_dbg(cs35l41->dev,
				"[%u] cmd %u returned invalid sts %u",
				i, cmd, sts);
		} else {
			return 0;
		}
	}

	dev_err(cs35l41->dev,
		"Failed to set mailbox cmd %u (status %u)\n",
		cmd, sts);

	return -ENOMSG;
}

static int cs35l41_dsp_audio_ev(struct snd_soc_dapm_widget *w,
				struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(component);
	unsigned int fw_status;
	int ret;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (!cs35l41->dsp.running)
			return wm_adsp_event(w, kcontrol, event);

		ret = regmap_read(cs35l41->regmap, CS35L41_DSP_MBOX_2, &fw_status);
		if (ret < 0) {
			dev_err(cs35l41->dev,
				"Failed to read firmware status: %d\n", ret);
			return ret;
		}

		switch (fw_status) {
		case CSPL_MBOX_STS_RUNNING:
		case CSPL_MBOX_STS_PAUSED:
			break;
		default:
			dev_err(cs35l41->dev, "Firmware status is invalid: %u\n",
				fw_status);
			return -EINVAL;
		}

		return cs35l41_set_cspl_mbox_cmd(cs35l41, CSPL_MBOX_CMD_RESUME);
	case SND_SOC_DAPM_PRE_PMD:
		return cs35l41_set_cspl_mbox_cmd(cs35l41, CSPL_MBOX_CMD_PAUSE);
	default:
		return 0;
	}
}

static const char * const cs35l41_pcm_source_texts[] = {"ASP", "DSP"};
static const unsigned int cs35l41_pcm_source_values[] = {0x08, 0x32};
static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_pcm_source_enum,
				  CS35L41_DAC_PCM1_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_pcm_source_texts,
				  cs35l41_pcm_source_values);

static const struct snd_kcontrol_new pcm_source_mux =
	SOC_DAPM_ENUM("PCM Source", cs35l41_pcm_source_enum);

static const char * const cs35l41_tx_input_texts[] = {
	"Zero", "ASPRX1", "ASPRX2", "VMON", "IMON",
	"VPMON", "VBSTMON", "DSPTX1", "DSPTX2"
};

static const unsigned int cs35l41_tx_input_values[] = {
	0x00, CS35L41_INPUT_SRC_ASPRX1, CS35L41_INPUT_SRC_ASPRX2,
	CS35L41_INPUT_SRC_VMON, CS35L41_INPUT_SRC_IMON, CS35L41_INPUT_SRC_VPMON,
	CS35L41_INPUT_SRC_VBSTMON, CS35L41_INPUT_DSP_TX1, CS35L41_INPUT_DSP_TX2
};

static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_asptx1_enum,
				  CS35L41_ASP_TX1_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_tx_input_texts,
				  cs35l41_tx_input_values);

static const struct snd_kcontrol_new asp_tx1_mux =
	SOC_DAPM_ENUM("ASPTX1 SRC", cs35l41_asptx1_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_asptx2_enum,
				  CS35L41_ASP_TX2_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_tx_input_texts,
				  cs35l41_tx_input_values);

static const struct snd_kcontrol_new asp_tx2_mux =
	SOC_DAPM_ENUM("ASPTX2 SRC", cs35l41_asptx2_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_asptx3_enum,
				  CS35L41_ASP_TX3_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_tx_input_texts,
				  cs35l41_tx_input_values);

static const struct snd_kcontrol_new asp_tx3_mux =
	SOC_DAPM_ENUM("ASPTX3 SRC", cs35l41_asptx3_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_asptx4_enum,
				  CS35L41_ASP_TX4_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_tx_input_texts,
				  cs35l41_tx_input_values);

static const struct snd_kcontrol_new asp_tx4_mux =
	SOC_DAPM_ENUM("ASPTX4 SRC", cs35l41_asptx4_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_dsprx1_enum,
				  CS35L41_DSP1_RX1_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_tx_input_texts,
				  cs35l41_tx_input_values);

static const struct snd_kcontrol_new dsp_rx1_mux =
	SOC_DAPM_ENUM("DSPRX1 SRC", cs35l41_dsprx1_enum);

static SOC_VALUE_ENUM_SINGLE_DECL(cs35l41_dsprx2_enum,
				  CS35L41_DSP1_RX2_SRC,
				  0, CS35L41_ASP_SOURCE_MASK,
				  cs35l41_tx_input_texts,
				  cs35l41_tx_input_values);

static const struct snd_kcontrol_new dsp_rx2_mux =
	SOC_DAPM_ENUM("DSPRX2 SRC", cs35l41_dsprx2_enum);

static const struct snd_kcontrol_new cs35l41_aud_controls[] = {
	SOC_SINGLE_SX_TLV("Digital PCM Volume", CS35L41_AMP_DIG_VOL_CTRL,
			  3, 0x4CF, 0x391, dig_vol_tlv),
	SOC_SINGLE_TLV("Analog PCM Volume", CS35L41_AMP_GAIN_CTRL, 5, 0x14, 0,
		       amp_gain_tlv),
	SOC_ENUM("PCM Soft Ramp", pcm_sft_ramp),
	SOC_SINGLE("HW Noise Gate Enable", CS35L41_NG_CFG, 8, 63, 0),
	SOC_SINGLE("HW Noise Gate Delay", CS35L41_NG_CFG, 4, 7, 0),
	SOC_SINGLE("HW Noise Gate Threshold", CS35L41_NG_CFG, 0, 7, 0),
	SOC_SINGLE("Aux Noise Gate CH1 Enable",
		   CS35L41_MIXER_NGATE_CH1_CFG, 16, 1, 0),
	SOC_SINGLE("Aux Noise Gate CH1 Entry Delay",
		   CS35L41_MIXER_NGATE_CH1_CFG, 8, 15, 0),
	SOC_SINGLE("Aux Noise Gate CH1 Threshold",
		   CS35L41_MIXER_NGATE_CH1_CFG, 0, 7, 0),
	SOC_SINGLE("Aux Noise Gate CH2 Entry Delay",
		   CS35L41_MIXER_NGATE_CH2_CFG, 8, 15, 0),
	SOC_SINGLE("Aux Noise Gate CH2 Enable",
		   CS35L41_MIXER_NGATE_CH2_CFG, 16, 1, 0),
	SOC_SINGLE("Aux Noise Gate CH2 Threshold",
		   CS35L41_MIXER_NGATE_CH2_CFG, 0, 7, 0),
	SOC_SINGLE("SCLK Force", CS35L41_SP_FORMAT, CS35L41_SCLK_FRC_SHIFT, 1, 0),
	SOC_SINGLE("LRCLK Force", CS35L41_SP_FORMAT, CS35L41_LRCLK_FRC_SHIFT, 1, 0),
	SOC_SINGLE("Invert Class D", CS35L41_AMP_DIG_VOL_CTRL,
		   CS35L41_AMP_INV_PCM_SHIFT, 1, 0),
	SOC_SINGLE("Amp Gain ZC", CS35L41_AMP_GAIN_CTRL,
		   CS35L41_AMP_GAIN_ZC_SHIFT, 1, 0),
	WM_ADSP2_PRELOAD_SWITCH("DSP1", 1),
	WM_ADSP_FW_CONTROL("DSP1", 0),
};

static irqreturn_t cs35l41_irq(int irq, void *data)
{
	struct cs35l41_private *cs35l41 = data;
	unsigned int status[4] = { 0, 0, 0, 0 };
	unsigned int masks[4] = { 0, 0, 0, 0 };
	int ret = IRQ_NONE;
	unsigned int i;

	pm_runtime_get_sync(cs35l41->dev);

	for (i = 0; i < ARRAY_SIZE(status); i++) {
		regmap_read(cs35l41->regmap,
			    CS35L41_IRQ1_STATUS1 + (i * CS35L41_REGSTRIDE),
			    &status[i]);
		regmap_read(cs35l41->regmap,
			    CS35L41_IRQ1_MASK1 + (i * CS35L41_REGSTRIDE),
			    &masks[i]);
	}

	/* Check to see if unmasked bits are active */
	if (!(status[0] & ~masks[0]) && !(status[1] & ~masks[1]) &&
	    !(status[2] & ~masks[2]) && !(status[3] & ~masks[3]))
		goto done;

	if (status[3] & CS35L41_OTP_BOOT_DONE) {
		regmap_update_bits(cs35l41->regmap, CS35L41_IRQ1_MASK4,
				   CS35L41_OTP_BOOT_DONE, CS35L41_OTP_BOOT_DONE);
	}

	/*
	 * The following interrupts require a
	 * protection release cycle to get the
	 * speaker out of Safe-Mode.
	 */
	if (status[0] & CS35L41_AMP_SHORT_ERR) {
		dev_crit_ratelimited(cs35l41->dev, "Amp short error\n");
		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_AMP_SHORT_ERR);
		regmap_write(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_AMP_SHORT_ERR_RLS,
				   CS35L41_AMP_SHORT_ERR_RLS);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_AMP_SHORT_ERR_RLS, 0);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L41_TEMP_WARN) {
		dev_crit_ratelimited(cs35l41->dev, "Over temperature warning\n");
		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_TEMP_WARN);
		regmap_write(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_TEMP_WARN_ERR_RLS,
				   CS35L41_TEMP_WARN_ERR_RLS);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_TEMP_WARN_ERR_RLS, 0);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L41_TEMP_ERR) {
		dev_crit_ratelimited(cs35l41->dev, "Over temperature error\n");
		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_TEMP_ERR);
		regmap_write(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_TEMP_ERR_RLS,
				   CS35L41_TEMP_ERR_RLS);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_TEMP_ERR_RLS, 0);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L41_BST_OVP_ERR) {
		dev_crit_ratelimited(cs35l41->dev, "VBST Over Voltage error\n");
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK, 0);
		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_BST_OVP_ERR);
		regmap_write(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_BST_OVP_ERR_RLS,
				   CS35L41_BST_OVP_ERR_RLS);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_BST_OVP_ERR_RLS, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK,
				   CS35L41_BST_EN_DEFAULT << CS35L41_BST_EN_SHIFT);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L41_BST_DCM_UVP_ERR) {
		dev_crit_ratelimited(cs35l41->dev, "DCM VBST Under Voltage Error\n");
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK, 0);
		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_BST_DCM_UVP_ERR);
		regmap_write(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_BST_UVP_ERR_RLS,
				   CS35L41_BST_UVP_ERR_RLS);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_BST_UVP_ERR_RLS, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK,
				   CS35L41_BST_EN_DEFAULT << CS35L41_BST_EN_SHIFT);
		ret = IRQ_HANDLED;
	}

	if (status[0] & CS35L41_BST_SHORT_ERR) {
		dev_crit_ratelimited(cs35l41->dev, "LBST error: powering off!\n");
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK, 0);
		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_BST_SHORT_ERR);
		regmap_write(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_BST_SHORT_ERR_RLS,
				   CS35L41_BST_SHORT_ERR_RLS);
		regmap_update_bits(cs35l41->regmap, CS35L41_PROTECT_REL_ERR_IGN,
				   CS35L41_BST_SHORT_ERR_RLS, 0);
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL2,
				   CS35L41_BST_EN_MASK,
				   CS35L41_BST_EN_DEFAULT << CS35L41_BST_EN_SHIFT);
		ret = IRQ_HANDLED;
	}

done:
	pm_runtime_mark_last_busy(cs35l41->dev);
	pm_runtime_put_autosuspend(cs35l41->dev);

	return ret;
}

static const struct reg_sequence cs35l41_pup_patch[] = {
	{ CS35L41_TEST_KEY_CTL, 0x00000055 },
	{ CS35L41_TEST_KEY_CTL, 0x000000AA },
	{ 0x00002084, 0x002F1AA0 },
	{ CS35L41_TEST_KEY_CTL, 0x000000CC },
	{ CS35L41_TEST_KEY_CTL, 0x00000033 },
};

static const struct reg_sequence cs35l41_pdn_patch[] = {
	{ CS35L41_TEST_KEY_CTL, 0x00000055 },
	{ CS35L41_TEST_KEY_CTL, 0x000000AA },
	{ 0x00002084, 0x002F1AA3 },
	{ CS35L41_TEST_KEY_CTL, 0x000000CC },
	{ CS35L41_TEST_KEY_CTL, 0x00000033 },
};

static int cs35l41_main_amp_event(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_component *component = snd_soc_dapm_to_component(w->dapm);
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(component);
	unsigned int val;
	int ret = 0;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		regmap_multi_reg_write_bypassed(cs35l41->regmap,
						cs35l41_pup_patch,
						ARRAY_SIZE(cs35l41_pup_patch));

		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL1,
				   CS35L41_GLOBAL_EN_MASK,
				   1 << CS35L41_GLOBAL_EN_SHIFT);

		usleep_range(1000, 1100);
		break;
	case SND_SOC_DAPM_POST_PMD:
		regmap_update_bits(cs35l41->regmap, CS35L41_PWR_CTRL1,
				   CS35L41_GLOBAL_EN_MASK, 0);

		ret = regmap_read_poll_timeout(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
					       val, val &  CS35L41_PDN_DONE_MASK,
					       1000, 100000);
		if (ret)
			dev_warn(cs35l41->dev, "PDN failed: %d\n", ret);

		regmap_write(cs35l41->regmap, CS35L41_IRQ1_STATUS1,
			     CS35L41_PDN_DONE_MASK);

		regmap_multi_reg_write_bypassed(cs35l41->regmap,
						cs35l41_pdn_patch,
						ARRAY_SIZE(cs35l41_pdn_patch));
		break;
	default:
		dev_err(cs35l41->dev, "Invalid event = 0x%x\n", event);
		ret = -EINVAL;
	}

	return ret;
}

static const struct snd_soc_dapm_widget cs35l41_dapm_widgets[] = {
	SND_SOC_DAPM_SPK("DSP1 Preload", NULL),
	SND_SOC_DAPM_SUPPLY_S("DSP1 Preloader", 100, SND_SOC_NOPM, 0, 0,
			      cs35l41_dsp_preload_ev,
			      SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_OUT_DRV_E("DSP1", SND_SOC_NOPM, 0, 0, NULL, 0,
			       cs35l41_dsp_audio_ev,
			       SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),

	SND_SOC_DAPM_OUTPUT("SPK"),

	SND_SOC_DAPM_AIF_IN("ASPRX1", NULL, 0, CS35L41_SP_ENABLES, 16, 0),
	SND_SOC_DAPM_AIF_IN("ASPRX2", NULL, 0, CS35L41_SP_ENABLES, 17, 0),
	SND_SOC_DAPM_AIF_OUT("ASPTX1", NULL, 0, CS35L41_SP_ENABLES, 0, 0),
	SND_SOC_DAPM_AIF_OUT("ASPTX2", NULL, 0, CS35L41_SP_ENABLES, 1, 0),
	SND_SOC_DAPM_AIF_OUT("ASPTX3", NULL, 0, CS35L41_SP_ENABLES, 2, 0),
	SND_SOC_DAPM_AIF_OUT("ASPTX4", NULL, 0, CS35L41_SP_ENABLES, 3, 0),

	SND_SOC_DAPM_SIGGEN("VSENSE"),
	SND_SOC_DAPM_SIGGEN("ISENSE"),
	SND_SOC_DAPM_SIGGEN("VP"),
	SND_SOC_DAPM_SIGGEN("VBST"),
	SND_SOC_DAPM_SIGGEN("TEMP"),

	SND_SOC_DAPM_SUPPLY("VMON", CS35L41_PWR_CTRL2, 12, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("IMON", CS35L41_PWR_CTRL2, 13, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("VPMON", CS35L41_PWR_CTRL2, 8, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("VBSTMON", CS35L41_PWR_CTRL2, 9, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("TEMPMON", CS35L41_PWR_CTRL2, 10, 0, NULL, 0),

	SND_SOC_DAPM_ADC("VMON ADC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("IMON ADC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("VPMON ADC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("VBSTMON ADC", NULL, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_ADC("TEMPMON ADC", NULL, SND_SOC_NOPM, 0, 0),

	SND_SOC_DAPM_ADC("CLASS H", NULL, CS35L41_PWR_CTRL3, 4, 0),

	SND_SOC_DAPM_OUT_DRV_E("Main AMP", CS35L41_PWR_CTRL2, 0, 0, NULL, 0,
			       cs35l41_main_amp_event,
			       SND_SOC_DAPM_POST_PMD |	SND_SOC_DAPM_POST_PMU),

	SND_SOC_DAPM_MUX("ASP TX1 Source", SND_SOC_NOPM, 0, 0, &asp_tx1_mux),
	SND_SOC_DAPM_MUX("ASP TX2 Source", SND_SOC_NOPM, 0, 0, &asp_tx2_mux),
	SND_SOC_DAPM_MUX("ASP TX3 Source", SND_SOC_NOPM, 0, 0, &asp_tx3_mux),
	SND_SOC_DAPM_MUX("ASP TX4 Source", SND_SOC_NOPM, 0, 0, &asp_tx4_mux),
	SND_SOC_DAPM_MUX("DSP RX1 Source", SND_SOC_NOPM, 0, 0, &dsp_rx1_mux),
	SND_SOC_DAPM_MUX("DSP RX2 Source", SND_SOC_NOPM, 0, 0, &dsp_rx2_mux),
	SND_SOC_DAPM_MUX("PCM Source", SND_SOC_NOPM, 0, 0, &pcm_source_mux),
	SND_SOC_DAPM_SWITCH("DRE", SND_SOC_NOPM, 0, 0, &dre_ctrl),
};

static const struct snd_soc_dapm_route cs35l41_audio_map[] = {
	{"DSP RX1 Source", "ASPRX1", "ASPRX1"},
	{"DSP RX1 Source", "ASPRX2", "ASPRX2"},
	{"DSP RX2 Source", "ASPRX1", "ASPRX1"},
	{"DSP RX2 Source", "ASPRX2", "ASPRX2"},

	{"DSP1", NULL, "DSP RX1 Source"},
	{"DSP1", NULL, "DSP RX2 Source"},

	{"ASP TX1 Source", "VMON", "VMON ADC"},
	{"ASP TX1 Source", "IMON", "IMON ADC"},
	{"ASP TX1 Source", "VPMON", "VPMON ADC"},
	{"ASP TX1 Source", "VBSTMON", "VBSTMON ADC"},
	{"ASP TX1 Source", "DSPTX1", "DSP1"},
	{"ASP TX1 Source", "DSPTX2", "DSP1"},
	{"ASP TX1 Source", "ASPRX1", "ASPRX1" },
	{"ASP TX1 Source", "ASPRX2", "ASPRX2" },
	{"ASP TX2 Source", "VMON", "VMON ADC"},
	{"ASP TX2 Source", "IMON", "IMON ADC"},
	{"ASP TX2 Source", "VPMON", "VPMON ADC"},
	{"ASP TX2 Source", "VBSTMON", "VBSTMON ADC"},
	{"ASP TX2 Source", "DSPTX1", "DSP1"},
	{"ASP TX2 Source", "DSPTX2", "DSP1"},
	{"ASP TX2 Source", "ASPRX1", "ASPRX1" },
	{"ASP TX2 Source", "ASPRX2", "ASPRX2" },
	{"ASP TX3 Source", "VMON", "VMON ADC"},
	{"ASP TX3 Source", "IMON", "IMON ADC"},
	{"ASP TX3 Source", "VPMON", "VPMON ADC"},
	{"ASP TX3 Source", "VBSTMON", "VBSTMON ADC"},
	{"ASP TX3 Source", "DSPTX1", "DSP1"},
	{"ASP TX3 Source", "DSPTX2", "DSP1"},
	{"ASP TX3 Source", "ASPRX1", "ASPRX1" },
	{"ASP TX3 Source", "ASPRX2", "ASPRX2" },
	{"ASP TX4 Source", "VMON", "VMON ADC"},
	{"ASP TX4 Source", "IMON", "IMON ADC"},
	{"ASP TX4 Source", "VPMON", "VPMON ADC"},
	{"ASP TX4 Source", "VBSTMON", "VBSTMON ADC"},
	{"ASP TX4 Source", "DSPTX1", "DSP1"},
	{"ASP TX4 Source", "DSPTX2", "DSP1"},
	{"ASP TX4 Source", "ASPRX1", "ASPRX1" },
	{"ASP TX4 Source", "ASPRX2", "ASPRX2" },
	{"ASPTX1", NULL, "ASP TX1 Source"},
	{"ASPTX2", NULL, "ASP TX2 Source"},
	{"ASPTX3", NULL, "ASP TX3 Source"},
	{"ASPTX4", NULL, "ASP TX4 Source"},
	{"AMP Capture", NULL, "ASPTX1"},
	{"AMP Capture", NULL, "ASPTX2"},
	{"AMP Capture", NULL, "ASPTX3"},
	{"AMP Capture", NULL, "ASPTX4"},

	{"DSP1", NULL, "VMON"},
	{"DSP1", NULL, "IMON"},
	{"DSP1", NULL, "VPMON"},
	{"DSP1", NULL, "VBSTMON"},
	{"DSP1", NULL, "TEMPMON"},

	{"VMON ADC", NULL, "VMON"},
	{"IMON ADC", NULL, "IMON"},
	{"VPMON ADC", NULL, "VPMON"},
	{"VBSTMON ADC", NULL, "VBSTMON"},
	{"TEMPMON ADC", NULL, "TEMPMON"},

	{"VMON ADC", NULL, "VSENSE"},
	{"IMON ADC", NULL, "ISENSE"},
	{"VPMON ADC", NULL, "VP"},
	{"VBSTMON ADC", NULL, "VBST"},
	{"TEMPMON ADC", NULL, "TEMP"},

	{"DSP1 Preload", NULL, "DSP1 Preloader"},
	{"DSP1", NULL, "DSP1 Preloader"},

	{"ASPRX1", NULL, "AMP Playback"},
	{"ASPRX2", NULL, "AMP Playback"},
	{"DRE", "Switch", "CLASS H"},
	{"Main AMP", NULL, "CLASS H"},
	{"Main AMP", NULL, "DRE"},
	{"SPK", NULL, "Main AMP"},

	{"PCM Source", "ASP", "ASPRX1"},
	{"PCM Source", "DSP", "DSP1"},
	{"CLASS H", NULL, "PCM Source"},
};

static const struct wm_adsp_region cs35l41_dsp1_regions[] = {
	{ .type = WMFW_HALO_PM_PACKED,	.base = CS35L41_DSP1_PMEM_0 },
	{ .type = WMFW_HALO_XM_PACKED,	.base = CS35L41_DSP1_XMEM_PACK_0 },
	{ .type = WMFW_HALO_YM_PACKED,	.base = CS35L41_DSP1_YMEM_PACK_0 },
	{. type = WMFW_ADSP2_XM,	.base = CS35L41_DSP1_XMEM_UNPACK24_0},
	{. type = WMFW_ADSP2_YM,	.base = CS35L41_DSP1_YMEM_UNPACK24_0},
};

static int cs35l41_set_channel_map(struct snd_soc_dai *dai, unsigned int tx_n,
				   unsigned int *tx_slot, unsigned int rx_n, unsigned int *rx_slot)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(dai->component);

	return cs35l41_set_channels(cs35l41->dev, cs35l41->regmap, tx_n, tx_slot, rx_n, rx_slot);
}

static int cs35l41_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(dai->component);
	unsigned int daifmt = 0;

	switch (fmt & SND_SOC_DAIFMT_CLOCK_PROVIDER_MASK) {
	case SND_SOC_DAIFMT_CBP_CFP:
		daifmt |= CS35L41_SCLK_MSTR_MASK | CS35L41_LRCLK_MSTR_MASK;
		break;
	case SND_SOC_DAIFMT_CBC_CFC:
		break;
	default:
		dev_warn(cs35l41->dev, "Mixed provider/consumer mode unsupported\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
		break;
	case SND_SOC_DAIFMT_I2S:
		daifmt |= 2 << CS35L41_ASP_FMT_SHIFT;
		break;
	default:
		dev_warn(cs35l41->dev, "Invalid or unsupported DAI format\n");
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_IF:
		daifmt |= CS35L41_LRCLK_INV_MASK;
		break;
	case SND_SOC_DAIFMT_IB_NF:
		daifmt |= CS35L41_SCLK_INV_MASK;
		break;
	case SND_SOC_DAIFMT_IB_IF:
		daifmt |= CS35L41_LRCLK_INV_MASK | CS35L41_SCLK_INV_MASK;
		break;
	case SND_SOC_DAIFMT_NB_NF:
		break;
	default:
		dev_warn(cs35l41->dev, "Invalid DAI clock INV\n");
		return -EINVAL;
	}

	return regmap_update_bits(cs35l41->regmap, CS35L41_SP_FORMAT,
				  CS35L41_SCLK_MSTR_MASK | CS35L41_LRCLK_MSTR_MASK |
				  CS35L41_ASP_FMT_MASK | CS35L41_LRCLK_INV_MASK |
				  CS35L41_SCLK_INV_MASK, daifmt);
}

struct cs35l41_global_fs_config {
	int rate;
	int fs_cfg;
};

static const struct cs35l41_global_fs_config cs35l41_fs_rates[] = {
	{ 12000,	0x01 },
	{ 24000,	0x02 },
	{ 48000,	0x03 },
	{ 96000,	0x04 },
	{ 192000,	0x05 },
	{ 11025,	0x09 },
	{ 22050,	0x0A },
	{ 44100,	0x0B },
	{ 88200,	0x0C },
	{ 176400,	0x0D },
	{ 8000,		0x11 },
	{ 16000,	0x12 },
	{ 32000,	0x13 },
};

static int cs35l41_pcm_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params,
				 struct snd_soc_dai *dai)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(dai->component);
	unsigned int rate = params_rate(params);
	u8 asp_wl;
	int i;

	for (i = 0; i < ARRAY_SIZE(cs35l41_fs_rates); i++) {
		if (rate == cs35l41_fs_rates[i].rate)
			break;
	}

	if (i >= ARRAY_SIZE(cs35l41_fs_rates)) {
		dev_err(cs35l41->dev, "Unsupported rate: %u\n", rate);
		return -EINVAL;
	}

	asp_wl = params_width(params);

	if (i < ARRAY_SIZE(cs35l41_fs_rates))
		regmap_update_bits(cs35l41->regmap, CS35L41_GLOBAL_CLK_CTRL,
				   CS35L41_GLOBAL_FS_MASK,
				   cs35l41_fs_rates[i].fs_cfg << CS35L41_GLOBAL_FS_SHIFT);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		regmap_update_bits(cs35l41->regmap, CS35L41_SP_FORMAT,
				   CS35L41_ASP_WIDTH_RX_MASK,
				   asp_wl << CS35L41_ASP_WIDTH_RX_SHIFT);
		regmap_update_bits(cs35l41->regmap, CS35L41_SP_RX_WL,
				   CS35L41_ASP_RX_WL_MASK,
				   asp_wl << CS35L41_ASP_RX_WL_SHIFT);
	} else {
		regmap_update_bits(cs35l41->regmap, CS35L41_SP_FORMAT,
				   CS35L41_ASP_WIDTH_TX_MASK,
				   asp_wl << CS35L41_ASP_WIDTH_TX_SHIFT);
		regmap_update_bits(cs35l41->regmap, CS35L41_SP_TX_WL,
				   CS35L41_ASP_TX_WL_MASK,
				   asp_wl << CS35L41_ASP_TX_WL_SHIFT);
	}

	return 0;
}

static int cs35l41_get_clk_config(int freq)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(cs35l41_pll_sysclk); i++) {
		if (cs35l41_pll_sysclk[i].freq == freq)
			return cs35l41_pll_sysclk[i].clk_cfg;
	}

	return -EINVAL;
}

static const unsigned int cs35l41_src_rates[] = {
	8000, 12000, 11025, 16000, 22050, 24000, 32000,
	44100, 48000, 88200, 96000, 176400, 192000
};

static const struct snd_pcm_hw_constraint_list cs35l41_constraints = {
	.count = ARRAY_SIZE(cs35l41_src_rates),
	.list = cs35l41_src_rates,
};

static int cs35l41_pcm_startup(struct snd_pcm_substream *substream,
			       struct snd_soc_dai *dai)
{
	if (substream->runtime)
		return snd_pcm_hw_constraint_list(substream->runtime, 0,
						  SNDRV_PCM_HW_PARAM_RATE,
						  &cs35l41_constraints);
	return 0;
}

static int cs35l41_component_set_sysclk(struct snd_soc_component *component,
					int clk_id, int source,
					unsigned int freq, int dir)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(component);
	int extclk_cfg, clksrc;

	switch (clk_id) {
	case CS35L41_CLKID_SCLK:
		clksrc = CS35L41_PLLSRC_SCLK;
		break;
	case CS35L41_CLKID_LRCLK:
		clksrc = CS35L41_PLLSRC_LRCLK;
		break;
	case CS35L41_CLKID_MCLK:
		clksrc = CS35L41_PLLSRC_MCLK;
		break;
	default:
		dev_err(cs35l41->dev, "Invalid CLK Config\n");
		return -EINVAL;
	}

	extclk_cfg = cs35l41_get_clk_config(freq);

	if (extclk_cfg < 0) {
		dev_err(cs35l41->dev, "Invalid CLK Config: %d, freq: %u\n",
			extclk_cfg, freq);
		return -EINVAL;
	}

	regmap_update_bits(cs35l41->regmap, CS35L41_PLL_CLK_CTRL,
			   CS35L41_PLL_OPENLOOP_MASK,
			   1 << CS35L41_PLL_OPENLOOP_SHIFT);
	regmap_update_bits(cs35l41->regmap, CS35L41_PLL_CLK_CTRL,
			   CS35L41_REFCLK_FREQ_MASK,
			   extclk_cfg << CS35L41_REFCLK_FREQ_SHIFT);
	regmap_update_bits(cs35l41->regmap, CS35L41_PLL_CLK_CTRL,
			   CS35L41_PLL_CLK_EN_MASK,
			   0 << CS35L41_PLL_CLK_EN_SHIFT);
	regmap_update_bits(cs35l41->regmap, CS35L41_PLL_CLK_CTRL,
			   CS35L41_PLL_CLK_SEL_MASK, clksrc);
	regmap_update_bits(cs35l41->regmap, CS35L41_PLL_CLK_CTRL,
			   CS35L41_PLL_OPENLOOP_MASK,
			   0 << CS35L41_PLL_OPENLOOP_SHIFT);
	regmap_update_bits(cs35l41->regmap, CS35L41_PLL_CLK_CTRL,
			   CS35L41_PLL_CLK_EN_MASK,
			   1 << CS35L41_PLL_CLK_EN_SHIFT);

	return 0;
}

static int cs35l41_dai_set_sysclk(struct snd_soc_dai *dai,
				  int clk_id, unsigned int freq, int dir)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(dai->component);
	unsigned int fs1_val;
	unsigned int fs2_val;
	unsigned int val;
	int fsindex;

	fsindex = cs35l41_get_fs_mon_config_index(freq);
	if (fsindex < 0) {
		dev_err(cs35l41->dev, "Invalid CLK Config freq: %u\n", freq);
		return -EINVAL;
	}

	dev_dbg(cs35l41->dev, "Set DAI sysclk %d\n", freq);

	if (freq <= 6144000) {
		/* Use the lookup table */
		fs1_val = cs35l41_fs_mon[fsindex].fs1;
		fs2_val = cs35l41_fs_mon[fsindex].fs2;
	} else {
		/* Use hard-coded values */
		fs1_val = 0x10;
		fs2_val = 0x24;
	}

	val = fs1_val;
	val |= (fs2_val << CS35L41_FS2_WINDOW_SHIFT) & CS35L41_FS2_WINDOW_MASK;
	regmap_write(cs35l41->regmap, CS35L41_TST_FS_MON0, val);

	return 0;
}

static int cs35l41_set_pdata(struct cs35l41_private *cs35l41)
{
	int ret;

	/* Set Platform Data */
	/* Required */
	if (cs35l41->pdata.bst_ipk &&
	    cs35l41->pdata.bst_ind && cs35l41->pdata.bst_cap) {
		ret = cs35l41_boost_config(cs35l41->dev, cs35l41->regmap, cs35l41->pdata.bst_ind,
					   cs35l41->pdata.bst_cap, cs35l41->pdata.bst_ipk);
		if (ret) {
			dev_err(cs35l41->dev, "Error in Boost DT config: %d\n", ret);
			return ret;
		}
	} else {
		dev_err(cs35l41->dev, "Incomplete Boost component DT config\n");
		return -EINVAL;
	}

	/* Optional */
	if (cs35l41->pdata.dout_hiz <= CS35L41_ASP_DOUT_HIZ_MASK &&
	    cs35l41->pdata.dout_hiz >= 0)
		regmap_update_bits(cs35l41->regmap, CS35L41_SP_HIZ_CTRL,
				   CS35L41_ASP_DOUT_HIZ_MASK,
				   cs35l41->pdata.dout_hiz);

	return 0;
}

static int cs35l41_irq_gpio_config(struct cs35l41_private *cs35l41)
{
	struct cs35l41_irq_cfg *irq_gpio_cfg1 = &cs35l41->pdata.irq_config1;
	struct cs35l41_irq_cfg *irq_gpio_cfg2 = &cs35l41->pdata.irq_config2;
	int irq_pol = IRQF_TRIGGER_NONE;

	regmap_update_bits(cs35l41->regmap, CS35L41_GPIO1_CTRL1,
			   CS35L41_GPIO_POL_MASK | CS35L41_GPIO_DIR_MASK,
			   irq_gpio_cfg1->irq_pol_inv << CS35L41_GPIO_POL_SHIFT |
			   !irq_gpio_cfg1->irq_out_en << CS35L41_GPIO_DIR_SHIFT);

	regmap_update_bits(cs35l41->regmap, CS35L41_GPIO2_CTRL1,
			   CS35L41_GPIO_POL_MASK | CS35L41_GPIO_DIR_MASK,
			   irq_gpio_cfg1->irq_pol_inv << CS35L41_GPIO_POL_SHIFT |
			   !irq_gpio_cfg1->irq_out_en << CS35L41_GPIO_DIR_SHIFT);

	regmap_update_bits(cs35l41->regmap, CS35L41_GPIO_PAD_CONTROL,
			   CS35L41_GPIO1_CTRL_MASK | CS35L41_GPIO2_CTRL_MASK,
			   irq_gpio_cfg1->irq_src_sel << CS35L41_GPIO1_CTRL_SHIFT |
			   irq_gpio_cfg2->irq_src_sel << CS35L41_GPIO2_CTRL_SHIFT);

	if ((irq_gpio_cfg2->irq_src_sel ==
			(CS35L41_GPIO_CTRL_ACTV_LO | CS35L41_VALID_PDATA)) ||
		(irq_gpio_cfg2->irq_src_sel ==
			(CS35L41_GPIO_CTRL_OPEN_INT | CS35L41_VALID_PDATA)))
		irq_pol = IRQF_TRIGGER_LOW;
	else if (irq_gpio_cfg2->irq_src_sel ==
			(CS35L41_GPIO_CTRL_ACTV_HI | CS35L41_VALID_PDATA))
		irq_pol = IRQF_TRIGGER_HIGH;

	return irq_pol;
}

static int cs35l41_component_probe(struct snd_soc_component *component)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(component);

	return wm_adsp2_component_probe(&cs35l41->dsp, component);
}

static void cs35l41_component_remove(struct snd_soc_component *component)
{
	struct cs35l41_private *cs35l41 = snd_soc_component_get_drvdata(component);

	wm_adsp2_component_remove(&cs35l41->dsp, component);
}

static const struct snd_soc_dai_ops cs35l41_ops = {
	.startup = cs35l41_pcm_startup,
	.set_fmt = cs35l41_set_dai_fmt,
	.hw_params = cs35l41_pcm_hw_params,
	.set_sysclk = cs35l41_dai_set_sysclk,
	.set_channel_map = cs35l41_set_channel_map,
};

static struct snd_soc_dai_driver cs35l41_dai[] = {
	{
		.name = "cs35l41-pcm",
		.id = 0,
		.playback = {
			.stream_name = "AMP Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_KNOT,
			.formats = CS35L41_RX_FORMATS,
		},
		.capture = {
			.stream_name = "AMP Capture",
			.channels_min = 1,
			.channels_max = 8,
			.rates = SNDRV_PCM_RATE_KNOT,
			.formats = CS35L41_TX_FORMATS,
		},
		.ops = &cs35l41_ops,
		.symmetric_rate = 1,
	},
};

static const struct snd_soc_component_driver soc_component_dev_cs35l41 = {
	.name = "cs35l41-codec",
	.probe = cs35l41_component_probe,
	.remove = cs35l41_component_remove,

	.dapm_widgets = cs35l41_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cs35l41_dapm_widgets),
	.dapm_routes = cs35l41_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cs35l41_audio_map),

	.controls = cs35l41_aud_controls,
	.num_controls = ARRAY_SIZE(cs35l41_aud_controls),
	.set_sysclk = cs35l41_component_set_sysclk,
};

static int cs35l41_handle_pdata(struct device *dev,
				struct cs35l41_platform_data *pdata,
				struct cs35l41_private *cs35l41)
{
	struct cs35l41_irq_cfg *irq_gpio1_config = &pdata->irq_config1;
	struct cs35l41_irq_cfg *irq_gpio2_config = &pdata->irq_config2;
	unsigned int val;
	int ret;

	ret = device_property_read_u32(dev, "cirrus,boost-peak-milliamp", &val);
	if (ret >= 0)
		pdata->bst_ipk = val;

	ret = device_property_read_u32(dev, "cirrus,boost-ind-nanohenry", &val);
	if (ret >= 0)
		pdata->bst_ind = val;

	ret = device_property_read_u32(dev, "cirrus,boost-cap-microfarad", &val);
	if (ret >= 0)
		pdata->bst_cap = val;

	ret = device_property_read_u32(dev, "cirrus,asp-sdout-hiz", &val);
	if (ret >= 0)
		pdata->dout_hiz = val;
	else
		pdata->dout_hiz = -1;

	/* GPIO1 Pin Config */
	irq_gpio1_config->irq_pol_inv = device_property_read_bool(dev,
					"cirrus,gpio1-polarity-invert");
	irq_gpio1_config->irq_out_en = device_property_read_bool(dev,
					"cirrus,gpio1-output-enable");
	ret = device_property_read_u32(dev, "cirrus,gpio1-src-select",
				       &val);
	if (ret >= 0)
		irq_gpio1_config->irq_src_sel = val | CS35L41_VALID_PDATA;

	/* GPIO2 Pin Config */
	irq_gpio2_config->irq_pol_inv = device_property_read_bool(dev,
					"cirrus,gpio2-polarity-invert");
	irq_gpio2_config->irq_out_en = device_property_read_bool(dev,
					"cirrus,gpio2-output-enable");
	ret = device_property_read_u32(dev, "cirrus,gpio2-src-select",
				       &val);
	if (ret >= 0)
		irq_gpio2_config->irq_src_sel = val | CS35L41_VALID_PDATA;

	return 0;
}

static const struct reg_sequence cs35l41_fs_errata_patch[] = {
	{ CS35L41_DSP1_RX1_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX2_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX3_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX4_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX5_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX6_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX7_RATE,	0x00000001 },
	{ CS35L41_DSP1_RX8_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX1_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX2_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX3_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX4_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX5_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX6_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX7_RATE,	0x00000001 },
	{ CS35L41_DSP1_TX8_RATE,	0x00000001 },
};

static int cs35l41_dsp_init(struct cs35l41_private *cs35l41)
{
	struct wm_adsp *dsp;
	int ret;

	dsp = &cs35l41->dsp;
	dsp->part = "cs35l41";
	dsp->num = 1;
	dsp->type = WMFW_HALO;
	dsp->rev = 0;
	dsp->fw = 9; /* 9 is WM_ADSP_FW_SPK_PROT in wm_adsp.c */
	dsp->dev = cs35l41->dev;
	dsp->regmap = cs35l41->regmap;
	dsp->base = CS35L41_DSP1_CTRL_BASE;
	dsp->base_sysinfo = CS35L41_DSP1_SYS_ID;
	dsp->mem = cs35l41_dsp1_regions;
	dsp->num_mems = ARRAY_SIZE(cs35l41_dsp1_regions);
	dsp->lock_regions = 0xFFFFFFFF;

	ret = regmap_multi_reg_write(cs35l41->regmap, cs35l41_fs_errata_patch,
				     ARRAY_SIZE(cs35l41_fs_errata_patch));
	if (ret < 0) {
		dev_err(cs35l41->dev, "Failed to write fs errata: %d\n", ret);
		return ret;
	}

	ret = wm_halo_init(dsp);
	if (ret) {
		dev_err(cs35l41->dev, "wm_halo_init failed: %d\n", ret);
		return ret;
	}

	ret = regmap_write(cs35l41->regmap, CS35L41_DSP1_RX5_SRC,
			   CS35L41_INPUT_SRC_VPMON);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Write INPUT_SRC_VPMON failed: %d\n", ret);
		goto err_dsp;
	}
	ret = regmap_write(cs35l41->regmap, CS35L41_DSP1_RX6_SRC,
			   CS35L41_INPUT_SRC_CLASSH);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Write INPUT_SRC_CLASSH failed: %d\n", ret);
		goto err_dsp;
	}
	ret = regmap_write(cs35l41->regmap, CS35L41_DSP1_RX7_SRC,
			   CS35L41_INPUT_SRC_TEMPMON);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Write INPUT_SRC_TEMPMON failed: %d\n", ret);
		goto err_dsp;
	}
	ret = regmap_write(cs35l41->regmap, CS35L41_DSP1_RX8_SRC,
			   CS35L41_INPUT_SRC_RSVD);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Write INPUT_SRC_RSVD failed: %d\n", ret);
		goto err_dsp;
	}

	return 0;

err_dsp:
	wm_adsp2_remove(dsp);

	return ret;
}

int cs35l41_probe(struct cs35l41_private *cs35l41,
		  struct cs35l41_platform_data *pdata)
{
	u32 regid, reg_revid, i, mtl_revid, int_status, chipid_match;
	int irq_pol = 0;
	int ret;

	if (pdata) {
		cs35l41->pdata = *pdata;
	} else {
		ret = cs35l41_handle_pdata(cs35l41->dev, &cs35l41->pdata, cs35l41);
		if (ret != 0)
			return ret;
	}

	for (i = 0; i < CS35L41_NUM_SUPPLIES; i++)
		cs35l41->supplies[i].supply = cs35l41_supplies[i];

	ret = devm_regulator_bulk_get(cs35l41->dev, CS35L41_NUM_SUPPLIES,
				      cs35l41->supplies);
	if (ret != 0) {
		dev_err(cs35l41->dev, "Failed to request core supplies: %d\n", ret);
		return ret;
	}

	ret = regulator_bulk_enable(CS35L41_NUM_SUPPLIES, cs35l41->supplies);
	if (ret != 0) {
		dev_err(cs35l41->dev, "Failed to enable core supplies: %d\n", ret);
		return ret;
	}

	/* returning NULL can be an option if in stereo mode */
	cs35l41->reset_gpio = devm_gpiod_get_optional(cs35l41->dev, "reset",
						      GPIOD_OUT_LOW);
	if (IS_ERR(cs35l41->reset_gpio)) {
		ret = PTR_ERR(cs35l41->reset_gpio);
		cs35l41->reset_gpio = NULL;
		if (ret == -EBUSY) {
			dev_info(cs35l41->dev,
				 "Reset line busy, assuming shared reset\n");
		} else {
			dev_err(cs35l41->dev,
				"Failed to get reset GPIO: %d\n", ret);
			goto err;
		}
	}
	if (cs35l41->reset_gpio) {
		/* satisfy minimum reset pulse width spec */
		usleep_range(2000, 2100);
		gpiod_set_value_cansleep(cs35l41->reset_gpio, 1);
	}

	usleep_range(2000, 2100);

	ret = regmap_read_poll_timeout(cs35l41->regmap, CS35L41_IRQ1_STATUS4,
				       int_status, int_status & CS35L41_OTP_BOOT_DONE,
				       1000, 100000);
	if (ret) {
		dev_err(cs35l41->dev,
			"Failed waiting for OTP_BOOT_DONE: %d\n", ret);
		goto err;
	}

	regmap_read(cs35l41->regmap, CS35L41_IRQ1_STATUS3, &int_status);
	if (int_status & CS35L41_OTP_BOOT_ERR) {
		dev_err(cs35l41->dev, "OTP Boot error\n");
		ret = -EINVAL;
		goto err;
	}

	ret = regmap_read(cs35l41->regmap, CS35L41_DEVID, &regid);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Get Device ID failed: %d\n", ret);
		goto err;
	}

	ret = regmap_read(cs35l41->regmap, CS35L41_REVID, &reg_revid);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Get Revision ID failed: %d\n", ret);
		goto err;
	}

	mtl_revid = reg_revid & CS35L41_MTLREVID_MASK;

	/* CS35L41 will have even MTLREVID
	 * CS35L41R will have odd MTLREVID
	 */
	chipid_match = (mtl_revid % 2) ? CS35L41R_CHIP_ID : CS35L41_CHIP_ID;
	if (regid != chipid_match) {
		dev_err(cs35l41->dev, "CS35L41 Device ID (%X). Expected ID %X\n",
			regid, chipid_match);
		ret = -ENODEV;
		goto err;
	}

	cs35l41_test_key_unlock(cs35l41->dev, cs35l41->regmap);

	ret = cs35l41_register_errata_patch(cs35l41->dev, cs35l41->regmap, reg_revid);
	if (ret)
		goto err;

	ret = cs35l41_otp_unpack(cs35l41->dev, cs35l41->regmap);
	if (ret < 0) {
		dev_err(cs35l41->dev, "OTP Unpack failed: %d\n", ret);
		goto err;
	}

	cs35l41_test_key_lock(cs35l41->dev, cs35l41->regmap);

	irq_pol = cs35l41_irq_gpio_config(cs35l41);

	/* Set interrupt masks for critical errors */
	regmap_write(cs35l41->regmap, CS35L41_IRQ1_MASK1,
		     CS35L41_INT1_MASK_DEFAULT);

	ret = devm_request_threaded_irq(cs35l41->dev, cs35l41->irq, NULL, cs35l41_irq,
					IRQF_ONESHOT | IRQF_SHARED | irq_pol,
					"cs35l41", cs35l41);
	if (ret != 0) {
		dev_err(cs35l41->dev, "Failed to request IRQ: %d\n", ret);
		goto err;
	}

	ret = cs35l41_set_pdata(cs35l41);
	if (ret < 0) {
		dev_err(cs35l41->dev, "Set pdata failed: %d\n", ret);
		goto err;
	}

	ret = cs35l41_dsp_init(cs35l41);
	if (ret < 0)
		goto err;

	pm_runtime_set_autosuspend_delay(cs35l41->dev, 3000);
	pm_runtime_use_autosuspend(cs35l41->dev);
	pm_runtime_mark_last_busy(cs35l41->dev);
	pm_runtime_set_active(cs35l41->dev);
	pm_runtime_get_noresume(cs35l41->dev);
	pm_runtime_enable(cs35l41->dev);

	ret = devm_snd_soc_register_component(cs35l41->dev,
					      &soc_component_dev_cs35l41,
					      cs35l41_dai, ARRAY_SIZE(cs35l41_dai));
	if (ret < 0) {
		dev_err(cs35l41->dev, "Register codec failed: %d\n", ret);
		goto err_pm;
	}

	pm_runtime_put_autosuspend(cs35l41->dev);

	dev_info(cs35l41->dev, "Cirrus Logic CS35L41 (%x), Revision: %02X\n",
		 regid, reg_revid);

	return 0;

err_pm:
	pm_runtime_disable(cs35l41->dev);
	pm_runtime_put_noidle(cs35l41->dev);

	wm_adsp2_remove(&cs35l41->dsp);
err:
	regulator_bulk_disable(CS35L41_NUM_SUPPLIES, cs35l41->supplies);
	gpiod_set_value_cansleep(cs35l41->reset_gpio, 0);

	return ret;
}
EXPORT_SYMBOL_GPL(cs35l41_probe);

void cs35l41_remove(struct cs35l41_private *cs35l41)
{
	pm_runtime_get_sync(cs35l41->dev);
	pm_runtime_disable(cs35l41->dev);

	regmap_write(cs35l41->regmap, CS35L41_IRQ1_MASK1, 0xFFFFFFFF);
	wm_adsp2_remove(&cs35l41->dsp);

	pm_runtime_put_noidle(cs35l41->dev);

	regulator_bulk_disable(CS35L41_NUM_SUPPLIES, cs35l41->supplies);
	gpiod_set_value_cansleep(cs35l41->reset_gpio, 0);
}
EXPORT_SYMBOL_GPL(cs35l41_remove);

static int __maybe_unused cs35l41_runtime_suspend(struct device *dev)
{
	struct cs35l41_private *cs35l41 = dev_get_drvdata(dev);

	dev_dbg(cs35l41->dev, "Runtime suspend\n");

	if (!cs35l41->dsp.preloaded || !cs35l41->dsp.running)
		return 0;

	dev_dbg(cs35l41->dev, "Enter hibernate\n");

	regmap_write(cs35l41->regmap, CS35L41_WAKESRC_CTL, 0x0088);
	regmap_write(cs35l41->regmap, CS35L41_WAKESRC_CTL, 0x0188);

	// Don't wait for ACK since bus activity would wake the device
	regmap_write(cs35l41->regmap, CS35L41_DSP_VIRT1_MBOX_1,
		     CSPL_MBOX_CMD_HIBERNATE);

	regcache_cache_only(cs35l41->regmap, true);
	regcache_mark_dirty(cs35l41->regmap);

	return 0;
}

static void cs35l41_wait_for_pwrmgt_sts(struct cs35l41_private *cs35l41)
{
	const int pwrmgt_retries = 10;
	unsigned int sts;
	int i, ret;

	for (i = 0; i < pwrmgt_retries; i++) {
		ret = regmap_read(cs35l41->regmap, CS35L41_PWRMGT_STS, &sts);
		if (ret)
			dev_err(cs35l41->dev, "Failed to read PWRMGT_STS: %d\n", ret);
		else if (!(sts & CS35L41_WR_PEND_STS_MASK))
			return;

		udelay(20);
	}

	dev_err(cs35l41->dev, "Timed out reading PWRMGT_STS\n");
}

static int cs35l41_exit_hibernate(struct cs35l41_private *cs35l41)
{
	const int wake_retries = 20;
	const int sleep_retries = 5;
	int ret, i, j;

	for (i = 0; i < sleep_retries; i++) {
		dev_dbg(cs35l41->dev, "Exit hibernate\n");

		for (j = 0; j < wake_retries; j++) {
			ret = cs35l41_set_cspl_mbox_cmd(cs35l41,
							CSPL_MBOX_CMD_OUT_OF_HIBERNATE);
			if (!ret)
				break;

			usleep_range(100, 200);
		}

		if (j < wake_retries) {
			dev_dbg(cs35l41->dev, "Wake success at cycle: %d\n", j);
			return 0;
		}

		dev_err(cs35l41->dev, "Wake failed, re-enter hibernate: %d\n", ret);

		cs35l41_wait_for_pwrmgt_sts(cs35l41);
		regmap_write(cs35l41->regmap, CS35L41_WAKESRC_CTL, 0x0088);

		cs35l41_wait_for_pwrmgt_sts(cs35l41);
		regmap_write(cs35l41->regmap, CS35L41_WAKESRC_CTL, 0x0188);

		cs35l41_wait_for_pwrmgt_sts(cs35l41);
		regmap_write(cs35l41->regmap, CS35L41_PWRMGT_CTL, 0x3);
	}

	dev_err(cs35l41->dev, "Timed out waking device\n");

	return -ETIMEDOUT;
}

static int __maybe_unused cs35l41_runtime_resume(struct device *dev)
{
	struct cs35l41_private *cs35l41 = dev_get_drvdata(dev);
	int ret;

	dev_dbg(cs35l41->dev, "Runtime resume\n");

	if (!cs35l41->dsp.preloaded || !cs35l41->dsp.running)
		return 0;

	regcache_cache_only(cs35l41->regmap, false);

	ret = cs35l41_exit_hibernate(cs35l41);
	if (ret)
		return ret;

	/* Test key needs to be unlocked to allow the OTP settings to re-apply */
	cs35l41_test_key_unlock(cs35l41->dev, cs35l41->regmap);
	ret = regcache_sync(cs35l41->regmap);
	cs35l41_test_key_lock(cs35l41->dev, cs35l41->regmap);
	if (ret) {
		dev_err(cs35l41->dev, "Failed to restore register cache: %d\n", ret);
		return ret;
	}

	return 0;
}

static int __maybe_unused cs35l41_sys_suspend(struct device *dev)
{
	struct cs35l41_private *cs35l41 = dev_get_drvdata(dev);

	dev_dbg(cs35l41->dev, "System suspend, disabling IRQ\n");
	disable_irq(cs35l41->irq);

	return 0;
}

static int __maybe_unused cs35l41_sys_suspend_noirq(struct device *dev)
{
	struct cs35l41_private *cs35l41 = dev_get_drvdata(dev);

	dev_dbg(cs35l41->dev, "Late system suspend, reenabling IRQ\n");
	enable_irq(cs35l41->irq);

	return 0;
}

static int __maybe_unused cs35l41_sys_resume_noirq(struct device *dev)
{
	struct cs35l41_private *cs35l41 = dev_get_drvdata(dev);

	dev_dbg(cs35l41->dev, "Early system resume, disabling IRQ\n");
	disable_irq(cs35l41->irq);

	return 0;
}

static int __maybe_unused cs35l41_sys_resume(struct device *dev)
{
	struct cs35l41_private *cs35l41 = dev_get_drvdata(dev);

	dev_dbg(cs35l41->dev, "System resume, reenabling IRQ\n");
	enable_irq(cs35l41->irq);

	return 0;
}

const struct dev_pm_ops cs35l41_pm_ops = {
	SET_RUNTIME_PM_OPS(cs35l41_runtime_suspend, cs35l41_runtime_resume, NULL)

	SET_SYSTEM_SLEEP_PM_OPS(cs35l41_sys_suspend, cs35l41_sys_resume)
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(cs35l41_sys_suspend_noirq, cs35l41_sys_resume_noirq)
};
EXPORT_SYMBOL_GPL(cs35l41_pm_ops);

MODULE_DESCRIPTION("ASoC CS35L41 driver");
MODULE_AUTHOR("David Rhodes, Cirrus Logic Inc, <david.rhodes@cirrus.com>");
MODULE_LICENSE("GPL");