// Value ranges, included by bwsl_constant_propagation.cpp.
//
// Every scalar and vector component gets a range: its lowest and highest
// value, whether each end is reached, and whether it can be NaN. Ranges
// start from constants, @bounds on the attributes, uniforms and struct
// fields loads read, and what operations guarantee (`saturate(x)` is in
// [0, 1]), and narrow inside the blocks a comparison's branch leads to.
//
// A value whose range is one number is a constant, which propagation then
// folds (with a CFG, as a pin, and propagation runs again). A min, max,
// clamp, saturate or abs whose operand ranges decide it becomes a copy or a
// cheaper operation, and so does a signed remainder or division whose
// operands aren't negative.
//
// Float ends follow the GPU's rounding: an operation's end is the float
// the GPU would round the exact result to, moved outward only when the
// double computing it wasn't exact. Division and square roots, which GPUs
// don't round correctly, get a few ULPs of slack; functions like sin and
// log aren't modeled.

static bool IsFloatScalar(CoreType scalar) { return scalar == CoreType::FLOAT; }

static void SetRangeFlag(ValueRange& range, u8 flag, bool on) {
    range.flags = on ? range.flags | flag : range.flags & ~flag;
}

static f64 TypeMinimum(CoreType scalar) {
    return scalar == CoreType::INT ? -2147483648.0 : scalar == CoreType::FLOAT ? -INFINITY : 0.0;
}

static f64 TypeMaximum(CoreType scalar) {
    return scalar == CoreType::INT ? 2147483647.0 : scalar == CoreType::UINT ? 4294967295.0
         : scalar == CoreType::BOOL ? 1.0 : INFINITY;
}

static ValueRange FullRange(CoreType scalar) {
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    range.low = TypeMinimum(scalar);
    range.high = TypeMaximum(scalar);
    SetRangeFlag(range, ValueRange::MAY_BE_NAN, IsFloatScalar(scalar));
    return range;
}

static ValueRange PointRange(CoreType scalar, u32 bits) {
    f64 value = scalar == CoreType::FLOAT ? static_cast<f64>(BitsToFloat(bits))
              : scalar == CoreType::INT ? static_cast<f64>(static_cast<s32>(bits))
              : scalar == CoreType::BOOL ? (bits ? 1.0 : 0.0) : static_cast<f64>(bits);
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    range.low = range.high = value;
    return range;
}

// Integers take whole ends within their type, both reached.
static ValueRange Normalize(ValueRange range, CoreType scalar) {
    if (!(range.flags & ValueRange::KNOWN) || IsFloatScalar(scalar)) return range;
    f64 low = (range.flags & ValueRange::LOW_EXCLUSIVE) && std::floor(range.low) == range.low ? range.low + 1.0 : std::ceil(range.low);
    f64 high = (range.flags & ValueRange::HIGH_EXCLUSIVE) && std::floor(range.high) == range.high ? range.high - 1.0 : std::floor(range.high);
    range.low = std::max(low, TypeMinimum(scalar));
    range.high = std::min(high, TypeMaximum(scalar));
    range.flags &= ~(ValueRange::LOW_EXCLUSIVE | ValueRange::HIGH_EXCLUSIVE);
    range.flags &= ~ValueRange::MAY_BE_NAN;
    if (range.low > range.high) range = FullRange(scalar);  // Contradiction: the code can't run
    return range;
}

static ValueRange FromDeclared(const ComponentBounds& bounds, CoreType scalar) {
    ValueRange range = FullRange(scalar);
    if (bounds.flags & (ComponentBounds::HAS_LOW | ComponentBounds::HAS_HIGH)) range.flags &= ~ValueRange::MAY_BE_NAN;
    if (bounds.flags & ComponentBounds::HAS_LOW) {
        range.low = std::max(bounds.low, TypeMinimum(scalar));
        SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, bounds.flags & ComponentBounds::LOW_EXCLUSIVE);
    }
    if (bounds.flags & ComponentBounds::HAS_HIGH) {
        range.high = std::min(bounds.high, TypeMaximum(scalar));
        SetRangeFlag(range, ValueRange::HIGH_EXCLUSIVE, bounds.flags & ComponentBounds::HIGH_EXCLUSIVE);
    }
    return Normalize(range, scalar);
}

// The smallest range holding both.
static ValueRange Hull(const ValueRange& a, const ValueRange& b) {
    if (!(a.flags & ValueRange::KNOWN)) return b;
    if (!(b.flags & ValueRange::KNOWN)) return a;
    ValueRange range = a;
    if (b.low < a.low || (b.low == a.low && !(b.flags & ValueRange::LOW_EXCLUSIVE))) {
        range.low = b.low;
        SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, (b.flags & ValueRange::LOW_EXCLUSIVE) && (b.low < a.low || (a.flags & ValueRange::LOW_EXCLUSIVE)));
    }
    if (b.high > a.high || (b.high == a.high && !(b.flags & ValueRange::HIGH_EXCLUSIVE))) {
        range.high = b.high;
        SetRangeFlag(range, ValueRange::HIGH_EXCLUSIVE, (b.flags & ValueRange::HIGH_EXCLUSIVE) && (b.high > a.high || (a.flags & ValueRange::HIGH_EXCLUSIVE)));
    }
    SetRangeFlag(range, ValueRange::MAY_BE_NAN, (a.flags & ValueRange::MAY_BE_NAN) || (b.flags & ValueRange::MAY_BE_NAN));
    return range;
}

static bool SameRange(const ValueRange& a, const ValueRange& b) {
    return a.flags == b.flags && a.low == b.low && a.high == b.high;
}

static bool IsPoint(const ValueRange& range) {
    return (range.flags & ValueRange::KNOWN) && !(range.flags & ValueRange::MAY_BE_NAN) && range.low == range.high && !(range.flags & ValueRange::LOW_EXCLUSIVE) &&
           !(range.flags & ValueRange::HIGH_EXCLUSIVE) && std::isfinite(range.low);
}

// Whether every number in `a` is below every number in `b` (strictly, or
// with `orEqual`, at most).
static bool IsBelow(const ValueRange& a, const ValueRange& b, bool orEqual) {
    if (a.high < b.low) return true;
    if (a.high > b.low) return false;
    return orEqual || (a.flags & ValueRange::HIGH_EXCLUSIVE) || (b.flags & ValueRange::LOW_EXCLUSIVE);
}

// 1 when the comparison is always true, 0 when always false, -1 otherwise.
// NaN makes every ordered comparison false and != true.
static s32 DecideComparison(u16 op, const ValueRange& a, const ValueRange& b) {
    if (!(a.flags & ValueRange::KNOWN) || !(b.flags & ValueRange::KNOWN)) return -1;
    bool nan = (a.flags & ValueRange::MAY_BE_NAN) || (b.flags & ValueRange::MAY_BE_NAN);
    switch (op) {
        case IR::OP_FLT: case IR::OP_ILT: case IR::OP_ULT:
            if (IsBelow(b, a, true)) return 0;
            return !nan && IsBelow(a, b, false) ? 1 : -1;
        case IR::OP_FLE: case IR::OP_ILE: case IR::OP_ULE:
            if (IsBelow(b, a, false)) return 0;
            return !nan && IsBelow(a, b, true) ? 1 : -1;
        case IR::OP_FGT: case IR::OP_IGT: case IR::OP_UGT:
            if (IsBelow(a, b, true)) return 0;
            return !nan && IsBelow(b, a, false) ? 1 : -1;
        case IR::OP_FGE: case IR::OP_IGE: case IR::OP_UGE:
            if (IsBelow(a, b, false)) return 0;
            return !nan && IsBelow(b, a, true) ? 1 : -1;
        case IR::OP_FEQ: case IR::OP_IEQ:
            if (IsBelow(a, b, false) || IsBelow(b, a, false)) return 0;
            return IsPoint(a) && IsPoint(b) && a.low == b.low ? 1 : -1;
        case IR::OP_FNE: case IR::OP_INE:
            if (IsBelow(a, b, false) || IsBelow(b, a, false)) return 1;
            return IsPoint(a) && IsPoint(b) && a.low == b.low ? 0 : -1;
        default:
            return -1;
    }
}

