#include "core/bwsl_symbol_table.h"
#include <cstdio>
#include <vector>

using namespace BWSL;

struct Fixture {
    std::vector<u8> memory = std::vector<u8>(8 * 1024 * 1024);
    BWSL_Arena arena;
    SymbolTableData table;

    Fixture() {
        arena.Initialize(memory.data(), memory.size());
        SymbolTable::Init(&table, &arena);
    }
};

static u32 IndexOf(const SymbolTableData& table, const Symbol* symbol) {
    return symbol ? static_cast<u32>(symbol - table.symbols.data) : INVALID_INDEX;
}

// Independent oracle: preserve the original full-array lookup semantics.
static Symbol* LinearLookup(SymbolTableData& table, u32 hash,
                            bool filterNamespace = false,
                            NamespaceKind ns = NamespaceKind::GLOBAL,
                            u32 module = INVALID_INDEX) {
    for (u32 i = table.symbols.count; i > 0;) {
        Symbol& symbol = table.symbols[--i];
        if (symbol.name.nameHash != hash) continue;
        if (filterNamespace && (symbol.namespaceKind != ns ||
            (ns == NamespaceKind::MODULE && symbol.moduleIndex != module))) continue;
        return &symbol;
    }
    return nullptr;
}

static void CheckLookups(SymbolTableData& table, const std::vector<ArenaString>& names) {
    for (const auto& name : names) {
        assert(SymbolTable::LookupByHash(&table, name.nameHash) ==
               LinearLookup(table, name.nameHash));
        for (auto ns : {NamespaceKind::GLOBAL, NamespaceKind::MODULE,
                        NamespaceKind::RESOURCES, NamespaceKind::ATTRIBUTES}) {
            for (u32 module : {INVALID_INDEX, 0u, 1u}) {
                Symbol* expected = LinearLookup(table, name.nameHash, true, ns, module);
                assert(SymbolTable::Lookup(&table, name, ns, module) == expected);
                assert(SymbolTable::FindSymbolInAliasScope(&table, name.nameHash, ns, module) == expected);
            }
        }
        assert(SymbolTable::LookupResource(&table, name) ==
               LinearLookup(table, name.nameHash, true, NamespaceKind::RESOURCES));
    }
}

static void TestCollisionsAndScopeGrowth() {
    Fixture fixture;
    auto& table = fixture.table;
    const auto name = ArenaString::MakeHashOnly("indexed_outer");
    std::vector<ArenaString> names{name};
    // Find two different names in the same bucket; leave the last one absent.
    for (u32 i = 0; names.size() < 3; ++i) {
        auto candidate = ArenaString::MakeHashOnly("bucket_collision_" + std::to_string(i));
        if (SymbolTable::SymbolBucket(&table, candidate.nameHash) ==
            SymbolTable::SymbolBucket(&table, name.nameHash)) names.push_back(candidate);
    }
    CheckLookups(table, names);
    SymbolTable::ExitScope(&table); // Global and empty scopes are no-ops.
    SymbolTable::EnterScope(&table);
    SymbolTable::ExitScope(&table);
    assert(SymbolTable::AddSymbol(&table, name, SymbolKind::PASS));
    assert(SymbolTable::AddSymbol(&table, names[1], SymbolKind::PASS));
    assert(!SymbolTable::AddSymbol(&table, name, SymbolKind::PASS));
    CheckLookups(table, names);
    SymbolTable::EnterScope(&table);
    assert(SymbolTable::AddSymbol(&table, name, SymbolKind::PASS));
    SymbolTable::EnterScope(&table);
    assert(SymbolTable::AddSymbol(&table, names[1], SymbolKind::PASS));
    assert(SymbolTable::AddSymbol(&table, name, SymbolKind::PASS));
    for (u32 i = 0; i < 1200; ++i) {
        names.push_back(ArenaString::MakeHashOnly("growth_" + std::to_string(i)));
        assert(SymbolTable::AddSymbol(&table, names.back(), SymbolKind::PASS));
    }
    assert(table.symbolBucketHeads.count >= 1200);
    CheckLookups(table, names);
    const u32 retainedCapacity = table.symbolBucketHeads.count;
    SymbolTable::ExitScope(&table);
    assert(IndexOf(table, SymbolTable::LookupByHash(&table, name.nameHash)) == 2);
    CheckLookups(table, names);
    SymbolTable::ExitScope(&table);
    assert(IndexOf(table, SymbolTable::LookupByHash(&table, name.nameHash)) == 0);
    assert(table.symbolBucketHeads.count == retainedCapacity);
    CheckLookups(table, names);
    // Reuse truncated slots repeatedly: removed names must never reappear.
    for (u32 i = 0; i < 30; ++i) {
        SymbolTable::EnterScope(&table);
        assert(SymbolTable::AddSymbol(&table, names[i + 3], SymbolKind::PASS));
        CheckLookups(table, names);
        SymbolTable::ExitScope(&table);
    }
    CheckLookups(table, names);
}

