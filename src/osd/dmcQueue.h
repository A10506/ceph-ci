// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 Red Hat Inc.
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */


#pragma once

#include <ostream>

#include "boost/variant.hpp"
#include "boost/regex.hpp"

#include "common/config.h"
#include "common/ceph_context.h"
#include "osd/PGQueueable.h"

#include "common/mClockPriorityQueue.h"
#include "messages/MOSDOp.h"
#include "common/dout.h"

#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix *_dout << "dmcQueue " << __func__ << " "

namespace ceph {

  using Request = std::pair<spg_t, PGQueueable>;
  using Client = entity_inst_t;

  // This class exists to bridge the ceph code, which treats the class
  // as the client, and the queue, where the class is
  // osd_op_type_t. So this adpater class will transform calls
  // appropriately.
  class dmcQueue : public OpQueue<Request, Client> {
    CephContext *cct;
public:
    enum class osd_op_type_t {
      client_op, osd_subop, osd_subop_pullpush, bg_snaptrim, bg_recovery, bg_scrub };

    using dmcClient = std::pair<entity_inst_t, osd_op_type_t>;

    using queue_t = mClockQueue<Request, dmcClient>;

    queue_t queue;

    map<osd_op_type_t, crimson::dmclock::ClientInfo> global_dmc_qos_map;

    bool parse_qos_spec_config(string svalue,
                               double& rsv, double& wgt, double& lmt, double& bdw) {
      char unit = '\0';
      boost::regex spec_pattern("([0-9]+\\.?[0-9]*,){3}[0-9]+\\.?[0-9]*[KMGT]?$");
      if (!boost::regex_match(svalue, spec_pattern)) {
        ldout(cct, 0) << "invalid configuration: " << svalue << ", "
                      << "pattern mismatch: " << spec_pattern << dendl;
        return false;
      }

      int r = sscanf(svalue.c_str(), "%lf,%lf,%lf,%lf%c",
                     &rsv, &wgt, &lmt, &bdw, &unit);
      if (r < 4 || r > 5) {
        ldout(cct, 0) << "invalid configuration: " << svalue << ", "
                      << "sscanf return " << r << dendl;
        return false;
      }

      switch(unit) {
      case 'T': bdw *= 1024; // fall through
      case 'G': bdw *= 1024; // fall through
      case 'M': bdw *= 1024; // fall through
      case 'K': bdw *= 1024; // fall through
      default: break;
      }
      return true;
    }

    void load_global_dmc_qos_config() {
      md_config_t *conf = cct->_conf;
      update_config("osd_dmc_queue_spec_clientop", conf->osd_dmc_queue_spec_clientop, true);
      update_config("osd_dmc_queue_spec_subop", conf->osd_dmc_queue_spec_subop, true);
      update_config("osd_dmc_queue_spec_pullpush", conf->osd_dmc_queue_spec_pullpush, true);
      update_config("osd_dmc_queue_spec_snaptrim", conf->osd_dmc_queue_spec_snaptrim, true);
      update_config("osd_dmc_queue_spec_recovery", conf->osd_dmc_queue_spec_recovery, true);
      update_config("osd_dmc_queue_spec_scrub", conf->osd_dmc_queue_spec_scrub, true);
    }

    void update_config(const std::string sitem, std::string svalue, bool init) override final {
      crimson::dmclock::ClientInfo qos;
      if (!parse_qos_spec_config(svalue,
          qos.reservation, qos.weight, qos.limit, qos.bandwidth) || !qos.valid()) {
        ldout(cct, 0) << "parse config failed or invalid qos spec: ["
                      << qos.reservation << ","
                      << qos.weight << "," << qos.limit << ","
                      << qos.bandwidth << "]" << dendl;
        if (!init) {
          return;
        }
        crimson::dmclock::ClientInfo qos_def(0, 100, 0, 0);
        qos = qos_def;
      }

      ldout(cct, 20) << "set " << sitem <<" to ["
                     << qos.reservation << ","
                     << qos.weight << "," << qos.limit << ","
                     << qos.bandwidth << "]" << dendl;

      if (sitem == "osd_dmc_queue_spec_clientop") {
        global_dmc_qos_map[osd_op_type_t::client_op].assign_spec(qos);
        global_dmc_qos_map[osd_op_type_t::client_op].version++;
      } else if (sitem == "osd_dmc_queue_spec_subop") {
        global_dmc_qos_map[osd_op_type_t::osd_subop].assign_spec(qos);
        global_dmc_qos_map[osd_op_type_t::osd_subop].version++;
      } else if (sitem == "osd_dmc_queue_spec_pullpush") {
        global_dmc_qos_map[osd_op_type_t::osd_subop_pullpush].assign_spec(qos);
        global_dmc_qos_map[osd_op_type_t::osd_subop_pullpush].version++;
      } else if (sitem == "osd_dmc_queue_spec_snaptrim") {
        global_dmc_qos_map[osd_op_type_t::bg_snaptrim].assign_spec(qos);
        global_dmc_qos_map[osd_op_type_t::bg_snaptrim].version++;
      } else if (sitem == "osd_dmc_queue_spec_recovery") {
        global_dmc_qos_map[osd_op_type_t::bg_recovery].assign_spec(qos);
        global_dmc_qos_map[osd_op_type_t::bg_recovery].version++;
      } else if (sitem == "osd_dmc_queue_spec_scrub") {
        global_dmc_qos_map[osd_op_type_t::bg_scrub].assign_spec(qos);
        global_dmc_qos_map[osd_op_type_t::bg_scrub].version++;
      }
    }

