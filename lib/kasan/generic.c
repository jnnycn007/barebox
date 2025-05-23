// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains core generic KASAN code.
 *
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 * Author: Andrey Ryabinin <ryabinin.a.a@gmail.com>
 *
 * Some code borrowed from https://github.com/xairy/kasan-prototype by
 *        Andrey Konovalov <andreyknvl@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#define pr_fmt(fmt) "kasan: generic: " fmt

#include <common.h>
#include <asm/reloc.h>

#include "kasan.h"

unsigned long kasan_shadow_start;
unsigned long kasan_shadow_base;
unsigned long kasan_shadowed_end;

/*
 * All functions below always inlined so compiler could
 * perform better optimizations in each of __asan_loadX/__assn_storeX
 * depending on memory access size X.
 */

static __always_inline bool memory_is_poisoned_1(unsigned long addr)
{
	s8 shadow_value = *(s8 *)kasan_mem_to_shadow((void *)addr);

	if (unlikely(shadow_value)) {
		s8 last_accessible_byte = addr & KASAN_SHADOW_MASK;
		return unlikely(last_accessible_byte >= shadow_value);
	}

	return false;
}

static __always_inline bool memory_is_poisoned_2_4_8(unsigned long addr,
						unsigned long size)
{
	u8 *shadow_addr = (u8 *)kasan_mem_to_shadow((void *)addr);

	/*
	 * Access crosses 8(shadow size)-byte boundary. Such access maps
	 * into 2 shadow bytes, so we need to check them both.
	 */
	if (unlikely(((addr + size - 1) & KASAN_SHADOW_MASK) < size - 1))
		return *shadow_addr || memory_is_poisoned_1(addr + size - 1);

	return memory_is_poisoned_1(addr + size - 1);
}

static __always_inline bool memory_is_poisoned_16(unsigned long addr)
{
	u16 *shadow_addr = (u16 *)kasan_mem_to_shadow((void *)addr);

	/* Unaligned 16-bytes access maps into 3 shadow bytes. */
	if (unlikely(!IS_ALIGNED(addr, KASAN_SHADOW_SCALE_SIZE)))
		return *shadow_addr || memory_is_poisoned_1(addr + 15);

	return *shadow_addr;
}

static __always_inline unsigned long bytes_is_nonzero(const u8 *start,
					size_t size)
{
	while (size) {
		if (unlikely(*start))
			return (unsigned long)start;
		start++;
		size--;
	}

	return 0;
}

static __always_inline unsigned long memory_is_nonzero(const void *start,
						const void *end)
{
	unsigned int words;
	unsigned long ret;
	unsigned int prefix = (unsigned long)start % 8;

	if (end - start <= 16)
		return bytes_is_nonzero(start, end - start);

	if (prefix) {
		prefix = 8 - prefix;
		ret = bytes_is_nonzero(start, prefix);
		if (unlikely(ret))
			return ret;
		start += prefix;
	}

	words = (end - start) / 8;
	while (words) {
		if (unlikely(*(u64 *)start))
			return bytes_is_nonzero(start, 8);
		start += 8;
		words--;
	}

	return bytes_is_nonzero(start, (end - start) % 8);
}

static __always_inline bool memory_is_poisoned_n(unsigned long addr,
						size_t size)
{
	unsigned long ret;

	ret = memory_is_nonzero(kasan_mem_to_shadow((void *)addr),
			kasan_mem_to_shadow((void *)addr + size - 1) + 1);

	if (unlikely(ret)) {
		unsigned long last_byte = addr + size - 1;
		s8 *last_shadow = (s8 *)kasan_mem_to_shadow((void *)last_byte);

		if (unlikely(ret != (unsigned long)last_shadow ||
			((long)(last_byte & KASAN_SHADOW_MASK) >= *last_shadow)))
			return true;
	}
	return false;
}

static __always_inline bool memory_is_poisoned(unsigned long addr, size_t size)
{
	if (__builtin_constant_p(size)) {
		switch (size) {
		case 1:
			return memory_is_poisoned_1(addr);
		case 2:
		case 4:
		case 8:
			return memory_is_poisoned_2_4_8(addr, size);
		case 16:
			return memory_is_poisoned_16(addr);
		default:
			BUILD_BUG();
		}
	}

	return memory_is_poisoned_n(addr, size);
}

