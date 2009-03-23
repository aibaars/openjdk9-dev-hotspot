/*
 * Copyright 1997-2009 Sun Microsystems, Inc.  All Rights Reserved.
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
#include "incls/_matcher.cpp.incl"

OptoReg::Name OptoReg::c_frame_pointer;



const int Matcher::base2reg[Type::lastype] = {
  Node::NotAMachineReg,0,0, Op_RegI, Op_RegL, 0, Op_RegN,
  Node::NotAMachineReg, Node::NotAMachineReg, /* tuple, array */
  Op_RegP, Op_RegP, Op_RegP, Op_RegP, Op_RegP, Op_RegP, /* the pointers */
  0, 0/*abio*/,
  Op_RegP /* Return address */, 0, /* the memories */
  Op_RegF, Op_RegF, Op_RegF, Op_RegD, Op_RegD, Op_RegD,
  0  /*bottom*/
};

const RegMask *Matcher::idealreg2regmask[_last_machine_leaf];
RegMask Matcher::mreg2regmask[_last_Mach_Reg];
RegMask Matcher::STACK_ONLY_mask;
RegMask Matcher::c_frame_ptr_mask;
const uint Matcher::_begin_rematerialize = _BEGIN_REMATERIALIZE;
const uint Matcher::_end_rematerialize   = _END_REMATERIALIZE;

//---------------------------Matcher-------------------------------------------
Matcher::Matcher( Node_List &proj_list ) :
  PhaseTransform( Phase::Ins_Select ),
#ifdef ASSERT
  _old2new_map(C->comp_arena()),
  _new2old_map(C->comp_arena()),
#endif
  _shared_nodes(C->comp_arena()),
  _reduceOp(reduceOp), _leftOp(leftOp), _rightOp(rightOp),
  _swallowed(swallowed),
  _begin_inst_chain_rule(_BEGIN_INST_CHAIN_RULE),
  _end_inst_chain_rule(_END_INST_CHAIN_RULE),
  _must_clone(must_clone), _proj_list(proj_list),
  _register_save_policy(register_save_policy),
  _c_reg_save_policy(c_reg_save_policy),
  _register_save_type(register_save_type),
  _ruleName(ruleName),
  _allocation_started(false),
  _states_arena(Chunk::medium_size),
  _visited(&_states_arena),
  _shared(&_states_arena),
  _dontcare(&_states_arena) {
  C->set_matcher(this);

  idealreg2spillmask[Op_RegI] = NULL;
  idealreg2spillmask[Op_RegN] = NULL;
  idealreg2spillmask[Op_RegL] = NULL;
  idealreg2spillmask[Op_RegF] = NULL;
  idealreg2spillmask[Op_RegD] = NULL;
  idealreg2spillmask[Op_RegP] = NULL;

  idealreg2debugmask[Op_RegI] = NULL;
  idealreg2debugmask[Op_RegN] = NULL;
  idealreg2debugmask[Op_RegL] = NULL;
  idealreg2debugmask[Op_RegF] = NULL;
  idealreg2debugmask[Op_RegD] = NULL;
  idealreg2debugmask[Op_RegP] = NULL;
  debug_only(_mem_node = NULL;)   // Ideal memory node consumed by mach node
}

//------------------------------warp_incoming_stk_arg------------------------
// This warps a VMReg into an OptoReg::Name
OptoReg::Name Matcher::warp_incoming_stk_arg( VMReg reg ) {
  OptoReg::Name warped;
  if( reg->is_stack() ) {  // Stack slot argument?
    warped = OptoReg::add(_old_SP, reg->reg2stack() );
    warped = OptoReg::add(warped, C->out_preserve_stack_slots());
    if( warped >= _in_arg_limit )
      _in_arg_limit = OptoReg::add(warped, 1); // Bump max stack slot seen
    if (!RegMask::can_represent(warped)) {
      // the compiler cannot represent this method's calling sequence
      C->record_method_not_compilable_all_tiers("unsupported incoming calling sequence");
      return OptoReg::Bad;
    }
    return warped;
  }
  return OptoReg::as_OptoReg(reg);
}

//---------------------------compute_old_SP------------------------------------
OptoReg::Name Compile::compute_old_SP() {
  int fixed    = fixed_slots();
  int preserve = in_preserve_stack_slots();
  return OptoReg::stack2reg(round_to(fixed + preserve, Matcher::stack_alignment_in_slots()));
}



#ifdef ASSERT
void Matcher::verify_new_nodes_only(Node* xroot) {
  // Make sure that the new graph only references new nodes
  ResourceMark rm;
  Unique_Node_List worklist;
  VectorSet visited(Thread::current()->resource_area());
  worklist.push(xroot);
  while (worklist.size() > 0) {
    Node* n = worklist.pop();
    visited <<= n->_idx;
    assert(C->node_arena()->contains(n), "dead node");
    for (uint j = 0; j < n->req(); j++) {
      Node* in = n->in(j);
      if (in != NULL) {
        assert(C->node_arena()->contains(in), "dead node");
        if (!visited.test(in->_idx)) {
          worklist.push(in);
        }
      }
    }
  }
}
#endif


//---------------------------match---------------------------------------------
void Matcher::match( ) {
  // One-time initialization of some register masks.
  init_spill_mask( C->root()->in(1) );
  _return_addr_mask = return_addr();
#ifdef _LP64
  // Pointers take 2 slots in 64-bit land
  _return_addr_mask.Insert(OptoReg::add(return_addr(),1));
#endif

  // Map a Java-signature return type into return register-value
  // machine registers for 0, 1 and 2 returned values.
  const TypeTuple *range = C->tf()->range();
  if( range->cnt() > TypeFunc::Parms ) { // If not a void function
    // Get ideal-register return type
    int ireg = base2reg[range->field_at(TypeFunc::Parms)->base()];
    // Get machine return register
    uint sop = C->start()->Opcode();
    OptoRegPair regs = return_value(ireg, false);

    // And mask for same
    _return_value_mask = RegMask(regs.first());
    if( OptoReg::is_valid(regs.second()) )
      _return_value_mask.Insert(regs.second());
  }

  // ---------------
  // Frame Layout

  // Need the method signature to determine the incoming argument types,
  // because the types determine which registers the incoming arguments are
  // in, and this affects the matched code.
  const TypeTuple *domain = C->tf()->domain();
  uint             argcnt = domain->cnt() - TypeFunc::Parms;
  BasicType *sig_bt        = NEW_RESOURCE_ARRAY( BasicType, argcnt );
  VMRegPair *vm_parm_regs  = NEW_RESOURCE_ARRAY( VMRegPair, argcnt );
  _parm_regs               = NEW_RESOURCE_ARRAY( OptoRegPair, argcnt );
  _calling_convention_mask = NEW_RESOURCE_ARRAY( RegMask, argcnt );
  uint i;
  for( i = 0; i<argcnt; i++ ) {
    sig_bt[i] = domain->field_at(i+TypeFunc::Parms)->basic_type();
  }

  // Pass array of ideal registers and length to USER code (from the AD file)
  // that will convert this to an array of register numbers.
  const StartNode *start = C->start();
  start->calling_convention( sig_bt, vm_parm_regs, argcnt );
#ifdef ASSERT
  // Sanity check users' calling convention.  Real handy while trying to
  // get the initial port correct.
  { for (uint i = 0; i<argcnt; i++) {
      if( !vm_parm_regs[i].first()->is_valid() && !vm_parm_regs[i].second()->is_valid() ) {
        assert(domain->field_at(i+TypeFunc::Parms)==Type::HALF, "only allowed on halve" );
        _parm_regs[i].set_bad();
        continue;
      }
      VMReg parm_reg = vm_parm_regs[i].first();
      assert(parm_reg->is_valid(), "invalid arg?");
      if (parm_reg->is_reg()) {
        OptoReg::Name opto_parm_reg = OptoReg::as_OptoReg(parm_reg);
        assert(can_be_java_arg(opto_parm_reg) ||
               C->stub_function() == CAST_FROM_FN_PTR(address, OptoRuntime::rethrow_C) ||
               opto_parm_reg == inline_cache_reg(),
               "parameters in register must be preserved by runtime stubs");
      }
      for (uint j = 0; j < i; j++) {
        assert(parm_reg != vm_parm_regs[j].first(),
               "calling conv. must produce distinct regs");
      }
    }
  }
#endif

  // Do some initial frame layout.

  // Compute the old incoming SP (may be called FP) as
  //   OptoReg::stack0() + locks + in_preserve_stack_slots + pad2.
  _old_SP = C->compute_old_SP();
  assert( is_even(_old_SP), "must be even" );

  // Compute highest incoming stack argument as
  //   _old_SP + out_preserve_stack_slots + incoming argument size.
  _in_arg_limit = OptoReg::add(_old_SP, C->out_preserve_stack_slots());
  assert( is_even(_in_arg_limit), "out_preserve must be even" );
  for( i = 0; i < argcnt; i++ ) {
    // Permit args to have no register
    _calling_convention_mask[i].Clear();
    if( !vm_parm_regs[i].first()->is_valid() && !vm_parm_regs[i].second()->is_valid() ) {
      continue;
    }
    // calling_convention returns stack arguments as a count of
    // slots beyond OptoReg::stack0()/VMRegImpl::stack0.  We need to convert this to
    // the allocators point of view, taking into account all the
    // preserve area, locks & pad2.

    OptoReg::Name reg1 = warp_incoming_stk_arg(vm_parm_regs[i].first());
    if( OptoReg::is_valid(reg1))
      _calling_convention_mask[i].Insert(reg1);

    OptoReg::Name reg2 = warp_incoming_stk_arg(vm_parm_regs[i].second());
    if( OptoReg::is_valid(reg2))
      _calling_convention_mask[i].Insert(reg2);

    // Saved biased stack-slot register number
    _parm_regs[i].set_pair(reg2, reg1);
  }

  // Finally, make sure the incoming arguments take up an even number of
  // words, in case the arguments or locals need to contain doubleword stack
  // slots.  The rest of the system assumes that stack slot pairs (in
  // particular, in the spill area) which look aligned will in fact be
  // aligned relative to the stack pointer in the target machine.  Double
  // stack slots will always be allocated aligned.
  _new_SP = OptoReg::Name(round_to(_in_arg_limit, RegMask::SlotsPerLong));

  // Compute highest outgoing stack argument as
  //   _new_SP + out_preserve_stack_slots + max(outgoing argument size).
  _out_arg_limit = OptoReg::add(_new_SP, C->out_preserve_stack_slots());
  assert( is_even(_out_arg_limit), "out_preserve must be even" );

  if (!RegMask::can_represent(OptoReg::add(_out_arg_limit,-1))) {
    // the compiler cannot represent this method's calling sequence
    C->record_method_not_compilable("must be able to represent all call arguments in reg mask");
  }

  if (C->failing())  return;  // bailed out on incoming arg failure

  // ---------------
  // Collect roots of matcher trees.  Every node for which
  // _shared[_idx] is cleared is guaranteed to not be shared, and thus
  // can be a valid interior of some tree.
  find_shared( C->root() );
  find_shared( C->top() );

  C->print_method("Before Matching");

  // Swap out to old-space; emptying new-space
  Arena *old = C->node_arena()->move_contents(C->old_arena());

  // Save debug and profile information for nodes in old space:
  _old_node_note_array = C->node_note_array();
  if (_old_node_note_array != NULL) {
    C->set_node_note_array(new(C->comp_arena()) GrowableArray<Node_Notes*>
                           (C->comp_arena(), _old_node_note_array->length(),
                            0, NULL));
  }

  // Pre-size the new_node table to avoid the need for range checks.
  grow_new_node_array(C->unique());

  // Reset node counter so MachNodes start with _idx at 0
  int nodes = C->unique(); // save value
  C->set_unique(0);

  // Recursively match trees from old space into new space.
  // Correct leaves of new-space Nodes; they point to old-space.
  _visited.Clear();             // Clear visit bits for xform call
  C->set_cached_top_node(xform( C->top(), nodes ));
  if (!C->failing()) {
    Node* xroot =        xform( C->root(), 1 );
    if (xroot == NULL) {
      Matcher::soft_match_failure();  // recursive matching process failed
      C->record_method_not_compilable("instruction match failed");
    } else {
      // During matching shared constants were attached to C->root()
      // because xroot wasn't available yet, so transfer the uses to
      // the xroot.
      for( DUIterator_Fast jmax, j = C->root()->fast_outs(jmax); j < jmax; j++ ) {
        Node* n = C->root()->fast_out(j);
        if (C->node_arena()->contains(n)) {
          assert(n->in(0) == C->root(), "should be control user");
          n->set_req(0, xroot);
          --j;
          --jmax;
        }
      }

      C->set_root(xroot->is_Root() ? xroot->as_Root() : NULL);
#ifdef ASSERT
      verify_new_nodes_only(xroot);
#endif
    }
  }
  if (C->top() == NULL || C->root() == NULL) {
    C->record_method_not_compilable("graph lost"); // %%% cannot happen?
  }
  if (C->failing()) {
    // delete old;
    old->destruct_contents();
    return;
  }
  assert( C->top(), "" );
  assert( C->root(), "" );
  validate_null_checks();

  // Now smoke old-space
  NOT_DEBUG( old->destruct_contents() );

  // ------------------------
  // Set up save-on-entry registers
  Fixup_Save_On_Entry( );
}


