// procsim.hpp
#ifndef PROCSIM_HPP
#define PROCSIM_HPP

#include <cstdint>
#include <cstdio>
#include <vector>

// Default functional unit counts and fetch width
#define DEFAULT_K0 1
#define DEFAULT_K1 2
#define DEFAULT_K2 3
#define DEFAULT_R 8
#define DEFAULT_F 4

enum InstrStage
{
    STAGE_INST = 0, // the “instruction number” we print (tag+1)
    STAGE_FETCH,    // cycle when fetched
    STAGE_DISP,     // cycle when dispatched
    STAGE_SCHED,    // cycle when scheduled into an FU
    STAGE_EXEC,     // cycle when execution started
    STAGE_STATE,    // cycle when result was written back
    NUM_STAGES      // number of stages
};

// Represents a single instruction flowing through the pipeline.
// Tracks timing for each stage and readiness of its sources.
typedef struct _proc_inst_t
{
    uint32_t instruction_address; // Program counter of the instruction
    int32_t op_code;              // Operation type (index into FU pool)
    int32_t src_reg[2];           // Source register indices (128 means “no register”)
    int32_t dest_reg;             // Destination register
    int src_tag[2];               // Tags to track when operands are ready
    int dest_tag;                 // Unique tag assigned to this instruction
    bool src_ready[2];            // Whether each source operand is available
    bool fired;                   // Has the instruction been issued to an FU?
    bool dispatched;              // Has it left the fetch/dispatch queue?
    bool _null;                   // True if read_instruction hit EOF
    int fu_wait;                  // Cycles spent waiting in the FU
    int fet_cyc;                  // Cycle when fetched
    int disp_cyc;                 // Cycle when dispatched
    int sched_cyc;                // Cycle when scheduled into a FU
    int exec_cyc;                 // Cycle when execution starts
    int stateUp_cyc;              // Cycle when result is written back
    bool completed;               // Has it finished write-back?
    bool fu_busy;                 // Is it currently occupying an FU?
    bool CDB_busy;                // Is it waiting on a result bus?

    // Initialize flags and tags to defaults
    _proc_inst_t()
    {
        for (int i = 0; i < 2; i++)
        {
            src_ready[i] = false;
            src_tag[i] = -1;
        }
        dispatched = completed = fired = fu_busy = CDB_busy = false;
        fet_cyc = disp_cyc = sched_cyc = exec_cyc = stateUp_cyc = -1;
        fu_wait = 0;
        _null = false;
    }
} proc_inst_t;

// Holds final performance metrics for the run
typedef struct _proc_stats_t
{
    float avg_inst_retired;            // Instructions retired per cycle
    float avg_inst_fired;              // Instructions issued per cycle
    float avg_disp_size;               // Average dispatch queue occupancy
    unsigned long max_disp_size;       // Maximum dispatch queue occupancy
    unsigned long retired_instruction; // Total instructions processed
    unsigned long cycle_count;         // Total cycles taken
} proc_stats_t;

// Reads one instruction line from stdin into p_inst.
// Returns true if a valid instruction was parsed.
bool read_instruction(proc_inst_t *p_inst);

// Configure the pipeline:
//   robSize       : number of entries in the Re-Order Buffer (result buses)
//   fu0Count,1,2  : number of functional units of each type
//   fetchWidth    : how many instructions we fetch per cycle
void setup_proc(uint64_t robSize,
                uint64_t fu0Count,
                uint64_t fu1Count,
                uint64_t fu2Count,
                uint64_t fetchWidth);

// Runs the simulation until all instructions complete
void run_proc(proc_stats_t *p_stats);

// Final cleanup, prints per-instruction timing and computes averages
void complete_proc(proc_stats_t *p_stats);

// Internal helper classes -------------------------------------------------

// Tracks a single functional unit slot
class Function_k
{
public:
    bool busy;    // Is the unit occupied?
    int tag;      // Which instruction tag is using it?
    int dest_reg; // Destination register for the result
    Function_k() : busy(false), tag(-1), dest_reg(-1) {}
};

// A pool of Function_k units, dynamically sized
class Function_Unit
{
public:
    std::vector<Function_k> units;
};

// Models a register’s availability and tag in the register file
class Register_File
{
public:
    bool ready; // true if value is valid
    int tag;    // tag of the instruction that will produce it
    Register_File() : ready(true), tag(-1) {}
};

#endif // PROCSIM_HPP
