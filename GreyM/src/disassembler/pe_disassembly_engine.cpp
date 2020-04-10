#include "pch.h"
#include "pe_disassembly_engine.h"
#include "../utils/safe_instruction.h"

PeDisassemblyEngine::PeDisassemblyEngine( const PortableExecutable pe )
    : pe_( pe ),
      disassembler_handle_( 0 ),
      code_( 0 ),
      current_code_index_( 0 ),
      current_instruction_code_( nullptr ),
      code_buf_size_( 0 ),
      address_( 0 ),
      pe_section_headers_( pe_.GetSectionHeaders() ),
      pe_text_section_header_( pe_section_headers_.FromName( ".text" ) ),
      pe_image_base_( pe_.GetNtHeaders()->OptionalHeader.ImageBase ) {
#ifdef _WIN64
  const cs_mode mode = cs_mode::CS_MODE_64;
#else
  const cs_mode mode = cs_mode::CS_MODE_32;
#endif

  const cs_err cs_status = cs_open( CS_ARCH_X86, mode, &disassembler_handle_ );

  if ( cs_status != cs_err::CS_ERR_OK ) {
    throw std::runtime_error( "cs_open failed with error code " +
                              std::to_string( cs_status ) );
  }

  const cs_err detail_status =
      cs_option( disassembler_handle_, cs_opt_type::CS_OPT_DETAIL, CS_OPT_ON );

  if ( detail_status != cs_err::CS_ERR_OK ) {
    throw std::runtime_error( "cs_option failed with error code " +
                              std::to_string( cs_status ) );
  }
}

void PeDisassemblyEngine::SetDisassemblyPoint(
    const DisassemblyPoint& disasm_point,
    const size_t disasm_buffer_size ) {
  code_ = disasm_point.code;
  address_ = disasm_point.rva;
  code_buf_size_ = disasm_buffer_size;
}

bool IsGuaranteedJump( const cs_insn& instruction ) {
  return instruction.id == x86_insn::X86_INS_JMP ||
         instruction.id == x86_insn::X86_INS_LJMP;
}

uintptr_t GetOperandRva( const cs_x86_op& operand,
                         const uintptr_t image_base ) {
  // in x64 pe, the disp is already the rva
  // x64: mov eax, dword ptr ds:[rcx+rax*4+0x10F2F4], (0x10F2F4 is the RVA)
  // x86: jmp dword ptr ds:[ecx*4+0x12EB9C8], (0x12EB9C8 is the rva + image
  // base)

  switch ( operand.type ) {
    case x86_op_type::X86_OP_IMM:
#ifdef _WIN64
      return static_cast<uintptr_t>( operand.imm );
#else
      return static_cast<uintptr_t>( operand.imm ) - image_base;
#endif
      break;
    case x86_op_type::X86_OP_MEM:
#ifdef _WIN64
      return static_cast<uintptr_t>( operand.mem.disp );
#else
      return static_cast<uintptr_t>( operand.mem.disp ) - image_base;
#endif
      break;
    default:
      break;
  }

  throw std::runtime_error( "should never occur" );
}

bool PeDisassemblyEngine::IsVTableOrFunction( const cs_x86_op& operand1,
                                              const cs_x86_op& operand2 ) {
  if ( operand1.type == x86_op_type::X86_OP_MEM &&
       operand2.type == x86_op_type::X86_OP_IMM ) {
    const auto dest_section = pe_section_headers_.FromRva(
        GetOperandRva( operand2, pe_image_base_ ) );
    // if the destination is in a section
    if ( dest_section != nullptr )
      return true;
  }

  return false;
}

bool PeDisassemblyEngine::IsJumpTable( const cs_insn& instruction,
                                       const uint8_t* code,
                                       const uint64_t rva ) {
  const auto& detail = instruction.detail->x86;
#ifdef _WIN64
  if ( detail.op_count == 2 ) {
    return IsJumpTableX64( instruction, detail.operands[ 0 ],
                           detail.operands[ 1 ], code, rva );
  }
#else
  if ( detail.op_count == 1 ) {
    return IsJumpTableX86( instruction, detail.operands[ 0 ] );
  }
#endif

  return false;
}

