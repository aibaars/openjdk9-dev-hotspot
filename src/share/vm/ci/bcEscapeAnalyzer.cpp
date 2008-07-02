/*
 * Copyright 2005-2008 Sun Microsystems, Inc.  All Rights Reserved.
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
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 */


#include "incls/_precompiled.incl"
#include "incls/_bcEscapeAnalyzer.cpp.incl"


#ifndef PRODUCT
  #define TRACE_BCEA(level, code)                                            \
    if (EstimateArgEscape && BCEATraceLevel >= level) {                        \
      code;                                                                  \
    }
#else
  #define TRACE_BCEA(level, code)
#endif

// Maintain a map of which aguments a local variable or
// stack slot may contain.  In addition to tracking
// arguments, it tracks two special values, "allocated"
// which represents any object allocated in the current
// method, and "unknown" which is any other object.
// Up to 30 arguments are handled, with the last one
// representing summary information for any extra arguments
class BCEscapeAnalyzer::ArgumentMap {
  uint  _bits;
  enum {MAXBIT = 29,
        ALLOCATED = 1,
        UNKNOWN = 2};

  uint int_to_bit(uint e) const {
    if (e > MAXBIT)
      e = MAXBIT;
    return (1 << (e + 2));
  }

public:
  ArgumentMap()                         { _bits = 0;}
  void set_bits(uint bits)              { _bits = bits;}
  uint get_bits() const                 { return _bits;}
  void clear()                          { _bits = 0;}
  void set_all()                        { _bits = ~0u; }
  bool is_empty() const                 { return _bits == 0; }
  bool contains(uint var) const         { return (_bits & int_to_bit(var)) != 0; }
  bool is_singleton(uint var) const     { return (_bits == int_to_bit(var)); }
  bool contains_unknown() const         { return (_bits & UNKNOWN) != 0; }
  bool contains_allocated() const       { return (_bits & ALLOCATED) != 0; }
  bool contains_vars() const            { return (_bits & (((1 << MAXBIT) -1) << 2)) != 0; }
  void set(uint var)                    { _bits = int_to_bit(var); }
  void add(uint var)                    { _bits |= int_to_bit(var); }
  void add_unknown()                    { _bits = UNKNOWN; }
  void add_allocated()                  { _bits = ALLOCATED; }
  void set_union(const ArgumentMap &am)     { _bits |= am._bits; }
  void set_intersect(const ArgumentMap &am) { _bits |= am._bits; }
  void set_difference(const ArgumentMap &am) { _bits &=  ~am._bits; }
  void operator=(const ArgumentMap &am) { _bits = am._bits; }
  bool operator==(const ArgumentMap &am) { return _bits == am._bits; }
  bool operator!=(const ArgumentMap &am) { return _bits != am._bits; }
};

class BCEscapeAnalyzer::StateInfo {
public:
  ArgumentMap *_vars;
  ArgumentMap *_stack;
  short _stack_height;
  short _max_stack;
  bool _initialized;
  ArgumentMap empty_map;

  StateInfo() {
    empty_map.clear();
  }

  ArgumentMap raw_pop()  { assert(_stack_height > 0, "stack underflow"); return _stack[--_stack_height]; }
  ArgumentMap  apop()    { return raw_pop(); }
  void spop()            { raw_pop(); }
  void lpop()            { spop(); spop(); }
  void raw_push(ArgumentMap i)   { assert(_stack_height < _max_stack, "stack overflow"); _stack[_stack_height++] = i; }
  void apush(ArgumentMap i)      { raw_push(i); }
  void spush()           { raw_push(empty_map); }
  void lpush()           { spush(); spush(); }

};

void BCEscapeAnalyzer::set_returned(ArgumentMap vars) {
  for (int i = 0; i < _arg_size; i++) {
    if (vars.contains(i))
      _arg_returned.set_bit(i);
  }
  _return_local = _return_local && !(vars.contains_unknown() || vars.contains_allocated());
  _return_allocated = _return_allocated && vars.contains_allocated() && !(vars.contains_unknown() || vars.contains_vars());
}

// return true if any element of vars is an argument
bool BCEscapeAnalyzer::is_argument(ArgumentMap vars) {
  for (int i = 0; i < _arg_size; i++) {
    if (vars.contains(i))
      return true;
  }
  return false;
}

// return true if any element of vars is an arg_stack argument
bool BCEscapeAnalyzer::is_arg_stack(ArgumentMap vars){
  if (_conservative)
    return true;
  for (int i = 0; i < _arg_size; i++) {
    if (vars.contains(i) && _arg_stack.at(i))
      return true;
  }
  return false;
}

void BCEscapeAnalyzer::clear_bits(ArgumentMap vars, BitMap &bm) {
  for (int i = 0; i < _arg_size; i++) {
    if (vars.contains(i)) {
      bm.clear_bit(i);
    }
  }
}

void BCEscapeAnalyzer::set_method_escape(ArgumentMap vars) {
  clear_bits(vars, _arg_local);
}

void BCEscapeAnalyzer::set_global_escape(ArgumentMap vars) {
  clear_bits(vars, _arg_local);
  clear_bits(vars, _arg_stack);
  if (vars.contains_allocated())
    _allocated_escapes = true;
}

void BCEscapeAnalyzer::set_dirty(ArgumentMap vars) {
  clear_bits(vars, _dirty);
}

void BCEscapeAnalyzer::set_modified(ArgumentMap vars, int offs, int size) {

  for (int i = 0; i < _arg_size; i++) {
    if (vars.contains(i)) {
      set_arg_modified(i, offs, size);
    }
  }
  if (vars.contains_unknown())
    _unknown_modified = true;
}

bool BCEscapeAnalyzer::is_recursive_call(ciMethod* callee) {
  for (BCEscapeAnalyzer* scope = this; scope != NULL; scope = scope->_parent) {
    if (scope->method() == callee) {
      return true;
    }
  }
  return false;
}