//------------------------------Fixup_Save_On_Entry----------------------------
// The stated purpose of this routine is to take care of save-on-entry
// registers.  However, the overall goal of the Match phase is to convert into
// machine-specific instructions which have RegMasks to guide allocation.
// So what this procedure really does is put a valid RegMask on each input
// to the machine-specific variations of all Return, TailCall and Halt
// instructions.  It also adds edgs to define the save-on-entry values (and of
// course gives them a mask).

static RegMask *init_input_masks( uint size, RegMask &ret_adr, RegMask &fp ) {
  RegMask *rms = NEW_RESOURCE_ARRAY( RegMask, size );
  // Do all the pre-defined register masks
  rms[TypeFunc::Control  ] = RegMask::Empty;
  rms[TypeFunc::I_O      ] = RegMask::Empty;
  rms[TypeFunc::Memory   ] = RegMask::Empty;
  rms[TypeFunc::ReturnAdr] = ret_adr;
  rms[TypeFunc::FramePtr ] = fp;
  return rms;
}

//---------------------------init_first_stack_mask-----------------------------
// Create the initial stack mask used by values spilling to the stack.
// Disallow any debug info in outgoing argument areas by setting the
// initial mask accordingly.
void Matcher::init_first_stack_mask() {

  // Allocate storage for spill masks as masks for the appropriate load type.
  RegMask *rms = (RegMask*)C->comp_arena()->Amalloc_D(sizeof(RegMask)*12);
  idealreg2spillmask[Op_RegN] = &rms[0];
  idealreg2spillmask[Op_RegI] = &rms[1];
  idealreg2spillmask[Op_RegL] = &rms[2];
  idealreg2spillmask[Op_RegF] = &rms[3];
  idealreg2spillmask[Op_RegD] = &rms[4];
  idealreg2spillmask[Op_RegP] = &rms[5];
  idealreg2debugmask[Op_RegN] = &rms[6];
  idealreg2debugmask[Op_RegI] = &rms[7];
  idealreg2debugmask[Op_RegL] = &rms[8];
  idealreg2debugmask[Op_RegF] = &rms[9];
  idealreg2debugmask[Op_RegD] = &rms[10];
  idealreg2debugmask[Op_RegP] = &rms[11];

  OptoReg::Name i;

  // At first, start with the empty mask
  C->FIRST_STACK_mask().Clear();

  // Add in the incoming argument area
  OptoReg::Name init = OptoReg::add(_old_SP, C->out_preserve_stack_slots());
  for (i = init; i < _in_arg_limit; i = OptoReg::add(i,1))
    C->FIRST_STACK_mask().Insert(i);

  // Add in all bits past the outgoing argument area
  guarantee(RegMask::can_represent(OptoReg::add(_out_arg_limit,-1)),
            "must be able to represent all call arguments in reg mask");
  init = _out_arg_limit;
  for (i = init; RegMask::can_represent(i); i = OptoReg::add(i,1))
    C->FIRST_STACK_mask().Insert(i);

  // Finally, set the "infinite stack" bit.
  C->FIRST_STACK_mask().set_AllStack();

  // Make spill masks.  Registers for their class, plus FIRST_STACK_mask.
#ifdef _LP64
  *idealreg2spillmask[Op_RegN] = *idealreg2regmask[Op_RegN];
   idealreg2spillmask[Op_RegN]->OR(C->FIRST_STACK_mask());
#endif
  *idealreg2spillmask[Op_RegI] = *idealreg2regmask[Op_RegI];
   idealreg2spillmask[Op_RegI]->OR(C->FIRST_STACK_mask());
  *idealreg2spillmask[Op_RegL] = *idealreg2regmask[Op_RegL];
   idealreg2spillmask[Op_RegL]->OR(C->FIRST_STACK_mask());
  *idealreg2spillmask[Op_RegF] = *idealreg2regmask[Op_RegF];
   idealreg2spillmask[Op_RegF]->OR(C->FIRST_STACK_mask());
  *idealreg2spillmask[Op_RegD] = *idealreg2regmask[Op_RegD];
   idealreg2spillmask[Op_RegD]->OR(C->FIRST_STACK_mask());
  *idealreg2spillmask[Op_RegP] = *idealreg2regmask[Op_RegP];
   idealreg2spillmask[Op_RegP]->OR(C->FIRST_STACK_mask());

  // Make up debug masks.  Any spill slot plus callee-save registers.
  // Caller-save registers are assumed to be trashable by the various
  // inline-cache fixup routines.
  *idealreg2debugmask[Op_RegN]= *idealreg2spillmask[Op_RegN];
  *idealreg2debugmask[Op_RegI]= *idealreg2spillmask[Op_RegI];
  *idealreg2debugmask[Op_RegL]= *idealreg2spillmask[Op_RegL];
  *idealreg2debugmask[Op_RegF]= *idealreg2spillmask[Op_RegF];
  *idealreg2debugmask[Op_RegD]= *idealreg2spillmask[Op_RegD];
  *idealreg2debugmask[Op_RegP]= *idealreg2spillmask[Op_RegP];

  // Prevent stub compilations from attempting to reference
  // callee-saved registers from debug info
  bool exclude_soe = !Compile::current()->is_method_compilation();

  for( i=OptoReg::Name(0); i<OptoReg::Name(_last_Mach_Reg); i = OptoReg::add(i,1) ) {
    // registers the caller has to save do not work
    if( _register_save_policy[i] == 'C' ||
        _register_save_policy[i] == 'A' ||
        (_register_save_policy[i] == 'E' && exclude_soe) ) {
      idealreg2debugmask[Op_RegN]->Remove(i);
      idealreg2debugmask[Op_RegI]->Remove(i); // Exclude save-on-call
      idealreg2debugmask[Op_RegL]->Remove(i); // registers from debug
      idealreg2debugmask[Op_RegF]->Remove(i); // masks
      idealreg2debugmask[Op_RegD]->Remove(i);
      idealreg2debugmask[Op_RegP]->Remove(i);
    }
  }
}

//---------------------------is_save_on_entry----------------------------------
bool Matcher::is_save_on_entry( int reg ) {
  return
    _register_save_policy[reg] == 'E' ||
    _register_save_policy[reg] == 'A' || // Save-on-entry register?
    // Also save argument registers in the trampolining stubs
    (C->save_argument_registers() && is_spillable_arg(reg));
}

