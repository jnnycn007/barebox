// SPDX-License-Identifier: GPL-2.0-only

#include <common.h>
#include <driver.h>
#include <mci.h>
#include <io.h>
#include <dma.h>
#include <linux/bitfield.h>

#include "sdhci.h"

#define MAX_TUNING_LOOP 40

enum sdhci_reset_reason {
	SDHCI_RESET_FOR_INIT,
	SDHCI_RESET_FOR_REQUEST_ERROR,
	SDHCI_RESET_FOR_REQUEST_ERROR_DATA_ONLY,
	SDHCI_RESET_FOR_TUNING_ABORT,
	SDHCI_RESET_FOR_CARD_REMOVED,
	SDHCI_RESET_FOR_CQE_RECOVERY,
};

static inline struct device *sdhci_dev(struct sdhci *host)
{
	return host->mci ? host->mci->hw_dev : NULL;
}

static void sdhci_reset_for_reason(struct sdhci *host, enum sdhci_reset_reason reason)
{
	if (host->quirks2 & SDHCI_QUIRK2_ISSUE_CMD_DAT_RESET_TOGETHER) {
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return;
	}

	switch (reason) {
	case SDHCI_RESET_FOR_INIT:
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		break;
	case SDHCI_RESET_FOR_REQUEST_ERROR:
	case SDHCI_RESET_FOR_TUNING_ABORT:
	case SDHCI_RESET_FOR_CARD_REMOVED:
	case SDHCI_RESET_FOR_CQE_RECOVERY:
		sdhci_reset(host, SDHCI_RESET_CMD);
		sdhci_reset(host, SDHCI_RESET_DATA);
		break;
	case SDHCI_RESET_FOR_REQUEST_ERROR_DATA_ONLY:
		sdhci_reset(host, SDHCI_RESET_DATA);
		break;
	}
}

#define sdhci_reset_for(h, r) sdhci_reset_for_reason((h), SDHCI_RESET_FOR_##r)

static int sdhci_send_command_retry(struct sdhci *host, struct mci_cmd *cmd)
{
	int timeout = 10;

	while ((sdhci_read32(host, SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT_CMD)) {
		if (!timeout--)
			return -ETIMEDOUT;

		mdelay(1);
	}

	return host->mci->ops.send_cmd(host->mci, cmd, NULL);
}

/*
 * We use sdhci_send_tuning() because mmc_send_tuning() is not a good fit. SDHCI
 * tuning command does not have a data payload (or rather the hardware does it
 * automatically) so mmc_send_tuning() will return -EIO. Also the tuning command
 * interrupt setup is different to other commands and there is no timeout
 * interrupt so special handling is needed.
 */
static int sdhci_send_tuning(struct sdhci *host, u32 opcode)
{
	struct mci_cmd cmd = {};
	int ret;

	cmd.cmdidx = opcode;
	cmd.resp_type = MMC_RSP_R1 | MMC_CMD_ADTC;

	/*
	 * In response to CMD19, the card sends 64 bytes of tuning
	 * block to the Host Controller. So we set the block size
	 * to 64 here.
	 */
	if (cmd.cmdidx == MMC_SEND_TUNING_BLOCK_HS200 &&
	    host->mci->ios.bus_width == MMC_BUS_WIDTH_8) {
		sdhci_write16(host, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 128));
	} else {
		sdhci_write16(host, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 64));
	}

	ret = sdhci_send_command_retry(host, &cmd);

	return ret;
}

static void sdhci_end_tuning(struct sdhci *host)
{
	sdhci_write32(host, SDHCI_INT_ENABLE, host->tuning_old_ier);
	sdhci_write32(host, SDHCI_SIGNAL_ENABLE, host->tuning_old_sig);
}

static void sdhci_start_tuning(struct sdhci *host)
{
	u16 ctrl;

	ctrl = sdhci_read16(host, SDHCI_HOST_CONTROL2);
	ctrl |= SDHCI_CTRL_EXEC_TUNING;
	sdhci_write16(host, SDHCI_HOST_CONTROL2, ctrl);

	mdelay(1);

	host->tuning_old_ier = sdhci_read32(host, SDHCI_INT_ENABLE);
	host->tuning_old_sig = sdhci_read32(host, SDHCI_SIGNAL_ENABLE);

	sdhci_write32(host, SDHCI_INT_ENABLE, SDHCI_INT_DATA_AVAIL);
	sdhci_write32(host, SDHCI_SIGNAL_ENABLE, SDHCI_INT_DATA_AVAIL);
}

static void sdhci_reset_tuning(struct sdhci *host)
{
	u16 ctrl;

	ctrl = sdhci_read16(host, SDHCI_HOST_CONTROL2);
	ctrl &= ~SDHCI_CTRL_TUNED_CLK;
	ctrl &= ~SDHCI_CTRL_EXEC_TUNING;
	sdhci_write16(host, SDHCI_HOST_CONTROL2, ctrl);
}

