/* Copyright (c) 2014-2019, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/module.h>
#include <sound/core.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/jack.h>
#include <sound/q6afe-v2.h>
#include <sound/info.h>
#include <sound/pcm_params.h>
#include <device_event.h>
#include <soc/qcom/socinfo.h>
#include <qdsp6v2/msm-pcm-routing-v2.h>
#include <sound/q6core.h>
#include <linux/mfd/wcd9xxx/wcd-gpio-ctrl.h>
#include "../codecs/wcd9xxx-common.h"
#include "../codecs/wcd9335.h"
#include "../codecs/wsa881x.h"
#include "../codecs/tlv320aic3x.h"
#include "msm-audio-pinctrl.h"

/* Spk control */
#define MDM_SPK_ON 1
#define MDM_HIFI_ON    1

#define WCD9XXX_MBHC_DEF_BUTTONS 8
#define WCD9XXX_MBHC_DEF_RLOADS 5
/*
 * MDM9650 run Tasha at 12.288 Mhz.
 * At present MDM supports 12.288mhz
 * only. Tasha supports 9.6 MHz also.
 */
#define MDM_MCLK_CLK_12P288MHZ 12288000
#define MDM_MCLK_CLK_9P6HZ 9600000
#define MDM_MI2S_RATE 48000
#define DEV_NAME_STR_LEN  32

#define SAMPLE_RATE_8KHZ 8000
#define SAMPLE_RATE_16KHZ 16000
#define SAMPLE_RATE_48KHZ 48000
#define NO_OF_BITS_PER_SAMPLE  16

#define LPAIF_OFFSET 0x07700000
#define LPAIF_PRI_MODE_MUXSEL (LPAIF_OFFSET + 0x2008)
#define LPAIF_SEC_MODE_MUXSEL (LPAIF_OFFSET + 0x200c)

#define LPASS_CSR_GP_IO_MUX_SPKR_CTL (LPAIF_OFFSET + 0x2004)
#define LPASS_CSR_GP_IO_MUX_MIC_CTL  (LPAIF_OFFSET + 0x2000)

#define I2S_SEL 0
#define PCM_SEL 1
#define I2S_PCM_SEL_OFFSET 0
#define I2S_PCM_MASTER_MODE 1
#define I2S_PCM_SLAVE_MODE 0

#define PRI_TLMM_CLKS_EN_MASTER 0x4
#define SEC_TLMM_CLKS_EN_MASTER 0x2
#define PRI_TLMM_CLKS_EN_SLAVE 0x100000
#define SEC_TLMM_CLKS_EN_SLAVE 0x800000
#define CLOCK_ON  1
#define CLOCK_OFF 0

/* Machine driver Name*/
#define DRV_NAME "mdm9650-asoc-snd"
#define TDM_SLOT_OFFSET_MAX    8

enum {
	PRIMARY_TDM_RX_0,
	PRIMARY_TDM_TX_0,
	SECONDARY_TDM_RX_0,
	SECONDARY_TDM_TX_0,
	TDM_MAX,
};

enum mi2s_pcm_mux {
	PRI_MI2S_PCM,
	SEC_MI2S_PCM,
	MI2S_PCM_MAX_INTF
};

enum mi2s_types {
	PRI_MI2S,
	SEC_MI2S,
};

struct mdm_machine_data {
	u32 mclk_freq;
	u16 prim_mi2s_mode;
	u16 sec_mi2s_mode;
	u16 prim_auxpcm_mode;
	struct device_node *prim_master_p;
	struct device_node *prim_slave_p;
	u16 sec_auxpcm_mode;
	u16 sec_tdm_mode;
	struct device_node *sec_master_p;
	struct device_node *sec_slave_p;
	u32 prim_clk_usrs;
	int hph_en1_gpio;
	int hph_en0_gpio;
	struct snd_info_entry *codec_root;
	void __iomem *lpaif_pri_muxsel_virt_addr;
	void __iomem *lpaif_sec_muxsel_virt_addr;
	void __iomem *lpass_mux_spkr_ctl_virt_addr;
	void __iomem *lpass_mux_mic_ctl_virt_addr;
};

static const struct afe_clk_cfg lpass_default = {
	AFE_API_VERSION_I2S_CONFIG,
	Q6AFE_LPASS_IBIT_CLK_1_P536_MHZ,
	Q6AFE_LPASS_OSR_CLK_12_P288_MHZ,
	Q6AFE_LPASS_CLK_SRC_INTERNAL,
	Q6AFE_LPASS_CLK_ROOT_DEFAULT,
	Q6AFE_LPASS_MODE_BOTH_VALID,
	0,
};

static const struct afe_clk_set lpass_default_v2 = {
	AFE_API_VERSION_I2S_CONFIG,
	Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT,
	Q6AFE_LPASS_IBIT_CLK_1_P536_MHZ,
	Q6AFE_LPASS_CLK_ATTRIBUTE_COUPLE_NO,
	Q6AFE_LPASS_CLK_ROOT_DEFAULT,
	0,
};

struct mdm_wsa881x_dev_info {
	struct device_node *of_node;
	u32 index;
};

static struct snd_soc_aux_dev *mdm_aux_dev;
static struct snd_soc_codec_conf *mdm_codec_conf;

static int mdm_auxpcm_rate = 8000;

static struct mutex cdc_mclk_mutex;
static int mdm_mi2s_rx_ch = 1;
static int mdm_mi2s_tx_ch = 1;
static int mdm_sec_mi2s_rx_ch = 1;
static int mdm_sec_mi2s_tx_ch = 1;
static int mdm_mi2s_rate = SAMPLE_RATE_48KHZ;
static int mdm_sec_mi2s_rate = SAMPLE_RATE_48KHZ;

static int mdm_mi2s_mode = I2S_PCM_MASTER_MODE;
static int mdm_sec_mi2s_mode = I2S_PCM_MASTER_MODE;
static int mdm_auxpcm_mode = I2S_PCM_MASTER_MODE;
static int mdm_sec_auxpcm_mode = I2S_PCM_MASTER_MODE;
static int mdm_sec_tdm_mode = I2S_PCM_MASTER_MODE;

/* TDM default channels */
static int mdm_pri_tdm_rx_0_ch = 8;
static int mdm_pri_tdm_tx_0_ch = 8;

static int mdm_sec_tdm_rx_0_ch = 8;
static int mdm_sec_tdm_tx_0_ch = 8;

/* TDM default bit format */
static int mdm_pri_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int mdm_pri_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;

static int mdm_sec_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;
static int mdm_sec_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;

/* TDM default sampling rate */
static int mdm_pri_tdm_rx_0_sample_rate = SAMPLE_RATE_48KHZ;
static int mdm_pri_tdm_tx_0_sample_rate = SAMPLE_RATE_48KHZ;

static int mdm_sec_tdm_rx_0_sample_rate = SAMPLE_RATE_48KHZ;
static int mdm_sec_tdm_tx_0_sample_rate = SAMPLE_RATE_48KHZ;

static char const *tdm_ch_text[] = {"One", "Two", "Three", "Four",
	"Five", "Six", "Seven", "Eight"};
static char const *tdm_bit_format_text[] = {"S16_LE", "S24_LE", "S24_3LE",
					    "S32_LE"};
static char const *tdm_sample_rate_text[] = {"KHZ_16", "KHZ_48"};

static char const *tdm_num_slots_text[] = {"eight", "sixteen"};

/* TDM default Number of Slots */
static int mdm_tdm_num_slots = 8;

/* TDM default offset */
static unsigned int tdm_slot_offset[TDM_MAX][TDM_SLOT_OFFSET_MAX] = {
	/* PRI_TDM_RX */
	{0, 4, 8, 12, 16, 20, 24, 28},
	/* PRI_TDM_TX */
	{0, 4, 8, 12, 16, 20, 24, 28},
	/* SEC_TDM_RX */
	{0, 4, 8, 12, 16, 20, 24, 28},
	/* SEC_TDM_TX */
	{0, 4, 8, 12, 16, 20, 24, 28},
};

static int mdm_spk_control = 1;
static int mdm_hifi_control;
static atomic_t mi2s_ref_count;
static atomic_t sec_mi2s_ref_count;
static atomic_t sec_tdm_ref_count;

static int mdm_enable_codec_ext_clk(struct snd_soc_codec *codec,
					int enable, bool dapm);

static void *def_tasha_mbhc_cal(void);
static void *adsp_state_notifier;
static bool dummy_device_registered;

static struct wcd_mbhc_config wcd_mbhc_cfg = {
	.read_fw_bin = false,
	.calibration = NULL,
	.detect_extn_cable = true,
	.mono_stero_detection = false,
	.swap_gnd_mic = NULL,
	.hs_ext_micbias = true,
};

static int mdm_wsa881x_init(struct snd_soc_component *component)
{
	u8 spkleft_ports[WSA881X_MAX_SWR_PORTS] = {100, 101, 102, 106};
	u8 spkright_ports[WSA881X_MAX_SWR_PORTS] = {103, 104, 105, 107};
	unsigned int ch_rate[WSA881X_MAX_SWR_PORTS] = {2400, 600, 300, 1200};
	unsigned int ch_mask[WSA881X_MAX_SWR_PORTS] = {0x1, 0xF, 0x3, 0x3};
	struct snd_soc_codec *codec = snd_soc_component_to_codec(component);
	struct mdm_machine_data *pdata;
	struct snd_soc_dapm_context *dapm;

	if (!codec) {
		pr_err("%s codec is NULL\n", __func__);
		return -EINVAL;
	}

	dapm = &codec->dapm;

	if (!strcmp(component->name_prefix, "SpkrLeft")) {
		dev_dbg(codec->dev, "%s: setting left ch map to codec %s\n",
			__func__, codec->component.name);
		wsa881x_set_channel_map(codec, &spkleft_ports[0],
				WSA881X_MAX_SWR_PORTS, &ch_mask[0],
				&ch_rate[0]);
		if (dapm->component) {
			snd_soc_dapm_ignore_suspend(dapm, "SpkrLeft IN");
			snd_soc_dapm_ignore_suspend(dapm, "SpkrLeft SPKR");
		}
	} else if (!strcmp(component->name_prefix, "SpkrRight")) {
		dev_dbg(codec->dev, "%s: setting right ch map to codec %s\n",
			__func__, codec->component.name);
		wsa881x_set_channel_map(codec, &spkright_ports[0],
				WSA881X_MAX_SWR_PORTS, &ch_mask[0],
				&ch_rate[0]);
		if (dapm->component) {
			snd_soc_dapm_ignore_suspend(dapm, "SpkrRight IN");
			snd_soc_dapm_ignore_suspend(dapm, "SpkrRight SPKR");
		}
	} else {
		dev_err(codec->dev, "%s: wrong codec name %s\n", __func__,
			codec->component.name);
		return -EINVAL;
	}
	pdata = snd_soc_card_get_drvdata(component->card);
	if (pdata && pdata->codec_root)
		wsa881x_codec_info_create_codec_entry(pdata->codec_root, codec);

	return 0;
}

static int mdm_set_lpass_clk_v1(struct snd_soc_pcm_runtime *rtd,
				bool enable,
				u16 mi2s_port,
				int rate,
				u16 mode)
{
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);
	struct afe_clk_cfg lpass_clks = lpass_default;
	int bit_clk_freq = (rate * 2 * NO_OF_BITS_PER_SAMPLE);
	int ret;

	dev_dbg(card->dev, "%s: Setting clock using v1\n", __func__);

	if (mode) {
		/* enable mclk for the automotive card */
		if (!strcmp(card->name, "mdm-auto-i2s-snd-card"))
			lpass_clks.clk_set_mode = Q6AFE_LPASS_MODE_BOTH_VALID;
		else
			lpass_clks.clk_set_mode = Q6AFE_LPASS_MODE_CLK1_VALID;

		if (enable) {
			lpass_clks.clk_val1 = bit_clk_freq;
			lpass_clks.clk_val2 = pdata->mclk_freq;
		} else {
			lpass_clks.clk_val1 = Q6AFE_LPASS_IBIT_CLK_DISABLE;
			lpass_clks.clk_val2 = Q6AFE_LPASS_OSR_CLK_DISABLE;
		}

		ret = afe_set_lpass_clock(mi2s_port, &lpass_clks);
		if (ret < 0) {
			dev_err(card->dev,
				"%s: afe_set_lpass_clock failed, err: %d\n",
				__func__, ret);
			goto done;
		}

		dev_dbg(card->dev, "%s: clk_1 = %x clk_2 = %x mode = %x\n",
			__func__, lpass_clks.clk_val1, lpass_clks.clk_val2,
			lpass_clks.clk_set_mode);
	}
	ret = 0;

done:
	return ret;
}

static int mdm_set_lpass_clk_v2(struct snd_soc_pcm_runtime *rtd,
				bool enable,
				enum mi2s_types mi2s_type,
				int rate,
				u16 mode)
{
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);
	struct afe_clk_set m_clk = lpass_default_v2;
	struct afe_clk_set ibit_clk = lpass_default_v2;
	u16 mi2s_port;
	u16 ibit_clk_id;
	int bit_clk_freq = (rate * 2 * NO_OF_BITS_PER_SAMPLE);
	int ret = 0;

	dev_dbg(card->dev, "%s: setting lpass clock using v2\n", __func__);

	if (pdata == NULL) {
		dev_err(card->dev, "%s: platform data is null\n", __func__);
		ret = -ENOMEM;
		goto done;
	}

	if (mi2s_type == PRI_MI2S) {
		mi2s_port = AFE_PORT_ID_PRIMARY_MI2S_RX;
		ibit_clk_id = Q6AFE_LPASS_CLK_ID_PRI_MI2S_IBIT;
	} else {
		mi2s_port = AFE_PORT_ID_SECONDARY_MI2S_RX;
		ibit_clk_id = Q6AFE_LPASS_CLK_ID_SEC_MI2S_IBIT;
	}

	/* Set both mclk and ibit clocks when using LPASS_CLK_VER_2 */
	m_clk.clk_id = Q6AFE_LPASS_CLK_ID_MCLK_3;
	m_clk.clk_freq_in_hz = pdata->mclk_freq;
	m_clk.enable = enable;
	ret = afe_set_lpass_clock_v2(mi2s_port, &m_clk);
	if (ret < 0) {
		dev_err(card->dev,
			"%s: afe_set_lpass_clock_v2 failed for mclk_3 with ret %d\n",
			__func__, ret);
		goto done;
	}

	if (mode) {
		ibit_clk.clk_id = ibit_clk_id;
		ibit_clk.clk_freq_in_hz = bit_clk_freq;
		ibit_clk.enable = enable;
		ret = afe_set_lpass_clock_v2(mi2s_port, &ibit_clk);
		if (ret < 0) {
			dev_err(card->dev,
				"%s: afe_set_lpass_clock_v2 failed for ibit with ret %d\n",
				__func__, ret);
			goto err_ibit_clk_set;
		}
	}
	ret = 0;

