#ifndef SCHEDULER_HPP
#define SCHEDULER_HPP

#include "data_structures.hpp"
#include <vector>

int calculate_II_res(const std::vector<Instruction>& instructions);

void schedule_ASAP_basic(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                         std::vector<Bundle>& schedule,
                         const std::vector<Instruction>& instructions,
                         std::vector<int>& scheduled_cycle);

void schedule_ASAP_advanced(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                            std::vector<Bundle>& schedule,
                            const std::vector<Instruction>& instructions,
                            std::vector<int>& scheduled_cycle,
                            int& out_ii,
                            int& out_loop_beginning,
                            int& out_num_stages,
                            std::vector<int>& out_stage_by_id);

void schedule_bb2(const std::vector<DependencyAnalysisTableEntry>& analysis_table,
                  std::vector<Bundle>& schedule,
                  const std::vector<Instruction>& instructions,
                  std::vector<int>& scheduled_cycle);

#endif // SCHEDULER_HPP