// ---- Float ends ----

static f64 FloatBelow(f64 value) {
    if (!std::isfinite(value)) return value;
    float f = static_cast<float>(value);
    if (static_cast<f64>(f) > value) f = std::nextafter(f, -INFINITY);
    return f;
}

static f64 FloatAbove(f64 value) {
    if (!std::isfinite(value)) return value;
    float f = static_cast<float>(value);
    if (static_cast<f64>(f) < value) f = std::nextafter(f, INFINITY);
    return f;
}

// The lowest float the GPU can round `exact` to, given `exact` is `value`
// plus `error` (both exact in double, error 0 when value is exact).
static f64 RoundedLow(f64 value, f64 error) {
    if (!std::isfinite(value) || std::isnan(error)) return value;
    if (error < 0.0) value = std::nextafter(value, -INFINITY);
    return FloatBelow(value);
}

static f64 RoundedHigh(f64 value, f64 error) {
    if (!std::isfinite(value) || std::isnan(error)) return value;
    if (error > 0.0) value = std::nextafter(value, INFINITY);
    return FloatAbove(value);
}

static f64 SumError(f64 a, f64 b, f64 sum) {
    f64 bv = sum - a;
    return (a - (sum - bv)) + (b - bv);  // TwoSum
}

// Widens a range by `ulps` float ULPs on both sides, for operations the GPU
// computes less exactly. Their error is relative, so a zero end stays zero
// and an end doesn't change sign.
static ValueRange WidenUlps(ValueRange range, u32 ulps) {
    for (u32 k = 0; k < ulps; k++) {
        if (std::isfinite(range.low) && range.low != 0.0) {
            f64 next = std::nextafter(static_cast<float>(range.low), -INFINITY);
            if (range.low < 0.0 || next > 0.0) range.low = next;
        }
        if (std::isfinite(range.high) && range.high != 0.0) {
            f64 next = std::nextafter(static_cast<float>(range.high), INFINITY);
            if (range.high > 0.0 || next < 0.0) range.high = next;
        }
    }
    range.flags &= ~(ValueRange::LOW_EXCLUSIVE | ValueRange::HIGH_EXCLUSIVE);
    return range;
}

static bool MayBeInfinite(const ValueRange& r) { return r.low == -INFINITY || r.high == INFINITY; }
static bool MayBeZero(const ValueRange& r) { return r.low <= 0.0 && r.high >= 0.0; }

static ValueRange FloatAdd(const ValueRange& a, const ValueRange& b, bool subtract) {
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    f64 bLow = subtract ? -b.high : b.low, bHigh = subtract ? -b.low : b.high;
    bool infinities = (a.high == INFINITY && bLow == -INFINITY) || (a.low == -INFINITY && bHigh == INFINITY);
    if (infinities) return FullRange(CoreType::FLOAT);
    f64 low = a.low + bLow, high = a.high + bHigh;
    range.low = RoundedLow(low, SumError(a.low, bLow, low));
    range.high = RoundedHigh(high, SumError(a.high, bHigh, high));
    SetRangeFlag(range, ValueRange::MAY_BE_NAN, (a.flags & ValueRange::MAY_BE_NAN) || (b.flags & ValueRange::MAY_BE_NAN));
    return range;
}

static ValueRange FloatMultiply(const ValueRange& a, const ValueRange& b) {
    if ((MayBeZero(a) && MayBeInfinite(b)) || (MayBeInfinite(a) && MayBeZero(b))) return FullRange(CoreType::FLOAT);
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    range.low = INFINITY;
    range.high = -INFINITY;
    f64 as[2] = {a.low, a.high}, bs[2] = {b.low, b.high};
    for (f64 x : as) {
        for (f64 y : bs) {
            f64 product = x * y;
            f64 error = std::isfinite(product) ? std::fma(x, y, -product) : 0.0;
            range.low = std::min(range.low, RoundedLow(product, error));
            range.high = std::max(range.high, RoundedHigh(product, error));
        }
    }
    SetRangeFlag(range, ValueRange::MAY_BE_NAN, (a.flags & ValueRange::MAY_BE_NAN) || (b.flags & ValueRange::MAY_BE_NAN));
    return range;
}

static ValueRange FloatDivide(const ValueRange& a, const ValueRange& b) {
    if (MayBeZero(b) || (MayBeInfinite(a) && MayBeInfinite(b))) return FullRange(CoreType::FLOAT);
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    range.low = INFINITY;
    range.high = -INFINITY;
    f64 as[2] = {a.low, a.high}, bs[2] = {b.low, b.high};
    for (f64 x : as) {
        for (f64 y : bs) {
            f64 quotient = x / y;
            range.low = std::min(range.low, FloatBelow(quotient));
            range.high = std::max(range.high, FloatAbove(quotient));
        }
    }
    range = WidenUlps(range, 3);
    SetRangeFlag(range, ValueRange::MAY_BE_NAN, (a.flags & ValueRange::MAY_BE_NAN) || (b.flags & ValueRange::MAY_BE_NAN));
    return range;
}

static ValueRange MinimumRange(const ValueRange& a, const ValueRange& b, bool maximum) {
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    if ((a.flags & ValueRange::MAY_BE_NAN) || (b.flags & ValueRange::MAY_BE_NAN)) {
        range = Hull(a, b);  // GLSL leaves min and max of NaN undefined
        range.flags |= ValueRange::MAY_BE_NAN;
        return range;
    }
    if (!maximum) {
        range.low = std::min(a.low, b.low);
        SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, a.low < b.low ? (a.flags & ValueRange::LOW_EXCLUSIVE) : b.low < a.low ? (b.flags & ValueRange::LOW_EXCLUSIVE)
                           : (a.flags & ValueRange::LOW_EXCLUSIVE) && (b.flags & ValueRange::LOW_EXCLUSIVE));
        range.high = std::min(a.high, b.high);
        SetRangeFlag(range, ValueRange::HIGH_EXCLUSIVE, a.high < b.high ? (a.flags & ValueRange::HIGH_EXCLUSIVE) : b.high < a.high ? (b.flags & ValueRange::HIGH_EXCLUSIVE)
                            : (a.flags & ValueRange::HIGH_EXCLUSIVE) || (b.flags & ValueRange::HIGH_EXCLUSIVE));
    } else {
        range.low = std::max(a.low, b.low);
        SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, a.low > b.low ? (a.flags & ValueRange::LOW_EXCLUSIVE) : b.low > a.low ? (b.flags & ValueRange::LOW_EXCLUSIVE)
                           : (a.flags & ValueRange::LOW_EXCLUSIVE) || (b.flags & ValueRange::LOW_EXCLUSIVE));
        range.high = std::max(a.high, b.high);
        SetRangeFlag(range, ValueRange::HIGH_EXCLUSIVE, a.high > b.high ? (a.flags & ValueRange::HIGH_EXCLUSIVE) : b.high > a.high ? (b.flags & ValueRange::HIGH_EXCLUSIVE)
                            : (a.flags & ValueRange::HIGH_EXCLUSIVE) && (b.flags & ValueRange::HIGH_EXCLUSIVE));
    }
    return range;
}

// ---- Integer ends ----

static ValueRange IntegerResult(f64 low, f64 high, CoreType scalar) {
    // Wrapping past the type's limits leaves any value possible.
    if (low < TypeMinimum(scalar) || high > TypeMaximum(scalar)) return FullRange(scalar);
    ValueRange range = {};
    range.flags |= ValueRange::KNOWN;
    range.low = low;
    range.high = high;
    return range;
}

