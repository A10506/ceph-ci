// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

/*
 * Copyright (C) 2017 Red Hat Inc.
 */


#pragma once

#include <map>
#include <deque>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

#include "run_every.h"
#include "dmclock_util.h"
#include "dmclock_recs.h"

#include "gtest/gtest_prod.h"
#include "../../common/Formatter.h"

namespace crimson {
  namespace dmclock {
    struct ServerInfo {
      Counter   delta_prev_req;
      Counter   rho_prev_req;
      Counter   cost_prev_req;
      uint32_t  my_delta;
      uint32_t  my_rho;
      uint32_t  my_cost;

      Counter  sum_delta_self;
      Counter  sum_delta_prev;
      Counter  sum_rho_self;
      Counter  sum_rho_prev;
      Counter  sum_cost_self;
      Counter  sum_cost_prev;
      uint32_t rate_delta;
      uint32_t rate_rho;
      uint32_t rate_cost;
      uint32_t rate_delta_peak;
      uint32_t rate_rho_peak;

      std::vector<std::pair<uint32_t, uint32_t>> lastest_rates;
      uint32_t rates_idx;

      ServerInfo(Counter _delta_prev_req,
                 Counter _rho_prev_req, Counter _cost_prev_req) :
	delta_prev_req(_delta_prev_req),
	rho_prev_req(_rho_prev_req),
        cost_prev_req(_cost_prev_req),
	my_delta(0),
	my_rho(0),
        my_cost(0),
        sum_delta_self(0),
        sum_delta_prev(0),
        sum_rho_self(0),
        sum_rho_prev(0),
        sum_cost_self(0),
        sum_cost_prev(0),
        rate_delta(0),
        rate_rho(0),
        rate_cost(0),
        rate_delta_peak(0),
        rate_rho_peak(0),
        rates_idx(0)
      {
	// empty
      }

      inline void req_update(Counter delta, Counter rho, Counter cost) {
	delta_prev_req = delta;
	rho_prev_req = rho;
	cost_prev_req = cost;
	my_delta = 0;
	my_rho = 0;
	my_cost = 0;
      }

      inline void resp_update(PhaseType phase, uint32_t cost) {
	++my_delta;
        ++sum_delta_self;
        if (phase == PhaseType::reservation) {
          ++my_rho;
          ++sum_rho_self;
        }
        my_cost += cost;
        sum_cost_self += cost;
      }
    };


    // S is server identifier type
    template<typename S>
    class ServiceTracker {
      // we don't want to include gtest.h just for FRIEND_TEST
      friend class dmclock_client_server_erase_Test;

      using TimePoint = decltype(std::chrono::steady_clock::now());
      using Duration = std::chrono::milliseconds;
      using MarkPoint = std::pair<TimePoint,Counter>;

      Counter                 delta_counter; // # reqs completed
      Counter                 rho_counter;   // # reqs completed via reservation
      Counter                 cost_bytes;
      std::map<S,ServerInfo>  server_map;
      mutable std::mutex      data_mtx;      // protects Counters and map

      using DataGuard = std::lock_guard<decltype(data_mtx)>;

      // clean config

      std::deque<MarkPoint>     clean_mark_points;
      Duration                  clean_age;     // age at which ServerInfo cleaned

      // NB: All threads declared at end, so they're destructed firs!

      std::unique_ptr<RunEvery> cleaning_job;
      std::unique_ptr<RunEvery> calc_svr_job;

    public:

      // we have to start the counters at 1, as 0 is used in the
      // cleaning process
      template<typename Rep, typename Per>
      ServiceTracker(std::chrono::duration<Rep,Per> _clean_every,
		     std::chrono::duration<Rep,Per> _clean_age) :
	delta_counter(1),
	rho_counter(1),
	cost_bytes(1),
	clean_age(std::chrono::duration_cast<Duration>(_clean_age))
      {
	cleaning_job =
	  std::unique_ptr<RunEvery>(
	    new RunEvery(_clean_every,
			 std::bind(&ServiceTracker::do_clean, this)));

        calc_svr_job =
          std::unique_ptr<RunEvery>(
            new RunEvery(std::chrono::seconds(1),
             std::bind(&ServiceTracker::do_svr_calc, this)));

      }


      // the reason we're overloading the constructor rather than
      // using default values for the arguments is so that callers
      // have to either use all defaults or specify all timings; with
      // default arguments they could specify some without others
      ServiceTracker() :
	ServiceTracker(std::chrono::minutes(5), std::chrono::minutes(10))
      {
	// empty
      }


      /*
       * Incorporates the RespParams received into the various counter.
       */
      void track_resp(const S& server_id, const PhaseType& phase, Counter cost = 0) {
	DataGuard g(data_mtx);

	auto it = server_map.find(server_id);
	if (server_map.end() == it) {
	  // this code can only run if a request did not precede the
	  // response or if the record was cleaned up b/w when
	  // the request was made and now
	  ServerInfo si(delta_counter, rho_counter, cost_bytes);
	  si.resp_update(phase, uint32_t(cost));
	  server_map.emplace(server_id, si);
	} else {
	  it->second.resp_update(phase, uint32_t(cost));
	}

	++delta_counter;
	if (PhaseType::reservation == phase) {
	  ++rho_counter;
	}
	cost_bytes += cost;
      }