static void sdhci_abort_tuning(struct sdhci *host, u32 opcode)
{
	sdhci_reset_tuning(host);

	sdhci_reset_for(host, TUNING_ABORT);

	sdhci_end_tuning(host);

	mci_send_abort_tuning(host->mci->mci, opcode);
}

static int __sdhci_execute_tuning(struct sdhci *host, u32 opcode)
{
	int i;
	int ret;

	/*
	 * Issue opcode repeatedly till Execute Tuning is set to 0 or the number
	 * of loops reaches tuning loop count.
	 * Some controllers are known to always require 40 iterations.
	 */
	for (i = 0; i < host->tuning_loop_count; i++) {
		u16 ctrl;

		ret = sdhci_send_tuning(host, opcode);
		if (ret) {
			sdhci_abort_tuning(host, opcode);
			return -ETIMEDOUT;
		}

		/* Spec does not require a delay between tuning cycles */
		if (host->tuning_delay > 0)
			mdelay(host->tuning_delay);

		ctrl = sdhci_read16(host, SDHCI_HOST_CONTROL2);
		if (!(ctrl & SDHCI_CTRL_EXEC_TUNING)) {
			if (ctrl & SDHCI_CTRL_TUNED_CLK) {
				return 0; /* Success! */
			}
			break;
		}

	}

	dev_dbg(sdhci_dev(host), "Tuning timeout, falling back to fixed sampling clock\n");
	sdhci_reset_tuning(host);
	return -EAGAIN;
}

int sdhci_execute_tuning(struct sdhci *sdhci, u32 opcode)
{
	struct mci_host *host = sdhci->mci;
	int err = 0;
	unsigned int tuning_count = 0;

	if (sdhci->tuning_mode == SDHCI_TUNING_MODE_1)
		tuning_count = sdhci->tuning_count;

	/*
	 * The Host Controller needs tuning in case of SDR104 and DDR50
	 * mode, and for SDR50 mode when Use Tuning for SDR50 is set in
	 * the Capabilities register.
	 * If the Host Controller supports the HS200 mode then the
	 * tuning function has to be executed.
	 */
	switch (host->ios.timing) {
	/* HS400 tuning is done in HS200 mode */
	case MMC_TIMING_MMC_HS400:
		err = -EINVAL;
		goto out;

	case MMC_TIMING_MMC_HS200:
		break;

	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_UHS_DDR50:
		break;

	case MMC_TIMING_UHS_SDR50:
		fallthrough;

	default:
		goto out;
	}

	if (sdhci->platform_execute_tuning) {
		err = sdhci->platform_execute_tuning(host, opcode);
		goto out;
	}

	if (sdhci->tuning_delay < 0)
		sdhci->tuning_delay = opcode == MMC_SEND_TUNING_BLOCK;

	sdhci_start_tuning(sdhci);

	sdhci->tuning_err = __sdhci_execute_tuning(sdhci, opcode);

	sdhci_end_tuning(sdhci);
out:

	return err;
}
EXPORT_SYMBOL_GPL(sdhci_execute_tuning);

void sdhci_read_response(struct sdhci *sdhci, struct mci_cmd *cmd)
{
	if (cmd->resp_type & MMC_RSP_136) {
		u32 cmdrsp3, cmdrsp2, cmdrsp1, cmdrsp0;

		cmdrsp3 = sdhci_read32(sdhci, SDHCI_RESPONSE_3);
		cmdrsp2 = sdhci_read32(sdhci, SDHCI_RESPONSE_2);
		cmdrsp1 = sdhci_read32(sdhci, SDHCI_RESPONSE_1);
		cmdrsp0 = sdhci_read32(sdhci, SDHCI_RESPONSE_0);
		cmd->response[0] = (cmdrsp3 << 8) | (cmdrsp2 >> 24);
		cmd->response[1] = (cmdrsp2 << 8) | (cmdrsp1 >> 24);
		cmd->response[2] = (cmdrsp1 << 8) | (cmdrsp0 >> 24);
		cmd->response[3] = (cmdrsp0 << 8);
	} else {
		cmd->response[0] = sdhci_read32(sdhci, SDHCI_RESPONSE_0);
	}
}