//---------------------------Fixup_Save_On_Entry-------------------------------
void Matcher::Fixup_Save_On_Entry( ) {
  init_first_stack_mask();

  Node *root = C->root();       // Short name for root
  // Count number of save-on-entry registers.
  uint soe_cnt = number_of_saved_registers();
  uint i;

  // Find the procedure Start Node
  StartNode *start = C->start();
  assert( start, "Expect a start node" );

  // Save argument registers in the trampolining stubs
  if( C->save_argument_registers() )
    for( i = 0; i < _last_Mach_Reg; i++ )
      if( is_spillable_arg(i) )
        soe_cnt++;

  // Input RegMask array shared by all Returns.
  // The type for doubles and longs has a count of 2, but
  // there is only 1 returned value
  uint ret_edge_cnt = TypeFunc::Parms + ((C->tf()->range()->cnt() == TypeFunc::Parms) ? 0 : 1);
  RegMask *ret_rms  = init_input_masks( ret_edge_cnt + soe_cnt, _return_addr_mask, c_frame_ptr_mask );
  // Returns have 0 or 1 returned values depending on call signature.
  // Return register is specified by return_value in the AD file.
  if (ret_edge_cnt > TypeFunc::Parms)
    ret_rms[TypeFunc::Parms+0] = _return_value_mask;

  // Input RegMask array shared by all Rethrows.
  uint reth_edge_cnt = TypeFunc::Parms+1;
  RegMask *reth_rms  = init_input_masks( reth_edge_cnt + soe_cnt, _return_addr_mask, c_frame_ptr_mask );
  // Rethrow takes exception oop only, but in the argument 0 slot.
  reth_rms[TypeFunc::Parms] = mreg2regmask[find_receiver(false)];
#ifdef _LP64
  // Need two slots for ptrs in 64-bit land
  reth_rms[TypeFunc::Parms].Insert(OptoReg::add(OptoReg::Name(find_receiver(false)),1));
#endif

  // Input RegMask array shared by all TailCalls
  uint tail_call_edge_cnt = TypeFunc::Parms+2;
  RegMask *tail_call_rms = init_input_masks( tail_call_edge_cnt + soe_cnt, _return_addr_mask, c_frame_ptr_mask );

  // Input RegMask array shared by all TailJumps
  uint tail_jump_edge_cnt = TypeFunc::Parms+2;
  RegMask *tail_jump_rms = init_input_masks( tail_jump_edge_cnt + soe_cnt, _return_addr_mask, c_frame_ptr_mask );

  // TailCalls have 2 returned values (target & moop), whose masks come
  // from the usual MachNode/MachOper mechanism.  Find a sample
  // TailCall to extract these masks and put the correct masks into
  // the tail_call_rms array.
  for( i=1; i < root->req(); i++ ) {
    MachReturnNode *m = root->in(i)->as_MachReturn();
    if( m->ideal_Opcode() == Op_TailCall ) {
      tail_call_rms[TypeFunc::Parms+0] = m->MachNode::in_RegMask(TypeFunc::Parms+0);
      tail_call_rms[TypeFunc::Parms+1] = m->MachNode::in_RegMask(TypeFunc::Parms+1);
      break;
    }
  }

  // TailJumps have 2 returned values (target & ex_oop), whose masks come
  // from the usual MachNode/MachOper mechanism.  Find a sample
  // TailJump to extract these masks and put the correct masks into
  // the tail_jump_rms array.
  for( i=1; i < root->req(); i++ ) {
    MachReturnNode *m = root->in(i)->as_MachReturn();
    if( m->ideal_Opcode() == Op_TailJump ) {
      tail_jump_rms[TypeFunc::Parms+0] = m->MachNode::in_RegMask(TypeFunc::Parms+0);
      tail_jump_rms[TypeFunc::Parms+1] = m->MachNode::in_RegMask(TypeFunc::Parms+1);
      break;
    }
  }

  // Input RegMask array shared by all Halts
  uint halt_edge_cnt = TypeFunc::Parms;
  RegMask *halt_rms = init_input_masks( halt_edge_cnt + soe_cnt, _return_addr_mask, c_frame_ptr_mask );

  // Capture the return input masks into each exit flavor
  for( i=1; i < root->req(); i++ ) {
    MachReturnNode *exit = root->in(i)->as_MachReturn();
    switch( exit->ideal_Opcode() ) {
      case Op_Return   : exit->_in_rms = ret_rms;  break;
      case Op_Rethrow  : exit->_in_rms = reth_rms; break;
      case Op_TailCall : exit->_in_rms = tail_call_rms; break;
      case Op_TailJump : exit->_in_rms = tail_jump_rms; break;
      case Op_Halt     : exit->_in_rms = halt_rms; break;
      default          : ShouldNotReachHere();
    }
  }

  // Next unused projection number from Start.
  int proj_cnt = C->tf()->domain()->cnt();

  // Do all the save-on-entry registers.  Make projections from Start for
  // them, and give them a use at the exit points.  To the allocator, they
  // look like incoming register arguments.
  for( i = 0; i < _last_Mach_Reg; i++ ) {
    if( is_save_on_entry(i) ) {

      // Add the save-on-entry to the mask array
      ret_rms      [      ret_edge_cnt] = mreg2regmask[i];
      reth_rms     [     reth_edge_cnt] = mreg2regmask[i];
      tail_call_rms[tail_call_edge_cnt] = mreg2regmask[i];
      tail_jump_rms[tail_jump_edge_cnt] = mreg2regmask[i];
      // Halts need the SOE registers, but only in the stack as debug info.
      // A just-prior uncommon-trap or deoptimization will use the SOE regs.
      halt_rms     [     halt_edge_cnt] = *idealreg2spillmask[_register_save_type[i]];

      Node *mproj;

      // Is this a RegF low half of a RegD?  Double up 2 adjacent RegF's
      // into a single RegD.
      if( (i&1) == 0 &&
          _register_save_type[i  ] == Op_RegF &&
          _register_save_type[i+1] == Op_RegF &&
          is_save_on_entry(i+1) ) {
        // Add other bit for double
        ret_rms      [      ret_edge_cnt].Insert(OptoReg::Name(i+1));
        reth_rms     [     reth_edge_cnt].Insert(OptoReg::Name(i+1));
        tail_call_rms[tail_call_edge_cnt].Insert(OptoReg::Name(i+1));
        tail_jump_rms[tail_jump_edge_cnt].Insert(OptoReg::Name(i+1));
        halt_rms     [     halt_edge_cnt].Insert(OptoReg::Name(i+1));
        mproj = new (C, 1) MachProjNode( start, proj_cnt, ret_rms[ret_edge_cnt], Op_RegD );
        proj_cnt += 2;          // Skip 2 for doubles
      }
      else if( (i&1) == 1 &&    // Else check for high half of double
               _register_save_type[i-1] == Op_RegF &&
               _register_save_type[i  ] == Op_RegF &&
               is_save_on_entry(i-1) ) {
        ret_rms      [      ret_edge_cnt] = RegMask::Empty;
        reth_rms     [     reth_edge_cnt] = RegMask::Empty;
        tail_call_rms[tail_call_edge_cnt] = RegMask::Empty;
        tail_jump_rms[tail_jump_edge_cnt] = RegMask::Empty;
        halt_rms     [     halt_edge_cnt] = RegMask::Empty;
        mproj = C->top();
      }
      // Is this a RegI low half of a RegL?  Double up 2 adjacent RegI's
      // into a single RegL.
      else if( (i&1) == 0 &&
          _register_save_type[i  ] == Op_RegI &&
          _register_save_type[i+1] == Op_RegI &&
        is_save_on_entry(i+1) ) {
        // Add other bit for long
        ret_rms      [      ret_edge_cnt].Insert(OptoReg::Name(i+1));
        reth_rms     [     reth_edge_cnt].Insert(OptoReg::Name(i+1));
        tail_call_rms[tail_call_edge_cnt].Insert(OptoReg::Name(i+1));
        tail_jump_rms[tail_jump_edge_cnt].Insert(OptoReg::Name(i+1));
        halt_rms     [     halt_edge_cnt].Insert(OptoReg::Name(i+1));
        mproj = new (C, 1) MachProjNode( start, proj_cnt, ret_rms[ret_edge_cnt], Op_RegL );
        proj_cnt += 2;          // Skip 2 for longs
      }
      else if( (i&1) == 1 &&    // Else check for high half of long
               _register_save_type[i-1] == Op_RegI &&
               _register_save_type[i  ] == Op_RegI &&
               is_save_on_entry(i-1) ) {
        ret_rms      [      ret_edge_cnt] = RegMask::Empty;
        reth_rms     [     reth_edge_cnt] = RegMask::Empty;
        tail_call_rms[tail_call_edge_cnt] = RegMask::Empty;
        tail_jump_rms[tail_jump_edge_cnt] = RegMask::Empty;
        halt_rms     [     halt_edge_cnt] = RegMask::Empty;
        mproj = C->top();
      } else {
        // Make a projection for it off the Start
        mproj = new (C, 1) MachProjNode( start, proj_cnt++, ret_rms[ret_edge_cnt], _register_save_type[i] );
      }

      ret_edge_cnt ++;
      reth_edge_cnt ++;
      tail_call_edge_cnt ++;
      tail_jump_edge_cnt ++;
      halt_edge_cnt ++;

      // Add a use of the SOE register to all exit paths
      for( uint j=1; j < root->req(); j++ )
        root->in(j)->add_req(mproj);
    } // End of if a save-on-entry register
  } // End of for all machine registers
}

//------------------------------init_spill_mask--------------------------------
void Matcher::init_spill_mask( Node *ret ) {
  if( idealreg2regmask[Op_RegI] ) return; // One time only init

  OptoReg::c_frame_pointer = c_frame_pointer();
  c_frame_ptr_mask = c_frame_pointer();
#ifdef _LP64
  // pointers are twice as big
  c_frame_ptr_mask.Insert(OptoReg::add(c_frame_pointer(),1));
#endif

  // Start at OptoReg::stack0()
  STACK_ONLY_mask.Clear();
  OptoReg::Name init = OptoReg::stack2reg(0);
  // STACK_ONLY_mask is all stack bits
  OptoReg::Name i;
  for (i = init; RegMask::can_represent(i); i = OptoReg::add(i,1))
    STACK_ONLY_mask.Insert(i);
  // Also set the "infinite stack" bit.
  STACK_ONLY_mask.set_AllStack();

  // Copy the register names over into the shared world
  for( i=OptoReg::Name(0); i<OptoReg::Name(_last_Mach_Reg); i = OptoReg::add(i,1) ) {
    // SharedInfo::regName[i] = regName[i];
    // Handy RegMasks per machine register
    mreg2regmask[i].Insert(i);
  }

  // Grab the Frame Pointer
  Node *fp  = ret->in(TypeFunc::FramePtr);
  Node *mem = ret->in(TypeFunc::Memory);
  const TypePtr* atp = TypePtr::BOTTOM;
  // Share frame pointer while making spill ops
  set_shared(fp);

  // Compute generic short-offset Loads
#ifdef _LP64
  MachNode *spillCP = match_tree(new (C, 3) LoadNNode(NULL,mem,fp,atp,TypeInstPtr::BOTTOM));
#endif
  MachNode *spillI  = match_tree(new (C, 3) LoadINode(NULL,mem,fp,atp));
  MachNode *spillL  = match_tree(new (C, 3) LoadLNode(NULL,mem,fp,atp));
  MachNode *spillF  = match_tree(new (C, 3) LoadFNode(NULL,mem,fp,atp));
  MachNode *spillD  = match_tree(new (C, 3) LoadDNode(NULL,mem,fp,atp));
  MachNode *spillP  = match_tree(new (C, 3) LoadPNode(NULL,mem,fp,atp,TypeInstPtr::BOTTOM));
  assert(spillI != NULL && spillL != NULL && spillF != NULL &&
         spillD != NULL && spillP != NULL, "");

  // Get the ADLC notion of the right regmask, for each basic type.
#ifdef _LP64
  idealreg2regmask[Op_RegN] = &spillCP->out_RegMask();
#endif
  idealreg2regmask[Op_RegI] = &spillI->out_RegMask();
  idealreg2regmask[Op_RegL] = &spillL->out_RegMask();
  idealreg2regmask[Op_RegF] = &spillF->out_RegMask();
  idealreg2regmask[Op_RegD] = &spillD->out_RegMask();
  idealreg2regmask[Op_RegP] = &spillP->out_RegMask();
}

#ifdef ASSERT
static void match_alias_type(Compile* C, Node* n, Node* m) {
  if (!VerifyAliases)  return;  // do not go looking for trouble by default
  const TypePtr* nat = n->adr_type();
  const TypePtr* mat = m->adr_type();
  int nidx = C->get_alias_index(nat);
  int midx = C->get_alias_index(mat);
  // Detune the assert for cases like (AndI 0xFF (LoadB p)).
  if (nidx == Compile::AliasIdxTop && midx >= Compile::AliasIdxRaw) {
    for (uint i = 1; i < n->req(); i++) {
      Node* n1 = n->in(i);
      const TypePtr* n1at = n1->adr_type();
      if (n1at != NULL) {
        nat = n1at;
        nidx = C->get_alias_index(n1at);
      }
    }
  }
  // %%% Kludgery.  Instead, fix ideal adr_type methods for all these cases:
  if (nidx == Compile::AliasIdxTop && midx == Compile::AliasIdxRaw) {
    switch (n->Opcode()) {
    case Op_PrefetchRead:
    case Op_PrefetchWrite:
      nidx = Compile::AliasIdxRaw;
      nat = TypeRawPtr::BOTTOM;
      break;
    }
  }
  if (nidx == Compile::AliasIdxRaw && midx == Compile::AliasIdxTop) {
    switch (n->Opcode()) {
    case Op_ClearArray:
      midx = Compile::AliasIdxRaw;
      mat = TypeRawPtr::BOTTOM;
      break;
    }
  }
  if (nidx == Compile::AliasIdxTop && midx == Compile::AliasIdxBot) {
    switch (n->Opcode()) {
    case Op_Return:
    case Op_Rethrow:
    case Op_Halt:
    case Op_TailCall:
    case Op_TailJump:
      nidx = Compile::AliasIdxBot;
      nat = TypePtr::BOTTOM;
      break;
    }
  }
  if (nidx == Compile::AliasIdxBot && midx == Compile::AliasIdxTop) {
    switch (n->Opcode()) {
    case Op_StrComp:
    case Op_AryEq:
    case Op_MemBarVolatile:
    case Op_MemBarCPUOrder: // %%% these ideals should have narrower adr_type?
      nidx = Compile::AliasIdxTop;
      nat = NULL;
      break;
    }
  }
  if (nidx != midx) {
    if (PrintOpto || (PrintMiscellaneous && (WizardMode || Verbose))) {
      tty->print_cr("==== Matcher alias shift %d => %d", nidx, midx);
      n->dump();
      m->dump();
    }
    assert(C->subsume_loads() && C->must_alias(nat, midx),
           "must not lose alias info when matching");
  }
}
#endif


//------------------------------MStack-----------------------------------------
// State and MStack class used in xform() and find_shared() iterative methods.
enum Node_State { Pre_Visit,  // node has to be pre-visited
                      Visit,  // visit node
                 Post_Visit,  // post-visit node
             Alt_Post_Visit   // alternative post-visit path
                };

class MStack: public Node_Stack {
  public:
    MStack(int size) : Node_Stack(size) { }

    void push(Node *n, Node_State ns) {
      Node_Stack::push(n, (uint)ns);
    }
    void push(Node *n, Node_State ns, Node *parent, int indx) {
      ++_inode_top;
      if ((_inode_top + 1) >= _inode_max) grow();
      _inode_top->node = parent;
      _inode_top->indx = (uint)indx;
      ++_inode_top;
      _inode_top->node = n;
      _inode_top->indx = (uint)ns;
    }
    Node *parent() {
      pop();
      return node();
    }
    Node_State state() const {
      return (Node_State)index();
    }
    void set_state(Node_State ns) {
      set_index((uint)ns);
    }
};