bool BCEscapeAnalyzer::is_arg_modified(int arg, int offset, int size_in_bytes) {
  if (offset == OFFSET_ANY)
    return _arg_modified[arg] != 0;
  assert(arg >= 0 && arg < _arg_size, "must be an argument.");
  bool modified = false;
  int l = offset / HeapWordSize;
  int h = round_to(offset + size_in_bytes, HeapWordSize) / HeapWordSize;
  if (l > ARG_OFFSET_MAX)
    l = ARG_OFFSET_MAX;
  if (h > ARG_OFFSET_MAX+1)
    h = ARG_OFFSET_MAX + 1;
  for (int i = l; i < h; i++) {
    modified = modified || (_arg_modified[arg] & (1 << i)) != 0;
  }
  return modified;
}

void BCEscapeAnalyzer::set_arg_modified(int arg, int offset, int size_in_bytes) {
  if (offset == OFFSET_ANY) {
    _arg_modified[arg] =  (uint) -1;
    return;
  }
  assert(arg >= 0 && arg < _arg_size, "must be an argument.");
  int l = offset / HeapWordSize;
  int h = round_to(offset + size_in_bytes, HeapWordSize) / HeapWordSize;
  if (l > ARG_OFFSET_MAX)
    l = ARG_OFFSET_MAX;
  if (h > ARG_OFFSET_MAX+1)
    h = ARG_OFFSET_MAX + 1;
  for (int i = l; i < h; i++) {
    _arg_modified[arg] |= (1 << i);
  }
}

void BCEscapeAnalyzer::invoke(StateInfo &state, Bytecodes::Code code, ciMethod* target, ciKlass* holder) {
  int i;

  // retrieve information about the callee
  ciInstanceKlass* klass = target->holder();
  ciInstanceKlass* calling_klass = method()->holder();
  ciInstanceKlass* callee_holder = ciEnv::get_instance_klass_for_declared_method_holder(holder);
  ciInstanceKlass* actual_recv = callee_holder;

  // some methods are obviously bindable without any type checks so
  // convert them directly to an invokespecial.
  if (target->is_loaded() && !target->is_abstract() &&
      target->can_be_statically_bound() && code == Bytecodes::_invokevirtual) {
    code = Bytecodes::_invokespecial;
  }

  // compute size of arguments
  int arg_size = target->arg_size();
  if (!target->is_loaded() && code == Bytecodes::_invokestatic) {
    arg_size--;
  }
  int arg_base = MAX2(state._stack_height - arg_size, 0);

  // direct recursive calls are skipped if they can be bound statically without introducing
  // dependencies and if parameters are passed at the same position as in the current method
  // other calls are skipped if there are no unescaped arguments passed to them
  bool directly_recursive = (method() == target) &&
               (code != Bytecodes::_invokevirtual || target->is_final_method() || state._stack[arg_base] .is_empty());

  // check if analysis of callee can safely be skipped
  bool skip_callee = true;
  for (i = state._stack_height - 1; i >= arg_base && skip_callee; i--) {
    ArgumentMap arg = state._stack[i];
    skip_callee = !is_argument(arg) || !is_arg_stack(arg) || (directly_recursive && arg.is_singleton(i - arg_base));
  }
  if (skip_callee) {
    TRACE_BCEA(3, tty->print_cr("[EA] skipping method %s::%s", holder->name()->as_utf8(), target->name()->as_utf8()));
    for (i = 0; i < arg_size; i++) {
      set_method_escape(state.raw_pop());
    }
    _unknown_modified = true;  // assume the worst since we don't analyze the called method
    return;
  }

  // determine actual method (use CHA if necessary)
  ciMethod* inline_target = NULL;
  if (target->is_loaded() && klass->is_loaded()
      && (klass->is_initialized() || klass->is_interface() && target->holder()->is_initialized())
      && target->will_link(klass, callee_holder, code)) {
    if (code == Bytecodes::_invokestatic
        || code == Bytecodes::_invokespecial
        || code == Bytecodes::_invokevirtual && target->is_final_method()) {
      inline_target = target;
    } else {
      inline_target = target->find_monomorphic_target(calling_klass, callee_holder, actual_recv);
    }
  }

  if (inline_target != NULL && !is_recursive_call(inline_target)) {
    // analyze callee
    BCEscapeAnalyzer analyzer(inline_target, this);

    // adjust escape state of actual parameters
    bool must_record_dependencies = false;
    for (i = arg_size - 1; i >= 0; i--) {
      ArgumentMap arg = state.raw_pop();
      if (!is_argument(arg))
        continue;
      for (int j = 0; j < _arg_size; j++) {
        if (arg.contains(j)) {
          _arg_modified[j] |= analyzer._arg_modified[i];
        }
      }
      if (!is_arg_stack(arg)) {
        // arguments have already been recognized as escaping
      } else if (analyzer.is_arg_stack(i) && !analyzer.is_arg_returned(i)) {
        set_method_escape(arg);
        must_record_dependencies = true;
      } else {
        set_global_escape(arg);
      }
    }
    _unknown_modified = _unknown_modified || analyzer.has_non_arg_side_affects();

    // record dependencies if at least one parameter retained stack-allocatable
    if (must_record_dependencies) {
      if (code == Bytecodes::_invokeinterface || code == Bytecodes::_invokevirtual && !target->is_final_method()) {
        _dependencies.append(actual_recv);
        _dependencies.append(inline_target);
      }
      _dependencies.appendAll(analyzer.dependencies());
    }
  } else {
    TRACE_BCEA(1, tty->print_cr("[EA] virtual method %s is not monomorphic.",
                                target->name()->as_utf8()));
    // conservatively mark all actual parameters as escaping globally
    for (i = 0; i < arg_size; i++) {
      ArgumentMap arg = state.raw_pop();
      if (!is_argument(arg))
        continue;
      set_modified(arg, OFFSET_ANY, type2size[T_INT]*HeapWordSize);
      set_global_escape(arg);
    }
    _unknown_modified = true;  // assume the worst since we don't know the called method
  }
}

bool BCEscapeAnalyzer::contains(uint arg_set1, uint arg_set2) {
  return ((~arg_set1) | arg_set2) == 0;
}