void sdhci_set_cmd_xfer_mode(struct sdhci *host, struct mci_cmd *cmd,
			     struct mci_data *data, bool dma, u32 *command,
			     u32 *xfer)
{
	*command = 0;
	*xfer = 0;

	if (!(cmd->resp_type & MMC_RSP_PRESENT))
		*command |= SDHCI_RESP_NONE;
	else if (cmd->resp_type & MMC_RSP_136)
		*command |= SDHCI_RESP_TYPE_136;
	else if (cmd->resp_type & MMC_RSP_BUSY)
		*command |= SDHCI_RESP_TYPE_48_BUSY;
	else
		*command |= SDHCI_RESP_TYPE_48;

	if (cmd->resp_type & MMC_RSP_CRC)
		*command |= SDHCI_CMD_CRC_CHECK_EN;
	if (cmd->resp_type & MMC_RSP_OPCODE)
		*command |= SDHCI_CMD_INDEX_CHECK_EN;

	*command |= SDHCI_CMD_INDEX(cmd->cmdidx);

	if (cmd->cmdidx == MMC_SEND_TUNING_BLOCK ||
	    cmd->cmdidx == MMC_SEND_TUNING_BLOCK_HS200) {
		*command |= SDHCI_DATA_PRESENT;
		*xfer = SDHCI_DATA_TO_HOST;

	} else if (data) {
		*command |= SDHCI_DATA_PRESENT;

		*xfer |= SDHCI_BLOCK_COUNT_EN;

		if (data->blocks > 1)
			*xfer |= SDHCI_MULTIPLE_BLOCKS;

		if (data->flags & MMC_DATA_READ)
			*xfer |= SDHCI_DATA_TO_HOST;

		if (dma && !mmc_op_tuning(cmd->cmdidx))
			*xfer |= SDHCI_DMA_EN;
	} else if (mmc_op_tuning(cmd->cmdidx)) {
		*command |= SDHCI_DATA_PRESENT;
	}
}

static void sdhci_rx_pio(struct sdhci *sdhci, struct mci_data *data,
			 unsigned int block)
{
	u32 *buf = (u32 *)data->dest;
	int i;

	buf += block * data->blocksize / sizeof(u32);

	for (i = 0; i < data->blocksize / sizeof(u32); i++)
		buf[i] = sdhci_read32(sdhci, SDHCI_BUFFER);
}

static void sdhci_tx_pio(struct sdhci *sdhci, struct mci_data *data,
			 unsigned int block)
{
	const u32 *buf = (const u32 *)data->src;
	int i;

	buf += block * data->blocksize / sizeof(u32);

	for (i = 0; i < data->blocksize / sizeof(u32); i++)
		sdhci_write32(sdhci, SDHCI_BUFFER, buf[i]);
}

void sdhci_set_bus_width(struct sdhci *host, int width)
{
	u8 ctrl;

	BUG_ON(!host->mci); /* Call sdhci_setup_host() before using this */

	ctrl = sdhci_read8(host, SDHCI_HOST_CONTROL);
	if (width == MMC_BUS_WIDTH_8) {
		ctrl &= ~SDHCI_CTRL_4BITBUS;
		ctrl |= SDHCI_CTRL_8BITBUS;
	} else {
		if (host->mci->host_caps & MMC_CAP_8_BIT_DATA)
			ctrl &= ~SDHCI_CTRL_8BITBUS;
		if (width == MMC_BUS_WIDTH_4)
			ctrl |= SDHCI_CTRL_4BITBUS;
		else
			ctrl &= ~SDHCI_CTRL_4BITBUS;
	}
	sdhci_write8(host, SDHCI_HOST_CONTROL, ctrl);
}

static void sdhci_set_uhs_signaling(struct sdhci *host, unsigned timing)
{
	u16 ctrl_2;

	ctrl_2 = sdhci_read16(host, SDHCI_HOST_CONTROL2);
	/* Select Bus Speed Mode for host */
	ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;
	if ((timing == MMC_TIMING_MMC_HS200) ||
	    (timing == MMC_TIMING_UHS_SDR104))
		ctrl_2 |= SDHCI_CTRL_UHS_SDR104;
	else if (timing == MMC_TIMING_UHS_SDR12)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
	else if (timing == MMC_TIMING_UHS_SDR25)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
	else if (timing == MMC_TIMING_UHS_SDR50)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR50;
	else if ((timing == MMC_TIMING_UHS_DDR50) ||
		 (timing == MMC_TIMING_MMC_DDR52))
		ctrl_2 |= SDHCI_CTRL_UHS_DDR50;
	else if (timing == MMC_TIMING_MMC_HS400)
		ctrl_2 |= SDHCI_CTRL_HS400; /* Non-standard */
	sdhci_write16(host, SDHCI_HOST_CONTROL2, ctrl_2);
}
EXPORT_SYMBOL_GPL(sdhci_set_uhs_signaling);

static inline bool sdhci_can_64bit_dma(struct sdhci *host)
{
	/*
	 * According to SD Host Controller spec v4.10, bit[27] added from
	 * version 4.10 in Capabilities Register is used as 64-bit System
	 * Address support for V4 mode.
	 */
	if (host->version >= SDHCI_SPEC_410 && host->v4_mode)
		return host->caps & SDHCI_CAN_64BIT_V4;

	return host->caps & SDHCI_CAN_64BIT;
}


