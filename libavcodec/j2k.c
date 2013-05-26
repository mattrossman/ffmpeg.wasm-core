/*
 * JPEG 2000 encoder and decoder common functions
 * Copyright (c) 2007 Kamil Nowosad
 * Copyright (c) 2013 Nicolas Bertrand <nicoinattendu@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * JPEG 2000 image encoder and decoder common functions
 * @file
 * @author Kamil Nowosad
 */

#include "libavutil/common.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "j2k.h"

#define SHL(a, n) ((n) >= 0 ? (a) << (n) : (a) >> -(n))

/* tag tree routines */

/* allocate the memory for tag tree */
static int tag_tree_size(int w, int h)
{
    int res = 0;
    while (w > 1 || h > 1) {
        res += w * h;
        w = (w + 1) >> 1;
        h = (h + 1) >> 1;
    }
    return res + 1;
}

Jpeg2000TgtNode *ff_j2k_tag_tree_init(int w, int h)
{
    int pw = w, ph = h;
    Jpeg2000TgtNode *res, *t, *t2;
    int32_t tt_size;

    tt_size = tag_tree_size(w, h);

    t = res = av_mallocz(tt_size, sizeof(*t));
    if (!res)
        return NULL;

    while (w > 1 || h > 1) {
        int i, j;
        pw = w;
        ph = h;

        w  = (w + 1) >> 1;
        h  = (h + 1) >> 1;
        t2 = t + pw * ph;

        for (i = 0; i < ph; i++)
            for (j = 0; j < pw; j++)
                t[i * pw + j].parent = &t2[(i >> 1) * w + (j >> 1)];

        t = t2;
    }
    t[0].parent = NULL;
    return res;
}

static void tag_tree_zero(Jpeg2000TgtNode *t, int w, int h)
{
    int i, siz = tag_tree_size(w, h);

    for (i = 0; i < siz; i++) {
        t[i].val = 0;
        t[i].vis = 0;
    }
}

static int getsigctxno(int flag, int bandno)
{
    int h, v, d;

    h = ((flag & JPEG2000_T1_SIG_E)  ? 1 : 0) +
        ((flag & JPEG2000_T1_SIG_W)  ? 1 : 0);
    v = ((flag & JPEG2000_T1_SIG_N)  ? 1 : 0) +
        ((flag & JPEG2000_T1_SIG_S)  ? 1 : 0);
    d = ((flag & JPEG2000_T1_SIG_NE) ? 1 : 0) +
        ((flag & JPEG2000_T1_SIG_NW) ? 1 : 0) +
        ((flag & JPEG2000_T1_SIG_SE) ? 1 : 0) +
        ((flag & JPEG2000_T1_SIG_SW) ? 1 : 0);

    if (bandno < 3) {
        if (bandno == 1)
            FFSWAP(int, h, v);
        if (h == 2) return 8;
        if (h == 1) {
            if (v >= 1) return 7;
            if (d >= 1) return 6;
            return 5;
        }
        if (v == 2) return 4;
        if (v == 1) return 3;
        if (d >= 2) return 2;
        if (d == 1) return 1;
    } else{
        if (d >= 3) return 8;
        if (d == 2) {
            if (h+v >= 1) return 7;
            return 6;
        }
        if (d == 1) {
            if (h+v >= 2) return 5;
            if (h+v == 1) return 4;
            return 3;
        }
        if (h+v >= 2) return 2;
        if (h+v == 1) return 1;
    }
    return 0;
}


static const int contribtab[3][3] = { {  0, -1,  1 }, { -1, -1,  0 }, {  1,  0,  1 } };
static const int  ctxlbltab[3][3] = { { 13, 12, 11 }, { 10,  9, 10 }, { 11, 12, 13 } };
static const int  xorbittab[3][3] = { {  1,  1,  1 }, {  1,  0,  0 }, {  0,  0,  0 } };