//------------------------------xform------------------------------------------
// Given a Node in old-space, Match him (Label/Reduce) to produce a machine
// Node in new-space.  Given a new-space Node, recursively walk his children.
Node *Matcher::transform( Node *n ) { ShouldNotCallThis(); return n; }
Node *Matcher::xform( Node *n, int max_stack ) {
  // Use one stack to keep both: child's node/state and parent's node/index
  MStack mstack(max_stack * 2 * 2); // C->unique() * 2 * 2
  mstack.push(n, Visit, NULL, -1);  // set NULL as parent to indicate root

  while (mstack.is_nonempty()) {
    n = mstack.node();          // Leave node on stack
    Node_State nstate = mstack.state();
    if (nstate == Visit) {
      mstack.set_state(Post_Visit);
      Node *oldn = n;
      // Old-space or new-space check
      if (!C->node_arena()->contains(n)) {
        // Old space!
        Node* m;
        if (has_new_node(n)) {  // Not yet Label/Reduced
          m = new_node(n);
        } else {
          if (!is_dontcare(n)) { // Matcher can match this guy
            // Calls match special.  They match alone with no children.
            // Their children, the incoming arguments, match normally.
            m = n->is_SafePoint() ? match_sfpt(n->as_SafePoint()):match_tree(n);
            if (C->failing())  return NULL;
            if (m == NULL) { Matcher::soft_match_failure(); return NULL; }
          } else {                  // Nothing the matcher cares about
            if( n->is_Proj() && n->in(0)->is_Multi()) {       // Projections?
              // Convert to machine-dependent projection
              m = n->in(0)->as_Multi()->match( n->as_Proj(), this );
#ifdef ASSERT
              _new2old_map.map(m->_idx, n);
#endif
              if (m->in(0) != NULL) // m might be top
                collect_null_checks(m, n);
            } else {                // Else just a regular 'ol guy
              m = n->clone();       // So just clone into new-space
#ifdef ASSERT
              _new2old_map.map(m->_idx, n);
#endif
              // Def-Use edges will be added incrementally as Uses
              // of this node are matched.
              assert(m->outcnt() == 0, "no Uses of this clone yet");
            }
          }

          set_new_node(n, m);       // Map old to new
          if (_old_node_note_array != NULL) {
            Node_Notes* nn = C->locate_node_notes(_old_node_note_array,
                                                  n->_idx);
            C->set_node_notes_at(m->_idx, nn);
          }
          debug_only(match_alias_type(C, n, m));
        }
        n = m;    // n is now a new-space node
        mstack.set_node(n);
      }

      // New space!
      if (_visited.test_set(n->_idx)) continue; // while(mstack.is_nonempty())

      int i;
      // Put precedence edges on stack first (match them last).
      for (i = oldn->req(); (uint)i < oldn->len(); i++) {
        Node *m = oldn->in(i);
        if (m == NULL) break;
        // set -1 to call add_prec() instead of set_req() during Step1
        mstack.push(m, Visit, n, -1);
      }

      // For constant debug info, I'd rather have unmatched constants.
      int cnt = n->req();
      JVMState* jvms = n->jvms();
      int debug_cnt = jvms ? jvms->debug_start() : cnt;

      // Now do only debug info.  Clone constants rather than matching.
      // Constants are represented directly in the debug info without
      // the need for executable machine instructions.
      // Monitor boxes are also represented directly.
      for (i = cnt - 1; i >= debug_cnt; --i) { // For all debug inputs do
        Node *m = n->in(i);          // Get input
        int op = m->Opcode();
        assert((op == Op_BoxLock) == jvms->is_monitor_use(i), "boxes only at monitor sites");
        if( op == Op_ConI || op == Op_ConP || op == Op_ConN ||
            op == Op_ConF || op == Op_ConD || op == Op_ConL
            // || op == Op_BoxLock  // %%%% enable this and remove (+++) in chaitin.cpp
            ) {
          m = m->clone();
#ifdef ASSERT
          _new2old_map.map(m->_idx, n);
#endif
          mstack.push(m, Post_Visit, n, i); // Don't need to visit
          mstack.push(m->in(0), Visit, m, 0);
        } else {
          mstack.push(m, Visit, n, i);
        }
      }

      // And now walk his children, and convert his inputs to new-space.
      for( ; i >= 0; --i ) { // For all normal inputs do
        Node *m = n->in(i);  // Get input
        if(m != NULL)
          mstack.push(m, Visit, n, i);
      }

    }
    else if (nstate == Post_Visit) {
      // Set xformed input
      Node *p = mstack.parent();
      if (p != NULL) { // root doesn't have parent
        int i = (int)mstack.index();
        if (i >= 0)
          p->set_req(i, n); // required input
        else if (i == -1)
          p->add_prec(n);   // precedence input
        else
          ShouldNotReachHere();
      }
      mstack.pop(); // remove processed node from stack
    }
    else {
      ShouldNotReachHere();
    }
  } // while (mstack.is_nonempty())
  return n; // Return new-space Node
}

//------------------------------warp_outgoing_stk_arg------------------------
OptoReg::Name Matcher::warp_outgoing_stk_arg( VMReg reg, OptoReg::Name begin_out_arg_area, OptoReg::Name &out_arg_limit_per_call ) {
  // Convert outgoing argument location to a pre-biased stack offset
  if (reg->is_stack()) {
    OptoReg::Name warped = reg->reg2stack();
    // Adjust the stack slot offset to be the register number used
    // by the allocator.
    warped = OptoReg::add(begin_out_arg_area, warped);
    // Keep track of the largest numbered stack slot used for an arg.
    // Largest used slot per call-site indicates the amount of stack
    // that is killed by the call.
    if( warped >= out_arg_limit_per_call )
      out_arg_limit_per_call = OptoReg::add(warped,1);
    if (!RegMask::can_represent(warped)) {
      C->record_method_not_compilable_all_tiers("unsupported calling sequence");
      return OptoReg::Bad;
    }
    return warped;
  }
  return OptoReg::as_OptoReg(reg);
}


//------------------------------match_sfpt-------------------------------------
// Helper function to match call instructions.  Calls match special.
// They match alone with no children.  Their children, the incoming
// arguments, match normally.
MachNode *Matcher::match_sfpt( SafePointNode *sfpt ) {
  MachSafePointNode *msfpt = NULL;
  MachCallNode      *mcall = NULL;
  uint               cnt;
  // Split out case for SafePoint vs Call
  CallNode *call;
  const TypeTuple *domain;
  ciMethod*        method = NULL;
  if( sfpt->is_Call() ) {
    call = sfpt->as_Call();
    domain = call->tf()->domain();
    cnt = domain->cnt();

    // Match just the call, nothing else
    MachNode *m = match_tree(call);
    if (C->failing())  return NULL;
    if( m == NULL ) { Matcher::soft_match_failure(); return NULL; }

    // Copy data from the Ideal SafePoint to the machine version
    mcall = m->as_MachCall();

    mcall->set_tf(         call->tf());
    mcall->set_entry_point(call->entry_point());
    mcall->set_cnt(        call->cnt());

    if( mcall->is_MachCallJava() ) {
      MachCallJavaNode *mcall_java  = mcall->as_MachCallJava();
      const CallJavaNode *call_java =  call->as_CallJava();
      method = call_java->method();
      mcall_java->_method = method;
      mcall_java->_bci = call_java->_bci;
      mcall_java->_optimized_virtual = call_java->is_optimized_virtual();
      if( mcall_java->is_MachCallStaticJava() )
        mcall_java->as_MachCallStaticJava()->_name =
         call_java->as_CallStaticJava()->_name;
      if( mcall_java->is_MachCallDynamicJava() )
        mcall_java->as_MachCallDynamicJava()->_vtable_index =
         call_java->as_CallDynamicJava()->_vtable_index;
    }
    else if( mcall->is_MachCallRuntime() ) {
      mcall->as_MachCallRuntime()->_name = call->as_CallRuntime()->_name;
    }
    msfpt = mcall;
  }
  // This is a non-call safepoint
  else {
    call = NULL;
    domain = NULL;
    MachNode *mn = match_tree(sfpt);
    if (C->failing())  return NULL;
    msfpt = mn->as_MachSafePoint();
    cnt = TypeFunc::Parms;
  }

  // Advertise the correct memory effects (for anti-dependence computation).
  msfpt->set_adr_type(sfpt->adr_type());

  // Allocate a private array of RegMasks.  These RegMasks are not shared.
  msfpt->_in_rms = NEW_RESOURCE_ARRAY( RegMask, cnt );
  // Empty them all.
  memset( msfpt->_in_rms, 0, sizeof(RegMask)*cnt );

  // Do all the pre-defined non-Empty register masks
  msfpt->_in_rms[TypeFunc::ReturnAdr] = _return_addr_mask;
  msfpt->_in_rms[TypeFunc::FramePtr ] = c_frame_ptr_mask;

  // Place first outgoing argument can possibly be put.
  OptoReg::Name begin_out_arg_area = OptoReg::add(_new_SP, C->out_preserve_stack_slots());
  assert( is_even(begin_out_arg_area), "" );
  // Compute max outgoing register number per call site.
  OptoReg::Name out_arg_limit_per_call = begin_out_arg_area;
  // Calls to C may hammer extra stack slots above and beyond any arguments.
  // These are usually backing store for register arguments for varargs.
  if( call != NULL && call->is_CallRuntime() )
    out_arg_limit_per_call = OptoReg::add(out_arg_limit_per_call,C->varargs_C_out_slots_killed());


  // Do the normal argument list (parameters) register masks
  int argcnt = cnt - TypeFunc::Parms;
  if( argcnt > 0 ) {          // Skip it all if we have no args
    BasicType *sig_bt  = NEW_RESOURCE_ARRAY( BasicType, argcnt );
    VMRegPair *parm_regs = NEW_RESOURCE_ARRAY( VMRegPair, argcnt );
    int i;
    for( i = 0; i < argcnt; i++ ) {
      sig_bt[i] = domain->field_at(i+TypeFunc::Parms)->basic_type();
    }
    // V-call to pick proper calling convention
    call->calling_convention( sig_bt, parm_regs, argcnt );

#ifdef ASSERT
    // Sanity check users' calling convention.  Really handy during
    // the initial porting effort.  Fairly expensive otherwise.
    { for (int i = 0; i<argcnt; i++) {
      if( !parm_regs[i].first()->is_valid() &&
          !parm_regs[i].second()->is_valid() ) continue;
      VMReg reg1 = parm_regs[i].first();
      VMReg reg2 = parm_regs[i].second();
      for (int j = 0; j < i; j++) {
        if( !parm_regs[j].first()->is_valid() &&
            !parm_regs[j].second()->is_valid() ) continue;
        VMReg reg3 = parm_regs[j].first();
        VMReg reg4 = parm_regs[j].second();
        if( !reg1->is_valid() ) {
          assert( !reg2->is_valid(), "valid halvsies" );
        } else if( !reg3->is_valid() ) {
          assert( !reg4->is_valid(), "valid halvsies" );
        } else {
          assert( reg1 != reg2, "calling conv. must produce distinct regs");
          assert( reg1 != reg3, "calling conv. must produce distinct regs");
          assert( reg1 != reg4, "calling conv. must produce distinct regs");
          assert( reg2 != reg3, "calling conv. must produce distinct regs");
          assert( reg2 != reg4 || !reg2->is_valid(), "calling conv. must produce distinct regs");
          assert( reg3 != reg4, "calling conv. must produce distinct regs");
        }
      }
    }
    }
#endif

    // Visit each argument.  Compute its outgoing register mask.
    // Return results now can have 2 bits returned.
    // Compute max over all outgoing arguments both per call-site
    // and over the entire method.
    for( i = 0; i < argcnt; i++ ) {
      // Address of incoming argument mask to fill in
      RegMask *rm = &mcall->_in_rms[i+TypeFunc::Parms];
      if( !parm_regs[i].first()->is_valid() &&
          !parm_regs[i].second()->is_valid() ) {
        continue;               // Avoid Halves
      }
      // Grab first register, adjust stack slots and insert in mask.
      OptoReg::Name reg1 = warp_outgoing_stk_arg(parm_regs[i].first(), begin_out_arg_area, out_arg_limit_per_call );
      if (OptoReg::is_valid(reg1))
        rm->Insert( reg1 );
      // Grab second register (if any), adjust stack slots and insert in mask.
      OptoReg::Name reg2 = warp_outgoing_stk_arg(parm_regs[i].second(), begin_out_arg_area, out_arg_limit_per_call );
      if (OptoReg::is_valid(reg2))
        rm->Insert( reg2 );
    } // End of for all arguments

    // Compute number of stack slots needed to restore stack in case of
    // Pascal-style argument popping.
    mcall->_argsize = out_arg_limit_per_call - begin_out_arg_area;
  }

  // Compute the max stack slot killed by any call.  These will not be
  // available for debug info, and will be used to adjust FIRST_STACK_mask
  // after all call sites have been visited.
  if( _out_arg_limit < out_arg_limit_per_call)
    _out_arg_limit = out_arg_limit_per_call;

  if (mcall) {
    // Kill the outgoing argument area, including any non-argument holes and
    // any legacy C-killed slots.  Use Fat-Projections to do the killing.
    // Since the max-per-method covers the max-per-call-site and debug info
    // is excluded on the max-per-method basis, debug info cannot land in
    // this killed area.
    uint r_cnt = mcall->tf()->range()->cnt();
    MachProjNode *proj = new (C, 1) MachProjNode( mcall, r_cnt+10000, RegMask::Empty, MachProjNode::fat_proj );
    if (!RegMask::can_represent(OptoReg::Name(out_arg_limit_per_call-1))) {
      C->record_method_not_compilable_all_tiers("unsupported outgoing calling sequence");
    } else {
      for (int i = begin_out_arg_area; i < out_arg_limit_per_call; i++)
        proj->_rout.Insert(OptoReg::Name(i));
    }
    if( proj->_rout.is_NotEmpty() )
      _proj_list.push(proj);
  }
  // Transfer the safepoint information from the call to the mcall
  // Move the JVMState list
  msfpt->set_jvms(sfpt->jvms());
  for (JVMState* jvms = msfpt->jvms(); jvms; jvms = jvms->caller()) {
    jvms->set_map(sfpt);
  }

  // Debug inputs begin just after the last incoming parameter
  assert( (mcall == NULL) || (mcall->jvms() == NULL) ||
          (mcall->jvms()->debug_start() + mcall->_jvmadj == mcall->tf()->domain()->cnt()), "" );

  // Move the OopMap
  msfpt->_oop_map = sfpt->_oop_map;

  // Registers killed by the call are set in the local scheduling pass
  // of Global Code Motion.
  return msfpt;
}

