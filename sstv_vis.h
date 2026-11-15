/* SPDX-License-Identifier: GPL-3.0-only */
/* Coherent VIS fallback: decimated I/Q, 20 ms windows, 2 ms energy bins. */
#define VE_BANKS 40
#define VE_BINS 256

typedef struct VisEnergy {
    float re[VE_BANKS], im[VE_BANKS], cr[VE_BANKS], ci[VE_BANKS], nr[VE_BANKS], ni[VE_BANKS];
    uint8_t e[VE_BINS][VE_BANKS];
    float *raw;
    double hop;
    int N, pos, rebase, rebase_period, ready, active;
    uint32_t j, next, deadline, count[NMODES], first[NMODES], last[NMODES];
    float score[NMODES], offset[NMODES];
} VisEnergy;

static int ve_slot(int bin) { return bin < 30 ? bin - 3 : bin - 34 + 27; }
static int ve_bin(int slot) { return slot < 27 ? slot + 3 : slot - 27 + 34; }

static VisEnergy *ve_new(double Fs)
{
    VisEnergy *v = d_calloc(1, sizeof *v);
    int b;
    if (!v) return 0;
    v->N = iround(.020 * Fs); v->hop = .002 * Fs;
    v->rebase = v->rebase_period = iround(Fs);
    v->next = (uint32_t)(v->hop + .5);
    v->raw = d_calloc((size_t)(2 * v->N), sizeof(float));
    if (!v->raw) { D_FREE(v); return 0; }
    for (b = 0; b < VE_BANKS; b++) {
        double w = 2 * PI_D * (900 + 25 * ve_bin(b) - FC_HZ) / Fs;
        v->cr[b] = (float)m_cos(w); v->ci[b] = (float)m_sin(w);
        v->nr[b] = (float)m_cos(w * v->N); v->ni[b] = (float)m_sin(w * v->N);
    }
    return v;
}

static void ve_free(VisEnergy *v) { if (v) { D_FREE(v->raw); D_FREE(v); } }

static void ve_reset(VisEnergy *v, uint32_t at)
{
    memset(v->raw, 0, (size_t)(2 * v->N) * sizeof(float));
    memset(v->re, 0, sizeof v->re); memset(v->im, 0, sizeof v->im);
    memset(v->e, 0, sizeof v->e);
    v->pos = v->ready = v->active = 0;
    v->rebase = v->rebase_period;
    v->j = (uint32_t)(at / v->hop);
    v->next = (uint32_t)((v->j + 1.0) * v->hop + .5);
}

static float ve_ratio(const VisEnergy *v, uint32_t j, int b, int tone)
{
    const uint8_t *e = v->e[j & (VE_BINS - 1)];
    float sum = (float)(e[ve_slot(b)] + e[ve_slot(b+4)] + e[ve_slot(b+8)] +
                        e[ve_slot(b+16)] + e[ve_slot(b+32)]);
    return sum ? e[ve_slot(b+tone)] / sum : 0;
}

/* Rebase once a second to bound float sliding-DFT drift while idle. */
static void ve_rebase(VisEnergy *v)
{
    int b, k;
    for (b = 0; b < VE_BANKS; b++) {
        float re = 0, im = 0, wr = 1, wi = 0;
        int p = v->pos;
        for (k = 0; k < v->N; k++) {
            float r;
            if (--p < 0) p = v->N - 1;
            re += v->raw[2*p] * wr - v->raw[2*p+1] * wi;
            im += v->raw[2*p] * wi + v->raw[2*p+1] * wr;
            r = wr * v->cr[b] - wi * v->ci[b];
            wi = wr * v->ci[b] + wi * v->cr[b]; wr = r;
        }
        v->re[b] = re; v->im[b] = im;
    }
}

