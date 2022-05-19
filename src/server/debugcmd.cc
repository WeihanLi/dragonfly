// Copyright 2022, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//
#include "server/debugcmd.h"

#include <absl/strings/str_cat.h>

#include <boost/fiber/operations.hpp>
#include <filesystem>

#include "base/logging.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/main_service.h"
#include "server/rdb_load.h"
#include "server/server_state.h"
#include "server/string_family.h"
#include "server/transaction.h"
#include "util/uring/uring_fiber_algo.h"

DECLARE_string(dir);
DECLARE_string(dbfilename);

namespace dfly {

using namespace std;
using namespace util;
namespace this_fiber = ::boost::this_fiber;
using boost::intrusive_ptr;
using boost::fibers::fiber;
using namespace facade;
namespace fs = std::filesystem;
using absl::StrAppend;

struct PopulateBatch {
  DbIndex dbid;
  uint64_t index[32];
  uint64_t sz = 0;

  PopulateBatch(DbIndex id) : dbid(id) {
  }
};

struct ObjInfo {
  unsigned encoding;
  unsigned bucket_id = 0;
  unsigned slot_id = 0;

  int64_t ttl = INT64_MAX;
  bool has_sec_precision = false;

  ObjInfo(unsigned e, unsigned bid) : encoding(e), bucket_id(bid) {
  }

  ObjInfo() = default;
};

void DoPopulateBatch(std::string_view prefix, size_t val_size, const SetCmd::SetParams& params,
                     const PopulateBatch& batch) {
  SetCmd sg(&EngineShard::tlocal()->db_slice());

  for (unsigned i = 0; i < batch.sz; ++i) {
    string key = absl::StrCat(prefix, ":", batch.index[i]);
    string val = absl::StrCat("value:", batch.index[i]);

    if (val.size() < val_size) {
      val.resize(val_size, 'x');
    }
    sg.Set(params, key, val);
  }
}

DebugCmd::DebugCmd(ServerFamily* owner, ConnectionContext* cntx) : sf_(*owner), cntx_(cntx) {
}

void DebugCmd::Run(CmdArgList args) {
  std::string_view subcmd = ArgS(args, 1);
  if (subcmd == "HELP") {
    std::string_view help_arr[] = {
        "DEBUG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "OBJECT <key>",
        "    Show low-level info about `key` and associated value.",
        "LOAD <filename>"
        "RELOAD [option ...]",
        "    Save the RDB on disk (TBD) and reload it back to memory. Valid <option> values:",
        "    * NOSAVE: the database will be loaded from an existing RDB file.",
        "    Examples:",
        "    * DEBUG RELOAD NOSAVE: replace the current database with the contents of an",
        "      existing RDB file.",
        "POPULATE <count> [<prefix>] [<size>]",
        "    Create <count> string keys named key:<num>. If <prefix> is specified then",
        "    it is used instead of the 'key' prefix.",
        "HELP",
        "    Prints this help.",
    };
    return (*cntx_)->SendSimpleStrArr(help_arr, ABSL_ARRAYSIZE(help_arr));
  }

  VLOG(1) << "subcmd " << subcmd;

  if (subcmd == "POPULATE") {
    return Populate(args);
  }

  if (subcmd == "RELOAD") {
    return Reload(args);
  }

  if (subcmd == "LOAD" && args.size() == 3) {
    return Load(ArgS(args, 2));
  }

  if (subcmd == "OBJECT" && args.size() == 3) {
    string_view key = ArgS(args, 2);
    return Inspect(key);
  }

  string reply = absl::StrCat("Unknown subcommand or wrong number of arguments for '", subcmd,
                              "'. Try DEBUG HELP.");
  return (*cntx_)->SendError(reply, kSyntaxErr);
}

void DebugCmd::Reload(CmdArgList args) {
  bool save = true;

  for (size_t i = 2; i < args.size(); ++i) {
    ToUpper(&args[i]);
    std::string_view opt = ArgS(args, i);
    VLOG(1) << "opt " << opt;

    if (opt == "NOSAVE") {
      save = false;
    } else {
      return (*cntx_)->SendError("DEBUG RELOAD only supports the NOSAVE options.");
    }
  }

  error_code ec;

  if (save) {
    string err_details;
    const CommandId* cid = sf_.service().FindCmd("SAVE");
    CHECK_NOTNULL(cid);
    intrusive_ptr<Transaction> trans(new Transaction{cid});
    trans->InitByArgs(0, {});
    VLOG(1) << "Performing save";
    ec = sf_.DoSave(trans.get(), &err_details);
    if (ec) {
      return (*cntx_)->SendError(absl::StrCat(err_details, ec.message()));
    }
  }

  string last_save_file = sf_.LastSaveFile();
  Load(last_save_file);
}

void DebugCmd::Load(std::string_view filename) {
  auto [current, switched] = sf_.global_state()->Next(GlobalState::LOADING);
  if (!switched) {
    LOG(WARNING) << GlobalState::Name(current) << " in progress, ignored";
    return;
  }

  ProactorPool& pp = sf_.service().proactor_pool();
  pp.Await([&](ProactorBase*) {
    CHECK(ServerState::tlocal()->gstate() == GlobalState::IDLE);
    ServerState::tlocal()->set_gstate(GlobalState::LOADING);
  });

  const CommandId* cid = sf_.service().FindCmd("FLUSHALL");
  intrusive_ptr<Transaction> flush_trans(new Transaction{cid});
  flush_trans->InitByArgs(0, {});
  VLOG(1) << "Performing flush";
  error_code ec = sf_.DoFlush(flush_trans.get(), DbSlice::kDbAll);
  if (ec) {
    LOG(ERROR) << "Error flushing db " << ec.message();
  }

  fs::path path(filename);

  if (filename.empty()) {
    fs::path dir_path(FLAGS_dir);
    string filename = FLAGS_dbfilename;
    dir_path.append(filename);
    path = dir_path;
  }

  // switches back to
  ec = sf_.LoadRdb(path.generic_string());
  if (ec) {
    return (*cntx_)->SendError(ec.message());
  }

  (*cntx_)->SendOk();
}

void DebugCmd::Populate(CmdArgList args) {
  if (args.size() < 3 || args.size() > 5) {
    return (*cntx_)->SendError(
        "Unknown subcommand or wrong number of arguments for 'populate'. Try DEBUG HELP.");
  }

  uint64_t total_count = 0;
  if (!absl::SimpleAtoi(ArgS(args, 2), &total_count))
    return (*cntx_)->SendError(kUintErr);
  std::string_view prefix{"key"};

  if (args.size() > 3) {
    prefix = ArgS(args, 3);
  }
  uint32_t val_size = 0;
  if (args.size() > 4) {
    std::string_view str = ArgS(args, 4);
    if (!absl::SimpleAtoi(str, &val_size))
      return (*cntx_)->SendError(kUintErr);
  }

  ProactorPool& pp = sf_.service().proactor_pool();
  size_t runners_count = pp.size();
  vector<pair<uint64_t, uint64_t>> ranges(runners_count - 1);
  uint64_t batch_size = total_count / runners_count;
  size_t from = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    ranges[i].first = from;
    ranges[i].second = batch_size;
    from += batch_size;
  }
  ranges.emplace_back(from, total_count - from);

