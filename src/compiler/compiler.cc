// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "compiler/compiler.h"

#include "common/debug.h"
#include "common/expected.h"

#include "perfetto/rx_producer.h"  // TODO: refactor BinaryWireProtobuf to separate header.
#include "inode2filename/inode.h"
#include "inode2filename/search_directories.h"

#include <android-base/unique_fd.h>
#include <android-base/parseint.h>
#include <android-base/file.h>

#include <perfetto/trace/trace.pb.h>  // ::perfetto::protos::Trace
#include <perfetto/trace/trace_packet.pb.h>  // ::perfetto::protos::TracePacket

#include "rxcpp/rx.hpp"
#include <iostream>
#include <fstream>
#include <optional>

#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <syscall.h>
#include <fcntl.h>
#include <unistd.h>

namespace iorap::compiler {

using Inode = iorap::inode2filename::Inode;
using InodeResult = iorap::inode2filename::InodeResult;
using SearchDirectories = iorap::inode2filename::SearchDirectories;

template <typename T>
using ProtobufPtr = iorap::perfetto::ProtobufPtr<T>;

// Attempt to read protobufs from the filenames.
// Emits one (or none) protobuf for each filename, in the same order as the filenames.
// On any errors, the items are dropped (errors are written to the error LOG).
//
// All work is done on the same Coordinator as the Subscriber.
template <typename ProtoT /*extends MessageLite*/>
auto/*observable<ProtoT>*/ ReadProtosFromFileNames(rxcpp::observable<std::string> filenames) {
  // using BinaryWireProtoT = ::iorap::perfetto::BinaryWireProtobuf<ProtoT>;
  using BinaryWireProtoT = ::iorap::perfetto::PerfettoTraceProto;

  return filenames
    .map([](const std::string& filename) {
      LOG(VERBOSE) << "compiler::ReadProtosFromFileNames " << filename << " [begin]";
      std::optional<BinaryWireProtoT> maybe_proto =
          BinaryWireProtoT::ReadFullyFromFile(filename);
      if (!maybe_proto) {
        LOG(ERROR) << "Failed to read file: " << filename;
      }
      return maybe_proto;
    })
    .filter([](const std::optional<BinaryWireProtoT>& proto) {
      return proto.has_value();
    })
    .map([](std::optional<BinaryWireProtoT> proto) -> BinaryWireProtoT {
      return std::move(proto.value());
    })  // TODO: refactor to something that flattens the optional, and logs in one operator.
    .map([](BinaryWireProtoT proto) {
      std::optional<ProtobufPtr<ProtoT>> t = proto.template MaybeUnserialize<ProtoT>();
      if (!t) {
        LOG(ERROR) << "Failed to parse protobuf: ";  // TODO: filename.
      }
      return t;
    })
    .filter([](const std::optional<ProtobufPtr<ProtoT>>& proto) {
      return proto.has_value();
    })
    .map([](std::optional<ProtobufPtr<ProtoT>> proto) -> ProtobufPtr<ProtoT> {
      LOG(VERBOSE) << "compiler::ReadProtosFromFileNames [success]";
      return std::move(proto.value());
      // TODO: protobufs have no move constructor. this might be inefficient?
    });

/*
  return filenames
    .map([](const std::string& filename) {
      LOG(VERBOSE) << "compiler::ReadProtosFromFileNames " << filename << " [begin]";
      std::optional<BinaryWireProtoT> maybe_proto =
          BinaryWireProtoT::ReadFullyFromFile(filename);
      if (!maybe_proto) {
        LOG(ERROR) << "Failed to read file: " << filename;
      }

      std::unique_ptr<BinaryWireProtoT> ptr;
      if (maybe_proto) {
        ptr.reset(new BinaryWireProtoT{std::move(*maybe_proto)});
      }
      return ptr;
    })
    .filter([](const std::unique_ptr<BinaryWireProtoT>& proto) {
      return proto != nullptr;
    })
    .map([](std::unique_ptr<BinaryWireProtoT>& proto) {
      std::optional<ProtoT> t = proto->template MaybeUnserialize<ProtoT>();
      if (!t) {
        LOG(ERROR) << "Failed to parse protobuf: ";  // TODO: filename.
      }
      return t;
    })
    .filter([](const std::optional<ProtoT>& proto) {
      return proto.has_value();
    })
    .map([](std::optional<ProtoT> proto) -> ProtoT {
      LOG(VERBOSE) << "compiler::ReadProtosFromFileNames [success]";
      return std::move(proto.value());
      // TODO: protobufs have no move constructor. this might be inefficient?
    });
    */
}

auto/*observable<::perfetto::protos::Trace>*/ ReadPerfettoTraceProtos(
    std::vector<std::string> filenames) {
  auto filename_obs = rxcpp::observable<>::iterate(std::move(filenames));
  rxcpp::observable<ProtobufPtr<::perfetto::protos::Trace>> obs =
      ReadProtosFromFileNames<::perfetto::protos::Trace>(std::move(filename_obs));
  return obs;
}

// A flattened data representation of an MmFileMap*FtraceEvent.
// This representation is used for streaming processing.
//
// Note: Perfetto applies a 'union' over all possible fields on all possible devices
// (and uses the max sizeof per-field).
//
// Since all protobuf fields are optional, fields not present on a particular device are always
// null
struct PageCacheFtraceEvent {
  /*
   * Ftrace buffer-specific
   */
  uint32_t cpu;  // e.g. 0-7 for the cpu core number.