static int getsgnctxno(int flag, uint8_t *xorbit)
{
    int vcontrib, hcontrib;

    hcontrib = contribtab[flag & JPEG2000_T1_SIG_E ? flag & JPEG2000_T1_SGN_E ? 1 : 2 : 0]
                         [flag & JPEG2000_T1_SIG_W ? flag & JPEG2000_T1_SGN_W ? 1 : 2 : 0] + 1;
    vcontrib = contribtab[flag & JPEG2000_T1_SIG_S ? flag & JPEG2000_T1_SGN_S ? 1 : 2 : 0]
                         [flag & JPEG2000_T1_SIG_N ? flag & JPEG2000_T1_SGN_N ? 1 : 2 : 0] + 1;
    *xorbit = xorbittab[hcontrib][vcontrib];

    return ctxlbltab[hcontrib][vcontrib];
}

void ff_j2k_set_significant(Jpeg2000T1Context *t1, int x, int y,
                            int negative)
{
    x++;
    y++;
    t1->flags[y][x] |= JPEG2000_T1_SIG;
    if (negative) {
        t1->flags[y][x + 1] |= JPEG2000_T1_SIG_W | JPEG2000_T1_SGN_W;
        t1->flags[y][x - 1] |= JPEG2000_T1_SIG_E | JPEG2000_T1_SGN_E;
        t1->flags[y + 1][x] |= JPEG2000_T1_SIG_N | JPEG2000_T1_SGN_N;
        t1->flags[y - 1][x] |= JPEG2000_T1_SIG_S | JPEG2000_T1_SGN_S;
    } else {
        t1->flags[y][x + 1] |= JPEG2000_T1_SIG_W;
        t1->flags[y][x - 1] |= JPEG2000_T1_SIG_E;
        t1->flags[y + 1][x] |= JPEG2000_T1_SIG_N;
        t1->flags[y - 1][x] |= JPEG2000_T1_SIG_S;
    }
    t1->flags[y + 1][x + 1] |= JPEG2000_T1_SIG_NW;
    t1->flags[y + 1][x - 1] |= JPEG2000_T1_SIG_NE;
    t1->flags[y - 1][x + 1] |= JPEG2000_T1_SIG_SW;
    t1->flags[y - 1][x - 1] |= JPEG2000_T1_SIG_SE;
}