done:
	return ret;

err_ibit_clk_set:
	m_clk.enable = false;
	if (afe_set_lpass_clock_v2(mi2s_port, &m_clk)) {
		dev_err(card->dev,
			"%s: afe_set_lpass_clock_v2 failed for mclk_3\n",
			__func__);
	}
	return ret;
}

static int mdm_mi2s_clk_ctl(struct snd_soc_pcm_runtime *rtd, bool enable,
				int rate, u16 mode)
{
	enum lpass_clk_ver lpass_clk_ver;
	int ret;

	lpass_clk_ver = afe_get_lpass_clk_ver();

	if (lpass_clk_ver == LPASS_CLK_VER_2)
		ret = mdm_set_lpass_clk_v2(rtd, enable, PRI_MI2S, rate, mode);
	else
		ret = mdm_set_lpass_clk_v1(rtd, enable, MI2S_RX, rate, mode);

	return ret;
}

static void mdm_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	if (atomic_dec_return(&mi2s_ref_count) == 0) {
		ret = mdm_mi2s_clk_ctl(rtd, false, 0, pdata->prim_mi2s_mode);
		if (ret < 0)
			pr_err("%s Clock disable failed\n", __func__);

		if (pdata->prim_mi2s_mode == 1)
			ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
						"pri_mi2s_aux_master");
		else
			ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
						"pri_mi2s_aux_slave");
		if (ret)
			pr_err("%s: failed to set pri gpios to sleep: %d\n",
					__func__, ret);
	}
}

static int mdm_mi2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);
	int ret = 0;

	pdata->prim_mi2s_mode = mdm_mi2s_mode;
	if (atomic_inc_return(&mi2s_ref_count) == 1) {
		if (pdata->lpaif_pri_muxsel_virt_addr != NULL) {
			ret = afe_enable_lpass_core_shared_clock(MI2S_RX,
								 CLOCK_ON);
			if (ret < 0) {
				ret = -EINVAL;
				goto err;
			}
			iowrite32(I2S_SEL << I2S_PCM_SEL_OFFSET,
					pdata->lpaif_pri_muxsel_virt_addr);
			if (pdata->lpass_mux_spkr_ctl_virt_addr != NULL) {
				if (pdata->prim_mi2s_mode == 1)
					iowrite32(PRI_TLMM_CLKS_EN_MASTER,
					pdata->lpass_mux_spkr_ctl_virt_addr);
				else
					iowrite32(PRI_TLMM_CLKS_EN_SLAVE,
					pdata->lpass_mux_spkr_ctl_virt_addr);
			} else {
				dev_err(card->dev, "%s: mux spkr ctl virt addr is NULL\n",
						__func__);

				ret = -EINVAL;
				goto err;
			}
		} else {
			dev_err(card->dev, "%s lpaif_pri_muxsel_virt_addr is NULL\n",
					__func__);

			ret = -EINVAL;
			goto done;
		}
		/*
		 * This sets the CONFIG PARAMETER WS_SRC.
		 * 1 means internal clock master mode.
		 * 0 means external clock slave mode.
		 */
		if (pdata->prim_mi2s_mode == 1) {
			ret = msm_gpioset_activate(CLIENT_WCD_EXT,
						"pri_mi2s_aux_master");
			if (ret < 0) {
				pr_err("%s pinctrl set failed\n", __func__);
				goto err;
			}
			ret = mdm_mi2s_clk_ctl(rtd, true, mdm_mi2s_rate,
						pdata->prim_mi2s_mode);
			if (ret < 0) {
				dev_err(card->dev, "%s clock enable failed\n",
						__func__);
				goto err;
			}
			ret = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_CBS_CFS);
			if (ret < 0) {
				mdm_mi2s_clk_ctl(rtd, false,
						0, pdata->prim_mi2s_mode);
				dev_err(card->dev,
					"%s Set fmt for cpu dai failed\n",
					__func__);
				goto err;
			}
			ret = snd_soc_dai_set_fmt(codec_dai,
					SND_SOC_DAIFMT_CBS_CFS |
					SND_SOC_DAIFMT_I2S);
			if (ret < 0) {
				mdm_mi2s_clk_ctl(rtd, false,
						0, pdata->prim_mi2s_mode);
				dev_err(card->dev,
					"%s Set fmt for codec dai failed\n",
					__func__);
			}
			ret = snd_soc_dai_set_sysclk(codec_dai,
					CLKIN_MCLK,
					pdata->mclk_freq, SND_SOC_CLOCK_OUT);
			if (ret < 0)
				pr_err("%s Set sysclk for codec dai failed\n",
					__func__);
		} else {
			/*
			 * Disable bit clk in slave mode for QC codec.
			 * Enable only mclk.
			 */
			ret = msm_gpioset_activate(CLIENT_WCD_EXT,
						"pri_mi2s_aux_slave");
			if (ret < 0) {
				pr_err("%s pinctrl set failed\n", __func__);
				goto err;
			}
			ret = mdm_mi2s_clk_ctl(rtd, false, 0,
						pdata->prim_mi2s_mode);
			if (ret < 0) {
				dev_err(card->dev,
					"%s clock enable failed\n", __func__);
				goto err;
			}
			ret = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_CBM_CFM);
			if (ret < 0) {
				dev_err(card->dev,
					"%s Set fmt for cpu dai failed\n",
					__func__);
				goto err;
			}
		}
err:
		if (ret)
			atomic_dec_return(&mi2s_ref_count);
		afe_enable_lpass_core_shared_clock(MI2S_RX, CLOCK_OFF);
	}
done:
	return ret;
}

static int mdm_sec_mi2s_clk_ctl(struct snd_soc_pcm_runtime *rtd, bool enable,
				int rate, u16 mode)
{
	enum lpass_clk_ver lpass_clk_ver;
	int ret;

	lpass_clk_ver = afe_get_lpass_clk_ver();

	if (lpass_clk_ver == LPASS_CLK_VER_2)
		ret = mdm_set_lpass_clk_v2(rtd, enable, SEC_MI2S, rate, mode);
	else
		ret = mdm_set_lpass_clk_v1(rtd, enable,
					SECONDARY_I2S_RX, rate, mode);

	return ret;
}

static void mdm_sec_mi2s_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	if (atomic_dec_return(&sec_mi2s_ref_count) == 0) {
		ret = mdm_sec_mi2s_clk_ctl(rtd, false,
					0, pdata->sec_mi2s_mode);
		if (ret < 0)
			pr_err("%s Clock disable failed\n", __func__);

		if (pdata->sec_mi2s_mode == 1)
			ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
						"sec_mi2s_aux_master");
		else
			ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
						"sec_mi2s_aux_slave");
		if (ret)
			pr_err("%s: failed to set sec gpios to sleep: %d\n",
				__func__, ret);
	}
}

static int mdm_sec_mi2s_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);
	int ret = 0;

	pdata->sec_mi2s_mode = mdm_sec_mi2s_mode;
	if (atomic_inc_return(&sec_mi2s_ref_count) == 1) {
		if (pdata->lpaif_sec_muxsel_virt_addr != NULL) {
			ret = afe_enable_lpass_core_shared_clock(
					SECONDARY_I2S_RX, CLOCK_ON);
			if (ret < 0) {
				ret = -EINVAL;
				goto done;
			}
			iowrite32(I2S_SEL << I2S_PCM_SEL_OFFSET,
					pdata->lpaif_sec_muxsel_virt_addr);

			if (pdata->lpass_mux_mic_ctl_virt_addr != NULL) {
				if (pdata->sec_mi2s_mode == 1)
					iowrite32(SEC_TLMM_CLKS_EN_MASTER,
					pdata->lpass_mux_mic_ctl_virt_addr);
				else
					iowrite32(SEC_TLMM_CLKS_EN_SLAVE,
					pdata->lpass_mux_mic_ctl_virt_addr);
			} else {
				dev_err(card->dev,
					"%s: mux spkr ctl virt addr is NULL\n",
					 __func__);
				ret = -EINVAL;
				goto err;
			}
		} else {
			dev_err(card->dev,
				"%s lpaif_sec_muxsel_virt_addr is NULL\n",
				__func__);
			ret = -EINVAL;
			goto done;
		}
		/*
		 * This sets the CONFIG PARAMETER WS_SRC.
		 * 1 means internal clock master mode.
		 * 0 means external clock slave mode.
		 */
		if (pdata->sec_mi2s_mode == 1) {
			ret = msm_gpioset_activate(CLIENT_WCD_EXT,
						"sec_mi2s_aux_master");
			if (ret < 0) {
				pr_err("%s pinctrl set failed\n", __func__);
				goto err;
			}
			ret = mdm_sec_mi2s_clk_ctl(rtd, true,
						mdm_sec_mi2s_rate,
						pdata->sec_mi2s_mode);
			if (ret < 0) {
				dev_err(card->dev, "%s clock enable failed\n",
					__func__);
				goto err;
			}
			ret = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_CBS_CFS);
			if (ret < 0) {
				ret = mdm_sec_mi2s_clk_ctl(rtd, false,
						0, pdata->sec_mi2s_mode);
				dev_err(card->dev,
					"%s Set fmt for cpu dai failed\n",
					__func__);
			}
		} else {
			/*
			 * Enable mclk here, if needed for external codecs.
			 * Optional. Refer primary mi2s slave interface.
			 */
			ret = msm_gpioset_activate(CLIENT_WCD_EXT,
						"sec_mi2s_aux_slave");
			if (ret < 0) {
				pr_err("%s pinctrl set failed\n", __func__);
				goto err;
			}
			ret = mdm_sec_mi2s_clk_ctl(rtd, false, 0,
						pdata->sec_mi2s_mode);
			if (ret < 0) {
				dev_err(card->dev,
					"%s clock enable failed\n", __func__);
				goto err;
			}
			ret = snd_soc_dai_set_fmt(cpu_dai,
					SND_SOC_DAIFMT_CBM_CFM);
			if (ret < 0)
				dev_err(card->dev,
					"%s Set fmt for cpu dai failed\n",
					__func__);
		}
err:
			if (ret)
				atomic_dec_return(&sec_mi2s_ref_count);
			afe_enable_lpass_core_shared_clock(
							SECONDARY_I2S_RX,
							CLOCK_OFF);
	}
done:
	return ret;
}

static struct snd_soc_ops mdm_mi2s_be_ops = {
	.startup = mdm_mi2s_startup,
	.shutdown = mdm_mi2s_shutdown,
};

static struct snd_soc_ops mdm_sec_mi2s_be_ops = {
	.startup = mdm_sec_mi2s_startup,
	.shutdown = mdm_sec_mi2s_shutdown,
};

static int mdm_mi2s_rate_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_i2s_rate  = %d", __func__,
		 mdm_mi2s_rate);
	ucontrol->value.integer.value[0] = mdm_mi2s_rate;
	return 0;
}

static int mdm_sec_mi2s_rate_get(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_sec_i2s_rate  = %d", __func__,
		 mdm_sec_mi2s_rate);
	ucontrol->value.integer.value[0] = mdm_sec_mi2s_rate;
	return 0;
}

static int mdm_mi2s_rate_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_mi2s_rate = SAMPLE_RATE_8KHZ;
		break;
	case 1:
		mdm_mi2s_rate = SAMPLE_RATE_16KHZ;
		break;
	case 2:
	default:
		mdm_mi2s_rate = SAMPLE_RATE_48KHZ;
		break;
	}
	pr_debug("%s: mdm_i2s_rate = %d ucontrol->value = %d\n",
		 __func__, mdm_mi2s_rate,
		 (int)ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_sec_mi2s_rate_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_sec_mi2s_rate = SAMPLE_RATE_8KHZ;
		break;
	case 1:
		mdm_sec_mi2s_rate = SAMPLE_RATE_16KHZ;
		break;
	case 2:
	default:
		mdm_sec_mi2s_rate = SAMPLE_RATE_48KHZ;
		break;
	}
	pr_debug("%s: mdm_sec_mi2s_rate = %d ucontrol->value = %d\n",
		 __func__, mdm_sec_mi2s_rate,
		 (int)ucontrol->value.integer.value[0]);
	return 0;
}

static inline int param_is_mask(int p)
{
	return (p >= SNDRV_PCM_HW_PARAM_FIRST_MASK) &&
	       (p <= SNDRV_PCM_HW_PARAM_LAST_MASK);
}

static inline struct snd_mask *param_to_mask(struct snd_pcm_hw_params *p, int n)
{
	return &(p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]);
}

static void param_set_mask(struct snd_pcm_hw_params *p, int n, unsigned bit)
{
	if (bit >= SNDRV_MASK_MAX)
		return;
	if (param_is_mask(n)) {
		struct snd_mask *m = param_to_mask(p, n);

		m->bits[0] = 0;
		m->bits[1] = 0;
		m->bits[bit >> 5] |= (1 << (bit & 31));
	}
}

static int mdm_mi2s_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rt,
					      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
		       SNDRV_PCM_FORMAT_S16_LE);
	rate->min = rate->max = mdm_mi2s_rate;
	channels->min = channels->max = mdm_mi2s_rx_ch;
	return 0;
}

static int mdm_sec_mi2s_rx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rt,
					      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
		       SNDRV_PCM_FORMAT_S16_LE);
	rate->min = rate->max = mdm_sec_mi2s_rate;
	channels->min = channels->max = mdm_sec_mi2s_rx_ch;
	return 0;
}

static int mdm_mi2s_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rt,
					     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
		       SNDRV_PCM_FORMAT_S16_LE);
	rate->min = rate->max = mdm_mi2s_rate;
	channels->min = channels->max = mdm_mi2s_tx_ch;
	return 0;
}

static int mdm_sec_mi2s_tx_be_hw_params_fixup(struct snd_soc_pcm_runtime *rt,
					     struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
		       SNDRV_PCM_FORMAT_S16_LE);
	rate->min = rate->max = mdm_sec_mi2s_rate;
	channels->min = channels->max = mdm_sec_mi2s_tx_ch;
	return 0;
}

static int mdm_be_hw_params_fixup(struct snd_soc_pcm_runtime *rt,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
						      SNDRV_PCM_HW_PARAM_RATE);
	param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
		       SNDRV_PCM_FORMAT_S16_LE);
	rate->min = rate->max = MDM_MI2S_RATE;
	return 0;
}