    dmc_op_tracker get_dmc_op_tracker(Request &item) {
      osd_op_type_t type = get_osd_op_type(item);
      if (type == osd_op_type_t::client_op) {
        const OpRequestRef op =
          boost::get<OpRequestRef>(item.second.get_variant());
        assert(op->get_req()->get_type() == CEPH_MSG_OSD_OP);
        MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
        dmc_op_tracker opt = m->get_dmc_op_tracker();
        if (opt.valid())
          return opt;
      } else if (type == osd_op_type_t::osd_subop_pullpush) {
        const OpRequestRef op =
          boost::get<OpRequestRef>(item.second.get_variant());
        dmc_op_tracker opt = op->get_dmc_op_tracker();
        if (opt.valid())
          return opt;
      }

      return dmc_op_tracker(1, 1);
    }

    dmcClient make_dmcclient(Client client, Request item) {
      osd_op_type_t type = get_osd_op_type(item);
      if (type == osd_op_type_t::osd_subop_pullpush) {
        return std::make_pair(Client(), type);
      }
      return std::make_pair(client, type);
    }

    dmc::ClientInfo fill_in_client_info(Request item) {
      osd_op_type_t type = get_osd_op_type(item);
      if (type == osd_op_type_t::client_op) {
        /*
         * For client ops,  use client-specified QoS parameters,
         * if there is any.
         */
        const OpRequestRef op =
          boost::get<OpRequestRef>(item.second.get_variant());
        assert(op->get_req()->get_type() == CEPH_MSG_OSD_OP);
        MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
        dmc_qos_spec qos = m->get_dmc_qos_spec();
        if (qos.valid()) {
          return dmc::ClientInfo(qos.reservation, qos.weight,
                                 qos.limit, qos.bandwidth, qos.version);
        }
      }

      return global_dmc_qos_map[type];
    }

    dmc::ReqParams fill_in_req_params(dmc_op_tracker opt) {
      return dmc::ReqParams(opt.delta, opt.rho, opt.cost);
    }

    static dmc::ClientInfo client_info_func(const dmcClient& client) {
      // global client info obtainer, DO NOT USE
      assert(0 == "implement me");
      return dmc::ClientInfo();
    }

  protected:

    struct pg_queueable_visitor : public boost::static_visitor<osd_op_type_t> {
      osd_op_type_t operator()(const OpRequestRef& o) const {
        return osd_op_type_t::client_op;
      }
      osd_op_type_t operator()(const PGSnapTrim& o) const {
        return osd_op_type_t::bg_snaptrim;
      }
      osd_op_type_t operator()(const PGRecovery& o) const {
        return osd_op_type_t::bg_recovery;
      }
      osd_op_type_t operator()(const PGScrub& o) const {
        return osd_op_type_t::bg_scrub;
      }
    };

    static pg_queueable_visitor visitor;

    osd_op_type_t get_osd_op_type(const Request& request) {
      osd_op_type_t type =
        boost::apply_visitor(visitor, request.second.get_variant());
      
      if (type != osd_op_type_t::client_op)
        return type;

      const OpRequestRef op =
        boost::get<OpRequestRef>(request.second.get_variant());
      uint16_t msg_type = op->get_req()->get_header().type;
      if (msg_type == CEPH_MSG_OSD_OP) {
        return osd_op_type_t::client_op;
      } else if (msg_type == MSG_OSD_PG_PULL || msg_type == MSG_OSD_PG_PUSH) {
        return osd_op_type_t::osd_subop_pullpush;
      } else {
        return osd_op_type_t::osd_subop;
      }

      return type;
    }

