/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright (c) 2014 Joyent, Inc.  All rights reserved.
 */

/*
 * ARMv6 mappings between memory nodes and physical addressese.
 *
 * XXX Fill this in when we know much more about what it looks like.
 */

#include <sys/memnode.h>
/* This is only used for panic and can be removed when we implement the file */
#include <sys/cmn_err.h>

int max_mem_nodes = 1;

struct mem_node_conf mem_node_config[MAX_MEM_NODES];

int
plat_pfn_to_mem_node(pfn_t pfn)
{
	panic("plat_pfn_to_mem_nodek");
}

void
plat_assign_lgrphand_to_mem_node(lgrp_handle_t hdl, int mnode)
{
	panic("plat_assign_lgrphand_to_mem_node");
}

lgrp_handle_t
plat_mem_node_to_lgrphand(int mnode)
{
	panic("plat_mem_node_to_lgrphand");
}

void
plat_slice_add(pfn_t start, pfn_t end)
{
	panic("plat_slice_add");
}

void
plat_slice_del(pfn_t start, pfn_t end)
{
	panic("plat_slice_del");
}