static bool kasan_initialized;

bool kasan_enabled(void)
{
	return kasan_initialized;
}

static __always_inline bool check_memory_region_inline(unsigned long addr,
						size_t size, bool write,
						unsigned long ret_ip)
{
	/*
	 * Hardening options like -ftrivial-auto-var-init=zero can end up
	 * emitting memset calls to initialize stack variables.
	 * This can lead to this function reached before relocation.
	 *
	 * Play it safe by ensuring we are relocated before proceeding.
	 */
	if (global_variable_offset() != 0)
		return true;

	if (!kasan_initialized)
		return true;

	if (addr < kasan_shadow_start)
		return true;

	if (addr > kasan_shadowed_end)
		return true;

	if (unlikely(size == 0))
		return true;

	if (unlikely(addr + size < addr))
		return !kasan_report(addr, size, write, ret_ip);

	if (addr < kasan_shadow_base)
		return true;

	if (likely(!memory_is_poisoned(addr, size)))
		return true;

	return !kasan_report(addr, size, write, ret_ip);
}

int kasan_is_poisoned_shadow(const void *_addr, size_t size)
{
	unsigned long addr = (unsigned long)_addr;
	unsigned long ret;

	if (!kasan_initialized)
		return -1;

	if (addr < kasan_shadow_start)
		return -1;

	if (addr > kasan_shadowed_end)
		return -1;

	if (unlikely(size == 0))
		return -1;

	if (unlikely(addr + size < addr))
		return -1;

	if (addr < kasan_shadow_base)
		return -1;

	ret = memory_is_nonzero(kasan_mem_to_shadow((void *)addr),
			kasan_mem_to_shadow((void *)addr + size - 1) + 1);

	if (unlikely(ret)) {
		unsigned long last_byte = addr + size - 1;
		s8 *last_shadow = (s8 *)kasan_mem_to_shadow((void *)last_byte);

		if (unlikely(ret != (unsigned long)last_shadow ||
			((long)(last_byte & KASAN_SHADOW_MASK) >= *last_shadow)))
			return 1;
	}
	return 0;
}

void kasan_init(unsigned long membase, unsigned long memsize,
		unsigned long shadow_base)
{
	kasan_shadow_start = membase;
	kasan_shadow_base = shadow_base;
	kasan_shadowed_end = membase + memsize - 1;

	kasan_unpoison_shadow((void *)membase, memsize);
	kasan_initialized = true;

	pr_debug("initialized for 0x%08lx-0x%lx with shadow at 0x%08lx\n",
		 kasan_shadow_start, kasan_shadowed_end, kasan_shadow_base);
}

bool __no_sanitize_address check_memory_region(unsigned long addr,
					       size_t size, bool write,
					       unsigned long ret_ip)
{
	return check_memory_region_inline(addr, size, write, ret_ip);
}

static void register_global(struct kasan_global *global)
{
	size_t aligned_size = round_up(global->size, KASAN_SHADOW_SCALE_SIZE);

	kasan_unpoison_shadow(global->beg, global->size);

	kasan_poison_shadow(global->beg + aligned_size,
		global->size_with_redzone - aligned_size,
		KASAN_GLOBAL_REDZONE);
}

void __asan_register_globals(struct kasan_global *globals, size_t size)
{
	int i;

	for (i = 0; i < size; i++)
		register_global(&globals[i]);
}
EXPORT_SYMBOL(__asan_register_globals);

void __asan_unregister_globals(struct kasan_global *globals, size_t size)
{
}
EXPORT_SYMBOL(__asan_unregister_globals);

