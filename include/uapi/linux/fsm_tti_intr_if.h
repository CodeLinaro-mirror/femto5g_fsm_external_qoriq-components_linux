/* Copyright (c) 2019, The Linux Foundation. All rights reserved.
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
#ifndef __FSM_TTI_INTR_IF__
#define __FSM_TTI_INTR_IF__

#define FSM_TTI_MONOTONIC_CLOCK_MASK	1000000000
#define FSM_TTI_SET_SFN_SLOT_INFO_MSG	0xAA /* ioctl command */

#define FSM_TTI_MAX_SFN_NUM		1024
#define FSM_TTI_DEFAULT_MAX_SLOT_NUM	80
#define FSM_TTI_MAX_SFN_MOD_FACTOR	0x3FF /* equivalent to MOD 1024 */

struct fsm_tti_internal_stats {
	unsigned int		initial_sfn;
	unsigned int		initial_slot;
	unsigned long long	current_tti_count;
	unsigned long long	first_tti_recv_time;
	unsigned long long	current_tti_recv_time;
};

struct fsm_tti_mmap_info {
	unsigned int	sfn;		/* sfn information range 0-1023 */
	unsigned int	slot;		/* slot information range 0-79 */

	/* Timestamp Information */
	unsigned long long	intr_recv_count;
	unsigned long long	abs_recv_time;
};

#endif /* __FSM_TTI_INTR_IF__ */