static void sdhci_set_adma_addr(struct sdhci *host, dma_addr_t addr)
{
	sdhci_write32(host, SDHCI_ADMA_ADDRESS, lower_32_bits(addr));
	if (host->flags & SDHCI_USE_64_BIT_DMA)
		sdhci_write32(host, SDHCI_ADMA_ADDRESS_HI, upper_32_bits(addr));
}

static void sdhci_set_sdma_addr(struct sdhci *host, dma_addr_t addr)
{
	if (host->v4_mode)
		sdhci_set_adma_addr(host, addr);
	else
		sdhci_write32(host, SDHCI_DMA_ADDRESS, addr);
}

#ifdef __PBL__
/*
 * Stubs to make timeout logic below work in PBL
 */

#define get_time_ns()		0
/*
 * Use time in us as a busy counter timeout value
 */
#define is_timeout(s, t)	((s)++ > ((t) >> 10))

#endif

int sdhci_wait_for_done(struct sdhci *sdhci, u32 mask)
{
	u64 start = get_time_ns();
	u32 stat;

	do {
		stat = sdhci_read32(sdhci, SDHCI_INT_STATUS);

		if (stat & SDHCI_INT_TIMEOUT)
			return -ETIMEDOUT;

		if (stat & SDHCI_INT_ERROR) {
			dev_err(sdhci_dev(sdhci), "SDHCI_INT_ERROR: 0x%08x\n",
				stat);
			return -EPERM;
		}

		if (is_timeout(start, 1000 * MSECOND)) {
			dev_err(sdhci_dev(sdhci),
				"SDHCI timeout while waiting for done\n");
			return -ETIMEDOUT;
		}
	} while ((stat & mask) != mask);

	return 0;
}

void sdhci_setup_data_pio(struct sdhci *sdhci, struct mci_data *data)
{
	if (!data)
		return;

	sdhci_write32(sdhci, SDHCI_BLOCK_SIZE, sdhci->sdma_boundary |
		      SDHCI_TRANSFER_BLOCK_SIZE(data->blocksize) | data->blocks << 16);
}

static void sdhci_config_dma(struct sdhci *host)
{
	u8 ctrl;
	u16 ctrl2;

	if (host->version < SDHCI_SPEC_200)
		return;

	ctrl = sdhci_read8(host, SDHCI_HOST_CONTROL);
	/* Note if DMA Select is zero then SDMA is selected */
	ctrl &= ~SDHCI_CTRL_DMA_MASK;
	sdhci_write8(host, SDHCI_HOST_CONTROL, ctrl);

	if (host->flags & SDHCI_USE_64_BIT_DMA) {
		/*
		 * If v4 mode, all supported DMA can be 64-bit addressing if
		 * controller supports 64-bit system address, otherwise only
		 * ADMA can support 64-bit addressing.
		 */
		if (host->v4_mode) {
			ctrl2 = sdhci_read16(host, SDHCI_HOST_CONTROL2);
			ctrl2 |= SDHCI_CTRL_64BIT_ADDR;
			sdhci_write16(host, SDHCI_HOST_CONTROL2, ctrl2);
		}
	}
}

void sdhci_setup_data_dma(struct sdhci *sdhci, struct mci_data *data,
			  dma_addr_t *dma)
{
	struct device *dev = sdhci_dev(sdhci);
	int nbytes;

	if (!data) {
		if (dma)
			*dma = SDHCI_NO_DMA;
		return;
	}

	sdhci_setup_data_pio(sdhci, data);

	if (!dma)
		return;

	nbytes = data->blocks * data->blocksize;

	if (data->flags & MMC_DATA_READ)
		*dma = dma_map_single(dev, data->dest, nbytes,
				      DMA_FROM_DEVICE);
	else
		*dma = dma_map_single(dev, (void *)data->src, nbytes,
				      DMA_TO_DEVICE);

	if (dma_mapping_error(dev, *dma)) {
		*dma = SDHCI_NO_DMA;
		return;
	}

	sdhci_config_dma(sdhci);
	sdhci_set_sdma_addr(sdhci, *dma);
}

void sdhci_teardown_data(struct sdhci *sdhci,
			 struct mci_data *data, dma_addr_t dma)
{
	struct device *dev = sdhci_dev(sdhci);
	unsigned nbytes;

	if (IN_PBL || !data || dma_mapping_error(dev, dma))
		return;

	nbytes = data->blocks * data->blocksize;

	if (data->flags & MMC_DATA_READ)
		dma_unmap_single(dev, dma, nbytes, DMA_FROM_DEVICE);
	else
		dma_unmap_single(dev, dma, nbytes, DMA_TO_DEVICE);
}

int sdhci_transfer_data_dma(struct sdhci *sdhci, struct mci_cmd *cmd,
			    struct mci_data *data, dma_addr_t dma)
{
	struct device *dev = sdhci_dev(sdhci);
	u64 start;
	int nbytes;
	u32 irqcheck, irqstat;
	int ret;

	if (!data)
		return 0;

	nbytes = data->blocks * data->blocksize;

