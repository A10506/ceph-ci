// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2017 Greg Farnum/Red Hat <gfarnum@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

/**
 * This service abstracts out the specific implementation providing information
 * needed by parts of the Monitor based around PGStats. This'll make for
 * an easier transition from the PGMonitor-based queries where we handle
 * PGStats directly, to where we are getting information passed in from
 * the Ceph Manager.
 *
 * This initial implementation cheats by wrapping a PGMap so we don't need
 * to reimplement everything in one go.
 */

#ifndef CEPH_PGSTATSERVICE_H
#define CEPH_PGSTATSERVICE_H

struct creating_pgs_t;

class PGStatService {
public:
  PGStatService() {}
  virtual ~PGStatService() {}
  // FIXME: Kill this once we rip out PGMonitor post-luminous
  /** returns true if the underlying data is readable. Always true
   *  post-luminous, but not when we are redirecting to the yPGMonitor
   */
  virtual bool is_readable() const { return true; }
  virtual const pool_stat_t* get_pool_stat(int64_t poolid) const = 0;
  virtual const osd_stat_t& get_osd_sum() const {
    ceph_abort();
  }
  virtual const osd_stat_t *get_osd_stat(int osd) const {
    ceph_abort();
  }
  virtual const mempool::pgmap::unordered_map<int32_t,osd_stat_t>& get_osd_stat() const {
    ceph_abort();
  }
  virtual float get_full_ratio() const {
    ceph_abort();
  }
  virtual float get_nearfull_ratio() const {
    ceph_abort();
  }
  virtual bool have_creating_pgs() const {
    ceph_abort();
  }
  virtual bool is_creating_pg(pg_t pgid) const {
    ceph_abort();
  }
  virtual epoch_t get_min_last_epoch_clean() const {
    ceph_abort();
  }

  virtual bool have_full_osds() const {
    ceph_abort();
  }
  virtual bool have_nearfull_osds() const {
    ceph_abort();
  }

  virtual size_t get_num_pg_by_osd(int osd) const {
    ceph_abort();
  }
  virtual ceph_statfs get_statfs(OSDMap &osd_map,
				 boost::optional<int64_t> data_pool) const = 0;
  virtual void print_summary(Formatter *f, ostream *out) const = 0;
  virtual void dump_info(Formatter *f) const = 0;
  virtual void dump_fs_stats(stringstream *ss, Formatter *f, bool verbose) const = 0;
  virtual void dump_pool_stats(const OSDMap& osdm, stringstream *ss, Formatter *f,
			       bool verbose) const = 0;

};

#endif