//---------------------------match_tree----------------------------------------
// Match a Ideal Node DAG - turn it into a tree; Label & Reduce.  Used as part
// of the whole-sale conversion from Ideal to Mach Nodes.  Also used for
// making GotoNodes while building the CFG and in init_spill_mask() to identify
// a Load's result RegMask for memoization in idealreg2regmask[]
MachNode *Matcher::match_tree( const Node *n ) {
  assert( n->Opcode() != Op_Phi, "cannot match" );
  assert( !n->is_block_start(), "cannot match" );
  // Set the mark for all locally allocated State objects.
  // When this call returns, the _states_arena arena will be reset
  // freeing all State objects.
  ResourceMark rm( &_states_arena );

  LabelRootDepth = 0;

  // StoreNodes require their Memory input to match any LoadNodes
  Node *mem = n->is_Store() ? n->in(MemNode::Memory) : (Node*)1 ;
#ifdef ASSERT
  Node* save_mem_node = _mem_node;
  _mem_node = n->is_Store() ? (Node*)n : NULL;
#endif
  // State object for root node of match tree
  // Allocate it on _states_arena - stack allocation can cause stack overflow.
  State *s = new (&_states_arena) State;
  s->_kids[0] = NULL;
  s->_kids[1] = NULL;
  s->_leaf = (Node*)n;
  // Label the input tree, allocating labels from top-level arena
  Label_Root( n, s, n->in(0), mem );
  if (C->failing())  return NULL;

  // The minimum cost match for the whole tree is found at the root State
  uint mincost = max_juint;
  uint cost = max_juint;
  uint i;
  for( i = 0; i < NUM_OPERANDS; i++ ) {
    if( s->valid(i) &&                // valid entry and
        s->_cost[i] < cost &&         // low cost and
        s->_rule[i] >= NUM_OPERANDS ) // not an operand
      cost = s->_cost[mincost=i];
  }
  if (mincost == max_juint) {
#ifndef PRODUCT
    tty->print("No matching rule for:");
    s->dump();
#endif
    Matcher::soft_match_failure();
    return NULL;
  }
  // Reduce input tree based upon the state labels to machine Nodes
  MachNode *m = ReduceInst( s, s->_rule[mincost], mem );
#ifdef ASSERT
  _old2new_map.map(n->_idx, m);
  _new2old_map.map(m->_idx, (Node*)n);
#endif

  // Add any Matcher-ignored edges
  uint cnt = n->req();
  uint start = 1;
  if( mem != (Node*)1 ) start = MemNode::Memory+1;
  if( n->is_AddP() ) {
    assert( mem == (Node*)1, "" );
    start = AddPNode::Base+1;
  }
  for( i = start; i < cnt; i++ ) {
    if( !n->match_edge(i) ) {
      if( i < m->req() )
        m->ins_req( i, n->in(i) );
      else
        m->add_req( n->in(i) );
    }
  }

  debug_only( _mem_node = save_mem_node; )
  return m;
}


//------------------------------match_into_reg---------------------------------
// Choose to either match this Node in a register or part of the current
// match tree.  Return true for requiring a register and false for matching
// as part of the current match tree.
static bool match_into_reg( const Node *n, Node *m, Node *control, int i, bool shared ) {

  const Type *t = m->bottom_type();

  if( t->singleton() ) {
    // Never force constants into registers.  Allow them to match as
    // constants or registers.  Copies of the same value will share
    // the same register.  See find_shared_node.
    return false;
  } else {                      // Not a constant
    // Stop recursion if they have different Controls.
    // Slot 0 of constants is not really a Control.
    if( control && m->in(0) && control != m->in(0) ) {

      // Actually, we can live with the most conservative control we
      // find, if it post-dominates the others.  This allows us to
      // pick up load/op/store trees where the load can float a little
      // above the store.
      Node *x = control;
      const uint max_scan = 6;   // Arbitrary scan cutoff
      uint j;
      for( j=0; j<max_scan; j++ ) {
        if( x->is_Region() )    // Bail out at merge points
          return true;
        x = x->in(0);
        if( x == m->in(0) )     // Does 'control' post-dominate
          break;                // m->in(0)?  If so, we can use it
      }
      if( j == max_scan )       // No post-domination before scan end?
        return true;            // Then break the match tree up
    }
    if (m->is_DecodeN() && Matcher::clone_shift_expressions) {
      // These are commonly used in address expressions and can
      // efficiently fold into them on X64 in some cases.
      return false;
    }
  }

  // Not forceable cloning.  If shared, put it into a register.
  return shared;
}


//------------------------------Instruction Selection--------------------------
// Label method walks a "tree" of nodes, using the ADLC generated DFA to match
// ideal nodes to machine instructions.  Trees are delimited by shared Nodes,
// things the Matcher does not match (e.g., Memory), and things with different
// Controls (hence forced into different blocks).  We pass in the Control
// selected for this entire State tree.

// The Matcher works on Trees, but an Intel add-to-memory requires a DAG: the
// Store and the Load must have identical Memories (as well as identical
// pointers).  Since the Matcher does not have anything for Memory (and
// does not handle DAGs), I have to match the Memory input myself.  If the
// Tree root is a Store, I require all Loads to have the identical memory.
Node *Matcher::Label_Root( const Node *n, State *svec, Node *control, const Node *mem){
  // Since Label_Root is a recursive function, its possible that we might run
  // out of stack space.  See bugs 6272980 & 6227033 for more info.
  LabelRootDepth++;
  if (LabelRootDepth > MaxLabelRootDepth) {
    C->record_method_not_compilable_all_tiers("Out of stack space, increase MaxLabelRootDepth");
    return NULL;
  }
  uint care = 0;                // Edges matcher cares about
  uint cnt = n->req();
  uint i = 0;

  // Examine children for memory state
  // Can only subsume a child into your match-tree if that child's memory state
  // is not modified along the path to another input.
  // It is unsafe even if the other inputs are separate roots.
  Node *input_mem = NULL;
  for( i = 1; i < cnt; i++ ) {
    if( !n->match_edge(i) ) continue;
    Node *m = n->in(i);         // Get ith input
    assert( m, "expect non-null children" );
    if( m->is_Load() ) {
      if( input_mem == NULL ) {
        input_mem = m->in(MemNode::Memory);
      } else if( input_mem != m->in(MemNode::Memory) ) {
        input_mem = NodeSentinel;
      }
    }
  }

  for( i = 1; i < cnt; i++ ){// For my children
    if( !n->match_edge(i) ) continue;
    Node *m = n->in(i);         // Get ith input
    // Allocate states out of a private arena
    State *s = new (&_states_arena) State;
    svec->_kids[care++] = s;
    assert( care <= 2, "binary only for now" );

    // Recursively label the State tree.
    s->_kids[0] = NULL;
    s->_kids[1] = NULL;
    s->_leaf = m;

    // Check for leaves of the State Tree; things that cannot be a part of
    // the current tree.  If it finds any, that value is matched as a
    // register operand.  If not, then the normal matching is used.
    if( match_into_reg(n, m, control, i, is_shared(m)) ||
        //
        // Stop recursion if this is LoadNode and the root of this tree is a
        // StoreNode and the load & store have different memories.
        ((mem!=(Node*)1) && m->is_Load() && m->in(MemNode::Memory) != mem) ||
        // Can NOT include the match of a subtree when its memory state
        // is used by any of the other subtrees
        (input_mem == NodeSentinel) ) {
#ifndef PRODUCT
      // Print when we exclude matching due to different memory states at input-loads
      if( PrintOpto && (Verbose && WizardMode) && (input_mem == NodeSentinel)
        && !((mem!=(Node*)1) && m->is_Load() && m->in(MemNode::Memory) != mem) ) {
        tty->print_cr("invalid input_mem");
      }
#endif
      // Switch to a register-only opcode; this value must be in a register
      // and cannot be subsumed as part of a larger instruction.
      s->DFA( m->ideal_reg(), m );

    } else {
      // If match tree has no control and we do, adopt it for entire tree
      if( control == NULL && m->in(0) != NULL && m->req() > 1 )
        control = m->in(0);         // Pick up control
      // Else match as a normal part of the match tree.
      control = Label_Root(m,s,control,mem);
      if (C->failing()) return NULL;
    }
  }


  // Call DFA to match this node, and return
  svec->DFA( n->Opcode(), n );

#ifdef ASSERT
  uint x;
  for( x = 0; x < _LAST_MACH_OPER; x++ )
    if( svec->valid(x) )
      break;

  if (x >= _LAST_MACH_OPER) {
    n->dump();
    svec->dump();
    assert( false, "bad AD file" );
  }
#endif
  return control;
}