  /*
   * Ftrace-event general data
   */

  // Nanoseconds since an epoch.
  // Epoch is configurable by writing into trace_clock.
  // By default this timestamp is CPU local.
  uint64_t timestamp;
  // Kernel pid (do not confuse with userspace pid aka tgid)
  uint32_t pid;

  // Tagged by our code while parsing the ftraces:
  uint64_t timestamp_relative;  // timestamp relative to first ftrace within a Trace protobuf.
  bool add_to_page_cache;  // AddToPageCache=true, DeleteFromPageCache=false.

  /*
   * mm_filemap-specific data
   *
   * Fields are common:
   * - MmFilemapAddToPageCacheFtraceEvent
   * - MmFilemapDeleteFromPageCacheFtraceEvent
   */
  uint64_t pfn;    // page frame number (physical) - null on some devices, e.g. marlin
  uint64_t i_ino;  // inode number (use in conjunction with s_dev)
  uint64_t index;  // offset into file: this is a multiple of the page size (usually 4096).
  uint64_t s_dev;  // (dev_t) device number
  uint64_t page;   // struct page*. - null on some devices, e.g. blueline.

  Inode inode() const {
    return Inode::FromDeviceAndInode(static_cast<dev_t>(s_dev),
                                     static_cast<ino_t>(i_ino));
  }
};

std::ostream& operator<<(std::ostream& os, const PageCacheFtraceEvent& e) {
  os << "{";
  os << "cpu:" << e.cpu << ",";
  os << "timestamp:" << e.timestamp << ",";
  os << "pid:" << e.pid << ",";
  os << "timestamp_relative:" << e.timestamp_relative << ",";
  os << "add_to_page_cache:" << e.add_to_page_cache << ",";
  os << "pfn:" << e.pfn << ",";
  os << "i_ino:" << e.i_ino << ",";
  os << "index:" << e.index << ",";
  os << "s_dev:" << e.s_dev << ",";
  os << "page:" << e.page;
  os << "}";

  return os;
}

/*
 * sample blueline output:
 *
 * $ adb shell cat /d/tracing/events/filemap/mm_filemap_add_to_page_cache/format
 *
 * name: mm_filemap_add_to_page_cache
 * ID: 178
 * format:
 * 	field:unsigned short common_type;	offset:0;	size:2;	signed:0;
 * 	field:unsigned char common_flags;	offset:2;	size:1;	signed:0;
 * 	field:unsigned char common_preempt_count;	offset:3;	size:1;	signed:0;
 * 	field:int common_pid;	offset:4;	size:4;	signed:1;
 *
 * 	field:unsigned long pfn;	offset:8;	size:8;	signed:0;
 * 	field:unsigned long i_ino;	offset:16;	size:8;	signed:0;
 * 	field:unsigned long index;	offset:24;	size:8;	signed:0;
 * 	field:dev_t s_dev;	offset:32;	size:4;	signed:0;
 *
 * print fmt: "dev %d:%d ino %lx page=%p pfn=%lu ofs=%lu", ((unsigned int) ((REC->s_dev) >> 20)),
 *            ((unsigned int) ((REC->s_dev) & ((1U << 20) - 1))), REC->i_ino,
 *             (((struct page *)(((0xffffffffffffffffUL) - ((1UL) << ((39) - 1)) + 1) -
 *                 ((1UL) << ((39) - 12 - 1 + 6))) - (memstart_addr >> 12)) + (REC->pfn)),
 *            REC->pfn, REC->index << 12
 */

auto /*observable<PageCacheFtraceEvent>*/ SelectPageCacheFtraceEvents(
    ProtobufPtr<::perfetto::protos::Trace> trace_ptr) {
  const ::perfetto::protos::Trace& trace = *trace_ptr;

  constexpr bool kDebugFunction = true;

  return rxcpp::observable<>::create<PageCacheFtraceEvent>(
      [trace=std::move(trace)](rxcpp::subscriber<PageCacheFtraceEvent> sub) {
    uint64_t timestamp = 0;
    uint64_t timestamp_relative = 0;

    std::optional<uint64_t> timestamp_relative_start;
    uint32_t cpu = 0;
    uint32_t pid = 0;
    bool add_to_page_cache = true;

    auto on_next_page_cache_event = [&](const auto& mm_event) {
      PageCacheFtraceEvent out;
      out.timestamp = timestamp;
      out.cpu = cpu;
      out.pid = pid;

      out.timestamp_relative = timestamp_relative;
      out.add_to_page_cache = add_to_page_cache;

      out.pfn = mm_event.pfn();
      out.i_ino = mm_event.i_ino();
      out.index = mm_event.index();
      out.s_dev = mm_event.s_dev();
      out.page = mm_event.page();

      sub.on_next(std::move(out));
    };

    for (const ::perfetto::protos::TracePacket& packet : trace.packet()) {
      // Break out of all loops if we are unsubscribed.
      if (!sub.is_subscribed()) {
        if (kDebugFunction) LOG(VERBOSE) << "compiler::SelectPageCacheFtraceEvents unsubscribe";
        return;
      }

      if (kDebugFunction) LOG(VERBOSE) << "compiler::SelectPageCacheFtraceEvents TracePacket";

      if (packet.has_timestamp()) {
        timestamp_relative_start = timestamp_relative_start.value_or(packet.timestamp());
        timestamp = packet.timestamp();  // XX: should we call 'has_timestamp()' ?
      } else {
        timestamp = 0;
      }

      if (packet.has_ftrace_events()) {
        const ::perfetto::protos::FtraceEventBundle& ftrace_event_bundle =
            packet.ftrace_events();

        cpu = ftrace_event_bundle.cpu();  // XX: has_cpu ?

        for (const ::perfetto::protos::FtraceEvent& event : ftrace_event_bundle.event()) {
          // Break out of all loops if we are unsubscribed.
          if (!sub.is_subscribed()) {
            return;
          }

          if (event.has_timestamp()) {
            timestamp_relative_start = timestamp_relative_start.value_or(event.timestamp());
            timestamp = event.timestamp();
          } else {
            DCHECK(packet.has_timestamp() == false)
                << "Timestamp in outer packet but not inner packet";
            // XX: use timestamp from the perfetto TracePacket ???
            // REVIEWERS: not sure if this is ok, does it use the same clock source and
            // is the packet data going to be the same clock sample as the Ftrace event?
          }

          if (timestamp_relative_start) {
            // Otherwise this assumption is incorrect and we need an extra pass
            // to determine the minimum timestamp in a trace.

            // FIXME: handle this case:
            if ((false)) {
              DCHECK_GE(timestamp, *timestamp_relative_start)
                  << "Ftrace timestamps must rise in value";
              // TODO: if this fails, we can probably just do this in separate functions?
              // this function is already becoming quite large.
            }

            if (timestamp < *timestamp_relative_start) {  // when DCHECK is disabled.
              LOG(WARNING) << "FIXME: Ftrace timestamps must rise in value: "
                           << "timestamp=" << timestamp << "ns, "
                           << "timestamp_relative=" << *timestamp_relative_start << "ns";

              // avoid underflow
              timestamp_relative = 0;
            } else {
              timestamp_relative = timestamp - *timestamp_relative_start;
            }
          } else {
            timestamp_relative = 0;
          }

          {
            // FIXME: add an extra pass to calculate minimum timestamp for proper relativeness.
            // Using this workaround will 'break' proper optimal merging of multiple traces,
            // but a single trace will still be compiled fine.
            timestamp_relative = timestamp;
          }

          pid = event.pid();  // XX: has_pid ?

          if (event.has_mm_filemap_add_to_page_cache()) {
            add_to_page_cache = true;

            const ::perfetto::protos::MmFilemapAddToPageCacheFtraceEvent& mm_event =
                event.mm_filemap_add_to_page_cache();

            on_next_page_cache_event(mm_event);
          } else if (event.has_mm_filemap_delete_from_page_cache()) {
            add_to_page_cache = false;

            const ::perfetto::protos::MmFilemapDeleteFromPageCacheFtraceEvent& mm_event =
                event.mm_filemap_delete_from_page_cache();

            on_next_page_cache_event(mm_event);
          }
        }
      } else {
        if (kDebugFunction) {
          LOG(VERBOSE) << "compiler::SelectPageCacheFtraceEvents no ftrace event bundle";
        }
      }
    }

    if (kDebugFunction) {
      LOG(VERBOSE) << "compiler::SelectPageCacheFtraceEvents#on_completed";
    }

    // Let subscriber know there are no more items.
    sub.on_completed();
  });
}

auto /*observable<Inode*/ SelectDistinctInodesFromTraces(
    rxcpp::observable<ProtobufPtr<::perfetto::protos::Trace>> traces) {
  // Emit only unique (s_dev, i_ino) pairs from all Trace protos.
  auto obs = traces
    .flat_map([](ProtobufPtr<::perfetto::protos::Trace> trace) {
      rxcpp::observable<PageCacheFtraceEvent> obs = SelectPageCacheFtraceEvents(std::move(trace));
      // FIXME: dont check this in
      // return obs;
      //return obs.take(100);   // for faster development
      return obs;
    })  // TODO: Upstream bug? using []()::perfetto::protos::Trace&) causes a compilation error.
    .map([](const PageCacheFtraceEvent& event) -> Inode {
      return Inode::FromDeviceAndInode(static_cast<dev_t>(event.s_dev),
                                       static_cast<ino_t>(event.i_ino));
    })
    .tap([](const Inode& inode) {
      LOG(VERBOSE) << "SelectDistinctInodesFromTraces (pre-distinct): " << inode;
    })
    .distinct()  // observable<Inode>*/
    ;

  return obs;
}
// TODO: static assert checks for convertible return values.

auto/*observable<InodeResult>*/ ResolveInodesToFileNames(rxcpp::observable<Inode> inodes) {
  static auto* system_call = new SystemCallImpl{};
  std::vector<std::string> root_directories;
  root_directories.push_back("/system");
  root_directories.push_back("/apex");
  root_directories.push_back("/data");
  root_directories.push_back("/vendor");
  root_directories.push_back("/product");
  root_directories.push_back("/metadata");
  inode2filename::SearchMode search_mode{inode2filename::SearchMode::kInProcessDirect};
  // TODO: above parameters should be injectable.

  SearchDirectories search{borrowed<SystemCall*>(system_call)};
  return search.FindFilenamesFromInodes(std::move(root_directories),
                                        std::move(inodes),
                                        search_mode);
}

using InodeMap = std::unordered_map<Inode, std::string /*filename*/>;
auto /*just observable<InodeMap>*/ ReduceResolvedInodesToMap(
      rxcpp::observable<InodeResult> inode_results) {
  return inode_results.reduce(
    InodeMap{},
    [](InodeMap m, InodeResult result) {
      if (result) {
        LOG(VERBOSE) << "compiler::ReduceResolvedInodesToMap insert " << result;
        m.insert({std::move(result.inode), std::move(result.data.value())});
      } else {
        // TODO: side stats for how many of these are failed to resolve?
        LOG(WARNING) << "compiler: Failed to resolve inode, " << result;
      }
      return m;
    },
    [](InodeMap m) {
      return m;  // TODO: use an identity function
    }); // emits exactly 1 InodeMap value.
}

struct ResolvedPageCacheFtraceEvent {
  std::string filename;
  PageCacheFtraceEvent event;
};

std::ostream& operator<<(std::ostream& os, const ResolvedPageCacheFtraceEvent& e) {
  os << "{";
  os << "filename:\"" << e.filename << "\",";
  os << e.event;
  os << "}";

  return os;
}

struct CombinedState {
  CombinedState() = default;
  explicit CombinedState(InodeMap inode_map) : inode_map{std::move(inode_map)} {}
  explicit CombinedState(PageCacheFtraceEvent event) : ftrace_event{std::move(event)} {}

