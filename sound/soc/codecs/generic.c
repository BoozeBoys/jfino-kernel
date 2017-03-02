/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/of.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

static const struct snd_soc_dapm_widget generic_dapm_widgets[] = {
	SND_SOC_DAPM_OUTPUT("DOUT"),
};

static const struct snd_soc_dapm_route generic_dapm_routes[] = {
	{"DOUT", NULL, "Data out"},
};

static struct snd_soc_dai_driver generic_dai = {
	.name = "generic",
	.playback = {
		.stream_name	= "Data out",
		.channels_min	= 1,
		.channels_max	= 1,
		.rates		= SNDRV_PCM_RATE_8000_192000,
		.formats	= SNDRV_PCM_FMTBIT_S16_LE
				| SNDRV_PCM_FMTBIT_S24_LE
				| SNDRV_PCM_FMTBIT_S32_LE,
	},
};

static struct snd_soc_codec_driver soc_generic = {
	.dapm_widgets		= generic_dapm_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(generic_dapm_widgets),
	.dapm_routes		= generic_dapm_routes,
	.num_dapm_routes	= ARRAY_SIZE(generic_dapm_routes),
};

static int generic_dev_probe(struct platform_device *pdev)
{
	return snd_soc_register_codec(&pdev->dev,
			&soc_generic, &generic_dai, 1);
}

static int generic_dev_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

MODULE_ALIAS("platform:generic-codec");

static const struct of_device_id generic_of_ids[] = {
	{ .compatible = "develed,ssc-generic" },
	{ /* sentinel */ },
};

MODULE_DEVICE_TABLE(of, generic_of_ids);

static struct platform_driver generic_driver = {
	.driver = {
		.name = "ssc-generic",
		.owner = THIS_MODULE,
		.of_match_table = generic_of_ids,
	},
	.probe = generic_dev_probe,
	.remove = generic_dev_remove,
};

module_platform_driver(generic_driver);

MODULE_DESCRIPTION("Generic SSC driver");
MODULE_AUTHOR("Pietro Lorefice <pietro@develer.com>");
MODULE_LICENSE("GPL");