bool PeDisassemblyEngine::IsJumpTableX86( const cs_insn& instruction,
                                          const cs_x86_op& operand ) {
  // check if the jump is a jump table
  if ( IsGuaranteedJump( instruction ) ||
       instruction.id == x86_insn::X86_INS_MOV ) {
    if ( operand.type == x86_op_type::X86_OP_MEM &&
         operand.mem.scale == sizeof( uint32_t ) ) {
      const auto jump_table_rva = GetOperandRva( operand, pe_image_base_ );

      // is the jump table within the text section?
      if ( section::IsRvaWithinSection(
               *pe_text_section_header_,
               static_cast<uintptr_t>( jump_table_rva ) ) ) {
        return true;
      }
    }
  }

  return false;
}

bool PeDisassemblyEngine::IsJumpTableX64( const cs_insn& instruction,
                                          const cs_x86_op& operand1,
                                          const cs_x86_op& operand2,
                                          const uint8_t* code,
                                          const uint64_t rva ) {
  /*
    ja test executable.7FF6D311F222
    movsxd rax,dword ptr ss:[rbp+174]
    lea rcx,qword ptr ds:[7FF6D3010000]
    * mov eax,dword ptr ds:[rcx+rax*4+10F2F4]
    * add rax,rcx
    * jmp rax
  */

  // x64 jump table example: mov eax, dword ptr ds:[rcx+rax*4+0x10F2F4]
  if ( instruction.id == x86_insn::X86_INS_MOV ) {
    if ( operand1.type == x86_op_type::X86_OP_REG ) {
      if ( operand2.type == x86_op_type::X86_OP_MEM &&
           operand2.mem.scale == sizeof( uint32_t ) ) {
        // disassemble the next 2 instructions
        assert( ( code_buf_size_ - current_code_index_ ) > 0 );

        auto code_copy = code + instruction.size;
        auto code_size = code_buf_size_ - current_code_index_;
        uint64_t rva_copy = rva + static_cast<uint64_t>( instruction.size );

        cs_insn* instruction2 = cs_malloc( disassembler_handle_ );

        // absolutely disgusting way to do this, however, it work, whatever
        // set to free 1 instruction
        SafeInstructions safe_instruction = 1;
        safe_instruction.SetInstructions( instruction2 );

        // disassemble first instruction
        auto disasm_status =
            cs_disasm_iter( disassembler_handle_, &code_copy, &code_size,
                            &rva_copy, instruction2 );

        if ( instruction2->id != x86_insn::X86_INS_ADD )
          return false;

        if ( instruction2->detail->x86.op_count != 2 )
          return false;

        if ( instruction2->detail->x86.operands[ 0 ].type !=
                 x86_op_type::X86_OP_REG ||
             instruction2->detail->x86.operands[ 1 ].type !=
                 x86_op_type::X86_OP_REG )
          return false;

        const auto saved_add_operand1 =
            instruction2->detail->x86.operands[ 0 ].reg;

        // disassemble second instruction
        disasm_status = cs_disasm_iter( disassembler_handle_, &code_copy,
                                        &code_size, &rva_copy, instruction2 );

        // check if we are jumping to the previously used register in MOV
        if ( IsGuaranteedJump( *instruction2 ) &&
             instruction2->detail->x86.operands[ 0 ].type ==
                 x86_op_type::X86_OP_REG &&
             instruction2->detail->x86.operands[ 0 ].reg == saved_add_operand1 )
          return true;
      }
    }
  }
  return false;
}

DisassemblyPoint
PeDisassemblyEngine::GetOperandDestinationValueDisassasemblyPoint(
    const cs_insn& instruction,
    const uint8_t* instruction_code_ptr,
    const uintptr_t rva ) {
  const auto operand_dest_rva = rva;

  const uint8_t* operand_dest_code = nullptr;

  // In the MOV instruction, the rva we are looking for is an absolute rva,
  // therefore we convert the value to a file offset manually
  if ( instruction.id == x86_insn::X86_INS_MOV ) {
    const auto pe_image_code_ptr = pe_.GetPeImagePtr();
    const auto file_offset =
        pe_section_headers_.RvaToFileOffset( operand_dest_rva );
    operand_dest_code = pe_image_code_ptr + file_offset;
  } else {
    // push or jmp
    const auto dest_delta = operand_dest_rva - instruction.address;
    operand_dest_code = instruction_code_ptr + dest_delta;
  }

  assert( operand_dest_code != nullptr );

  DisassemblyPoint disasm_point;
  disasm_point.rva = operand_dest_rva;
  disasm_point.code = const_cast<uint8_t*>( operand_dest_code );

  return disasm_point;
}