	start = get_time_ns();

	irqcheck = SDHCI_INT_XFER_COMPLETE;
	if (mmc_op_tuning(cmd->cmdidx))
		irqcheck = SDHCI_INT_DATA_AVAIL;

	do {
		irqstat = sdhci_read32(sdhci, SDHCI_INT_STATUS);

		if (irqstat & SDHCI_INT_DATA_END_BIT) {
			ret = -EIO;
			goto out;
		}

		if (irqstat & SDHCI_INT_DATA_CRC) {
			ret = -EBADMSG;
			goto out;
		}

		if (irqstat & SDHCI_INT_DATA_TIMEOUT) {
			ret = -ETIMEDOUT;
			goto out;
		}

		/*
		 * We currently don't do anything fancy with DMA
		 * boundaries, but as we can't disable the feature
		 * we need to at least restart the transfer.
		 *
		 * According to the spec sdhci_readl(host, SDHCI_DMA_ADDRESS)
		 * should return a valid address to continue from, but as
		 * some controllers are faulty, don't trust them.
		 */
		if (irqstat & SDHCI_INT_DMA) {
			/*
			 * DMA engine has stopped on buffer boundary. Acknowledge
			 * the interrupt and kick the DMA engine again.
			 */
			sdhci_write32(sdhci, SDHCI_INT_STATUS, SDHCI_INT_DMA);
			sdhci_set_sdma_addr(sdhci, ALIGN(dma, SDHCI_DEFAULT_BOUNDARY_SIZE));
		}

		if (irqstat & irqcheck)
			break;

		if (is_timeout(start, 10 * SECOND)) {
			dev_alert(dev, "DMA wait timed out. Resetting, but recovery unlikely\n");
			sdhci_reset(sdhci, SDHCI_RESET_ALL);
			ret = -ETIMEDOUT;
			goto out;
		}
	} while (1);

	ret = 0;
out:
	sdhci_teardown_data(sdhci, data, dma);

	return ret;
}

int sdhci_transfer_data_pio(struct sdhci *sdhci, struct mci_cmd *cmd,
			    struct mci_data *data)
{
	unsigned int block = 0;
	u32 stat, prs, irqcheck;
	uint64_t start = get_time_ns();

	if (!data)
		return 0;

	irqcheck = SDHCI_INT_XFER_COMPLETE;
	if (mmc_op_tuning(cmd->cmdidx))
		irqcheck = SDHCI_INT_DATA_AVAIL;

	do {
		stat = sdhci_read32(sdhci, SDHCI_INT_STATUS);
		if (stat & SDHCI_INT_ERROR)
			return -EIO;

		if (block >= data->blocks)
			continue;

		prs = sdhci_read32(sdhci, SDHCI_PRESENT_STATE);

		if (prs & SDHCI_BUFFER_READ_ENABLE &&
		    data->flags & MMC_DATA_READ) {
			sdhci_rx_pio(sdhci, data, block);
			block++;
			start = get_time_ns();
		}

		if (prs & SDHCI_BUFFER_WRITE_ENABLE &&
		    !(data->flags & MMC_DATA_READ)) {
			sdhci_tx_pio(sdhci, data, block);
			block++;
			start = get_time_ns();
		}

		if (is_timeout(start, 10 * SECOND))
			return -ETIMEDOUT;

	} while (!(stat & irqcheck));

	return 0;
}

int sdhci_transfer_data(struct sdhci *sdhci, struct mci_cmd *cmd,
			struct mci_data *data, dma_addr_t dma)
{
	struct device *dev = sdhci_dev(sdhci);

	if (!data)
		return 0;

	if (dma_mapping_error(dev, dma))
		return sdhci_transfer_data_pio(sdhci, cmd, data);
	else
		return sdhci_transfer_data_dma(sdhci, cmd, data, dma);
}

int sdhci_reset(struct sdhci *sdhci, u8 mask)
{
	u8 val;

	sdhci_write8(sdhci, SDHCI_SOFTWARE_RESET, mask);

	return sdhci_read8_poll_timeout(sdhci, SDHCI_SOFTWARE_RESET,
					val, !(val & mask),
					100 * USEC_PER_MSEC);
}

static u16 sdhci_get_preset_value(struct sdhci *host)
{
	u16 preset = 0;

	BUG_ON(!host->mci); /* Call sdhci_setup_host() before using this */

	switch (host->timing) {
	case MMC_TIMING_UHS_SDR12:
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_SDR12);
		break;
	case MMC_TIMING_UHS_SDR25:
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_SDR25);
		break;
	case MMC_TIMING_UHS_SDR50:
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_SDR50);
		break;
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_MMC_HS200:
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_SDR104);
		break;
	case MMC_TIMING_UHS_DDR50:
	case MMC_TIMING_MMC_DDR52:
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_DDR50);
		break;
	case MMC_TIMING_MMC_HS400:
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_HS400);
		break;
	default:
		dev_warn(sdhci_dev(host), "Invalid UHS-I mode selected\n");
		preset = sdhci_read16(host, SDHCI_PRESET_FOR_SDR12);
		break;
	}
	return preset;
}