// A conversion's result from its source's range: an integer to a float
// may round either way, a float to an integer truncates (and NaN or an
// out-of-range value gives anything), and between integers the value
// stays when the other type holds it.
static ValueRange ConvertRange(const ValueRange& source, CoreType from, CoreType to) {
    if (from == to) return source;
    if (!(source.flags & ValueRange::KNOWN) || to == CoreType::BOOL || from == CoreType::INVALID) return FullRange(to);
    if (IsFloatScalar(to)) {
        if (IsFloatScalar(from)) return FullRange(to);
        ValueRange range = {};
        range.flags |= ValueRange::KNOWN;
        range.low = FloatBelow(source.low);
        range.high = FloatAbove(source.high);
        return range;
    }
    if (IsFloatScalar(from)) {
        if (source.flags & ValueRange::MAY_BE_NAN) return FullRange(to);
        return IntegerResult(std::trunc(source.low), std::trunc(source.high), to);
    }
    return IntegerResult(source.low, source.high, to);
}

// ---- Constraints from branches ----

// The constraint a comparison puts on `range` when it holds: `range op bound`.
static ValueRange ApplyRelation(ValueRange range, u16 op, const ValueRange& bound, bool holds, CoreType scalar) {
    if (!(bound.flags & ValueRange::KNOWN)) return range;
    // A comparison that fails says nothing about NaN, so only its ordered
    // half narrows the range.
    u16 relation = op;
    if (!holds) {
        switch (op) {
            case IR::OP_FLT: case IR::OP_ILT: case IR::OP_ULT: relation = IR::OP_FGE; break;
            case IR::OP_FLE: case IR::OP_ILE: case IR::OP_ULE: relation = IR::OP_FGT; break;
            case IR::OP_FGT: case IR::OP_IGT: case IR::OP_UGT: relation = IR::OP_FLE; break;
            case IR::OP_FGE: case IR::OP_IGE: case IR::OP_UGE: relation = IR::OP_FLT; break;
            case IR::OP_FNE: case IR::OP_INE: relation = IR::OP_FEQ; break;
            default: return range;
        }
    } else if (op == IR::OP_FNE || op == IR::OP_INE) {
        return range;
    }
    bool narrowsNaN = holds || !IsFloatScalar(scalar);
    switch (relation) {
        case IR::OP_FLT: case IR::OP_ILT: case IR::OP_ULT:
        case IR::OP_FLE: case IR::OP_ILE: case IR::OP_ULE: {
            bool strict = relation == IR::OP_FLT || relation == IR::OP_ILT || relation == IR::OP_ULT;
            if (bound.high < range.high || (bound.high == range.high && strict && !(range.flags & ValueRange::HIGH_EXCLUSIVE))) {
                range.high = bound.high;
                SetRangeFlag(range, ValueRange::HIGH_EXCLUSIVE, strict || (bound.flags & ValueRange::HIGH_EXCLUSIVE));
            }
            break;
        }
        case IR::OP_FGT: case IR::OP_IGT: case IR::OP_UGT:
        case IR::OP_FGE: case IR::OP_IGE: case IR::OP_UGE: {
            bool strict = relation == IR::OP_FGT || relation == IR::OP_IGT || relation == IR::OP_UGT;
            if (bound.low > range.low || (bound.low == range.low && strict && !(range.flags & ValueRange::LOW_EXCLUSIVE))) {
                range.low = bound.low;
                SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, strict || (bound.flags & ValueRange::LOW_EXCLUSIVE));
            }
            break;
        }
        case IR::OP_FEQ: case IR::OP_IEQ:
            range = ApplyRelation(range, IR::OP_FLE, bound, true, scalar);
            range = ApplyRelation(range, IR::OP_FGE, bound, true, scalar);
            break;
        default:
            return range;
    }
    if (narrowsNaN && holds) range.flags &= ~ValueRange::MAY_BE_NAN;
    if (range.low > range.high) return range;  // The block can't run; anything goes
    return Normalize(range, scalar);
}

static bool IsComparison(u16 op) {
    return (op >= IR::OP_FEQ && op <= IR::OP_UGE);
}

static u16 SwapComparison(u16 op) {
    switch (op) {
        case IR::OP_FLT: return IR::OP_FGT; case IR::OP_FGT: return IR::OP_FLT;
        case IR::OP_FLE: return IR::OP_FGE; case IR::OP_FGE: return IR::OP_FLE;
        case IR::OP_ILT: return IR::OP_IGT; case IR::OP_IGT: return IR::OP_ILT;
        case IR::OP_ILE: return IR::OP_IGE; case IR::OP_IGE: return IR::OP_ILE;
        case IR::OP_ULT: return IR::OP_UGT; case IR::OP_UGT: return IR::OP_ULT;
        case IR::OP_ULE: return IR::OP_UGE; case IR::OP_UGE: return IR::OP_ULE;
        default: return op;
    }
}

static void AddConstraint(ConstantPropagationData* data, u32 block, u16 reg, u16 op, u16 other, bool holds) {
    if (data->constraintCount == data->constraintCapacity) return;
    RangeConstraint& constraint = data->constraints[data->constraintCount];
    constraint.reg = reg;
    constraint.op = op;
    constraint.other = other;
    constraint.holds = holds;
    constraint.parent = data->blockConstraints[block];
    data->blockConstraints[block] = data->constraintCount++;
}

// What `condition` being `holds` says, as constraints on the registers it
// compares: through !, and through && when it holds and || when it doesn't.
static void AddConditionConstraints(ConstantPropagationData* data, u32 block, u16 condition, bool holds,
                                    u32 depth) {
    const IRProgram* ir = data->ir;
    if (depth > 4 || !IsRegisterOperand(data, condition) || data->definitionCounts[condition] != 1) return;
    u32 definition = data->definitions[condition];
    if (definition & PHI_REFERENCE) return;
    u16 op = ir->opcodes[definition];
    u16 a = ir->GetOperand(definition, 0), b = ir->GetOperand(definition, 1);
    if (op == IR::OP_NOT) {
        AddConditionConstraints(data, block, a, !holds, depth + 1);
    } else if ((op == IR::OP_AND && holds) || (op == IR::OP_OR && !holds)) {
        AddConditionConstraints(data, block, a, holds, depth + 1);
        AddConditionConstraints(data, block, b, holds, depth + 1);
    } else if (op == IR::OP_LOAD_REG || op == IR::OP_STORE_REG) {
        AddConditionConstraints(data, block, a, holds, depth + 1);
    } else if (IsComparison(op)) {
        if (IsRegisterOperand(data, a) && GetComponentCount(GetRegisterType(ir, a)) == 1) {
            AddConstraint(data, block, a, op, b, holds);
        }
        if (IsRegisterOperand(data, b) && GetComponentCount(GetRegisterType(ir, b)) == 1) {
            AddConstraint(data, block, b, SwapComparison(op), a, holds);
        }
    }
}

// Each block's constraints: its dominator's, and those of the branch that
// is its only way in.
static void BuildConstraints(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    data->constraintCount = 0;
    for (u32 b = 0; b < cfg->blockCount; b++) data->blockConstraints[b] = NO_DEFINITION;
    if (!cfg->dominatorsComputed) return;
    // Reverse postorder visits a block's dominator first.
    for (u32 k = cfg->blockCount; k-- > 0;) {
        u32 block = cfg->postorder[k];
        if (block >= cfg->blockCount) continue;
        u32 parent = cfg->immediateDominators[block];
        if (parent < cfg->blockCount && parent != block) data->blockConstraints[block] = data->blockConstraints[parent];
        if (cfg->PredecessorCount(block) != 1) continue;
        u32 predecessor = cfg->predecessorData[cfg->predecessorOffsets[block]];
        if (predecessor >= cfg->blockCount) continue;
        u32 last = cfg->lastInst[predecessor];
        if (ir->opcodes[last] != IR::OP_BRANCH) continue;
        u32 trueBlock = ir->GetBranchTrueTarget(last) < ir->instructionCount ? cfg->instToBlock[ir->GetBranchTrueTarget(last)] : NO_BLOCK;
        u32 falseBlock = ir->GetBranchFalseTarget(last) < ir->instructionCount ? cfg->instToBlock[ir->GetBranchFalseTarget(last)] : NO_BLOCK;
        if (trueBlock == falseBlock) continue;
        if (block == trueBlock) AddConditionConstraints(data, block, ir->GetOperand(last, 0), true, 0);
        else if (block == falseBlock) AddConditionConstraints(data, block, ir->GetOperand(last, 0), false, 0);
    }
}