void PeDisassemblyEngine::ParseJumpTable( const cs_insn& instruction,
                                          const cs_x86_op& operand ) {
  assert( operand.type == x86_op_type::X86_OP_MEM );

  const auto operand_rva = GetOperandRva( operand, pe_image_base_ );

  AddressRange jump_table_address_range;

  jump_table_address_range.begin_address = operand_rva;

  int i = 0;

  for ( ;; i += operand.mem.scale ) {
    // #ifndef _WIN64
    const auto jump_table_disasm_point =
        GetOperandDestinationValueDisassasemblyPoint(
            instruction, current_instruction_code_, operand_rva );
    //#else
    //    // in x64 mode, the jump tables are MOV's and not JMP's
    //    // meaning that the jump table address is not relative, but absolute
    //    const auto dest_delta = operand_rva - instruction.address;
    //
    //    auto operand_dest_code = current_instruction_code_ + dest_delta;
    //
    //    DisassemblyPoint jump_table_disasm_point;
    //    jump_table_disasm_point.rva = dest_delta;
    //    jump_table_disasm_point.code = const_cast<uint8_t*>( operand_dest_code
    //    );
    //#endif

    const auto jump_table_dest_section =
        pe_section_headers_.FromRva( jump_table_disasm_point.rva );

    // is the jump table located inside any section?
    if ( jump_table_dest_section == nullptr )
      break;

    // is the target function/address within the text section?
    // if ( !pe::IsRvaWithinSection( *pe_text_section_,
    // jump_table_disasm_point.rva) )
    //   break;

    const auto jump_table_code_dest = jump_table_disasm_point.code + i;

    // if the scale is different, then we line below this line won't work
    assert( operand.mem.scale == sizeof( uint32_t ) );

    const auto item_dest_va =
        *reinterpret_cast<const uint32_t*>( jump_table_code_dest );

    // did we reach the end?
    if ( item_dest_va == 0xCCCCCCCC || item_dest_va == 0 )
      break;

#ifdef _WIN64
    const auto item_dest_rva = item_dest_va;  // x64: item_dest_va is also rva,
        // need not subtract image base
#else
    const auto item_dest_rva =
        item_dest_va - pe_image_base_;  // x86: item_dest_va is rva + image base
#endif

    // is the target function/address within the text section?
    if ( !section::IsRvaWithinSection( *pe_text_section_header_,
                                       item_dest_rva ) )
      break;

    const auto item_dest_delta = item_dest_rva - instruction.address;
    const auto item_dest_code = current_instruction_code_ + item_dest_delta;

    DisassemblyPoint disasm_point;
    disasm_point.rva = item_dest_rva;
    disasm_point.code = reinterpret_cast<const uint8_t*>( item_dest_code );

    AddDisassemblyPoint( disasm_point );
  }

  jump_table_address_range.end_address = operand_rva + i;

  data_ranges_.push_back( jump_table_address_range );
}

// checks if the current address that is being disassembled is within a part
// of data within the code section, example a jump table
bool PeDisassemblyEngine::IsAddressWithinDataSectionOfCode(
    const uint64_t address ) {
  for ( const auto& range : data_ranges_ ) {
    // if the current address is within a data section of the .text
    // section example a jump table
    if ( address >= range.begin_address && address < range.end_address ) {
      return true;
    }
  }

  return false;
};

bool PeDisassemblyEngine::IsFunction( const uint8_t* code,
                                      const uintptr_t rva ) {
#ifdef _WIN64
  return IsFunctionX64( code, rva );
#else
  return IsFunctionX86( code, rva, 0 );
#endif
}

