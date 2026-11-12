#include "rtx_hair/spirv_module.h"

#include <algorithm>
#include <cstring>

namespace rtx_hair::spirv {
namespace {
struct GrammarOperand
{
    uint8_t kind, quantifier;
    uint16_t table;
};
struct GrammarInstruction
{
    uint16_t opcode;
    uint8_t hasType, hasResult;
    uint16_t first, count;
};
struct GrammarEnumParameters
{
    uint16_t table;
    uint32_t value;
    uint16_t first, count;
};
#include "rtx_hair/spirv_grammar.inl"

enum Kind : uint8_t
{
    kId = 1,
    kLiteral,
    kString,
    kContextNumber,
    kPairLiteralId,
    kPairIdLiteral,
    kPairIdId,
    kValueEnum,
    kBitEnum,
};
enum Quantifier : uint8_t
{
    kOne,
    kOptional,
    kAny,
};

// Opcodes the editor itself needs by number.
enum : uint16_t
{
    kOpSourceContinued = 2,
    kOpSource = 3,
    kOpSourceExtension = 4,
    kOpName = 5,
    kOpMemberName = 6,
    kOpString = 7,
    kOpExtension = 10,
    kOpExtInstImport = 11,
    kOpMemoryModel = 14,
    kOpEntryPoint = 15,
    kOpExecutionMode = 16,
    kOpCapability = 17,
    kOpTypeVoid = 19,
    kOpTypeBool = 20,
    kOpTypeInt = 21,
    kOpTypeFloat = 22,
    kOpTypeVector = 23,
    kOpTypeArray = 28,
    kOpTypePointer = 32,
    kOpConstantTrue = 41,
    kOpConstantFalse = 42,
    kOpConstant = 43,
    kOpConstantNull = 46,
    kOpFunction = 54,
    kOpFunctionEnd = 56,
    kOpVariable = 59,
    kOpDecorate = 71,
    kOpMemberDecorate = 72,
    kOpDecorationGroup = 73,
    kOpGroupDecorate = 74,
    kOpGroupMemberDecorate = 75,
    kOpSwitch = 251,
    kOpModuleProcessed = 330,
    kOpExecutionModeId = 331,
    kOpDecorateId = 332,
    kOpDecorateString = 5632,
    kOpMemberDecorateString = 5633,
};
constexpr uint32_t kDecorationArrayStride = 6, kDecorationBuiltIn = 11;

const GrammarInstruction* FindInstruction(uint16_t opcode)
{
    const auto* end = std::end(kGrammarInstructions);
    const auto* it = std::lower_bound(std::begin(kGrammarInstructions), end, opcode,
        [](const GrammarInstruction& g, uint16_t op) { return g.opcode < op; });
    return it != end && it->opcode == opcode ? it : nullptr;
}

const GrammarEnumParameters* FindParameters(uint16_t table, uint32_t value)
{
    const auto* end = std::end(kGrammarEnumParameters);
    const auto* it = std::lower_bound(std::begin(kGrammarEnumParameters), end, std::make_pair(table, value),
        [](const GrammarEnumParameters& g, const std::pair<uint16_t, uint32_t>& key) {
            return g.table != key.first ? g.table < key.first : g.value < key.second;
        });
    return it != end && it->table == table && it->value == value ? it : nullptr;
}

// Walks one operand list (an instruction's, or an enum value's parameters) from word `w`.
class Walker
{
public:
    Walker(const Inst& inst, std::vector<IdOperand>& out, uint32_t literalWords) : inst_(inst), out_(out), literalWords_(literalWords) {}