void BCEscapeAnalyzer::iterate_one_block(ciBlock *blk, StateInfo &state, GrowableArray<ciBlock *> &successors) {

  blk->set_processed();
  ciBytecodeStream s(method());
  int limit_bci = blk->limit_bci();
  bool fall_through = false;
  ArgumentMap allocated_obj;
  allocated_obj.add_allocated();
  ArgumentMap unknown_obj;
  unknown_obj.add_unknown();
  ArgumentMap empty_map;

  s.reset_to_bci(blk->start_bci());
  while (s.next() != ciBytecodeStream::EOBC() && s.cur_bci() < limit_bci) {
    fall_through = true;
    switch (s.cur_bc()) {
      case Bytecodes::_nop:
        break;
      case Bytecodes::_aconst_null:
        state.apush(empty_map);
        break;
      case Bytecodes::_iconst_m1:
      case Bytecodes::_iconst_0:
      case Bytecodes::_iconst_1:
      case Bytecodes::_iconst_2:
      case Bytecodes::_iconst_3:
      case Bytecodes::_iconst_4:
      case Bytecodes::_iconst_5:
      case Bytecodes::_fconst_0:
      case Bytecodes::_fconst_1:
      case Bytecodes::_fconst_2:
      case Bytecodes::_bipush:
      case Bytecodes::_sipush:
        state.spush();
        break;
      case Bytecodes::_lconst_0:
      case Bytecodes::_lconst_1:
      case Bytecodes::_dconst_0:
      case Bytecodes::_dconst_1:
        state.lpush();
        break;
      case Bytecodes::_ldc:
      case Bytecodes::_ldc_w:
      case Bytecodes::_ldc2_w:
        if (type2size[s.get_constant().basic_type()] == 1) {
          state.spush();
        } else {
          state.lpush();
        }
        break;
      case Bytecodes::_aload:
        state.apush(state._vars[s.get_index()]);
        break;
      case Bytecodes::_iload:
      case Bytecodes::_fload:
      case Bytecodes::_iload_0:
      case Bytecodes::_iload_1:
      case Bytecodes::_iload_2:
      case Bytecodes::_iload_3:
      case Bytecodes::_fload_0:
      case Bytecodes::_fload_1:
      case Bytecodes::_fload_2:
      case Bytecodes::_fload_3:
        state.spush();
        break;
      case Bytecodes::_lload:
      case Bytecodes::_dload:
      case Bytecodes::_lload_0:
      case Bytecodes::_lload_1:
      case Bytecodes::_lload_2:
      case Bytecodes::_lload_3:
      case Bytecodes::_dload_0:
      case Bytecodes::_dload_1:
      case Bytecodes::_dload_2:
      case Bytecodes::_dload_3:
        state.lpush();
        break;
      case Bytecodes::_aload_0:
        state.apush(state._vars[0]);
        break;
      case Bytecodes::_aload_1:
        state.apush(state._vars[1]);
        break;
      case Bytecodes::_aload_2:
        state.apush(state._vars[2]);
        break;
      case Bytecodes::_aload_3:
        state.apush(state._vars[3]);
        break;
      case Bytecodes::_iaload:
      case Bytecodes::_faload:
      case Bytecodes::_baload:
      case Bytecodes::_caload:
      case Bytecodes::_saload:
        state.spop();
        set_method_escape(state.apop());
        state.spush();
        break;
      case Bytecodes::_laload:
      case Bytecodes::_daload:
        state.spop();
        set_method_escape(state.apop());
        state.lpush();
        break;
      case Bytecodes::_aaload:
        { state.spop();
          ArgumentMap array = state.apop();
          set_method_escape(array);
          state.apush(unknown_obj);
          set_dirty(array);
        }
        break;
      case Bytecodes::_istore:
      case Bytecodes::_fstore:
      case Bytecodes::_istore_0:
      case Bytecodes::_istore_1:
      case Bytecodes::_istore_2:
      case Bytecodes::_istore_3:
      case Bytecodes::_fstore_0:
      case Bytecodes::_fstore_1:
      case Bytecodes::_fstore_2:
      case Bytecodes::_fstore_3:
        state.spop();
        break;
      case Bytecodes::_lstore:
      case Bytecodes::_dstore:
      case Bytecodes::_lstore_0:
      case Bytecodes::_lstore_1:
      case Bytecodes::_lstore_2:
      case Bytecodes::_lstore_3:
      case Bytecodes::_dstore_0:
      case Bytecodes::_dstore_1:
      case Bytecodes::_dstore_2:
      case Bytecodes::_dstore_3:
        state.lpop();
        break;
      case Bytecodes::_astore:
        state._vars[s.get_index()] = state.apop();
        break;
      case Bytecodes::_astore_0:
        state._vars[0] = state.apop();
        break;
      case Bytecodes::_astore_1:
        state._vars[1] = state.apop();
        break;
      case Bytecodes::_astore_2:
        state._vars[2] = state.apop();
        break;
      case Bytecodes::_astore_3:
        state._vars[3] = state.apop();
        break;
      case Bytecodes::_iastore:
      case Bytecodes::_fastore:
      case Bytecodes::_bastore:
      case Bytecodes::_castore:
      case Bytecodes::_sastore:
      {
        state.spop();
        state.spop();
        ArgumentMap arr = state.apop();
        set_method_escape(arr);
        set_modified(arr, OFFSET_ANY, type2size[T_INT]*HeapWordSize);
        break;
      }
      case Bytecodes::_lastore:
      case Bytecodes::_dastore:
      {
        state.lpop();
        state.spop();
        ArgumentMap arr = state.apop();
        set_method_escape(arr);
        set_modified(arr, OFFSET_ANY, type2size[T_LONG]*HeapWordSize);
        break;
      }
      case Bytecodes::_aastore:
      {
        set_global_escape(state.apop());
        state.spop();
        ArgumentMap arr = state.apop();
        set_modified(arr, OFFSET_ANY, type2size[T_OBJECT]*HeapWordSize);
        break;
      }
      case Bytecodes::_pop:
        state.raw_pop();
        break;
      case Bytecodes::_pop2:
        state.raw_pop();
        state.raw_pop();
        break;
      case Bytecodes::_dup:
        { ArgumentMap w1 = state.raw_pop();
          state.raw_push(w1);
          state.raw_push(w1);
        }
        break;
      case Bytecodes::_dup_x1:
        { ArgumentMap w1 = state.raw_pop();
          ArgumentMap w2 = state.raw_pop();
          state.raw_push(w1);
          state.raw_push(w2);
          state.raw_push(w1);
        }
        break;
      case Bytecodes::_dup_x2:
        { ArgumentMap w1 = state.raw_pop();
          ArgumentMap w2 = state.raw_pop();
          ArgumentMap w3 = state.raw_pop();
          state.raw_push(w1);
          state.raw_push(w3);
          state.raw_push(w2);
          state.raw_push(w1);
        }
        break;
      case Bytecodes::_dup2:
        { ArgumentMap w1 = state.raw_pop();
          ArgumentMap w2 = state.raw_pop();
          state.raw_push(w2);
          state.raw_push(w1);
          state.raw_push(w2);
          state.raw_push(w1);
        }
        break;
      case Bytecodes::_dup2_x1:
        { ArgumentMap w1 = state.raw_pop();
          ArgumentMap w2 = state.raw_pop();
          ArgumentMap w3 = state.raw_pop();
          state.raw_push(w2);
          state.raw_push(w1);
          state.raw_push(w3);
          state.raw_push(w2);
          state.raw_push(w1);
        }
        break;
      case Bytecodes::_dup2_x2:
        { ArgumentMap w1 = state.raw_pop();
          ArgumentMap w2 = state.raw_pop();
          ArgumentMap w3 = state.raw_pop();
          ArgumentMap w4 = state.raw_pop();
          state.raw_push(w2);
          state.raw_push(w1);
          state.raw_push(w4);
          state.raw_push(w3);
          state.raw_push(w2);
          state.raw_push(w1);
        }
        break;
      case Bytecodes::_swap:
        { ArgumentMap w1 = state.raw_pop();
          ArgumentMap w2 = state.raw_pop();
          state.raw_push(w1);
          state.raw_push(w2);
        }
        break;
      case Bytecodes::_iadd:
      case Bytecodes::_fadd:
      case Bytecodes::_isub:
      case Bytecodes::_fsub:
      case Bytecodes::_imul:
      case Bytecodes::_fmul:
      case Bytecodes::_idiv:
      case Bytecodes::_fdiv:
      case Bytecodes::_irem:
      case Bytecodes::_frem:
      case Bytecodes::_iand:
      case Bytecodes::_ior:
      case Bytecodes::_ixor:
        state.spop();
        state.spop();
        state.spush();
        break;
      case Bytecodes::_ladd:
      case Bytecodes::_dadd:
      case Bytecodes::_lsub:
      case Bytecodes::_dsub:
      case Bytecodes::_lmul:
      case Bytecodes::_dmul:
      case Bytecodes::_ldiv:
      case Bytecodes::_ddiv:
      case Bytecodes::_lrem:
      case Bytecodes::_drem:
      case Bytecodes::_land:
      case Bytecodes::_lor:
      case Bytecodes::_lxor:
        state.lpop();
        state.lpop();
        state.lpush();
        break;
      case Bytecodes::_ishl:
      case Bytecodes::_ishr:
      case Bytecodes::_iushr:
        state.spop();
        state.spop();
        state.spush();
        break;
      case Bytecodes::_lshl:
      case Bytecodes::_lshr:
      case Bytecodes::_lushr:
        state.spop();
        state.lpop();
        state.lpush();
        break;
      case Bytecodes::_ineg:
      case Bytecodes::_fneg:
        state.spop();
        state.spush();
        break;
      case Bytecodes::_lneg:
      case Bytecodes::_dneg:
        state.lpop();
        state.lpush();
        break;
      case Bytecodes::_iinc:
        break;
      case Bytecodes::_i2l:
      case Bytecodes::_i2d:
      case Bytecodes::_f2l:
      case Bytecodes::_f2d:
        state.spop();
        state.lpush();
        break;
      case Bytecodes::_i2f:
      case Bytecodes::_f2i:
        state.spop();
        state.spush();
        break;
      case Bytecodes::_l2i:
      case Bytecodes::_l2f:
      case Bytecodes::_d2i:
      case Bytecodes::_d2f:
        state.lpop();
        state.spush();
        break;
      case Bytecodes::_l2d:
      case Bytecodes::_d2l:
        state.lpop();
        state.lpush();
        break;
      case Bytecodes::_i2b:
      case Bytecodes::_i2c:
      case Bytecodes::_i2s:
        state.spop();
        state.spush();
        break;
      case Bytecodes::_lcmp:
      case Bytecodes::_dcmpl:
      case Bytecodes::_dcmpg:
        state.lpop();
        state.lpop();
        state.spush();
        break;
      case Bytecodes::_fcmpl:
      case Bytecodes::_fcmpg:
        state.spop();
        state.spop();
        state.spush();
        break;
      case Bytecodes::_ifeq:
      case Bytecodes::_ifne:
      case Bytecodes::_iflt:
      case Bytecodes::_ifge:
      case Bytecodes::_ifgt:
      case Bytecodes::_ifle:
      {
        state.spop();
        int dest_bci = s.get_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        successors.push(_methodBlocks->block_containing(dest_bci));
        break;
      }
      case Bytecodes::_if_icmpeq:
      case Bytecodes::_if_icmpne:
      case Bytecodes::_if_icmplt:
      case Bytecodes::_if_icmpge:
      case Bytecodes::_if_icmpgt:
      case Bytecodes::_if_icmple:
      {
        state.spop();
        state.spop();
        int dest_bci = s.get_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        successors.push(_methodBlocks->block_containing(dest_bci));
        break;
      }
      case Bytecodes::_if_acmpeq:
      case Bytecodes::_if_acmpne:
      {
        set_method_escape(state.apop());
        set_method_escape(state.apop());
        int dest_bci = s.get_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        successors.push(_methodBlocks->block_containing(dest_bci));
        break;
      }
      case Bytecodes::_goto:
      {
        int dest_bci = s.get_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        successors.push(_methodBlocks->block_containing(dest_bci));
        fall_through = false;
        break;
      }
      case Bytecodes::_jsr:
      {
        int dest_bci = s.get_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        state.apush(empty_map);
        successors.push(_methodBlocks->block_containing(dest_bci));
        fall_through = false;
        break;
      }
      case Bytecodes::_ret:
        // we don't track  the destination of a "ret" instruction
        assert(s.next_bci() == limit_bci, "branch must end block");
        fall_through = false;
        break;
      case Bytecodes::_return:
        assert(s.next_bci() == limit_bci, "return must end block");
        fall_through = false;
        break;
      case Bytecodes::_tableswitch:
        {
          state.spop();
          Bytecode_tableswitch* switch_ = Bytecode_tableswitch_at(s.cur_bcp());
          int len = switch_->length();
          int dest_bci;
          for (int i = 0; i < len; i++) {
            dest_bci = s.cur_bci() + switch_->dest_offset_at(i);
            assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
            successors.push(_methodBlocks->block_containing(dest_bci));
          }
          dest_bci = s.cur_bci() + switch_->default_offset();
          assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
          successors.push(_methodBlocks->block_containing(dest_bci));
          assert(s.next_bci() == limit_bci, "branch must end block");
          fall_through = false;
          break;
        }
      case Bytecodes::_lookupswitch:
        {
          state.spop();
          Bytecode_lookupswitch* switch_ = Bytecode_lookupswitch_at(s.cur_bcp());
          int len = switch_->number_of_pairs();
          int dest_bci;
          for (int i = 0; i < len; i++) {
            dest_bci = s.cur_bci() + switch_->pair_at(i)->offset();
            assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
            successors.push(_methodBlocks->block_containing(dest_bci));
          }
          dest_bci = s.cur_bci() + switch_->default_offset();
          assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
          successors.push(_methodBlocks->block_containing(dest_bci));
          fall_through = false;
          break;
        }
      case Bytecodes::_ireturn:
      case Bytecodes::_freturn:
        state.spop();
        fall_through = false;
        break;
      case Bytecodes::_lreturn:
      case Bytecodes::_dreturn:
        state.lpop();
        fall_through = false;
        break;
      case Bytecodes::_areturn:
        set_returned(state.apop());
        fall_through = false;
        break;
      case Bytecodes::_getstatic:
      case Bytecodes::_getfield:
        { bool will_link;
          ciField* field = s.get_field(will_link);
          BasicType field_type = field->type()->basic_type();
          if (s.cur_bc() != Bytecodes::_getstatic) {
            set_method_escape(state.apop());
          }
          if (field_type == T_OBJECT || field_type == T_ARRAY) {
            state.apush(unknown_obj);
          } else if (type2size[field_type] == 1) {
            state.spush();
          } else {
            state.lpush();
          }
        }
        break;
      case Bytecodes::_putstatic:
      case Bytecodes::_putfield:
        { bool will_link;
          ciField* field = s.get_field(will_link);
          BasicType field_type = field->type()->basic_type();
          if (field_type == T_OBJECT || field_type == T_ARRAY) {
            set_global_escape(state.apop());
          } else if (type2size[field_type] == 1) {
            state.spop();
          } else {
            state.lpop();
          }
          if (s.cur_bc() != Bytecodes::_putstatic) {
            ArgumentMap p = state.apop();
            set_method_escape(p);
            set_modified(p, will_link ? field->offset() : OFFSET_ANY, type2size[field_type]*HeapWordSize);
          }
        }
        break;
      case Bytecodes::_invokevirtual:
      case Bytecodes::_invokespecial:
      case Bytecodes::_invokestatic:
      case Bytecodes::_invokeinterface:
        { bool will_link;
          ciMethod* target = s.get_method(will_link);
          ciKlass* holder = s.get_declared_method_holder();
          invoke(state, s.cur_bc(), target, holder);
          ciType* return_type = target->return_type();
          if (!return_type->is_primitive_type()) {
            state.apush(unknown_obj);
          } else if (return_type->is_one_word()) {
            state.spush();
          } else if (return_type->is_two_word()) {
            state.lpush();
          }
        }
        break;
      case Bytecodes::_xxxunusedxxx:
        ShouldNotReachHere();
        break;
      case Bytecodes::_new:
        state.apush(allocated_obj);
        break;
      case Bytecodes::_newarray:
      case Bytecodes::_anewarray:
        state.spop();
        state.apush(allocated_obj);
        break;
      case Bytecodes::_multianewarray:
        { int i = s.cur_bcp()[3];
          while (i-- > 0) state.spop();
          state.apush(allocated_obj);
        }
        break;
      case Bytecodes::_arraylength:
        set_method_escape(state.apop());
        state.spush();
        break;
      case Bytecodes::_athrow:
        set_global_escape(state.apop());
        fall_through = false;
        break;
      case Bytecodes::_checkcast:
        { ArgumentMap obj = state.apop();
          set_method_escape(obj);
          state.apush(obj);
        }
        break;
      case Bytecodes::_instanceof:
        set_method_escape(state.apop());
        state.spush();
        break;
      case Bytecodes::_monitorenter:
      case Bytecodes::_monitorexit:
        state.apop();
        break;
      case Bytecodes::_wide:
        ShouldNotReachHere();
        break;
      case Bytecodes::_ifnull:
      case Bytecodes::_ifnonnull:
      {
        set_method_escape(state.apop());
        int dest_bci = s.get_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        successors.push(_methodBlocks->block_containing(dest_bci));
        break;
      }
      case Bytecodes::_goto_w:
      {
        int dest_bci = s.get_far_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        successors.push(_methodBlocks->block_containing(dest_bci));
        fall_through = false;
        break;
      }
      case Bytecodes::_jsr_w:
      {
        int dest_bci = s.get_far_dest();
        assert(_methodBlocks->is_block_start(dest_bci), "branch destination must start a block");
        assert(s.next_bci() == limit_bci, "branch must end block");
        state.apush(empty_map);
        successors.push(_methodBlocks->block_containing(dest_bci));
        fall_through = false;
        break;
      }
      case Bytecodes::_breakpoint:
        break;
      default:
        ShouldNotReachHere();
        break;
    }

  }
  if (fall_through) {
    int fall_through_bci = s.cur_bci();
    if (fall_through_bci < _method->code_size()) {
      assert(_methodBlocks->is_block_start(fall_through_bci), "must fall through to block start.");
      successors.push(_methodBlocks->block_containing(fall_through_bci));
    }
  }
}