bool PeDisassemblyEngine::IsFunctionX86( const uint8_t* code,
                                         const uintptr_t rva,
                                         int recursion_counter ) {
  // we only try to follow jumps 10 times deep
  if ( recursion_counter > 10 )
    return false;

  cs_insn* instructions = nullptr;

  assert( ( code_buf_size_ - current_code_index_ ) > 0 );

  const auto kDisassembleInstructionCount = 3;

  SafeInstructions disassembled_instructions_count = cs_disasm(
      disassembler_handle_, code, code_buf_size_ - current_code_index_, rva,
      kDisassembleInstructionCount, &instructions );

  disassembled_instructions_count.SetInstructions( instructions );

  if ( disassembled_instructions_count.GetDisassembledInstructionCount() !=
       kDisassembleInstructionCount ) {
    return false;
  }

  auto instruction1 = &instructions[ 0 ];
  auto instruction2 = &instructions[ 1 ];

  if ( IsGuaranteedJump( *instruction1 ) ) {
    const auto& operand = instruction1->detail->x86.operands[ 0 ];
    const auto jump_target_rva = operand.imm;
    const auto jump_dest_disasm_point =
        GetOperandDestinationValueDisassasemblyPoint(
            *instruction1, code, static_cast<uintptr_t>( jump_target_rva ) );
    if ( !section::IsRvaWithinSection( *pe_text_section_header_,
                                       jump_dest_disasm_point.rva ) )
      return false;

    return IsFunctionX86( jump_dest_disasm_point.code,
                          jump_dest_disasm_point.rva, ++recursion_counter );
  }

  // if the first instruction is mov edi, edi
  if ( instruction1->id == x86_insn::X86_INS_MOV &&
       instruction1->detail->x86.op_count == 2 &&
       instruction1->detail->x86.operands[ 0 ].reg == x86_reg::X86_REG_EDI &&
       instruction1->detail->x86.operands[ 1 ].reg == x86_reg::X86_REG_EDI ) {
    instruction1 = &instructions[ 1 ];
    instruction2 = &instructions[ 2 ];
  }

  if ( instruction1->detail->x86.op_count != 1 )
    return false;

  if ( instruction1->id != x86_insn::X86_INS_PUSH ||
       instruction1->detail->x86.operands[ 0 ].reg != x86_reg::X86_REG_EBP )
    return false;

  if ( instruction2->id != x86_insn::X86_INS_MOV ||
       instruction2->detail->x86.op_count != 2 ) {
    return false;
  }

  if ( instruction2->detail->x86.operands[ 0 ].reg != x86_reg::X86_REG_EBP ||
       instruction2->detail->x86.operands[ 1 ].reg != x86_reg::X86_REG_ESP ) {
    return false;
  }

  return true;
}