// ---- Operand and instruction ranges ----

// A register's stored range. While the ranges are being computed, one not
// reached yet is unknown, and data->sawUnknownRange records that an
// evaluation read it; in a straight-line walk, a register nothing has
// assigned yet holds anything.
static ValueRange GetStoredRange(const ConstantPropagationData* data, u16 operand, u32 component) {
    if (IR::IsConstant(operand)) {
        ConstantValue value = DecodeConstant(data->ir, operand);
        if (value.state != ValueState::CONSTANT) return FullRange(CoreType::FLOAT);
        return PointRange(value.type, value.bits[0]);
    }
    if (!IsRegisterOperand(data, operand)) return FullRange(CoreType::FLOAT);
    CoreType scalar = GetScalarType(GetRegisterType(data->ir, operand));
    if (scalar == CoreType::INVALID) return FullRange(CoreType::FLOAT);
    if (!data->tracked[operand]) return FullRange(scalar);
    ConstantValue value = GetRegisterValue(data, operand);
    if (component < value.count && ((KnownMask(value) >> component) & 1u)) {
        return PointRange(scalar, value.bits[component]);
    }
    u32 count = data->bitOffsets[operand + 1] - data->bitOffsets[operand];
    if (!data->rangesReady || component >= count || GetMatrixSize(GetRegisterType(data->ir, operand)) != 0) {
        return FullRange(scalar);
    }
    ValueRange range = data->ranges[data->bitOffsets[operand] + component];
    if (range.flags & ValueRange::KNOWN) return range;
    if (data->straightLine) return FullRange(scalar);
    data->sawUnknownRange = true;
    return range;
}

// An operand's range where `block` reads it: its own, narrowed by the
// conditions the block runs under.
static ValueRange GetOperandRange(const ConstantPropagationData* data, u16 operand, u32 component, u32 block) {
    ValueRange range = GetStoredRange(data, operand, component);
    if (data->straightLine || block >= data->cfg->blockCount || IR::IsConstant(operand)) return range;
    CoreType scalar = GetScalarType(GetRegisterType(data->ir, operand));
    for (u32 c = data->blockConstraints[block]; c != NO_DEFINITION; c = data->constraints[c].parent) {
        const RangeConstraint& constraint = data->constraints[c];
        if (constraint.reg != operand) continue;
        // A bound not known yet narrows nothing, and isn't a reason to wait.
        bool saw = data->sawUnknownRange;
        ValueRange bound = GetStoredRange(data, constraint.other, 0);
        data->sawUnknownRange = saw;
        range = ApplyRelation(range, constraint.op, bound, constraint.holds, scalar);
    }
    return range;
}

// The declared bounds a load reads, or null.
static const DeclaredBounds* GetLoadBounds(const ConstantPropagationData* data, u32 inst) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    if (op == IR::OP_LOAD_ATTR) {
        u16 index = ir->GetOperand(inst, 0);
        return index < 16 ? ir->attributeBounds[index] : nullptr;
    }
    if (op == IR::OP_LOAD_UNIFORM) {
        u16 binding = ir->GetOperand(inst, 0);
        return binding < 32 ? ir->uniformBounds[binding] : nullptr;
    }
    if (op == IR::OP_LOAD_INPUT) {
        u16 slot = ir->GetOperand(inst, 0);
        return slot < 32 ? ir->inputBounds[slot] : nullptr;
    }
    if (op == IR::OP_STRUCT_EXTRACT && ir->structFieldBounds && ir->registerStructTypes) {
        u16 object = ir->GetOperand(inst, 0);
        u32 field = ir->GetOperand(inst, 1);
        if (object >= ir->registerCount) return nullptr;
        u32 hash = ir->registerStructTypes[object];
        for (u32 s = 0; hash && s < ir->structTypeCount; s++) {
            const IRProgram::StructTypeInfo& info = ir->structTypes[s];
            if (info.nameHash != hash) continue;
            return field < info.fieldCount && info.fieldOffset + field < ir->structFieldCapacity
                       ? ir->structFieldBounds[info.fieldOffset + field] : nullptr;
        }
    }
    return nullptr;
}

// The largest framebuffer coordinate: above every API's and device's
// framebuffer size limit (D3D12 and Metal 16384, Vulkan devices up to 32768).
constexpr f64 MAX_FRAMEBUFFER_COORDINATE = 65536.0;

// What a built-in input guarantees, from its slot.
static ValueRange GetBuiltinRange(const ConstantPropagationData* data, u32 slot, u32 component, CoreType scalar) {
    const IRProgram* ir = data->ir;
    ValueRange range = FullRange(scalar);
    u32 size = component < 3 ? ir->workgroupSize[component] : 0;
    u32 invocations = ir->workgroupSize[0] * ir->workgroupSize[1] * ir->workgroupSize[2];
    switch (slot) {
        case BuiltinInputSlot::LOCAL_INVOCATION_ID:
            if (size != 0) range.high = size - 1.0;
            return range;
        case BuiltinInputSlot::LOCAL_INVOCATION_INDEX:
            if (invocations != 0) range.high = invocations - 1.0;
            return range;
        case BuiltinInputSlot::NUM_WORKGROUPS:
            range.low = 1.0;  // A dispatch of no workgroups runs no invocation
            return range;
        case BuiltinInputSlot::FRAG_COORD:
            // x and y are inside the framebuffer. Depth can leave [0, 1]
            // with an unrestricted depth range, so z and w keep anything.
            if (component < 2) {
                range.low = 0.0;
                range.high = MAX_FRAMEBUFFER_COORDINATE;
                range.flags &= ~ValueRange::MAY_BE_NAN;
            }
            return range;
        default:
            return range;
    }
}

// The operations EvaluateComponentRange works out; everything else, loads
// included (their operands are slots, not registers), is full.
static bool IsRangeModeled(u16 op) {
    switch (op) {
        case IR::OP_LOAD_REG: case IR::OP_STORE_REG: case IR::OP_VEC_EXTRACT: case IR::OP_VEC_INSERT:
        case IR::OP_VEC_CONSTRUCT: case IR::OP_VEC_SHUFFLE: case IR::OP_SELECT:
        case IR::OP_FADD: case IR::OP_FSUB: case IR::OP_FMUL: case IR::OP_FDIV: case IR::OP_FNEG:
        case IR::OP_FABS: case IR::OP_FMIN: case IR::OP_FMAX: case IR::OP_FCLAMP: case IR::OP_SATURATE:
        case IR::OP_FLOOR: case IR::OP_CEIL: case IR::OP_TRUNC: case IR::OP_ROUND: case IR::OP_FRACT:
        case IR::OP_STEP: case IR::OP_SIGN: case IR::OP_SQRT: case IR::OP_EXP: case IR::OP_EXP2:
        case IR::OP_LENGTH: case IR::OP_DISTANCE: case IR::OP_I2F: case IR::OP_U2F:
        case IR::OP_IADD: case IR::OP_ISUB: case IR::OP_IMUL: case IR::OP_INEG: case IR::OP_IABS:
        case IR::OP_IMIN: case IR::OP_UMIN: case IR::OP_IMAX: case IR::OP_UMAX: case IR::OP_ICLAMP:
        case IR::OP_UCLAMP: case IR::OP_IDIV: case IR::OP_IMOD: case IR::OP_AND: case IR::OP_SHR:
        case IR::OP_ASR: case IR::OP_F2I: case IR::OP_F2U: case IR::OP_I2U: case IR::OP_U2I:
            return true;
        default:
            return IsComparison(op);
    }
}