static int mdm_mi2s_rx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_mi2s_rx_ch %d\n", __func__,
		 mdm_mi2s_rx_ch);

	ucontrol->value.integer.value[0] = mdm_mi2s_rx_ch - 1;
	return 0;
}

static int mdm_sec_mi2s_rx_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_sec_mi2s_rx_ch %d\n", __func__,
		 mdm_mi2s_rx_ch);

	ucontrol->value.integer.value[0] = mdm_sec_mi2s_rx_ch - 1;
	return 0;
}

static int mdm_mi2s_rx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	mdm_mi2s_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s mdm_mi2s_rx_ch %d\n", __func__,
		 mdm_mi2s_rx_ch);

	return 1;
}

static int mdm_sec_mi2s_rx_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	mdm_mi2s_rx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s mdm_sec_mi2s_rx_ch %d\n", __func__,
		 mdm_sec_mi2s_rx_ch);

	return 1;
}

static int mdm_mi2s_tx_ch_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_mi2s_tx_ch %d\n", __func__,
		 mdm_mi2s_tx_ch);

	ucontrol->value.integer.value[0] = mdm_mi2s_tx_ch - 1;
	return 0;
}

static int mdm_sec_mi2s_tx_ch_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_sec_mi2s_tx_ch %d\n", __func__,
		 mdm_mi2s_tx_ch);

	ucontrol->value.integer.value[0] = mdm_sec_mi2s_tx_ch - 1;
	return 0;
}

static int mdm_mi2s_tx_ch_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	mdm_mi2s_tx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s mdm_mi2s_tx_ch %d\n", __func__,
		 mdm_mi2s_tx_ch);

	return 1;
}

static int mdm_sec_mi2s_tx_ch_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	mdm_mi2s_tx_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s mdm_sec_mi2s_tx_ch %d\n", __func__,
		 mdm_sec_mi2s_tx_ch);

	return 1;
}

static int mdm_mi2s_mode_get(struct snd_kcontrol *kcontrol,
			  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_mi2s_mode %d\n", __func__,
			mdm_mi2s_mode);

	ucontrol->value.integer.value[0] = mdm_mi2s_mode;
	return 0;
}

static int mdm_mi2s_mode_put(struct snd_kcontrol *kcontrol,
			    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_mi2s_mode = I2S_PCM_MASTER_MODE;
		break;
	case 1:
		mdm_mi2s_mode = I2S_PCM_SLAVE_MODE;
		break;
	default:
		mdm_mi2s_mode = I2S_PCM_MASTER_MODE;
		break;
	}
	pr_debug("%s: mdm_mi2s_mode = %d ucontrol->value = %d\n",
			__func__, mdm_mi2s_mode,
			(int)ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_sec_mi2s_mode_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_sec_mi2s_mode %d\n", __func__,
			 mdm_sec_mi2s_mode);

	ucontrol->value.integer.value[0] = mdm_sec_mi2s_mode;
	return 0;
}

static int mdm_sec_mi2s_mode_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_sec_mi2s_mode = I2S_PCM_MASTER_MODE;
		break;
	case 1:
		mdm_sec_mi2s_mode = I2S_PCM_SLAVE_MODE;
		break;
	default:
		mdm_sec_mi2s_mode = I2S_PCM_MASTER_MODE;
		break;
	}
	pr_debug("%s: mdm_sec_mi2s_mode = %d ucontrol->value = %d\n",
		__func__, mdm_sec_mi2s_mode,
		(int)ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_auxpcm_mode_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_auxpcm_mode %d\n", __func__,
		 mdm_auxpcm_mode);

	ucontrol->value.integer.value[0] = mdm_auxpcm_mode;
	return 0;
}

static int mdm_auxpcm_mode_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_auxpcm_mode = I2S_PCM_MASTER_MODE;
		break;
	case 1:
		mdm_auxpcm_mode = I2S_PCM_SLAVE_MODE;
		break;
	default:
		mdm_auxpcm_mode = I2S_PCM_MASTER_MODE;
		break;
	}
	pr_debug("%s: mdm_auxpcm_mode = %d ucontrol->value = %d\n",
		__func__, mdm_auxpcm_mode,
		(int)ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_sec_auxpcm_mode_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_sec_auxpcm_mode %d\n", __func__,
		 mdm_sec_auxpcm_mode);

	ucontrol->value.integer.value[0] = mdm_sec_auxpcm_mode;
	return 0;
}

static int mdm_sec_auxpcm_mode_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_sec_auxpcm_mode = I2S_PCM_MASTER_MODE;
		break;
	case 1:
		mdm_sec_auxpcm_mode = I2S_PCM_SLAVE_MODE;
		break;
	default:
		mdm_sec_auxpcm_mode = I2S_PCM_MASTER_MODE;
		break;
	}
	pr_debug("%s: mdm_sec_auxpcm_mode = %d ucontrol->value = %d\n",
		__func__, mdm_sec_auxpcm_mode,
		(int)ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_sec_tdm_mode_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_sec_tdm_mode %d\n", __func__,
		 mdm_sec_tdm_mode);

	ucontrol->value.integer.value[0] = mdm_sec_tdm_mode;
	return 0;
}

static int mdm_sec_tdm_mode_put(struct snd_kcontrol *kcontrol,
				    struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_sec_tdm_mode = I2S_PCM_MASTER_MODE;
		break;
	case 1:
		mdm_sec_tdm_mode = I2S_PCM_SLAVE_MODE;
		break;
	default:
		mdm_sec_tdm_mode = I2S_PCM_MASTER_MODE;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_mode = %d ucontrol->value = %d\n",
		 __func__, mdm_sec_tdm_mode,
		 (int)ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_pri_tdm_rx_0_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_pri_tdm_rx_0_ch = %d\n", __func__,
		mdm_pri_tdm_rx_0_ch);
	ucontrol->value.integer.value[0] = mdm_pri_tdm_rx_0_ch - 1;
	return 0;
}

static int mdm_pri_tdm_rx_0_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	mdm_pri_tdm_rx_0_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: mdm_pri_tdm_rx_0_ch = %d\n", __func__,
		mdm_pri_tdm_rx_0_ch);
	return 0;
}

static int mdm_pri_tdm_tx_0_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_pri_tdm_tx_0_ch = %d\n", __func__,
		mdm_pri_tdm_tx_0_ch);
	ucontrol->value.integer.value[0] = mdm_pri_tdm_tx_0_ch - 1;
	return 0;
}

static int mdm_pri_tdm_tx_0_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	mdm_pri_tdm_tx_0_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: mdm_pri_tdm_tx_0_ch = %d\n", __func__,
		mdm_pri_tdm_tx_0_ch);
	return 0;
}

static int mdm_sec_tdm_rx_0_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_sec_tdm_rx_0_ch = %d\n", __func__,
		mdm_sec_tdm_rx_0_ch);
	ucontrol->value.integer.value[0] = mdm_sec_tdm_rx_0_ch - 1;
	return 0;
}

static int mdm_sec_tdm_rx_0_ch_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	mdm_sec_tdm_rx_0_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: mdm_sec_tdm_rx_0_ch = %d\n", __func__,
		mdm_sec_tdm_rx_0_ch);
	return 0;
}

static int mdm_sec_tdm_tx_0_ch_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_sec_tdm_tx_0_ch = %d\n", __func__,
		mdm_sec_tdm_tx_0_ch);
	ucontrol->value.integer.value[0] = mdm_sec_tdm_tx_0_ch - 1;
	return 0;
}

static int mdm_sec_tdm_tx_0_ch_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	mdm_sec_tdm_tx_0_ch = ucontrol->value.integer.value[0] + 1;
	pr_debug("%s: mdm_sec_tdm_tx_0_ch = %d\n", __func__,
		mdm_sec_tdm_tx_0_ch);
	return 0;
}

static int mdm_pri_tdm_rx_0_bit_format_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_pri_tdm_rx_0_bit_format) {
	case SNDRV_PCM_FORMAT_S32_LE:
		ucontrol->value.integer.value[0] = 3;
		break;
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_rx_0_bit_format = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_pri_tdm_rx_0_bit_format_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 3:
		mdm_pri_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S32_LE;
		break;
	case 2:
		mdm_pri_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		mdm_pri_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		mdm_pri_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_rx_0_bit_format = %d\n",
		 __func__, mdm_pri_tdm_rx_0_bit_format);
	return 0;
}

static int mdm_pri_tdm_tx_0_bit_format_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_pri_tdm_tx_0_bit_format) {
	case SNDRV_PCM_FORMAT_S32_LE:
		ucontrol->value.integer.value[0] = 3;
		break;
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_tx_0_bit_format = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_pri_tdm_tx_0_bit_format_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 3:
		mdm_pri_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S32_LE;
		break;
	case 2:
		mdm_pri_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		mdm_pri_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		mdm_pri_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_tx_0_bit_format = %d\n",
		 __func__, mdm_pri_tdm_tx_0_bit_format);
	return 0;
}

static int mdm_sec_tdm_rx_0_bit_format_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_sec_tdm_rx_0_bit_format) {
	case SNDRV_PCM_FORMAT_S32_LE:
		ucontrol->value.integer.value[0] = 3;
		break;
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_rx_0_bit_format = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_sec_tdm_rx_0_bit_format_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 3:
		mdm_sec_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S32_LE;
		break;
	case 2:
		mdm_sec_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		mdm_sec_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		mdm_sec_tdm_rx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_rx_0_bit_format = %d\n",
		 __func__, mdm_sec_tdm_rx_0_bit_format);
	return 0;
}

static int mdm_sec_tdm_tx_0_bit_format_get(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_sec_tdm_tx_0_bit_format) {
	case SNDRV_PCM_FORMAT_S32_LE:
		ucontrol->value.integer.value[0] = 3;
		break;
	case SNDRV_PCM_FORMAT_S24_3LE:
		ucontrol->value.integer.value[0] = 2;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		ucontrol->value.integer.value[0] = 1;
		break;
	case SNDRV_PCM_FORMAT_S16_LE:
	default:
		ucontrol->value.integer.value[0] = 0;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_tx_0_bit_format = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int mdm_sec_tdm_tx_0_bit_format_put(struct snd_kcontrol *kcontrol,
				  struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 3:
		mdm_sec_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S32_LE;
		break;
	case 2:
		mdm_sec_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S24_3LE;
		break;
	case 1:
		mdm_sec_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S24_LE;
		break;
	case 0:
	default:
		mdm_sec_tdm_tx_0_bit_format = SNDRV_PCM_FORMAT_S16_LE;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_tx_0_bit_format = %d\n",
		 __func__, mdm_sec_tdm_tx_0_bit_format);
	return 0;
}

static int mdm_pri_tdm_rx_0_sample_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_pri_tdm_rx_0_sample_rate) {
	case SAMPLE_RATE_16KHZ:
		ucontrol->value.integer.value[0] = 0;
		break;
	case SAMPLE_RATE_48KHZ:
	default:
		ucontrol->value.integer.value[0] = 1;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_rx_0_sample_rate = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int  mdm_pri_tdm_rx_0_sample_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_pri_tdm_rx_0_sample_rate = SAMPLE_RATE_16KHZ;
		break;
	case 1:
	default:
		mdm_pri_tdm_rx_0_sample_rate = SAMPLE_RATE_48KHZ;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_rx_0_sample_rate = %d\n",
		 __func__, mdm_pri_tdm_rx_0_sample_rate);
	return 0;
}

static int mdm_sec_tdm_rx_0_sample_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_sec_tdm_rx_0_sample_rate) {
	case SAMPLE_RATE_16KHZ:
		ucontrol->value.integer.value[0] = 0;
		break;
	case SAMPLE_RATE_48KHZ:
	default:
		ucontrol->value.integer.value[0] = 1;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_rx_0_sample_rate = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int  mdm_sec_tdm_rx_0_sample_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_sec_tdm_rx_0_sample_rate = SAMPLE_RATE_16KHZ;
		break;
	case 1:
	default:
		mdm_sec_tdm_rx_0_sample_rate = SAMPLE_RATE_48KHZ;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_rx_0_sample_rate = %d\n",
		 __func__, mdm_sec_tdm_rx_0_sample_rate);
	return 0;
}

static int mdm_pri_tdm_tx_0_sample_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_pri_tdm_tx_0_sample_rate) {
	case SAMPLE_RATE_16KHZ:
		ucontrol->value.integer.value[0] = 0;
		break;
	case SAMPLE_RATE_48KHZ:
	default:
		ucontrol->value.integer.value[0] = 1;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_tx_0_sample_rate = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int  mdm_pri_tdm_tx_0_sample_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_pri_tdm_tx_0_sample_rate = SAMPLE_RATE_16KHZ;
		break;
	case 1:
	default:
		mdm_pri_tdm_tx_0_sample_rate = SAMPLE_RATE_48KHZ;
		break;
	}
	pr_debug("%s: mdm_pri_tdm_tx_0_sample_rate = %d\n",
		 __func__, mdm_pri_tdm_tx_0_sample_rate);
	return 0;
}

static int mdm_sec_tdm_tx_0_sample_rate_get(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (mdm_sec_tdm_tx_0_sample_rate) {
	case SAMPLE_RATE_16KHZ:
		ucontrol->value.integer.value[0] = 0;
		break;
	case SAMPLE_RATE_48KHZ:
	default:
		ucontrol->value.integer.value[0] = 1;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_tx_0_sample_rate = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);
	return 0;
}

static int  mdm_sec_tdm_tx_0_sample_rate_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_sec_tdm_tx_0_sample_rate = SAMPLE_RATE_16KHZ;
		break;
	case 1:
	default:
		mdm_sec_tdm_tx_0_sample_rate = SAMPLE_RATE_48KHZ;
		break;
	}
	pr_debug("%s: mdm_sec_tdm_tx_0_sample_rate = %d\n",
		 __func__, mdm_sec_tdm_tx_0_sample_rate);
	return 0;
}

static int  mdm_tdm_num_slots_put(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 1:
		mdm_tdm_num_slots = 16;
		break;
	case 0:
	default:
		mdm_tdm_num_slots = 8;
		break;
	}
	pr_debug("%s: mdm_tdm_num_slots = %d\n",
		 __func__, mdm_tdm_num_slots);
	return 0;
}

static int mdm_mi2s_get_spk(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_spk_control %d", __func__, mdm_spk_control);

	ucontrol->value.integer.value[0] = mdm_spk_control;
	return 0;
}

