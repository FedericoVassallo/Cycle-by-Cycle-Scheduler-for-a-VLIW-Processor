#ifndef REGISTER_ALLOCATION_HPP
#define REGISTER_ALLOCATION_HPP

#include "data_structures.hpp"
#include <vector>

// holds the register allocation results so BB2 can be rewritten
// after it gets placed in the schedule by schedule_bb2
struct AllocResult {
    // new destination register for each instruction id
    std::vector<int> new_dest_reg;
    // new source registers for each instruction id
    std::vector<std::vector<int>> new_source_regs;
    // cycle where the loop body starts after scheduling
    int loop_beginning = -1;
};

// basic register allocation for normal loop scheduling
// returns renamed registers and loop beginning info
AllocResult alloc_b(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle);

// after BB2 is scheduled, rewrite BB2 bundle text with allocated registers
void rewrite_bb2_bundles(std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions,
                         const std::vector<int>& scheduled_cycle,
                         const AllocResult& alloc);
                         
// rotating register allocation for loop.pip scheduling
// uses ii, stage mapping and number of stages
AllocResult alloc_r(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle,
                    int ii,
                    int loop_beginning,
                    int num_stages,
                    const std::vector<int>& stage_by_id);

#endif // REGISTER_ALLOCATION_HPP