// One component of an instruction's result, from its operands' ranges in
// `block`. Full where nothing is known.
static ValueRange EvaluateComponentRange(const ConstantPropagationData* data, u32 inst, u32 component, u32 block) {
    const IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    u16 dest = ir->destinations[inst];
    CoreType resultType = GetRegisterType(ir, dest);
    CoreType scalar = GetScalarType(resultType);
    u32 count = GetComponentCount(resultType);
    if (scalar == CoreType::INVALID || GetMatrixSize(resultType) != 0) return FullRange(CoreType::FLOAT);
    auto operand = [&](u32 slot) { return ir->GetOperand(inst, slot); };
    auto width = [&](u16 value) -> u32 {
        return IR::IsConstant(value) ? 1 : IsRegisterOperand(data, value) ? GetComponentCount(GetRegisterType(ir, value)) : 0;
    };
    // An operand's component, or its only one when it's splatted. One of
    // another component type than the result (comparisons aside) says
    // nothing about it: IR like that comes from a type error.
    auto scalarOf = [&](u16 value) {
        return IR::IsConstant(value) ? DecodeConstant(ir, value).type
                                     : IsRegisterOperand(data, value) ? GetScalarType(GetRegisterType(ir, value))
                                                                      : CoreType::INVALID;
    };
    auto at = [&](u32 slot) {
        u16 value = operand(slot);
        if (!IsComparison(op) && op != IR::OP_SELECT && scalarOf(value) != scalar) return FullRange(scalar);
        return GetOperandRange(data, value, width(value) == 1 ? 0 : component, block);
    };
    // The same for the operations that move a component unchanged.
    auto move = [&](u16 value, u32 index) {
        if (scalarOf(value) != scalar) return FullRange(scalar);
        return GetOperandRange(data, value, index, block);
    };
    // A constructor's argument component, converted to the result's type:
    // `int2(q)` converts each component of a float2.
    auto convert = [&](u16 value, u32 index) {
        return ConvertRange(GetOperandRange(data, value, index, block), scalarOf(value), scalar);
    };

    if (const DeclaredBounds* bounds = GetLoadBounds(data, inst)) {
        if (bounds->count == count && component < 4) return FromDeclared(bounds->components[component], scalar);
        return FullRange(scalar);
    }
    if (op == IR::OP_LOAD_INPUT) return GetBuiltinRange(data, operand(0), component, scalar);
    if (!IsRangeModeled(op)) return FullRange(scalar);

    switch (op) {
        case IR::OP_LOAD_REG: case IR::OP_STORE_REG:
            return move(operand(0), component);
        case IR::OP_VEC_EXTRACT:
            return move(operand(0), operand(1));
        case IR::OP_VEC_INSERT:
            return component == operand(1) ? move(operand(2), 0) : move(operand(0), component);
        case IR::OP_VEC_CONSTRUCT: {
            u32 arguments = ir->metadata[inst];
            if (arguments == 0 || arguments > 4) arguments = 4;
            u32 filled = 0, single = 0;
            for (u32 k = 0; k < arguments; k++) {
                u16 value = operand(k);
                if (value == 0xFFFF) continue;
                u32 w = width(value);
                if (w == 0) return FullRange(scalar);
                if (component < filled + w) return convert(value, component - filled);
                filled += w;
                single = k;
            }
            if (filled == 1) return convert(operand(single), 0);  // Splat
            return PointRange(scalar, 0);  // Missing components are zero
        }
        case IR::OP_VEC_SHUFFLE: {
            u16 first = operand(0);
            u32 index = (ir->metadata[inst] >> (component * 4)) & 0xF;
            u32 firstWidth = width(first);
            if (firstWidth == 0) return FullRange(scalar);
            if (index < firstWidth) return move(first, index);
            return count > 1 ? move(operand(1), index - firstWidth) : FullRange(scalar);
        }
        case IR::OP_SELECT: {
            u16 condition = operand(2);
            ValueRange decided = GetOperandRange(data, condition, width(condition) == 1 ? 0 : component, block);
            u32 width0 = width(operand(0)), width1 = width(operand(1));
            ValueRange whenFalse = move(operand(0), width0 == 1 ? 0 : component);
            ValueRange whenTrue = move(operand(1), width1 == 1 ? 0 : component);
            if (IsPoint(decided)) return decided.low != 0.0 ? whenTrue : whenFalse;
            return Hull(whenFalse, whenTrue);
        }
        default:
            break;
    }

    if (IsComparison(op)) {
        s32 decided = DecideComparison(op, at(0), at(1));
        return decided < 0 ? FullRange(CoreType::BOOL) : PointRange(CoreType::BOOL, decided);
    }

    if (IsFloatScalar(scalar)) {
        ValueRange a = at(0);
        switch (op) {
            case IR::OP_FADD: return FloatAdd(a, at(1), false);
            case IR::OP_FSUB: return FloatAdd(a, at(1), true);
            case IR::OP_FMUL:
                if (operand(0) == operand(1) && IsRegisterOperand(data, operand(0))) {
                    // x * x isn't negative.
                    ValueRange square = FloatMultiply(a, a);
                    if (square.low < 0.0) {
                        square.low = 0.0;
                        square.flags &= ~ValueRange::LOW_EXCLUSIVE;
                    }
                    return square;
                }
                return FloatMultiply(a, at(1));
            case IR::OP_FDIV: return FloatDivide(a, at(1));
            case IR::OP_FNEG: {
                ValueRange range = a;
                range.low = -a.high;
                range.high = -a.low;
                SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, a.flags & ValueRange::HIGH_EXCLUSIVE);
                SetRangeFlag(range, ValueRange::HIGH_EXCLUSIVE, a.flags & ValueRange::LOW_EXCLUSIVE);
                return range;
            }
            case IR::OP_FABS: {
                ValueRange range = a;
                if (a.low >= 0.0) return a;
                range.high = std::max(-a.low, a.high);
                range.flags &= ~ValueRange::HIGH_EXCLUSIVE;
                range.low = a.high <= 0.0 ? -a.high : 0.0;
                SetRangeFlag(range, ValueRange::LOW_EXCLUSIVE, a.high <= 0.0 && (a.flags & ValueRange::HIGH_EXCLUSIVE));
                return range;
            }
            case IR::OP_FMIN: return MinimumRange(a, at(1), false);
            case IR::OP_FMAX: return MinimumRange(a, at(1), true);
            case IR::OP_FCLAMP: return MinimumRange(MinimumRange(a, at(1), true), at(2), false);
            case IR::OP_SATURATE: {
                ValueRange range = MinimumRange(MinimumRange(a, PointRange(scalar, FLOAT_ZERO_BITS), true),
                                                PointRange(scalar, FLOAT_ONE_BITS), false);
                if (a.flags & ValueRange::MAY_BE_NAN) {
                    range.low = std::max(range.low, 0.0);
                    range.high = std::min(range.high, 1.0);
                }
                return range;
            }
            case IR::OP_FLOOR: case IR::OP_CEIL: case IR::OP_TRUNC: case IR::OP_ROUND: {
                f64 (*f)(f64) = op == IR::OP_FLOOR ? static_cast<f64 (*)(f64)>(std::floor)
                              : op == IR::OP_CEIL ? static_cast<f64 (*)(f64)>(std::ceil)
                              : op == IR::OP_TRUNC ? static_cast<f64 (*)(f64)>(std::trunc)
                                                   : static_cast<f64 (*)(f64)>(std::nearbyint);
                ValueRange range = a;
                range.low = f(a.low);
                range.high = f(a.high);
                range.flags &= ~(ValueRange::LOW_EXCLUSIVE | ValueRange::HIGH_EXCLUSIVE);
                return range;
            }
            case IR::OP_FRACT: {
                ValueRange range = {};
                range.flags |= ValueRange::KNOWN;
                range.low = 0.0;
                range.high = 1.0;
                SetRangeFlag(range, ValueRange::MAY_BE_NAN, (a.flags & ValueRange::MAY_BE_NAN) || MayBeInfinite(a));
                return range;
            }
            case IR::OP_STEP: {
                // step(edge, x) is 0 when x < edge, else 1.
                s32 below = DecideComparison(IR::OP_FLT, at(1), a);
                if (below == 1) return PointRange(scalar, FLOAT_ZERO_BITS);
                if (below == 0 && !(at(1).flags & ValueRange::MAY_BE_NAN) && !(a.flags & ValueRange::MAY_BE_NAN)) return PointRange(scalar, FLOAT_ONE_BITS);
                ValueRange range = PointRange(scalar, FLOAT_ZERO_BITS);
                range.high = 1.0;
                return range;
            }
            case IR::OP_SIGN: {
                if (!(a.flags & ValueRange::MAY_BE_NAN) && IsBelow(PointRange(scalar, FLOAT_ZERO_BITS), a, false)) return PointRange(scalar, FLOAT_ONE_BITS);
                if (!(a.flags & ValueRange::MAY_BE_NAN) && IsBelow(a, PointRange(scalar, FLOAT_ZERO_BITS), false)) return PointRange(scalar, FLOAT_MINUS_ONE_BITS);
                ValueRange range = PointRange(scalar, FLOAT_MINUS_ONE_BITS);
                range.high = 1.0;
                SetRangeFlag(range, ValueRange::MAY_BE_NAN, a.flags & ValueRange::MAY_BE_NAN);
                return range;
            }
            case IR::OP_SQRT: {
                ValueRange range = {};
                range.flags |= ValueRange::KNOWN;
                SetRangeFlag(range, ValueRange::MAY_BE_NAN, (a.flags & ValueRange::MAY_BE_NAN) || a.low < 0.0);
                range.low = a.low > 0.0 ? FloatBelow(std::sqrt(a.low)) : 0.0;
                range.high = a.high > 0.0 ? FloatAbove(std::sqrt(a.high)) : 0.0;
                range = WidenUlps(range, 3);
                range.low = std::max(range.low, 0.0);
                return range;
            }
            case IR::OP_EXP: case IR::OP_EXP2: case IR::OP_LENGTH: case IR::OP_DISTANCE: {
                ValueRange range = FullRange(scalar);
                range.low = 0.0;
                SetRangeFlag(range, ValueRange::MAY_BE_NAN, op == IR::OP_EXP || op == IR::OP_EXP2 ? (a.flags & ValueRange::MAY_BE_NAN) : true);
                return range;
            }
            case IR::OP_I2F: case IR::OP_U2F:
                return convert(operand(0), width(operand(0)) == 1 ? 0 : component);
            default:
                return FullRange(scalar);
        }
    }

    if (scalar == CoreType::INT || scalar == CoreType::UINT) {
        ValueRange a = at(0);
        switch (op) {
            case IR::OP_IADD: { ValueRange b = at(1); return IntegerResult(a.low + b.low, a.high + b.high, scalar); }
            case IR::OP_ISUB: { ValueRange b = at(1); return IntegerResult(a.low - b.high, a.high - b.low, scalar); }
            case IR::OP_IMUL: {
                ValueRange b = at(1);
                f64 corners[4] = {a.low * b.low, a.low * b.high, a.high * b.low, a.high * b.high};
                return IntegerResult(*std::min_element(corners, corners + 4), *std::max_element(corners, corners + 4), scalar);
            }
            case IR::OP_INEG: return IntegerResult(-a.high, -a.low, scalar);
            case IR::OP_IABS:
                if (a.low >= 0.0) return a;
                if (a.high <= 0.0) return IntegerResult(-a.high, -a.low, scalar);
                return IntegerResult(0.0, std::max(-a.low, a.high), scalar);
            case IR::OP_IMIN: case IR::OP_UMIN: return MinimumRange(a, at(1), false);
            case IR::OP_IMAX: case IR::OP_UMAX: return MinimumRange(a, at(1), true);
            case IR::OP_ICLAMP: case IR::OP_UCLAMP: return MinimumRange(MinimumRange(a, at(1), true), at(2), false);
            case IR::OP_IDIV: {
                ValueRange b = at(1);
                if (a.low < 0.0 || b.low <= 0.0) return FullRange(scalar);
                return IntegerResult(std::floor(a.low / b.high), std::floor(a.high / b.low), scalar);
            }
            case IR::OP_IMOD: {
                ValueRange b = at(1);
                if (a.low < 0.0 || b.low <= 0.0) return FullRange(scalar);
                return IntegerResult(0.0, std::min(a.high, b.high - 1.0), scalar);
            }
            case IR::OP_AND: {
                // A mask that isn't negative bounds the result by itself.
                ValueRange b = at(1);
                f64 high = TypeMaximum(scalar);
                if (a.low >= 0.0) high = std::min(high, a.high);
                if (b.low >= 0.0) high = std::min(high, b.high);
                return high == TypeMaximum(scalar) && scalar == CoreType::INT ? FullRange(scalar)
                                                                              : IntegerResult(0.0, high, scalar);
            }
            case IR::OP_SHR: case IR::OP_ASR: {
                ValueRange b = at(1);
                if (a.low < 0.0 || !IsPoint(b) || b.low < 0.0 || b.low >= 32.0) return FullRange(scalar);
                f64 divisor = std::ldexp(1.0, static_cast<int>(b.low));
                return IntegerResult(std::floor(a.low / divisor), std::floor(a.high / divisor), scalar);
            }
            case IR::OP_F2I: case IR::OP_F2U: case IR::OP_I2U: case IR::OP_U2I:
                return convert(operand(0), width(operand(0)) == 1 ? 0 : component);
            default:
                return FullRange(scalar);
        }
    }
    return FullRange(scalar);
}

