// procsim.cpp
#include "procsim.hpp"
#include <queue>
#include <vector>
#include <array>
#include <algorithm>

//----------------------------------------------------------------------
// configuration (set in setup_proc)
static uint64_t R, K0, K1, K2, F; // result buses, # of FUs, fetch rate
static unsigned next_tag;         // tag generator
static bool more_to_fetch;        // still instructions in trace?
static unsigned current_cycle;    // global cycle counter

static unsigned long sum_disp_size = 0, max_disp_size = 0;
static unsigned long sum_inst_fired = 0, total_retired = 0;

const unsigned LAT0 = 1; // latency of k0
const unsigned LAT1 = 1; // latency of k1
const unsigned LAT2 = 1; // latency of k2

// pipeline stage indices
enum stage_t
{
        S_FETCH = 0,
        S_DISPATCH,
        S_SCHEDULE,
        S_EXECUTE,
        S_STATE_UPDATE
};

//----------------------------------------------------------------------
// instruction bookkeeping
struct inst_entry_t
{
        proc_inst_t inst;       // the raw micro-op
        unsigned tag;           // sequential tag
        bool src_ready[2];      // operand ready?
        unsigned src_tag[2];    // if not, which tag
        unsigned remaining_lat; // for execute
        unsigned when[5];       // cycle numbers for each stage
};
static std::vector<inst_entry_t *> all_entries; // keep every entry for final dump

//----------------------------------------------------------------------
// "Pipeline registers" between stages

// Fetch → Dispatch
static std::vector<inst_entry_t *> fetch_buffer;

// Dispatch → Scheduling (unbounded)
static std::queue<inst_entry_t *> dispatch_queue;

// Scheduling → Execute  (reservation station, capacity = 2*(K0+K1+K2))
static std::vector<inst_entry_t *> scheduling_queue;

// Execute → State-Update  (instructions that have finished execution this cycle)
static std::vector<inst_entry_t *> done_queue;

//----------------------------------------------------------------------
// Register-alias table & FU availability
static std::array<unsigned, 128> reg_status; // which tag will write each arch reg
static unsigned free_k0, free_k1, free_k2;   // available FUs of each type

static std::vector<unsigned> just_broadcast;
//----------------------------------------------------------------------
// Stage prototypes
static void fetch_stage();
static void dispatch_stage();
static void schedule_stage();
static void execute_stage();
static void state_update_stage();

//----------------------------------------------------------------------
// Initialization
void setup_proc(uint64_t r, uint64_t k0, uint64_t k1, uint64_t k2, uint64_t f)
{
        R = r;
        K0 = k0;
        K1 = k1;
        K2 = k2;
        F = f;

        sum_disp_size = 0;
        max_disp_size = 0;
        sum_inst_fired = 0;
        total_retired = 0;

        more_to_fetch = true;
        next_tag = 1;
        current_cycle = 0;

        fetch_buffer.clear();
        while (!dispatch_queue.empty())
                dispatch_queue.pop();
        scheduling_queue.clear();
        done_queue.clear();
        all_entries.clear();

        reg_status.fill(0);
        free_k0 = K0;
        free_k1 = K1;
        free_k2 = K2;
}

//----------------------------------------------------------------------
// The “reverse-order” cycle
void run_proc(proc_stats_t *p_stats)
{
        // run until no stage has any work
        while (more_to_fetch || !fetch_buffer.empty() || !dispatch_queue.empty() || !scheduling_queue.empty() || !done_queue.empty())
        {
                ++current_cycle;
                p_stats->cycle_count = current_cycle;
                just_broadcast.clear();
                // 5) retire & free buses
                state_update_stage();

                // 4) complete executions
                execute_stage();

                // 3) issue from reservation station (and allocate FUs)
                schedule_stage();

                // 2) move fetch_buffer → dispatch_queue
                dispatch_stage();

                auto dq = dispatch_queue.size();
                sum_disp_size += dq;
                max_disp_size = std::max(max_disp_size, (unsigned long)dq);

                // 1) fetch up to F from trace into fetch_buffer
                fetch_stage();
        }
}

//----------------------------------------------------------------------
// Final cleanup & stats
void complete_proc(proc_stats_t *p_stats)
{
        // 1) Fill in the summary statistics in the proc_stats_t struct
        p_stats->retired_instruction = total_retired;
        p_stats->max_disp_size = max_disp_size;
        p_stats->avg_disp_size = (double)sum_disp_size / p_stats->cycle_count;
        p_stats->avg_inst_fired = (double)sum_inst_fired / p_stats->cycle_count;
        p_stats->avg_inst_retired = (double)total_retired / p_stats->cycle_count;

        // 2) Print the per‐instruction timestamps
        printf("INST\tFETCH\tDISP\tSCHED\tEXEC\tSTATE\n");
        for (auto *e : all_entries)
        {
                printf("%u\t%u\t%u\t%u\t%u\t%u\n",
                       e->tag,
                       e->when[S_FETCH],
                       e->when[S_DISPATCH],
                       e->when[S_SCHEDULE],
                       e->when[S_EXECUTE],
                       e->when[S_STATE_UPDATE]);
                delete e;
        }
}

//----------------------------------------------------------------------
// Stage stubs to fill in…

