// Part of bwsl_spirv_backend.cpp. Include from that file only.
// Final module assembly, section growth, ID growth, and composite type hashing.

std::vector<u32> SPIRVBuilder::Finalize() {
  std::vector<u32> spirv;

  // Reserve space for efficiency
  u32 total_size = 5 + // header
                   capabilities.count + extensions.count +
                   extInstImports.count + memoryModel.count +
                   entryPoints.count + executionModes.count + debugNames.count +
                   decorations.count + typesConstants.count + globals.count +
                   functions.count;
  spirv.reserve(total_size);

  // Header
  spirv.push_back(SpvMagicNumber);
  spirv.push_back(spvVersion);
  spirv.push_back(0);      // Generator ID
  spirv.push_back(nextId); // Bound
  spirv.push_back(0);      // Schema

  // Sections in required order
  auto appendSection = [&spirv](const Section &s) {
    spirv.insert(spirv.end(), s.words, s.words + s.count);
  };

  appendSection(capabilities);
  appendSection(extensions);
  appendSection(extInstImports);
  appendSection(memoryModel);
  appendSection(entryPoints);
  appendSection(executionModes);
  appendSection(debugNames);
  appendSection(decorations);
  appendSection(typesConstants);
  appendSection(globals);
  appendSection(functions);

  return spirv;
}

// ============= Memory Management =============
void SPIRVBuilder::GrowSection(Section *section) {
  u32 new_capacity = section->capacity * 2;
  u32 *new_words = (u32 *)arena->Allocate(new_capacity * sizeof(u32), 64);
  memcpy(new_words, section->words, section->count * sizeof(u32));
  section->words = new_words;
  section->capacity = new_capacity;
}

void SPIRVBuilder::GrowCurrentFunction() {
  u32 new_capacity = currentFunctionCapacity * 2;
  u32 *new_func = (u32 *)arena->Allocate(new_capacity * sizeof(u32), 64);
  if (!new_func) {
    // Arena exhausted — pathological fuzz inputs can produce absurdly long
    // SPIR-V function bodies that overflow the fixed 512 KB builder arena.
    // Without this guard, new_func is null, memcpy(nullptr, currentFunction,
    // size) on the first failure corrupts currentFunction, and the second
    // Grow call does memcpy(nullptr, nullptr, N) -> ASan memcpy-param-overlap.
    // Fallback: abandon growth by pointing currentFunction at a tiny static
    // bit-bucket. Subsequent writes stomp on the bucket, producing garbage
    // SPIR-V that SPIR-V validation rejects, but we exit cleanly.
    static u32 bit_bucket[4096];
    currentFunction = bit_bucket;
    currentFunctionCapacity = 4096;
    currentFunctionSize = 0;
    return;
  }
  if (currentFunction && currentFunctionSize > 0) {
    // Clamp the copy to the old buffer's actual size. Call-site checks
    // use `size + needed > capacity` to decide when to Grow, so size itself
    // is allowed to equal capacity; but pathological inputs can (through
    // multi-word instruction paths that skip the check) leave size > old
    // capacity. Copying size words would read past the old buffer, and in
    // an arena allocator the next chunk is often the newly-allocated one
    // -> memcpy-param-overlap.
    u32 toCopy = currentFunctionSize < currentFunctionCapacity
                     ? currentFunctionSize
                     : currentFunctionCapacity;
    memcpy(new_func, currentFunction, toCopy * sizeof(u32));
  }
  currentFunction = new_func;
  currentFunctionCapacity = new_capacity;
}

void SPIRVBuilder::GrowIdArrays() {
  u32 new_capacity = idCapacity * 2;

  u32 *new_spirv_ids = (u32 *)arena->Allocate(new_capacity * sizeof(u32), 64);
  u16 *new_id_types = (u16 *)arena->Allocate(new_capacity * sizeof(u16), 64);
  u32 *new_id_decorations =
      (u32 *)arena->Allocate(new_capacity * sizeof(u32), 64);
  bool *new_has_pre_allocated =
      (bool *)arena->Allocate(new_capacity * sizeof(bool), 64);

  memcpy(new_spirv_ids, spirvIds, idCapacity * sizeof(u32));
  memcpy(new_id_types, idTypes, idCapacity * sizeof(u16));
  memcpy(new_id_decorations, idDecorations, idCapacity * sizeof(u32));
  memcpy(new_has_pre_allocated, hasPreAllocatedId, idCapacity * sizeof(bool));

  memset(&new_spirv_ids[idCapacity], 0,
         (new_capacity - idCapacity) * sizeof(u32));
  memset(&new_id_types[idCapacity], 0,
         (new_capacity - idCapacity) * sizeof(u16));
  memset(&new_id_decorations[idCapacity], 0,
         (new_capacity - idCapacity) * sizeof(u32));
  memset(&new_has_pre_allocated[idCapacity], 0,
         (new_capacity - idCapacity) * sizeof(bool));

  spirvIds = new_spirv_ids;
  idTypes = new_id_types;
  idDecorations = new_id_decorations;
  hasPreAllocatedId = new_has_pre_allocated;
  idCapacity = new_capacity;
}

u32 SPIRVBuilder::HashCompositeType(u32 *components, u32 count) {
  return Utils::HashWords(components, count);
}

