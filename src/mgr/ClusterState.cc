// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 John Spray <john.spray@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

#include "messages/MMgrDigest.h"
#include "messages/MMonMgrReport.h"
#include "messages/MPGStats.h"

#include "mgr/ClusterState.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mgr
#undef dout_prefix
#define dout_prefix *_dout << "mgr " << __func__ << " "

ClusterState::ClusterState(
  MonClient *monc_,
  Objecter *objecter_,
  const MgrMap& mgrmap)
  : monc(monc_),
    objecter(objecter_),
    lock("ClusterState"),
    mgr_map(mgrmap),
    pgservice(pg_map)
{}

void ClusterState::set_objecter(Objecter *objecter_)
{
  Mutex::Locker l(lock);

  objecter = objecter_;
}

void ClusterState::set_fsmap(FSMap const &new_fsmap)
{
  Mutex::Locker l(lock);

  fsmap = new_fsmap;
}

void ClusterState::set_mgr_map(MgrMap const &new_mgrmap)
{
  Mutex::Locker l(lock);
  mgr_map = new_mgrmap;
}

void ClusterState::set_service_map(ServiceMap const &new_service_map)
{
  Mutex::Locker l(lock);
  servicemap = new_service_map;
}

void ClusterState::load_digest(MMgrDigest *m)
{
  health_json = std::move(m->health_json);
  mon_status_json = std::move(m->mon_status_json);
}

void ClusterState::ingest_pgstats(MPGStats *stats)
{
  Mutex::Locker l(lock);

  const int from = stats->get_orig_source().num();

  pending_inc.update_stat(from, stats->epoch, std::move(stats->osd_stat));

  for (auto p : stats->pg_stat) {
    pg_t pgid = p.first;
    const auto &pg_stats = p.second;

    // In case we're hearing about a PG that according to last
    // OSDMap update should not exist
    if (existing_pools.count(pgid.pool()) == 0) {
      dout(15) << " got " << pgid
	       << " reported at " << pg_stats.reported_epoch << ":"
               << pg_stats.reported_seq
               << " state " << pg_state_string(pg_stats.state)
               << " but pool not in " << existing_pools
               << dendl;
      continue;
    }
    // In case we already heard about more recent stats from this PG
    // from another OSD
    const auto q = pg_map.pg_stat.find(pgid);
    if (q != pg_map.pg_stat.end() &&
	q->second.get_version_pair() > pg_stats.get_version_pair()) {
      dout(15) << " had " << pgid << " from "
	       << q->second.reported_epoch << ":"
               << q->second.reported_seq << dendl;
      continue;
    }

    pending_inc.pg_stat_updates[pgid] = pg_stats;

    // try to drop any pending stale state since we are hearing again
    pending_stale.erase(pgid);
  }

  for (const auto &p : stats->op_stat) {
    auto op_stat = p.second;
    pg_map.perf_pools[p.first.pool()].update_stat(op_stat);
    pg_map.perf_sum.update_stat(op_stat);
  }
}

void ClusterState::try_mark_pg_stale() {
  utime_t now = ceph_clock_now();
  double delay = g_conf->get_val<double>("mgr_mark_pg_stale_delay");
  for (auto it = pending_stale.begin();
       it != pending_stale.end(); /* no inc */) {
    auto cur = pg_map.pg_stat.find(it->first);
    if (cur == pg_map.pg_stat.end()) {
      pending_stale.erase(it++);
      continue;
    }
    if (cur->second.state & PG_STATE_STALE) {
      // already stale
      pending_stale.erase(it++);
      continue;
    }

    if (now - it->second >= delay) {
      pg_stat_t *newstat;
      auto pi = pending_inc.pg_stat_updates.find(it->first);
      if (pi != pending_inc.pg_stat_updates.end()) {
        if (pi->second.state & PG_STATE_STALE) {
          it++; // pending to mark
          continue;
        } else {
          newstat = &pi->second;
        }
      } else {
        newstat = &pending_inc.pg_stat_updates[it->first];
        *newstat = cur->second;
      }
      newstat->state |= PG_STATE_STALE;
      dout(10) << " mark pg (" << *it
	       << ") to stale at " << now << dendl;
    }
    it++;
  }
}

void ClusterState::update_delta_stats()
{
  pending_inc.stamp = ceph_clock_now();
  pending_inc.version = pg_map.version + 1; // to make apply_incremental happy
  dout(10) << " v" << pending_inc.version << dendl;

  dout(30) << " pg_map before:\n";
  JSONFormatter jf(true);
  jf.dump_object("pg_map", pg_map);
  jf.flush(*_dout);
  *_dout << dendl;
  dout(30) << " incremental:\n";
  JSONFormatter jf(true);
  jf.dump_object("pending_inc", pending_inc);
  jf.flush(*_dout);
  *_dout << dendl;

  pg_map.apply_incremental(g_ceph_context, pending_inc);
  pending_inc = PGMap::Incremental();
}

