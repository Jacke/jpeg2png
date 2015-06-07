#include <stdint.h>
#include <string.h>

#include "jpeg2png.h"
#include "compute.h"
#include "utils.h"
#include "box.h"
#include "logger.h"

#include "ooura/dct.h"

#ifdef USE_SIMD
  #include "compute_simd_step.c"
  #define compute_step_tv compute_step_tv_simd
#else
  #define compute_step_tv compute_step_tv_c
#endif

POSSIBLY_UNUSED static double compute_step_tv_c(unsigned w, unsigned h, float *in, float *objective_gradient, float *in_x, float *in_y) {
        double tv = 0.;
        for(unsigned y = 0; y < h; y++) {
                for(unsigned x = 0; x < w; x++) {
                        // forward gradient x
                        float g_x = x >= w-1 ? 0. : *p(in, x+1, y, w, h) - *p(in, x, y, w, h);
                        // forward gradient y
                        float g_y = y >= h-1 ? 0. : *p(in, x, y+1, w, h) - *p(in, x, y, w, h);
                        // norm
                        float g_norm = sqrt(sqr(g_x) + sqr(g_y));
                        tv += g_norm;
                        // compute derivatives
                        if(g_norm != 0) {
                                *p(objective_gradient, x, y, w, h) += -(g_x + g_y) / g_norm;
                                if(x < w-1) {
                                        *p(objective_gradient, x+1, y, w, h) += g_x / g_norm;
                                }
                                if(y < h-1) {
                                        *p(objective_gradient, x, y+1, w, h) += g_y / g_norm;
                                }
                        }
                        *p(in_x, x, y, w, h) = g_x;
                        *p(in_y, x, y, w, h) = g_y;
                }
        }
        return tv;
}

POSSIBLY_UNUSED static void verify_compute_step_tv(unsigned w, unsigned h, double tv, float *in, float *objective_gradient, float *in_x, float *in_y) {
        puts("verify");
        float *objective_gradient_ = alloc_real(w*h);
        float *in_x_ = alloc_real(w*h);
        float *in_y_ = alloc_real(w*h);
        double tv_c = compute_step_tv_c(w, h, in, objective_gradient_, in_x_, in_y_);
        compare("in_x", w, h, in_x_, in_x);
        compare("in_y", w, h, in_y_, in_y);
        compare("objective_gradient", w, h, objective_gradient_, objective_gradient);
        printf("simd %f, original %f\n", tv, tv_c);
        free_real(objective_gradient_);
        free_real(in_y_);
        free_real(in_x_);
}

static double compute_step_tv2(unsigned w, unsigned h, float *objective_gradient, float *in_x, float *in_y, float alpha) {
        double tv2 = 0.;
        for(unsigned y = 0; y < h; y++) {
                for(unsigned x = 0; x < w; x++) {
                        // backward x
                        float g_xx = x <= 0 ? 0. : *p(in_x, x, y, w, h) - *p(in_x, x-1, y, w, h);
                        // backward x
                        float g_yx = x <= 0 ? 0. : *p(in_y, x, y, w, h) - *p(in_y, x-1, y, w, h);
                        // backward y
                        float g_xy = y <= 0 ? 0. : *p(in_x, x, y, w, h) - *p(in_x, x, y-1, w, h);
                        // backward y
                        float g_yy = y <= 0 ? 0. : *p(in_y, x, y, w, h) - *p(in_y, x, y-1, w, h);
                        // norm
                        float g2_norm = sqrt(sqr(g_xx) + sqr(g_yx) + sqr(g_xy) + sqr(g_yy));
                        tv2 += g2_norm;
                        // compute derivatives
                        if(g2_norm != 0.) {
                                *p(objective_gradient, x, y, w, h) += alpha * (-(2. * g_xx + g_xy + g_yx + 2. *  g_yy) / g2_norm);
                                if(x > 0) {
                                        *p(objective_gradient, x-1, y, w, h) += alpha * ((g_yx + g_xx) / g2_norm);
                                }
                                if(x < w-1) {
                                        *p(objective_gradient, x+1, y, w, h) += alpha * ((g_xx + g_xy) / g2_norm);
                                }
                                if(y > 0) {
                                        *p(objective_gradient, x, y-1, w, h) += alpha * ((g_yy + g_xy) / g2_norm);
                                }
                                if(y < h-1) {
                                        *p(objective_gradient, x, y+1, w, h) += alpha * ((g_yy + g_yx) / g2_norm);
                                }
                                if(x < w-1 && y > 0) {
                                        *p(objective_gradient, x+1, y-1, w, h) += alpha * ((-g_xy) / g2_norm);
                                }
                                if(x > 0 && y < h-1) {
                                        *p(objective_gradient, x-1, y+1, w, h) += alpha * ((-g_yx) / g2_norm);
                                }
                        }
                }
        }
        return tv2;
}