    void set_request_phase(const dmc::PhaseType phase, Request &item) {
      osd_op_type_t type =
        boost::apply_visitor(visitor, item.second.get_variant());

      if (type != osd_op_type_t::client_op)
        return;

      dmc_op_tracker opt;
      switch (phase) {
      case dmc::PhaseType::reservation:
        opt.phase = DMC_OP_PHASE_RESERVATION;
        break;
      case dmc::PhaseType::priority:
        opt.phase = DMC_OP_PHASE_PRIORITY;
        break;
      default:
        assert(0 == "invalid phase type");
        break;
      }
      // set the cost and bring it back to client later.
      // just the write operation take effect, since read cost is zero here.
      opt.cost = item.second.get_cost();

      OpRequestRef op =
        boost::get<OpRequestRef>(item.second.get_variant());
      op->set_dmc_op_tracker(opt);
    }

  public:

    dmcQueue(CephContext *_cct) : cct(_cct), queue(&client_info_func, false) {
      load_global_dmc_qos_config();
    }

    inline unsigned length() const override final {
      return queue.length();
    }

    // ops of this priority should be deleted immediately
    inline void remove_by_class(Client client, std::list<Request> *out)
      override final {
      queue.remove_by_filter(
      [&client, out] (const Request& r) -> bool {
         if (client == r.second.get_owner()) {
           out->push_front(r);
           return true;
         } else {
           return false;
         }
       });
    }

    inline void enqueue_strict(Client client, unsigned priority, Request item)
      override final {
      queue.enqueue_strict(make_dmcclient(client, item), 0, item);
    }

    // enqueue op in the front of the strict queue
    inline void enqueue_strict_front(Client client, unsigned priority, Request item)
      override final {
      queue.enqueue_strict_front(make_dmcclient(client, item), priority, item);
    }

    // enqueue the op in the front of the regular queue
    inline void enqueue_front(Client client, unsigned priority, unsigned cost,
      Request item) override final {
      queue.enqueue_front(make_dmcclient(client, item), priority, cost, item);
    }

    // enqueue op in the back of the regular queue
    inline void enqueue(Client client, unsigned priority, unsigned cost,
      Request item) override final {
      // cost is ignored
      queue.enqueue_dmc(make_dmcclient(client, item), priority, 0, item,
        fill_in_client_info(item),
        fill_in_req_params(get_dmc_op_tracker(item)));
    }

    // return if the queue is empty
    inline bool empty() const override final {
      return queue.empty();
    }

    // return an op to be dispatched
    inline Request dequeue() override final {
      bool from_dmc;
      dmc::PhaseType phase;
      Request r = queue.dequeue_dmc(&from_dmc, &phase);
      if (from_dmc) // only set phase if request comes out from a dmc queue
        set_request_phase(phase, r);
      return r;
    }

    // rormatted output of the queue
    void dump(ceph::Formatter *f) const override final {
      f->open_object_section("default qos specs");
      for (auto it = global_dmc_qos_map.begin(); it != global_dmc_qos_map.end(); it++) {
        std::stringstream oss;
        if (it->first == osd_op_type_t::client_op) {
          oss << "<clientop>";
        } else if (it->first == osd_op_type_t::osd_subop) {
          oss << "<subop   >";
        } else if (it->first == osd_op_type_t::osd_subop_pullpush) {
          oss << "<pullpush>";
        } else if (it->first == osd_op_type_t::bg_snaptrim) {
          oss << "<snaptrim>";
        } else if (it->first == osd_op_type_t::bg_recovery) {
          oss << "<recovery>";
        } else if (it->first == osd_op_type_t::bg_scrub) {
          oss << "<scrub   >";
        } else {
          assert(0 == "invalid osd op type");
        }
        f->dump_stream(oss.str().c_str())
           << "[" << it->second.reservation << "/" << it->second.reservation_inv
           << "," << it->second.weight << "/" << it->second.weight_inv
           << "," << it->second.limit << "/" << it->second.limit_inv
           << "," << it->second.bandwidth << "/" << it->second.bandwidth_inv
           << "].v" << it->second.version;
      }
      f->close_section();
      queue.dump(f);
    }
  }; // class dmcQueue

  inline ostream& operator<<(ostream& out,
    const dmcQueue::dmcClient& client) {
    switch(client.second) {
    case dmcQueue::osd_op_type_t::client_op:
      out << "<clientop>";
      break;
    case dmcQueue::osd_op_type_t::osd_subop:
      out << "<subop   >";
      break;
    case dmcQueue::osd_op_type_t::osd_subop_pullpush:
      out << "<pullpush>";
      break;
    case dmcQueue::osd_op_type_t::bg_snaptrim:
      out << "<snaptrim>";
      break;
    case dmcQueue::osd_op_type_t::bg_recovery:
      out << "<recovery>";
      break;
    case dmcQueue::osd_op_type_t::bg_scrub:
      out << "<scrub   >";
      break;
    default:
      out << "<\?\?\?>";
    }
    return out << " " << client.first;
  }

} // namespace ceph

#undef dout_subsys
#undef dout_prefix
#define dout_prefix *_dout

