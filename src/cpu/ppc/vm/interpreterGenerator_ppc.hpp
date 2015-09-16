/*
 * Copyright (c) 2002, 2015, Oracle and/or its affiliates. All rights reserved.
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

#ifndef CPU_PPC_VM_INTERPRETERGENERATOR_PPC_HPP
#define CPU_PPC_VM_INTERPRETERGENERATOR_PPC_HPP

 friend class AbstractInterpreterGenerator;

 private:

  address generate_abstract_entry(void);
  address generate_jump_to_normal_entry(void);
  address generate_accessor_entry(void) { return generate_jump_to_normal_entry(); }
  address generate_empty_entry(void) { return generate_jump_to_normal_entry(); }
  address generate_Reference_get_entry(void);

  address generate_CRC32_update_entry();
  address generate_CRC32_updateBytes_entry(AbstractInterpreter::MethodKind kind);
  address generate_CRC32C_updateBytes_entry(AbstractInterpreter::MethodKind kind) { return NULL; }

#endif // CPU_PPC_VM_INTERPRETERGENERATOR_PPC_HPP