  vector<fiber> fb_arr(ranges.size());
  for (size_t i = 0; i < ranges.size(); ++i) {
    auto range = ranges[i];

    // whatever we do, we should not capture i by reference.
    fb_arr[i] = pp.at(i)->LaunchFiber([range, prefix, val_size, this] {
      this->PopulateRangeFiber(range.first, range.second, prefix, val_size);
    });
  }
  for (auto& fb : fb_arr)
    fb.join();

  (*cntx_)->SendOk();
}

void DebugCmd::PopulateRangeFiber(uint64_t from, uint64_t len, std::string_view prefix,
                                  unsigned value_len) {
  this_fiber::properties<FiberProps>().set_name("populate_range");
  VLOG(1) << "PopulateRange: " << from << "-" << (from + len - 1);

  string key = absl::StrCat(prefix, ":");
  size_t prefsize = key.size();
  DbIndex db_indx = cntx_->db_index();
  EngineShardSet& ess = *shard_set;
  std::vector<PopulateBatch> ps(ess.size(), PopulateBatch{db_indx});
  SetCmd::SetParams params{db_indx};

  for (uint64_t i = from; i < from + len; ++i) {
    StrAppend(&key, i);
    ShardId sid = Shard(key, ess.size());
    key.resize(prefsize);  // shrink back

    auto& shard_batch = ps[sid];
    shard_batch.index[shard_batch.sz++] = i;
    if (shard_batch.sz == 32) {
      ess.Add(sid, [=] {
        DoPopulateBatch(prefix, value_len, params, shard_batch);
        if (i % 50 == 0) {
          this_fiber::yield();
        }
      });

      // we capture shard_batch by value so we can override it here.
      shard_batch.sz = 0;
    }
  }

  ess.RunBlockingInParallel([&](EngineShard* shard) {
    DoPopulateBatch(prefix, value_len, params, ps[shard->shard_id()]);
  });
}

void DebugCmd::Inspect(string_view key) {
  EngineShardSet& ess = *shard_set;
  ShardId sid = Shard(key, ess.size());

  auto cb = [&]() -> facade::OpResult<ObjInfo> {
    auto& db_slice = EngineShard::tlocal()->db_slice();
    auto [pt, exp_t] = db_slice.GetTables(cntx_->db_index());

    PrimeIterator it = pt->Find(key);
    if (!IsValid(it)) {
      return OpStatus::KEY_NOTFOUND;
    }

    ObjInfo oinfo(it->second.Encoding(), it.bucket_id());
    oinfo.slot_id = it.slot_id();

    if (it->second.HasExpire()) {
      ExpireIterator exp_it = exp_t->Find(it->first);
      CHECK(!exp_it.is_done());

      time_t exp_time = db_slice.ExpireTime(exp_it);
      oinfo.ttl = exp_time - db_slice.Now();
      oinfo.has_sec_precision = exp_it->second.is_second_precision();
    }

    return oinfo;
  };

  OpResult<ObjInfo> res = ess.Await(sid, cb);
  if (res) {
    string resp;
    StrAppend(&resp, "encoding:", strEncoding(res->encoding), " bucket_id:", res->bucket_id);
    StrAppend(&resp, " slot:", res->slot_id);

    if (res->ttl != INT64_MAX) {
      StrAppend(&resp, " ttl:", res->ttl, res->has_sec_precision ? "s" : "ms");
    }
    (*cntx_)->SendSimpleString(resp);
  } else {
    (*cntx_)->SendError(res.status());
  }
}

}  // namespace dfly
