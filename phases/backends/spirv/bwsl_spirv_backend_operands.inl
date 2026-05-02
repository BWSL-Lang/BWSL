// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Result and operand type helpers shared by instruction emission.

static CoreType GetFallbackAttributeType(u32 attrIdx);
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
  if (op1_reg & 0x8000) {
    // Float constant
    return GetTypeId(CoreType::FLOAT);
  } else if (op1_reg & 0x4000) {
    // Int constant
    return GetTypeId(CoreType::INT);
  } else if (op1_reg & 0x2000) {
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
  if ((reg & 0xC000) == 0xC000)
    return CoreType::BOOL; // Bool constant
  if (reg & 0x8000)
    return CoreType::FLOAT; // Float constant
  if (reg & 0x4000)
    return CoreType::INT; // Int constant
  if (reg & 0x2000)
    return CoreType::UINT; // Uint constant

  if (ir->registerTypes && reg < ir->registerCount) {
    return static_cast<CoreType>(ir->registerTypes[reg]);
  }
  return CoreType::FLOAT;
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