  CombinedState(InodeMap inode_map, PageCacheFtraceEvent event)
    : inode_map(std::move(inode_map)),
      ftrace_event{std::move(event)} {}

  std::optional<InodeMap> inode_map;
  std::optional<PageCacheFtraceEvent> ftrace_event;

  bool HasAll() const {
    return inode_map.has_value() && ftrace_event.has_value();
  }

  const InodeMap& GetInodeMap() const {
    DCHECK(HasAll());
    return inode_map.value();
  }

  InodeMap& GetInodeMap() {
    DCHECK(HasAll());
    return inode_map.value();
  }

  const PageCacheFtraceEvent& GetEvent() const {
    DCHECK(HasAll());
    return ftrace_event.value();
  }

  PageCacheFtraceEvent& GetEvent() {
    DCHECK(HasAll());
    return ftrace_event.value();
  }

  void Merge(CombinedState&& other) {
    if (other.inode_map) {
      inode_map = std::move(other.inode_map);
    }
    if (other.ftrace_event) {
      ftrace_event = std::move(other.ftrace_event);
    }
  }
};

std::ostream& operator<<(std::ostream& os, const CombinedState& s) {
  os << "CombinedState{inode_map:";
  if (s.inode_map) {
    os << "|sz=" << (s.inode_map.value().size()) << "|";
  } else {
    os << "(null)";
  }
  os << ",event:";
  if (s.ftrace_event) {
    //os << s.ftrace_event.value().timestamp << "ns";
    os << s.ftrace_event.value();
  } else {
    os << "(null)";
  }
  os << "}";
  return os;
}

auto/*observable<ResolvedPageCacheFtraceEvent>*/ ResolvePageCacheEntriesFromProtos(
    rxcpp::observable<ProtobufPtr<::perfetto::protos::Trace>> traces) {

  // 1st chain = emits exactly 1 InodeMap.

  // [proto, proto, proto...] -> [inode, inode, inode, ...]
  auto/*observable<Inode>*/ distinct_inodes = SelectDistinctInodesFromTraces(traces);
  rxcpp::observable<Inode> distinct_inodes_obs = distinct_inodes.as_dynamic();
  // [inode, inode, inode, ...] -> [(inode, {filename|error}), ...]
  auto/*observable<InodeResult>*/ inode_names = ResolveInodesToFileNames(distinct_inodes_obs);
  // rxcpp has no 'join' operators, so do a manual join with concat.
  auto/*observable<InodeMap>*/ inode_name_map = ReduceResolvedInodesToMap(inode_names);

  // 2nd chain = emits all PageCacheFtraceEvent
  auto/*observable<PageCacheFtraceEvent>*/ page_cache_ftrace_events = traces
    .flat_map([](ProtobufPtr<::perfetto::protos::Trace> trace) {
      rxcpp::observable<PageCacheFtraceEvent> obs = SelectPageCacheFtraceEvents(std::move(trace));
      return obs;
    });

  auto inode_name_map_precombine = inode_name_map
    .map([](InodeMap inode_map) {
      LOG(VERBOSE) << "compiler::ResolvePageCacheEntriesFromProtos#inode_name_map_precombine ";
      return CombinedState{std::move(inode_map)};
    });

  auto page_cache_ftrace_events_precombine = page_cache_ftrace_events
    .map([](PageCacheFtraceEvent event) {
      LOG(VERBOSE)
          << "compiler::ResolvePageCacheEntriesFromProtos#page_cache_ftrace_events_precombine "
          << event;
      return CombinedState{std::move(event)};
    });

  // Combine 1st+2nd chain.
  //
  // concat subscribes to each observable, waiting until its completed, before subscribing
  // to the next observable and waiting again.
  //
  // During all this, every #on_next is immediately forwarded to the downstream observables.
  // In our case, we want to block until InodeNameMap is ready, and re-iterate all ftrace events.
  auto/*observable<ResolvedPageCacheFtraceEvent>*/ resolved_events = inode_name_map_precombine
    .concat(page_cache_ftrace_events_precombine)
    .scan(CombinedState{},
          [](CombinedState current_state, CombinedState delta_state) {
            LOG(VERBOSE) << "compiler::ResolvePageCacheEntriesFromProtos#scan "
                          << "current=" << current_state << ","
                          << "delta=" << delta_state;
            // IT0    = (,)               + (InodeMap,)
            // IT1    = (InodeMap,)       + (,Event)
            // IT2..N = (InodeMap,Event1) + (,Event2)
            current_state.Merge(std::move(delta_state));
            return current_state;
          })
    .filter([](const CombinedState& state) {
      return state.HasAll();
    })
    .map([](CombinedState& state) -> std::optional<ResolvedPageCacheFtraceEvent> {
      PageCacheFtraceEvent& event = state.GetEvent();
      const InodeMap& inode_map = state.GetInodeMap();

      auto it = inode_map.find(event.inode());
      if (it != inode_map.end()) {
        std::string filename = it->second;
        LOG(VERBOSE) << "compiler::ResolvePageCacheEntriesFromProtos combine_latest " << event;
        return ResolvedPageCacheFtraceEvent{std::move(filename), std::move(event)};
      } else {
        LOG(ERROR) << "compiler: FtraceEvent's inode did not have resolved filename: " << event;
        return std::nullopt;
      }
    })
    .filter(
      [](std::optional<ResolvedPageCacheFtraceEvent> maybe_event) {
        return maybe_event.has_value();
      })
    .map([](std::optional<ResolvedPageCacheFtraceEvent> maybe_event) {
      return std::move(maybe_event.value());
    });
    // -> observable<ResolvedPageCacheFtraceEvent>

  return resolved_events;
}

namespace detail {
bool multiless_one(const std::string& a, const std::string& b) {
  return std::lexicographical_compare(a.begin(), a.end(),
                                      b.begin(), b.end());
}

template <typename T>
constexpr bool multiless_one(T&& a, T&& b) {   // a < b
  using std::less;  // ADL
  return less<std::decay_t<T>>{}(std::forward<T>(a), std::forward<T>(b));
}

constexpr bool multiless() {
  return false;  // [] < [] is always false.
}

template <typename T, typename ... Args>
constexpr bool multiless(T&& a, T&& b, Args&&... args) {
  if (a != b) {
    return multiless_one(std::forward<T>(a), std::forward<T>(b));
  } else {
    return multiless(std::forward<Args>(args)...);
  }
}

}  // namespace detail

// Return [A0...An] < [B0...Bn] ; vector-like scalar comparison of each field.
// Arguments are passed in the order A0,B0,A1,B1,...,An,Bn.
template <typename ... Args>
constexpr bool multiless(Args&&... args) {
  return detail::multiless(std::forward<Args>(args)...);
}

struct CompilerPageCacheEvent {
  std::string filename;
  uint64_t timestamp_relative;  // use relative timestamp because absolute values aren't comparable
                                // across different trace protos.
                                // relative timestamps can be said to be 'approximately' comparable.
                                // assuming we compare the same application startup's trace times.
  bool add_to_page_cache;  // AddToPageCache=true, DeleteFromPageCache=false.
  uint64_t index;          // offset into file: this is a multiple of the page size (usually 4096).

