/*
 * Copyright (c) 2013-2018 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "psmx2.h"

static inline int normalize_core_id(int core_id, int num_cores)
{
	if (core_id < 0)
		core_id += num_cores;

	if (core_id < 0)
		core_id = 0;

	if (core_id >= num_cores)
		core_id = num_cores - 1;

	return core_id;
}

static int psmx2_progress_set_affinity(char *affinity)
{
	int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
	int core_id;
	cpu_set_t cpuset;
	char *triplet;
	int n, start, end, stride;
	int set_count = 0;

	if (!affinity) {
		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"progress thread affinity not set\n");
		return 0;
	}

	CPU_ZERO(&cpuset);

	for (triplet = affinity; triplet; triplet = strchr(triplet, 'c')) {
		if (triplet[0] == ',')
			triplet++;

		stride = 1;
		n = sscanf(triplet, "%d:%d:%d", &start, &end, &stride);
		if (n < 1)
			continue;

		if (n < 2)
			end = start;

		if (stride < 1)
			stride = 1;

		start = normalize_core_id(start, num_cores);
		end = normalize_core_id(end, num_cores);

		for (core_id = start; core_id <= end; core_id += stride) {
			CPU_SET(core_id, &cpuset);
			set_count++;
		}

		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"core set [%d:%d:%d] added to progress thread affinity set\n",
			start, end, stride);
	}

	if (set_count)
		pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
	else
		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"progress thread affinity not set due to invalid format\n");

	return set_count;
}

static void *psmx2_progress_func(void *args)
{
	struct psmx2_fid_domain *domain = args;
	int affinity_set;
	int sleep_usec;
	struct timespec ts;

	FI_INFO(&psmx2_prov, FI_LOG_CORE, "\n");

	affinity_set = psmx2_progress_set_affinity(psmx2_env.prog_affinity);

	/* Negative sleep time means let the system choose the default.
	 * If affinity is set, sleep a short time to get better latency.
	 * If affinity is not set, short sleep time doesn't make difference.
	 */
	sleep_usec = psmx2_env.prog_interval;
	if (sleep_usec < 0) {
		if (affinity_set)
			sleep_usec = 1;
		else
			sleep_usec = 1000;
	}

	ts.tv_sec = sleep_usec / 1000000;
	ts.tv_nsec = (sleep_usec % 1000000) * 1000;

	while (1) {
		psmx2_progress_all(domain);
		nanosleep(&ts, NULL);
	}

	return NULL;
}

static void psmx2_domain_start_progress(struct psmx2_fid_domain *domain)
{
	int err;

	err = pthread_create(&domain->progress_thread, NULL,
			     psmx2_progress_func, (void *)domain);
	if (err) {
		domain->progress_thread = pthread_self();
		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"pthread_create returns %d\n", err);
	} else {
		FI_INFO(&psmx2_prov, FI_LOG_CORE, "progress thread started\n");
	}
}

static void psmx2_domain_stop_progress(struct psmx2_fid_domain *domain)
{
	int err;
	void *exit_code;

	if (!pthread_equal(domain->progress_thread, pthread_self())) {
		err = pthread_cancel(domain->progress_thread);
		if (err) {
			FI_INFO(&psmx2_prov, FI_LOG_CORE,
				"pthread_cancel returns %d\n", err);
		}
		err = pthread_join(domain->progress_thread, &exit_code);
		if (err) {
			FI_INFO(&psmx2_prov, FI_LOG_CORE,
				"pthread_join returns %d\n", err);
		} else {
			FI_INFO(&psmx2_prov, FI_LOG_CORE,
				"progress thread exited with code %ld (%s)\n",
				(uintptr_t)exit_code,
				(exit_code == PTHREAD_CANCELED) ?
					"PTHREAD_CANCELED" : "?");
		}
	}
}

static int psmx2_domain_close(fid_t fid)
{
	struct psmx2_fid_domain *domain;

	domain = container_of(fid, struct psmx2_fid_domain,
			      util_domain.domain_fid.fid);

	FI_INFO(&psmx2_prov, FI_LOG_DOMAIN, "refcnt=%d\n",
		ofi_atomic_get32(&domain->util_domain.ref));

	if (ofi_domain_close(&domain->util_domain))
		return 0;

	if (domain->progress_thread_enabled)
		psmx2_domain_stop_progress(domain);

	fastlock_destroy(&domain->sep_lock);
	fastlock_destroy(&domain->mr_lock);
	rbtDelete(domain->mr_map);

	psmx2_lock(&domain->fabric->domain_lock, 1);
	dlist_remove(&domain->entry);
	psmx2_unlock(&domain->fabric->domain_lock, 1);
	psmx2_fabric_release(domain->fabric);

	free(domain);
	return 0;
}

static struct fi_ops psmx2_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = psmx2_domain_close,
	.bind = fi_no_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static struct fi_ops_domain psmx2_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = psmx2_av_open,
	.cq_open = psmx2_cq_open,
	.endpoint = psmx2_ep_open,
	.scalable_ep = psmx2_sep_open,
	.cntr_open = psmx2_cntr_open,
	.poll_open = fi_poll_create,
	.stx_ctx = psmx2_stx_ctx,
	.srx_ctx = fi_no_srx_context,
	.query_atomic = psmx2_query_atomic,
};

