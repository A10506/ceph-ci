// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 John Spray <john.spray@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#ifndef DAEMON_STATE_H_
#define DAEMON_STATE_H_

#include <map>
#include <string>
#include <memory>
#include <set>
#include <boost/circular_buffer.hpp>

#include "common/RWLock.h"
#include "include/stringify.h"

#include "msg/msg_types.h"
#include "osd/osd_types.h"

// For PerfCounterType
#include "messages/MMgrReport.h"


// Unique reference to a daemon within a cluster
typedef std::pair<std::string, std::string> DaemonKey;

// An instance of a performance counter type, within
// a particular daemon.
class PerfCounterInstance
{
  class DataPoint
  {
    public:
    utime_t t;
    uint64_t v;
    DataPoint(utime_t t_, uint64_t v_)
      : t(t_), v(v_)
    {}
  };

  class AvgDataPoint
  {
    public:
    utime_t t;
    uint64_t s;
    uint64_t c;
    AvgDataPoint(utime_t t_, uint64_t s_, uint64_t c_)
      : t(t_), s(s_), c(c_)
    {}
  };

  boost::circular_buffer<DataPoint> buffer;
  boost::circular_buffer<AvgDataPoint> avg_buffer;

  uint64_t get_current() const;

  public:
  const boost::circular_buffer<DataPoint> & get_data() const
  {
    return buffer;
  }
  const boost::circular_buffer<AvgDataPoint> & get_data_avg() const
  {
    return avg_buffer;
  }
  void push(utime_t t, uint64_t const &v);
  void push_avg(utime_t t, uint64_t const &s, uint64_t const &c);

  PerfCounterInstance(enum perfcounter_type_d type)
  {
    if (type & PERFCOUNTER_LONGRUNAVG)
      avg_buffer = boost::circular_buffer<AvgDataPoint>(20);
    else
      buffer = boost::circular_buffer<DataPoint>(20);
  };
};


typedef std::map<std::string, PerfCounterType> PerfCounterTypes;

// Performance counters for one daemon
class DaemonPerfCounters
{
  public:
  // The record of perf stat types, shared between daemons
  PerfCounterTypes &types;

  DaemonPerfCounters(PerfCounterTypes &types_)
    : types(types_)
  {}

  std::map<std::string, PerfCounterInstance> instances;

  void update(MMgrReport *report);

  void clear()
  {
    instances.clear();
  }
};

// The state that we store about one daemon
class DaemonState
{
  public:
  Mutex lock = {"DaemonState::lock"};

  DaemonKey key;

  // The hostname where daemon was last seen running (extracted
  // from the metadata)
  std::string hostname;

  // The metadata (hostname, version, etc) sent from the daemon
  std::map<std::string, std::string> metadata;

  // TODO: this can be generalized to other daemons
  std::vector<OSDHealthMetric> osd_health_metrics;

  // Ephemeral state
  bool service_daemon = false;
  utime_t service_status_stamp;
  std::map<std::string, std::string> service_status;
  utime_t last_service_beacon;

  // The perf counters received in MMgrReport messages
  DaemonPerfCounters perf_counters;

  DaemonState(PerfCounterTypes &types_)
    : perf_counters(types_)
  {
  }
};

typedef std::shared_ptr<DaemonState> DaemonStatePtr;
typedef std::map<DaemonKey, DaemonStatePtr> DaemonStateCollection;




/**
 * Fuse the collection of per-daemon metadata from Ceph into
 * a view that can be queried by service type, ID or also
 * by server (aka fqdn).
 */
class DaemonStateIndex
{
  private:
  mutable RWLock lock = {"DaemonStateIndex", true, true, true};

  std::map<std::string, DaemonStateCollection> by_server;
  DaemonStateCollection all;
  std::set<DaemonKey> updating;

  void _erase(const DaemonKey& dmk);

  public:
  DaemonStateIndex() {}

  // FIXME: shouldn't really be public, maybe construct DaemonState
  // objects internally to avoid this.
  PerfCounterTypes types;

  void insert(DaemonStatePtr dm);
  bool exists(const DaemonKey &key) const;
  DaemonStatePtr get(const DaemonKey &key);