    bool List(uint16_t first, uint16_t count, size_t& w)
    {
        for (uint16_t k = 0; k < count; ++k)
        {
            const GrammarOperand& o = kGrammarOperands[first + k];
            if (o.quantifier == kOne)
            {
                if (w >= inst_.w.size() || !One(o, w)) return false;
            }
            else if (o.quantifier == kOptional)
            {
                if (w < inst_.w.size() && !One(o, w)) return false;
            }
            else
                while (w < inst_.w.size())
                    if (!One(o, w)) return false;
        }
        return true;
    }

private:
    bool Id(size_t& w)
    {
        if (w >= inst_.w.size()) return false;
        out_.push_back({w++, Role::Reference});
        return true;
    }
    bool Literal(size_t& w, uint32_t words = 1)
    {
        if (w + words > inst_.w.size()) return false;
        w += words;
        return true;
    }
    bool One(const GrammarOperand& o, size_t& w)
    {
        switch (o.kind)
        {
        case kId: return Id(w);
        case kLiteral: return Literal(w);
        case kString:
            while (w < inst_.w.size())
            {
                const Word x = inst_.w[w++];
                if ((x & 0xFFu) == 0 || (x & 0xFF00u) == 0 || (x & 0xFF0000u) == 0 || (x & 0xFF000000u) == 0) return true;
            }
            return false;
        case kContextNumber: w = inst_.w.size(); return true; // the last operand of OpConstant / OpSpecConstant
        case kPairLiteralId: return Literal(w, literalWords_) && Id(w);
        case kPairIdLiteral: return Id(w) && Literal(w);
        case kPairIdId: return Id(w) && Id(w);
        case kValueEnum:
        {
            if (w >= inst_.w.size()) return false;
            const Word value = inst_.w[w++];
            if (const auto* p = FindParameters(o.table, value)) return List(p->first, p->count, w);
            return true;
        }
        case kBitEnum:
        {
            if (w >= inst_.w.size()) return false;
            const Word mask = inst_.w[w++];
            for (uint32_t bit = 0; bit < 32; ++bit)
                if (mask & (1u << bit))
                    if (const auto* p = FindParameters(o.table, 1u << bit))
                        if (!List(p->first, p->count, w)) return false;
            return true;
        }
        default: return false;
        }
    }

