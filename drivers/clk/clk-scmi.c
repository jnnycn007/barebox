// SPDX-License-Identifier: GPL-2.0
/*
 * System Control and Power Interface (SCMI) Protocol based clock driver
 *
 * Copyright (C) 2018-2022 ARM Ltd.
 */

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <of.h>
#include <module.h>
#include <linux/scmi_protocol.h>
#include <linux/math64.h>

static const struct scmi_clk_proto_ops *scmi_proto_clk_ops;

struct scmi_clk {
	u32 id;
	struct clk_hw hw;
	const struct scmi_clock_info *info;
	const struct scmi_protocol_handle *ph;
	struct clk_parent_data *parent_data;
};

#define to_scmi_clk(clk) container_of(clk, struct scmi_clk, hw)

static unsigned long scmi_clk_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	int ret;
	u64 rate;
	struct scmi_clk *clk = to_scmi_clk(hw);

	ret = scmi_proto_clk_ops->rate_get(clk->ph, clk->id, &rate);
	if (ret)
		return 0;
	return rate;
}

static long scmi_clk_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	u64 fmin, fmax, ftmp;
	struct scmi_clk *clk = to_scmi_clk(hw);

	/*
	 * We can't figure out what rate it will be, so just return the
	 * rate back to the caller. scmi_clk_recalc_rate() will be called
	 * after the rate is set and we'll know what rate the clock is
	 * running at then.
	 */
	if (clk->info->rate_discrete)
		return rate;

	fmin = clk->info->range.min_rate;
	fmax = clk->info->range.max_rate;
	if (rate <= fmin)
		return fmin;
	else if (rate >= fmax)
		return fmax;

	ftmp = rate - fmin;
	ftmp += clk->info->range.step_size - 1; /* to round up */
	do_div(ftmp, clk->info->range.step_size);

	return ftmp * clk->info->range.step_size + fmin;
}

static int scmi_clk_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct scmi_clk *clk = to_scmi_clk(hw);

	return scmi_proto_clk_ops->rate_set(clk->ph, clk->id, rate);
}

static int scmi_clk_set_parent(struct clk_hw *hw, u8 parent_index)
{
	struct scmi_clk *clk = to_scmi_clk(hw);

	return scmi_proto_clk_ops->parent_set(clk->ph, clk->id, parent_index);
}

static int scmi_clk_get_parent(struct clk_hw *hw)
{
	struct scmi_clk *clk = to_scmi_clk(hw);
	u32 parent_id, p_idx;
	int ret;

	ret = scmi_proto_clk_ops->parent_get(clk->ph, clk->id, &parent_id);
	if (ret)
		return 0;

	for (p_idx = 0; p_idx < clk->info->num_parents; p_idx++) {
		if (clk->parent_data[p_idx].index == parent_id)
			break;
	}

	if (p_idx == clk->info->num_parents)
		return 0;

	return p_idx;
}

static int scmi_clk_enable(struct clk_hw *hw)
{
	struct scmi_clk *clk = to_scmi_clk(hw);

	return scmi_proto_clk_ops->enable(clk->ph, clk->id);
}

static void scmi_clk_disable(struct clk_hw *hw)
{
	struct scmi_clk *clk = to_scmi_clk(hw);

	scmi_proto_clk_ops->disable(clk->ph, clk->id);
}

static int scmi_clk_atomic_enable(struct clk_hw *hw)
{
	struct scmi_clk *clk = to_scmi_clk(hw);

	return scmi_proto_clk_ops->enable_atomic(clk->ph, clk->id);
}

static void scmi_clk_atomic_disable(struct clk_hw *hw)
{
	struct scmi_clk *clk = to_scmi_clk(hw);

	scmi_proto_clk_ops->disable_atomic(clk->ph, clk->id);
}

/*
 * We can provide enable/disable atomic callbacks only if the underlying SCMI
 * transport for an SCMI instance is configured to handle SCMI commands in an
 * atomic manner.
 *
 * When no SCMI atomic transport support is available we instead provide only
 * the prepare/unprepare API, as allowed by the clock framework when atomic
 * calls are not available.
 *
 * Two distinct sets of clk_ops are provided since we could have multiple SCMI
 * instances with different underlying transport quality, so they cannot be
 * shared.
 */
static const struct clk_ops scmi_clk_ops = {
	.recalc_rate = scmi_clk_recalc_rate,
	.round_rate = scmi_clk_round_rate,
	.set_rate = scmi_clk_set_rate,
	.enable = scmi_clk_enable,
	.disable = scmi_clk_disable,
	.set_parent = scmi_clk_set_parent,
	.get_parent = scmi_clk_get_parent,
};

static const struct clk_ops scmi_atomic_clk_ops = {
	.recalc_rate = scmi_clk_recalc_rate,
	.round_rate = scmi_clk_round_rate,
	.set_rate = scmi_clk_set_rate,
	.enable = scmi_clk_atomic_enable,
	.disable = scmi_clk_atomic_disable,
	.set_parent = scmi_clk_set_parent,
	.get_parent = scmi_clk_get_parent,
};

