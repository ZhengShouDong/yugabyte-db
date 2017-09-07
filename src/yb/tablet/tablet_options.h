// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
#ifndef YB_TABLET_TABLET_OPTIONS_H
#define YB_TABLET_TABLET_OPTIONS_H

namespace rocksdb {
class EventListener;
}

namespace yb {
namespace tablet {

struct TabletOptions {
  std::shared_ptr<rocksdb::Cache> block_cache;
  std::shared_ptr<rocksdb::MemoryMonitor> memory_monitor;
  std::vector<std::shared_ptr<rocksdb::EventListener>> listeners;
};

} // namespace tablet
} // namespace yb
#endif // YB_TABLET_TABLET_OPTIONS_H