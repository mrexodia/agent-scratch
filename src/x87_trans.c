/* SPDX-License-Identifier: MIT */
#include "x87_internal.h"
x87_f80 x87i_fsin(fctx *c, const unp *a, int *c2){(void)c;(void)a;*c2=0;return F80(0,0);}
x87_f80 x87i_fcos(fctx *c, const unp *a, int *c2){(void)c;(void)a;*c2=0;return F80(0,0);}
void    x87i_fsincos(fctx *c, const unp *a, x87_f80 *s, x87_f80 *co, int *c2){(void)c;(void)a;*c2=0;*s=*co=F80(0,0);}
x87_f80 x87i_fptan(fctx *c, const unp *a, int *c2){(void)c;(void)a;*c2=0;return F80(0,0);}
x87_f80 x87i_fpatan(fctx *c, const unp *y, const unp *x){(void)c;(void)x;(void)y;return F80(0,0);}
x87_f80 x87i_f2xm1(fctx *c, const unp *a){(void)c;(void)a;return F80(0,0);}
x87_f80 x87i_fyl2x(fctx *c, const unp *y, const unp *x){(void)c;(void)x;(void)y;return F80(0,0);}
x87_f80 x87i_fyl2xp1(fctx *c, const unp *y, const unp *x){(void)c;(void)x;(void)y;return F80(0,0);}
