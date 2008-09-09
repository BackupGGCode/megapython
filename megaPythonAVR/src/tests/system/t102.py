# PyMite - A flyweight Python interpreter for 8-bit microcontrollers and more.
# Copyright 2002 Dean Hall
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
#

#
# System Test 102
#
# Regression test for issue #102:
# Implement the remaining IMPORT_ bytecodes
#

# Test IMPORT_FROM
from sys import maxint
assert maxint == 0x7fffffff
print "maxint == 0x7fffffff"

# Test IMPORT_STAR
from sys import *
print "TODO: This line prevents a segfault. If it's gone, this fails.  Why?"
assert type(heap) == 0x08
assert type(exit) == 0x08