static int mdm_tdm_num_slots_get(struct snd_kcontrol *kcontrol,
		       struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s mdm_tdm_num_slots %d\n", __func__, mdm_tdm_num_slots);

	ucontrol->value.integer.value[0] = mdm_tdm_num_slots;
	return 0;
}

static void mdm_ext_control(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	pr_debug("%s mdm_spk_control %d", __func__, mdm_spk_control);

	mutex_lock(&codec->mutex);
	if (mdm_spk_control == MDM_SPK_ON) {
		snd_soc_dapm_enable_pin(dapm, "Lineout_1 amp");
		snd_soc_dapm_enable_pin(dapm, "Lineout_2 amp");
	} else {
		snd_soc_dapm_disable_pin(dapm, "Lineout_1 amp");
		snd_soc_dapm_disable_pin(dapm, "Lineout_2 amp");
	}
	mutex_unlock(&codec->mutex);
	snd_soc_dapm_sync(dapm);
}

static int mdm_mi2s_set_spk(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);

	pr_debug("%s()\n", __func__);

	if (mdm_spk_control == ucontrol->value.integer.value[0])
		return 0;
	mdm_spk_control = ucontrol->value.integer.value[0];
	mdm_ext_control(codec);
	return 1;
}

static int mdm_hifi_ctrl(struct snd_soc_codec *codec)
{
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_card *card = codec->component.card;
	struct mdm_machine_data *pdata =
				snd_soc_card_get_drvdata(card);

	pr_debug("%s: mdm_hifi_control = %d", __func__,
		 mdm_hifi_control);
	if (pdata->hph_en1_gpio < 0) {
		pr_err("%s: hph_en1_gpio is invalid\n", __func__);
		return -EINVAL;
	}
	mutex_lock(&codec->mutex);
	if (mdm_hifi_control == MDM_HIFI_ON) {
		gpio_direction_output(pdata->hph_en1_gpio, 1);
		/* 5msec delay needed as per HW requirement */
		usleep_range(5000, 5010);
	} else {
		gpio_direction_output(pdata->hph_en1_gpio, 0);
	}
	mutex_unlock(&codec->mutex);
	snd_soc_dapm_sync(dapm);
	return 0;
}

static int mdm_hifi_get(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	pr_debug("%s: mdm_hifi_control = %d\n",
			 __func__, mdm_hifi_control);
	ucontrol->value.integer.value[0] = mdm_hifi_control;
	return 0;
}

static int mdm_hifi_put(struct snd_kcontrol *kcontrol,
			      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_soc_kcontrol_codec(kcontrol);

	pr_debug("%s() ucontrol->value.integer.value[0] = %ld\n",
		 __func__, ucontrol->value.integer.value[0]);

	mdm_hifi_control = ucontrol->value.integer.value[0];
	mdm_hifi_ctrl(codec);
	return 1;
}

static int mdm_enable_codec_ext_clk(struct snd_soc_codec *codec,
					int enable, bool dapm)
{
	tasha_cdc_mclk_enable(codec, enable, dapm);

	return 0;
}

static int mdm_mclk_event(struct snd_soc_dapm_widget *w,
			      struct snd_kcontrol *kcontrol, int event)
{
	pr_debug("%s event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		return mdm_enable_codec_ext_clk(w->codec, 1, true);
	case SND_SOC_DAPM_POST_PMD:
		return mdm_enable_codec_ext_clk(w->codec, 0, true);
	}
	return 0;
}

static void mdm_auxpcm_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	if (pdata->prim_auxpcm_mode == 1)
		ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
					"pri_mi2s_aux_master");
	else
		ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
					"pri_mi2s_aux_slave");
	if (ret)
		pr_err("%s: failed to set prim gpios to sleep: %d\n",
				__func__, ret);
}

static int mdm_auxpcm_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	pdata->prim_auxpcm_mode = mdm_auxpcm_mode;
	if (pdata->lpaif_pri_muxsel_virt_addr != NULL) {
		ret = afe_enable_lpass_core_shared_clock(MI2S_RX, CLOCK_ON);
		if (ret < 0) {
			ret = -EINVAL;
			goto done;
		}
		iowrite32(PCM_SEL << I2S_PCM_SEL_OFFSET,
				pdata->lpaif_pri_muxsel_virt_addr);
		if (pdata->lpass_mux_spkr_ctl_virt_addr != NULL) {
			if (pdata->prim_auxpcm_mode == 1)
				iowrite32(PRI_TLMM_CLKS_EN_MASTER,
					pdata->lpass_mux_spkr_ctl_virt_addr);
			else
				iowrite32(PRI_TLMM_CLKS_EN_SLAVE,
					pdata->lpass_mux_spkr_ctl_virt_addr);
		} else {
			dev_err(card->dev, "%s lpass_mux_spkr_ctl_virt_addr is NULL\n",
			__func__);
			ret = -EINVAL;
		}
	} else {
		dev_err(card->dev, "%s lpaif_pri_muxsel_virt_addr is NULL\n",
		       __func__);
		ret = -EINVAL;
		goto done;
	}

	if (pdata->prim_auxpcm_mode == 1) {
		ret = msm_gpioset_activate(CLIENT_WCD_EXT,
					"pri_mi2s_aux_master");
		if (ret < 0)
			pr_err("%s pinctrl set failed\n", __func__);
	} else {
		ret = msm_gpioset_activate(CLIENT_WCD_EXT,
					"pri_mi2s_aux_slave");
		if (ret < 0)
			pr_err("%s pinctrl set failed\n", __func__);
	}
	afe_enable_lpass_core_shared_clock(MI2S_RX, CLOCK_OFF);
done:
	return ret;
}

static struct snd_soc_ops mdm_auxpcm_be_ops = {
	.startup = mdm_auxpcm_startup,
	.shutdown = mdm_auxpcm_shutdown,
};

static void mdm_sec_auxpcm_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	int ret;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	if (pdata->sec_auxpcm_mode == 1)
		ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
					"sec_mi2s_aux_master");
	else
		ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
					"sec_mi2s_aux_slave");
	if (ret)
		pr_err("%s: failed to set sec gpios to sleep: %d\n",
			__func__, ret);
}

static int mdm_sec_auxpcm_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	pdata->sec_auxpcm_mode = mdm_sec_auxpcm_mode;
	if (pdata->lpaif_sec_muxsel_virt_addr != NULL) {
		ret = afe_enable_lpass_core_shared_clock(MI2S_RX, CLOCK_ON);
		if (ret < 0) {
			ret = -EINVAL;
			goto done;
		}
		iowrite32(PCM_SEL << I2S_PCM_SEL_OFFSET,
				pdata->lpaif_sec_muxsel_virt_addr);
		if (pdata->lpass_mux_mic_ctl_virt_addr != NULL) {
			if (pdata->sec_auxpcm_mode == 1)
				iowrite32(SEC_TLMM_CLKS_EN_MASTER,
					pdata->lpass_mux_mic_ctl_virt_addr);
			else
				iowrite32(SEC_TLMM_CLKS_EN_SLAVE,
					pdata->lpass_mux_mic_ctl_virt_addr);
		} else {
			dev_err(card->dev,
				"%s lpass_mux_mic_ctl_virt_addr is NULL\n",
				__func__);
			ret = -EINVAL;
		}
	} else {
		dev_err(card->dev,
			"%s lpaif_sec_muxsel_virt_addr is NULL\n", __func__);
		ret = -EINVAL;
		goto done;
	}

	if (pdata->sec_auxpcm_mode == 1) {
		ret = msm_gpioset_activate(CLIENT_WCD_EXT,
					"sec_mi2s_aux_master");
		if (ret < 0)
			pr_err("%s pinctrl set failed\n", __func__);
	} else {
		ret = msm_gpioset_activate(CLIENT_WCD_EXT,
					"sec_mi2s_aux_slave");
		if (ret < 0)
			pr_err("%s pinctrl set failed\n", __func__);
	}
	afe_enable_lpass_core_shared_clock(MI2S_RX, CLOCK_OFF);
done:
	return ret;
}

static struct snd_soc_ops mdm_sec_auxpcm_be_ops = {
	.startup = mdm_sec_auxpcm_startup,
	.shutdown = mdm_sec_auxpcm_shutdown,
};

static int mdm_auxpcm_rate_get(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = mdm_auxpcm_rate;
	return 0;
}

static int mdm_auxpcm_rate_put(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	switch (ucontrol->value.integer.value[0]) {
	case 0:
		mdm_auxpcm_rate = 8000;
		break;
	case 1:
		mdm_auxpcm_rate = 16000;
		break;
	default:
		mdm_auxpcm_rate = 8000;
		break;
	}
	return 0;
}

static int mdm_auxpcm_be_params_fixup(struct snd_soc_pcm_runtime *rtd,
					  struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_RATE);

	struct snd_interval *channels =
		hw_param_interval(params, SNDRV_PCM_HW_PARAM_CHANNELS);

	rate->min = rate->max = mdm_auxpcm_rate;
	channels->min = channels->max = 1;

	return 0;
}

static const struct snd_soc_dapm_widget mdm9650_dapm_widgets[] = {

	SND_SOC_DAPM_SUPPLY("MCLK",  SND_SOC_NOPM, 0, 0,
	mdm_mclk_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),

	SND_SOC_DAPM_SPK("Lineout_1 amp", NULL),
	SND_SOC_DAPM_SPK("Lineout_3 amp", NULL),
	SND_SOC_DAPM_SPK("Lineout_2 amp", NULL),
	SND_SOC_DAPM_SPK("Lineout_4 amp", NULL),
	SND_SOC_DAPM_MIC("Handset Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCRight Headset Mic", NULL),
	SND_SOC_DAPM_MIC("ANCLeft Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Analog Mic4", NULL),
	SND_SOC_DAPM_MIC("Analog Mic6", NULL),
	SND_SOC_DAPM_MIC("Analog Mic7", NULL),
	SND_SOC_DAPM_MIC("Analog Mic8", NULL),

	SND_SOC_DAPM_MIC("Digital Mic1", NULL),
	SND_SOC_DAPM_MIC("Digital Mic2", NULL),
	SND_SOC_DAPM_MIC("Digital Mic3", NULL),
	SND_SOC_DAPM_MIC("Digital Mic4", NULL),
	SND_SOC_DAPM_MIC("Digital Mic5", NULL),
	SND_SOC_DAPM_MIC("Digital Mic6", NULL),
};

static struct snd_soc_dapm_route wcd9335_audio_paths[] = {
	{"MIC BIAS1", NULL, "MCLK"},
	{"MIC BIAS2", NULL, "MCLK"},
	{"MIC BIAS3", NULL, "MCLK"},
	{"MIC BIAS4", NULL, "MCLK"},
};

static const char *const spk_function[] = {"Off", "On"};
static const char *const hifi_function[] = {"Off", "On"};
static const char *const mi2s_rx_ch_text[] = {"One", "Two"};
static const char *const mi2s_tx_ch_text[] = {"One", "Two"};
static const char *const auxpcm_rate_text[] = {"rate_8000", "rate_16000"};

static const char *const mi2s_rate_text[] = {"rate_8000",
						"rate_16000", "rate_48000"};
static const char *const mode_text[] = {"master", "slave"};

static const struct soc_enum mdm_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, spk_function),
	SOC_ENUM_SINGLE_EXT(2, mi2s_rx_ch_text),
	SOC_ENUM_SINGLE_EXT(2, mi2s_tx_ch_text),
	SOC_ENUM_SINGLE_EXT(2, auxpcm_rate_text),
	SOC_ENUM_SINGLE_EXT(3, mi2s_rate_text),
	SOC_ENUM_SINGLE_EXT(2, hifi_function),
	SOC_ENUM_SINGLE_EXT(2, mode_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tdm_ch_text),
				tdm_ch_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tdm_bit_format_text),
				tdm_bit_format_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tdm_sample_rate_text),
				tdm_sample_rate_text),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tdm_num_slots_text),
				tdm_num_slots_text),
};