  // Note that these return by value rather than reference to avoid
  // callers needing to stay in lock while using result.  Callers must
  // still take the individual DaemonState::lock on each entry though.
  DaemonStateCollection get_by_server(const std::string &hostname) const;
  DaemonStateCollection get_by_service(const std::string &svc_name) const;
  DaemonStateCollection get_all() const {return all;}

  template<typename Callback, typename...Args>
  auto with_daemons_by_server(Callback&& cb, Args&&... args) const ->
    decltype(cb(by_server, std::forward<Args>(args)...)) {
    RWLock::RLocker l(lock);
    
    return std::forward<Callback>(cb)(by_server, std::forward<Args>(args)...);
  }

  void notify_updating(const DaemonKey &k) {
    RWLock::WLocker l(lock);
    updating.insert(k);
  }
  void clear_updating(const DaemonKey &k) {
    RWLock::WLocker l(lock);
    updating.erase(k);
  }
  bool is_updating(const DaemonKey &k) {
    RWLock::RLocker l(lock);
    return updating.count(k) > 0;
  }

  /**
   * Remove state for all daemons of this type whose names are
   * not present in `names_exist`.  Use this function when you have
   * a cluster map and want to ensure that anything absent in the map
   * is also absent in this class.
   */
  void cull(const std::string& svc_name,
	    const std::set<std::string>& names_exist);
};

struct cache_stat_t {
  uint64_t capacity;    // bytes
  uint64_t usage;
  uint64_t read_hits;
  uint64_t read_ops;    // read op count
  cache_stat_t(): capacity(0), usage(0), read_hits(0), read_ops(0) {};
  cache_stat_t(uint64_t capacity, uint64_t usage, uint64_t read_hits, uint64_t read_ops)
    : capacity(capacity), usage(usage), read_hits(read_hits), read_ops(read_ops) {};
};

struct pool_cache_t {
  uint64_t capacity;
  uint64_t usage;
  uint64_t read_hits;
  uint64_t read_ops;
  float    hit_rate;

  pool_cache_t(): capacity(0), usage(0), read_hits(0), read_ops(0), hit_rate(0) {};
  pool_cache_t(uint64_t cp, uint64_t usage, uint64_t read_hits, uint64_t read_ops)
    : capacity(cp), usage(usage), read_hits(read_hits), read_ops(read_ops), hit_rate(0) {};

  void calculate_hit_rate() {
    if (read_ops > 0) {
      hit_rate = read_hits * 1.0 / read_ops;
    }
  }

  void dump(Formatter *f, int64_t pool_id) {
    if (f) {
      std::string pool = stringify(pool_id);
      f->open_object_section(pool.c_str());
      f->dump_int("pool_id", pool_id);
      f->dump_unsigned("capacity", capacity);
      f->dump_unsigned("usage", usage);
      f->dump_unsigned("read_hits", read_hits);
      f->dump_unsigned("read_ops", read_ops);
      f->dump_float("hit_rate", hit_rate);
      f->close_section();
    }
  }
};

struct imageperf_t {
  string imgname;
  utime_t last_update;
  op_stat_t raw_data;
  op_stat_t pre_data;

  uint32_t rd_ops;	//io/s
  uint32_t rd_bws;	//Byte/s
  uint32_t rd_lat;	//millisecond
  uint32_t wr_ops;
  uint32_t wr_bws;
  uint32_t wr_lat;	//millisecond
  uint32_t total_ops;
  uint32_t total_bws;
  uint32_t total_lat;	//millisecond

  imageperf_t(string _imgname,
              op_stat_t _raw = op_stat_t())
    : imgname(_imgname), raw_data(), pre_data(),
      rd_ops(0), rd_bws(0), rd_lat(0),
      wr_ops(0), wr_bws(0), wr_lat(0),
      total_ops(0), total_bws(0), total_lat(0) {
      last_update = ceph_clock_now();
  }

  void update_stat(op_stat_t &rdata) {
    last_update = ceph_clock_now();
    raw_data.add(rdata);
  }
};

#endif