static int psmx2_key_compare(void *key1, void *key2)
{
	return (key1 < key2) ?  -1 : (key1 > key2);
}

static int psmx2_domain_init(struct psmx2_fid_domain *domain,
			     struct psmx2_ep_name *src_addr)
{
	int err;

	err = fastlock_init(&domain->mr_lock);
	if (err) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"fastlock_init(mr_lock) returns %d\n", err);
		goto err_out;
	}

	domain->mr_map = rbtNew(&psmx2_key_compare);
	if (!domain->mr_map) {
		FI_WARN(&psmx2_prov, FI_LOG_CORE,
			"rbtNew failed\n");
		goto err_out_destroy_mr_lock;
	}

	domain->mr_reserved_key = 1;
	domain->max_atomic_size = INT_MAX;

	ofi_atomic_initialize32(&domain->sep_cnt, 0);
	fastlock_init(&domain->sep_lock);
	dlist_init(&domain->sep_list);
	dlist_init(&domain->trx_ctxt_list);
	fastlock_init(&domain->trx_ctxt_lock);

	if (domain->progress_thread_enabled)
		psmx2_domain_start_progress(domain);

	return 0;

err_out_destroy_mr_lock:
	fastlock_destroy(&domain->mr_lock);

err_out:
	return err;
}

int psmx2_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		      struct fid_domain **domain, void *context)
{
	struct psmx2_fid_fabric *fabric_priv;
	struct psmx2_fid_domain *domain_priv;
	struct psmx2_ep_name *src_addr = info->src_addr;
	int mr_mode = (info->domain_attr->mr_mode & FI_MR_BASIC) ? FI_MR_BASIC : 0;
	int err;

	FI_INFO(&psmx2_prov, FI_LOG_DOMAIN, "\n");

	fabric_priv = container_of(fabric, struct psmx2_fid_fabric,
				   util_fabric.fabric_fid);

	if (!info->domain_attr->name ||
	    strcmp(info->domain_attr->name, PSMX2_DOMAIN_NAME)) {
		err = -FI_EINVAL;
		goto err_out;
	}

	domain_priv = (struct psmx2_fid_domain *) calloc(1, sizeof *domain_priv);
	if (!domain_priv) {
		err = -FI_ENOMEM;
		goto err_out;
	}

	err = ofi_domain_init(fabric, info, &domain_priv->util_domain, context);
	if (err)
		goto err_out_free_domain;

	/* fclass & context are set in ofi_domain_init */
	domain_priv->util_domain.domain_fid.fid.ops = &psmx2_fi_ops;
	domain_priv->util_domain.domain_fid.ops = &psmx2_domain_ops;
	domain_priv->util_domain.domain_fid.mr = &psmx2_mr_ops;
	domain_priv->mr_mode = mr_mode;
	domain_priv->mode = info->mode;
	domain_priv->caps = info->caps;
	domain_priv->fabric = fabric_priv;
	domain_priv->progress_thread_enabled =
		(info->domain_attr->data_progress == FI_PROGRESS_AUTO);
	domain_priv->addr_format = info->addr_format;

	if (info->addr_format == FI_ADDR_STR)
		src_addr = psmx2_string_to_ep_name(info->src_addr);

	err = psmx2_domain_init(domain_priv, src_addr);
	if (info->addr_format == FI_ADDR_STR)
		free(src_addr);
	if (err)
		goto err_out_close_domain;

	psmx2_fabric_acquire(fabric_priv);
	psmx2_lock(&fabric_priv->domain_lock, 1);
	dlist_insert_before(&domain_priv->entry, &fabric_priv->domain_list);
	psmx2_unlock(&fabric_priv->domain_lock, 1);

	psmx2_init_tag_layout(info);

	*domain = &domain_priv->util_domain.domain_fid;
	return 0;

err_out_close_domain:
	ofi_domain_close(&domain_priv->util_domain);

err_out_free_domain:
	free(domain_priv);

err_out:
	return err;
}

static int psmx2_domain_check_features(struct psmx2_fid_domain *domain,
				       uint64_t ep_caps)
{
	uint64_t domain_caps = domain->caps & ~PSMX2_SUB_CAPS;

	ep_caps &= ~PSMX2_SUB_CAPS;

	if ((domain_caps & ep_caps) != ep_caps) {
		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"caps mismatch: domain_caps=%s;\n",
			fi_tostr(&domain_caps, FI_TYPE_CAPS));

		FI_INFO(&psmx2_prov, FI_LOG_CORE,
			"caps mismatch: ep_caps=%s.\n",
			fi_tostr(&ep_caps, FI_TYPE_CAPS));

		return -FI_EOPNOTSUPP;
	}

	return 0;
}

int psmx2_domain_enable_ep(struct psmx2_fid_domain *domain,
			   struct psmx2_fid_ep *ep)
{
	int err;

	err = psmx2_domain_check_features(domain, ep->caps);
	if (err)
		return err;

	if ((ep->caps & FI_RMA) || (ep->caps & FI_ATOMICS)) {
		if (ep->tx) {
			err = psmx2_am_init(ep->tx);
			if (err)
				return err;
		}
		if (ep->rx && ep->rx != ep->tx)
			return psmx2_am_init(ep->rx);
	}

	return 0;
}

