#include "procsim.hpp"
#include <queue>
#include <vector>
#include <array>

static void fetch_stage();
static void dispatch_stage();
static void schedule_stage();
static void execute_stage();
static void state_update_stage();

// The five pipeline stages
enum stage_t
{
        S_FETCH = 0,
        S_DISPATCH,
        S_SCHEDULE,
        S_EXECUTE,
        S_STATE_UPDATE
};

struct inst_entry_t
{
        proc_inst_t inst;       // original fetched bits
        unsigned tag;           // unique increasing tag
        bool src_ready[2];      // is each source operand ready?
        unsigned src_tag[2];    // if not ready, which tag are we waiting on?
        unsigned remaining_lat; // cycles left in execute
        unsigned when[5];       // cycle each stage completed
};

static std::vector<inst_entry_t *> all_entries;
static unsigned current_cycle;

// configuration (set in setup_proc)
static uint64_t R, K0, K1, K2, F;

// Fetch bookkeeping
static bool more_to_fetch; // remains instructions in the trace?

// Tag generator
static unsigned next_tag;

// Dispatch queue (infinite FIFO)
static std::queue<inst_entry_t *> dispatch_q;

// Scheduling queue (reservation stations)
// Capacity = sum of 2×(K0+K1+K2), but vector lets us grow then check size.
static std::vector<inst_entry_t *> sched_q;

// Register status: for each architectural reg, which tag will write it, or 0 if free
static std::array<unsigned, 128> reg_status;

// Functional‐unit availability counters
static unsigned free_k0, free_k1, free_k2;

/**
 * Subroutine for initializing the processor. You many add and initialize any global or heap
 * variables as needed.
 * XXX: You're responsible for completing this routine
 *
 * @r ROB size
 * @k0 Number of k0 FUs
 * @k1 Number of k1 FUs
 * @k2 Number of k2 FUs
 * @f Number of instructions to fetch
 */
void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f)
{
        R = r;
        K0 = k0;
        K1 = k1;
        K2 = k2;
        F = f;
        more_to_fetch = true;
        next_tag = 1;
        dispatch_q = {};
        sched_q.clear();
        reg_status.fill(0); // 0 means “no pending writer”
        free_k0 = K0;
        free_k1 = K1;
        free_k2 = K2;
        current_cycle = 0;
}

// Attempt to fetch up to F instructions each cycle
static void fetch_stage()
{
        if (!more_to_fetch)
                return;
        for (uint64_t i = 0; i < F; ++i)
        {
                proc_inst_t base;
                if (!read_instruction(&base))
                {
                        // no more in the trace
                        more_to_fetch = false;
                        break;
                }
                // allocate a new entry
                inst_entry_t *ent = new inst_entry_t();
                all_entries.push_back(ent);
                ent->inst = base;
                ent->tag = next_tag++;
                // dependencies will be resolved in dispatch
                ent->src_ready[0] = ent->src_ready[1] = false;
                ent->src_tag[0] = ent->src_tag[1] = 0;
                ent->remaining_lat = 0;
                // push into the dispatch queue
                dispatch_q.push(ent);
                ent->when[S_FETCH] = current_cycle;
        }
}

static void dispatch_stage()
{
        // capacity of the reservation station
        const size_t cap = 2 * (K0 + K1 + K2);

        // pull as many as will fit
        while (!dispatch_q.empty() && sched_q.size() < cap)
        {
                inst_entry_t *ent = dispatch_q.front();
                dispatch_q.pop();

                // record dispatch cycle if you track it:
                // ent->when[S_DISPATCH] = current_cycle;

                // for each source operand
                for (int s = 0; s < 2; ++s)
                {
                        int32_t src = ent->inst.src_reg[s];
                        if (src < 0)
                        {
                                // no source
                                ent->src_ready[s] = true;
                                ent->src_tag[s] = 0;
                        }
                        else if (reg_status[src] == 0)
                        {
                                // register free
                                ent->src_ready[s] = true;
                                ent->src_tag[s] = 0;
                        }
                        else
                        {
                                // waiting on a producer
                                ent->src_ready[s] = false;
                                ent->src_tag[s] = reg_status[src];
                        }
                }

                // if this writes a destination, record that this tag will produce it
                int32_t dest = ent->inst.dest_reg;
                if (dest >= 0)
                {
                        reg_status[dest] = ent->tag;
                }

                // enter the scheduling queue
                sched_q.push_back(ent);
                ent->when[S_DISPATCH] = current_cycle;
        }
}

/**
 * Subroutine that simulates the processor.
 *   The processor should fetch instructions as appropriate, until all instructions have executed
 * XXX: You're responsible for completing this routine
 *
 * @p_stats Pointer to the statistics structure
 */
void run_proc(proc_stats_t *p_stats)
{
        // keep looping while we can still fetch, or have work in-flight
        while (more_to_fetch || !dispatch_q.empty() || !sched_q.empty()
               /* later: || any executing instructions */)
        {
                // second-half retirement happens first
                // state_update_stage();
                // complete any in-flight executes
                // execute_stage();
                // issue from reservation stations
                // schedule_stage();
                // move from dispatch→schedule
                // dispatch_stage();
                // bring in new ops from trace
                fetch_stage();

                // track dispatch-queue occupancy for stats here if you like
                current_cycle++;
                p_stats->cycle_count = current_cycle;
        }
}

/**
 * Subroutine for cleaning up any outstanding instructions and calculating overall statistics
 * such as average IPC, average fire rate etc.
 * XXX: You're responsible for completing this routine
 *
 * @p_stats Pointer to the statistics structure
 */
void complete_proc(proc_stats_t *p_stats) 
{
        printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");
        for (auto ent : all_entries)
        {
                printf("%u\t%u\t%u\t%u\t%u\t%u\n",
                       ent->tag,
                       ent->when[S_FETCH],
                       ent->when[S_DISPATCH],
                       ent->when[S_SCHEDULE],
                       ent->when[S_EXECUTE],
                       ent->when[S_STATE_UPDATE]);
                delete ent; // clean up
        }
}