bool PeDisassemblyEngine::IsFunctionX64( const uint8_t* code,
                                         const uintptr_t rva ) {
  /*
    mov qword ptr ss:[rsp + 18], r8
    mov qword ptr ss:[rsp + 10], rdx
    mov qword ptr ss:[rsp + 8], rcx
    movs [...]
    ..
    sub rsp, imm

    if the function begins with a mov
    then the first mov uses rsp + imm
    that imm should be (imm % 8 == 0)

    if imm > 8
      imm -= 8;

    check next mov

    redo

    Function Example:
      mov byte ptr ss:[rsp+20],r9b
      mov byte ptr ss:[rsp+18],r8b
      mov qword ptr ss:[rsp+10],rdx
      mov qword ptr ss:[rsp+8],rcx
      push rbp
      push rdi
      sub rsp,218
  */

  assert( ( code_buf_size_ - current_code_index_ ) > 0 );

  auto code_copy = code;
  auto code_size = code_buf_size_ - current_code_index_;
  uint64_t rva_copy = rva;

  cs_insn* instruction = cs_malloc( disassembler_handle_ );

  // absolutely disgusting way to do this, however, it work, whatever
  // set to free 1 instruction
  SafeInstructions safe_instruction = 1;
  safe_instruction.SetInstructions( instruction );

  // disassemble first instruction
  auto disasm_status = cs_disasm_iter( disassembler_handle_, &code_copy,
                                       &code_size, &rva_copy, instruction );

  // if the disassembly attempt failed, then invalid instruction
  if ( !disasm_status )
    return false;

  if ( IsGuaranteedJump( *instruction ) ) {
    const auto& operand = instruction->detail->x86.operands[ 0 ];
    const auto jump_target_rva = operand.imm;
    const auto jump_dest_disasm_point =
        GetOperandDestinationValueDisassasemblyPoint(
            *instruction, code, static_cast<uintptr_t>( jump_target_rva ) );

    if ( !section::IsRvaWithinSection( *pe_text_section_header_,
                                       jump_dest_disasm_point.rva ) )
      return false;

    return IsFunctionX64( jump_dest_disasm_point.code,
                          jump_dest_disasm_point.rva );
  }

  // checks if an instruction is mov [rsp + ?], reg
  const auto is_instruction_mov_rsp_plus_disp_reg =
      []( const cs_insn* instruction ) -> bool {
    // is mov op, op
    if ( instruction->id == x86_insn::X86_INS_MOV &&
         instruction->detail->x86.op_count == 2 ) {
      const auto operand1 = &instruction->detail->x86.operands[ 0 ];

      // is mov [rsp + ?], op
      if ( operand1->type == x86_op_type::X86_OP_MEM &&
           operand1->mem.base == x86_reg::X86_REG_RSP ) {
        const auto op1_value = operand1->mem.disp;

        // if the disp is 0, then we missing the + ? in the above instruction
        // comment
        assert( op1_value != 0 );

        const auto operand2 = &instruction->detail->x86.operands[ 1 ];

        // is mov [rsp + ?], reg
        if ( operand2->type == x86_op_type::X86_OP_REG ) {
          return true;
        }
      }
    }

    return false;
  };

  uint32_t total_movs_in_order = 0;

  if ( !is_instruction_mov_rsp_plus_disp_reg( instruction ) )
    return false;

  const auto op1_value = instruction->detail->x86.operands[ 0 ].mem.disp;

  // is it divisible by 8?
  if ( op1_value % 8 == 0 ) {
    total_movs_in_order = static_cast<uint32_t>(
        op1_value / 8 - 1 );  // 8 is the size of a uint64_t
  } else {
    return false;
  }

  // disassemble the following movs calculacted by the first mov instruction
  for ( uint32_t i = 0; i < total_movs_in_order; ++i ) {
    auto disasm_status = cs_disasm_iter( disassembler_handle_, &code_copy,
                                         &code_size, &rva_copy, instruction );

    if ( !disasm_status )
      return false;

    // is the instruction a correct mov?
    if ( !is_instruction_mov_rsp_plus_disp_reg( instruction ) )
      return false;
  }

  // disassemble until we reach a sub rsp, imm instruction
  // limit on 10 instructions
  for ( int i = 0; i < 10; ++i ) {
    auto disasm_status = cs_disasm_iter( disassembler_handle_, &code_copy,
                                         &code_size, &rva_copy, instruction );

    if ( !disasm_status )
      return false;

    if ( instruction->id == x86_insn::X86_INS_SUB ) {
      if ( instruction->detail->x86.op_count == 2 ) {
        const auto& op1 = instruction->detail->x86.operands[ 0 ];
        if ( op1.type == x86_op_type::X86_OP_REG &&
             op1.reg == x86_reg::X86_REG_RSP ) {
          const auto& op2 = instruction->detail->x86.operands[ 1 ];
          if ( op2.type == x86_op_type::X86_OP_IMM ) {
            return true;
          }
        }
      }
    }
  }

  return false;
}

