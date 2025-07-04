/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2013 ARM Ltd */

/* Author: Marc Zyngier */

#ifndef __ARM_PSCI_H__
#define __ARM_PSCI_H__

#include <linux/compiler.h>

struct device_node;

#define ARM_PSCI_VER(major, minor)	(((major) << 16) | (minor))
#define ARM_PSCI_VER_1_0		ARM_PSCI_VER(1,0)
#define ARM_PSCI_VER_0_2		ARM_PSCI_VER(0,2)

/* PSCI 0.1 interface */
#define ARM_PSCI_FN_BASE		0x95c1ba5e
#define ARM_PSCI_FN(n)			(ARM_PSCI_FN_BASE + (n))

#define ARM_PSCI_FN_CPU_SUSPEND		ARM_PSCI_FN(0)
#define ARM_PSCI_FN_CPU_OFF		ARM_PSCI_FN(1)
#define ARM_PSCI_FN_CPU_ON		ARM_PSCI_FN(2)
#define ARM_PSCI_FN_MIGRATE		ARM_PSCI_FN(3)

#define ARM_PSCI_RET_SUCCESS		0
#define ARM_PSCI_RET_NOT_SUPPORTED	(-1)
#define ARM_PSCI_RET_INVAL		(-2)
#define ARM_PSCI_RET_DENIED		(-3)
#define ARM_PSCI_RET_ALREADY_ON		(-4)
#define ARM_PSCI_RET_ON_PENDING		(-5)
#define ARM_PSCI_RET_INTERNAL_FAILURE	(-6)
#define ARM_PSCI_RET_NOT_PRESENT	(-7)
#define ARM_PSCI_RET_DISABLED		(-8)
#define ARM_PSCI_RET_INVALID_ADDRESS	(-9)

/* PSCI 0.2 interface */
#define ARM_PSCI_0_2_FN_BASE			0x84000000
#define ARM_PSCI_0_2_FN(n)			(ARM_PSCI_0_2_FN_BASE + (n))

#define ARM_PSCI_0_2_FN64_BASE			0xC4000000
#define ARM_PSCI_0_2_FN64(n)			(ARM_PSCI_0_2_FN64_BASE + (n))

#define ARM_PSCI_0_2_FN_PSCI_VERSION		ARM_PSCI_0_2_FN(0)
#define ARM_PSCI_0_2_FN_CPU_SUSPEND		ARM_PSCI_0_2_FN(1)
#define ARM_PSCI_0_2_FN_CPU_OFF			ARM_PSCI_0_2_FN(2)
#define ARM_PSCI_0_2_FN_CPU_ON			ARM_PSCI_0_2_FN(3)
#define ARM_PSCI_0_2_FN_AFFINITY_INFO		ARM_PSCI_0_2_FN(4)
#define ARM_PSCI_0_2_FN_MIGRATE			ARM_PSCI_0_2_FN(5)
#define ARM_PSCI_0_2_FN_MIGRATE_INFO_TYPE	ARM_PSCI_0_2_FN(6)
#define ARM_PSCI_0_2_FN_MIGRATE_INFO_UP_CPU	ARM_PSCI_0_2_FN(7)
#define ARM_PSCI_0_2_FN_SYSTEM_OFF		ARM_PSCI_0_2_FN(8)
#define ARM_PSCI_0_2_FN_SYSTEM_RESET		ARM_PSCI_0_2_FN(9)

#define ARM_PSCI_0_2_FN64_CPU_SUSPEND		ARM_PSCI_0_2_FN64(1)
#define ARM_PSCI_0_2_FN64_CPU_ON		ARM_PSCI_0_2_FN64(3)
#define ARM_PSCI_0_2_FN64_AFFINITY_INFO		ARM_PSCI_0_2_FN64(4)
#define ARM_PSCI_0_2_FN64_MIGRATE		ARM_PSCI_0_2_FN64(5)
#define ARM_PSCI_0_2_FN64_MIGRATE_INFO_UP_CPU	ARM_PSCI_0_2_FN64(7)

