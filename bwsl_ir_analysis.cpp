#include "bwsl_ir_analysis.h"
#include "bwsl_resource_reflection.h"
#include "bwsl_utils.h"
#include <cstring>

namespace BWSL {

static inline void MarkStorageBufferRead(IRAnalysis* analysis, u16 binding) {
  if (binding < 32) {
    analysis->usedStorageBufferMask |= (1u << binding);
    analysis->readStorageBufferMask |= (1u << binding);
  }
  analysis->Set(IRAnalysis::CAP_STORAGE_BUFFER);
}

static inline void MarkStorageBufferWrite(IRAnalysis* analysis, u16 binding) {
  if (binding < 32) {
    analysis->usedStorageBufferMask |= (1u << binding);
    analysis->writeStorageBufferMask |= (1u << binding);
  }
  analysis->Set(IRAnalysis::CAP_STORAGE_BUFFER);
}

static inline void MarkStorageImageRead(IRAnalysis* analysis, u16 binding) {
  if (binding < 32) {
    analysis->usedStorageImageMask |= (1u << binding);
    analysis->readStorageImageMask |= (1u << binding);
  }
  analysis->Set(IRAnalysis::CAP_IMAGE_LOAD);
}

static inline void MarkStorageImageWrite(IRAnalysis* analysis, u16 binding) {
  if (binding < 32) {
    analysis->usedStorageImageMask |= (1u << binding);
    analysis->writeStorageImageMask |= (1u << binding);
  }
  analysis->Set(IRAnalysis::CAP_IMAGE_STORE);
}

// Compute hashes at startup for output name resolution
static const u32 HASH_POSITION = Utils::HashStr("position");
static const u32 HASH_COLOR = Utils::HashStr("color");
static const u32 HASH_DEPTH = Utils::HashStr("depth");
static const u32 HASH_NORMAL = Utils::HashStr("normal");
static const u32 HASH_TEXCOORD = Utils::HashStr("texcoord");
static const u32 HASH_TANGENT = Utils::HashStr("tangent");

// Compute hashes for built-in input names
static const u32 HASH_VERTEX_ID = Utils::HashStr("vertex_id");
static const u32 HASH_INSTANCE_ID = Utils::HashStr("instance_id");

u32 OutputHashToSlot(u32 nameHash) {
  if (nameHash == HASH_POSITION)
    return OutputSlot::POSITION;
  if (nameHash == HASH_COLOR)
    return OutputSlot::COLOR;
  if (nameHash == HASH_DEPTH)
    return OutputSlot::DEPTH;
  if (nameHash == HASH_NORMAL)
    return OutputSlot::VARYING0;
  if (nameHash == HASH_TEXCOORD)
    return OutputSlot::VARYING1;
  if (nameHash == HASH_TANGENT)
    return OutputSlot::VARYING2;
  // Default: use hash to generate a slot in the varying range
  return OutputSlot::VARYING0 + (nameHash % 4);
}

void AnalyzeIR(IRAnalysis *analysis, const IR::IRProgram *ir) {
  memset(analysis, 0, sizeof(IRAnalysis));

  if (!ir || ir->instructionCount == 0)
    return;

  u16* storagePointerBindings = nullptr;
  if (ir->registerCount > 0) {
    storagePointerBindings =
        static_cast<u16*>(alloca(ir->registerCount * sizeof(u16)));
    for (u32 reg = 0; reg < ir->registerCount; reg++) {
      storagePointerBindings[reg] = 0xFFFF;
    }
  }

  for (u32 i = 0; i < ir->instructionCount; i++) {
    u16 op = ir->opcodes[i];

    switch (op) {
    // ========== Attribute/Input Loading ==========
    case IR::OP_LOAD_ATTR: {
      // operand[0] is the attribute index
      u32 attrIdx = ir->GetOperand(i, 0);
      if (attrIdx < 16) {
        analysis->usedAttributeMask |= (1 << attrIdx);
        // Capture type from destination register
        u16 destReg = ir->destinations[i];
        if (destReg < ir->registerCount && ir->registerTypes) {
          analysis->attributeTypes[attrIdx] = ir->registerTypes[destReg];
        }
      }
      break;
    }

    case IR::OP_LOAD_INPUT: {
      // operand[0] is the input slot index
      u32 inputSlot = ir->GetOperand(i, 0);

      // Check for built-in inputs (high slot indices 0x80+)
      if (inputSlot == BuiltinInputSlot::VERTEX_ID) {
        analysis->usedBuiltinInputMask |= IRAnalysis::BUILTIN_VERTEX_ID;
      } else if (inputSlot == BuiltinInputSlot::INSTANCE_ID) {
        analysis->usedBuiltinInputMask |= IRAnalysis::BUILTIN_INSTANCE_ID;
      } else if (inputSlot == BuiltinInputSlot::GLOBAL_INVOCATION_ID) {
        analysis->usedBuiltinInputMask |= IRAnalysis::GLOBAL_ID;
      } else if (inputSlot == BuiltinInputSlot::LOCAL_INVOCATION_ID) {
        analysis->usedBuiltinInputMask |= IRAnalysis::LOCAL_ID;
      } else if (inputSlot == BuiltinInputSlot::WORKGROUP_ID) {
        analysis->usedBuiltinInputMask |= IRAnalysis::WORKGROUP_ID;
      } else if (inputSlot == BuiltinInputSlot::NUM_WORKGROUPS) {
        analysis->usedBuiltinInputMask |= IRAnalysis::NUM_WORKGROUPS;
      } else if (inputSlot == BuiltinInputSlot::LOCAL_INVOCATION_INDEX) {
        analysis->usedBuiltinInputMask |= IRAnalysis::LOCAL_INDEX;
      } else if (inputSlot == BuiltinInputSlot::FRAG_COORD) {
        analysis->usedBuiltinInputMask |= IRAnalysis::BUILTIN_FRAG_COORD;
      } else if (inputSlot < 16) {
        // Fragment varyings (slots 0-15)
        analysis->usedInputMask |= (1 << inputSlot);
        // Capture type from destination register
        u16 destReg = ir->destinations[i];
        if (destReg < ir->registerCount && ir->registerTypes) {
          analysis->inputTypes[inputSlot] = ir->registerTypes[destReg];
        }
      }
      break;
    }

    // ========== Output Storage ==========
    case IR::OP_STORE_OUTPUT: {
      // Slot is now stored in operand[0] (set during IR lowering)
      // This enables dynamic vertex-to-fragment varying resolution
      u32 slot = ir->GetOperand(i, 0);
      if (slot < 32) {
        analysis->usedOutputMask |= (1 << slot);
        // Capture type from destination register (value is in dest for
        // STORE_OUTPUT) Note: EmitInstruction(OP_STORE_OUTPUT, valueReg, slot)
        // puts value in destinations
        u16 srcReg = ir->destinations[i];
        if (srcReg < ir->registerCount && ir->registerTypes) {
          analysis->outputTypes[slot] = ir->registerTypes[srcReg];
        } else if (ir->phiCount > 0 && ir->phiResultRegs) {
          // Check if this is a PHI result register (SSA renaming assigns high
          // IDs)
          for (u32 p = 0; p < ir->phiCount; p++) {
            if (ir->phiResultRegs[p] == srcReg) {
              analysis->outputTypes[slot] = ir->phiTypes[p];
              break;
            }
          }
        }
      }
      break;
    }
    case IR::OP_LOAD_OUTPUT: {
      u32 slot = ir->GetOperand(i, 0);
      if (slot < 32) {
        analysis->usedOutputMask |= (1 << slot);
        u16 destReg = ir->destinations[i];
        if (destReg < ir->registerCount && ir->registerTypes) {
          analysis->outputTypes[slot] = ir->registerTypes[destReg];
        }
      }
      break;
    }

    // ========== Uniform/Buffer Loading ==========
    case IR::OP_LOAD_UNIFORM: {
      // operand[0] is the binding index
      u16 binding = ir->GetOperand(i, 0);
      if (binding < 32) {
        analysis->usedUniformMask |= (1 << binding);
        // Capture type from destination register
        u16 destReg = ir->destinations[i];
        if (destReg < ir->registerCount && ir->registerTypes) {
          analysis->uniformTypes[binding] = ir->registerTypes[destReg];
          // Capture struct type hash for CUSTOM types
          CoreType regType = static_cast<CoreType>(ir->registerTypes[destReg]);
          if ((regType == CoreType::CUSTOM || regType == CoreType::ENUM) &&
              ir->registerStructTypes) {
            analysis->uniformTypeHashes[binding] =
                ir->registerStructTypes[destReg];
          }
        }
      }
      break;
    }

    case IR::OP_LOAD_BUFFER:
    case IR::OP_STORE_BUFFER: {
      // operand[0] is the binding index
      u16 binding = ir->GetOperand(i, 0);
      if (op == IR::OP_LOAD_BUFFER) {
        MarkStorageBufferRead(analysis, binding);
      } else {
        MarkStorageBufferWrite(analysis, binding);
      }
      break;
    }

    case IR::OP_STORAGE_PTR: {
      // operand[0] is the binding index
      u16 binding = ir->GetOperand(i, 0);
      if (binding < 32) {
        analysis->usedStorageBufferMask |= (1u << binding);
        u16 destReg = ir->destinations[i];
        if (storagePointerBindings && destReg < ir->registerCount) {
          storagePointerBindings[destReg] = binding;
        }
      }
      analysis->Set(IRAnalysis::CAP_STORAGE_BUFFER);
      break;
    }

    case IR::OP_STORAGE_FIELD:
    case IR::OP_STORAGE_INDEX: {
      u16 basePtrReg = ir->GetOperand(i, 0);
      u16 destReg = ir->destinations[i];
      if (storagePointerBindings &&
          basePtrReg < ir->registerCount &&
          destReg < ir->registerCount) {
        storagePointerBindings[destReg] = storagePointerBindings[basePtrReg];
      }
      break;
    }

    case IR::OP_STORAGE_LOAD: {
      u16 ptrReg = ir->GetOperand(i, 0);
      if (storagePointerBindings && ptrReg < ir->registerCount) {
        u16 binding = storagePointerBindings[ptrReg];
        if (binding != 0xFFFF) {
          MarkStorageBufferRead(analysis, binding);
        }
      }
      break;
    }

    case IR::OP_ARRAY_LOAD:
    case IR::OP_ARRAY_STORE: {
      u16 baseReg = (op == IR::OP_ARRAY_LOAD)
          ? ir->GetOperand(i, 0)
          : ir->destinations[i];
      if (storagePointerBindings && baseReg < ir->registerCount) {
        u16 binding = storagePointerBindings[baseReg];
        if (binding != 0xFFFF) {
          if (op == IR::OP_ARRAY_LOAD) {
            MarkStorageBufferRead(analysis, binding);
          } else {
            MarkStorageBufferWrite(analysis, binding);
          }
        }
      }
      break;
    }

    // ========== Texture Operations ==========
    case IR::OP_TEX_SAMPLE:
    case IR::OP_TEX_SAMPLE_LOD:
    case IR::OP_TEX_SAMPLE_BIAS:
    case IR::OP_TEX_SAMPLE_GRAD:
    case IR::OP_TEX_SAMPLE_CMP:
    case IR::OP_TEX_SAMPLE_OFFSET:
    case IR::OP_TEX_SAMPLE_LOD_OFFSET:
    case IR::OP_TEX_SAMPLE_BIAS_OFFSET:
    case IR::OP_TEX_GATHER:
    case IR::OP_TEX_GATHER_OFFSET:
    case IR::OP_TEX_FETCH:
    case IR::OP_TEX_FETCH_OFFSET:
    case IR::OP_TEX_SIZE:
    case IR::OP_TEX_LEVELS: {
      if (op == IR::OP_TEX_SIZE || op == IR::OP_TEX_LEVELS) {
        analysis->capabilityFlags |= IRAnalysis::CAP_IMAGE_QUERY;
      }
      if (op == IR::OP_TEX_SAMPLE_OFFSET ||
          op == IR::OP_TEX_SAMPLE_LOD_OFFSET ||
          op == IR::OP_TEX_SAMPLE_BIAS_OFFSET ||
          op == IR::OP_TEX_GATHER_OFFSET ||
          op == IR::OP_TEX_FETCH_OFFSET) {
        analysis->capabilityFlags |= IRAnalysis::CAP_IMAGE_GATHER_EXT;
      }
      // Texture register is encoded as 0x2000 | bindingIndex
      u16 texReg = ir->GetOperand(i, 0);
      if ((texReg & 0xF000) == 0x2000) {
        u16 binding = texReg & 0x0FFF;
        if (binding < 32) {
          analysis->usedTextureMask |= (1 << binding);
        }
      }
      u16 binding = 0xFFFF;
      if (TextureOpHasExplicitSampler(ir->metadata[i])) {
        binding = GetTextureOpExplicitSamplerBinding(ir->metadata[i]);
      } else {
        // Legacy IR used operand 1 for explicit samplers.
        u16 samplerReg = ir->GetOperand(i, 1);
        if ((samplerReg & 0xF000) == 0x3000) {
          binding = samplerReg & 0x0FFF;
        }
      }
      if (binding < 32) {
        analysis->usedSamplerMask |= (1 << binding);
      }
      break;
    }

    // ========== Bindless Texture Handle ==========
    case IR::OP_LOAD_TEX_HANDLE: {
      // Bindless textures don't use fixed binding slots
      // but we still track that textures are used
      u16 binding = ir->GetOperand(i, 0);
      if (binding < 32) {
        analysis->usedTextureMask |= (1 << binding);
      }
      break;
    }

    // ========== Image Operations ==========
    case IR::OP_IMG_LOAD: {
      // Storage image register is encoded as 0x2000 | bindingIndex (same as
      // textures)
      u16 imgReg = ir->GetOperand(i, 0);
      if ((imgReg & 0xF000) == 0x2000) {
        MarkStorageImageRead(analysis, static_cast<u16>(imgReg & 0x0FFF));
      }
      break;
    }

    case IR::OP_IMG_STORE: {
      // Storage image register is encoded as 0x2000 | bindingIndex (same as
      // textures)
      u16 imgReg = ir->GetOperand(i, 0);
      if ((imgReg & 0xF000) == 0x2000) {
        MarkStorageImageWrite(analysis, static_cast<u16>(imgReg & 0x0FFF));
      }
      break;
    }

    // ========== Derivative Operations ==========
    case IR::OP_DDX:
    case IR::OP_DDY:
    case IR::OP_FWIDTH: {
      analysis->Set(IRAnalysis::CAP_DERIVATIVES);
      break;
    }

    case IR::OP_DDX_FINE:
    case IR::OP_DDY_FINE:
    case IR::OP_FWIDTH_FINE: {
      analysis->Set(IRAnalysis::CAP_FINE_DERIVATIVES);
      break;
    }

    case IR::OP_DDX_COARSE:
    case IR::OP_DDY_COARSE:
    case IR::OP_FWIDTH_COARSE: {
      analysis->Set(IRAnalysis::CAP_COARSE_DERIVATIVES);
      break;
    }

    // ========== Wave/Subgroup Operations ==========
    case IR::OP_WAVE_SUM:
    case IR::OP_WAVE_MIN:
    case IR::OP_WAVE_MAX:
    case IR::OP_WAVE_ALL:
    case IR::OP_WAVE_ANY:
    case IR::OP_WAVE_READ_FIRST:
    case IR::OP_WAVE_READ_LANE: {
      analysis->Set(IRAnalysis::CAP_WAVE_OPS);
      break;
    }

    // ========== Atomic Operations ==========
    case IR::OP_ATOMIC_ADD:
    case IR::OP_ATOMIC_MIN:
    case IR::OP_ATOMIC_MAX:
    case IR::OP_ATOMIC_AND:
    case IR::OP_ATOMIC_OR:
    case IR::OP_ATOMIC_XOR:
    case IR::OP_ATOMIC_XCHG:
    case IR::OP_ATOMIC_CMP_XCHG: {
      u16 ptrReg = ir->GetOperand(i, 0);
      if (storagePointerBindings && ptrReg < ir->registerCount) {
        u16 binding = storagePointerBindings[ptrReg];
        if (binding != 0xFFFF) {
          MarkStorageBufferRead(analysis, binding);
          MarkStorageBufferWrite(analysis, binding);
        }
      }
      analysis->Set(IRAnalysis::CAP_ATOMICS);
      break;
    }

    default:
      break;
    }

    // Note: 64-bit types (INT64, UINT64, DOUBLE) are not currently supported
    // in the type system, so we don't check for them here.
    // If added in the future, check ir->types[i] against the new CoreType
    // values.
  }
}

} // namespace BWSL
