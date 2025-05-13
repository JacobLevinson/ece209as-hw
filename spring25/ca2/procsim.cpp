#include "procsim.hpp"
#include <queue>
#include <vector>
#include <array>
#include <algorithm>

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

// Completed-execution queue
static std::vector<inst_entry_t *> done_q;

// temporarily holds freshly fetched entries until next cycle’s dispatch
static std::vector<inst_entry_t *> fetch_buf;

// Functional‐unit availability counters
static unsigned free_k0, free_k1, free_k2;

static const unsigned LAT0 = 1;
static const unsigned LAT1 = 1;
static const unsigned LAT2 = 1;

static unsigned long sum_disp_size = 0;
static unsigned long max_disp_size = 0;
static unsigned long sum_inst_fired = 0;
static unsigned long total_retired = 0;

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
                        more_to_fetch = false;
                        break;
                }
                auto *ent = new inst_entry_t();
                all_entries.push_back(ent);

                ent->inst = base;
                ent->tag = next_tag++;
                ent->src_ready[0] = ent->src_ready[1] = false;
                ent->src_tag[0] = ent->src_tag[1] = 0;
                ent->remaining_lat = 0;
                ent->when[S_FETCH] = current_cycle;

                // *Don't* dispatch it immediately—hold it here
                fetch_buf.push_back(ent);
        }
}

static void dispatch_stage()
{
        // 1) Move last cycle’s fetches into the infinite dispatch queue
        for (auto *ent : fetch_buf)
        {
                // DISP: entering the dispatch queue in this cycle
                ent->when[S_DISPATCH] = current_cycle;
                dispatch_q.push(ent);
        }
        fetch_buf.clear();

        // (we leave the RS insertion to schedule_stage())
}

static void schedule_stage()
{
        // 1) Drain dispatch_q → sched_q up to capacity (this is SCHED)
        const size_t cap = 2 * (K0 + K1 + K2);
        while (!dispatch_q.empty() && sched_q.size() < cap)
        {
                inst_entry_t *ent = dispatch_q.front();
                dispatch_q.pop();

                // SCHED: entering the reservation station
                ent->when[S_SCHEDULE] = current_cycle;

                // resolve source operands
                for (int s = 0; s < 2; ++s)
                {
                        int32_t src = ent->inst.src_reg[s];
                        if (src < 0 || reg_status[src] == 0)
                        {
                                ent->src_ready[s] = true;
                                ent->src_tag[s] = 0;
                        }
                        else
                        {
                                ent->src_ready[s] = false;
                                ent->src_tag[s] = reg_status[src];
                        }
                }

                // set up for future write-back
                if (ent->inst.dest_reg >= 0)
                {
                        reg_status[ent->inst.dest_reg] = ent->tag;
                }

                sched_q.push_back(ent);
        }

        // 2) Collect all ready & never-yet-executed entries
        std::vector<inst_entry_t *> candidates;
        for (auto ent : sched_q)
        {
                if (ent->remaining_lat == 0 && ent->src_ready[0] && ent->src_ready[1] && ent->when[S_EXECUTE] == 0) // not yet issued
                {
                        candidates.push_back(ent);
                }
        }

        // 3) Issue in tag order
        std::sort(candidates.begin(), candidates.end(),
                  [](inst_entry_t *a, inst_entry_t *b)
                  {
                          return a->tag < b->tag;
                  });

        for (auto ent : candidates)
        {
                unsigned opc = ent->inst.op_code;
                if (opc < 0)
                        opc = 1; // default to k1
                unsigned *free_fu =
                    (opc == 0   ? &free_k0
                     : opc == 1 ? &free_k1
                                : &free_k2);

                if (*free_fu > 0)
                {
                        (*free_fu)--;

                        // EXEC: execution begins
                        ent->when[S_EXECUTE] = current_cycle;
                        sum_inst_fired++;

                        // set execution latency
                        unsigned lat = (opc == 0   ? LAT0
                                        : opc == 1 ? LAT1
                                                   : LAT2);
                        ent->remaining_lat = lat;
                }
        }
}

/// Execute stage: decrement latency and collect finished entries
/// Execute stage: decrement latency, collect finished entries, free FUs immediately
static void execute_stage()
{
        for (auto it = sched_q.begin(); it != sched_q.end(); /* nothing */)
        {
                inst_entry_t *ent = *it;
                if (ent->remaining_lat > 0)
                {
                        ent->remaining_lat--;
                        if (ent->remaining_lat == 0)
                        {

                                // retire next cycle, so queue it now
                                done_q.push_back(ent);
                        }
                }
                ++it;
        }
}

