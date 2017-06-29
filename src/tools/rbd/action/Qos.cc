// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "tools/rbd/ArgumentTypes.h"
#include "tools/rbd/Shell.h"
#include "tools/rbd/Utils.h"
#include "include/stringify.h"
#include "include/types.h"
#include "common/errno.h"
#include "common/Formatter.h"
#include "common/TextTable.h"
#include <iostream>
#include <boost/program_options.hpp>

#include "common/config.h"
#include "common/ceph_context.h"

extern CephContext *g_ceph_context;
extern md_config_t *g_conf;

namespace rbd {
namespace action {
namespace qos {

namespace at = argument_types;
namespace po = boost::program_options;

bool check_alldigital(string str)
{
  for (size_t i = 0; i < str.size(); i++) {
    int tmp = (int)str[i];
    if (tmp >= 48 && tmp <= 57) {
      continue;
    } else {
      return false;
    }
  }
  return true;
}

int get_qos_value(const po::variables_map &vm, int &qosr, int &qosw, int &qosl, int &qosb) {
  std::string val, miss;
  std::vector<std::string> vals;
  for (int i = 1; i <= 4; i++) {
    val = utils::get_positional_argument(vm, i);
    if (val.empty()) {
      miss = (i == 1) ? "reservation" :
             (i == 2 ? "weight" :( i == 3 ? "limit" : "bandwidth"));
      std::cerr << "error: qos " << miss << " was not specified" << std::endl;
      return -EINVAL;
    } else if (!check_alldigital(val)) {
      std::cerr << "error: invalid parameter \"" << val
                << "\", must be decimal numeric." << std::endl;
      return -EINVAL;
    }
    vals.push_back(val);
  }

  qosr = std::stoi(vals[0], nullptr);
  qosw = std::stoi(vals[1], nullptr);
  qosl = std::stoi(vals[2], nullptr);
  qosb = std::stoi(vals[3], nullptr);
  if (qosr < 0 || qosw < 0 || qosl < 0 || qosb < 0) {
    std::cerr << "error: qos spec should not be negative value" << std::endl;
    return -EINVAL;
  }
  if (qosl > 0 && qosl < qosr) {
    std::cerr << "error: qos resevation should not greater than limit" << std::endl;
    return -EINVAL;
  }
  if (qosr == 0 && qosw == 0) {
    std::cerr << "error: can not set both resevation and weight to zero" << std::endl;
    return -EINVAL;
  }
  return 0;
}


int qos_list(librbd::RBD &rbd, librados::IoCtx& io_ctx, Formatter *f) {
  std::vector<std::string> names;

  int r = rbd.list(io_ctx, names);
  if (r == -ENOENT) r = 0;
  if (r < 0) return r;

  TextTable tbl;
  if (f) {
    f->open_array_section("images");
  } else {
    tbl.define_column("NAME", TextTable::LEFT, TextTable::LEFT);
    tbl.define_column("RESRV", TextTable::RIGHT, TextTable::RIGHT);
    tbl.define_column("WEIGHT", TextTable::RIGHT, TextTable::RIGHT);
    tbl.define_column("LIMIT", TextTable::RIGHT, TextTable::RIGHT);
    tbl.define_column("BANDW", TextTable::RIGHT, TextTable::RIGHT);
    tbl.define_column("MFLAG", TextTable::RIGHT, TextTable::RIGHT);
  }

  for (std::vector<std::string>::const_iterator i = names.begin();
       i != names.end(); ++i) {
    librbd::Image image;    
    int qosr, qosw, qosl, qosb;
    int fmeta_rwl = 0;

    r = rbd.open_read_only(io_ctx, image, i->c_str(), NULL);
    // image might disappear between rbd.list() and rbd.open(); ignore
    // that, warn about other possible errors (EPERM, say, for opening
    // an old-format image, because you need execute permission for the
    // class method)
    if (r < 0) {
      if (r != -ENOENT) {
        std::cerr << "rbd: error opening " << *i << ": "
                  << cpp_strerror(r) << std::endl;
      }
      // in any event, continue to next image
      continue;
    }

    r = image.qos_spec_get(&qosr, &qosw, &qosl, &qosb, &fmeta_rwl);
    if (r < 0) {
      std::cerr << "rbd: failed to get qos template of image : " << *i
                << cpp_strerror(r) << std::endl;
      continue;
    }
    
    if (f) {
      f->open_object_section("image");
      f->dump_string("image", *i);
      f->dump_int("reservation", qosr);
      f->dump_int("weight", qosw);
      f->dump_int("limit", qosl);
      f->dump_int("bandwidth", qosb);
      f->dump_int("meta_falg", fmeta_rwl);
      f->close_section();
    } else {
      tbl << *i
          << qosr
          << qosw
          << qosl
          << qosb
          << fmeta_rwl
          << TextTable::endrow;
    }    
  }

  if (f) {
    f->close_section();
    f->flush(std::cout);
  } else if (!names.empty()) {
    std::cout << tbl;
  }

  return r < 0 ? r : 0;
}

void get_list_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_pool_options(positional, options);
  at::add_format_options(options);
}

void get_set_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
  positional->add_options()
    ("resrv", "image qos reservation");
  positional->add_options()
    ("weight", "image qos weight");
  positional->add_options()
    ("limit", "image qos limit");
  positional->add_options()
    ("bandw", "image qos bandwidth");

}

void get_get_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
}

