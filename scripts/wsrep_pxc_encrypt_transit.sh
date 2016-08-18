#!/bin/bash -ue
# Copyright (C) 2016 Percona Inc
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING. If not, write to the
# Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston
# MA  02110-1301  USA.

# Documentation: http://www.percona.com/doc/percona-xtradb-cluster/manual/xtrabackup_sst.html
# Make sure to read that before proceeding!


#-------------------------------------------------------------------------------
#
# Step-1: Parse and read input params arguments. These are mainly
# related to role/username/password/etc...
#
. $(dirname $0)/wsrep_sst_common

#-------------------------------------------------------------------------------
#
# Step-2: Setup default global variable that we plan to use during processing.
#


#-------------------------------------------------------------------------------
#
# Step-3: Function definitions
#


#-------------------------------------------------------------------------------
#
# Step-4: Main program execution
#

wsrep_check_programs rsync

# Return the path

# Determine the directory
# Extract to the directory

echo "success:/etc/mysql/.ssl/"

exit 0