// ---- Fixpoint over the CFG ----

constexpr u8 WIDEN_AFTER = 3;

// The range a phi gets: what its executable edges bring, each narrowed by
// the conditions its predecessor runs under.
static ValueRange EvaluatePhiRange(const ConstantPropagationData* data, u32 phi, u32 component) {
    const IRProgram* ir = data->ir;
    u32 block = ir->phiBlockIndices[phi];
    ValueRange range = {};
    for (u32 j = 0; j < ir->GetPhiOperandCount(phi); j++) {
        u32 predecessor = ir->GetPhiOperandBlock(phi, j);
        if (!IsEdgeExecutable(data, predecessor, block)) continue;
        u16 operand = ir->GetPhiOperandValue(phi, j);
        if (IsRegisterOperand(data, operand) && data->undef[operand]) continue;
        // An edge whose value isn't known yet adds nothing for now.
        ValueRange incoming = GetOperandRange(data, operand, component, predecessor);
        if (incoming.flags & ValueRange::KNOWN) range = Hull(range, incoming);
    }
    data->sawUnknownRange = false;
    return range;
}

// Stores `range` as a register's component, widening a phi's end that
// keeps moving. Every cycle runs through a phi, so that ends the iteration,
// and what's computed from the phi keeps what the conditions say about it.
// Returns whether it changed.
static bool StoreRange(ConstantPropagationData* data, u16 reg, u32 component, ValueRange range) {
    ValueRange& stored = data->ranges[data->bitOffsets[reg] + component];
    if (!(range.flags & ValueRange::KNOWN)) return false;
    ValueRange next = Hull(stored, range);
    if (SameRange(next, stored)) return false;
    CoreType scalar = GetScalarType(GetRegisterType(data->ir, reg));
    bool phi = (data->definitions[reg] & PHI_REFERENCE) != 0;
    if (phi && (stored.flags & ValueRange::KNOWN) && data->rangeChanges[reg] >= WIDEN_AFTER) {
        if (next.low < stored.low) next.low = TypeMinimum(scalar);
        if (next.high > stored.high) next.high = TypeMaximum(scalar);
    }
    stored = next;
    if (data->rangeChanges[reg] < 255) data->rangeChanges[reg]++;
    return true;
}