      /*
       * Returns the ReqParams for the given server.
       */
      ReqParams get_req_params(const S& server) {
	DataGuard g(data_mtx);
	auto it = server_map.find(server);
	if (server_map.end() == it) {
	  server_map.emplace(server, ServerInfo(delta_counter, rho_counter, cost_bytes));
	  return ReqParams(1, 1, 1);
	} else {
	  Counter delta =
	    1 + delta_counter - it->second.delta_prev_req - it->second.my_delta;
	  Counter rho =
	    1 + rho_counter - it->second.rho_prev_req - it->second.my_rho;
          Counter cost =
	    cost_bytes - it->second.cost_prev_req - it->second.my_cost;

	  it->second.req_update(delta_counter, rho_counter, cost_bytes);

	  return ReqParams(uint32_t(delta), uint32_t(rho), uint32_t(cost));
	}
      }

      void dump(ceph::Formatter *f) {
        f->open_object_section("servers_rate");
        for (auto i = server_map.begin(); i != server_map.end(); i++) {
          uint64_t avgrate_5seconds_sum  = 0;
          uint64_t avgrate_5seconds_res  = 0;
          uint64_t avgrate_30seconds_sum = 0;
          uint64_t avgrate_30seconds_res = 0;

          if ( i->second.rates_idx < 1
            || i->second.lastest_rates.size() < 1) {
            continue;
          }
          for (uint32_t n = 0, idx = (i->second.rates_idx - 1) % i->second.lastest_rates.size();
            n < 30; n++, idx = (idx == 0 ? i->second.lastest_rates.size() - 1 : idx - 1)) {
            if (n < 5) {
              avgrate_5seconds_sum += i->second.lastest_rates[idx].first;
              avgrate_5seconds_res += i->second.lastest_rates[idx].second;
            }
            avgrate_30seconds_sum += i->second.lastest_rates[idx].first;
            avgrate_30seconds_res += i->second.lastest_rates[idx].second;
          }
          avgrate_5seconds_sum = (avgrate_5seconds_sum + 2)/ 5; //+2 for round
          avgrate_5seconds_res = (avgrate_5seconds_res + 2)/ 5;
          avgrate_30seconds_sum = (avgrate_30seconds_sum + 15)/ 30; //+15 for round
          avgrate_30seconds_res = (avgrate_30seconds_res + 15)/ 30;

          std::stringstream oss;
          std::pair<int, int> osdshard = get_osd_shard(i->first);
          oss << "osd." << osdshard.first << "." << osdshard.second;
          f->dump_format(oss.str().c_str(), "[%6u|%-6u, %6u,%6u]  r:[%6u|%-6u, %6u,%6u] %u",
            i->second.rate_delta, i->second.rate_delta_peak,
            avgrate_5seconds_sum, avgrate_30seconds_sum,
            i->second.rate_rho, i->second.rate_rho_peak,
            avgrate_5seconds_res, avgrate_30seconds_res,
            i->second.rate_cost);
        }
        f->close_section();
      }


    private:

      /*
       * This is being called regularly by RunEvery. Every time it's
       * called it notes the time and delta counter (mark point) in a
       * deque. It also looks at the deque to find the most recent
       * mark point that is older than clean_age. It then walks the
       * map and delete all server entries that were last used before
       * that mark point.
       */
      void do_clean() {
	TimePoint now = std::chrono::steady_clock::now();
	DataGuard g(data_mtx);
	clean_mark_points.emplace_back(MarkPoint(now, delta_counter));

	Counter earliest = 0;
	auto point = clean_mark_points.front();
	while (point.first <= now - clean_age) {
	  earliest = point.second;
	  clean_mark_points.pop_front();
	  point = clean_mark_points.front();
	}

	if (earliest > 0) {
	  for (auto i = server_map.begin();
	       i != server_map.end();
	       /* empty */) {
	    auto i2 = i++;
	    if (i2->second.delta_prev_req <= earliest) {
	      server_map.erase(i2);
	    }
	  }
	}
      } // do_clean

      void do_svr_calc() {
        DataGuard g(data_mtx);
        for (auto i = server_map.begin(); i != server_map.end(); i++) {
          i->second.rate_delta = i->second.sum_delta_self - i->second.sum_delta_prev;
          i->second.rate_rho   = i->second.sum_rho_self - i->second.sum_rho_prev;
          i->second.rate_cost  = i->second.sum_cost_self - i->second.sum_cost_prev;

          i->second.sum_delta_prev = i->second.sum_delta_self;
          i->second.sum_rho_prev = i->second.sum_rho_self;
          i->second.sum_cost_prev = i->second.sum_cost_self;

          i->second.rate_delta_peak = i->second.rate_delta > i->second.rate_delta_peak ?
            i->second.rate_delta : i->second.rate_delta_peak;
          i->second.rate_rho_peak = i->second.rate_rho > i->second.rate_rho_peak ?
            i->second.rate_rho : i->second.rate_rho_peak;

          if (i->second.lastest_rates.size() < 32) {
            i->second.lastest_rates.emplace_back(
              std::make_pair(i->second.rate_delta, i->second.rate_rho));
          } else {
            i->second.lastest_rates[i->second.rates_idx % 32] =
              std::make_pair(i->second.rate_delta, i->second.rate_rho);
          }
          i->second.rates_idx++;
        }
      }

    }; // class ServiceTracker
  }
}