    const Inst& inst_;
    std::vector<IdOperand>& out_;
    uint32_t literalWords_;
};

bool SameWords(const Inst& inst, size_t from, std::initializer_list<Word> key)
{
    if (inst.w.size() != from + key.size()) return false;
    size_t i = from;
    for (const Word k : key)
        if (inst.w[i++] != k) return false;
    return true;
}
}

Inst Make(uint16_t opcode, std::initializer_list<Word> operands)
{
    Inst inst;
    inst.w.reserve(operands.size() + 1);
    inst.w.push_back((static_cast<Word>(operands.size() + 1) << 16) | opcode);
    inst.w.insert(inst.w.end(), operands.begin(), operands.end());
    return inst;
}

Inst Make(uint16_t opcode, const std::vector<Word>& operands)
{
    Inst inst;
    inst.w.reserve(operands.size() + 1);
    inst.w.push_back((static_cast<Word>(operands.size() + 1) << 16) | opcode);
    inst.w.insert(inst.w.end(), operands.begin(), operands.end());
    return inst;
}

void AppendString(std::vector<Word>& operands, std::string_view text)
{
    const size_t words = text.size() / 4 + 1;
    const size_t start = operands.size();
    operands.resize(start + words, 0);
    std::memcpy(operands.data() + start, text.data(), text.size());
}

std::string ReadString(const Inst& inst, size_t index, size_t* next)
{
    std::string text;
    size_t w = index;
    for (; w < inst.w.size(); ++w)
    {
        const Word x = inst.w[w];
        bool end = false;
        for (int b = 0; b < 4; ++b)
        {
            const char c = static_cast<char>((x >> (8 * b)) & 0xFFu);
            if (c == 0) { end = true; break; }
            text.push_back(c);
        }
        if (end) { ++w; break; }
    }
    if (next) *next = w;
    return text;
}

bool Module::Parse(std::span<const Word> words, std::string& error)
{
    *this = Module{};
    if (words.size() < 5 || words[0] != kMagic)
    {
        error = "not a SPIR-V module (or not in host byte order)";
        return false;
    }
    version = words[1];
    generator = words[2];
    bound = words[3];
    schema = words[4];
    size_t i = 5;
    bool inFunctions = false, open = false;
    while (i < words.size())
    {
        const uint32_t count = words[i] >> 16;
        const uint16_t op = static_cast<uint16_t>(words[i] & 0xFFFFu);
        if (count == 0 || i + count > words.size())
        {
            error = "truncated instruction at word " + std::to_string(i);
            return false;
        }
        if (!FindInstruction(op))
        {
            error = "opcode " + std::to_string(op) + " is not in the SPIR-V grammar table";
            return false;
        }
        Inst inst{std::vector<Word>(words.begin() + static_cast<ptrdiff_t>(i), words.begin() + static_cast<ptrdiff_t>(i + count))};
        i += count;
        if (op == kOpFunction)
        {
            if (open)
            {
                error = "OpFunction inside a function";
                return false;
            }
            functions.emplace_back();
            inFunctions = open = true;
        }
        if (inFunctions)
        {
            if (!open)
            {
                error = "an instruction between functions";
                return false;
            }
            functions.back().body.push_back(std::move(inst));
            if (op == kOpFunctionEnd) open = false;
            continue;
        }
        switch (op)
        {
        case kOpCapability: capabilities.push_back(std::move(inst)); break;
        case kOpExtension: extensions.push_back(std::move(inst)); break;
        case kOpExtInstImport: imports.push_back(std::move(inst)); break;
        case kOpMemoryModel: memoryModel.push_back(std::move(inst)); break;
        case kOpEntryPoint: entryPoints.push_back(std::move(inst)); break;
        case kOpExecutionMode:
        case kOpExecutionModeId: executionModes.push_back(std::move(inst)); break;
        case kOpSourceContinued:
        case kOpSource:
        case kOpSourceExtension:
        case kOpName:
        case kOpMemberName:
        case kOpString:
        case kOpModuleProcessed: debug.push_back(std::move(inst)); break;
        case kOpDecorate:
        case kOpMemberDecorate:
        case kOpDecorationGroup:
        case kOpGroupDecorate:
        case kOpGroupMemberDecorate:
        case kOpDecorateId:
        case kOpDecorateString:
        case kOpMemberDecorateString: annotations.push_back(std::move(inst)); break;
        default: globals.push_back(std::move(inst)); break;
        }
    }
    if (open)
    {
        error = "unterminated function";
        return false;
    }
    if (memoryModel.size() != 1)
    {
        error = "no single OpMemoryModel";
        return false;
    }
    Reindex();
    return true;
}

std::vector<Word> Module::Emit() const
{
    std::vector<Word> out{kMagic, version, generator, bound, schema};
    auto put = [&](const Inst& inst) {
        out.push_back((static_cast<Word>(inst.w.size()) << 16) | inst.Op()); // the count follows the words
        out.insert(out.end(), inst.w.begin() + 1, inst.w.end());
    };
    for (const auto* section : {&capabilities, &extensions, &imports, &memoryModel, &entryPoints, &executionModes, &debug, &annotations, &globals})
        for (const auto& inst : *section) put(inst);
    for (const auto& f : functions)
        for (const auto& inst : f.body) put(inst);
    return out;
}

bool Module::IdOperands(const Inst& inst, std::vector<IdOperand>& out) const
{
    out.clear();
    const GrammarInstruction* g = FindInstruction(inst.Op());
    if (!g) return false;
    size_t w = 1;
    if (g->hasType)
    {
        if (w >= inst.w.size()) return false;
        out.push_back({w++, Role::ResultType});
    }
    if (g->hasResult)
    {
        if (w >= inst.w.size()) return false;
        out.push_back({w++, Role::Result});
    }
    // OpSwitch's case literals are as wide as its selector.
    uint32_t literalWords = 1;
    if (inst.Op() == kOpSwitch && inst.w.size() > 1) literalWords = ScalarWidth(TypeOf(inst.w[1])) > 32 ? 2 : 1;
    Walker walker(inst, out, literalWords);
    return walker.List(g->first, g->count, w) && w == inst.w.size();
}

void Module::Reindex()
{
    global_.clear();
    type_.clear();
    builtIn_.clear();
    arrayStride_.clear();
    for (size_t i = 0; i < globals.size(); ++i)
    {
        const Inst& inst = globals[i];
        const GrammarInstruction* g = FindInstruction(inst.Op());
        if (!g || !g->hasResult) continue;
        const size_t r = g->hasType ? 2 : 1;
        if (inst.w.size() <= r) continue;
        global_[inst.w[r]] = i;
        if (g->hasType) type_[inst.w[r]] = inst.w[1];
    }
    for (const auto& f : functions)
        for (const auto& inst : f.body)
        {
            const GrammarInstruction* g = FindInstruction(inst.Op());
            if (g && g->hasType && g->hasResult && inst.w.size() > 2) type_[inst.w[2]] = inst.w[1];
        }
    for (const auto& a : annotations)
        if (a.Op() == kOpDecorate && a.w.size() >= 3)
        {
            if (a.w[2] == kDecorationBuiltIn && a.w.size() >= 4) builtIn_[a.w[1]] = a.w[3];
            if (a.w[2] == kDecorationArrayStride) arrayStride_[a.w[1]] = true;
        }
}

const Inst* Module::Global(Word id) const
{
    const auto it = global_.find(id);
    return it == global_.end() ? nullptr : &globals[it->second];
}

Word Module::TypeOf(Word id) const
{
    const auto it = type_.find(id);
    return it == type_.end() ? 0 : it->second;
}

bool Module::BuiltInOf(Word id, uint32_t& builtIn) const
{
    const auto it = builtIn_.find(id);
    if (it == builtIn_.end()) return false;
    builtIn = it->second;
    return true;
}

uint32_t Module::ScalarWidth(Word type) const
{
    const Inst* t = Global(type);
    return t && (t->Op() == kOpTypeInt || t->Op() == kOpTypeFloat) && t->w.size() >= 3 ? t->w[2] : 0;
}

Word Module::Append(Inst inst)
{
    const GrammarInstruction* g = FindInstruction(inst.Op());
    const size_t r = g && g->hasType ? 2 : 1;
    const Word id = inst.w[r];
    if (g && g->hasType) type_[id] = inst.w[1];
    globals.push_back(std::move(inst));
    global_[id] = globals.size() - 1;
    return id;
}

Word Module::Void()
{
    for (const auto& g : globals)
        if (g.Op() == kOpTypeVoid) return g.w[1];
    return Append(Make(kOpTypeVoid, {NewId()}));
}

Word Module::Bool()
{
    for (const auto& g : globals)
        if (g.Op() == kOpTypeBool) return g.w[1];
    return Append(Make(kOpTypeBool, {NewId()}));
}

Word Module::Int(uint32_t width, bool isSigned)
{
    for (const auto& g : globals)
        if (g.Op() == kOpTypeInt && SameWords(g, 2, {width, isSigned ? 1u : 0u})) return g.w[1];
    return Append(Make(kOpTypeInt, {NewId(), width, isSigned ? 1u : 0u}));
}

Word Module::Float32()
{
    for (const auto& g : globals)
        if (g.Op() == kOpTypeFloat && SameWords(g, 2, {32u})) return g.w[1];
    return Append(Make(kOpTypeFloat, {NewId(), 32u}));
}

Word Module::Vector(Word component, uint32_t count)
{
    for (const auto& g : globals)
        if (g.Op() == kOpTypeVector && SameWords(g, 2, {component, count})) return g.w[1];
    return Append(Make(kOpTypeVector, {NewId(), component, count}));
}

Word Module::Array(Word element, uint32_t length)
{
    for (const auto& g : globals)
    {
        if (g.Op() != kOpTypeArray || g.w.size() != 4 || g.w[2] != element || arrayStride_.count(g.w[1])) continue;
        const Inst* n = Global(g.w[3]);
        if (n && n->Op() == kOpConstant && n->w.size() == 4 && n->w[3] == length && ScalarWidth(n->w[1]) == 32) return g.w[1];
    }
    const Word size = U32(length);
    return Append(Make(kOpTypeArray, {NewId(), element, size}));
}

Word Module::Pointer(uint32_t storage, Word pointee)
{
    for (const auto& g : globals)
        if (g.Op() == kOpTypePointer && SameWords(g, 2, {storage, pointee})) return g.w[1];
    return Append(Make(kOpTypePointer, {NewId(), storage, pointee}));
}

Word Module::U32(uint32_t value)
{
    const Word type = Int(32, false);
    for (const auto& g : globals)
        if (g.Op() == kOpConstant && g.w.size() == 4 && g.w[1] == type && g.w[3] == value) return g.w[2];
    return Append(Make(kOpConstant, {type, NewId(), value}));
}

Word Module::I32(int32_t value)
{
    const Word type = Int(32, true);
    const Word bits = static_cast<Word>(value);
    for (const auto& g : globals)
        if (g.Op() == kOpConstant && g.w.size() == 4 && g.w[1] == type && g.w[3] == bits) return g.w[2];
    return Append(Make(kOpConstant, {type, NewId(), bits}));
}

Word Module::F32(float value)
{
    const Word type = Float32();
    Word bits;
    std::memcpy(&bits, &value, 4);
    for (const auto& g : globals)
        if (g.Op() == kOpConstant && g.w.size() == 4 && g.w[1] == type && g.w[3] == bits) return g.w[2];
    return Append(Make(kOpConstant, {type, NewId(), bits}));
}

Word Module::True()
{
    const Word type = Bool();
    for (const auto& g : globals)
        if (g.Op() == kOpConstantTrue && g.w[1] == type) return g.w[2];
    return Append(Make(kOpConstantTrue, {type, NewId()}));
}

Word Module::False()
{
    const Word type = Bool();
    for (const auto& g : globals)
        if (g.Op() == kOpConstantFalse && g.w[1] == type) return g.w[2];
    return Append(Make(kOpConstantFalse, {type, NewId()}));
}

Word Module::Null(Word type)
{
    for (const auto& g : globals)
        if (g.Op() == kOpConstantNull && g.w[1] == type) return g.w[2];
    return Append(Make(kOpConstantNull, {type, NewId()}));
}

Word Module::Glsl()
{
    for (const auto& i : imports)
        if (ReadString(i, 2) == "GLSL.std.450") return i.w[1];
    const Word id = NewId();
    std::vector<Word> operands{id};
    AppendString(operands, "GLSL.std.450");
    imports.push_back(Make(kOpExtInstImport, operands));
    return id;
}

bool Module::HasCapability(uint32_t capability) const
{
    for (const auto& c : capabilities)
        if (c.w.size() >= 2 && c.w[1] == capability) return true;
    return false;
}

void Module::AddCapability(uint32_t capability)
{
    if (!HasCapability(capability)) capabilities.push_back(Make(kOpCapability, {capability}));
}

void Module::RemoveCapability(uint32_t capability)
{
    std::erase_if(capabilities, [&](const Inst& c) { return c.w.size() >= 2 && c.w[1] == capability; });
}

bool Module::HasExtension(std::string_view name) const
{
    for (const auto& e : extensions)
        if (ReadString(e, 1) == name) return true;
    return false;
}

void Module::AddExtension(std::string_view name)
{
    if (HasExtension(name)) return;
    std::vector<Word> operands;
    AppendString(operands, name);
    extensions.push_back(Make(kOpExtension, operands));
}

void Module::RemoveExtension(std::string_view name)
{
    std::erase_if(extensions, [&](const Inst& e) { return ReadString(e, 1) == name; });
}

void Module::RemoveDecorations(Word id)
{
    std::erase_if(annotations, [&](const Inst& a) {
        return (a.Op() == kOpDecorate || a.Op() == kOpDecorateId || a.Op() == kOpDecorateString) && a.w.size() >= 2 && a.w[1] == id;
    });
    builtIn_.erase(id);
}

void Module::Decorate(Word id, uint32_t decoration, std::initializer_list<Word> literals)
{
    std::vector<Word> operands{id, decoration};
    operands.insert(operands.end(), literals.begin(), literals.end());
    annotations.push_back(Make(kOpDecorate, operands));
    if (decoration == kDecorationBuiltIn && literals.size() == 1) builtIn_[id] = *literals.begin();
}

Word Module::Variable(uint32_t storage, Word pointee, Word initializer)
{
    const Word pointer = Pointer(storage, pointee);
    const Word id = NewId();
    if (initializer) return Append(Make(kOpVariable, {pointer, id, storage, initializer}));
    return Append(Make(kOpVariable, {pointer, id, storage}));
}
}
