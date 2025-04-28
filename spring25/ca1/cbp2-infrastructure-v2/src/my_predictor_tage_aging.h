// my_predictor.h
// Improved TinyTAGE with Corrected Aging

class my_update : public branch_update
{
public:
	int provider;
	bool pred;
	bool alt_pred;
};

class my_predictor : public branch_predictor
{
public:
#define NHIST 6
#define TABLE_BITS 10
#define HISTORY_LENGTH 128
#define TAG_BITS 8
#define USEFUL_BITS 2

	my_update u;
	branch_info bi;
	unsigned int history;
	unsigned char base[1 << TABLE_BITS];
	struct entry_t
	{
		unsigned char ctr;
		unsigned char tag;
		unsigned char useful;
	} tage[NHIST][1 << TABLE_BITS];

	int hist_lengths[NHIST] = {4, 8, 16, 32, 64, 128};

	// === Stats
	unsigned long long total_preds = 0;
	unsigned long long base_preds = 0;
	unsigned long long tage_preds[NHIST] = {0};
	unsigned long long base_mispreds = 0;
	unsigned long long tage_mispreds[NHIST] = {0};
	unsigned long long allocations = 0;
	unsigned long long successful_allocations = 0;

	// === Aging support ===
	unsigned long long aging_counter = 0;
	const unsigned long long AGING_PERIOD = 1000000; // Age every 1 million predictions

	my_predictor(void) : history(0)
	{
		memset(base, 0, sizeof(base));
		memset(tage, 0, sizeof(tage));
	}

	~my_predictor()
	{
		FILE *f = fopen("predictor_stats.txt", "a");
		if (f)
		{
			fprintf(f, "\n=== Predictor Statistics ===\n");
			fprintf(f, "Total predictions: %llu\n", total_preds);
			fprintf(f, "Base predictor predictions: %llu\n", base_preds);
			fprintf(f, "Table hits:\n");
			for (int i = 0; i < NHIST; ++i)
				fprintf(f, "  Table %d hits: %llu\n", i, tage_preds[i]);
			fprintf(f, "Base mispredictions: %llu\n", base_mispreds);
			fprintf(f, "Table mispredictions:\n");
			for (int i = 0; i < NHIST; ++i)
				fprintf(f, "  Table %d mispreds: %llu\n", i, tage_mispreds[i]);
			fprintf(f, "Allocations attempted: %llu\n", allocations);
			fprintf(f, "Successful allocations: %llu\n", successful_allocations);
			fprintf(f, "==============================\n");
			fclose(f);
		}
	}

	branch_update *predict(branch_info &b)
	{
		bi = b;
		u.provider = -1;
		u.pred = true;
		u.alt_pred = true;
		total_preds++;
		aging_counter++;

		if (aging_counter >= AGING_PERIOD)
		{
			perform_aging();
			aging_counter = 0;
		}

		if (b.br_flags & BR_CONDITIONAL)
		{
			int best = -1;
			int alt = -1;
			for (int i = NHIST - 1; i >= 0; --i)
			{
				unsigned int idx = get_index(b.address, history, hist_lengths[i]);
				if (tage[i][idx].tag == get_tag(b.address, history, hist_lengths[i]))
				{
					if (best == -1)
						best = i;
					else if (alt == -1)
						alt = i;
				}
			}

			if (best != -1)
			{
				unsigned int idx = get_index(b.address, history, hist_lengths[best]);
				u.provider = best;
				u.pred = tage[best][idx].ctr >> 1;
				tage_preds[best]++;

				if (alt != -1)
				{
					unsigned int alt_idx = get_index(b.address, history, hist_lengths[alt]);
					u.alt_pred = tage[alt][alt_idx].ctr >> 1;
				}
				else
				{
					unsigned int idx2 = b.address & ((1 << TABLE_BITS) - 1);
					u.alt_pred = base[idx2] >> 1;
				}
			}
			else
			{
				unsigned int idx = b.address & ((1 << TABLE_BITS) - 1);
				u.pred = base[idx] >> 1;
				u.alt_pred = u.pred;
				base_preds++;
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
			bool correct = (mu->pred == taken);

			if (!correct)
			{
				if (mu->provider == -1)
					base_mispreds++;
				else
					tage_mispreds[mu->provider]++;
			}

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

				if (((tage[mu->provider][idx].ctr >> 1) == taken) && (tage[mu->provider][idx].useful < 3))
					tage[mu->provider][idx].useful++;
				else if (tage[mu->provider][idx].useful > 0)
					tage[mu->provider][idx].useful--;
			}

			bool provider_weak = true;
			if (mu->provider != -1)
			{
				unsigned int idx = get_index(bi.address, history, hist_lengths[mu->provider]);
				provider_weak = (tage[mu->provider][idx].ctr == 1 || tage[mu->provider][idx].ctr == 2);
			}

			if (!correct && provider_weak && (mu->alt_pred != taken))
			{
				allocations++;

				int best_victim = -1;
				int min_useful = 4;

				for (int i = 0; i < NHIST; ++i)
				{
					unsigned int idx = get_index(bi.address, history, hist_lengths[i]);
					if (tage[i][idx].useful < min_useful)
					{
						best_victim = i;
						min_useful = tage[i][idx].useful;
					}
				}

				if (best_victim != -1)
				{
					unsigned int idx = get_index(bi.address, history, hist_lengths[best_victim]);
					tage[best_victim][idx].tag = get_tag(bi.address, history, hist_lengths[best_victim]);
					tage[best_victim][idx].ctr = taken ? 2 : 1;
					tage[best_victim][idx].useful = 0;
					successful_allocations++;
				}
			}

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

	void perform_aging()
	{
		for (int i = 0; i < NHIST; ++i)
		{
			for (int j = 0; j < (1 << TABLE_BITS); ++j)
			{
				if ((rand() % 100) < 3) // Age only 3% randomly
				{
					if (tage[i][j].useful > 0)
						tage[i][j].useful--;
				}
			}
		}
	}
};