static bool HasRange(const ConstantPropagationData* data, u16 reg) {
    if (!IsRegisterOperand(data, reg) || !data->tracked[reg]) return false;
    CoreType type = GetRegisterType(data->ir, reg);
    return GetScalarType(type) != CoreType::INVALID && GetMatrixSize(type) == 0;
}

// Iterates the executable blocks in reverse postorder until no range
// changes, or gives up after 32 passes. Returns whether it settled.
static bool IterateRanges(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    bool changed = true;
    for (u32 pass = 0; changed && pass < 32; pass++) {
        changed = false;
        for (u32 k = cfg->blockCount; k-- > 0;) {
            u32 block = cfg->postorder[k];
            if (block >= cfg->blockCount || !data->blockExecutable[block]) continue;
            for (u32 p = data->blockPhiOffsets[block]; p < data->blockPhiOffsets[block + 1]; p++) {
                u32 phi = data->blockPhis[p];
                u16 result = ir->phiResultRegs[phi];
                if (!HasRange(data, result)) continue;
                u32 count = data->bitOffsets[result + 1] - data->bitOffsets[result];
                for (u32 c = 0; c < count; c++) changed |= StoreRange(data, result, c, EvaluatePhiRange(data, phi, c));
            }
            for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) {
                u16 dest = ir->destinations[i];
                if (!HasRange(data, dest) || data->definitions[dest] != i) continue;
                u32 count = data->bitOffsets[dest + 1] - data->bitOffsets[dest];
                for (u32 c = 0; c < count; c++) {
                    // An operand not reached yet leaves the result unknown too.
                    data->sawUnknownRange = false;
                    ValueRange range = EvaluateComponentRange(data, i, c, block);
                    if (!data->sawUnknownRange) changed |= StoreRange(data, dest, c, range);
                }
            }
        }
    }
    return !changed;
}

// Ranges for every register the executable blocks define.
static void ComputeRanges(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    const CFG* cfg = data->cfg;
    for (u32 k = 0; k < data->bitOffsets[data->regCount]; k++) data->ranges[k] = {};
    memset(data->rangeChanges, 0, data->regCount);
    BuildConstraints(data);
    data->rangesReady = cfg->dominatorsComputed;
    if (!data->rangesReady) return;
    if (!IterateRanges(data)) {
        data->rangesReady = false;  // Didn't settle: trust none of it
        return;
    }
    // A register still unknown read something that never got a range, and
    // a phi took it for no value at all. Count it as anything, and work the
    // ranges out again with that.
    bool unknown = false;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (!HasRange(data, static_cast<u16>(reg))) continue;
        u32 definition = data->definitions[reg];
        if (definition == NO_DEFINITION) continue;
        u32 block = definition & PHI_REFERENCE ? ir->phiBlockIndices[definition & ~PHI_REFERENCE]
                                               : cfg->instToBlock[definition];
        if (block >= cfg->blockCount || !data->blockExecutable[block]) continue;
        CoreType scalar = GetScalarType(GetRegisterType(ir, static_cast<u16>(reg)));
        for (u32 c = data->bitOffsets[reg]; c < data->bitOffsets[reg + 1]; c++) {
            if ((data->ranges[c].flags & ValueRange::KNOWN)) continue;
            data->ranges[c] = FullRange(scalar);
            data->rangeChanges[reg] = WIDEN_AFTER;
            unknown = true;
        }
    }
    if (unknown && !IterateRanges(data)) data->rangesReady = false;
}

// Pins registers whose range is a single number, so propagation folds them.
// Float zeros aren't: the value could be -0.0. Returns whether any is new.
static bool PinPointRanges(ConstantPropagationData* data) {
    if (!data->rangesReady) return false;
    const IRProgram* ir = data->ir;
    bool any = false;
    for (u32 reg = 0; reg < data->regCount; reg++) {
        if (!HasRange(data, reg) || data->pinned[reg] || data->states[reg] == ValueState::CONSTANT) continue;
        u32 definition = data->definitions[reg];
        if (definition == NO_DEFINITION) continue;
        if (!(definition & PHI_REFERENCE) && !data->blockExecutable[data->cfg->instToBlock[definition]]) continue;
        CoreType type = GetRegisterType(ir, static_cast<u16>(reg));
        CoreType scalar = GetScalarType(type);
        u32 count = data->bitOffsets[reg + 1] - data->bitOffsets[reg];
        u32 bits[MAX_COMPONENTS] = {};
        bool point = true;
        for (u32 c = 0; c < count && point; c++) {
            const ValueRange& range = data->ranges[data->bitOffsets[reg] + c];
            point = IsPoint(range) && !(IsFloatScalar(scalar) && range.low == 0.0);
            if (!point) break;
            if (IsFloatScalar(scalar)) bits[c] = FloatToBits(static_cast<float>(range.low));
            else if (scalar == CoreType::INT) bits[c] = static_cast<u32>(static_cast<s32>(range.low));
            else bits[c] = static_cast<u32>(range.low);
        }
        if (!point) continue;
        if (count == 1 && InternConstant(data->builder, scalar, bits[0]) == 0xFFFF) continue;
        if (definition & PHI_REFERENCE) {
            // A pinned phi takes its constant on every edge (see RewriteConstants).
            if (count != 1) continue;
            data->pinnedPhiOperand[definition & ~PHI_REFERENCE] = InternConstant(data->builder, scalar, bits[0]);
        }
        data->pinned[reg] = true;
        memcpy(data->pinnedBits + data->bitOffsets[reg], bits, count * sizeof(u32));
        any = true;
    }
    return any;
}

// ---- Rewrites ----

// Whether every component of `a` is at most (or below) every component of
// `b`, with neither NaN.
static bool AllAtMost(const ConstantPropagationData* data, u16 a, u16 b, u32 count, u32 block) {
    auto width = [&](u16 value) -> u32 {
        return IR::IsConstant(value) ? 1 : IsRegisterOperand(data, value) ? GetComponentCount(GetRegisterType(data->ir, value)) : 0;
    };
    u32 aw = width(a), bw = width(b);
    if (aw == 0 || bw == 0) return false;
    for (u32 c = 0; c < count; c++) {
        ValueRange ra = GetOperandRange(data, a, aw == 1 ? 0 : c, block);
        ValueRange rb = GetOperandRange(data, b, bw == 1 ? 0 : c, block);
        if (!(ra.flags & ValueRange::KNOWN) || !(rb.flags & ValueRange::KNOWN) || (ra.flags & ValueRange::MAY_BE_NAN) || (rb.flags & ValueRange::MAY_BE_NAN) || !IsBelow(ra, rb, true)) return false;
    }
    return true;
}

// The result is an operand: a copy of a register, or of a constant.
static bool RewriteAsOperand(ConstantPropagationData* data, u32 inst, u16 operand) {
    if (IR::IsConstant(operand)) {
        CoreType type = GetRegisterType(data->ir, data->ir->destinations[inst]);
        if (GetComponentCount(type) != 1) return false;
        MakeConstantCopy(data, inst, operand);
        return true;
    }
    return RewriteAs(data, inst, IR::OP_LOAD_REG, operand);
}

