#ifndef DATA_STRUCTURES_HPP
#define DATA_STRUCTURES_HPP

#include <string>
#include <vector>

// enum for the type of the instruction
enum class InstructionKind {
    Unknown,
    Nop,
    Mov,
    Add,
    Addi,
    Sub,
    Mulu,
    Ld,
    St,
    Loop,
    LoopPip
};

// to distinguish between the three basic blocks (BB0, BB1, BB2)
enum class BasicBlock {
    BB0,
    BB1,
    BB2,
};

// struct of a bundle, which can contain up to 5 instructions 
struct Bundle {
    std::string ALU0 = "nop";
    std::string ALU1 = "nop";
    std::string MUL = "nop";
    std::string MEM = "nop";
    std::string BRANCH = "nop";
};

// struct for the manipulation of the instruction
struct Instruction {
    std::string raw_text; // the text directly taken from the input.json
    std::string predicate_register;
    std::string opcode;
    InstructionKind kind = InstructionKind::Unknown; // initialized to unkown

    int destination_register = -1;
    std::vector<int> source_registers;

    bool has_immediate = false;
    int immediate_value = 0;

    bool has_memory_operand = false;
    int memory_offset = 0;
    int memory_base_register = -1;

    bool has_loop_target = false;
    int loop_target = -1;

    BasicBlock basic_block = BasicBlock::BB0; // basic block where it belongs 
};

// it rappresent the DependencyAnalysis for a given instruction
struct DependencyAnalysisTableEntry {
    int address = -1;
    int id = -1;
    InstructionKind instruction_type = InstructionKind::Unknown;
    int destination_register = -1;
    std::vector<int> local_dependencies; // When the producer and consumer are in the same basic block 

    std::vector<int> interloop_dependencies;  // both the producer and the consumer are inside the loop(BB1). But the consumer is not reading the value computed now, but the one calculated in the previous iteration
    // it is generated in one loop iteration and is needed for the next 

    std::vector<int> loop_invariant_dependencies; // If the producer is in BB0, and consumers are in BB1, and optionally in BB0 and BB2
    // for loop invariant the produces is in BB0 (outside the loop) and the consumer is in BB1 (inside the loop), but no instruction inside BB1 ever overwrites that register, so we can consider it as a constant value for the entire loop body

    std::vector<int> post_loop_dependencies; // // If the producer is in BB1 and the consumer in BB2, the data gets calculated during the loop but is used only at the end of the loop
};

// it rappresent the [j] entry of the S[s][i][j] of section 3.2.2 

struct HardwareResources {
    bool alu0_used = false; // Indicates if ALU0 is used in the current bundle
    bool alu1_used = false; // Indicates if ALU1 is used in the current bundle
    bool mul_used = false;  // Indicates if the MUL unit is used in the current bundle
    bool mem_used = false;  // Indicates if the MEM unit is used in the current bundle
    bool branch_used = false; // Indicates if the BRANCH unit is used in the current bundle
};

// is a struct that has a vector of HardwareResources (so basically it is a vector of bundles) in which it sets if for a given cycle a certain resource is occupied 
struct SlotTable {
    std::vector<HardwareResources> table;
    int currentII = 0; 

    // helper function to check the status of the slot table 
    void init_reset(int ii);
    bool can_schedule(int actual_cycle, InstructionKind instr_kind) const;  // const at the end to be sure it does not modify any data in SlotTable obj
    void reserve_resources(int actual_cycle, InstructionKind instr_kind);
};

// helpers used by both the scheduler and register allocation
int instruction_latency(InstructionKind kind);
bool is_alu_kind(InstructionKind kind);
bool ensure_bundle_capacity(std::vector<Bundle>& schedule, int cycle);
bool can_place_in_bundle(const Bundle& bundle, InstructionKind kind);
bool place_in_bundle(Bundle& bundle, InstructionKind kind, const std::string& raw_text);

// reconstructs instruction text from parsed fields and new register assignments
std::string rebuild_instruction_text(const Instruction& instr, int new_dest, const std::vector<int>& new_sources);

// returns the earliest ready cycle where we can schedule the instruction given the dependencies
int max_ready_cycle(const std::vector<int>& deps,
                    const std::vector<int>& scheduled_cycle,
                    const std::vector<InstructionKind>& kind_by_id);

// checks loop-carried recurrence constraints after a full BB1 placement attempt
bool interloop_constraints_satisfied(const std::vector<int>& bb1_ids,
                                     const std::vector<bool>& is_bb1,
                                     const std::vector<int>& analysis_index_by_id,
                                     const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                                     const std::vector<int>& scheduled_cycle,
                                     const std::vector<InstructionKind>& kind_by_id,
                                     int ii);

// schedules an entry (no modulo scheduling, used for BB0 and BB2 instructions)
int schedule_entry_no_modulo(const DependencyAnalysisTableEntry& entry,
                             const std::vector<Instruction>& instructions,
                             std::vector<Bundle>& schedule,
                             std::vector<int>& scheduled_cycle,
                             const std::vector<InstructionKind>& kind_by_id,
                             const std::vector<int>& extra_dependencies = {});

// attempts to schedule BB1 instructions with a given initiation interval (II)
bool try_bb1_schedule_with_ii(const std::vector<int>& bb1_ids,
                              const std::vector<bool>& is_bb1,
                              const std::vector<int>& analysis_index_by_id,
                              const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                              std::vector<Bundle>& schedule,
                              std::vector<int>& scheduled_cycle,
                              const std::vector<InstructionKind>& kind_by_id,
                              const std::vector<Instruction>& instructions,
                              SlotTable& slot_table,
                              int& ii,
                              int loop_beginning);

#endif // DATA_STRUCTURES_HPP