DisassemblyAction PeDisassemblyEngine::ParseInstruction(
    const cs_insn& instruction ) {
  const bool is_ret = cs_insn_group( disassembler_handle_, &instruction,
                                     cs_group_type::CS_GRP_RET );
  const bool is_interrupt = cs_insn_group( disassembler_handle_, &instruction,
                                           cs_group_type::CS_GRP_INT );
  const bool is_jump = cs_insn_group( disassembler_handle_, &instruction,
                                      cs_group_type::CS_GRP_JUMP );
  const bool is_call = cs_insn_group( disassembler_handle_, &instruction,
                                      cs_group_type::CS_GRP_CALL );

  const auto& ins_detail = instruction.detail->x86;

  if ( is_ret ) {
    // if the instruction is a return
    return DisassemblyAction::NextDisassemblyPoint;
  } else if ( is_call || is_jump ) {
    if ( ins_detail.op_count == 1 ) {
      const auto& operand = ins_detail.operands[ 0 ];

      if ( operand.type == x86_op_type::X86_OP_IMM ) {
        const auto dest_delta = operand.imm - instruction.address;

        // since the capstone api automatically increases the code and address
        // after disassembling the instruction, we have to calulcate the
        // original code pointer outselves.
        const uint8_t* instruction_code_ptr = code_ - instruction.size;

        DisassemblyPoint disasm_point;
        disasm_point.rva =
            static_cast<uintptr_t>( instruction.address + dest_delta );
        disasm_point.code =
            const_cast<uint8_t*>( instruction_code_ptr ) + dest_delta;

        AddDisassemblyPoint( disasm_point );

        if ( IsGuaranteedJump( instruction ) ) {
          // go immediately parse the jump destination
          return DisassemblyAction::NextDisassemblyPoint;
        } else {
          // continue disassembling on the next instruction
          return DisassemblyAction::NextInstruction;
        }
      } else if ( IsJumpTable( instruction, current_instruction_code_,
                               instruction.address ) ) {
        ParseJumpTable( instruction, operand );
        return DisassemblyAction::NextDisassemblyPoint;
      }
    } else {
      // invalid instruction, return ti another disassembly point
      return DisassemblyAction::NextDisassemblyPoint;
      // assert( false );
    }

    if ( IsGuaranteedJump( instruction ) ) {
      // go immediately parse the jump destination
      return DisassemblyAction::NextDisassemblyPoint;
    } else {
      // continue on the next instruction
      return DisassemblyAction::NextInstruction;
    }
  } else if ( is_interrupt ) {
    return DisassemblyAction::NextDisassemblyPoint;
  } else {
    if ( instruction.address == 0x10F1A6 ) {
      int test = 0;
    }

    switch ( instruction.id ) {
      case x86_insn::X86_INS_MOV: {
        switch ( ins_detail.op_count ) {
          case 2: {
            const auto& operand1 = ins_detail.operands[ 0 ];
            const auto& operand2 = ins_detail.operands[ 1 ];

            if ( IsJumpTable( instruction, current_instruction_code_,
                              instruction.address ) ) {
#ifndef _WIN64
              assert( false &&
                      "did we reach this in x86, we're only supposed to be "
                      "here in x64 for jump tables." );
#endif
              ParseJumpTable( instruction, operand2 );
              return DisassemblyAction::NextDisassemblyPoint;
            } else if ( IsVTableOrFunction( operand1, operand2 ) ) {
              // mov mem_op, imm_op
              // if is function

              const auto dest_disasm_point =
                  GetOperandDestinationValueDisassasemblyPoint(
                      instruction, current_instruction_code_,
                      GetOperandRva( operand2, pe_image_base_ ) );

              // if we are not in the text section, then don't even bother
              // checking if it is a function it is most likely a pointer to the
              // .rdata section or something
              if ( section::IsRvaWithinSection( *pe_text_section_header_,
                                                dest_disasm_point.rva ) &&
                   IsFunction( dest_disasm_point.code,
                               dest_disasm_point.rva ) ) {
                AddDisassemblyPoint( dest_disasm_point );
                return DisassemblyAction::NextInstruction;
              } else {
                // if not a function, the maybe a vtable

                // TODO: Fix the function when we even come to a function that
                // has a vtable
                /*
                for ( int i = 0;; i += 4 ) {
                  const auto jump_table_disasm_point =
                      GetOperandDestinationValueDisassasemblyPoint(
                          instruction, current_instruction_code_,
                          GetOperandRva( operand2, pe_image_base_ ) );

                  // if the jump table is not within the text section? really?
                  // what? if ( !pe::IsRvaWithinSection( *pe_text_section_,
                  //                               jump_table_disasm_point.rva )
                  //                               )
                  //   break;

                  if ( pe::GetSectionByRva( pe_sections_,
                                            jump_table_disasm_point.rva ) ==
                       nullptr )
                    break;

                  const auto jump_table_code_dest =
                      jump_table_disasm_point.code + i;

                  const auto item_dest_va = *reinterpret_cast<const uint32_t*>(
                      jump_table_code_dest );

                  // did we reach the end?
                  if ( item_dest_va == 0xCCCCCCCC )
                    break;

#ifdef _WIN64
                  const auto item_dest_rva =
                      item_dest_va;  // x64: item_dest_va is also rva,
                                     // need not subtract image base
#else
                  const auto item_dest_rva =
                      item_dest_va -
                      pe_image_base_;  // x86: item_dest_va is rva + image base
#endif

                  // is the target function/address within the text section?
                  if ( !pe::IsRvaWithinSection( *pe_text_section_,
                                                item_dest_rva ) )
                    break;

                  const auto item_dest_delta =
                      item_dest_rva - instruction.address;
                  const auto item_dest_code =
                      current_instruction_code_ + item_dest_delta;

                  // TODO: add it to the disassembly points array here
                  DisassemblyPoint disasm_point;
                  disasm_point.rva = item_dest_rva;
                  disasm_point.code = const_cast<uint8_t*>( item_dest_code );

                  if ( IsFunction( disasm_point.code, disasm_point.rva ) ) {
                    // it is a jump table with valid functions lol
                    int test = 0;
                  }
                }
                */
              }
            }
          } break;
          default:
            break;
        }
      } break;

      case x86_insn::X86_INS_PUSH: {
        const auto operand = ins_detail.operands[ 0 ];
        if ( operand.type == x86_op_type::X86_OP_IMM ) {
          const auto operand_rva = GetOperandRva( operand, pe_image_base_ );

          if ( section::IsRvaWithinSection( *pe_text_section_header_,
                                            operand_rva ) ) {
            const auto dest_disasm =
                GetOperandDestinationValueDisassasemblyPoint(
                    instruction, current_instruction_code_, operand_rva );

            if ( IsFunction( dest_disasm.code, dest_disasm.rva ) ) {
              AddDisassemblyPoint( dest_disasm );
            }
          }
        }
      } break;

      default:
        break;
    }
  }

  return DisassemblyAction::NextInstruction;
}