// min, max, clamp, saturate and abs that their operands' ranges decide,
// and signed remainders and divisions of a dividend that isn't negative.
static void RewriteWithRanges(ConstantPropagationData* data, u32 inst, u32 block) {
    IRProgram* ir = data->ir;
    u16 op = ir->opcodes[inst];
    u16 dest = ir->destinations[inst];
    if (!IsRegisterOperand(data, dest)) return;
    CoreType type = GetRegisterType(ir, dest);
    u32 count = GetComponentCount(type);
    if (count == 0 || GetMatrixSize(type) != 0) return;
    u16 a = ir->GetOperand(inst, 0), b = ir->GetOperand(inst, 1), c = ir->GetOperand(inst, 2);
    switch (op) {
        case IR::OP_FMIN: case IR::OP_IMIN: case IR::OP_UMIN:
            if (AllAtMost(data, a, b, count, block) && RewriteAsOperand(data, inst, a)) return;
            if (AllAtMost(data, b, a, count, block)) RewriteAsOperand(data, inst, b);
            return;
        case IR::OP_FMAX: case IR::OP_IMAX: case IR::OP_UMAX:
            if (AllAtMost(data, b, a, count, block) && RewriteAsOperand(data, inst, a)) return;
            if (AllAtMost(data, a, b, count, block)) RewriteAsOperand(data, inst, b);
            return;
        case IR::OP_FCLAMP: case IR::OP_ICLAMP: case IR::OP_UCLAMP: {
            bool aboveLow = AllAtMost(data, b, a, count, block);
            bool belowHigh = AllAtMost(data, a, c, count, block);
            u16 minimum = op == IR::OP_FCLAMP ? IR::OP_FMIN : op == IR::OP_ICLAMP ? IR::OP_IMIN : IR::OP_UMIN;
            u16 maximum = op == IR::OP_FCLAMP ? IR::OP_FMAX : op == IR::OP_ICLAMP ? IR::OP_IMAX : IR::OP_UMAX;
            if (aboveLow && belowHigh) {
                RewriteAsOperand(data, inst, a);
            } else if (AllAtMost(data, a, b, count, block)) {
                RewriteAsOperand(data, inst, b);
            } else if (AllAtMost(data, c, a, count, block)) {
                RewriteAsOperand(data, inst, c);
            } else if (aboveLow) {
                RewriteAs(data, inst, minimum, a, c);
            } else if (belowHigh) {
                RewriteAs(data, inst, maximum, a, b);
            }
            return;
        }
        case IR::OP_SATURATE: {
            u16 zero = InternConstant(data->builder, CoreType::FLOAT, FLOAT_ZERO_BITS);
            u16 one = InternConstant(data->builder, CoreType::FLOAT, FLOAT_ONE_BITS);
            if (zero == 0xFFFF || one == 0xFFFF) return;
            bool aboveZero = AllAtMost(data, zero, a, count, block);
            bool belowOne = AllAtMost(data, a, one, count, block);
            if (aboveZero && belowOne) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            else if (aboveZero) RewriteAs(data, inst, IR::OP_FMIN, a, one);
            else if (belowOne) RewriteAs(data, inst, IR::OP_FMAX, a, zero);
            return;
        }
        case IR::OP_FABS: case IR::OP_IABS: {
            u16 zero = op == IR::OP_FABS ? InternConstant(data->builder, CoreType::FLOAT, FLOAT_ZERO_BITS)
                                         : InternConstant(data->builder, CoreType::INT, 0);
            if (zero == 0xFFFF) return;
            // abs leaves NaN alone, so it may be one.
            bool nonNegative = true, nonPositive = true;
            for (u32 k = 0; k < count; k++) {
                ValueRange range = GetOperandRange(data, a, count == 1 ? 0 : k, block);
                nonNegative &= (range.flags & ValueRange::KNOWN) && range.low >= 0.0;
                nonPositive &= (range.flags & ValueRange::KNOWN) && range.high <= 0.0 &&
                               (op == IR::OP_FABS || range.low > TypeMinimum(CoreType::INT));
            }
            if (nonNegative) RewriteAs(data, inst, IR::OP_LOAD_REG, a);
            else if (nonPositive) RewriteAs(data, inst, op == IR::OP_FABS ? IR::OP_FNEG : IR::OP_INEG, a);
            return;
        }
        case IR::OP_IMOD: case IR::OP_IDIV: {
            // Signed remainder and division truncate toward zero, which
            // GLSL's `%` doesn't promise for negative operands, so the
            // backends expand a remainder into a division. Neither matters
            // when the operands aren't negative.
            if (GetScalarType(type) != CoreType::INT) return;
            u16 zero = InternConstant(data->builder, CoreType::INT, 0);
            if (zero == 0xFFFF || !AllAtMost(data, zero, a, count, block)) return;
            // A power-of-two divisor leaves the low bits, or shifts them out.
            ConstantValue divisor = GetKnownValue(data, b);
            bool known = count == 1 && divisor.state == ValueState::CONSTANT && divisor.type == CoreType::INT;
            s32 d = known ? static_cast<s32>(divisor.bits[0]) : 0;
            if (d > 1 && (d & (d - 1)) == 0) {
                u32 shift = 0;
                while ((1 << shift) != d) shift++;
                u16 operand = InternConstant(data->builder, CoreType::INT, op == IR::OP_IMOD ? static_cast<u32>(d - 1) : shift);
                if (operand != 0xFFFF && RewriteAs(data, inst, op == IR::OP_IMOD ? IR::OP_AND : IR::OP_ASR, a, operand)) return;
            }
            if (op == IR::OP_IMOD && AllAtMost(data, zero, b, count, block)) ir->flags[inst] |= IR::FLAG_NON_NEGATIVE_OPERANDS;
            return;
        }
        default:
            return;
    }
}

// ---- Ranges across stages ----

// Adds a store to a varying to the varying's range.
static void RecordVaryingRange(ConstantPropagationData* data, u32 inst, u32 block) {
    const IRProgram* ir = data->ir;
    if (!ir->varyingBoundsOut || ir->opcodes[inst] != IR::OP_STORE_OUTPUT) return;
    u32 varying = ir->GetOperand(inst, 0) - OutputSlot::VARYING0;
    u16 value = ir->destinations[inst];
    if (varying >= 16 || !IsRegisterOperand(data, value)) return;
    u32 count = GetComponentCount(GetRegisterType(ir, value));
    if (count == 0 || count > 4 || GetMatrixSize(GetRegisterType(ir, value)) != 0) return;
    ValueRange* stored = data->varyingRanges + varying * 4;
    for (u32 c = 0; c < count; c++) stored[c] = Hull(stored[c], GetOperandRange(data, value, c, block));
    data->varyingCounts[varying] = static_cast<u8>(count);
}

static void RecordVaryingRanges(ConstantPropagationData* data) {
    if (!data->ir->varyingBoundsOut || !data->rangesReady) return;
    const CFG* cfg = data->cfg;
    for (u32 block = 0; block < cfg->blockCount; block++) {
        if (!data->blockExecutable[block]) continue;
        for (u32 i = cfg->firstInst[block]; i <= cfg->lastInst[block]; i++) RecordVaryingRange(data, i, block);
    }
}

// Hands the varyings' ranges to the fragment stage, as bounds: a component
// that may be NaN has none.
static void ExportVaryingRanges(ConstantPropagationData* data) {
    const IRProgram* ir = data->ir;
    if (!ir->varyingBoundsOut) return;
    for (u32 v = 0; v < 16; v++) {
        DeclaredBounds& bounds = ir->varyingBoundsOut[v];
        bounds = {};
        u32 count = data->varyingCounts[v];
        for (u32 c = 0; c < count; c++) {
            const ValueRange& range = data->varyingRanges[v * 4 + c];
            ComponentBounds& component = bounds.components[c];
            component = {};
            if (!(range.flags & ValueRange::KNOWN) || (range.flags & ValueRange::MAY_BE_NAN)) continue;
            component.low = range.low;
            component.high = range.high;
            component.flags = ComponentBounds::HAS_LOW | ComponentBounds::HAS_HIGH;
            if (range.flags & ValueRange::LOW_EXCLUSIVE) component.flags |= ComponentBounds::LOW_EXCLUSIVE;
            if (range.flags & ValueRange::HIGH_EXCLUSIVE) component.flags |= ComponentBounds::HIGH_EXCLUSIVE;
        }
        bounds.count = static_cast<u8>(count);
    }
}
