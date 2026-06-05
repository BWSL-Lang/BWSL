// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Constant pools, composite constants, section emission, decorations, and names.


#ifdef BWSL_CLANGD
namespace BWSL {
#endif

u32 SPIRVBuilder::GetFloatConstantId(float value) {
  // Check for existing constant
  u32 hash = Utils::HashFloat(value);
  for (u32 i = 0; i < ir->floatCount; i++) {
    if (ir->floatConstants[i] == value && floatConstantIds[i] != 0) {
      return floatConstantIds[i];
    }
  }

  // Create new constant
  u32 const_id = AllocateId();
  u32 type_id = GetTypeId(CoreType::FLOAT);

  // Emit OpConstant
  u32 ops[] = {type_id, const_id, *(u32 *)&value};
  EmitToSection(&typesConstants, spv::OpConstant, ops, 3);

  // Cache it
  for (u32 i = 0; i < ir->floatCount; i++) {
    if (ir->floatConstants[i] == value) {
      floatConstantIds[i] = const_id;
      break;
    }
  }

  return const_id;
}

u32 SPIRVBuilder::GetIntConstantId(u32 value, bool isUnsigned) {
  // Similar to float constants, but handle both signed and unsigned
  // Use separate caches for signed vs unsigned constants
  u32 *cacheArray = isUnsigned ? uintConstantIds : intConstantIds;

  // First check cache
  for (u32 i = 0; i < ir->intCount; i++) {
    if (ir->intConstants[i] == value && cacheArray[i] != 0) {
      return cacheArray[i];
    }
  }

  u32 const_id = AllocateId();
  u32 type_id = GetTypeId(isUnsigned ? CoreType::UINT : CoreType::INT);

  u32 ops[] = {type_id, const_id, value};
  EmitToSection(&typesConstants, spv::OpConstant, ops, 3);

  // Cache the result
  for (u32 i = 0; i < ir->intCount; i++) {
    if (ir->intConstants[i] == value) {
      cacheArray[i] = const_id;
      break;
    }
  }

  return const_id;
}

u32 SPIRVBuilder::GetBoolConstantId(bool value) {
  // Cache bool constants (only two possible values)
  if (value) {
    if (boolTrueId != 0)
      return boolTrueId;
    u32 const_id = AllocateId();
    u32 type_id = GetTypeId(CoreType::BOOL);
    u32 ops[] = {type_id, const_id};
    EmitToSection(&typesConstants, spv::OpConstantTrue, ops, 2);
    boolTrueId = const_id;
    return const_id;
  } else {
    if (boolFalseId != 0)
      return boolFalseId;
    u32 const_id = AllocateId();
    u32 type_id = GetTypeId(CoreType::BOOL);
    u32 ops[] = {type_id, const_id};
    EmitToSection(&typesConstants, spv::OpConstantFalse, ops, 2);
    boolFalseId = const_id;
    return const_id;
  }
}

u32 SPIRVBuilder::GetCompositeConstantId(u32 type_id, u32 *constituents,
                                         u32 count) {
  // TODO: Implement composite constant creation
  u32 const_id = AllocateId();

  u32 *ops = (u32 *)arena->Allocate((2 + count) * sizeof(u32));
  ops[0] = type_id;
  ops[1] = const_id;
  memcpy(&ops[2], constituents, count * sizeof(u32));

  EmitToSection(&typesConstants, spv::OpConstantComposite, ops, 2 + count);
  return const_id;
}

// ============= Section Builders =============
void SPIRVBuilder::EmitToSection(Section *section, spv::Op op, u32 *operands,
                                 u32 operand_count) {
  u32 word_count = 1 + operand_count;

  if (section->count + word_count > section->capacity) {
    GrowSection(section);
  }

  section->words[section->count++] = (word_count << 16) | op;
  memcpy(&section->words[section->count], operands,
         operand_count * sizeof(u32));
  section->count += operand_count;
}

void SPIRVBuilder::EmitCapability(spv::Capability cap) {
  u32 ops[] = {cap};
  EmitToSection(&capabilities, spv::OpCapability, ops, 1);
}

void SPIRVBuilder::EmitExtension(const char *extName) {
  // OpExtension format: word_count | op, "extension_name\0"
  u32 nameLen = strlen(extName) + 1; // Include null terminator
  u32 nameWords = (nameLen + 3) / 4; // Round up to word boundary
  u32 wordCount = 1 + nameWords;     // op + name words

  // Grow section if needed
  if (extensions.count + wordCount > extensions.capacity) {
    GrowSection(&extensions);
  }

  extensions.words[extensions.count++] = (wordCount << 16) | spv::OpExtension;

  // Copy name with padding
  char *namePtr = reinterpret_cast<char *>(&extensions.words[extensions.count]);
  memset(namePtr, 0, nameWords * 4); // Zero-fill for padding
  memcpy(namePtr, extName,
         nameLen - 1); // Copy without null (will be zero from memset)
  extensions.count += nameWords;
}

void SPIRVBuilder::EmitDecoration(u32 id, spv::Decoration decoration,
                                  u32 *params, u32 param_count) {
  u32 *ops = (u32 *)arena->Allocate((2 + param_count) * sizeof(u32));
  ops[0] = id;
  ops[1] = decoration;
  memcpy(&ops[2], params, param_count * sizeof(u32));

  EmitToSection(&decorations, spv::OpDecorate, ops, 2 + param_count);
}

void SPIRVBuilder::EmitMemberDecoration(u32 structTypeId, u32 memberIndex,
                                        spv::Decoration decoration, u32 value) {
  // OpMemberDecorate format: struct_type, member, decoration, [operands]
  u32 word_count = 5; // header + struct + member + decoration + value

  if (decorations.count + word_count > decorations.capacity) {
    GrowSection(&decorations);
  }

  decorations.words[decorations.count++] =
      (word_count << 16) | spv::OpMemberDecorate;
  decorations.words[decorations.count++] = structTypeId;
  decorations.words[decorations.count++] = memberIndex;
  decorations.words[decorations.count++] = static_cast<u32>(decoration);
  decorations.words[decorations.count++] = value;
}

void SPIRVBuilder::EmitName(u32 id, const char *name) {
  if (!name || !name[0])
    return;

  u32 nameLen = strlen(name);
  u32 nameWords = (nameLen + 4) / 4; // Round up including null terminator
  u32 wordCount = 2 + nameWords;     // OpName + target + string words

  if (debugNames.count + wordCount > debugNames.capacity) {
    GrowSection(&debugNames);
  }

  u32 *words = debugNames.words + debugNames.count;
  words[0] = (wordCount << 16) | spv::OpName;
  words[1] = id;
  memset(&words[2], 0, nameWords * 4); // Zero-fill for padding/null terminator
  memcpy(&words[2], name, nameLen);
  debugNames.count += wordCount;
}

void SPIRVBuilder::EmitMemberName(u32 structTypeId, u32 memberIndex,
                                  const char *name) {
  if (!name || !name[0])
    return;

  u32 nameLen = strlen(name);
  u32 nameWords = (nameLen + 4) / 4; // Round up including null terminator
  u32 wordCount =
      3 + nameWords; // OpMemberName + struct + member + string words

  if (debugNames.count + wordCount > debugNames.capacity) {
    GrowSection(&debugNames);
  }

  u32 *words = debugNames.words + debugNames.count;
  words[0] = (wordCount << 16) | spv::OpMemberName;
  words[1] = structTypeId;
  words[2] = memberIndex;
  memset(&words[3], 0, nameWords * 4); // Zero-fill for padding/null terminator
  memcpy(&words[3], name, nameLen);
  debugNames.count += wordCount;
}

// ============= IR Translation =============

// Forward declarations for helpers used in TranslateInstruction


#ifdef BWSL_CLANGD
} // namespace BWSL
#endif