u16 sdhci_calc_clk(struct sdhci *host, unsigned int clock,
		   unsigned int *actual_clock, unsigned int input_clock)
{
	int div = 0; /* Initialized for compiler warning */
	int real_div = div, clk_mul = 1;
	u16 clk = 0;
	bool switch_base_clk = false;

	BUG_ON(!host->mci); /* Call sdhci_setup_host() before using this */

	if (host->version >= SDHCI_SPEC_300) {
		if (host->preset_enabled) {
			u16 pre_val;

			clk = sdhci_read16(host, SDHCI_CLOCK_CONTROL);
			pre_val = sdhci_get_preset_value(host);
			div = FIELD_GET(SDHCI_PRESET_SDCLK_FREQ_MASK, pre_val);
			if (host->clk_mul &&
				(pre_val & SDHCI_PRESET_CLKGEN_SEL)) {
				clk = SDHCI_PROG_CLOCK_MODE;
				real_div = div + 1;
				clk_mul = host->clk_mul;
			} else {
				real_div = max_t(int, 1, div << 1);
			}
			goto clock_set;
		}

		/*
		 * Check if the Host Controller supports Programmable Clock
		 * Mode.
		 */
		if (host->clk_mul) {
			for (div = 1; div <= 1024; div++) {
				if ((input_clock * host->clk_mul / div)
					<= clock)
					break;
			}
			if ((input_clock * host->clk_mul / div) <= clock) {
				/*
				 * Set Programmable Clock Mode in the Clock
				 * Control register.
				 */
				clk = SDHCI_PROG_CLOCK_MODE;
				real_div = div;
				clk_mul = host->clk_mul;
				div--;
			} else {
				/*
				 * Divisor can be too small to reach clock
				 * speed requirement. Then use the base clock.
				 */
				switch_base_clk = true;
			}
		}

		if (!host->clk_mul || switch_base_clk) {
			/* Version 3.00 divisors must be a multiple of 2. */
			if (input_clock <= clock)
				div = 1;
			else {
				for (div = 2; div < SDHCI_MAX_DIV_SPEC_300;
				     div += 2) {
					if ((input_clock / div) <= clock)
						break;
				}
			}
			real_div = div;
			div >>= 1;
			if ((host->quirks2 & SDHCI_QUIRK2_CLOCK_DIV_ZERO_BROKEN)
				&& !div && input_clock <= 25000000)
				div = 1;
		}
	} else {
		/* Version 2.00 divisors must be a power of 2. */
		for (div = 1; div < SDHCI_MAX_DIV_SPEC_200; div *= 2) {
			if ((input_clock / div) <= clock)
				break;
		}
		real_div = div;
		div >>= 1;
	}

clock_set:
	if (real_div)
		*actual_clock = (input_clock * clk_mul) / real_div;
	clk |= (div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT;
	clk |= ((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN)
		<< SDHCI_DIVIDER_HI_SHIFT;

	return clk;
}

void sdhci_enable_clk(struct sdhci *host, u16 clk)
{
	u64 start;

	BUG_ON(!host->mci); /* Call sdhci_setup_host() before using this */

	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_write16(host, SDHCI_CLOCK_CONTROL, clk);

	start = get_time_ns();
	while (!(sdhci_read16(host, SDHCI_CLOCK_CONTROL) &
		SDHCI_CLOCK_INT_STABLE)) {
		if (is_timeout(start, 150 * MSECOND)) {
			dev_err(sdhci_dev(host),
					"SDHCI clock stable timeout\n");
			return;
		}
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_write16(host, SDHCI_CLOCK_CONTROL, clk);
}

int sdhci_wait_idle(struct sdhci *host, struct mci_cmd *cmd, struct mci_data *data)
{
	u32 mask;
	ktime_t timeout_ns;
	int ret;

	mask = SDHCI_CMD_INHIBIT_CMD;

	if (data || (cmd && (cmd->resp_type & MMC_RSP_BUSY)))
		mask |= SDHCI_CMD_INHIBIT_DATA;

	if (cmd && (cmd->cmdidx == MMC_CMD_STOP_TRANSMISSION ||
		    mmc_op_tuning(cmd->cmdidx)))
		mask &= ~SDHCI_CMD_INHIBIT_DATA;

	timeout_ns = sdhci_compute_timeout(cmd, data);

	ret = wait_on_timeout(timeout_ns,
			!(sdhci_read32(host, SDHCI_PRESENT_STATE) & mask));
	if (ret) {
		dev_err(sdhci_dev(host),
				"SDHCI timeout while waiting for idle\n");
		return -EBUSY;
	}

	return 0;
}

int sdhci_wait_idle_data(struct sdhci *host, struct mci_cmd *cmd)
{
	u32 mask;
	ktime_t timeout_ns;
	int ret;

	mask = SDHCI_CMD_INHIBIT_CMD | SDHCI_CMD_INHIBIT_DATA;
	timeout_ns = SDHCI_CMD_DEFAULT_BUSY_TIMEOUT_NS;

	if (cmd && (cmd->cmdidx == MMC_CMD_STOP_TRANSMISSION ||
		    mmc_op_tuning(cmd->cmdidx)))
		mask &= ~SDHCI_CMD_INHIBIT_DATA;

	if (cmd && cmd->busy_timeout != 0)
		timeout_ns = ms_to_ktime(cmd->busy_timeout);

	ret = wait_on_timeout(timeout_ns,
			!(sdhci_read32(host, SDHCI_PRESENT_STATE) & mask));

	if (ret) {
		dev_err(sdhci_dev(host),
				"SDHCI timeout while waiting for idle\n");
		return -EBUSY;
	}

	return 0;
}

void sdhci_set_clock(struct sdhci *host, unsigned int clock, unsigned int input_clock)
{
	u16 clk;

	BUG_ON(!host->mci); /* Call sdhci_setup_host() before using this */

	host->mci->ios.clock = 0;

	sdhci_set_uhs_signaling(host, host->mci->ios.timing);

	sdhci_wait_idle_data(host, NULL);

	sdhci_write16(host, SDHCI_CLOCK_CONTROL, 0);

	if (clock == 0)
		return;

	clk = sdhci_calc_clk(host, clock, &host->mci->ios.clock, input_clock);
	sdhci_enable_clk(host, clk);
}

void sdhci_set_drv_type(struct sdhci *host, unsigned drv_type)
{
	u16 ctrl_2;

	if (host->preset_enabled)
		return;

	/*
	 * We only need to set Driver Strength if the
	 * preset value enable is not set.
	 */
	ctrl_2 = sdhci_read16(host, SDHCI_HOST_CONTROL2);
	ctrl_2 &= ~SDHCI_CTRL_DRV_TYPE_MASK;
	if (drv_type == MMC_SET_DRIVER_TYPE_A)
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_A;
	else if (drv_type == MMC_SET_DRIVER_TYPE_B)
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_B;
	else if (drv_type == MMC_SET_DRIVER_TYPE_C)
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_C;
	else if (drv_type == MMC_SET_DRIVER_TYPE_D)
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_D;
	else {
		dev_warn(sdhci_dev(host), "invalid driver type, default to driver type B\n");
		ctrl_2 |= SDHCI_CTRL_DRV_TYPE_B;
	}

	sdhci_write16(host, ctrl_2, SDHCI_HOST_CONTROL2);
}

static void sdhci_do_enable_v4_mode(struct sdhci *host)
{
	u16 ctrl2;

	ctrl2 = sdhci_read16(host, SDHCI_HOST_CONTROL2);
	if (ctrl2 & SDHCI_CTRL_V4_MODE)
		return;

	ctrl2 |= SDHCI_CTRL_V4_MODE;
	sdhci_write16(host, SDHCI_HOST_CONTROL2, ctrl2);
}

void sdhci_enable_v4_mode(struct sdhci *host)
{
	host->v4_mode = true;
	sdhci_do_enable_v4_mode(host);
}

void __sdhci_read_caps(struct sdhci *host, const u16 *ver,
			const u32 *caps, const u32 *caps1)
{
	u16 v;
	u64 dt_caps_mask = 0;
	u64 dt_caps = 0;
	struct device_node *np = dev_of_node(sdhci_dev(host));

	BUG_ON(!host->mci); /* Call sdhci_setup_host() before using this */

	if (host->read_caps)
		return;

	host->read_caps = true;

	sdhci_reset(host, SDHCI_RESET_ALL);

	if (host->v4_mode)
		sdhci_do_enable_v4_mode(host);

	of_property_read_u64(np, "sdhci-caps-mask", &dt_caps_mask);
	of_property_read_u64(np, "sdhci-caps", &dt_caps);

	v = ver ? *ver : sdhci_read16(host, SDHCI_HOST_VERSION);
	host->version = (v & SDHCI_SPEC_VER_MASK) >> SDHCI_SPEC_VER_SHIFT;

	if (host->quirks & SDHCI_QUIRK_MISSING_CAPS)
		return;

	if (caps) {
		host->caps = *caps;
	} else {
		host->caps = sdhci_read32(host, SDHCI_CAPABILITIES);
		host->caps &= ~lower_32_bits(dt_caps_mask);
		host->caps |= lower_32_bits(dt_caps);
	}

	if (host->version < SDHCI_SPEC_300)
		return;

	if (caps1) {
		host->caps1 = *caps1;
	} else {
		host->caps1 = sdhci_read32(host, SDHCI_CAPABILITIES_1);
		host->caps1 &= ~upper_32_bits(dt_caps_mask);
		host->caps1 |= upper_32_bits(dt_caps);
	}
}

int sdhci_setup_host(struct sdhci *host)
{
	struct mci_host *mci = host->mci;

	BUG_ON(!mci);

	sdhci_read_caps(host);

	if (!host->max_clk) {
		if (host->version >= SDHCI_SPEC_300)
			host->max_clk = FIELD_GET(SDHCI_CLOCK_V3_BASE_MASK, host->caps);
		else
			host->max_clk = FIELD_GET(SDHCI_CLOCK_BASE_MASK, host->caps);

		host->max_clk *= 1000000;
	}

	/*
	 * In case of Host Controller v3.00, find out whether clock
	 * multiplier is supported.
	 */
	host->clk_mul = FIELD_GET(SDHCI_CLOCK_MUL_MASK, host->caps1);

	/*
	 * In case the value in Clock Multiplier is 0, then programmable
	 * clock mode is not supported, otherwise the actual clock
	 * multiplier is one more than the value of Clock Multiplier
	 * in the Capabilities Register.
	 */
	if (host->clk_mul)
		host->clk_mul += 1;

	if (host->caps & SDHCI_CAN_VDD_180)
		mci->voltages |= MMC_VDD_165_195;
	if (host->caps & SDHCI_CAN_VDD_300)
		mci->voltages |= MMC_VDD_29_30 | MMC_VDD_30_31;
	if (host->caps & SDHCI_CAN_VDD_330)
		mci->voltages |= MMC_VDD_32_33 | MMC_VDD_33_34;

	if (host->caps & SDHCI_CAN_DO_HISPD)
		mci->host_caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED;

	if (host->caps & SDHCI_CAN_DO_8BIT)
		mci->host_caps |= MMC_CAP_8_BIT_DATA;

	host->sdma_boundary = SDHCI_DEFAULT_BOUNDARY_ARG;

	if (sdhci_can_64bit_dma(host))
		host->flags |= SDHCI_USE_64_BIT_DMA;

	if (host->quirks2 & SDHCI_QUIRK2_NO_1_8_V) {
		host->caps1 &= ~(SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
				 SDHCI_SUPPORT_DDR50);
		/*
		 * The SDHCI controller in a SoC might support HS200/HS400
		 * (indicated using mmc-hs200-1_8v/mmc-hs400-1_8v dt property),
		 * but if the board is modeled such that the IO lines are not
		 * connected to 1.8v then HS200/HS400 cannot be supported.
		 * Disable HS200/HS400 if the board does not have 1.8v connected
		 * to the IO lines. (Applicable for other modes in 1.8v)
		 */
		mci->caps2 &= ~(MMC_CAP2_HSX00_1_8V | MMC_CAP2_HS400_ES);
		mci->host_caps &= ~(MMC_CAP_1_8V_DDR | MMC_CAP_UHS);
	}

	/* Any UHS-I mode in caps implies SDR12 and SDR25 support. */
	if (host->caps1 & (SDHCI_SUPPORT_SDR104 | SDHCI_SUPPORT_SDR50 |
			   SDHCI_SUPPORT_DDR50))
		mci->host_caps |= MMC_CAP_UHS_SDR12 | MMC_CAP_UHS_SDR25;

	/* SDR104 supports also implies SDR50 support */
	if (host->caps1 & SDHCI_SUPPORT_SDR104) {
		mci->host_caps |= MMC_CAP_UHS_SDR104 | MMC_CAP_UHS_SDR50;
		/* SD3.0: SDR104 is supported so (for eMMC) the caps2
		 * field can be promoted to support HS200.
		 */
		if (!(host->quirks2 & SDHCI_QUIRK2_BROKEN_HS200))
			mci->caps2 |= MMC_CAP2_HS200;
	} else if (host->caps1 & SDHCI_SUPPORT_SDR50) {
		mci->host_caps |= MMC_CAP_UHS_SDR50;
	}

	if ((mci->caps2 & (MMC_CAP2_HS200_1_8V_SDR | MMC_CAP2_HS400_1_8V)))
		host->flags |= SDHCI_SIGNALING_180;

	host->tuning_delay = -1;
	host->tuning_loop_count = MAX_TUNING_LOOP;

	/* Initial value for re-tuning timer count */
	host->tuning_count = FIELD_GET(SDHCI_RETUNING_TIMER_COUNT_MASK,
				       host->caps1);

	/*
	 * In case Re-tuning Timer is not disabled, the actual value of
	 * re-tuning timer will be 2 ^ (n - 1).
	 */
	if (host->tuning_count)
		host->tuning_count = 1 << (host->tuning_count - 1);

	/* Re-tuning mode supported by the Host Controller */
	host->tuning_mode = FIELD_GET(SDHCI_RETUNING_MODE_MASK, host->caps1);

	return 0;
}
