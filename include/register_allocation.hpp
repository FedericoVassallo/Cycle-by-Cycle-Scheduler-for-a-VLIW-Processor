#ifndef REGISTER_ALLOCATION_HPP
#define REGISTER_ALLOCATION_HPP

#include "data_structures.hpp"
#include <vector>

// holds the register allocation results so BB2 can be rewritten
// after it gets placed in the schedule by schedule_bb2
struct AllocResult {
    std::vector<int> new_dest_reg;
    std::vector<std::vector<int>> new_source_regs;
    int loop_beginning = -1;
};

AllocResult alloc_b(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle);

void rewrite_bb2_bundles(std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions,
                         const std::vector<int>& scheduled_cycle,
                         const AllocResult& alloc);
                         
AllocResult alloc_r(std::vector<Bundle>& schedule,
                    const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                    const std::vector<Instruction>& instructions,
                    std::vector<int>& scheduled_cycle,
                    int ii,
                    int loop_beginning,
                    int num_stages,
                    const std::vector<int>& stage_by_id);

#endif // REGISTER_ALLOCATION_HPP