void get_remove_arguments(po::options_description *positional,
                   po::options_description *options) {
  at::add_image_spec_options(positional, options, at::ARGUMENT_MODIFIER_NONE);
}

int execute_list(const po::variables_map &vm) {
  size_t arg_index = 0;
  std::string pool_name = utils::get_pool_name(vm, &arg_index);

  at::Format::Formatter formatter;
  int r = utils::get_formatter(vm, &formatter);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  r = utils::init(pool_name, &rados, &io_ctx);
  if (r < 0) {
    return r;
  }

  librbd::RBD rbd;
  r = qos_list(rbd, io_ctx, formatter.get());
  if (r < 0) {
    std::cerr << "rbd: qos list: " << cpp_strerror(r) << std::endl;
    return r;
  }

  return 0;
}

int execute_set(const po::variables_map &vm) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  int rsv, wgt, lmt, bdw;
  r = get_qos_value(vm, rsv, wgt, lmt, bdw);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, image_name, "", "", false,
                                 &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  r = image.qos_spec_set(rsv, wgt, lmt, bdw);
  if (r < 0) {
    return r;
  }

  return 0;
}

int execute_get(const po::variables_map &vm) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, image_name, "", "", false,
                                 &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  int qosr, qosw, qosl, qosb;
  r = image.qos_spec_get(&qosr, &qosw, &qosl, &qosb, nullptr);
  if (r < 0) {
    std::cerr << "failed to get qos spec of image : " << image_name
              << cpp_strerror(r) << std::endl;
    return r;
  }

  std::cout << "[ " << qosr
            << ", " << qosw
            << ", " << qosl
            << ", " << qosb
            << " ]" << std::endl;

  return 0;
}

int execute_remove(const po::variables_map &vm) {
  size_t arg_index = 0;
  std::string pool_name;
  std::string image_name;
  std::string snap_name;
  int r = utils::get_pool_image_snapshot_names(
    vm, at::ARGUMENT_MODIFIER_NONE, &arg_index, &pool_name, &image_name,
    &snap_name, utils::SNAPSHOT_PRESENCE_NONE, utils::SPEC_VALIDATION_NONE);
  if (r < 0) {
    return r;
  }

  librados::Rados rados;
  librados::IoCtx io_ctx;
  librbd::Image image;
  r = utils::init_and_open_image(pool_name, image_name, "", "", false,
                                 &rados, &io_ctx, &image);
  if (r < 0) {
    return r;
  }

  r = image.qos_spec_del();
  if (r < 0) {
    std::cerr << "failed to remove qos spec of image : " << image_name
              << cpp_strerror(r) << std::endl;
    return r;
  }

  return 0;
}

Shell::Action action_list(
  {"qos", "list"}, {"qos", "ls"}, "List qos specs of images.",
   "", &get_list_arguments, &execute_list);

Shell::Action action_set(
  {"qos", "set"}, {}, "Set qos specs for an image.",
   "", &get_set_arguments, &execute_set);

Shell::Action action_get(
  {"qos", "get"}, {}, "Get qos specs of an image.",
   "", &get_get_arguments, &execute_get);

Shell::Action action_remove(
  {"qos", "remove"}, {"qos", "rm"}, "Delete meta qos specs of an image "
   "(that's use default qos from configuration).",
   "", &get_remove_arguments, &execute_remove);

} // namespace qos
} // namespace action
} // namespace rbd