int ff_j2k_init_component(Jpeg2000Component *comp, Jpeg2000CodingStyle *codsty, Jpeg2000QuantStyle *qntsty, int cbps, int dx, int dy)
{
    int reslevelno, bandno, gbandno = 0, ret, i, j, csize = 1;

    if (ret=ff_j2k_dwt_init(&comp->dwt, comp->coord, codsty->nreslevels-1, codsty->transform))
        return ret;
    for (i = 0; i < 2; i++)
        csize *= comp->coord[i][1] - comp->coord[i][0];

    comp->data = av_malloc(csize * sizeof(int));
    if (!comp->data)
        return AVERROR(ENOMEM);
    comp->reslevel = av_malloc(codsty->nreslevels * sizeof(Jpeg2000ResLevel));

    if (!comp->reslevel)
        return AVERROR(ENOMEM);
    for (reslevelno = 0; reslevelno < codsty->nreslevels; reslevelno++) {
        int declvl = codsty->nreslevels - reslevelno;
        Jpeg2000ResLevel *reslevel = comp->reslevel + reslevelno;

        for (i = 0; i < 2; i++)
            for (j = 0; j < 2; j++)
                reslevel->coord[i][j] =
                    ff_jpeg2000_ceildivpow2(comp->coord[i][j], declvl - 1);

        if (reslevelno == 0)
            reslevel->nbands = 1;
        else
            reslevel->nbands = 3;

        if (reslevel->coord[0][1] == reslevel->coord[0][0])
            reslevel->num_precincts_x = 0;
        else
            reslevel->num_precincts_x = ff_jpeg2000_ceildivpow2(reslevel->coord[0][1], codsty->log2_prec_width)
                                        - (reslevel->coord[0][0] >> codsty->log2_prec_width);

        if (reslevel->coord[1][1] == reslevel->coord[1][0])
            reslevel->num_precincts_y = 0;
        else
            reslevel->num_precincts_y = ff_jpeg2000_ceildivpow2(reslevel->coord[1][1], codsty->log2_prec_height)
                                        - (reslevel->coord[1][0] >> codsty->log2_prec_height);

        reslevel->band = av_malloc(reslevel->nbands * sizeof(Jpeg2000Band));
        if (!reslevel->band)
            return AVERROR(ENOMEM);
        for (bandno = 0; bandno < reslevel->nbands; bandno++, gbandno++) {
            Jpeg2000Band *band = reslevel->band + bandno;
            int cblkno, precx, precy, precno;
            int x0, y0, x1, y1;
            int xi0, yi0, xi1, yi1;
            int cblkperprecw, cblkperprech;

            if (qntsty->quantsty != JPEG2000_QSTY_NONE) {
                static const uint8_t lut_gain[2][4] = {{0, 0, 0, 0}, {0, 1, 1, 2}};
                int numbps;

                numbps = cbps + lut_gain[codsty->transform][bandno + reslevelno>0];
                band->stepsize = SHL(2048 + qntsty->mant[gbandno], 2 + numbps - qntsty->expn[gbandno]);
            } else
                band->stepsize = 1 << 13;

            if (reslevelno == 0) {  // the same everywhere
                band->codeblock_width = 1 << FFMIN(codsty->log2_cblk_width, codsty->log2_prec_width-1);
                band->codeblock_height = 1 << FFMIN(codsty->log2_cblk_height, codsty->log2_prec_height-1);
                for (i = 0; i < 2; i++)
                    for (j = 0; j < 2; j++)
                        band->coord[i][j] = ff_jpeg2000_ceildivpow2(comp->coord[i][j], declvl-1);
            } else{
                band->codeblock_width = 1 << FFMIN(codsty->log2_cblk_width, codsty->log2_prec_width);
                band->codeblock_height = 1 << FFMIN(codsty->log2_cblk_height, codsty->log2_prec_height);

                for (i = 0; i < 2; i++)
                    for (j = 0; j < 2; j++)
                        band->coord[i][j] = ff_jpeg2000_ceildivpow2(comp->coord[i][j] - (((bandno+1>>i)&1) << declvl-1), declvl);
            }
            band->cblknx = ff_jpeg2000_ceildiv(band->coord[0][1], band->codeblock_width)  - band->coord[0][0] / band->codeblock_width;
            band->cblkny = ff_jpeg2000_ceildiv(band->coord[1][1], band->codeblock_height) - band->coord[1][0] / band->codeblock_height;

            for (j = 0; j < 2; j++)
                band->coord[0][j] = ff_jpeg2000_ceildiv(band->coord[0][j], dx);
            for (j = 0; j < 2; j++)
                band->coord[1][j] = ff_jpeg2000_ceildiv(band->coord[1][j], dy);

            band->cblknx = ff_jpeg2000_ceildiv(band->cblknx, dx);
            band->cblkny = ff_jpeg2000_ceildiv(band->cblkny, dy);

            band->cblk = av_malloc(sizeof(Jpeg2000Cblk) * band->cblknx * band->cblkny);
            if (!band->cblk)
                return AVERROR(ENOMEM);
            band->prec = av_malloc(sizeof(Jpeg2000Cblk) * reslevel->num_precincts_x * reslevel->num_precincts_y);
            if (!band->prec)
                return AVERROR(ENOMEM);

            for (cblkno = 0; cblkno < band->cblknx * band->cblkny; cblkno++) {
                Jpeg2000Cblk *cblk = band->cblk + cblkno;
                cblk->zero = 0;
                cblk->lblock = 3;
                cblk->length = 0;
                cblk->lengthinc = 0;
                cblk->npasses = 0;
            }

            y0 = band->coord[1][0];
            y1 = ((band->coord[1][0] + (1<<codsty->log2_prec_height)) & ~((1<<codsty->log2_prec_height)-1)) - y0;
            yi0 = 0;
            yi1 = ff_jpeg2000_ceildivpow2(y1 - y0, codsty->log2_cblk_height) << codsty->log2_cblk_height;
            yi1 = FFMIN(yi1, band->cblkny);
            cblkperprech = 1<<(codsty->log2_prec_height - codsty->log2_cblk_height);
            for (precy = 0, precno = 0; precy < reslevel->num_precincts_y; precy++) {
                for (precx = 0; precx < reslevel->num_precincts_x; precx++, precno++) {
                    band->prec[precno].yi0 = yi0;
                    band->prec[precno].yi1 = yi1;
                }
                yi1 += cblkperprech;
                yi0 = yi1 - cblkperprech;
                yi1 = FFMIN(yi1, band->cblkny);
            }
            x0 = band->coord[0][0];
            x1 = ((band->coord[0][0] + (1<<codsty->log2_prec_width)) & ~((1<<codsty->log2_prec_width)-1)) - x0;
            xi0 = 0;
            xi1 = ff_jpeg2000_ceildivpow2(x1 - x0, codsty->log2_cblk_width) << codsty->log2_cblk_width;
            xi1 = FFMIN(xi1, band->cblknx);

            cblkperprecw = 1<<(codsty->log2_prec_width - codsty->log2_cblk_width);
            for (precx = 0, precno = 0; precx < reslevel->num_precincts_x; precx++) {
                for (precy = 0; precy < reslevel->num_precincts_y; precy++, precno = 0) {
                    Jpeg2000Prec *prec = band->prec + precno;
                    prec->xi0 = xi0;
                    prec->xi1 = xi1;
                    prec->cblkincl = ff_j2k_tag_tree_init(prec->xi1 - prec->xi0,
                                                          prec->yi1 - prec->yi0);
                    prec->zerobits = ff_j2k_tag_tree_init(prec->xi1 - prec->xi0,
                                                          prec->yi1 - prec->yi0);
                    if (!prec->cblkincl || !prec->zerobits)
                        return AVERROR(ENOMEM);

                }
                xi1 += cblkperprecw;
                xi0 = xi1 - cblkperprecw;
                xi1 = FFMIN(xi1, band->cblknx);
            }
        }
    }
    return 0;
}