void BCEscapeAnalyzer::merge_block_states(StateInfo *blockstates, ciBlock *dest, StateInfo *s_state) {
  StateInfo *d_state = blockstates + dest->index();
  int nlocals = _method->max_locals();

  // exceptions may cause transfer of control to handlers in the middle of a
  // block, so we don't merge the incoming state of exception handlers
  if (dest->is_handler())
    return;
  if (!d_state->_initialized ) {
    // destination not initialized, just copy
    for (int i = 0; i < nlocals; i++) {
      d_state->_vars[i] = s_state->_vars[i];
    }
    for (int i = 0; i < s_state->_stack_height; i++) {
      d_state->_stack[i] = s_state->_stack[i];
    }
    d_state->_stack_height = s_state->_stack_height;
    d_state->_max_stack = s_state->_max_stack;
    d_state->_initialized = true;
  } else if (!dest->processed()) {
    // we have not yet walked the bytecodes of dest, we can merge
    // the states
    assert(d_state->_stack_height == s_state->_stack_height, "computed stack heights must match");
    for (int i = 0; i < nlocals; i++) {
      d_state->_vars[i].set_union(s_state->_vars[i]);
    }
    for (int i = 0; i < s_state->_stack_height; i++) {
      d_state->_stack[i].set_union(s_state->_stack[i]);
    }
  } else {
    // the bytecodes of dest have already been processed, mark any
    // arguments in the source state which are not in the dest state
    // as global escape.
    // Future refinement:  we only need to mark these variable to the
    // maximum escape of any variables in dest state
    assert(d_state->_stack_height == s_state->_stack_height, "computed stack heights must match");
    ArgumentMap extra_vars;
    for (int i = 0; i < nlocals; i++) {
      ArgumentMap t;
      t = s_state->_vars[i];
      t.set_difference(d_state->_vars[i]);
      extra_vars.set_union(t);
    }
    for (int i = 0; i < s_state->_stack_height; i++) {
      ArgumentMap t;
      //extra_vars |= !d_state->_vars[i] & s_state->_vars[i];
      t.clear();
      t = s_state->_stack[i];
      t.set_difference(d_state->_stack[i]);
      extra_vars.set_union(t);
    }
    set_global_escape(extra_vars);
  }
}