static double compute_step(unsigned w, unsigned h, float *in, float *out, float step_size, float weight, float *objective_gradient, float *in_x, float *in_y, struct logger *log) {
        float alpha = weight / sqrt(4. / 2.);
        for(unsigned i = 0; i < h * w; i++) {
                objective_gradient[i] = 0.;
        }

        double tv = compute_step_tv(w, h, in, objective_gradient, in_x, in_y);
#ifdef SIMD_VERIFY
        verify_compute_step_tv(w, h, tv, in, objective_gradient, in_x, in_y);
#endif

        double tv2 = alpha == 0. ? 0. : compute_step_tv2(w, h, objective_gradient, in_x, in_y, alpha);

        float norm = 0.;
        for(unsigned i = 0; i < h * w; i++) {
                norm += sqr(objective_gradient[i]);
        }
        norm = sqrt(norm);

        for(unsigned i = 0; i < h * w; i++) {
                out[i] = in[i] - step_size * (objective_gradient[i] /  norm);
        }

        double objective = (tv + alpha * tv2) / (alpha + 1.);
        logger_log(log, objective, tv, tv2);

        return objective;
}

struct compute_projection_aux {
        float *q_min;
        float *q_max;
        float *temp;
};

static void compute_projection_init(unsigned w, unsigned h, int16_t *data, uint16_t quant_table[64], struct compute_projection_aux *aux) {
        float *q_max = alloc_real(h * w);
        if(!q_max) { die("allocation error"); }
        float *q_min = alloc_real(h * w);
        if(!q_min) { die("allocation error"); }
        unsigned blocks = (h / 8) * (w / 8);

        for(unsigned i = 0; i < blocks; i++) {
                for(unsigned j = 0; j < 64; j++) {
                       q_max[i*64+j] = (data[i*64+j] + 0.5) * quant_table[j];
                       q_min[i*64+j] = (data[i*64+j] - 0.5) * quant_table[j];
                }
        }

        aux->q_min = q_min;
        aux->q_max = q_max;

        float *temp = alloc_real(h * w);
        if(!temp) { die("allocation error"); }

        aux->temp = temp;
}

static void compute_projection_destroy(struct compute_projection_aux *aux) {
        free_real(aux->temp);
        free_real(aux->q_min);
        free_real(aux->q_max);
}

static void compute_projection(unsigned w, unsigned h, float *fdata, struct compute_projection_aux *aux) {
        float *temp = aux->temp;

        unsigned blocks = (h / 8) * (w / 8);

        box(fdata, temp, w, h);

        for(unsigned i = 0; i < blocks; i++) {
                dct8x8s(&temp[i*64]);
        }

        for(unsigned i = 0; i < h * w; i++) {
                temp[i] = CLAMP(temp[i], aux->q_min[i], aux->q_max[i]);
        }

        for(unsigned i = 0; i < blocks; i++) {
                idct8x8s(&temp[i*64]);
        }

        unbox(temp, fdata, w, h);
}

void compute(struct coef *coef, struct logger *log, struct progressbar *pb, uint16_t quant_table[64], float weight, unsigned iterations) {
        unsigned h = coef->h;
        unsigned w = coef->w;
        float *fdata = coef->fdata;
        ASSUME_ALIGNED(fdata);

        struct compute_projection_aux cpa;
        compute_projection_init(w, h, coef->data, quant_table, &cpa);

        float *temp_x = alloc_real(h * w);
        if(!temp_x) { die("allocation error"); }
        float *temp_y = alloc_real(h * w);
        if(!temp_y) { die("allocation error"); }
        float *temp_gradient = alloc_real(h * w);
        if(!temp_gradient) { die("allocation error"); }

        float *temp_fista = alloc_real(h * w);
        if(!temp_fista) { die("allocation error"); }
        memcpy(temp_fista, fdata, sizeof(float) * w * h);

        float radius = sqrt(w*h) / 2;
        for(unsigned i = 0; i < iterations; i++) {
                log->iteration = i;

                float k = i;
                for(unsigned j = 0; j < w * h; j++) {
                        temp_fista[j] = fdata[j] + (k - 2.)/(k+1.) * (fdata[j] - temp_fista[j]);
                }

                compute_step(w, h, temp_fista, temp_fista, radius / sqrt(1 + iterations), weight, temp_x, temp_y, temp_gradient, log);
                compute_projection(w, h, temp_fista, &cpa);

                float *t = fdata;
                fdata = temp_fista;
                temp_fista = t;

                if(pb) {
#ifdef USE_OPENMP
    #pragma omp critical(progressbar)
#endif
                        progressbar_inc(pb);
                }
        }

        coef->fdata = fdata;
        free_real(temp_x);
        free_real(temp_y);
        free_real(temp_gradient);
        free_real(temp_fista);

        compute_projection_destroy(&cpa);
}
