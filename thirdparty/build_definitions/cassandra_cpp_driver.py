#
# Copyright (c) YugaByte, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
# in compliance with the License. You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software distributed under the License
# is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
# or implied. See the License for the specific language governing permissions and limitations
# under the License.
#

import os
import sys

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from build_definitions import *

# C++ Cassandra driver
class CassandraCppDriverDependency(Dependency):
    def __init__(self):
        super(CassandraCppDriverDependency, self).__init__(
                'cassandra-cpp-driver', '2.9.0-yb-1',
                'https://github.com/YugaByte/cassandra-cpp-driver/archive/{0}.tar.gz',
                BUILD_GROUP_COMMON)
        self.copy_sources = False
        self.patch_version = 0
        self.patch_strip = 1

    def build(self, builder):
        builder.build_with_cmake(self,
                                 ['-DCMAKE_BUILD_TYPE=Release',
                                  '-DCMAKE_POSITION_INDEPENDENT_CODE=On',
                                  '-DCMAKE_INSTALL_PREFIX={}'.format(builder.prefix),
                                  '-DBUILD_SHARED_LIBS=On'])
