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

#include "common/config.h"
#include "common/ceph_context.h"
#include "osd/PGQueueable.h"

#include "common/mClockPriorityQueue.h"
#include "messages/MOSDOp.h"

namespace ceph {

  using Request = std::pair<spg_t, PGQueueable>;
  using Client = entity_inst_t;

  // This class exists to bridge the ceph code, which treats the class
  // as the client, and the queue, where the class is
  // osd_op_type. So this adpater class will transform calls
  // appropriately.
  class dmcQueue : public OpQueue<Request, Client> {

    enum class osd_op_type {
      OSD_OP_TYPE_CLIENT = 0,
      OSD_OP_TYPE_SUBOP,
      OSD_OP_TYPE_PG_SNAPTRIM,
      OSD_OP_TYPE_PG_RECOVERY,
      OSD_OP_TYPE_PG_SCRUB,
      OSD_OP_TYPE_MAX
    };

    using dmcClient = std::pair<entity_inst_t, osd_op_type>;

    using queue_t = mClockQueue<Request, dmcClient>;

    queue_t queue;

    map<osd_op_type, dmc_qos_spec> global_dmc_qos_map;

    void load_global_dmc_qos_config(CephContext *cct) {
      md_config_t *conf = cct->_conf;
      dmc_qos_spec qos;
      
      qos.reservation = conf->osd_dmc_opqueue_client_r;
      qos.weight = conf->osd_dmc_opqueue_client_w;
      qos.limit = conf->osd_dmc_opqueue_client_l;
      global_dmc_qos_map[osd_op_type::OSD_OP_TYPE_CLIENT] = qos;

      qos.reservation = conf->osd_dmc_opqueue_subop_r;
      qos.weight = conf->osd_dmc_opqueue_subop_w;
      qos.limit = conf->osd_dmc_opqueue_subop_l;
      global_dmc_qos_map[osd_op_type::OSD_OP_TYPE_SUBOP] = qos;

      qos.reservation = conf->osd_dmc_opqueue_pg_snaptrim_r;
      qos.weight = conf->osd_dmc_opqueue_pg_snaptrim_w;
      qos.limit = conf->osd_dmc_opqueue_pg_snaptrim_l;
      global_dmc_qos_map[osd_op_type::OSD_OP_TYPE_PG_SNAPTRIM] = qos;

      qos.reservation = conf->osd_dmc_opqueue_pg_recovery_r;
      qos.weight = conf->osd_dmc_opqueue_pg_recovery_w;
      qos.limit = conf->osd_dmc_opqueue_pg_recovery_l;
      global_dmc_qos_map[osd_op_type::OSD_OP_TYPE_PG_RECOVERY] = qos;

      qos.reservation = conf->osd_dmc_opqueue_pg_scrub_r;
      qos.weight = conf->osd_dmc_opqueue_pg_scrub_w;
      qos.limit = conf->osd_dmc_opqueue_pg_scrub_l;
      global_dmc_qos_map[osd_op_type::OSD_OP_TYPE_PG_SCRUB] = qos;
    }

    dmc_qos_spec get_dmc_qos_spec(Request &item) {
      osd_op_type type = get_osd_op_type(item);
      if (type == osd_op_type::OSD_OP_TYPE_CLIENT) {
        /* 
         * For client ops,  use client-specified QoS parameters, 
         * if there is any.
         */
        const OpRequestRef op =
          boost::get<OpRequestRef>(item.second.get_variant()); 
        assert(op->get_req()->get_type() == CEPH_MSG_OSD_OP);
        MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
        dmc_qos_spec qos = m->get_dmc_qos_spec();
        if (qos.valid())
          return qos;
      }

      return global_dmc_qos_map[type];
    }

    dmc_op_tracker get_dmc_op_tracker(Request &item) {
      osd_op_type type = get_osd_op_type(item);
      if (type == osd_op_type::OSD_OP_TYPE_CLIENT) {
        const OpRequestRef op =
          boost::get<OpRequestRef>(item.second.get_variant());
        assert(op->get_req()->get_type() == CEPH_MSG_OSD_OP);
        MOSDOp *m = static_cast<MOSDOp*>(op->get_req());
        dmc_op_tracker opt = m->get_dmc_op_tracker();
        if (opt.valid())
          return opt;
      }

      return dmc_op_tracker(1, 1);
    }

    dmcClient make_dmcclient(Client client, Request item) {
      return std::make_pair(client, get_osd_op_type(item));
    }

    dmc::ClientInfo fill_in_client_info(dmc_qos_spec qos) {
      return dmc::ClientInfo(qos.reservation, qos.weight,
                             qos.limit, qos.bandwidth, qos.version);
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

    struct pg_queueable_visitor : public boost::static_visitor<osd_op_type> {
      osd_op_type operator()(const OpRequestRef& o) const {
        return osd_op_type::OSD_OP_TYPE_CLIENT;
      }
      osd_op_type operator()(const PGSnapTrim& o) const {
        return osd_op_type::OSD_OP_TYPE_PG_SNAPTRIM;
      }
      osd_op_type operator()(const PGRecovery& o) const {
        return osd_op_type::OSD_OP_TYPE_PG_RECOVERY;
      }
      osd_op_type operator()(const PGScrub& o) const {
        return osd_op_type::OSD_OP_TYPE_PG_SCRUB;
      }
    };

    static pg_queueable_visitor visitor;

    osd_op_type get_osd_op_type(const Request& request) {
      osd_op_type type =
        boost::apply_visitor(visitor, request.second.get_variant());
      
      if (type != osd_op_type::OSD_OP_TYPE_CLIENT)
        return type;

      const OpRequestRef op =
        boost::get<OpRequestRef>(request.second.get_variant());
      uint16_t msg_type = op->get_req()->get_header().type;
      if (msg_type != CEPH_MSG_OSD_OP)
        return osd_op_type::OSD_OP_TYPE_SUBOP;

      return type;
    }

    void set_request_phase(const dmc::PhaseType phase, Request &item) {
      osd_op_type type =
        boost::apply_visitor(visitor, item.second.get_variant());

      if (type != osd_op_type::OSD_OP_TYPE_CLIENT)
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

    dmcQueue(CephContext *cct) : queue(&client_info_func) {
      load_global_dmc_qos_config(cct);
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
        fill_in_client_info(get_dmc_qos_spec(item)),
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
      queue.dump(f);
    }
  }; // class dmcQueue
} // namespace ceph