static const struct snd_kcontrol_new mdm_snd_controls[] = {
	SOC_ENUM_EXT("Speaker Function",   mdm_enum[0],
				 mdm_mi2s_get_spk,
				 mdm_mi2s_set_spk),
	SOC_ENUM_EXT("MI2S_RX Channels",   mdm_enum[1],
				 mdm_mi2s_rx_ch_get,
				 mdm_mi2s_rx_ch_put),
	SOC_ENUM_EXT("MI2S_TX Channels",   mdm_enum[2],
				 mdm_mi2s_tx_ch_get,
				 mdm_mi2s_tx_ch_put),
	SOC_ENUM_EXT("AUX PCM SampleRate", mdm_enum[3],
				 mdm_auxpcm_rate_get,
				 mdm_auxpcm_rate_put),
	SOC_ENUM_EXT("MI2S SampleRate", mdm_enum[4],
				 mdm_mi2s_rate_get,
				 mdm_mi2s_rate_put),
	SOC_ENUM_EXT("SEC_MI2S_RX Channels", mdm_enum[1],
				 mdm_sec_mi2s_rx_ch_get,
				 mdm_sec_mi2s_rx_ch_put),
	SOC_ENUM_EXT("SEC_MI2S_TX Channels", mdm_enum[2],
				 mdm_sec_mi2s_tx_ch_get,
				 mdm_sec_mi2s_tx_ch_put),
	SOC_ENUM_EXT("SEC MI2S SampleRate", mdm_enum[4],
				 mdm_sec_mi2s_rate_get,
				 mdm_sec_mi2s_rate_put),
	SOC_ENUM_EXT("HiFi Function", mdm_enum[5],
				 mdm_hifi_get,
				 mdm_hifi_put),
	SOC_ENUM_EXT("MI2S Mode", mdm_enum[6],
				 mdm_mi2s_mode_get,
				 mdm_mi2s_mode_put),
	SOC_ENUM_EXT("SEC_MI2S Mode", mdm_enum[6],
				 mdm_sec_mi2s_mode_get,
				 mdm_sec_mi2s_mode_put),
	SOC_ENUM_EXT("AUXPCM Mode", mdm_enum[6],
				 mdm_auxpcm_mode_get,
				 mdm_auxpcm_mode_put),
	SOC_ENUM_EXT("SEC_AUXPCM Mode", mdm_enum[6],
				 mdm_sec_auxpcm_mode_get,
				 mdm_sec_auxpcm_mode_put),
	SOC_ENUM_EXT("SEC_TDM Mode", mdm_enum[6],
				 mdm_sec_tdm_mode_get,
				 mdm_sec_tdm_mode_put),
	SOC_ENUM_EXT("PRI_TDM_RX_0 Channels", mdm_enum[7],
			mdm_pri_tdm_rx_0_ch_get, mdm_pri_tdm_rx_0_ch_put),
	SOC_ENUM_EXT("PRI_TDM_TX_0 Channels", mdm_enum[7],
			mdm_pri_tdm_tx_0_ch_get, mdm_pri_tdm_tx_0_ch_put),
	SOC_ENUM_EXT("SEC_TDM_RX_0 Channels", mdm_enum[7],
			mdm_sec_tdm_rx_0_ch_get, mdm_sec_tdm_rx_0_ch_put),
	SOC_ENUM_EXT("SEC_TDM_TX_0 Channels", mdm_enum[7],
			mdm_sec_tdm_tx_0_ch_get, mdm_sec_tdm_tx_0_ch_put),
	SOC_ENUM_EXT("PRI_TDM_RX_0 Bit Format", mdm_enum[8],
			mdm_pri_tdm_rx_0_bit_format_get,
			mdm_pri_tdm_rx_0_bit_format_put),
	SOC_ENUM_EXT("PRI_TDM_TX_0 Bit Format", mdm_enum[8],
			mdm_pri_tdm_tx_0_bit_format_get,
			mdm_pri_tdm_tx_0_bit_format_put),
	SOC_ENUM_EXT("SEC_TDM_RX_0 Bit Format", mdm_enum[8],
			mdm_sec_tdm_rx_0_bit_format_get,
			mdm_sec_tdm_rx_0_bit_format_put),
	SOC_ENUM_EXT("SEC_TDM_TX_0 Bit Format", mdm_enum[8],
			mdm_sec_tdm_tx_0_bit_format_get,
			mdm_sec_tdm_tx_0_bit_format_put),
	SOC_ENUM_EXT("PRI_TDM_RX_0 SampleRate", mdm_enum[9],
			mdm_pri_tdm_rx_0_sample_rate_get,
			mdm_pri_tdm_rx_0_sample_rate_put),
	SOC_ENUM_EXT("PRI_TDM_TX_0 SampleRate", mdm_enum[9],
			mdm_pri_tdm_tx_0_sample_rate_get,
			mdm_pri_tdm_tx_0_sample_rate_put),
	SOC_ENUM_EXT("SEC_TDM_RX_0 SampleRate", mdm_enum[9],
			mdm_sec_tdm_rx_0_sample_rate_get,
			mdm_sec_tdm_rx_0_sample_rate_put),
	SOC_ENUM_EXT("SEC_TDM_TX_0 SampleRate", mdm_enum[9],
			mdm_sec_tdm_tx_0_sample_rate_get,
			mdm_sec_tdm_tx_0_sample_rate_put),
	SOC_ENUM_EXT("MDM_TDM Slots", mdm_enum[10],
			mdm_tdm_num_slots_get,
			mdm_tdm_num_slots_put),
};
static int mdm_tdm_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_interval *rate = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
					SNDRV_PCM_HW_PARAM_CHANNELS);
	switch (cpu_dai->id) {
	case AFE_PORT_ID_PRIMARY_TDM_RX:
		channels->min = channels->max = mdm_pri_tdm_rx_0_ch;
		param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
			mdm_pri_tdm_rx_0_bit_format);
		rate->min = rate->max = mdm_pri_tdm_rx_0_sample_rate;
		break;
	case AFE_PORT_ID_PRIMARY_TDM_TX:
		channels->min = channels->max = mdm_pri_tdm_tx_0_ch;
		param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
			mdm_pri_tdm_tx_0_bit_format);
		rate->min = rate->max = mdm_pri_tdm_tx_0_sample_rate;
		break;
	case AFE_PORT_ID_SECONDARY_TDM_RX:
		channels->min = channels->max = mdm_sec_tdm_rx_0_ch;
		param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
			mdm_sec_tdm_rx_0_bit_format);
		rate->min = rate->max = mdm_sec_tdm_rx_0_sample_rate;
		break;
	case AFE_PORT_ID_SECONDARY_TDM_TX:
		channels->min = channels->max = mdm_sec_tdm_tx_0_ch;
		param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT,
			mdm_sec_tdm_tx_0_bit_format);
		rate->min = rate->max = mdm_sec_tdm_tx_0_sample_rate;
		break;
	default:
		pr_err("%s: dai id 0x%x not supported\n",
			__func__, cpu_dai->id);
		return -EINVAL;
	}

	pr_debug("%s: dai id = 0x%x channels = %d rate = %d\n",
		__func__, cpu_dai->id, channels->max, rate->max);

	return 0;
}

static unsigned int tdm_param_set_slot_mask(u16 port_id,
				int slot_width, int slots)
{
	unsigned int slot_mask = 0;
	int upper, lower, i, j;
	unsigned int *slot_offset;

	switch (port_id) {
	case AFE_PORT_ID_PRIMARY_TDM_RX:
		lower = PRIMARY_TDM_RX_0;
		upper = PRIMARY_TDM_RX_0;
		break;
	case AFE_PORT_ID_PRIMARY_TDM_TX:
		lower = PRIMARY_TDM_TX_0;
		upper = PRIMARY_TDM_TX_0;
		break;
	case AFE_PORT_ID_SECONDARY_TDM_RX:
		lower = SECONDARY_TDM_RX_0;
		upper = SECONDARY_TDM_RX_0;
		break;
	case AFE_PORT_ID_SECONDARY_TDM_TX:
		lower = SECONDARY_TDM_TX_0;
		upper = SECONDARY_TDM_TX_0;
		break;
	default:
		return slot_mask;
	}

	for (i = lower; i <= upper; i++) {
		slot_offset = tdm_slot_offset[i];
		for (j = 0; j < TDM_SLOT_OFFSET_MAX; j++) {
			if (slot_offset[j] != AFE_SLOT_MAPPING_OFFSET_INVALID)
				/*
				 * set the mask of active slot according to
				 * the offset table for the group of devices
				 */
				slot_mask |=
				    (1 << ((slot_offset[j] * 8) / slot_width));
			else
				break;
		}
	}

	return slot_mask;
}

static int mdm_tdm_snd_hw_params(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;
	int channels, slot_width, slots;
	unsigned int slot_mask;
	unsigned int *slot_offset;
	int offset_channels = 0;
	int i;

	pr_debug("%s: dai id = 0x%x\n", __func__, cpu_dai->id);

	channels = params_channels(params);
	switch (channels) {
	case 1:
	case 2:
	case 3:
	case 4:
	case 6:
	case 8:
		switch (params_format(params)) {
		case SNDRV_PCM_FORMAT_S32_LE:
		case SNDRV_PCM_FORMAT_S24_LE:
		case SNDRV_PCM_FORMAT_S16_LE:
			/*
			 * up to 8 channel HW configuration should
			 * use 32 bit slot width for max support of
			 * stream bit width. (slot_width > bit_width)
			 */
			slot_width = 32;
			break;
		default:
			pr_err("%s: invalid param format 0x%x\n",
				__func__, params_format(params));
			return -EINVAL;
		}
		slots = mdm_tdm_num_slots;
		slot_mask = tdm_param_set_slot_mask(cpu_dai->id,
			slot_width, slots);
		if (!slot_mask) {
			pr_err("%s: invalid slot_mask 0x%x\n",
				__func__, slot_mask);
			return -EINVAL;
		}
		break;
	default:
		pr_err("%s: invalid param channels %d\n",
			__func__, channels);
		return -EINVAL;
	}
	switch (cpu_dai->id) {
	case AFE_PORT_ID_PRIMARY_TDM_RX:
		slot_offset = tdm_slot_offset[PRIMARY_TDM_RX_0];
		break;
	case AFE_PORT_ID_PRIMARY_TDM_TX:
		slot_offset = tdm_slot_offset[PRIMARY_TDM_TX_0];
		break;
	case AFE_PORT_ID_SECONDARY_TDM_RX:
		slot_offset = tdm_slot_offset[SECONDARY_TDM_RX_0];
		break;
	case AFE_PORT_ID_SECONDARY_TDM_TX:
		slot_offset = tdm_slot_offset[SECONDARY_TDM_TX_0];
		break;
	default:
		pr_err("%s: dai id 0x%x not supported\n",
			__func__, cpu_dai->id);
		return -EINVAL;
	}

	for (i = 0; i < TDM_SLOT_OFFSET_MAX; i++) {
		if (slot_offset[i] != AFE_SLOT_MAPPING_OFFSET_INVALID)
			offset_channels++;
		else
			break;
	}
	if (offset_channels == 0) {
		pr_err("%s: slot offset not supported, offset_channels %d\n",
			__func__, offset_channels);
		return -EINVAL;
	}

	if (channels > offset_channels) {
		pr_err("%s: channels %d exceed offset_channels %d\n",
			__func__, channels, offset_channels);
		return -EINVAL;
	}

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, 0, slot_mask,
			slots, slot_width);
		if (ret < 0) {
			pr_err("%s: failed to set tdm slot, err:%d\n",
				__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai,
			0, NULL, channels, slot_offset);
		if (ret < 0) {
			pr_err("%s: failed to set channel map, err:%d\n",
				__func__, ret);
			goto end;
		}
	} else {
		ret = snd_soc_dai_set_tdm_slot(cpu_dai, slot_mask, 0,
			slots, slot_width);
		if (ret < 0) {
			pr_err("%s: failed to set tdm slot, err:%d\n",
				__func__, ret);
			goto end;
		}

		ret = snd_soc_dai_set_channel_map(cpu_dai,
			channels, slot_offset, 0, NULL);
		if (ret < 0) {
			pr_err("%s: failed to set channel map, err:%d\n",
				__func__, ret);
			goto end;
		}
	}

end:
	return ret;
}

static int mdm_tdm_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata =
			snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0;

	pr_debug("%s(): substream = %s  stream = %d\n", __func__,
			 substream->name, substream->stream);
	pr_debug("%s: dai id = 0x%x\n", __func__, cpu_dai->id);

	switch (cpu_dai->id) {
	case AFE_PORT_ID_PRIMARY_TDM_RX:
	case AFE_PORT_ID_PRIMARY_TDM_RX_1:
	case AFE_PORT_ID_PRIMARY_TDM_RX_2:
	case AFE_PORT_ID_PRIMARY_TDM_RX_3:
	case AFE_PORT_ID_PRIMARY_TDM_RX_4:
	case AFE_PORT_ID_PRIMARY_TDM_RX_5:
	case AFE_PORT_ID_PRIMARY_TDM_RX_6:
	case AFE_PORT_ID_PRIMARY_TDM_RX_7:
	case AFE_PORT_ID_PRIMARY_TDM_TX:
	case AFE_PORT_ID_PRIMARY_TDM_TX_1:
	case AFE_PORT_ID_PRIMARY_TDM_TX_2:
	case AFE_PORT_ID_PRIMARY_TDM_TX_3:
	case AFE_PORT_ID_PRIMARY_TDM_TX_4:
	case AFE_PORT_ID_PRIMARY_TDM_TX_5:
	case AFE_PORT_ID_PRIMARY_TDM_TX_6:
	case AFE_PORT_ID_PRIMARY_TDM_TX_7:
		break;
	case AFE_PORT_ID_SECONDARY_TDM_RX:
	case AFE_PORT_ID_SECONDARY_TDM_RX_1:
	case AFE_PORT_ID_SECONDARY_TDM_RX_2:
	case AFE_PORT_ID_SECONDARY_TDM_RX_3:
	case AFE_PORT_ID_SECONDARY_TDM_RX_4:
	case AFE_PORT_ID_SECONDARY_TDM_RX_5:
	case AFE_PORT_ID_SECONDARY_TDM_RX_6:
	case AFE_PORT_ID_SECONDARY_TDM_RX_7:
	case AFE_PORT_ID_SECONDARY_TDM_TX:
	case AFE_PORT_ID_SECONDARY_TDM_TX_1:
	case AFE_PORT_ID_SECONDARY_TDM_TX_2:
	case AFE_PORT_ID_SECONDARY_TDM_TX_3:
	case AFE_PORT_ID_SECONDARY_TDM_TX_4:
	case AFE_PORT_ID_SECONDARY_TDM_TX_5:
	case AFE_PORT_ID_SECONDARY_TDM_TX_6:
	case AFE_PORT_ID_SECONDARY_TDM_TX_7:

	pdata->sec_tdm_mode = mdm_sec_tdm_mode;
	if (atomic_inc_return(&sec_tdm_ref_count) == 1) {
		if (pdata->lpaif_sec_muxsel_virt_addr != NULL) {
			ret = afe_enable_lpass_core_shared_clock(MI2S_RX,
				CLOCK_ON);
			if (ret < 0) {
				ret = -EINVAL;
				goto done;
			}
			iowrite32(PCM_SEL << I2S_PCM_SEL_OFFSET,
				pdata->lpaif_sec_muxsel_virt_addr);
			if (pdata->lpass_mux_mic_ctl_virt_addr != NULL) {
				if (pdata->sec_tdm_mode == 1) {
					iowrite32(SEC_TLMM_CLKS_EN_MASTER,
					pdata->lpass_mux_mic_ctl_virt_addr);
					}
				else{
					iowrite32(SEC_TLMM_CLKS_EN_SLAVE,
					pdata->lpass_mux_mic_ctl_virt_addr);
				}
			} else {
				pr_err("%s lpass_mux_mic_ctl_virt_addr is NULL\n",
					__func__);
				ret = -EINVAL;
				goto err;
			}
		} else {
			pr_err("%s lpaif_sec_muxsel_virt_addr is NULL\n",
					__func__);
			ret = -EINVAL;
			goto done;
		}
		if (pdata->sec_tdm_mode == 1) {
			ret = msm_gpioset_activate(CLIENT_WCD_EXT,
					"sec_mi2s_aux_master");
			if (ret < 0) {
				pr_err("%s pinctrl set failed\n", __func__);
				goto err;
			}
		} else {
			ret = msm_gpioset_activate(CLIENT_WCD_EXT,
			"sec_mi2s_aux_slave");
			if (ret < 0) {
				pr_err("%s pinctrl set failed\n", __func__);
				goto err;
			}
		}
		afe_enable_lpass_core_shared_clock(MI2S_RX, CLOCK_OFF);
	}
		break;
	default:
		pr_err("%s: dai id 0x%x not supported\n",
		       __func__, cpu_dai->id);
		break;
		return -EINVAL;
	}
err:
done:
	if (ret < 0)
		atomic_dec(&sec_tdm_ref_count);
	return ret;

}

