/*
 * Copyright (c) 2001, 2004, Oracle and/or its affiliates. All rights reserved.
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

package sun.jvm.hotspot.interpreter;

import sun.jvm.hotspot.oops.*;
import sun.jvm.hotspot.utilities.*;

public class Bytecode {
  Method method;
  int bci;
  static final int jintSize = 4;
  static final String spaces = " ";
  static final String comma  = ", ";

  Bytecode(Method method, int bci) {
    this.method = method;
    this.bci    = bci;
  }

  // Address computation
  // NOTE: assumes that the start of the method's bytecodes is 4-byte aligned
  int alignedOffset(int offset) {
    return Bits.roundTo(bci + offset, jintSize) - bci;
  }

  int javaSignedWordAt(int offset) {
    return method.getBytecodeIntArg(bci + offset);
  }

  short javaShortAt(int offset) {
    return method.getBytecodeShortArg(bci + offset);
  }

  byte javaByteAt(int offset) {
    return method.getBytecodeByteArg(bci + offset);
  }

  public Method method() { return method; }
  public int    bci()    { return bci;    }

  // hotspot byte code
  public int code() {
    return Bytecodes.codeAt(method(), bci());
  }

  // jvm byte code
  public int javaCode() {
    return Bytecodes.javaCode(code());
  }

  public String getBytecodeName() {
    return Bytecodes.name(code());
  }

  public String getJavaBytecodeName() {
    return Bytecodes.name(javaCode());
  }

  public int getLength() {
    return Bytecodes.lengthAt(method(), bci());
  }

  public int getJavaLength() {
    return Bytecodes.javaLengthAt(method(), bci());
  }

  public String toString() {
    StringBuffer buf = new StringBuffer(getJavaBytecodeName());
    if (code() != javaCode()) {
       buf.append(spaces);
       buf.append('[');
       buf.append(getBytecodeName());
       buf.append(']');
    }
    return buf.toString();
  }
}