static void fetch_stage()
{
        if (!more_to_fetch)
                return;

        for (uint64_t i = 0; i < F; ++i)
        {
                proc_inst_t inst;
                if (!read_instruction(&inst))
                {
                        // no more in the trace
                        more_to_fetch = false;
                        break;
                }

                // allocate a new entry
                auto *e = new inst_entry_t();

                // save to full list for later dump
                all_entries.push_back(e);

                // copy in the raw bits
                e->inst = inst;

                // assign a tag
                e->tag = next_tag++;

                // clear timing fields
                for (int s = 0; s < 5; ++s)
                        e->when[s] = 0;

                // record this stage
                e->when[S_FETCH] = current_cycle;

                // init dependency bits (will be set in dispatch)
                e->src_ready[0] = e->src_ready[1] = false;
                e->src_tag[0] = e->src_tag[1] = 0;
                e->remaining_lat = 0;

                // push into the next pipeline register
                fetch_buffer.push_back(e);
        }
}

static void dispatch_stage()
{
        // move everything that was fetched last cycle into the dispatch queue
        for (auto *e : fetch_buffer)
        {
                // record dispatch timestamp
                e->when[S_DISPATCH] = current_cycle;
                dispatch_queue.push(e);
        }
        fetch_buffer.clear();
}

static void schedule_stage()
{
        // 1) Move from dispatch_queue → scheduling_queue up to capacity
        const size_t cap = 2 * (K0 + K1 + K2);
        while (!dispatch_queue.empty() && scheduling_queue.size() < cap)
        {
                inst_entry_t *e = dispatch_queue.front();
                dispatch_queue.pop();

                // record that we entered the RS this cycle
                e->when[S_SCHEDULE] = current_cycle;

                // resolve source operands now
                for (int s = 0; s < 2; ++s)
                {
                        int32_t src = e->inst.src_reg[s];
                        if (src < 0 || reg_status[src] == 0)
                        {
                                e->src_ready[s] = true;
                                e->src_tag[s] = 0;
                        }
                        else
                        {
                                e->src_ready[s] = false;
                                e->src_tag[s] = reg_status[src];
                        }
                }

                // set RAT for the destination
                if (e->inst.dest_reg >= 0)
                {
                        reg_status[e->inst.dest_reg] = e->tag;
                }

                scheduling_queue.push_back(e);
        }

        // **NO** FU checks or when[S_EXECUTE] here any more.
}

static void execute_stage()
{
        // --- (1) Issue / fire any ready instructions ---
        // Gather all entries that
        //  * are in the RS (scheduling_queue),
        //  * have not yet been issued (when[S_EXECUTE]==0),
        //  * and whose operands are ready.
        std::vector<inst_entry_t *> cand;
        for (auto *e : scheduling_queue) {
                if (e->when[S_EXECUTE]==0 && e->src_ready[0] && e->src_ready[1]) {
                  // check that neither src_tag is in just_broadcast
                  bool blocked = false;
                  for (unsigned t : just_broadcast)
                    if (e->src_tag[0]==t || e->src_tag[1]==t) {
                      blocked = true;
                      break;
                    }
                  if (!blocked) cand.push_back(e);
                }
              }
        // Sort by tag to enforce in-order fire
        std::sort(cand.begin(), cand.end(),
        [](inst_entry_t *a, inst_entry_t *b) { return a->tag < b->tag; });

        // Try to fire each in tag order, up to FU availability
        for (auto *e : cand)
        {
                unsigned opc = e->inst.op_code;
                if (opc < 0)
                {
                        opc = 1;
                }
                unsigned *fu = (opc == 0   ? &free_k0
                                : opc == 1 ? &free_k1
                                           : &free_k2);
                if (*fu > 0)
                {
                        (*fu)--;
                        sum_inst_fired++;

                        // record execution start this cycle
                        e->when[S_EXECUTE] = current_cycle;
                        // set its execution latency
                        e->remaining_lat = (opc == 0   ? LAT0
                                            : opc == 1 ? LAT1
                                                       : LAT2);
                }
        }

        // --- (2) Advance all in-flight executions ---
        std::vector<inst_entry_t *> just_finished;
        for (auto *e : scheduling_queue)
        {
                if (e->remaining_lat > 0)
                {
                        e->remaining_lat--;
                        if (e->remaining_lat == 0)
                        {
                                just_finished.push_back(e);
                        }
                }
        }

        // Tie‐break same‐cycle completions by tag
        std::sort(just_finished.begin(), just_finished.end(),
                  [](inst_entry_t *a, inst_entry_t *b)
                  { return a->tag < b->tag; });

        // Enqueue into done_queue in that order
        for (auto *e : just_finished)
        {
                done_queue.push_back(e);
        }
}

static void state_update_stage()
{
        if (done_queue.empty())
                return;

        // retire in tag order
        unsigned num = std::min<unsigned>(done_queue.size(), R);
        for (unsigned i = 0; i < num; ++i)
        {
                auto *e = done_queue[i];

                // record state‐update
                e->when[S_STATE_UPDATE] = current_cycle;

                // Cant execute on this same cycle
                just_broadcast.push_back(e->tag);

                // Count this as retired
                total_retired++;

                // free its FU
                unsigned opc = e->inst.op_code;
                if (opc < 0){
                        opc = 1;
                }
                if (opc == 0)
                        ++free_k0;
                else if (opc == 1)
                        ++free_k1;
                else
                        ++free_k2;

                // write‐back: clear RAT if still pointing
                int32_t dest = e->inst.dest_reg;
                if (dest >= 0 && reg_status[dest] == e->tag)
                        reg_status[dest] = 0;

                // broadcast to RS
                for (auto *other : scheduling_queue)
                {
                        for (int s = 0; s < 2; ++s)
                        {
                                if (!other->src_ready[s] && other->src_tag[s] == e->tag)
                                        other->src_ready[s] = true;
                        }
                }

                // remove from scheduling queue
                scheduling_queue.erase(
                    std::find(scheduling_queue.begin(), scheduling_queue.end(), e));
        }

        // drop them from done_queue
        done_queue.erase(done_queue.begin(), done_queue.begin() + num);
}