/* PSCI 1.0 interface */
#define ARM_PSCI_1_0_FN_PSCI_FEATURES		ARM_PSCI_0_2_FN(10)
#define ARM_PSCI_1_0_FN_CPU_FREEZE		ARM_PSCI_0_2_FN(11)
#define ARM_PSCI_1_0_FN_CPU_DEFAULT_SUSPEND	ARM_PSCI_0_2_FN(12)
#define ARM_PSCI_1_0_FN_NODE_HW_STATE		ARM_PSCI_0_2_FN(13)
#define ARM_PSCI_1_0_FN_SYSTEM_SUSPEND		ARM_PSCI_0_2_FN(14)
#define ARM_PSCI_1_0_FN_SET_SUSPEND_MODE	ARM_PSCI_0_2_FN(15)
#define ARM_PSCI_1_0_FN_STAT_RESIDENCY		ARM_PSCI_0_2_FN(16)
#define ARM_PSCI_1_0_FN_STAT_COUNT		ARM_PSCI_0_2_FN(17)

#define ARM_PSCI_1_0_FN64_CPU_DEFAULT_SUSPEND	ARM_PSCI_0_2_FN64(12)
#define ARM_PSCI_1_0_FN64_NODE_HW_STATE		ARM_PSCI_0_2_FN64(13)
#define ARM_PSCI_1_0_FN64_SYSTEM_SUSPEND	ARM_PSCI_0_2_FN64(14)
#define ARM_PSCI_1_0_FN64_STAT_RESIDENCY	ARM_PSCI_0_2_FN64(16)
#define ARM_PSCI_1_0_FN64_STAT_COUNT		ARM_PSCI_0_2_FN64(17)

/* PSCI affinity level state returned by AFFINITY_INFO */
#define PSCI_AFFINITY_LEVEL_ON		0
#define PSCI_AFFINITY_LEVEL_OFF		1
#define PSCI_AFFINITY_LEVEL_ON_PENDING	2

struct psci_ops {
	int (*cpu_suspend)(u32 power_state, unsigned long entry, u32 context_id);
	int (*cpu_off)(void);
	int (*cpu_on)(u32 cpu_id);
	int (*affinity_info)(u32 affinity, u32 lowest_affinity_level);
	int (*migrate)(u32 cpu_id);
	int (*migrate_info_type)(void);
	int (*migrate_info_up_cpu)(void);
	void (*system_off)(void);
	void (*system_reset)(void);
};

#ifdef CONFIG_ARM_PSCI
void psci_set_ops(struct psci_ops *ops);
#else
static inline void psci_set_ops(struct psci_ops *ops)
{
}
#endif

#ifdef CONFIG_ARM_PSCI_CLIENT
int psci_invoke(ulong function, ulong arg0, ulong arg1, ulong arg2,
		ulong *result);

int psci_get_version(void);
#else
static inline int psci_invoke(ulong function, ulong arg0, ulong arg1, ulong arg2,
		ulong *result)
{
	return -ENOSYS;
}

static inline int psci_get_version(void)
{
	return -ENOSYS;
}
#endif

void psci_cpu_entry(void);

#ifdef CONFIG_ARM_PSCI_DEBUG
void psci_set_putc(void (*putcf)(void *ctx, int c), void *ctx);
void psci_putc(char c);
int psci_puts(const char *str);
int psci_printf(const char *fmt, ...) __printf(1, 2);
#else

static inline void psci_set_putc(void (*putcf)(void *ctx, int c), void *ctx)
{
}

static inline void psci_putc(char c)
{
}

static inline int psci_puts(const char *str)
{
	return 0;
}

static inline __printf(1, 2) int psci_printf(const char *fmt, ...)
{
	return 0;
}
#endif

int psci_get_cpu_id(void);

int of_psci_fixup(struct device_node *root, unsigned long psci_version,
		  const char *method);

#endif /* __ARM_PSCI_H__ */