static int scmi_clk_ops_init(struct device *dev, struct scmi_clk *sclk,
			     const struct clk_ops *scmi_ops)
{
	struct clk_init_data init = {
		.flags = CLK_GET_RATE_NOCACHE,
		.num_parents = sclk->info->num_parents,
		.ops = scmi_ops,
		.name = sclk->info->name,
	};

	if (sclk->info->num_parents > 0) {
		init.parent_hws = devm_kcalloc(dev, sclk->info->num_parents,
					       sizeof(void *), GFP_KERNEL);
		if (!init.parent_hws)
			return -ENOMEM;

		for (int i = 0; i < sclk->info->num_parents; i++) {
			init.parent_hws[i] = sclk->parent_data[i].hw;
		}
	}

	sclk->hw.init = &init;
	return clk_hw_register(dev, &sclk->hw);
}

static int scmi_clocks_probe(struct scmi_device *sdev)
{
	int idx, count, err;
	unsigned int atomic_threshold;
	bool is_atomic;
	struct clk_hw **hws;
	struct clk_hw_onecell_data *clk_data;
	struct device *dev = &sdev->dev;
	struct device_node *np = dev->of_node;
	const struct scmi_handle *handle = sdev->handle;
	struct scmi_protocol_handle *ph;
	struct scmi_clk *sclks;

	if (!handle)
		return -ENODEV;

	scmi_proto_clk_ops =
		handle->dev_protocol_get(sdev, SCMI_PROTOCOL_CLOCK, &ph);
	if (IS_ERR(scmi_proto_clk_ops))
		return PTR_ERR(scmi_proto_clk_ops);

	count = scmi_proto_clk_ops->count_get(ph);
	if (count < 0) {
		dev_err(dev, "%pOFn: invalid clock output count\n", np);
		return -EINVAL;
	}

	clk_data = devm_kzalloc(dev, struct_size(clk_data, hws, count),
				GFP_KERNEL);
	if (!clk_data)
		return -ENOMEM;

	clk_data->num = count;
	hws = clk_data->hws;

	is_atomic = handle->is_transport_atomic(handle, &atomic_threshold);

	sclks = devm_kzalloc(dev, sizeof(*sclks) * count, GFP_KERNEL);

	for (idx = 0; idx < count; idx++) {
		struct scmi_clk *sclk = &sclks[idx];

		hws[idx] = &sclk->hw;
	}

	for (idx = 0; idx < count; idx++) {
		struct scmi_clk *sclk = &sclks[idx];
		const struct clk_ops *scmi_ops;

		sclk->info = scmi_proto_clk_ops->info_get(ph, idx);
		if (!sclk->info) {
			dev_dbg(dev, "invalid clock info for idx %d\n", idx);
			continue;
		}

		sclk->id = idx;
		sclk->ph = ph;

		/*
		 * Note that when transport is atomic but SCMI protocol did not
		 * specify (or support) an enable_latency associated with a
		 * clock, we default to use atomic operations mode.
		 */
		if (is_atomic &&
		    sclk->info->enable_latency <= atomic_threshold)
			scmi_ops = &scmi_atomic_clk_ops;
		else
			scmi_ops = &scmi_clk_ops;

		/* Initialize clock parent data. */
		if (sclk->info->num_parents > 0) {
			sclk->parent_data = devm_kcalloc(dev, sclk->info->num_parents,
							 sizeof(*sclk->parent_data), GFP_KERNEL);
			if (!sclk->parent_data)
				return -ENOMEM;

			for (int i = 0; i < sclk->info->num_parents; i++) {
				sclk->parent_data[i].index = sclk->info->parents[i];
				sclk->parent_data[i].hw = hws[sclk->info->parents[i]];
			}
		}

		err = scmi_clk_ops_init(dev, sclk, scmi_ops);
		if (err) {
			dev_err(dev, "failed to register clock %d\n", idx);
			devm_kfree(dev, sclk->parent_data);
			devm_kfree(dev, sclk);
			hws[idx] = NULL;
		} else {
			dev_dbg(dev, "Registered clock:%s%s\n",
				sclk->info->name,
				scmi_ops == &scmi_atomic_clk_ops ?
				" (atomic ops)" : "");
		}
	}

	return of_clk_add_hw_provider(np, of_clk_hw_onecell_get, clk_data);
}

static const struct scmi_device_id scmi_id_table[] = {
	{ SCMI_PROTOCOL_CLOCK, "clocks" },
	{ },
};
MODULE_DEVICE_TABLE(scmi, scmi_id_table);

static struct scmi_driver scmi_clocks_driver = {
	.name = "scmi-clocks",
	.probe = scmi_clocks_probe,
	.id_table = scmi_id_table,
};
core_scmi_driver(scmi_clocks_driver);

MODULE_AUTHOR("Sudeep Holla <sudeep.holla@arm.com>");
MODULE_DESCRIPTION("ARM SCMI clock driver");
MODULE_LICENSE("GPL v2");
