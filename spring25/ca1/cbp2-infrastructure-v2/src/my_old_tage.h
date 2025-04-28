// my_predictor.h
// This file contains a sample my_predictor class.
// It is a simple 32,768-entry gshare with a history length of 15.
// Note that this predictor doesn't use the whole 32 kilobytes available
// for the CBP-2 contest; it is just an example.

// my_predictor.h
// Improved TinyTAGE: 6 tables, smarter allocation, useful counters

class my_update : public branch_update
{
public:
        int provider; // Which table made the prediction (-1 = base)
        bool pred;    // What was the prediction
};

class my_predictor : public branch_predictor
{
public:
#define NHIST 6            // Now 6 tagged tables
#define TABLE_BITS 10      // 1024 entries per table
#define HISTORY_LENGTH 128 // Global history length
#define TAG_BITS 8         // 8 bits for tag
#define USEFUL_BITS 2      // 2-bit useful counter

        my_update u;
        branch_info bi;
        unsigned int history;
        unsigned char base[1 << TABLE_BITS]; // Base predictor
        struct entry_t
        {
                unsigned char ctr;    // 2-bit counter
                unsigned char tag;    // 8-bit tag
                unsigned char useful; // 2-bit useful counter
        } tage[NHIST][1 << TABLE_BITS];

        int hist_lengths[NHIST] = {4, 8, 16, 32, 64, 128}; // Longer histories now

        my_predictor(void) : history(0)
        {
                memset(base, 0, sizeof(base));
                memset(tage, 0, sizeof(tage));
        }

        branch_update *predict(branch_info &b)
        {
                bi = b;
                u.provider = -1;
                u.pred = true;

                if (b.br_flags & BR_CONDITIONAL)
                {
                        int best = -1;
                        for (int i = NHIST - 1; i >= 0; --i)
                        {
                                unsigned int idx = get_index(b.address, history, hist_lengths[i]);
                                if (tage[i][idx].tag == get_tag(b.address, history, hist_lengths[i]))
                                {
                                        best = i;
                                        break;
                                }
                        }

                        if (best != -1)
                        {
                                unsigned int idx = get_index(b.address, history, hist_lengths[best]);
                                u.provider = best;
                                u.pred = tage[best][idx].ctr >> 1;
                        }
                        else
                        {
                                unsigned int idx = b.address & ((1 << TABLE_BITS) - 1);
                                u.pred = base[idx] >> 1;
                        }
                }
                else
                {
                        u.pred = true;
                }

                u.direction_prediction(u.pred);
                u.target_prediction(0);
                return &u;
        }

        void update(branch_update *u, bool taken, unsigned int target)
        {
                if (bi.br_flags & BR_CONDITIONAL)
                {
                        my_update *mu = (my_update *)u;
                        if (mu->provider == -1)
                        {
                                unsigned int idx = bi.address & ((1 << TABLE_BITS) - 1);
                                if (taken)
                                {
                                        if (base[idx] < 3)
                                                base[idx]++;
                                }
                                else
                                {
                                        if (base[idx] > 0)
                                                base[idx]--;
                                }
                        }
                        else
                        {
                                unsigned int idx = get_index(bi.address, history, hist_lengths[mu->provider]);
                                if (taken)
                                {
                                        if (tage[mu->provider][idx].ctr < 3)
                                                tage[mu->provider][idx].ctr++;
                                }
                                else
                                {
                                        if (tage[mu->provider][idx].ctr > 0)
                                                tage[mu->provider][idx].ctr--;
                                }

                                // Promote usefulness if correct prediction
                                if (((tage[mu->provider][idx].ctr >> 1) == taken) && (tage[mu->provider][idx].useful < 3))
                                        tage[mu->provider][idx].useful++;
                                else if (tage[mu->provider][idx].useful > 0)
                                        tage[mu->provider][idx].useful--;
                        }

                        // Allocate new entries if misprediction and no good alternate
                        if (mu->pred != taken)
                        {
                                for (int i = 0; i < NHIST; ++i)
                                {
                                        unsigned int idx = get_index(bi.address, history, hist_lengths[i]);
                                        if (tage[i][idx].useful == 0)
                                        {
                                                tage[i][idx].tag = get_tag(bi.address, history, hist_lengths[i]);
                                                tage[i][idx].ctr = taken ? 2 : 1;
                                                tage[i][idx].useful = 0;
                                                break;
                                        }
                                }
                        }

                        // Always update global history
                        history = (history << 1) | (taken ? 1 : 0);
                        history &= (1 << HISTORY_LENGTH) - 1;
                }
        }

private:
        unsigned int get_index(unsigned int addr, unsigned int hist, int histlen)
        {
                return (addr ^ (hist & ((1 << histlen) - 1))) & ((1 << TABLE_BITS) - 1);
        }

        unsigned int get_tag(unsigned int addr, unsigned int hist, int histlen)
        {
                return ((addr >> 4) ^ (hist >> (histlen / 2))) & ((1 << TAG_BITS) - 1);
        }
};
