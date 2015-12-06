/*
 * Copyright 2012, 2015 SAP AG. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

// This is only a stub. Will flesh out later when/if we add further support
// for PASE.

#include "libo4.hpp"

bool libo4::init() { return false; }
void libo4::cleanup() {}
bool libo4::get_memory_info (unsigned long long* p_virt_total, unsigned long long* p_real_total,
  unsigned long long* p_real_free, unsigned long long* p_pgsp_total, unsigned long long* p_pgsp_free) {
  return false;
}
bool libo4::get_load_avg (double* p_avg1, double* p_avg5, double* p_avg15) { return false; }
bool libo4::realpath (const char* file_name, char* resolved_name, int resolved_name_len) { return false; }

