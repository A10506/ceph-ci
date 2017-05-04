// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
  *
 * Copyright (C) 2015 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <libgen.h>
#include <unistd.h>

#include "KernelDevice.h"
#if defined(HAVE_SPDK)
#include "NVMEDevice.h"
#endif

#include "common/debug.h"
#include "common/EventTrace.h"

#define dout_context cct
#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "bdev "

void IOContext::aio_wait()
{
  std::unique_lock<std::mutex> l(lock);
  // see aio_wake for waker.  note that even if the aios have completed
  // we wait until aio_wake is called to avoid potential for use-after-free.
  assert(priv == nullptr);
  while (num_running.load() > 0 || num_reading.load() > 0 || unwoken > 0) {
    dout(10) << __func__ << " " << this
	     << " waiting for " << num_running.load() << " aios and/or "
	     << num_reading.load() << " readers and/or "
	     << (int)unwoken << " completion thread wakers to complete" << dendl;
    cond.wait(l);
  }
  dout(20) << __func__ << " " << this << " done" << dendl;
}

BlockDevice *BlockDevice::create(CephContext* cct, const string& path,
				 aio_callback_t cb, void *cbpriv)
{
  string type = "kernel";
  char buf[PATH_MAX + 1];
  int r = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (r >= 0) {
    buf[r] = '\0';
    char *bname = ::basename(buf);
    if (strncmp(bname, SPDK_PREFIX, sizeof(SPDK_PREFIX)-1) == 0)
      type = "ust-nvme";
  }
  dout(1) << __func__ << " path " << path << " type " << type << dendl;

  if (type == "kernel") {
    return new KernelDevice(cct, cb, cbpriv);
  }
#if defined(HAVE_SPDK)
  if (type == "ust-nvme") {
    return new NVMEDevice(cct, cb, cbpriv);
  }
#endif

  derr << __func__ << " unknown backend " << type << dendl;
  ceph_abort();
  return NULL;
}

void BlockDevice::queue_reap_ioc(IOContext *ioc)
{
  std::lock_guard<std::mutex> l(ioc_reap_lock);
  if (ioc_reap_count.load() == 0)
    ++ioc_reap_count;
  ioc_reap_queue.push_back(ioc);
}

void BlockDevice::reap_ioc()
{
  if (ioc_reap_count.load()) {
    std::lock_guard<std::mutex> l(ioc_reap_lock);
    for (auto p : ioc_reap_queue) {
      dout(20) << __func__ << " reap ioc " << p << dendl;
      delete p;
    }
    ioc_reap_queue.clear();
    --ioc_reap_count;
  }
}