void BCEscapeAnalyzer::iterate_blocks(Arena *arena) {
  int numblocks = _methodBlocks->num_blocks();
  int stkSize   = _method->max_stack();
  int numLocals = _method->max_locals();
  StateInfo state;

  int datacount = (numblocks + 1) * (stkSize + numLocals);
  int datasize = datacount * sizeof(ArgumentMap);
  StateInfo *blockstates = (StateInfo *) arena->Amalloc(numblocks * sizeof(StateInfo));
  ArgumentMap *statedata  = (ArgumentMap *) arena->Amalloc(datasize);
  for (int i = 0; i < datacount; i++) ::new ((void*)&statedata[i]) ArgumentMap();
  ArgumentMap *dp = statedata;
  state._vars = dp;
  dp += numLocals;
  state._stack = dp;
  dp += stkSize;
  state._initialized = false;
  state._max_stack = stkSize;
  for (int i = 0; i < numblocks; i++) {
    blockstates[i]._vars = dp;
    dp += numLocals;
    blockstates[i]._stack = dp;
    dp += stkSize;
    blockstates[i]._initialized = false;
    blockstates[i]._stack_height = 0;
    blockstates[i]._max_stack  = stkSize;
  }
  GrowableArray<ciBlock *> worklist(arena, numblocks / 4, 0, NULL);
  GrowableArray<ciBlock *> successors(arena, 4, 0, NULL);

  _methodBlocks->clear_processed();

  // initialize block 0 state from method signature
  ArgumentMap allVars;   // all oop arguments to method
  ciSignature* sig = method()->signature();
  int j = 0;
  ciBlock* first_blk = _methodBlocks->block_containing(0);
  int fb_i = first_blk->index();
  if (!method()->is_static()) {
    // record information for "this"
    blockstates[fb_i]._vars[j].set(j);
    allVars.add(j);
    j++;
  }
  for (int i = 0; i < sig->count(); i++) {
    ciType* t = sig->type_at(i);
    if (!t->is_primitive_type()) {
      blockstates[fb_i]._vars[j].set(j);
      allVars.add(j);
    }
    j += t->size();
  }
  blockstates[fb_i]._initialized = true;
  assert(j == _arg_size, "just checking");

  ArgumentMap unknown_map;
  unknown_map.add_unknown();

  worklist.push(first_blk);
  while(worklist.length() > 0) {
    ciBlock *blk = worklist.pop();
    StateInfo *blkState = blockstates + blk->index();
    if (blk->is_handler() || blk->is_ret_target()) {
      // for an exception handler or a target of a ret instruction, we assume the worst case,
      // that any variable could contain any argument
      for (int i = 0; i < numLocals; i++) {
        state._vars[i] = allVars;
      }
      if (blk->is_handler()) {
        state._stack_height = 1;
      } else {
        state._stack_height = blkState->_stack_height;
      }
      for (int i = 0; i < state._stack_height; i++) {
// ??? should this be unknown_map ???
        state._stack[i] = allVars;
      }
    } else {
      for (int i = 0; i < numLocals; i++) {
        state._vars[i] = blkState->_vars[i];
      }
      for (int i = 0; i < blkState->_stack_height; i++) {
        state._stack[i] = blkState->_stack[i];
      }
      state._stack_height = blkState->_stack_height;
    }
    iterate_one_block(blk, state, successors);
    // if this block has any exception handlers, push them
    // onto successor list
    if (blk->has_handler()) {
      DEBUG_ONLY(int handler_count = 0;)
      int blk_start = blk->start_bci();
      int blk_end = blk->limit_bci();
      for (int i = 0; i < numblocks; i++) {
        ciBlock *b = _methodBlocks->block(i);
        if (b->is_handler()) {
          int ex_start = b->ex_start_bci();
          int ex_end = b->ex_limit_bci();
          if ((ex_start >= blk_start && ex_start < blk_end) ||
              (ex_end > blk_start && ex_end <= blk_end)) {
            successors.push(b);
          }
          DEBUG_ONLY(handler_count++;)
        }
      }
      assert(handler_count > 0, "must find at least one handler");
    }
    // merge computed variable state with successors
    while(successors.length() > 0) {
      ciBlock *succ = successors.pop();
      merge_block_states(blockstates, succ, &state);
      if (!succ->processed())
        worklist.push(succ);
    }
  }
}