// Con nodes reduced using the same rule can share their MachNode
// which reduces the number of copies of a constant in the final
// program.  The register allocator is free to split uses later to
// split live ranges.
MachNode* Matcher::find_shared_node(Node* leaf, uint rule) {
  if (!leaf->is_Con() && !leaf->is_DecodeN()) return NULL;

  // See if this Con has already been reduced using this rule.
  if (_shared_nodes.Size() <= leaf->_idx) return NULL;
  MachNode* last = (MachNode*)_shared_nodes.at(leaf->_idx);
  if (last != NULL && rule == last->rule()) {
    // Don't expect control change for DecodeN
    if (leaf->is_DecodeN())
      return last;
    // Get the new space root.
    Node* xroot = new_node(C->root());
    if (xroot == NULL) {
      // This shouldn't happen give the order of matching.
      return NULL;
    }

    // Shared constants need to have their control be root so they
    // can be scheduled properly.
    Node* control = last->in(0);
    if (control != xroot) {
      if (control == NULL || control == C->root()) {
        last->set_req(0, xroot);
      } else {
        assert(false, "unexpected control");
        return NULL;
      }
    }
    return last;
  }
  return NULL;
}


//------------------------------ReduceInst-------------------------------------
// Reduce a State tree (with given Control) into a tree of MachNodes.
// This routine (and it's cohort ReduceOper) convert Ideal Nodes into
// complicated machine Nodes.  Each MachNode covers some tree of Ideal Nodes.
// Each MachNode has a number of complicated MachOper operands; each
// MachOper also covers a further tree of Ideal Nodes.

// The root of the Ideal match tree is always an instruction, so we enter
// the recursion here.  After building the MachNode, we need to recurse
// the tree checking for these cases:
// (1) Child is an instruction -
//     Build the instruction (recursively), add it as an edge.
//     Build a simple operand (register) to hold the result of the instruction.
// (2) Child is an interior part of an instruction -
//     Skip over it (do nothing)
// (3) Child is the start of a operand -
//     Build the operand, place it inside the instruction
//     Call ReduceOper.
MachNode *Matcher::ReduceInst( State *s, int rule, Node *&mem ) {
  assert( rule >= NUM_OPERANDS, "called with operand rule" );

  MachNode* shared_node = find_shared_node(s->_leaf, rule);
  if (shared_node != NULL) {
    return shared_node;
  }

  // Build the object to represent this state & prepare for recursive calls
  MachNode *mach = s->MachNodeGenerator( rule, C );
  mach->_opnds[0] = s->MachOperGenerator( _reduceOp[rule], C );
  assert( mach->_opnds[0] != NULL, "Missing result operand" );
  Node *leaf = s->_leaf;
  // Check for instruction or instruction chain rule
  if( rule >= _END_INST_CHAIN_RULE || rule < _BEGIN_INST_CHAIN_RULE ) {
    assert(C->node_arena()->contains(s->_leaf) || !has_new_node(s->_leaf),
           "duplicating node that's already been matched");
    // Instruction
    mach->add_req( leaf->in(0) ); // Set initial control
    // Reduce interior of complex instruction
    ReduceInst_Interior( s, rule, mem, mach, 1 );
  } else {
    // Instruction chain rules are data-dependent on their inputs
    mach->add_req(0);             // Set initial control to none
    ReduceInst_Chain_Rule( s, rule, mem, mach );
  }

  // If a Memory was used, insert a Memory edge
  if( mem != (Node*)1 ) {
    mach->ins_req(MemNode::Memory,mem);
#ifdef ASSERT
    // Verify adr type after matching memory operation
    const MachOper* oper = mach->memory_operand();
    if (oper != NULL && oper != (MachOper*)-1 &&
        mach->adr_type() != TypeRawPtr::BOTTOM) { // non-direct addressing mode
      // It has a unique memory operand.  Find corresponding ideal mem node.
      Node* m = NULL;
      if (leaf->is_Mem()) {
        m = leaf;
      } else {
        m = _mem_node;
        assert(m != NULL && m->is_Mem(), "expecting memory node");
      }
      const Type* mach_at = mach->adr_type();
      // DecodeN node consumed by an address may have different type
      // then its input. Don't compare types for such case.
      if (m->adr_type() != mach_at &&
          (m->in(MemNode::Address)->is_DecodeN() ||
           m->in(MemNode::Address)->is_AddP() &&
           m->in(MemNode::Address)->in(AddPNode::Address)->is_DecodeN() ||
           m->in(MemNode::Address)->is_AddP() &&
           m->in(MemNode::Address)->in(AddPNode::Address)->is_AddP() &&
           m->in(MemNode::Address)->in(AddPNode::Address)->in(AddPNode::Address)->is_DecodeN())) {
        mach_at = m->adr_type();
      }
      if (m->adr_type() != mach_at) {
        m->dump();
        tty->print_cr("mach:");
        mach->dump(1);
      }
      assert(m->adr_type() == mach_at, "matcher should not change adr type");
    }
#endif
  }

  // If the _leaf is an AddP, insert the base edge
  if( leaf->is_AddP() )
    mach->ins_req(AddPNode::Base,leaf->in(AddPNode::Base));

  uint num_proj = _proj_list.size();

  // Perform any 1-to-many expansions required
  MachNode *ex = mach->Expand(s,_proj_list);
  if( ex != mach ) {
    assert(ex->ideal_reg() == mach->ideal_reg(), "ideal types should match");
    if( ex->in(1)->is_Con() )
      ex->in(1)->set_req(0, C->root());
    // Remove old node from the graph
    for( uint i=0; i<mach->req(); i++ ) {
      mach->set_req(i,NULL);
    }
#ifdef ASSERT
    _new2old_map.map(ex->_idx, s->_leaf);
#endif
  }

  // PhaseChaitin::fixup_spills will sometimes generate spill code
  // via the matcher.  By the time, nodes have been wired into the CFG,
  // and any further nodes generated by expand rules will be left hanging
  // in space, and will not get emitted as output code.  Catch this.
  // Also, catch any new register allocation constraints ("projections")
  // generated belatedly during spill code generation.
  if (_allocation_started) {
    guarantee(ex == mach, "no expand rules during spill generation");
    guarantee(_proj_list.size() == num_proj, "no allocation during spill generation");
  }

  if (leaf->is_Con() || leaf->is_DecodeN()) {
    // Record the con for sharing
    _shared_nodes.map(leaf->_idx, ex);
  }

  return ex;
}

void Matcher::ReduceInst_Chain_Rule( State *s, int rule, Node *&mem, MachNode *mach ) {
  // 'op' is what I am expecting to receive
  int op = _leftOp[rule];
  // Operand type to catch childs result
  // This is what my child will give me.
  int opnd_class_instance = s->_rule[op];
  // Choose between operand class or not.
  // This is what I will receive.
  int catch_op = (FIRST_OPERAND_CLASS <= op && op < NUM_OPERANDS) ? opnd_class_instance : op;
  // New rule for child.  Chase operand classes to get the actual rule.
  int newrule = s->_rule[catch_op];

  if( newrule < NUM_OPERANDS ) {
    // Chain from operand or operand class, may be output of shared node
    assert( 0 <= opnd_class_instance && opnd_class_instance < NUM_OPERANDS,
            "Bad AD file: Instruction chain rule must chain from operand");
    // Insert operand into array of operands for this instruction
    mach->_opnds[1] = s->MachOperGenerator( opnd_class_instance, C );

    ReduceOper( s, newrule, mem, mach );
  } else {
    // Chain from the result of an instruction
    assert( newrule >= _LAST_MACH_OPER, "Do NOT chain from internal operand");
    mach->_opnds[1] = s->MachOperGenerator( _reduceOp[catch_op], C );
    Node *mem1 = (Node*)1;
    debug_only(Node *save_mem_node = _mem_node;)
    mach->add_req( ReduceInst(s, newrule, mem1) );
    debug_only(_mem_node = save_mem_node;)
  }
  return;
}


uint Matcher::ReduceInst_Interior( State *s, int rule, Node *&mem, MachNode *mach, uint num_opnds ) {
  if( s->_leaf->is_Load() ) {
    Node *mem2 = s->_leaf->in(MemNode::Memory);
    assert( mem == (Node*)1 || mem == mem2, "multiple Memories being matched at once?" );
    debug_only( if( mem == (Node*)1 ) _mem_node = s->_leaf;)
    mem = mem2;
  }
  if( s->_leaf->in(0) != NULL && s->_leaf->req() > 1) {
    if( mach->in(0) == NULL )
      mach->set_req(0, s->_leaf->in(0));
  }

  // Now recursively walk the state tree & add operand list.
  for( uint i=0; i<2; i++ ) {   // binary tree
    State *newstate = s->_kids[i];
    if( newstate == NULL ) break;      // Might only have 1 child
    // 'op' is what I am expecting to receive
    int op;
    if( i == 0 ) {
      op = _leftOp[rule];
    } else {
      op = _rightOp[rule];
    }
    // Operand type to catch childs result
    // This is what my child will give me.
    int opnd_class_instance = newstate->_rule[op];
    // Choose between operand class or not.
    // This is what I will receive.
    int catch_op = (op >= FIRST_OPERAND_CLASS && op < NUM_OPERANDS) ? opnd_class_instance : op;
    // New rule for child.  Chase operand classes to get the actual rule.
    int newrule = newstate->_rule[catch_op];

    if( newrule < NUM_OPERANDS ) { // Operand/operandClass or internalOp/instruction?
      // Operand/operandClass
      // Insert operand into array of operands for this instruction
      mach->_opnds[num_opnds++] = newstate->MachOperGenerator( opnd_class_instance, C );
      ReduceOper( newstate, newrule, mem, mach );

    } else {                    // Child is internal operand or new instruction
      if( newrule < _LAST_MACH_OPER ) { // internal operand or instruction?
        // internal operand --> call ReduceInst_Interior
        // Interior of complex instruction.  Do nothing but recurse.
        num_opnds = ReduceInst_Interior( newstate, newrule, mem, mach, num_opnds );
      } else {
        // instruction --> call build operand(  ) to catch result
        //             --> ReduceInst( newrule )
        mach->_opnds[num_opnds++] = s->MachOperGenerator( _reduceOp[catch_op], C );
        Node *mem1 = (Node*)1;
        debug_only(Node *save_mem_node = _mem_node;)
        mach->add_req( ReduceInst( newstate, newrule, mem1 ) );
        debug_only(_mem_node = save_mem_node;)
      }
    }
    assert( mach->_opnds[num_opnds-1], "" );
  }
  return num_opnds;
}

// This routine walks the interior of possible complex operands.
// At each point we check our children in the match tree:
// (1) No children -
//     We are a leaf; add _leaf field as an input to the MachNode
// (2) Child is an internal operand -
//     Skip over it ( do nothing )
// (3) Child is an instruction -
//     Call ReduceInst recursively and
//     and instruction as an input to the MachNode
void Matcher::ReduceOper( State *s, int rule, Node *&mem, MachNode *mach ) {
  assert( rule < _LAST_MACH_OPER, "called with operand rule" );
  State *kid = s->_kids[0];
  assert( kid == NULL || s->_leaf->in(0) == NULL, "internal operands have no control" );

  // Leaf?  And not subsumed?
  if( kid == NULL && !_swallowed[rule] ) {
    mach->add_req( s->_leaf );  // Add leaf pointer
    return;                     // Bail out
  }

  if( s->_leaf->is_Load() ) {
    assert( mem == (Node*)1, "multiple Memories being matched at once?" );
    mem = s->_leaf->in(MemNode::Memory);
    debug_only(_mem_node = s->_leaf;)
  }
  if( s->_leaf->in(0) && s->_leaf->req() > 1) {
    if( !mach->in(0) )
      mach->set_req(0,s->_leaf->in(0));
    else {
      assert( s->_leaf->in(0) == mach->in(0), "same instruction, differing controls?" );
    }
  }

  for( uint i=0; kid != NULL && i<2; kid = s->_kids[1], i++ ) {   // binary tree
    int newrule;
    if( i == 0 )
      newrule = kid->_rule[_leftOp[rule]];
    else
      newrule = kid->_rule[_rightOp[rule]];

    if( newrule < _LAST_MACH_OPER ) { // Operand or instruction?
      // Internal operand; recurse but do nothing else
      ReduceOper( kid, newrule, mem, mach );

    } else {                    // Child is a new instruction
      // Reduce the instruction, and add a direct pointer from this
      // machine instruction to the newly reduced one.
      Node *mem1 = (Node*)1;
      debug_only(Node *save_mem_node = _mem_node;)
      mach->add_req( ReduceInst( kid, newrule, mem1 ) );
      debug_only(_mem_node = save_mem_node;)
    }
  }
}


