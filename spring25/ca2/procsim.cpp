// procsim.cpp
#include "procsim.hpp"
#include <vector>
#include <cstdio>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace
{
        // Maximum number of instructions we can handle
        constexpr int MAX_INSTRUCTIONS = 100000;

        // Current size of the dispatch queue (waiting to be scheduled)
        unsigned long currentDispatchQueueSize = 0;
        // Largest we ever saw the dispatch queue get
        unsigned long maximumDispatchQueueSize = 0;
        // Running sum of dispatch queue sizes (for average)
        unsigned long totalDispatchQueueSizeSum = 0;

        // Records timing stages for each retired instruction
        int instructionTimeline[MAX_INSTRUCTIONS][6];

        // Next unique tag assigned to a fetched instruction
        int nextInstructionTag = 0;

        // Available resources in the pipeline
        int robCapacity = 0;           // total result buses / ROB entries
        int robAvailable = 0;          // how many remain free
        int fuCapacity[3] = {0};       // total FUs per op code
        int fuAvailableUnits[3] = {0}; // how many FUs of each type are free
        int fetchWidth = 0;            // instructions fetched per cycle
        int scheduleQueueSize = 0;     // size of schedule queue (2×FUs)
        int freeReservationSlots = 0;  // how many RS entries remain

        int cycleCount = 1; // simulation cycle, starts at 1

        // Pools and register file state
        Function_Unit fuPool[3];
        Register_File registerFile[129]; // entry 128 acts as “dummy reg”

        // Active instructions in flight
        std::vector<_proc_inst_t *> pipelineInstQueue;
        using InstIt = std::vector<_proc_inst_t *>::iterator;

        // Fetches a new instruction from stdin and sets up its tags and timing
        _proc_inst_t *fetchNextInstruction()
        {
                auto inst = new _proc_inst_t();
                int ret = fscanf(stdin, "%x %d %d %d %d",
                                 &inst->instruction_address,
                                 &inst->op_code,
                                 &inst->dest_reg,
                                 &inst->src_reg[0],
                                 &inst->src_reg[1]);
                // Hit EOF or bad line?
                if (ret != 5)
                {
                        inst->_null = true;
                        return inst;
                }
                inst->_null = false;

                // Map invalid register (-1) to index 128
                for (int i = 0; i < 2; i++)
                        if (inst->src_reg[i] == -1)
                                inst->src_reg[i] = 128;

                // Some traces use -1 opcode; normalize to 1
                if (inst->op_code == -1)
                        inst->op_code = 1;

                // Assign unique tag and compute fetch/dispatch cycles
                inst->dest_tag = nextInstructionTag;
                inst->fet_cyc = 1 + nextInstructionTag / fetchWidth;
                inst->disp_cyc = inst->fet_cyc + 1;
                ++nextInstructionTag;
                return inst;
        }

        // Update statistics for dispatch queue occupancy
        void updateDispatchStats()
        {
                // Only accumulate while we’re still fetching real instructions
                if (fetchWidth * cycleCount <= MAX_INSTRUCTIONS)
                {
                        currentDispatchQueueSize += fetchWidth;
                        maximumDispatchQueueSize = std::max(currentDispatchQueueSize,
                                                            maximumDispatchQueueSize);
                }
                totalDispatchQueueSizeSum += currentDispatchQueueSize;
        }

        // True if this instruction is now ready to leave dispatch and enter RS
        bool isReadyForDispatch(_proc_inst_t *inst)
        {
                return inst->disp_cyc <= cycleCount && inst->sched_cyc == -1 && inst->exec_cyc == -1 && inst->stateUp_cyc == -1 && !inst->dispatched;
        }

        // Fetch up to fetchWidth instructions, or until reservation slots fill
        void fetchInstructions()
        {
                int slots = std::min(fetchWidth, freeReservationSlots);
                for (int i = 0; i < slots; i++)
                {
                        auto inst = fetchNextInstruction();
                        if (inst->_null)
                                return; // no more instructions
                        pipelineInstQueue.push_back(inst);
                }
        }

        // Move ready instructions into the schedule queue, allocate tags/registers
        void dispatchInstructions()
        {
                for (auto inst : pipelineInstQueue)
                {
                        if (isReadyForDispatch(inst) && freeReservationSlots > 0)
                        {
                                --currentDispatchQueueSize;
                                --freeReservationSlots;

                                // Check register file: if ready, mark src_ready; else stamp tag
                                for (int j = 0; j < 2; j++)
                                {
                                        int r = inst->src_reg[j];
                                        if (registerFile[r].ready)
                                        {
                                                inst->src_ready[j] = true;
                                        }
                                        else
                                        {
                                                inst->src_tag[j] = registerFile[r].tag;
                                                inst->src_ready[j] = false;
                                        }
                                }

                                // Allocate destination register in RF
                                if (inst->dest_reg != -1)
                                {
                                        registerFile[inst->dest_reg].tag = inst->dest_tag;
                                        registerFile[inst->dest_reg].ready = false;
                                }
                                else
                                {
                                        registerFile[128].tag = inst->dest_tag;
                                }

                                inst->dispatched = true;
                                inst->sched_cyc = cycleCount + 1;
                        }
                }
        }

        // True if instruction is in RS and hasn't fired yet
        bool isReadyToIssue(_proc_inst_t *inst)
        {
                return inst->sched_cyc <= cycleCount && !inst->fired;
        }

        // Issue any ready instructions into functional units
        void issueInstructions()
        {
                for (auto inst : pipelineInstQueue)
                {
                        if (isReadyToIssue(inst) && inst->src_ready[0] && inst->src_ready[1] && fuAvailableUnits[inst->op_code] > 0)
                        {
                                inst->fired = true;
                                inst->fu_busy = true;
                                --fuAvailableUnits[inst->op_code];
                                inst->exec_cyc = cycleCount + 1;
                        }
                }
        }

        // Advance execution: write back from FUs and CDBs
        void executeInstructions()
        {
                // First handle instructions that have waited the longest in FUs
                int maxWait = 0;
                for (auto inst : pipelineInstQueue)
                        maxWait = std::max(maxWait, inst->fu_wait);

                for (int w = maxWait; w > 0; --w)
                {
                        for (auto inst : pipelineInstQueue)
                        {
                                if (inst->fu_wait == w && robAvailable > 0)
                                {
                                        --robAvailable;
                                        ++fuAvailableUnits[inst->op_code];
                                        inst->fu_busy = false;
                                        inst->fu_wait = 0;
                                        inst->CDB_busy = true;
                                        inst->completed = true;
                                        inst->stateUp_cyc = cycleCount + 1;
                                }
                        }
                }

                // Then any instruction that has finished exec but not yet retired
                if (robAvailable > 0)
                {
                        for (auto inst : pipelineInstQueue)
                        {
                                if (inst->exec_cyc != -1 && inst->exec_cyc <= cycleCount && !inst->completed && robAvailable > 0)
                                {
                                        --robAvailable;
                                        ++fuAvailableUnits[inst->op_code];
                                        inst->fu_busy = false;
                                        inst->fu_wait = 0;
                                        inst->CDB_busy = true;
                                        inst->completed = true;
                                        inst->stateUp_cyc = cycleCount + 1;
                                }
                        }
                }

                // Any fired-but-not-completed instruction keeps accumulating wait cycles
                for (auto inst : pipelineInstQueue)
                {
                        if (inst->fired && !inst->completed)
                                ++inst->fu_wait;
                }
        }

        // When a CDB slot frees, mark the register as ready
        void updateRegisterFile()
        {
                for (auto inst : pipelineInstQueue)
                {
                        if (inst->CDB_busy)
                        {
                                int d = (inst->dest_reg != -1 ? inst->dest_reg : 128);
                                if (registerFile[d].tag == inst->dest_tag)
                                {
                                        registerFile[d].ready = true;
                                }
                        }
                }
        }

        // Broadcast tag availability to waiting instructions
        void broadcastResults()
        {
                for (auto inst : pipelineInstQueue)
                {
                        if (inst->CDB_busy)
                        {
                                for (auto other : pipelineInstQueue)
                                {
                                        if (other->src_tag[0] == inst->dest_tag)
                                                other->src_ready[0] = true;
                                        if (other->src_tag[1] == inst->dest_tag)
                                                other->src_ready[1] = true;
                                }
                                inst->CDB_busy = false;
                                ++robAvailable;
                        }
                }
        }

        // Remove completed instructions, record their timeline, free RS slots
        void retireInstructions()
        {
                for (auto it = pipelineInstQueue.begin(); it != pipelineInstQueue.end();)
                {
                        auto inst = *it;
                        if (inst->completed)
                        {
                                ++freeReservationSlots;
                                // Save timing: INST, FETCH, DISP, SCHED, EXEC, STATE
                                instructionTimeline[inst->dest_tag][STAGE_INST] = inst->dest_tag + 1;
                                instructionTimeline[inst->dest_tag][STAGE_FETCH] = inst->fet_cyc;
                                instructionTimeline[inst->dest_tag][STAGE_DISP] = inst->disp_cyc;
                                instructionTimeline[inst->dest_tag][STAGE_SCHED] = inst->sched_cyc;
                                instructionTimeline[inst->dest_tag][STAGE_EXEC] = inst->exec_cyc;
                                instructionTimeline[inst->dest_tag][STAGE_STATE] = inst->stateUp_cyc;
                                it = pipelineInstQueue.erase(it);
                                delete inst;
                        }
                        else
                        {
                                ++it;
                        }
                }
        }

} // end anonymous namespace