bool BCEscapeAnalyzer::do_analysis() {
  Arena* arena = CURRENT_ENV->arena();
  // identify basic blocks
  _methodBlocks = _method->get_method_blocks();

  iterate_blocks(arena);
  // TEMPORARY
  return true;
}

vmIntrinsics::ID BCEscapeAnalyzer::known_intrinsic() {
  vmIntrinsics::ID iid = method()->intrinsic_id();

  if (iid == vmIntrinsics::_getClass ||
      iid ==  vmIntrinsics::_fillInStackTrace ||
      iid == vmIntrinsics::_hashCode)
    return iid;
  else
    return vmIntrinsics::_none;
}

bool BCEscapeAnalyzer::compute_escape_for_intrinsic(vmIntrinsics::ID iid) {
  ArgumentMap arg;
  arg.clear();
  switch (iid) {
  case vmIntrinsics::_getClass:
    _return_local = false;
    break;
  case vmIntrinsics::_fillInStackTrace:
    arg.set(0); // 'this'
    set_returned(arg);
    break;
  case vmIntrinsics::_hashCode:
    // initialized state is correct
    break;
  default:
    assert(false, "unexpected intrinsic");
  }
  return true;
}

void BCEscapeAnalyzer::initialize() {
  int i;

  // clear escape information (method may have been deoptimized)
  methodData()->clear_escape_info();

  // initialize escape state of object parameters
  ciSignature* sig = method()->signature();
  int j = 0;
  if (!method()->is_static()) {
    _arg_local.set_bit(0);
    _arg_stack.set_bit(0);
    j++;
  }
  for (i = 0; i < sig->count(); i++) {
    ciType* t = sig->type_at(i);
    if (!t->is_primitive_type()) {
      _arg_local.set_bit(j);
      _arg_stack.set_bit(j);
    }
    j += t->size();
  }
  assert(j == _arg_size, "just checking");

  // start with optimistic assumption
  ciType *rt = _method->return_type();
  if (rt->is_primitive_type()) {
    _return_local = false;
    _return_allocated = false;
  } else {
    _return_local = true;
    _return_allocated = true;
  }
  _allocated_escapes = false;
  _unknown_modified = false;
}