#define DEFINE_ASAN_LOAD_STORE(size)					\
	void __no_sanitize_address __asan_load##size(unsigned long addr)			\
	{								\
		check_memory_region_inline(addr, size, false, _RET_IP_);\
	}								\
	EXPORT_SYMBOL(__asan_load##size);				\
	__alias(__asan_load##size)					\
	void __no_sanitize_address __asan_load##size##_noabort(unsigned long);		\
	EXPORT_SYMBOL(__asan_load##size##_noabort);			\
	void __asan_store##size(unsigned long addr)			\
	{								\
		check_memory_region_inline(addr, size, true, _RET_IP_);	\
	}								\
	EXPORT_SYMBOL(__asan_store##size);				\
	__alias(__asan_store##size)					\
	void __asan_store##size##_noabort(unsigned long);		\
	EXPORT_SYMBOL(__asan_store##size##_noabort)

DEFINE_ASAN_LOAD_STORE(1);
DEFINE_ASAN_LOAD_STORE(2);
DEFINE_ASAN_LOAD_STORE(4);
DEFINE_ASAN_LOAD_STORE(8);
DEFINE_ASAN_LOAD_STORE(16);

void __asan_loadN(unsigned long addr, size_t size)
{
	check_memory_region(addr, size, false, _RET_IP_);
}
EXPORT_SYMBOL(__asan_loadN);

__alias(__asan_loadN)
void __asan_loadN_noabort(unsigned long, size_t);
EXPORT_SYMBOL(__asan_loadN_noabort);

void __asan_storeN(unsigned long addr, size_t size)
{
	check_memory_region(addr, size, true, _RET_IP_);
}
EXPORT_SYMBOL(__asan_storeN);

__alias(__asan_storeN)
void __asan_storeN_noabort(unsigned long, size_t);
EXPORT_SYMBOL(__asan_storeN_noabort);

/* to shut up compiler complaints */
void __asan_handle_no_return(void) {}
EXPORT_SYMBOL(__asan_handle_no_return);

/* Emitted by compiler to poison alloca()ed objects. */
void __asan_alloca_poison(unsigned long addr, size_t size)
{
	size_t rounded_up_size = round_up(size, KASAN_SHADOW_SCALE_SIZE);
	size_t padding_size = round_up(size, KASAN_ALLOCA_REDZONE_SIZE) -
			rounded_up_size;
	size_t rounded_down_size = round_down(size, KASAN_SHADOW_SCALE_SIZE);

	const void *left_redzone = (const void *)(addr -
			KASAN_ALLOCA_REDZONE_SIZE);
	const void *right_redzone = (const void *)(addr + rounded_up_size);

	WARN_ON(!IS_ALIGNED(addr, KASAN_ALLOCA_REDZONE_SIZE));

	kasan_unpoison_shadow((const void *)(addr + rounded_down_size),
			      size - rounded_down_size);
	kasan_poison_shadow(left_redzone, KASAN_ALLOCA_REDZONE_SIZE,
			KASAN_ALLOCA_LEFT);
	kasan_poison_shadow(right_redzone,
			padding_size + KASAN_ALLOCA_REDZONE_SIZE,
			KASAN_ALLOCA_RIGHT);
}
EXPORT_SYMBOL(__asan_alloca_poison);

/* Emitted by compiler to unpoison alloca()ed areas when the stack unwinds. */
void __asan_allocas_unpoison(const void *stack_top, const void *stack_bottom)
{
	if (unlikely(!stack_top || stack_top > stack_bottom))
		return;

	kasan_unpoison_shadow(stack_top, stack_bottom - stack_top);
}
EXPORT_SYMBOL(__asan_allocas_unpoison);

/* Emitted by the compiler to [un]poison local variables. */
#define DEFINE_ASAN_SET_SHADOW(byte) \
	void __asan_set_shadow_##byte(const void *addr, size_t size)	\
	{								\
		__memset((void *)addr, 0x##byte, size);			\
	}								\
	EXPORT_SYMBOL(__asan_set_shadow_##byte)

DEFINE_ASAN_SET_SHADOW(00);
DEFINE_ASAN_SET_SHADOW(f1);
DEFINE_ASAN_SET_SHADOW(f2);
DEFINE_ASAN_SET_SHADOW(f3);
DEFINE_ASAN_SET_SHADOW(f5);
DEFINE_ASAN_SET_SHADOW(f8);