// -------------------------------------------------------------------------
// Java-Java calling convention
// (what you use when Java calls Java)

//------------------------------find_receiver----------------------------------
// For a given signature, return the OptoReg for parameter 0.
OptoReg::Name Matcher::find_receiver( bool is_outgoing ) {
  VMRegPair regs;
  BasicType sig_bt = T_OBJECT;
  calling_convention(&sig_bt, &regs, 1, is_outgoing);
  // Return argument 0 register.  In the LP64 build pointers
  // take 2 registers, but the VM wants only the 'main' name.
  return OptoReg::as_OptoReg(regs.first());
}

// A method-klass-holder may be passed in the inline_cache_reg
// and then expanded into the inline_cache_reg and a method_oop register
//   defined in ad_<arch>.cpp


//------------------------------find_shared------------------------------------
// Set bits if Node is shared or otherwise a root
void Matcher::find_shared( Node *n ) {
  // Allocate stack of size C->unique() * 2 to avoid frequent realloc
  MStack mstack(C->unique() * 2);
  // Mark nodes as address_visited if they are inputs to an address expression
  VectorSet address_visited(Thread::current()->resource_area());
  mstack.push(n, Visit);     // Don't need to pre-visit root node
  while (mstack.is_nonempty()) {
    n = mstack.node();       // Leave node on stack
    Node_State nstate = mstack.state();
    uint nop = n->Opcode();
    if (nstate == Pre_Visit) {
      if (address_visited.test(n->_idx)) { // Visited in address already?
        // Flag as visited and shared now.
        set_visited(n);
      }
      if (is_visited(n)) {   // Visited already?
        // Node is shared and has no reason to clone.  Flag it as shared.
        // This causes it to match into a register for the sharing.
        set_shared(n);       // Flag as shared and
        mstack.pop();        // remove node from stack
        continue;
      }
      nstate = Visit; // Not already visited; so visit now
    }
    if (nstate == Visit) {
      mstack.set_state(Post_Visit);
      set_visited(n);   // Flag as visited now
      bool mem_op = false;

      switch( nop ) {  // Handle some opcodes special
      case Op_Phi:             // Treat Phis as shared roots
      case Op_Parm:
      case Op_Proj:            // All handled specially during matching
      case Op_SafePointScalarObject:
        set_shared(n);
        set_dontcare(n);
        break;
      case Op_If:
      case Op_CountedLoopEnd:
        mstack.set_state(Alt_Post_Visit); // Alternative way
        // Convert (If (Bool (CmpX A B))) into (If (Bool) (CmpX A B)).  Helps
        // with matching cmp/branch in 1 instruction.  The Matcher needs the
        // Bool and CmpX side-by-side, because it can only get at constants
        // that are at the leaves of Match trees, and the Bool's condition acts
        // as a constant here.
        mstack.push(n->in(1), Visit);         // Clone the Bool
        mstack.push(n->in(0), Pre_Visit);     // Visit control input
        continue; // while (mstack.is_nonempty())
      case Op_ConvI2D:         // These forms efficiently match with a prior
      case Op_ConvI2F:         //   Load but not a following Store
        if( n->in(1)->is_Load() &&        // Prior load
            n->outcnt() == 1 &&           // Not already shared
            n->unique_out()->is_Store() ) // Following store
          set_shared(n);       // Force it to be a root
        break;
      case Op_ReverseBytesI:
      case Op_ReverseBytesL:
        if( n->in(1)->is_Load() &&        // Prior load
            n->outcnt() == 1 )            // Not already shared
          set_shared(n);                  // Force it to be a root
        break;
      case Op_BoxLock:         // Cant match until we get stack-regs in ADLC
      case Op_IfFalse:
      case Op_IfTrue:
      case Op_MachProj:
      case Op_MergeMem:
      case Op_Catch:
      case Op_CatchProj:
      case Op_CProj:
      case Op_JumpProj:
      case Op_JProj:
      case Op_NeverBranch:
        set_dontcare(n);
        break;
      case Op_Jump:
        mstack.push(n->in(1), Visit);         // Switch Value
        mstack.push(n->in(0), Pre_Visit);     // Visit Control input
        continue;                             // while (mstack.is_nonempty())
      case Op_StrComp:
      case Op_AryEq:
        set_shared(n); // Force result into register (it will be anyways)
        break;
      case Op_ConP: {  // Convert pointers above the centerline to NUL
        TypeNode *tn = n->as_Type(); // Constants derive from type nodes
        const TypePtr* tp = tn->type()->is_ptr();
        if (tp->_ptr == TypePtr::AnyNull) {
          tn->set_type(TypePtr::NULL_PTR);
        }
        break;
      }
      case Op_ConN: {  // Convert narrow pointers above the centerline to NUL
        TypeNode *tn = n->as_Type(); // Constants derive from type nodes
        const TypePtr* tp = tn->type()->make_ptr();
        if (tp && tp->_ptr == TypePtr::AnyNull) {
          tn->set_type(TypeNarrowOop::NULL_PTR);
        }
        break;
      }
      case Op_Binary:         // These are introduced in the Post_Visit state.
        ShouldNotReachHere();
        break;
      case Op_StoreB:         // Do match these, despite no ideal reg
      case Op_StoreC:
      case Op_StoreCM:
      case Op_StoreD:
      case Op_StoreF:
      case Op_StoreI:
      case Op_StoreL:
      case Op_StoreP:
      case Op_StoreN:
      case Op_Store16B:
      case Op_Store8B:
      case Op_Store4B:
      case Op_Store8C:
      case Op_Store4C:
      case Op_Store2C:
      case Op_Store4I:
      case Op_Store2I:
      case Op_Store2L:
      case Op_Store4F:
      case Op_Store2F:
      case Op_Store2D:
      case Op_ClearArray:
      case Op_SafePoint:
        mem_op = true;
        break;
      case Op_LoadB:
      case Op_LoadUS:
      case Op_LoadD:
      case Op_LoadF:
      case Op_LoadI:
      case Op_LoadKlass:
      case Op_LoadNKlass:
      case Op_LoadL:
      case Op_LoadS:
      case Op_LoadP:
      case Op_LoadN:
      case Op_LoadRange:
      case Op_LoadD_unaligned:
      case Op_LoadL_unaligned:
      case Op_Load16B:
      case Op_Load8B:
      case Op_Load4B:
      case Op_Load4C:
      case Op_Load2C:
      case Op_Load8C:
      case Op_Load8S:
      case Op_Load4S:
      case Op_Load2S:
      case Op_Load4I:
      case Op_Load2I:
      case Op_Load2L:
      case Op_Load4F:
      case Op_Load2F:
      case Op_Load2D:
        mem_op = true;
        // Must be root of match tree due to prior load conflict
        if( C->subsume_loads() == false ) {
          set_shared(n);
        }
        // Fall into default case
      default:
        if( !n->ideal_reg() )
          set_dontcare(n);  // Unmatchable Nodes
      } // end_switch

      for(int i = n->req() - 1; i >= 0; --i) { // For my children
        Node *m = n->in(i); // Get ith input
        if (m == NULL) continue;  // Ignore NULLs
        uint mop = m->Opcode();

        // Must clone all producers of flags, or we will not match correctly.
        // Suppose a compare setting int-flags is shared (e.g., a switch-tree)
        // then it will match into an ideal Op_RegFlags.  Alas, the fp-flags
        // are also there, so we may match a float-branch to int-flags and
        // expect the allocator to haul the flags from the int-side to the
        // fp-side.  No can do.
        if( _must_clone[mop] ) {
          mstack.push(m, Visit);
          continue; // for(int i = ...)
        }

        // Clone addressing expressions as they are "free" in most instructions
        if( mem_op && i == MemNode::Address && mop == Op_AddP ) {
          if (m->in(AddPNode::Base)->Opcode() == Op_DecodeN) {
            // Bases used in addresses must be shared but since
            // they are shared through a DecodeN they may appear
            // to have a single use so force sharing here.
            set_shared(m->in(AddPNode::Base)->in(1));
          }

          // Some inputs for address expression are not put on stack
          // to avoid marking them as shared and forcing them into register
          // if they are used only in address expressions.
          // But they should be marked as shared if there are other uses
          // besides address expressions.

          Node *off = m->in(AddPNode::Offset);
          if( off->is_Con() &&
              // When there are other uses besides address expressions
              // put it on stack and mark as shared.
              !is_visited(m) ) {
            address_visited.test_set(m->_idx); // Flag as address_visited
            Node *adr = m->in(AddPNode::Address);

            // Intel, ARM and friends can handle 2 adds in addressing mode
            if( clone_shift_expressions && adr->is_AddP() &&
                // AtomicAdd is not an addressing expression.
                // Cheap to find it by looking for screwy base.
                !adr->in(AddPNode::Base)->is_top() &&
                // Are there other uses besides address expressions?
                !is_visited(adr) ) {
              address_visited.set(adr->_idx); // Flag as address_visited
              Node *shift = adr->in(AddPNode::Offset);
              // Check for shift by small constant as well
              if( shift->Opcode() == Op_LShiftX && shift->in(2)->is_Con() &&
                  shift->in(2)->get_int() <= 3 &&
                  // Are there other uses besides address expressions?
                  !is_visited(shift) ) {
                address_visited.set(shift->_idx); // Flag as address_visited
                mstack.push(shift->in(2), Visit);
                Node *conv = shift->in(1);
#ifdef _LP64
                // Allow Matcher to match the rule which bypass
                // ConvI2L operation for an array index on LP64
                // if the index value is positive.
                if( conv->Opcode() == Op_ConvI2L &&
                    conv->as_Type()->type()->is_long()->_lo >= 0 &&
                    // Are there other uses besides address expressions?
                    !is_visited(conv) ) {
                  address_visited.set(conv->_idx); // Flag as address_visited
                  mstack.push(conv->in(1), Pre_Visit);
                } else
#endif
                mstack.push(conv, Pre_Visit);
              } else {
                mstack.push(shift, Pre_Visit);
              }
              mstack.push(adr->in(AddPNode::Address), Pre_Visit);
              mstack.push(adr->in(AddPNode::Base), Pre_Visit);
            } else {  // Sparc, Alpha, PPC and friends
              mstack.push(adr, Pre_Visit);
            }

            // Clone X+offset as it also folds into most addressing expressions
            mstack.push(off, Visit);
            mstack.push(m->in(AddPNode::Base), Pre_Visit);
            continue; // for(int i = ...)
          } // if( off->is_Con() )
        }   // if( mem_op &&
        mstack.push(m, Pre_Visit);
      }     // for(int i = ...)
    }
    else if (nstate == Alt_Post_Visit) {
      mstack.pop(); // Remove node from stack
      // We cannot remove the Cmp input from the Bool here, as the Bool may be
      // shared and all users of the Bool need to move the Cmp in parallel.
      // This leaves both the Bool and the If pointing at the Cmp.  To
      // prevent the Matcher from trying to Match the Cmp along both paths
      // BoolNode::match_edge always returns a zero.

      // We reorder the Op_If in a pre-order manner, so we can visit without
      // accidentally sharing the Cmp (the Bool and the If make 2 users).
      n->add_req( n->in(1)->in(1) ); // Add the Cmp next to the Bool
    }
    else if (nstate == Post_Visit) {
      mstack.pop(); // Remove node from stack

      // Now hack a few special opcodes
      switch( n->Opcode() ) {       // Handle some opcodes special
      case Op_StorePConditional:
      case Op_StoreIConditional:
      case Op_StoreLConditional:
      case Op_CompareAndSwapI:
      case Op_CompareAndSwapL:
      case Op_CompareAndSwapP:
      case Op_CompareAndSwapN: {   // Convert trinary to binary-tree
        Node *newval = n->in(MemNode::ValueIn );
        Node *oldval  = n->in(LoadStoreNode::ExpectedIn);
        Node *pair = new (C, 3) BinaryNode( oldval, newval );
        n->set_req(MemNode::ValueIn,pair);
        n->del_req(LoadStoreNode::ExpectedIn);
        break;
      }
      case Op_CMoveD:              // Convert trinary to binary-tree
      case Op_CMoveF:
      case Op_CMoveI:
      case Op_CMoveL:
      case Op_CMoveN:
      case Op_CMoveP: {
        // Restructure into a binary tree for Matching.  It's possible that
        // we could move this code up next to the graph reshaping for IfNodes
        // or vice-versa, but I do not want to debug this for Ladybird.
        // 10/2/2000 CNC.
        Node *pair1 = new (C, 3) BinaryNode(n->in(1),n->in(1)->in(1));
        n->set_req(1,pair1);
        Node *pair2 = new (C, 3) BinaryNode(n->in(2),n->in(3));
        n->set_req(2,pair2);
        n->del_req(3);
        break;
      }
      default:
        break;
      }
    }
    else {
      ShouldNotReachHere();
    }
  } // end of while (mstack.is_nonempty())
}