static void mdm_tdm_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct mdm_machine_data *pdata =
			snd_soc_card_get_drvdata(card);
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret;

	switch (cpu_dai->id) {
	case AFE_PORT_ID_PRIMARY_TDM_RX:
	case AFE_PORT_ID_PRIMARY_TDM_RX_1:
	case AFE_PORT_ID_PRIMARY_TDM_RX_2:
	case AFE_PORT_ID_PRIMARY_TDM_RX_3:
	case AFE_PORT_ID_PRIMARY_TDM_RX_4:
	case AFE_PORT_ID_PRIMARY_TDM_RX_5:
	case AFE_PORT_ID_PRIMARY_TDM_RX_6:
	case AFE_PORT_ID_PRIMARY_TDM_RX_7:
	case AFE_PORT_ID_PRIMARY_TDM_TX:
	case AFE_PORT_ID_PRIMARY_TDM_TX_1:
	case AFE_PORT_ID_PRIMARY_TDM_TX_2:
	case AFE_PORT_ID_PRIMARY_TDM_TX_3:
	case AFE_PORT_ID_PRIMARY_TDM_TX_4:
	case AFE_PORT_ID_PRIMARY_TDM_TX_5:
	case AFE_PORT_ID_PRIMARY_TDM_TX_6:
	case AFE_PORT_ID_PRIMARY_TDM_TX_7:
		break;
	case AFE_PORT_ID_SECONDARY_TDM_RX:
	case AFE_PORT_ID_SECONDARY_TDM_RX_1:
	case AFE_PORT_ID_SECONDARY_TDM_RX_2:
	case AFE_PORT_ID_SECONDARY_TDM_RX_3:
	case AFE_PORT_ID_SECONDARY_TDM_RX_4:
	case AFE_PORT_ID_SECONDARY_TDM_RX_5:
	case AFE_PORT_ID_SECONDARY_TDM_RX_6:
	case AFE_PORT_ID_SECONDARY_TDM_RX_7:
	case AFE_PORT_ID_SECONDARY_TDM_TX:
	case AFE_PORT_ID_SECONDARY_TDM_TX_1:
	case AFE_PORT_ID_SECONDARY_TDM_TX_2:
	case AFE_PORT_ID_SECONDARY_TDM_TX_3:
	case AFE_PORT_ID_SECONDARY_TDM_TX_4:
	case AFE_PORT_ID_SECONDARY_TDM_TX_5:
	case AFE_PORT_ID_SECONDARY_TDM_TX_6:
	case AFE_PORT_ID_SECONDARY_TDM_TX_7:
		if (atomic_dec_return(&sec_tdm_ref_count) == 0) {
			if (pdata->sec_tdm_mode == 1)
				ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
					"sec_mi2s_aux_master");
			else
				ret = msm_gpioset_suspend(CLIENT_WCD_EXT,
					"sec_mi2s_aux_slave");
			if (ret)
				pr_err("failed to set sec gpios to sleep:%d\n",
					ret);
		}
		break;
	}
}

static struct snd_soc_ops mdm_tdm_be_ops = {
	.startup = mdm_tdm_startup,
	.hw_params = mdm_tdm_snd_hw_params,
	.shutdown = mdm_tdm_shutdown,
};

static int mdm_mi2s_audrx_init(struct snd_soc_pcm_runtime *rtd)
{
	int ret = 0;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_card *card;
	struct snd_info_entry *entry;
	struct mdm_machine_data *pdata =
				snd_soc_card_get_drvdata(rtd->card);

	pr_debug("%s dev_name %s\n", __func__, dev_name(cpu_dai->dev));

	rtd->pmdown_time = 0;
	ret = snd_soc_add_codec_controls(codec, mdm_snd_controls,
					 ARRAY_SIZE(mdm_snd_controls));
	if (ret < 0) {
		pr_err("%s: add_codec_controls failed, %d\n",
			__func__, ret);
		goto done;
	}

	snd_soc_dapm_new_controls(dapm, mdm9650_dapm_widgets,
				  ARRAY_SIZE(mdm9650_dapm_widgets));

	snd_soc_dapm_add_routes(dapm, wcd9335_audio_paths,
				ARRAY_SIZE(wcd9335_audio_paths));

	/*
	 * After DAPM Enable pins always
	 * DAPM SYNC needs to be called.
	 */
	snd_soc_dapm_enable_pin(dapm, "Lineout_1 amp");
	snd_soc_dapm_enable_pin(dapm, "Lineout_3 amp");
	snd_soc_dapm_enable_pin(dapm, "Lineout_2 amp");
	snd_soc_dapm_enable_pin(dapm, "Lineout_4 amp");

	snd_soc_dapm_ignore_suspend(dapm, "Lineout_1 amp");
	snd_soc_dapm_ignore_suspend(dapm, "Lineout_3 amp");
	snd_soc_dapm_ignore_suspend(dapm, "Lineout_2 amp");
	snd_soc_dapm_ignore_suspend(dapm, "Lineout_4 amp");
	snd_soc_dapm_ignore_suspend(dapm, "ultrasound amp");
	snd_soc_dapm_ignore_suspend(dapm, "Handset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "ANCRight Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "ANCLeft Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic1");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic2");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic3");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic4");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic5");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic6");

	snd_soc_dapm_ignore_suspend(dapm, "MADINPUT");
	snd_soc_dapm_ignore_suspend(dapm, "MAD_CPE_INPUT");
	snd_soc_dapm_ignore_suspend(dapm, "EAR");
	snd_soc_dapm_ignore_suspend(dapm, "HEADPHONE");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT1");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT2");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT3");
	snd_soc_dapm_ignore_suspend(dapm, "LINEOUT4");
	snd_soc_dapm_ignore_suspend(dapm, "SPK_OUT");
	snd_soc_dapm_ignore_suspend(dapm, "ANC HEADPHONE");
	snd_soc_dapm_ignore_suspend(dapm, "ANC EAR");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC1");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC2");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC3");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC4");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC5");
	snd_soc_dapm_ignore_suspend(dapm, "AMIC6");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC1");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC2");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC3");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC4");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC5");
	snd_soc_dapm_ignore_suspend(dapm, "Digital Mic0");
	snd_soc_dapm_ignore_suspend(dapm, "DMIC0");
	snd_soc_dapm_ignore_suspend(dapm, "SPK1 OUT");
	snd_soc_dapm_ignore_suspend(dapm, "SPK2 OUT");
	snd_soc_dapm_ignore_suspend(dapm, "AIF4 VI");
	snd_soc_dapm_ignore_suspend(dapm, "VIINPUT");
	snd_soc_dapm_ignore_suspend(dapm, "HPHL");
	snd_soc_dapm_ignore_suspend(dapm, "HPHR");
	snd_soc_dapm_ignore_suspend(dapm, "ANC HPHL");
	snd_soc_dapm_ignore_suspend(dapm, "ANC HPHR");
	snd_soc_dapm_ignore_suspend(dapm, "ANC LINEOUT1");
	snd_soc_dapm_ignore_suspend(dapm, "ANC LINEOUT2");

	snd_soc_dapm_sync(dapm);

	wcd_mbhc_cfg.calibration = def_tasha_mbhc_cal();
	if (wcd_mbhc_cfg.calibration)
		ret = tasha_mbhc_hs_detect(codec, &wcd_mbhc_cfg);
	else
		ret = -ENOMEM;

	card = rtd->card->snd_card;
	entry = snd_register_module_info(card->module,
					 "codecs",
					 card->proc_root);
	if (!entry) {
		pr_debug("%s: Cannot create codecs module entry\n",
			 __func__);
		ret = 0;
		goto done;
	}
	pdata->codec_root = entry;
	tasha_codec_info_create_codec_entry(pdata->codec_root, codec);
done:
	return ret;
}

static int mdm_mi2s_audrx_init_auto(struct snd_soc_pcm_runtime *rtd)
{
	int ret = 0;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;

	pr_debug("%s dev_name %s\n", __func__, dev_name(cpu_dai->dev));

	rtd->pmdown_time = 0;
	ret = snd_soc_add_codec_controls(codec, mdm_snd_controls,
					 ARRAY_SIZE(mdm_snd_controls));
	if (ret < 0) {
		pr_err("%s: add_codec_controls failed, %d\n",
			__func__, ret);
		goto done;
	}

done:
	return ret;
}

static void *def_tasha_mbhc_cal(void)
{
	void *tasha_wcd_cal;
	struct wcd_mbhc_btn_detect_cfg *btn_cfg;
	u16 *btn_high;

	tasha_wcd_cal = kzalloc(WCD_MBHC_CAL_SIZE(WCD_MBHC_DEF_BUTTONS,
				WCD9XXX_MBHC_DEF_RLOADS), GFP_KERNEL);
	if (!tasha_wcd_cal)
		return NULL;

#define S(X, Y) ((WCD_MBHC_CAL_PLUG_TYPE_PTR(tasha_wcd_cal)->X) = (Y))
	S(v_hs_max, 1500);
#undef S
#define S(X, Y) ((WCD_MBHC_CAL_BTN_DET_PTR(tasha_wcd_cal)->X) = (Y))
	S(num_btn, WCD_MBHC_DEF_BUTTONS);
#undef S

	btn_cfg = WCD_MBHC_CAL_BTN_DET_PTR(tasha_wcd_cal);
	btn_high = ((void *)&btn_cfg->_v_btn_low) +
		(sizeof(btn_cfg->_v_btn_low[0]) * btn_cfg->num_btn);

	btn_high[0] = 75;
	btn_high[1] = 150;
	btn_high[2] = 237;
	btn_high[3] = 450;
	btn_high[4] = 500;
	btn_high[5] = 590;
	btn_high[6] = 675;
	btn_high[7] = 780;

	return tasha_wcd_cal;
}