void ClusterState::sample_perf_stats()
{
  utime_t now = ceph_clock_now();
  int64_t clean_interval =
    g_ceph_context->_conf->get_val<int64_t>("mgr_image_idle_to_clean_interval");

  for (auto iti = pg_map.perf_images.begin();
       iti != pg_map.perf_images.end(); /* empty */) {
    if (now - iti->second.last_update > clean_interval) {
      iti = pg_map.perf_images.erase(iti);
      continue;
    }
    iti->second.sample_delta();
    iti++;
  }

  pg_map.perf_sum.sample_delta();
  for (auto itp = pg_map.perf_pools.begin();
       itp != pg_map.perf_pools.end(); /* empty */) {
    if (existing_pools.count(itp->first) == 0) {
      itp = pg_map.perf_pools.erase(itp);
      continue;
    }
    itp->second.sample_delta();
    itp++;
  }
}

void ClusterState::update_image_stats(const string &sid,
                                      op_stat_t &stat, const string &sname)
{
  auto it = pg_map.perf_images.find(sid);
  if (it != pg_map.perf_images.end()) {
    it->second.update_stat(stat);
  } else {
    pg_map.perf_images.emplace(sid, perf_stat_t(sname, stat));
  }
}

void ClusterState::notify_osdmap(const OSDMap &osd_map)
{
  Mutex::Locker l(lock);

  pending_inc.stamp = ceph_clock_now();
  pending_inc.version = pg_map.version + 1; // to make apply_incremental happy
  dout(10) << " v" << pending_inc.version << dendl;

  PGMapUpdater::check_osd_map(g_ceph_context, osd_map, pg_map, &pending_inc);

  // update our list of pools that exist, so that we can filter pg_map updates
  // in synchrony with this OSDMap.
  existing_pools.clear();
  for (auto& p : osd_map.get_pools()) {
    existing_pools.insert(p.first);
  }

  // brute force this for now (don't bother being clever by only
  // checking osds that went up/down)
  set<int> need_check_down_pg_osds;
  PGMapUpdater::check_down_pgs(osd_map, pg_map, true,
			       need_check_down_pg_osds, pending_stale);

  dout(30) << " pg_map before:\n";
  JSONFormatter jf(true);
  jf.dump_object("pg_map", pg_map);
  jf.flush(*_dout);
  *_dout << dendl;
  dout(30) << " incremental:\n";
  JSONFormatter jf(true);
  jf.dump_object("pending_inc", pending_inc);
  jf.flush(*_dout);
  *_dout << dendl;

  pg_map.apply_incremental(g_ceph_context, pending_inc);
  pending_inc = PGMap::Incremental();
  // TODO: Complete the separation of PG state handling so
  // that a cut-down set of functionality remains in PGMonitor
  // while the full-blown PGMap lives only here.
}

