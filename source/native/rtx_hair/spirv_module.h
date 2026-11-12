#pragma once
// vkhair.1: a small SPIR-V module editor for universal RTX hair on Vulkan (lss_spirv.h). It parses a
// module into its logical-layout sections, walks every <id> operand exactly (spirv_grammar.inl,
// generated from the SPIR-V grammar), interns types and constants, and emits the words again. No
// SPIRV-Tools, glslang or DXC: the DLL edits shaders with this alone.
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rtx_hair::spirv {
using Word = uint32_t;

inline constexpr Word kMagic = 0x07230203u;

// One instruction, first word included (word count << 16 | opcode).
struct Inst
{
    std::vector<Word> w;
    uint16_t Op() const { return static_cast<uint16_t>(w[0] & 0xFFFFu); }
};

// opcode + operands; the first word is computed.
Inst Make(uint16_t opcode, std::initializer_list<Word> operands);
Inst Make(uint16_t opcode, const std::vector<Word>& operands);
// A literal string's words (nul-terminated, zero-padded), appended to `operands`.
void AppendString(std::vector<Word>& operands, std::string_view text);
// The literal string starting at word `index` of `inst`; `next` (optional) gets the word after it.
std::string ReadString(const Inst& inst, size_t index, size_t* next = nullptr);

struct Function
{
    std::vector<Inst> body; // OpFunction ... OpFunctionEnd
    Word Id() const { return body.front().w[2]; }
};

enum class Role
{
    ResultType,
    Result,
    Reference,
};
struct IdOperand
{
    size_t word;
    Role role;
};

class Module
{
public:
    // False with `error` on a malformed module or an opcode the grammar table does not know.
    bool Parse(std::span<const Word> words, std::string& error);
    std::vector<Word> Emit() const;

    Word version{}, generator{}, bound{}, schema{};
    std::vector<Inst> capabilities, extensions, imports, memoryModel, entryPoints, executionModes, debug, annotations, globals;
    std::vector<Function> functions;

    Word NewId() { return bound++; }

    // Every <id> operand of `inst` (result type, result, references) in word order. False if the
    // instruction does not decode against the grammar (then `out` is incomplete).
    bool IdOperands(const Inst& inst, std::vector<IdOperand>& out) const;

    // Lookups over the whole module (rebuilt by Reindex(); interning keeps them current).
    void Reindex();
    const Inst* Global(Word id) const;  // type, constant or global variable defining `id`
    Word TypeOf(Word id) const;         // result type of any value, 0 if unknown
    bool BuiltInOf(Word id, uint32_t& builtIn) const;
    uint32_t ScalarWidth(Word type) const; // bits of an int/float scalar type, 0 otherwise

    // Interning: an existing declaration, else a new one appended to the globals.
    Word Void();
    Word Bool();
    Word Int(uint32_t width, bool isSigned);
    Word Float32();
    Word Vector(Word component, uint32_t count);
    Word Array(Word element, uint32_t length); // never one with an ArrayStride decoration
    Word Pointer(uint32_t storage, Word pointee);
    Word U32(uint32_t value);
    Word I32(int32_t value);
    Word F32(float value);
    Word True();
    Word False();
    Word Null(Word type);
    Word Glsl(); // the GLSL.std.450 import

    bool HasCapability(uint32_t capability) const;
    void AddCapability(uint32_t capability);
    void RemoveCapability(uint32_t capability);
    bool HasExtension(std::string_view name) const;
    void AddExtension(std::string_view name);
    void RemoveExtension(std::string_view name);

    // OpDecorate/OpDecorateId/OpDecorateString naming `id` removed; OpDecorate added.
    void RemoveDecorations(Word id);
    void Decorate(Word id, uint32_t decoration, std::initializer_list<Word> literals);

    // A global OpVariable of `storage` pointing at `pointee` (optional initializer).
    Word Variable(uint32_t storage, Word pointee, Word initializer = 0);

private:
    Word Append(Inst inst); // to the globals; returns its result <id>
    std::unordered_map<Word, size_t> global_;  // result <id> -> globals index
    std::unordered_map<Word, Word> type_;      // result <id> -> result type <id>
    std::unordered_map<Word, uint32_t> builtIn_;
    std::unordered_map<Word, bool> arrayStride_;
};
}
