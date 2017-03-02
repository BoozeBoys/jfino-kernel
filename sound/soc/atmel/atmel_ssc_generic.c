/*
 * atmel_ssc_generic - Atmel ASoC driver for generic SSC output.
 *
 * Copyright (C) 2017 Develer S.r.l.
 *
 * Author: Pietro Lorefice <pietro@develer.com>
 *
 * GPLv2 or later
 */

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <sound/soc.h>
#include <sound/pcm_params.h>

#include "atmel_ssc_dai.h"

static const struct snd_soc_dapm_widget atmel_asoc_generic_dapm_widgets[] = {
	SND_SOC_DAPM_LINE("Data out", NULL),
};

static int atmel_asoc_generic_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	unsigned int rate = params_rate(params);

	snd_soc_dai_set_sysclk(codec_dai, 0, rate, 0);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		snd_soc_dai_set_clkdiv(cpu_dai, ATMEL_SSC_TCMR_PERIOD, 23);
	else
		snd_soc_dai_set_clkdiv(cpu_dai, ATMEL_SSC_RCMR_PERIOD, 23);

	return 0;
}

static struct snd_soc_ops atmel_asoc_generic_ops = {
	.hw_params = atmel_asoc_generic_hw_params,
};

static struct snd_soc_dai_link atmel_asoc_generic_dailink = {
	.name = "generic",
	.stream_name = "Playback",
	.codec_dai_name = "generic",
	.dai_fmt = SND_SOC_DAIFMT_I2S
		| SND_SOC_DAIFMT_CBM_CFM,
	.ops = &atmel_asoc_generic_ops,
};

static struct snd_soc_card atmel_asoc_generic_card = {
	.name = "atmel_asoc_generic",
	.owner = THIS_MODULE,
	.dai_link = &atmel_asoc_generic_dailink,
	.num_links = 1,
	.dapm_widgets = atmel_asoc_generic_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(atmel_asoc_generic_dapm_widgets),
	.fully_routed = true,
};

static int atmel_asoc_generic_dt_init(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device_node *codec_np, *cpu_np;
	struct snd_soc_card *card = &atmel_asoc_generic_card;
	struct snd_soc_dai_link *dailink = &atmel_asoc_generic_dailink;
	int ret;

	if (!np) {
		dev_err(&pdev->dev, "only device tree supported\n");
		return -EINVAL;
	}

	ret = snd_soc_of_parse_card_name(card, "atmel,model");
	if (ret) {
		dev_err(&pdev->dev, "failed to parse card name\n");
		return ret;
	}

	ret = snd_soc_of_parse_audio_routing(card, "atmel,audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "failed to parse audio routing\n");
		return ret;
	}

	cpu_np = of_parse_phandle(np, "atmel,ssc-controller", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "failed to get dai and pcm info\n");
		ret = -EINVAL;
		return ret;
	}
	dailink->cpu_of_node = cpu_np;
	dailink->platform_of_node = cpu_np;
	of_node_put(cpu_np);

	codec_np = of_parse_phandle(np, "atmel,audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "failed to get codec info\n");
		ret = -EINVAL;
		return ret;
	}
	dailink->codec_of_node = codec_np;
	of_node_put(codec_np);

	return 0;
}

static int atmel_asoc_generic_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &atmel_asoc_generic_card;
	struct snd_soc_dai_link *dailink = &atmel_asoc_generic_dailink;
	int id, ret;

	card->dev = &pdev->dev;
	ret = atmel_asoc_generic_dt_init(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to init dt info\n");
		return ret;
	}

	id = of_alias_get_id((struct device_node *)dailink->cpu_of_node, "ssc");
	ret = atmel_ssc_set_audio(id);
	if (ret != 0) {
		dev_err(&pdev->dev, "failed to set SSC %d for audio\n", id);
		return ret;
	}

	ret = snd_soc_register_card(card);
	if (ret) {
		dev_err(&pdev->dev, "snd_soc_register_card failed\n");
		goto err_set_audio;
	}

	return 0;

err_set_audio:
	atmel_ssc_put_audio(id);
	return ret;
}

static int atmel_asoc_generic_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct snd_soc_dai_link *dailink = &atmel_asoc_generic_dailink;
	int id;

	id = of_alias_get_id((struct device_node *)dailink->cpu_of_node, "ssc");

	snd_soc_unregister_card(card);
	atmel_ssc_put_audio(id);

	return 0;
}

#ifdef CONFIG_OF
static const struct of_device_id atmel_asoc_generic_dt_ids[] = {
	{ .compatible = "atmel,asoc-generic", },
	{ }
};
#endif

static struct platform_driver atmel_asoc_generic_driver = {
	.driver = {
		.name = "atmel-generic-audio",
		.of_match_table = of_match_ptr(atmel_asoc_generic_dt_ids),
	},
	.probe = atmel_asoc_generic_probe,
	.remove = atmel_asoc_generic_remove,
};

module_platform_driver(atmel_asoc_generic_driver);

/* Module information */
MODULE_AUTHOR("Pietro Lorefice <pietro@develer.com>");
MODULE_DESCRIPTION("ALSA SoC machine driver for Atmel SoC and generic SSC devices");
MODULE_LICENSE("GPL");