  // All other data from the ftrace is dropped because we don't currently use it in the
  // compiler algorithms.

  CompilerPageCacheEvent() = default;
  CompilerPageCacheEvent(const ResolvedPageCacheFtraceEvent& resolved)
    : CompilerPageCacheEvent(resolved.filename, resolved.event) {
  }

  CompilerPageCacheEvent(ResolvedPageCacheFtraceEvent&& resolved)
    : CompilerPageCacheEvent(std::move(resolved.filename), std::move(resolved.event)) {
  }

  // Compare all fields (except the timestamp field).
  static bool LessIgnoringTimestamp(const CompilerPageCacheEvent& a,
                                    const CompilerPageCacheEvent& b) {
    return multiless(a.filename, b.filename,
                     a.add_to_page_cache, b.add_to_page_cache,
                     a.index, b.index);
  }

  // Compare all fields. Timestamps get highest precedence.
  bool operator<(const CompilerPageCacheEvent& rhs) const {
    return multiless(timestamp_relative, rhs.timestamp_relative,
                     filename, rhs.filename,
                     add_to_page_cache, rhs.add_to_page_cache,
                     index, rhs.index);
  }

 private:
  CompilerPageCacheEvent(std::string filename, const PageCacheFtraceEvent& event)
    : filename(std::move(filename)),
      timestamp_relative(event.timestamp_relative),
      add_to_page_cache(event.add_to_page_cache),
      index(event.index) {
   }
};

std::ostream& operator<<(std::ostream& os, const CompilerPageCacheEvent& e) {
  os << "{";
  os << "filename:\"" << e.filename << "\",";
  os << "timestamp:" << e.timestamp_relative << ",";
  os << "add_to_page_cache:" << e.add_to_page_cache << ",";
  os << "index:" << e.index;
  os << "}";
  return os;
}

// Compile an observable chain of 'ResolvedPageCacheFtraceEvent' into
// an observable chain of distinct, timestamp-ordered, CompilerPageCacheEvent.
//
// This is a reducing operation: No items are emitted until resolved_events is completed.
auto/*observable<CompilerPageCacheEvent>*/ CompilePageCacheEvents(
    rxcpp::observable<ResolvedPageCacheFtraceEvent> resolved_events) {

  struct CompilerPageCacheEventIgnoringTimestampLess {
    bool operator()(const CompilerPageCacheEvent& lhs,
                    const CompilerPageCacheEvent& rhs) const {
      return CompilerPageCacheEvent::LessIgnoringTimestamp(lhs, rhs);
    }
  };

  // Greedy O(N) compilation algorithm.
  //
  // This produces an inoptimal result (e.g. a small timestamp
  // that might occur only 1% of the time nevertheless wins out), but the
  // algorithm itself is quite simple, and doesn't require any heuristic tuning.

  // First pass: *Merge* into set that ignores the timestamp value for order, but retains
  //             the smallest timestamp value if the same key is re-inserted.
  using IgnoreTimestampForOrderingSet =
      std::set<CompilerPageCacheEvent, CompilerPageCacheEventIgnoringTimestampLess>;
  // Second pass: *Sort* data by smallest timestamp first.
  using CompilerPageCacheEventSet =
      std::set<CompilerPageCacheEvent>;

  return resolved_events
    .map(
      [](ResolvedPageCacheFtraceEvent event) {
        // Drop all the extra metadata like pid, cpu, etc.
        // When we merge we could keep a list of the original data, but there is no advantage
        // to doing so.
        return CompilerPageCacheEvent{std::move(event)};
      }
    )
   .reduce(
    IgnoreTimestampForOrderingSet{},
    [](IgnoreTimestampForOrderingSet set, CompilerPageCacheEvent event) {
      // Add each event to the set, keying by everything but the timestamp.
      // If the key is already inserted, re-insert with the smaller timestamp value.
      auto it = set.find(event);

      if (it == set.end()) {
        // Need to insert new element.
        set.insert(std::move(event));
      } else if (it->timestamp_relative > event.timestamp_relative) {
        // Replace existing element: the new element has a smaller timestamp.
        it = set.erase(it);
        // Amortized O(1) time if insertion happens in the position before the hint.
        set.insert(it, std::move(event));
      } // else: Skip insertion. Element already present with the minimum timestamp.

      return set;
    },
    [](IgnoreTimestampForOrderingSet set) {
      // Extract all elements from 'set', re-insert into 'ts_set'.
      // The values are now ordered by timestamp (and then the rest of the fields).
      CompilerPageCacheEventSet ts_set;
      ts_set.merge(std::move(set));


      std::shared_ptr<CompilerPageCacheEventSet> final_set{
          new CompilerPageCacheEventSet{std::move(ts_set)}};
      return final_set;
      // return ts_set;
    })  // observable<CompilerPageCacheEventSet> (just)
  .flat_map(
    [](std::shared_ptr<CompilerPageCacheEventSet> final_set) {
      // TODO: flat_map seems to make a copy of the parameter _every single iteration_
      // without the shared_ptr it would just make a copy of the set every time it went
      // through the iterate function.
      // Causing absurdly slow compile times x1000 slower than we wanted.
      // TODO: file a bug upstream and/or fix upstream.
      CompilerPageCacheEventSet& ts_set = *final_set;
    // [](CompilerPageCacheEventSet& ts_set) {
      LOG(DEBUG) << "compiler: Merge-pass completed (" << ts_set.size() << " entries).";
      //return rxcpp::sources::iterate(std::move(ts_set));
      return rxcpp::sources::iterate(ts_set).map([](CompilerPageCacheEvent e) { return e; });
    }
  );    // observable<CompilerPageCacheEvent>
}

bool PerformCompilation(std::vector<std::string> input_file_names, std::string output_file_name) {
  auto trace_protos = ReadPerfettoTraceProtos(std::move(input_file_names));
  auto resolved_events = ResolvePageCacheEntriesFromProtos(std::move(trace_protos));
  auto compiled_events = CompilePageCacheEvents(std::move(resolved_events));

  // TODO: write to a file, instead of to stdout.
  (void)output_file_name;

  std::ofstream ofs;
  if (!output_file_name.empty()) {
    ofs.open(output_file_name);

    if (!ofs) {
      LOG(ERROR) << "compiler: Failed to open output file for writing: " << output_file_name;
      return false;
    }
  }

  static bool constexpr kOutputResults = true;

  int counter = 0;
  compiled_events
    // .as_blocking()
    .subscribe([&](CompilerPageCacheEvent event) {
      if (kOutputResults) {
        if (output_file_name.empty()) {
          LOG(INFO) << "CompilerPageCacheEvent" << event << std::endl;
        } else {
          ofs << event << "\n";  // TODO: write in proto format instead.
        }
      }
      ++counter;
    });

  LOG(DEBUG) << "compiler: Compilation completed (" << counter << " events).";

  return true;
}

}  // namespace iorap::compiler