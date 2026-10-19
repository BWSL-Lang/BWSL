// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Result and operand type helpers shared by instruction emission.
#pragma once
#include "bwsl_spirv_backend.cpp"

namespace BWSL {

static CoreType GetFallbackAttributeType(u8 attrIdx);
static CoreType GetFallbackOutputType(u32 slot);

// Helper to get result type for arithmetic operations
u32 SPIRVBuilder::GetResultType(u16 dest_reg, u16 op1_reg) {
  u32 typeId = 0;

  // Check for SPIR-V type override first (for struct array element types etc.)
  if (dest_reg < idCapacity && spirvTypeOverrides[dest_reg] != 0) {
    return spirvTypeOverrides[dest_reg];
  }

  // Try destination register type first
  if (ir->registerTypes && dest_reg < ir->registerCount) {
    CoreType regType = static_cast<CoreType>(ir->registerTypes[dest_reg]);
    if (regType != CoreType::VOID && regType != CoreType::INVALID) {
      if (regType == CoreType::CUSTOM || regType == CoreType::ENUM) {
        if (ir->registerStructTypes) {
          u32 structHash = ir->registerStructTypes[dest_reg];
          if (structHash != 0) {
            typeId = GetStructTypeId(structHash);
            if (typeId != 0)
              return typeId;
          }
        }
      } else {
        typeId = GetTypeId(regType);
        if (typeId != 0)
          return typeId;
      }
    }
  }

    // Fallback: use first operand's type
    if (IR::IsFloatOrBoolConstant(op1_reg)) {
        // Float constant
        return GetTypeId(CoreType::FLOAT);
    } else if (IR::IsIntConstant(op1_reg)) {
        // Int constant
        return GetTypeId(CoreType::INT);
    } else if (IR::IsUintConstant(op1_reg)) {
        // Uint constant
        return GetTypeId(CoreType::UINT);
    } else if (ir->registerTypes && op1_reg < ir->registerCount) {
        CoreType op1Type = static_cast<CoreType>(ir->registerTypes[op1_reg]);
        if (op1Type == CoreType::CUSTOM || op1Type == CoreType::ENUM) {
            if (ir->registerStructTypes) {
                u32 structHash = ir->registerStructTypes[op1_reg];
                if (structHash != 0) {
                    typeId = GetStructTypeId(structHash);
                    if (typeId != 0)
                        return typeId;
                }
            }
        } else {
            typeId = GetTypeId(op1Type);
            if (typeId != 0)
                return typeId;
        }
    }

  // Ultimate fallback: float is the most common type in shaders
  return GetTypeId(CoreType::FLOAT);
}

CoreType SPIRVBuilder::GetOperandType(u16 reg) {
    if (IR::IsBoolConstant(reg) || reg == IR::UnusedOperand)
        return CoreType::BOOL; // Bool constant
    if (IR::IsFloatConstant(reg))
        return CoreType::FLOAT; // Float constant
    if (IR::IsIntConstant(reg))
        return CoreType::INT; // Int constant
    if (IR::IsUintConstant(reg))
        return CoreType::UINT; // Uint constant

  if (ir->registerTypes && reg < ir->registerCount) {
    return static_cast<CoreType>(ir->registerTypes[reg]);
  }
  return CoreType::FLOAT;
}

// XOR and the shifts have always read registerTypes at the first operand's
// low 14 bits. For a constant that is the register numbered like its pool
// index (plus 0x2000 for uint ones), so a constant first operand takes some
// register's signedness. Reading the same register keeps their output.
CoreType SPIRVBuilder::GetBitwiseLegacyType(u16 reg) {
  u32 slot = 0x3FFF;  // The invalid and unused markers
  if (IR::IsRegister(reg)) slot = reg;
  else if (IR::IsIntConstant(reg)) slot = IR::IntConstantIndex(reg);
  else if (IR::IsUintConstant(reg)) slot = 0x2000 | IR::UintConstantIndex(reg);
  else if (IR::IsFloatConstant(reg)) slot = IR::FloatConstantIndex(reg);
  else if (IR::IsBoolConstant(reg)) slot = IR::BoolConstantIndex(reg);
  if (!ir->registerTypes || slot >= ir->registerCount) return CoreType::VOID;
  return static_cast<CoreType>(ir->registerTypes[slot]);
}

CoreType GetScalarComponentType(CoreType vecType) {
  switch (vecType) {
  case CoreType::FLOAT:
  case CoreType::FLOAT2:
  case CoreType::FLOAT3:
  case CoreType::FLOAT4:
    return CoreType::FLOAT;
  case CoreType::INT:
  case CoreType::INT2:
  case CoreType::INT3:
  case CoreType::INT4:
    return CoreType::INT;
  case CoreType::UINT:
  case CoreType::UINT2:
  case CoreType::UINT3:
  case CoreType::UINT4:
    return CoreType::UINT;
  case CoreType::BOOL:
  case CoreType::BOOL2:
  case CoreType::BOOL3:
  case CoreType::BOOL4:
    return CoreType::BOOL;
  case CoreType::CUSTOM:
  case CoreType::ENUM:
    // Struct types don't have scalar components - return FLOAT as a reasonable
    // fallback This can happen during vector construction when type tracking is
    // imperfect
    return CoreType::FLOAT;
  default: {
    // Only print error for truly unexpected types
    fprintf(stderr, "Error: GetScalarComponentType failed for type %u\n",
            vecType);
    return CoreType::FLOAT; // Fallback
  }
  }
}

spv::Op SPIRVBuilder::IRToSpvOp(IR::OpCode op) {
  if (static_cast<u32>(op) < 256) {
    return IR_TO_SPV_OP_TABLE[static_cast<u32>(op)];
  }
  return spv::OpNop;
}


} // namespace BWSL