void ClusterState::dump_imgsperf(Formatter *f, set<string> &who) {
  bool in_us = g_ceph_context->_conf->get_val<bool>("mgr_op_latency_in_us");
  int64_t unit = in_us ? 1000 : 1000000;

  f->open_object_section("image perf statistics");
  for (auto it = pg_map.perf_images.begin(); it != pg_map.perf_images.end(); it++) {
    if (!who.count("all") && !who.count(it->first)) {
      continue;
    }
    double duration = std::max((double)it->second.time_deltas, 1.0);
    uint64_t latency = it->second.stat_deltas.op_num == 0 ? 0 :
      (it->second.stat_deltas.op_latency / it->second.stat_deltas.op_num + unit - 1) / unit;
    uint64_t latc_rd = it->second.stat_deltas.rd_num == 0 ? 0 :
      (it->second.stat_deltas.rd_latency / it->second.stat_deltas.rd_num + unit - 1) / unit;
    uint64_t latc_wr = it->second.stat_deltas.wr_num == 0 ? 0 :
      (it->second.stat_deltas.wr_latency / it->second.stat_deltas.wr_num + unit - 1) / unit;
    f->open_object_section(it->first.c_str());
    f->dump_string("name", it->second.name);
    f->dump_unsigned("ops", it->second.stat_deltas.op_num / duration);
    f->dump_unsigned("ops_rd", it->second.stat_deltas.rd_num / duration);
    f->dump_unsigned("ops_wr", it->second.stat_deltas.wr_num / duration);
    f->dump_unsigned("thruput", it->second.stat_deltas.op_bytes / duration);
    f->dump_unsigned("thruput_rd", it->second.stat_deltas.rd_bytes / duration);
    f->dump_unsigned("thruput_wr", it->second.stat_deltas.wr_bytes / duration);
    f->dump_unsigned("latency", latency);
    f->dump_unsigned("latency_rd", latc_rd);
    f->dump_unsigned("latency_wr", latc_wr);

    std::stringstream oss;
    oss << duration;
    f->open_object_section("raw_data");
    f->dump_format(oss.str().c_str(), "%9lu,%9lu,%9lu | %9lu,%9lu,%9lu | %9lu,%9lu,%9lu",
      it->second.stat_deltas.op_num,
      it->second.stat_deltas.rd_num,
      it->second.stat_deltas.wr_num,
      it->second.stat_deltas.op_bytes,
      it->second.stat_deltas.rd_bytes,
      it->second.stat_deltas.wr_bytes,
      it->second.stat_deltas.op_latency,
      it->second.stat_deltas.rd_latency,
      it->second.stat_deltas.wr_latency);
    for (auto &dl : it->second.delta_list) {
      std::stringstream delta_t;
      delta_t << (double)dl.second;
      f->dump_format(delta_t.str().c_str(), "%9lu,%9lu,%9lu | %9lu,%9lu,%9lu | %9lu,%9lu,%9lu",
        dl.first.op_num,
        dl.first.rd_num,
        dl.first.wr_num,
        dl.first.op_bytes,
        dl.first.rd_bytes,
        dl.first.wr_bytes,
        dl.first.op_latency,
        dl.first.rd_latency,
        dl.first.wr_latency);
    }
    f->close_section();
    f->close_section();
  }
  f->close_section();
}

void ClusterState::dump_imgsperf(ostream& ss, set<string> &who) {
  TextTable tab;
  uint32_t seqn = 0;

  tab.define_column("IMAGE_ID", TextTable::LEFT, TextTable::LEFT);
  tab.define_column("IOPS", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("IOPS_RD", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("IOPS_WR", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("|", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("THROUGHPUT", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("THRU_RD", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("THRU_WR", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("|", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("LATENCY", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("LAT_RD", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("LAT_WR", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("|", TextTable::LEFT, TextTable::RIGHT);
  tab.define_column("IMAGE_NAME", TextTable::LEFT, TextTable::LEFT);

  bool in_us = g_ceph_context->_conf->get_val<bool>("mgr_op_latency_in_us");
  int64_t unit = in_us ? 1000 : 1000000;
  for (auto it = pg_map.perf_images.begin(); it != pg_map.perf_images.end(); it++) {
    if (!who.count("all") && !who.count(it->first)) {
      continue;
    }

    double duration = std::max((double)it->second.time_deltas, 1.0);
    uint64_t latency = it->second.stat_deltas.op_num == 0 ? 0 :
      (it->second.stat_deltas.op_latency / it->second.stat_deltas.op_num + unit - 1) / unit;
    uint64_t latc_rd = it->second.stat_deltas.rd_num == 0 ? 0 :
      (it->second.stat_deltas.rd_latency / it->second.stat_deltas.rd_num + unit - 1) / unit;
    uint64_t latc_wr = it->second.stat_deltas.wr_num == 0 ? 0 :
      (it->second.stat_deltas.wr_latency / it->second.stat_deltas.wr_num + unit - 1) / unit;

    string imgname = std::to_string(++seqn) + " " + it->first;
    tab << imgname
        << (uint64_t)(it->second.stat_deltas.op_num / duration)
        << (uint64_t)(it->second.stat_deltas.rd_num / duration)
        << (uint64_t)(it->second.stat_deltas.wr_num / duration)
        << "|"
        << (uint64_t)(it->second.stat_deltas.op_bytes / duration)
        << (uint64_t)(it->second.stat_deltas.rd_bytes / duration)
        << (uint64_t)(it->second.stat_deltas.wr_bytes / duration)
        << "|"
        << latency
        << latc_rd
        << latc_wr
        << "|"
        << it->second.name
        << TextTable::endrow;
  }

  ss << tab;
}

void ClusterState::dump(Formatter *f) {
  f->open_object_section("pg pending stale");
  for (auto &ps: pending_stale) {
    std::stringstream oss;
    oss << ps.first;
    f->dump_stream(oss.str().c_str()) << ps.second;
  }
  f->close_section();

  set<string> what;
  what.insert("all");
  dump_imgsperf(f, what);
}

