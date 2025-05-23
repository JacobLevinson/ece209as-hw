// trace.h
// This file declares functions and a struct for reading trace files.

// these #define the Unix commands for decompressing gzip, bzip2, and
// plain files.  If they are somewhere else on your system, change these
// definitions.

#define ZCAT            "/usr/bin/gzip -dc"
#define BZCAT           "/usr/bin/bzip2 -dc"
#define CAT             "/usr/bin/cat"

struct trace {
	bool	taken;
	unsigned int target;
	branch_info bi;
};

void init_trace (char *);
trace *read_trace (void);
void end_trace (void);