#ifdef ASSERT
// machine-independent root to machine-dependent root
void Matcher::dump_old2new_map() {
  _old2new_map.dump();
}
#endif

//---------------------------collect_null_checks-------------------------------
// Find null checks in the ideal graph; write a machine-specific node for
// it.  Used by later implicit-null-check handling.  Actually collects
// either an IfTrue or IfFalse for the common NOT-null path, AND the ideal
// value being tested.
void Matcher::collect_null_checks( Node *proj, Node *orig_proj ) {
  Node *iff = proj->in(0);
  if( iff->Opcode() == Op_If ) {
    // During matching If's have Bool & Cmp side-by-side
    BoolNode *b = iff->in(1)->as_Bool();
    Node *cmp = iff->in(2);
    int opc = cmp->Opcode();
    if (opc != Op_CmpP && opc != Op_CmpN) return;

    const Type* ct = cmp->in(2)->bottom_type();
    if (ct == TypePtr::NULL_PTR ||
        (opc == Op_CmpN && ct == TypeNarrowOop::NULL_PTR)) {

      bool push_it = false;
      if( proj->Opcode() == Op_IfTrue ) {
        extern int all_null_checks_found;
        all_null_checks_found++;
        if( b->_test._test == BoolTest::ne ) {
          push_it = true;
        }
      } else {
        assert( proj->Opcode() == Op_IfFalse, "" );
        if( b->_test._test == BoolTest::eq ) {
          push_it = true;
        }
      }
      if( push_it ) {
        _null_check_tests.push(proj);
        Node* val = cmp->in(1);
#ifdef _LP64
        if (UseCompressedOops && !Matcher::clone_shift_expressions &&
            val->bottom_type()->isa_narrowoop()) {
          //
          // Look for DecodeN node which should be pinned to orig_proj.
          // On platforms (Sparc) which can not handle 2 adds
          // in addressing mode we have to keep a DecodeN node and
          // use it to do implicit NULL check in address.
          //
          // DecodeN node was pinned to non-null path (orig_proj) during
          // CastPP transformation in final_graph_reshaping_impl().
          //
          uint cnt = orig_proj->outcnt();
          for (uint i = 0; i < orig_proj->outcnt(); i++) {
            Node* d = orig_proj->raw_out(i);
            if (d->is_DecodeN() && d->in(1) == val) {
              val = d;
              val->set_req(0, NULL); // Unpin now.
              break;
            }
          }
        }
#endif
        _null_check_tests.push(val);
      }
    }
  }
}

//---------------------------validate_null_checks------------------------------
// Its possible that the value being NULL checked is not the root of a match
// tree.  If so, I cannot use the value in an implicit null check.
void Matcher::validate_null_checks( ) {
  uint cnt = _null_check_tests.size();
  for( uint i=0; i < cnt; i+=2 ) {
    Node *test = _null_check_tests[i];
    Node *val = _null_check_tests[i+1];
    if (has_new_node(val)) {
      // Is a match-tree root, so replace with the matched value
      _null_check_tests.map(i+1, new_node(val));
    } else {
      // Yank from candidate list
      _null_check_tests.map(i+1,_null_check_tests[--cnt]);
      _null_check_tests.map(i,_null_check_tests[--cnt]);
      _null_check_tests.pop();
      _null_check_tests.pop();
      i-=2;
    }
  }
}


// Used by the DFA in dfa_sparc.cpp.  Check for a prior FastLock
// acting as an Acquire and thus we don't need an Acquire here.  We
// retain the Node to act as a compiler ordering barrier.
bool Matcher::prior_fast_lock( const Node *acq ) {
  Node *r = acq->in(0);
  if( !r->is_Region() || r->req() <= 1 ) return false;
  Node *proj = r->in(1);
  if( !proj->is_Proj() ) return false;
  Node *call = proj->in(0);
  if( !call->is_Call() || call->as_Call()->entry_point() != OptoRuntime::complete_monitor_locking_Java() )
    return false;

  return true;
}

// Used by the DFA in dfa_sparc.cpp.  Check for a following FastUnLock
// acting as a Release and thus we don't need a Release here.  We
// retain the Node to act as a compiler ordering barrier.
bool Matcher::post_fast_unlock( const Node *rel ) {
  Compile *C = Compile::current();
  assert( rel->Opcode() == Op_MemBarRelease, "" );
  const MemBarReleaseNode *mem = (const MemBarReleaseNode*)rel;
  DUIterator_Fast imax, i = mem->fast_outs(imax);
  Node *ctrl = NULL;
  while( true ) {
    ctrl = mem->fast_out(i);            // Throw out-of-bounds if proj not found
    assert( ctrl->is_Proj(), "only projections here" );
    ProjNode *proj = (ProjNode*)ctrl;
    if( proj->_con == TypeFunc::Control &&
        !C->node_arena()->contains(ctrl) ) // Unmatched old-space only
      break;
    i++;
  }
  Node *iff = NULL;
  for( DUIterator_Fast jmax, j = ctrl->fast_outs(jmax); j < jmax; j++ ) {
    Node *x = ctrl->fast_out(j);
    if( x->is_If() && x->req() > 1 &&
        !C->node_arena()->contains(x) ) { // Unmatched old-space only
      iff = x;
      break;
    }
  }
  if( !iff ) return false;
  Node *bol = iff->in(1);
  // The iff might be some random subclass of If or bol might be Con-Top
  if (!bol->is_Bool())  return false;
  assert( bol->req() > 1, "" );
  return (bol->in(1)->Opcode() == Op_FastUnlock);
}

// Used by the DFA in dfa_xxx.cpp.  Check for a following barrier or
// atomic instruction acting as a store_load barrier without any
// intervening volatile load, and thus we don't need a barrier here.
// We retain the Node to act as a compiler ordering barrier.
bool Matcher::post_store_load_barrier(const Node *vmb) {
  Compile *C = Compile::current();
  assert( vmb->is_MemBar(), "" );
  assert( vmb->Opcode() != Op_MemBarAcquire, "" );
  const MemBarNode *mem = (const MemBarNode*)vmb;

  // Get the Proj node, ctrl, that can be used to iterate forward
  Node *ctrl = NULL;
  DUIterator_Fast imax, i = mem->fast_outs(imax);
  while( true ) {
    ctrl = mem->fast_out(i);            // Throw out-of-bounds if proj not found
    assert( ctrl->is_Proj(), "only projections here" );
    ProjNode *proj = (ProjNode*)ctrl;
    if( proj->_con == TypeFunc::Control &&
        !C->node_arena()->contains(ctrl) ) // Unmatched old-space only
      break;
    i++;
  }

  for( DUIterator_Fast jmax, j = ctrl->fast_outs(jmax); j < jmax; j++ ) {
    Node *x = ctrl->fast_out(j);
    int xop = x->Opcode();

    // We don't need current barrier if we see another or a lock
    // before seeing volatile load.
    //
    // Op_Fastunlock previously appeared in the Op_* list below.
    // With the advent of 1-0 lock operations we're no longer guaranteed
    // that a monitor exit operation contains a serializing instruction.

    if (xop == Op_MemBarVolatile ||
        xop == Op_FastLock ||
        xop == Op_CompareAndSwapL ||
        xop == Op_CompareAndSwapP ||
        xop == Op_CompareAndSwapN ||
        xop == Op_CompareAndSwapI)
      return true;

    if (x->is_MemBar()) {
      // We must retain this membar if there is an upcoming volatile
      // load, which will be preceded by acquire membar.
      if (xop == Op_MemBarAcquire)
        return false;
      // For other kinds of barriers, check by pretending we
      // are them, and seeing if we can be removed.
      else
        return post_store_load_barrier((const MemBarNode*)x);
    }

    // Delicate code to detect case of an upcoming fastlock block
    if( x->is_If() && x->req() > 1 &&
        !C->node_arena()->contains(x) ) { // Unmatched old-space only
      Node *iff = x;
      Node *bol = iff->in(1);
      // The iff might be some random subclass of If or bol might be Con-Top
      if (!bol->is_Bool())  return false;
      assert( bol->req() > 1, "" );
      return (bol->in(1)->Opcode() == Op_FastUnlock);
    }
    // probably not necessary to check for these
    if (x->is_Call() || x->is_SafePoint() || x->is_block_proj())
      return false;
  }
  return false;
}

//=============================================================================
//---------------------------State---------------------------------------------
State::State(void) {
#ifdef ASSERT
  _id = 0;
  _kids[0] = _kids[1] = (State*)(intptr_t) CONST64(0xcafebabecafebabe);
  _leaf = (Node*)(intptr_t) CONST64(0xbaadf00dbaadf00d);
  //memset(_cost, -1, sizeof(_cost));
  //memset(_rule, -1, sizeof(_rule));
#endif
  memset(_valid, 0, sizeof(_valid));
}

#ifdef ASSERT
State::~State() {
  _id = 99;
  _kids[0] = _kids[1] = (State*)(intptr_t) CONST64(0xcafebabecafebabe);
  _leaf = (Node*)(intptr_t) CONST64(0xbaadf00dbaadf00d);
  memset(_cost, -3, sizeof(_cost));
  memset(_rule, -3, sizeof(_rule));
}
#endif

#ifndef PRODUCT
//---------------------------dump----------------------------------------------
void State::dump() {
  tty->print("\n");
  dump(0);
}

void State::dump(int depth) {
  for( int j = 0; j < depth; j++ )
    tty->print("   ");
  tty->print("--N: ");
  _leaf->dump();
  uint i;
  for( i = 0; i < _LAST_MACH_OPER; i++ )
    // Check for valid entry
    if( valid(i) ) {
      for( int j = 0; j < depth; j++ )
        tty->print("   ");
        assert(_cost[i] != max_juint, "cost must be a valid value");
        assert(_rule[i] < _last_Mach_Node, "rule[i] must be valid rule");
        tty->print_cr("%s  %d  %s",
                      ruleName[i], _cost[i], ruleName[_rule[i]] );
      }
  tty->print_cr("");

  for( i=0; i<2; i++ )
    if( _kids[i] )
      _kids[i]->dump(depth+1);
}
#endif