/* Digital audio interface connects codec <---> CPU */
static struct snd_soc_dai_link mdm_dai[] = {
	/* FrontEnd DAI Links */
	{
		.name = "MDM Media1",
		.stream_name = "MultiMedia1",
		.cpu_dai_name = "MultiMedia1",
		.platform_name  = "msm-pcm-dsp.0",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		/* This dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA1
	},
	{
		.name = "MSM VoIP",
		.stream_name = "VoIP",
		.cpu_dai_name = "VoIP",
		.platform_name  = "msm-voip-dsp",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		/* This dainlink has VOIP support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_VOIP,
	},
	{
		.name = "Circuit-Switch Voice",
		.stream_name = "CS-Voice",
		.cpu_dai_name   = "CS-VOICE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* This dainlink has Voice support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_CS_VOICE,
	},
	{
		.name = "Primary MI2S RX Hostless",
		.stream_name = "Primary MI2S_RX Hostless Playback",
		.cpu_dai_name = "PRI_MI2S_RX_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "VoLTE",
		.stream_name = "VoLTE",
		.cpu_dai_name   = "VoLTE",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.be_id = MSM_FRONTEND_DAI_VOLTE,
	},
	{	.name = "MSM AFE-PCM RX",
		.stream_name = "AFE-PROXY RX",
		.cpu_dai_name = "msm-dai-q6-dev.241",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
	},
	{
		.name = "MSM AFE-PCM TX",
		.stream_name = "AFE-PROXY TX",
		.cpu_dai_name = "msm-dai-q6-dev.240",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.platform_name  = "msm-pcm-afe",
		.ignore_suspend = 1,
	},
	{
		.name = "DTMF RX Hostless",
		.stream_name = "DTMF RX Hostless",
		.cpu_dai_name	= "DTMF_RX_HOSTLESS",
		.platform_name	= "msm-pcm-dtmf",
		.dynamic = 1,
		.dpcm_playback = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
				SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_DTMF_RX,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
	},
	{
		.name = "DTMF TX",
		.stream_name = "DTMF TX",
		.cpu_dai_name = "msm-dai-stub-dev.4",
		.platform_name = "msm-pcm-dtmf",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
	},
	{
		.name = "MDM Compress1",
		.stream_name = "COMPR",
		.cpu_dai_name = "MultiMedia4",
		.platform_name = "msm-compress-dsp",
		.dynamic = 1,
		.async_ops = ASYNC_DPCM_SND_SOC_HW_PARAMS,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA4,
	},
	{
		.name = "CS-VOICE HOST RX CAPTURE",
		.stream_name = "CS-VOICE HOST RX CAPTURE",
		.cpu_dai_name = "msm-dai-stub-dev.5",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
	},
	{
		.name = "CS-VOICE HOST RX PLAYBACK",
		.stream_name = "CS-VOICE HOST RX PLAYBACK",
		.cpu_dai_name = "msm-dai-stub-dev.6",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
	},
	{
		.name = "CS-VOICE HOST TX CAPTURE",
		.stream_name = "CS-VOICE HOST TX CAPTURE",
		.cpu_dai_name = "msm-dai-stub-dev.7",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
	},
	{
		.name = "CS-VOICE HOST TX PLAYBACK",
		.stream_name = "CS-VOICE HOST TX PLAYBACK",
		.cpu_dai_name = "msm-dai-stub-dev.8",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.ignore_suspend = 1,
		 .ignore_pmdown_time = 1,
	},
	{
		.name = "MDM Media2",
		.stream_name = "MultiMedia2",
		.cpu_dai_name   = "MultiMedia2",
		.platform_name  = "msm-pcm-dsp.0",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		/* this dainlink has playback support */
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA2,
	},
	{
		.name = "MDM Media6",
		.stream_name = "MultiMedia6",
		.cpu_dai_name   = "MultiMedia6",
		.platform_name  = "msm-pcm-loopback",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		/* this dainlink has playback support */
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA6,
	},
	{
		.name = "Primary MI2S TX Hostless",
		.stream_name = "Primary MI2S_TX Hostless Playback",
		.cpu_dai_name = "PRI_MI2S_TX_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "MDM LowLatency",
		.stream_name = "MultiMedia5",
		.cpu_dai_name   = "MultiMedia5",
		.platform_name  = "msm-pcm-dsp.1",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.ignore_suspend = 1,
		/* this dainlink has playback support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_MULTIMEDIA5,
	},
	{
		.name = "MDM VoiceMMode1",
		.stream_name = "VoiceMMode1",
		.cpu_dai_name   = "VoiceMMode1",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* This dainlink has Voice support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_VOICEMMODE1,
	},
	{
		.name = "MDM VoiceMMode2",
		.stream_name = "VoiceMMode2",
		.cpu_dai_name   = "VoiceMMode2",
		.platform_name  = "msm-pcm-voice",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		/* This dainlink has Voice support */
		.ignore_pmdown_time = 1,
		.be_id = MSM_FRONTEND_DAI_VOICEMMODE2,
	},
	{
		.name = "VoiceMMode1 HOST RX CAPTURE",
		.stream_name = "VoiceMMode1 HOST RX CAPTURE",
		.cpu_dai_name = "msm-dai-stub-dev.5",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
	},
	{
		.name = "VoiceMMode1 HOST RX PLAYBACK",
		.stream_name = "VoiceMMode1 HOST RX PLAYBACK",
		.cpu_dai_name = "msm-dai-stub-dev.6",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
	},
	{
		.name = "VoiceMMode1 HOST TX CAPTURE",
		.stream_name = "VoiceMMode1 HOST TX CAPTURE",
		.cpu_dai_name = "msm-dai-stub-dev.7",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
	},
	{
		.name = "VoiceMMode1 HOST TX PLAYBACK",
		.stream_name = "VoiceMMode1 HOST TX PLAYBACK",
		.cpu_dai_name = "msm-dai-stub-dev.8",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.ignore_suspend = 1,
		 .ignore_pmdown_time = 1,
	},
	{
		.name = "VoiceMMode2 HOST RX CAPTURE",
		.stream_name = "VoiceMMode2 HOST RX CAPTURE",
		.cpu_dai_name = "msm-dai-stub-dev.5",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
	},
	{
		.name = "VoiceMMode2 HOST RX PLAYBACK",
		.stream_name = "VOiceMMode2 HOST RX PLAYBACK",
		.cpu_dai_name = "msm-dai-stub-dev.6",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
	},
	{
		.name = "VoiceMMode2 HOST TX CAPTURE",
		.stream_name = "VoiceMMode2 HOST TX CAPTURE",
		.cpu_dai_name = "msm-dai-stub-dev.7",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.ignore_suspend = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
	},
	{
		.name = "VoiceMMode2 HOST TX PLAYBACK",
		.stream_name = "VOiceMMode2 HOST TX PLAYBACK",
		.cpu_dai_name = "msm-dai-stub-dev.8",
		.platform_name  = "msm-voice-host-pcm",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.ignore_suspend = 1,
		 .ignore_pmdown_time = 1,
	},
	{
		.name = "Secondary MI2S RX Hostless",
		.stream_name = "Secondary MI2S_RX Hostless Playback",
		.cpu_dai_name = "SEC_MI2S_RX_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Secondary MI2S TX Hostless",
		.stream_name = "Secondary MI2S_TX Hostless Playback",
		.cpu_dai_name = "SEC_MI2S_TX_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Primary AUXPCM RX Hostless",
		.stream_name = "AUXPCM_HOSTLESS Playback",
		.cpu_dai_name = "AUXPCM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Primary AUXPCM TX Hostless",
		.stream_name = "AUXPCM_HOSTLESS Capture",
		.cpu_dai_name = "AUXPCM_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Secondary AUXPCM RX Hostless",
		.stream_name = "SEC_AUXPCM_HOSTLESS Playback",
		.cpu_dai_name = "SEC_AUXPCM_RX_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Secondary AUXPCM TX Hostless",
		.stream_name = "SEC_AUXPCM_HOSTLESS Capture",
		.cpu_dai_name = "SEC_AUXPCM_TX_HOSTLESS",
		.platform_name  = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	/* Backend DAI Links */
	{
		.name = LPASS_BE_AFE_PCM_RX,
		.stream_name = "AFE Playback",
		.cpu_dai_name = "msm-dai-q6-dev.224",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_RX,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_AFE_PCM_TX,
		.stream_name = "AFE Capture",
		.cpu_dai_name = "msm-dai-q6-dev.225",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_AFE_PCM_TX,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_AUXPCM_RX,
		.stream_name = "AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6-auxpcm.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_RX,
		.be_hw_params_fixup = mdm_auxpcm_be_params_fixup,
		.ops = &mdm_auxpcm_be_ops,
		.ignore_pmdown_time = 1,
		/* this dainlink has playback support */
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_AUXPCM_TX,
		.stream_name = "AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6-auxpcm.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_AUXPCM_TX,
		.be_hw_params_fixup = mdm_auxpcm_be_params_fixup,
		.ops = &mdm_auxpcm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_AUXPCM_RX,
		.stream_name = "Sec AUX PCM Playback",
		.cpu_dai_name = "msm-dai-q6-auxpcm.2",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_SEC_AUXPCM_RX,
		.be_hw_params_fixup = mdm_auxpcm_be_params_fixup,
		.ops = &mdm_sec_auxpcm_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_AUXPCM_TX,
		.stream_name = "Sec AUX PCM Capture",
		.cpu_dai_name = "msm-dai-q6-auxpcm.2",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_SEC_AUXPCM_TX,
		.be_hw_params_fixup = mdm_auxpcm_be_params_fixup,
		.ops = &mdm_sec_auxpcm_be_ops,
		.ignore_suspend = 1,
	},
	/* Incall Record Uplink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_TX,
		.stream_name = "Voice Uplink Capture",
		.cpu_dai_name = "msm-dai-q6-dev.32772",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_TX,
		.be_hw_params_fixup = mdm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Record Downlink BACK END DAI Link */
	{
		.name = LPASS_BE_INCALL_RECORD_RX,
		.stream_name = "Voice Downlink Capture",
		.cpu_dai_name = "msm-dai-q6-dev.32771",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_INCALL_RECORD_RX,
		.be_hw_params_fixup = mdm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	/* Incall Music BACK END DAI Link */
	{
		.name = LPASS_BE_VOICE_PLAYBACK_TX,
		.stream_name = "Voice Farend Playback",
		.cpu_dai_name = "msm-dai-q6-dev.32773",
		.platform_name = "msm-pcm-routing",
		.codec_name     = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_VOICE_PLAYBACK_TX,
		.be_hw_params_fixup = mdm_be_hw_params_fixup,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_MI2S_RX,
		.stream_name = "Secondary MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_SECONDARY_MI2S_RX,
		.be_hw_params_fixup = &mdm_sec_mi2s_rx_be_hw_params_fixup,
		.ops = &mdm_sec_mi2s_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_MI2S_TX,
		.stream_name = "Secondary MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s.1",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_SECONDARY_MI2S_TX,
		.be_hw_params_fixup = &mdm_sec_mi2s_tx_be_hw_params_fixup,
		.ops = &mdm_sec_mi2s_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	/* FE TDM DAI links */
	{
		.name = "Primary TDM RX 0 Hostless",
		.stream_name = "Primary TDM RX 0 Hostless",
		.cpu_dai_name = "PRI_TDM_RX_0_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Primary TDM TX 0 Hostless",
		.stream_name = "Primary TDM TX 0 Hostless",
		.cpu_dai_name = "PRI_TDM_TX_0_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Secondary TDM RX 0 Hostless",
		.stream_name = "Secondary TDM RX 0 Hostless",
		.cpu_dai_name = "SEC_TDM_RX_0_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_playback = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
	{
		.name = "Secondary TDM TX 0 Hostless",
		.stream_name = "Secondary TDM TX 0 Hostless",
		.cpu_dai_name = "SEC_TDM_TX_0_HOSTLESS",
		.platform_name = "msm-pcm-hostless",
		.dynamic = 1,
		.dpcm_capture = 1,
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST},
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
	},
};

static struct snd_soc_dai_link mdm_tasha_dai[] = {
	/* Backend DAI Links */
	{
		.name = LPASS_BE_PRI_MI2S_RX,
		.stream_name = "Primary MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s.0",
		.platform_name = "msm-pcm-routing",
		.codec_name = "tasha_codec",
		.codec_dai_name = "tasha_i2s_rx1",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_PRI_MI2S_RX,
		.init  = &mdm_mi2s_audrx_init,
		.be_hw_params_fixup = &mdm_mi2s_rx_be_hw_params_fixup,
		.ops = &mdm_mi2s_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_PRI_MI2S_TX,
		.stream_name = "Primary MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s.0",
		.platform_name = "msm-pcm-routing",
		.codec_name = "tasha_codec",
		.codec_dai_name = "tasha_i2s_tx1",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_PRI_MI2S_TX,
		.be_hw_params_fixup = &mdm_mi2s_tx_be_hw_params_fixup,
		.ops = &mdm_mi2s_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	/* TDM be dai links */
	{
		.name = LPASS_BE_PRI_TDM_RX_0,
		.stream_name = "Primary TDM0 Playback",
		.cpu_dai_name = "msm-dai-q6-tdm.36864",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_PRI_TDM_RX_0,
		.be_hw_params_fixup = mdm_tdm_be_hw_params_fixup,
		.ops = &mdm_tdm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_PRI_TDM_TX_0,
		.stream_name = "Primary TDM0 Capture",
		.cpu_dai_name = "msm-dai-q6-tdm.36865",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_PRI_TDM_TX_0,
		.be_hw_params_fixup = mdm_tdm_be_hw_params_fixup,
		.ops = &mdm_tdm_be_ops,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
	},
	{
		.name = LPASS_BE_SEC_TDM_RX_0,
		.stream_name = "Secondary TDM0 Playback",
		.cpu_dai_name = "msm-dai-q6-tdm.36880",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-rx",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_SEC_TDM_RX_0,
		.be_hw_params_fixup = mdm_tdm_be_hw_params_fixup,
		.ops = &mdm_tdm_be_ops,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_SEC_TDM_TX_0,
		.stream_name = "Secondary TDM0 Capture",
		.cpu_dai_name = "msm-dai-q6-tdm.36881",
		.platform_name = "msm-pcm-routing",
		.codec_name = "msm-stub-codec.1",
		.codec_dai_name = "msm-stub-tx",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_SEC_TDM_TX_0,
		.be_hw_params_fixup = mdm_tdm_be_hw_params_fixup,
		.ops = &mdm_tdm_be_ops,
		.ignore_suspend = 1,
		.ignore_pmdown_time = 1,
	},
};

static struct snd_soc_dai_link mdm_auto_dai[] = {
	/* Backend DAI Links */
	{
		.name = LPASS_BE_PRI_MI2S_RX,
		.stream_name = "Primary MI2S Playback",
		.cpu_dai_name = "msm-dai-q6-mi2s.0",
		.platform_name = "msm-pcm-routing",
		.codec_name = "tlv320aic3x-codec",
		.codec_dai_name = "tlv320aic3x-hifi",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.be_id = MSM_BACKEND_DAI_PRI_MI2S_RX,
		.init  = &mdm_mi2s_audrx_init_auto,
		.be_hw_params_fixup = &mdm_mi2s_rx_be_hw_params_fixup,
		.ops = &mdm_mi2s_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
	{
		.name = LPASS_BE_PRI_MI2S_TX,
		.stream_name = "Primary MI2S Capture",
		.cpu_dai_name = "msm-dai-q6-mi2s.0",
		.platform_name = "msm-pcm-routing",
		.codec_name = "tlv320aic3x-codec",
		.codec_dai_name = "tlv320aic3x-hifi",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.be_id = MSM_BACKEND_DAI_PRI_MI2S_TX,
		.be_hw_params_fixup = &mdm_mi2s_tx_be_hw_params_fixup,
		.ops = &mdm_mi2s_be_ops,
		.ignore_pmdown_time = 1,
		.ignore_suspend = 1,
	},
};

static struct snd_soc_dai_link *mdm_tasha_dai_links;

static struct snd_soc_dai_link *mdm_auto_dai_links;

static struct snd_soc_card snd_soc_card_mdm_tasha = {
	.name = "mdm-tasha-i2s-snd-card",
};

static struct snd_soc_card snd_soc_card_mdm_auto = {
	.name = "mdm-auto-i2s-snd-card",
};

static int mdm_populate_dai_link_component_of_node(
					struct snd_soc_card *card)
{
	int i, index, ret = 0;
	struct device *cdev = card->dev;
	struct snd_soc_dai_link *dai_link = card->dai_link;
	struct device_node *np;

	if (!cdev) {
		pr_err("%s: Sound card device memory NULL\n", __func__);
		return -ENODEV;
	}

	for (i = 0; i < card->num_links; i++) {
		if (dai_link[i].platform_of_node && dai_link[i].cpu_of_node)
			continue;

		/* populate platform_of_node for snd card dai links */
		if (dai_link[i].platform_name &&
		    !dai_link[i].platform_of_node) {
			index = of_property_match_string(cdev->of_node,
						"asoc-platform-names",
						dai_link[i].platform_name);
			if (index < 0) {
				pr_debug("%s: No match found for platform name: %s\n",
					__func__, dai_link[i].platform_name);
				ret = index;
				goto err;
			}

			np = of_parse_phandle(cdev->of_node, "asoc-platform",
					      index);
			if (!np) {
				pr_err("%s: retrieving phandle for platform %s, index %d failed\n",
					__func__, dai_link[i].platform_name,
					index);
				ret = -ENODEV;
				goto err;
			}
			dai_link[i].platform_of_node = np;
			dai_link[i].platform_name = NULL;
		}

		/* populate cpu_of_node for snd card dai links */
		if (dai_link[i].cpu_dai_name && !dai_link[i].cpu_of_node) {
			index = of_property_match_string(cdev->of_node,
						 "asoc-cpu-names",
						 dai_link[i].cpu_dai_name);
			if (index >= 0) {
				np = of_parse_phandle(cdev->of_node, "asoc-cpu",
						index);
				if (!np) {
					pr_err("%s: retrieving phandle for cpu dai %s failed\n",
						__func__,
						dai_link[i].cpu_dai_name);
					ret = -ENODEV;
					goto err;
				}
				dai_link[i].cpu_of_node = np;
				dai_link[i].cpu_dai_name = NULL;
			}
		}

		/* populate codec_of_node for snd card dai links */
		if (dai_link[i].codec_name && !dai_link[i].codec_of_node) {
			index = of_property_match_string(cdev->of_node,
						 "asoc-codec-names",
						 dai_link[i].codec_name);
			if (index < 0)
				continue;
			np = of_parse_phandle(cdev->of_node, "asoc-codec",
					      index);
			if (!np) {
				pr_err("%s: retrieving phandle for codec %s failed\n",
					__func__, dai_link[i].codec_name);
				ret = -ENODEV;
				goto err;
			}
			dai_link[i].codec_of_node = np;
			dai_link[i].codec_name = NULL;
		}
	}

err:
	return ret;
}


static int mdm_init_wsa_dev(struct platform_device *pdev,
				struct snd_soc_card *card)
{
	struct device_node *wsa_of_node;
	u32 wsa_max_devs;
	u32 wsa_dev_cnt;
	char *dev_name_str = NULL;
	struct mdm_wsa881x_dev_info *wsa881x_dev_info;
	const char *wsa_auxdev_name_prefix[1];
	int found = 0;
	int i;
	int ret;

	/* Get maximum WSA device count for this platform */
	ret = of_property_read_u32(pdev->dev.of_node,
				   "qcom,wsa-max-devs", &wsa_max_devs);
	if (ret) {
		dev_dbg(&pdev->dev,
			 "%s: wsa-max-devs property missing in DT %s, ret = %d\n",
			 __func__, pdev->dev.of_node->full_name, ret);
		return 0;
	}
	if (wsa_max_devs == 0) {
		dev_warn(&pdev->dev,
			 "%s: Max WSA devices is 0 for this target?\n",
			 __func__);
		return 0;
	}

	/* Get count of WSA device phandles for this platform */
	wsa_dev_cnt = of_count_phandle_with_args(pdev->dev.of_node,
						 "qcom,wsa-devs", NULL);
	if (wsa_dev_cnt == -ENOENT) {
		dev_dbg(&pdev->dev, "%s: No wsa device defined in DT.\n",
			 __func__);
		return 0;
	} else if (wsa_dev_cnt <= 0) {
		dev_err(&pdev->dev,
			"%s: Error reading wsa device from DT. wsa_dev_cnt = %d\n",
			__func__, wsa_dev_cnt);
		return -EINVAL;
	}

	/*
	 * Expect total phandles count to be NOT less than maximum possible
	 * WSA count. However, if it is less, then assign same value to
	 * max count as well.
	 */
	if (wsa_dev_cnt < wsa_max_devs) {
		dev_dbg(&pdev->dev,
			"%s: wsa_max_devs = %d cannot exceed wsa_dev_cnt = %d\n",
			__func__, wsa_max_devs, wsa_dev_cnt);
		wsa_max_devs = wsa_dev_cnt;
	}

	/* Make sure prefix string passed for each WSA device */
	ret = of_property_count_strings(pdev->dev.of_node,
					"qcom,wsa-aux-dev-prefix");
	if (ret != wsa_dev_cnt) {
		dev_err(&pdev->dev,
			"%s: expecting %d wsa prefix. Defined only %d in DT\n",
			__func__, wsa_dev_cnt, ret);
		return -EINVAL;
	}

	/*
	 * Alloc mem to store phandle and index info of WSA device, if already
	 * registered with ALSA core
	 */
	wsa881x_dev_info = devm_kcalloc(&pdev->dev, wsa_max_devs,
					sizeof(struct mdm_wsa881x_dev_info),
					GFP_KERNEL);
	if (!wsa881x_dev_info)
		return -ENOMEM;

	/*
	 * search and check whether all WSA devices are already
	 * registered with ALSA core or not. If found a node, store
	 * the node and the index in a local array of struct for later
	 * use.
	 */
	for (i = 0; i < wsa_dev_cnt; i++) {
		wsa_of_node = of_parse_phandle(pdev->dev.of_node,
					    "qcom,wsa-devs", i);
		if (unlikely(!wsa_of_node)) {
			/* we should not be here */
			dev_err(&pdev->dev,
				"%s: wsa dev node is not present\n",
				__func__);
			return -EINVAL;
		}
		if (soc_find_component(wsa_of_node, NULL)) {
			/* WSA device registered with ALSA core */
			wsa881x_dev_info[found].of_node = wsa_of_node;
			wsa881x_dev_info[found].index = i;
			found++;
			if (found == wsa_max_devs)
				break;
		}
	}

	if (found < wsa_max_devs) {
		dev_dbg(&pdev->dev,
			"%s: failed to find %d components. Found only %d\n",
			__func__, wsa_max_devs, found);
		return -EPROBE_DEFER;
	}
	dev_info(&pdev->dev,
		"%s: found %d wsa881x devices registered with ALSA core\n",
		__func__, found);

	card->num_aux_devs = wsa_max_devs;
	card->num_configs = wsa_max_devs;

	/* Alloc array of AUX devs struct */
	mdm_aux_dev = devm_kcalloc(&pdev->dev, card->num_aux_devs,
				       sizeof(struct snd_soc_aux_dev),
				       GFP_KERNEL);
	if (!mdm_aux_dev)
		return -ENOMEM;

	/* Alloc array of codec conf struct */
	mdm_codec_conf = devm_kcalloc(&pdev->dev, card->num_aux_devs,
					  sizeof(struct snd_soc_codec_conf),
					  GFP_KERNEL);
	if (!mdm_codec_conf)
		return -ENOMEM;

	for (i = 0; i < card->num_aux_devs; i++) {
		dev_name_str = devm_kzalloc(&pdev->dev, DEV_NAME_STR_LEN,
					    GFP_KERNEL);
		if (!dev_name_str)
			return -ENOMEM;

		ret = of_property_read_string_index(pdev->dev.of_node,
						    "qcom,wsa-aux-dev-prefix",
						    wsa881x_dev_info[i].index,
						    wsa_auxdev_name_prefix);
		if (ret) {
			dev_err(&pdev->dev,
				"%s: failed to read wsa aux dev prefix, ret = %d\n",
				__func__, ret);
			return -EINVAL;
		}

		snprintf(dev_name_str, strlen("wsa881x.%d"), "wsa881x.%d", i);
		mdm_aux_dev[i].name = dev_name_str;
		mdm_aux_dev[i].codec_name = NULL;
		mdm_aux_dev[i].codec_of_node =
					wsa881x_dev_info[i].of_node;
		mdm_aux_dev[i].init = mdm_wsa881x_init;
		mdm_codec_conf[i].dev_name = NULL;
		mdm_codec_conf[i].name_prefix = wsa_auxdev_name_prefix[0];
		mdm_codec_conf[i].of_node =
					wsa881x_dev_info[i].of_node;
	}
	card->codec_conf = mdm_codec_conf;
	card->aux_dev = mdm_aux_dev;

	return 0;
}

static const struct of_device_id mdm_asoc_machine_of_match[]  = {
	{ .compatible = "qcom,mdm-audio-tasha",
	  .data = "tasha_codec"},
	{ .compatible = "qcom,mdm-audio-auto",
	  .data = "auto_codec"},
	{},
};

static struct snd_soc_card *populate_snd_card_dailinks(struct device *dev)
{
	struct snd_soc_card *card = NULL;
	struct snd_soc_dai_link *dailink;
	const struct of_device_id *match;
	int len_1, len_2;

	match = of_match_node(mdm_asoc_machine_of_match, dev->of_node);
	if (!match) {
		dev_err(dev, "%s: No DT match found for sound card\n",
				__func__);
		return NULL;
	}

	if (!strcmp(match->data, "tasha_codec")) {
		len_1 = ARRAY_SIZE(mdm_dai);
		len_2 = len_1 + ARRAY_SIZE(mdm_tasha_dai);
		mdm_tasha_dai_links = devm_kcalloc(dev, len_2,
			sizeof(*mdm_tasha_dai_links), GFP_KERNEL);
		if (!mdm_tasha_dai_links)
			return NULL;
		card = &snd_soc_card_mdm_tasha;
		memcpy(mdm_tasha_dai_links, mdm_dai, sizeof(mdm_dai));
		memcpy(mdm_tasha_dai_links + len_1, mdm_tasha_dai,
		       sizeof(mdm_tasha_dai));
		dailink = mdm_tasha_dai_links;
	} else if (!strcmp(match->data, "auto_codec")) {
		len_1 = ARRAY_SIZE(mdm_dai);
		len_2 = len_1 + ARRAY_SIZE(mdm_auto_dai);
		mdm_auto_dai_links = devm_kcalloc(dev, len_2,
			sizeof(*mdm_auto_dai_links), GFP_KERNEL);
		if (!mdm_auto_dai_links)
			return NULL;
		card = &snd_soc_card_mdm_auto;
		memcpy(mdm_auto_dai_links, mdm_dai, sizeof(mdm_dai));
		memcpy(mdm_auto_dai_links + len_1, mdm_auto_dai,
		       sizeof(mdm_auto_dai));
		dailink = mdm_auto_dai_links;
	}

	if (card) {
		card->dai_link = dailink;
		card->num_links = len_2;
	}

	return card;
}

static int mdm_asoc_machine_probe(struct platform_device *pdev)
{
	int ret;
	struct mdm_machine_data *pdata;
	struct snd_soc_card *card;
	const struct of_device_id *match;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev,
			"%s No platform supplied from device tree\n", __func__);

		return -EINVAL;
	}

	match = of_match_node(mdm_asoc_machine_of_match, pdev->dev.of_node);
	if (!match) {
		dev_err(&pdev->dev, "%s: No DT match found for sound card\n",
				__func__);
		return -EINVAL;
	}

	pdata = devm_kzalloc(&pdev->dev, sizeof(struct mdm_machine_data),
			     GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	if (!strcmp(match->data, "tasha_codec")) {
		ret = of_property_read_u32(pdev->dev.of_node,
					   "qcom,tasha-mclk-clk-freq",
					   &pdata->mclk_freq);
		if (ret) {
			dev_err(&pdev->dev,
				"%s Looking up %s property in node %s failed",
				__func__, "qcom,tasha-mclk-clk-freq",
				pdev->dev.of_node->full_name);

			goto err;
		}
	} else {
		pdata->mclk_freq = MDM_MCLK_CLK_12P288MHZ;
	}

	/* At present only 12.288MHz is supported on MDM. */
	if (q6afe_check_osr_clk_freq(pdata->mclk_freq)) {
		dev_err(&pdev->dev, "%s Unsupported tasha mclk freq %u\n",
			__func__, pdata->mclk_freq);

		ret = -EINVAL;
		goto err;
	}

	ret = msm_gpioset_initialize(CLIENT_WCD_EXT, &pdev->dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "Error reading dtsi file for gpios\n");
		return -EINVAL;
	}

	mutex_init(&cdc_mclk_mutex);
	atomic_set(&mi2s_ref_count, 0);
	atomic_set(&sec_mi2s_ref_count, 0);
	atomic_set(&sec_tdm_ref_count, 0);
	pdata->prim_clk_usrs = 0;

	card = populate_snd_card_dailinks(&pdev->dev);
	if (!card) {
		dev_err(&pdev->dev, "%s: Card uninitialized\n", __func__);
		ret = -EINVAL;
		goto err;
	}

	card->dev = &pdev->dev;
	platform_set_drvdata(pdev, card);
	snd_soc_card_set_drvdata(card, pdata);

	ret = snd_soc_of_parse_card_name(card, "qcom,model");
	if (ret)
		goto err;
	if (of_property_read_bool(pdev->dev.of_node, "qcom,audio-routing")) {
		ret = snd_soc_of_parse_audio_routing(card,
						     "qcom,audio-routing");
		if (ret)
			goto err;
	}
	ret = mdm_populate_dai_link_component_of_node(card);
	if (ret) {
		ret = -EPROBE_DEFER;
		goto err;
	}

	if (!strcmp(match->data, "tasha_codec")) {
		ret = mdm_init_wsa_dev(pdev, card);
		if (ret)
			goto err;
	}

	ret = snd_soc_register_card(card);
	if (ret == -EPROBE_DEFER) {
		goto err;
	} else if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed (%d)\n", ret);
		goto err;
	}

	pdata->lpaif_pri_muxsel_virt_addr = ioremap(LPAIF_PRI_MODE_MUXSEL, 4);
	if (pdata->lpaif_pri_muxsel_virt_addr == NULL) {
		pr_err("%s Pri muxsel virt addr is null\n", __func__);

		ret = -EINVAL;
		goto err;
	}
	pdata->lpass_mux_spkr_ctl_virt_addr =
				ioremap(LPASS_CSR_GP_IO_MUX_SPKR_CTL, 4);
	if (pdata->lpass_mux_spkr_ctl_virt_addr == NULL) {
		pr_err("%s lpass spkr ctl virt addr is null\n", __func__);

		ret = -EINVAL;
		goto err1;
	}

	pdata->lpaif_sec_muxsel_virt_addr = ioremap(LPAIF_SEC_MODE_MUXSEL, 4);
	if (pdata->lpaif_sec_muxsel_virt_addr == NULL) {
		pr_err("%s Sec muxsel virt addr is null\n", __func__);
		ret = -EINVAL;
		goto err2;
	}

	pdata->lpass_mux_mic_ctl_virt_addr =
				ioremap(LPASS_CSR_GP_IO_MUX_MIC_CTL, 4);
	if (pdata->lpass_mux_mic_ctl_virt_addr == NULL) {
		pr_err("%s lpass_mux_mic_ctl_virt_addr is null\n",
		       __func__);
		ret = -EINVAL;
		goto err3;
	}

	return 0;
err3:
	iounmap(pdata->lpaif_sec_muxsel_virt_addr);
err2:
	iounmap(pdata->lpass_mux_spkr_ctl_virt_addr);
err1:
	iounmap(pdata->lpaif_pri_muxsel_virt_addr);
err:
	devm_kfree(&pdev->dev, pdata);
	return ret;
}

static int mdm_asoc_machine_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct mdm_machine_data *pdata = snd_soc_card_get_drvdata(card);

	pdata->mclk_freq = 0;
	gpio_free(pdata->hph_en1_gpio);
	gpio_free(pdata->hph_en0_gpio);
	iounmap(pdata->lpaif_pri_muxsel_virt_addr);
	iounmap(pdata->lpass_mux_spkr_ctl_virt_addr);
	iounmap(pdata->lpaif_sec_muxsel_virt_addr);
	iounmap(pdata->lpass_mux_mic_ctl_virt_addr);
	snd_soc_unregister_card(card);

	return 0;
}

static struct platform_driver mdm_asoc_machine_driver = {
	.driver = {
		.name = DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = mdm_asoc_machine_of_match,
	},
	.probe = mdm_asoc_machine_probe,
	.remove = mdm_asoc_machine_remove,
};

static int  mdm_adsp_state_callback(struct notifier_block *nb,
					unsigned long value, void *priv)
{
	if (!dummy_device_registered && SUBSYS_AFTER_POWERUP == value) {
		platform_driver_register(&mdm_asoc_machine_dummy_driver);
		platform_device_register(&dummy_machine_device);
		dummy_device_registered = true;
	}

		return NOTIFY_OK;
}

static struct notifier_block adsp_state_notifier_block = {
	.notifier_call = mdm_adsp_state_callback,
	.priority = -INT_MAX,
};

static int __init mdm_soc_platform_init(void)
{
	adsp_state_notifier = subsys_notif_register_notifier("modem",
						&adsp_state_notifier_block);
	return 0;
}

module_init(mdm_soc_platform_init);

MODULE_DESCRIPTION("ALSA SoC msm");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:" MDM9650_MACHINE_DRV_NAME);
MODULE_DEVICE_TABLE(of, mdm9650_asoc_machine_of_match);