static int ve_feed(VisEnergy *v, float xr, float xi, uint32_t at,
                   const Mode **mode, double *end, float *offset)
{
    int b, p = v->pos;
    float oldr = v->raw[2*p], oldi = v->raw[2*p+1], peak = 0, powers[VE_BANKS];
    uint32_t j;
    v->raw[2*p] = xr; v->raw[2*p+1] = xi;
    if (++v->pos == v->N) v->pos = 0;
    for (b = 0; b < VE_BANKS; b++) {
        float r = v->re[b] * v->cr[b] - v->im[b] * v->ci[b] + xr -
                  (oldr * v->nr[b] - oldi * v->ni[b]);
        v->im[b] = v->re[b] * v->ci[b] + v->im[b] * v->cr[b] + xi -
                   (oldr * v->ni[b] + oldi * v->nr[b]);
        v->re[b] = r;
    }
    if (--v->rebase == 0) { ve_rebase(v); v->rebase = v->rebase_period; }
    if (at < v->next) return 0;
    j = v->j++;
    v->next = (uint32_t)((v->j + 1.0) * v->hop + .5);
    for (b = 0; b < VE_BANKS; b++) {
        powers[b] = v->re[b] * v->re[b] + v->im[b] * v->im[b];
        if (powers[b] > peak) peak = powers[b];
    }
    peak = peak > 1e-20f ? 255 / peak : 0;
    for (b = 0; b < VE_BANKS; b++) {
        float q = powers[b] * peak + .5f;
        v->e[j & (VE_BINS - 1)][b] = (uint8_t)(q >= 255 ? 255 : (int)q);
    }
    if (v->ready < 250) { v->ready++; return 0; }
    {
        float best = 0; int bc = 0, bb = 0;
        for (b = 3; b <= 13; b++) {
            float lead = ve_ratio(v, j-200, b, 32), s, t, score;
            int code = 0, ones = 0, ok = 1, bit;
            const Mode *m;
            if (lead < .60f || ve_ratio(v, j-250, b, 32) < .60f) continue;
            s = ve_ratio(v, j-2, b, 4); t = ve_ratio(v, j-137, b, 4);
            if (s < .48f || t < .48f) continue;
            score = lead + s + t;
            for (bit = 0; bit < 8; bit++) {
                float a = ve_ratio(v, j-122+15*bit, b, 0), z = ve_ratio(v, j-122+15*bit, b, 8);
                if (a > z && a > .48f) { code |= 1 << bit; ones++; score += a; }
                else if (z > a && z > .48f) score += z;
                else { ok = 0; break; }
            }
            m = mode_by_vis(code & 127);
            if (ok && !(ones & 1) && m && score > best) { best = score; bc = (int)(m - MODES); bb = b; }
        }
        if (best > 0) {
            if (!v->active) {
                v->active = 1; v->deadline = j + 20;
                memset(v->count, 0, sizeof v->count); memset(v->score, 0, sizeof v->score);
            }
            if (!v->count[bc]) v->first[bc] = j;
            v->last[bc] = j; v->count[bc]++;
            if (best > v->score[bc]) {
                const uint8_t *e = v->e[(j-200) & (VE_BINS - 1)];
                int bin = bb + 32;
                float a = e[ve_slot(bin-1)], z = e[ve_slot(bin+1)];
                float den = a - 2 * e[ve_slot(bin)] + z;
                float d = den < 0 ? .5f * (a-z) / den : 0;
                if (d < -.5f) d = -.5f;
                if (d > .5f) d = .5f;
                v->score[bc] = best; v->offset[bc] = 900 + 25*bb - 1100 + 25*d;
            }
        }
    }
    if (v->active && j >= v->deadline) {
        int c, best = -1;
        v->active = 0;
        for (c = 0; c < NMODES; c++)
            if ((v->count[c] >= 2 || (v->count[c] && v->score[c] >= 7.7f)) && (best < 0 || v->count[c] > v->count[best] ||
                (v->count[c] == v->count[best] && v->score[c] > v->score[best]))) best = c;
        if (best >= 0) {
            *mode = &MODES[best];
            *end = (1 + .5 * (v->first[best] + v->last[best])) * v->hop + 1.5;
            *offset = v->offset[best];
            return 1;
        }
    }
    return 0;
}