static void TestOverloadsAndResources() {
    Fixture fixture;
    auto& table = fixture.table;
    const auto name = ArenaString::MakeHashOnly("indexed_shared");
    const auto floatMask = MakeOverloadMask(CoreType::FLOAT);
    const auto intMask = MakeOverloadMask(CoreType::INT);
    auto addFunction = [&](OverloadTypeMask mask, NamespaceKind ns, u32 module) {
        Symbol* symbol = SymbolTable::AddSymbol(&table, name, SymbolKind::FUNCTION, ns, module);
        assert(symbol);
        table.functions[symbol->index].paramTypeMasks.Push(&fixture.arena, mask);
        return IndexOf(table, symbol);
    };
    const u32 globalFloat = addFunction(floatMask, NamespaceKind::GLOBAL, INVALID_INDEX);
    const u32 globalInt = addFunction(intMask, NamespaceKind::GLOBAL, INVALID_INDEX);
    const u32 moduleFloat = addFunction(floatMask, NamespaceKind::MODULE, 0);
    addFunction(intMask, NamespaceKind::MODULE, 1);
    Symbol* resource = SymbolTable::AddResource(&table, name);
    assert(resource);
    table.resources[resource->index].stageFlags = 1; // Vertex only.
    assert(SymbolTable::AddAttribute(&table, name));
    assert(SymbolTable::ValidateResourceAccess(&table, name, ShaderStage::Vertex));
    assert(!SymbolTable::ValidateResourceAccess(&table, name, ShaderStage::Fragment));
    auto lookup = [&](OverloadTypeMask mask, NamespaceKind ns, u32 module) {
        return IndexOf(table, SymbolTable::LookupFunctionOverloadInNamespace(
            &table, name, &mask, 1, ns, module));
    };
    assert(lookup(floatMask, NamespaceKind::GLOBAL, INVALID_INDEX) == globalFloat);
    assert(lookup(intMask, NamespaceKind::GLOBAL, INVALID_INDEX) == globalInt);
    assert(lookup(floatMask, NamespaceKind::MODULE, 0) == moduleFloat);
    assert(lookup(floatMask, NamespaceKind::MODULE, 1) == INVALID_INDEX);
    SymbolTable::EnterScope(&table);
    const u32 localFloat = addFunction(floatMask, NamespaceKind::GLOBAL, INVALID_INDEX);
    assert(lookup(floatMask, NamespaceKind::GLOBAL, INVALID_INDEX) == localFloat);
    assert(lookup(intMask, NamespaceKind::GLOBAL, INVALID_INDEX) == globalInt);
    SymbolTable::ExitScope(&table);
    assert(lookup(floatMask, NamespaceKind::GLOBAL, INVALID_INDEX) == globalFloat);
    CheckLookups(table, {name});
}

static void TestMixedOperationsAgainstLinearLookup() {
    Fixture fixture;
    auto& table = fixture.table;
    std::vector<ArenaString> names;
    for (u32 i = 0; i < 192; ++i)
        names.push_back(ArenaString::MakeHashOnly("mixed_" + std::to_string(i)));
    // Deterministic random operations cover successful/rejected insertion,
    // shadowing, module ownership, empty scopes, and repeated slot reuse.
    u32 state = 0x13579bdfu;
    auto random = [&]() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    for (u32 step = 0; step < 5000; ++step) {
        const u32 operation = random() % 16;
        if (operation == 0 && table.currentScope < 12) {
            SymbolTable::EnterScope(&table);
        } else if (operation == 1) {
            SymbolTable::ExitScope(&table);
        } else {
            const auto name = names[random() % names.size()];
            const auto ns = static_cast<NamespaceKind>(random() % 4);
            const u32 module = ns == NamespaceKind::MODULE ? random() % 2 : INVALID_INDEX;
            bool duplicate = false;
            for (u32 i = table.scopeStartIndices[table.currentScope]; i < table.symbols.count; ++i) {
                const Symbol& sym = table.symbols[i];
                if (sym.name == name && sym.namespaceKind == ns && sym.moduleIndex == module)
                    duplicate = true;
            }
            assert((SymbolTable::AddSymbol(&table, name, SymbolKind::PASS, ns, module) == nullptr) == duplicate);
        }
        if (step % 32 == 0) CheckLookups(table, names);
    }
    while (table.currentScope > 0) {
        SymbolTable::ExitScope(&table);
        CheckLookups(table, names);
    }
}

int main() {
    TestCollisionsAndScopeGrowth();
    TestOverloadsAndResources();
    TestMixedOperationsAgainstLinearLookup();
    std::printf("Symbol table: collisions, growth, scopes, namespaces, overloads, resources, and differential checks PASS (Symbol: %zu bytes)\n", sizeof(Symbol));
}
