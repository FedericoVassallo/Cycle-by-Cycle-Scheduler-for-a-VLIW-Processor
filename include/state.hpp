#ifndef STATE_HPP
#define STATE_HPP

#include <string>
#include <vector>

enum class InstructionKind {
    kUnknown,
    kNop,
    kMov,
    kAdd,
    kAddi,
    kSub,
    kMulu,
    kLd,
    kSt,
    kLoop,
    kLoopPip
};

struct Bundle {
    std::string ALU0 = "nop";
    std::string ALU1 = "nop";
    std::string MUL = "nop";
    std::string MEM = "nop";
    std::string BRANCH = "nop";
};

struct Instruction {
    std::string raw_text;
    std::string predicate_register;
    std::string opcode;
    InstructionKind kind = InstructionKind::kUnknown;

    int destination_register = -1;
    std::vector<int> source_registers;

    bool has_immediate = false;
    int immediate_value = 0;

    bool has_memory_operand = false;
    int memory_offset = 0;
    int memory_base_register = -1;

    bool has_loop_target = false;
    int loop_target = -1;
};

struct DependencyAnalysisTableEntry {
    int address = -1;
    int id = -1;
    InstructionKind instruction_type = InstructionKind::kUnknown;
    int destination_register = -1;
    std::vector<int> local_dependencies;
    std::vector<int> interloop_dependencies;
    std::vector<int> loop_invariant_dependencies;
    std::vector<int> post_loop_dependencies;
};

#endif // STATE_HPP