void BCEscapeAnalyzer::clear_escape_info() {
  ciSignature* sig = method()->signature();
  int arg_count = sig->count();
  ArgumentMap var;
  if (!method()->is_static()) {
    arg_count++;  // allow for "this"
  }
  for (int i = 0; i < arg_count; i++) {
    set_arg_modified(i, OFFSET_ANY, 4);
    var.clear();
    var.set(i);
    set_modified(var, OFFSET_ANY, 4);
    set_global_escape(var);
  }
  _arg_local.clear();
  _arg_stack.clear();
  _arg_returned.clear();
  _return_local = false;
  _return_allocated = false;
  _allocated_escapes = true;
  _unknown_modified = true;
}


void BCEscapeAnalyzer::compute_escape_info() {
  int i;
  assert(!methodData()->has_escape_info(), "do not overwrite escape info");

  vmIntrinsics::ID iid = known_intrinsic();

  // check if method can be analyzed
  if (iid ==  vmIntrinsics::_none && (method()->is_abstract() || method()->is_native() || !method()->holder()->is_initialized()
      || _level > MaxBCEAEstimateLevel
      || method()->code_size() > MaxBCEAEstimateSize)) {
    if (BCEATraceLevel >= 1) {
      tty->print("Skipping method because: ");
      if (method()->is_abstract())
        tty->print_cr("method is abstract.");
      else if (method()->is_native())
        tty->print_cr("method is native.");
      else if (!method()->holder()->is_initialized())
        tty->print_cr("class of method is not initialized.");
      else if (_level > MaxBCEAEstimateLevel)
        tty->print_cr("level (%d) exceeds MaxBCEAEstimateLevel (%d).",
                      _level, MaxBCEAEstimateLevel);
      else if (method()->code_size() > MaxBCEAEstimateSize)
        tty->print_cr("code size (%d) exceeds MaxBCEAEstimateSize.",
                      method()->code_size(), MaxBCEAEstimateSize);
      else
        ShouldNotReachHere();
    }
    clear_escape_info();

    return;
  }

  if (BCEATraceLevel >= 1) {
    tty->print("[EA] estimating escape information for");
    if (iid != vmIntrinsics::_none)
      tty->print(" intrinsic");
    method()->print_short_name();
    tty->print_cr(" (%d bytes)", method()->code_size());
  }

  bool success;

  initialize();

  // Do not scan method if it has no object parameters and
  // does not returns an object (_return_allocated is set in initialize()).
  if (_arg_local.is_empty() && !_return_allocated) {
    // Clear all info since method's bytecode was not analysed and
    // set pessimistic escape information.
    clear_escape_info();
    methodData()->set_eflag(methodDataOopDesc::allocated_escapes);
    methodData()->set_eflag(methodDataOopDesc::unknown_modified);
    methodData()->set_eflag(methodDataOopDesc::estimated);
    return;
  }

  if (iid != vmIntrinsics::_none)
    success = compute_escape_for_intrinsic(iid);
  else {
    success = do_analysis();
  }

  // don't store interprocedural escape information if it introduces
  // dependencies or if method data is empty
  //
  if (!has_dependencies() && !methodData()->is_empty()) {
    for (i = 0; i < _arg_size; i++) {
      if (_arg_local.at(i)) {
        assert(_arg_stack.at(i), "inconsistent escape info");
        methodData()->set_arg_local(i);
        methodData()->set_arg_stack(i);
      } else if (_arg_stack.at(i)) {
        methodData()->set_arg_stack(i);
      }
      if (_arg_returned.at(i)) {
        methodData()->set_arg_returned(i);
      }
      methodData()->set_arg_modified(i, _arg_modified[i]);
    }
    if (_return_local) {
      methodData()->set_eflag(methodDataOopDesc::return_local);
    }
    if (_return_allocated) {
      methodData()->set_eflag(methodDataOopDesc::return_allocated);
    }
    if (_allocated_escapes) {
      methodData()->set_eflag(methodDataOopDesc::allocated_escapes);
    }
    if (_unknown_modified) {
      methodData()->set_eflag(methodDataOopDesc::unknown_modified);
    }
    methodData()->set_eflag(methodDataOopDesc::estimated);
  }
}