/// State-update (retire) stage: retire up to R instructions in tag order
static void state_update_stage()
{
        if (done_q.empty())
                return;

        // retire in tag order
        std::sort(done_q.begin(), done_q.end(),
                  [](inst_entry_t *a, inst_entry_t *b)
                  {
                          return a->tag < b->tag;
                  });

        unsigned to_retire = std::min<unsigned>(done_q.size(), R);
        for (unsigned i = 0; i < to_retire; ++i)
        {
                inst_entry_t *ent = done_q[i];

                // record state-update (retire) cycle
                ent->when[S_STATE_UPDATE] = current_cycle + 1;

                // count it
                total_retired++;

                // Free its FU
                unsigned opc = ent->inst.op_code;
                if (opc < 0)
                        opc = 1;
                if (opc == 0)
                        free_k0++;
                else if (opc == 1)
                        free_k1++;
                else
                        free_k2++;

                // write-back: clear reg_status if still pointing to this tag
                int32_t dest = ent->inst.dest_reg;
                if (dest >= 0 && reg_status[dest] == ent->tag)
                {
                        reg_status[dest] = 0;
                }

                // broadcast result to reservation stations
                for (auto other : sched_q)
                {
                        for (int s = 0; s < 2; ++s)
                        {
                                if (!other->src_ready[s] && other->src_tag[s] == ent->tag)
                                {
                                        other->src_ready[s] = true;
                                }
                        }
                }

                // remove from scheduling queue
                auto it = std::find(sched_q.begin(), sched_q.end(), ent);
                if (it != sched_q.end())
                        sched_q.erase(it);
        }

        // erase the retired entries from done_q
        done_q.erase(done_q.begin(), done_q.begin() + to_retire);
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
        current_cycle = 0;

        // Loop until there’s nothing left to fetch, dispatch, schedule, or retire
        while (more_to_fetch || !dispatch_q.empty() || !sched_q.empty() || !done_q.empty())
        {
                // Start a new cycle
                current_cycle++;
                p_stats->cycle_count = current_cycle;

                // ==== DEBUG TRACE for first few cycles ====
                if (current_cycle <= 8)
                {
                        // print to stderr so it doesn’t mix with your .output
                        fprintf(stderr, "=== Cycle %u ===\n", current_cycle);
                        fprintf(stderr, " fetch_buf: ");
                        for (auto *e : fetch_buf)
                                fprintf(stderr, "%u ", e->tag);
                        fprintf(stderr, "\n dispatch_q: ");
                        {
                                // peek without popping
                                std::queue<inst_entry_t *> tmp = dispatch_q;
                                while (!tmp.empty())
                                {
                                        fprintf(stderr, "%u ", tmp.front()->tag);
                                        tmp.pop();
                                }
                        }
                        fprintf(stderr, "\n sched_q: ");
                        for (auto *e : sched_q)
                                fprintf(stderr, "%u ", e->tag);
                        fprintf(stderr, "\n done_q:  ");
                        for (auto *e : done_q)
                                fprintf(stderr, "%u ", e->tag);
                        fprintf(stderr, "\n free_k0=%u, k1=%u, k2=%u\n\n",
                                free_k0, free_k1, free_k2);
                }

                // Finish any in-flight executions
                execute_stage();

                // Second-half of the cycle: retire and free resources
                state_update_stage();

                // First-half of this cycle: issue ready ops
                schedule_stage();

                // Second-half: move from dispatch→schedule
                dispatch_stage();

                // Track dispatch-queue stats
                unsigned dq_sz = dispatch_q.size();
                sum_disp_size += dq_sz;
                if (dq_sz > max_disp_size)
                        max_disp_size = dq_sz;

                // Finally, fetch up to F new ops
                fetch_stage();
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
        p_stats->retired_instruction = total_retired;
        p_stats->max_disp_size = max_disp_size;
        p_stats->avg_disp_size = (double)sum_disp_size / p_stats->cycle_count;
        p_stats->avg_inst_fired = (double)sum_inst_fired / p_stats->cycle_count;
        p_stats->avg_inst_retired = (double)total_retired / p_stats->cycle_count;

        // now print your per‐inst log
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
                delete ent;
        }
}