void setup_proc(uint64_t r, uint64_t k0, uint64_t k1,
                uint64_t k2, uint64_t f)
{
        // Initialize resource counts from driver parameters
        robCapacity = static_cast<int>(r);
        robAvailable = static_cast<int>(r);
        fuCapacity[0] = static_cast<int>(k0);
        fuCapacity[1] = static_cast<int>(k1);
        fuCapacity[2] = static_cast<int>(k2);
        for (int i = 0; i < 3; i++){
                fuPool[i].units.resize(fuCapacity[i]);
                fuAvailableUnits[i] = fuCapacity[i];
        }

        fetchWidth = static_cast<int>(f);
        scheduleQueueSize = 2 * (k0 + k1 + k2);
        freeReservationSlots = scheduleQueueSize;
}

void run_proc(proc_stats_t *p_stats)
{
        // Continue cycle-by-cycle until no instructions remain in flight
        do
        {
                executeInstructions();
                updateRegisterFile();
                issueInstructions();
                dispatchInstructions();
                fetchInstructions();
                updateDispatchStats();
                broadcastResults();
                retireInstructions();
                ++cycleCount;
        } while (!pipelineInstQueue.empty());
}

void complete_proc(proc_stats_t *p_stats)
{
        // Print header then each instruction’s lifecycle
        printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");
        for (int i = 0; i < MAX_INSTRUCTIONS; ++i)
        {
                for (int j = 0; j < 6; ++j)
                        printf("%d\t", instructionTimeline[i][j]);
                printf("\n");
        }
        printf("\n");

        // Fill out the stats struct for the driver to print
        p_stats->retired_instruction = nextInstructionTag;
        p_stats->cycle_count = cycleCount;
        p_stats->avg_inst_fired = static_cast<double>(nextInstructionTag) / cycleCount;
        p_stats->avg_inst_retired = p_stats->avg_inst_fired;
        p_stats->max_disp_size = maximumDispatchQueueSize;
        p_stats->avg_disp_size = static_cast<double>(totalDispatchQueueSizeSum) / cycleCount;
}
