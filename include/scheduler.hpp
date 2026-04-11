#ifndef SCHEDULER_HPP
#define SCHEDULER_HPP

#include "data_structures.hpp"
#include <vector>

// Computes the resource-constrained lower bound on the Initiation Interval (II_res)
// using equation II_res = max_i(ceil(N_i / U_i))
// Only considers instructions in BB1.
int calculate_II_res(const std::vector<Instruction>& instructions);

// ASAP scheduling using the simple loop instruction
// Produces a schedule with a single-stage loop body where II = loop body length.
void schedule_ASAP_basic(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                         std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions);

// ASAP scheduling using the loop.pip instruction 
// Interloop constraints are checked during scheduling 
void schedule_ASAP_advanced(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                            std::vector<Bundle>& schedule,
                            const std::vector<Instruction>& instructions);

#endif // SCHEDULER_HPP