void BCEscapeAnalyzer::read_escape_info() {
  assert(methodData()->has_escape_info(), "no escape info available");

  // read escape information from method descriptor
  for (int i = 0; i < _arg_size; i++) {
    _arg_local.at_put(i, methodData()->is_arg_local(i));
    _arg_stack.at_put(i, methodData()->is_arg_stack(i));
    _arg_returned.at_put(i, methodData()->is_arg_returned(i));
    _arg_modified[i] = methodData()->arg_modified(i);
  }
  _return_local = methodData()->eflag_set(methodDataOopDesc::return_local);
  _return_allocated = methodData()->eflag_set(methodDataOopDesc::return_allocated);
  _allocated_escapes = methodData()->eflag_set(methodDataOopDesc::allocated_escapes);
  _unknown_modified = methodData()->eflag_set(methodDataOopDesc::unknown_modified);

}

#ifndef PRODUCT
void BCEscapeAnalyzer::dump() {
  tty->print("[EA] estimated escape information for");
  method()->print_short_name();
  tty->print_cr(has_dependencies() ? " (not stored)" : "");
  tty->print("     non-escaping args:      ");
  _arg_local.print_on(tty);
  tty->print("     stack-allocatable args: ");
  _arg_stack.print_on(tty);
  if (_return_local) {
    tty->print("     returned args:          ");
    _arg_returned.print_on(tty);
  } else if (is_return_allocated()) {
    tty->print_cr("     return allocated value");
  } else {
    tty->print_cr("     return non-local value");
  }
  tty->print("     modified args: ");
  for (int i = 0; i < _arg_size; i++) {
    if (_arg_modified[i] == 0)
      tty->print("    0");
    else
      tty->print("    0x%x", _arg_modified[i]);
  }
  tty->cr();
  tty->print("     flags: ");
  if (_return_allocated)
    tty->print(" return_allocated");
  if (_allocated_escapes)
    tty->print(" allocated_escapes");
  if (_unknown_modified)
    tty->print(" unknown_modified");
  tty->cr();
}
#endif

BCEscapeAnalyzer::BCEscapeAnalyzer(ciMethod* method, BCEscapeAnalyzer* parent)
    : _conservative(method == NULL || !EstimateArgEscape)
    , _method(method)
    , _methodData(method ? method->method_data() : NULL)
    , _arg_size(method ? method->arg_size() : 0)
    , _stack()
    , _arg_local(_arg_size)
    , _arg_stack(_arg_size)
    , _arg_returned(_arg_size)
    , _dirty(_arg_size)
    , _return_local(false)
    , _return_allocated(false)
    , _allocated_escapes(false)
    , _unknown_modified(false)
    , _dependencies()
    , _parent(parent)
    , _level(parent == NULL ? 0 : parent->level() + 1) {
  if (!_conservative) {
    _arg_local.clear();
    _arg_stack.clear();
    _arg_returned.clear();
    _dirty.clear();
    Arena* arena = CURRENT_ENV->arena();
    _arg_modified = (uint *) arena->Amalloc(_arg_size * sizeof(uint));
    Copy::zero_to_bytes(_arg_modified, _arg_size * sizeof(uint));

    if (methodData() == NULL)
      return;
    bool printit = _method->should_print_assembly();
    if (methodData()->has_escape_info()) {
      TRACE_BCEA(2, tty->print_cr("[EA] Reading previous results for %s.%s",
                                  method->holder()->name()->as_utf8(),
                                  method->name()->as_utf8()));
      read_escape_info();
    } else {
      TRACE_BCEA(2, tty->print_cr("[EA] computing results for %s.%s",
                                  method->holder()->name()->as_utf8(),
                                  method->name()->as_utf8()));

      compute_escape_info();
      methodData()->update_escape_info();
    }
#ifndef PRODUCT
    if (BCEATraceLevel >= 3) {
      // dump escape information
      dump();
    }
#endif
  }
}

void BCEscapeAnalyzer::copy_dependencies(Dependencies *deps) {
  if(!has_dependencies())
    return;
  for (int i = 0; i < _dependencies.length(); i+=2) {
    ciKlass *k = _dependencies[i]->as_klass();
    ciMethod *m = _dependencies[i+1]->as_method();
    deps->assert_unique_concrete_method(k, m);
  }
}
