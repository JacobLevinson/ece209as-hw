import numpy as np
import matplotlib.pyplot as plt

# ------------------------------------------
#  GPU peak numbers     (Titan V, FP32)
FLOP_peak  = 13.8e12            # FLOP/s
BW_peak    = 652e9              # Byte/s
AI_tilt    = FLOP_peak / BW_peak
# ------------------------------------------

kernels = ["Conv1", "Conv2", "FC1", "FC2"]
gflops  = np.array([71.4, 53.9, 16.9, 9.11]) * 1e9   # -> FLOP/s
bw      = np.array([94.21, 2.09, 73.0, 20.4]) * 1e9  # -> B/s
ai      = gflops / bw

# Roof-line
ai_line = np.logspace(-1, 4, 256)
roof    = np.minimum(BW_peak * ai_line, FLOP_peak)

fig, ax = plt.subplots(figsize=(8,6))
ax.loglog(ai_line, roof/1e12, '-', lw=1.5, label="Roof-line")
ax.axvline(AI_tilt, ls=':', color='grey')
ax.text(AI_tilt*1.05, FLOP_peak/1e12*0.8,
        "AI_tilt ≈ {:.0f} flop/byte".format(AI_tilt), rotation=90,
        va='center', color='grey')

ax.scatter(ai, gflops/1e12, s=80, marker='o', color='black')
for i, txt in enumerate(kernels):
    ax.annotate(txt, (ai[i]*1.1, gflops[i]/1e12*1.1))

ax.set_xlabel("Arithmetic intensity [FLOP / B]")
ax.set_ylabel("Performance [TFLOP s⁻¹]")
ax.set_title("Roof-line — TITAN V, batch = 1")
ax.grid(True, which='both')
plt.tight_layout()
plt.savefig("roofline_titanV.png", dpi=200)
print("saved figure → roofline_titanV.png")