void ff_j2k_reinit(Jpeg2000Component *comp, Jpeg2000CodingStyle *codsty)
{
    int reslevelno, bandno, cblkno, precno;
    for (reslevelno = 0; reslevelno < codsty->nreslevels; reslevelno++) {
        Jpeg2000ResLevel *rlevel = comp->reslevel + reslevelno;
        for (bandno = 0; bandno < rlevel->nbands; bandno++) {
            Jpeg2000Band *band = rlevel->band + bandno;
            for(precno = 0; precno < rlevel->num_precincts_x * rlevel->num_precincts_y; precno++) {
                Jpeg2000Prec *prec = band->prec + precno;
                tag_tree_zero(prec->zerobits, prec->xi1 - prec->xi0, prec->yi1 - prec->yi0);
                tag_tree_zero(prec->cblkincl, prec->xi1 - prec->xi0, prec->yi1 - prec->yi0);
            }
            for (cblkno = 0; cblkno < band->cblknx * band->cblkny; cblkno++) {
                Jpeg2000Cblk *cblk = band->cblk + cblkno;
                cblk->length = 0;
                cblk->lblock = 3;
            }
        }
    }
}

void ff_j2k_cleanup(Jpeg2000Component *comp, Jpeg2000CodingStyle *codsty)
{
    int reslevelno, bandno, precno;
    for (reslevelno = 0; reslevelno < codsty->nreslevels; reslevelno++) {
        Jpeg2000ResLevel *reslevel = comp->reslevel + reslevelno;

        for (bandno = 0; bandno < reslevel->nbands ; bandno++) {
            Jpeg2000Band *band = reslevel->band + bandno;
                for (precno = 0; precno < reslevel->num_precincts_x * reslevel->num_precincts_y; precno++) {
                    Jpeg2000Prec *prec = band->prec + precno;
                    av_freep(&prec->zerobits);
                    av_freep(&prec->cblkincl);
                }
                av_freep(&band->cblk);
                av_freep(&band->prec);
            }
        av_freep(&reslevel->band);
    }

    ff_j2k_dwt_destroy(&comp->dwt);
    av_freep(&comp->reslevel);
    av_freep(&comp->data);
}