bool PeDisassemblyEngine::ContinueFromRedirectionInstructions() {
  // check if there is nothing more to disassemble
  if ( disassembly_points_.size() <= 0 )
    return false;

  const auto next_disasm_point = disassembly_points_.back();

  code_ = next_disasm_point.code;
  address_ = next_disasm_point.rva;

  disassembly_points_.pop_back();

  return true;
}

void PeDisassemblyEngine::ParseRDataSection() {
  const auto rdata_section_header = pe_section_headers_.FromName( ".rdata" );

  if ( rdata_section_header == nullptr ) {
    throw std::runtime_error( ".rdata was not found" );
  }

  // TODO: Check x64 if the function pointer size is 4 or 8 bits

  auto pe_image_ptr = const_cast<uint8_t*>( pe_.GetPeImagePtr() );

  for ( uint32_t i = 0; i < rdata_section_header->SizeOfRawData;
        i += sizeof( uintptr_t ) ) {
    const auto raw_file_data = rdata_section_header->PointerToRawData + i;
    const auto raw_file_data_value =
        *reinterpret_cast<uintptr_t*>( pe_image_ptr + raw_file_data );

    // if the value (to be address) is 0, it is not function pointer
    if ( !raw_file_data_value )
      continue;

    // TODO: Check x64 if we need to remove image base or not
    const auto possible_function_pointer_rva =
        raw_file_data_value - pe_image_base_;

    if ( section::IsRvaWithinSection( *pe_text_section_header_,
                                      possible_function_pointer_rva ) ) {
      const auto possible_function_pointer_offset =
          pe_section_headers_.RvaToFileOffset( possible_function_pointer_rva );

      if ( !possible_function_pointer_offset )
        continue;

      const auto possible_function_pointer_code =
          pe_image_ptr + possible_function_pointer_offset;
      if ( IsFunction( possible_function_pointer_code,
                       possible_function_pointer_rva ) ) {
        DisassemblyPoint disasm_point;
        disasm_point.code =
            const_cast<uint8_t*>( possible_function_pointer_code );
        disasm_point.rva = possible_function_pointer_rva;
        AddDisassemblyPoint( disasm_point );
      } else {
        int test = 0;
      }
    }
  }
}

void PeDisassemblyEngine::AddDisassemblyPoint(
    const DisassemblyPoint& disasm_point ) {
  // if the point of disassembly already exists, do not add it again
  if ( disassembly_points_cache_.find( disasm_point.rva ) !=
       disassembly_points_cache_.end() ) {
    return;
  }

  disassembly_points_.push_back( disasm_point );
  disassembly_points_cache_.insert( disasm_point.rva );
}