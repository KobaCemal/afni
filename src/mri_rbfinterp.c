#include "mrilib.h"

#ifdef USE_OMP
#include <omp.h>
#endif

#undef DEBUG

/******************************************************************************
  Radial Basis Function (RBF) interpolation in 3D.
  + These functions are not designed for high efficiency!
  + They are designed for repeated interpolations from the
    same set of knots, using different sets of values each time.
  + The C2 polynomial with compact support (0 < r < 1)
    f(r) = (1-r)^4*(4r+1) is the RBF used here, with a global linear
    polynomial.
  + Setup for a given collection of knots:        RBF_setup_knots()
  + Setup for a given set of values at the knots: RBF_setup_evalues()
  + Evaluation at an arbitrary set of points:     RBF_evaluate()
*******************************************************************************/

#if 0         /* would be needed for thin-plate spline RBF */
#undef  Rlog
#define Rlog(x)       (((x)<=0.0f) ? 0.0f : logf(x))
#endif

/*! C2 polynomial: argument is 1-r, which should be between 0 and 1 */

#undef  RBF_func
#define RBF_func(x)    ((x)*(x)*(x)*(x)*(5.0f-4.0f*x))

/*----------------------------------------------------------------------------*/
static int verb = 0 ;
void RBF_set_verbosity( int ii ){ verb = ii ; }

/*----------------------------------------------------------------------------*/
/*! Evaluate RBF expansion on a set of points in rbg,
    given the set of knots in rbk, and given the coefficients of the RBF
    (or the values at the fit points) in rbe, and store results in array val
    -- which should be rbg->npt points long.
*//*--------------------------------------------------------------------------*/

int RBF_evaluate( RBF_knots *rbk, RBF_evalues *rbe,
                  RBF_evalgrid *rbg, float *val    )
{
   int npt , nk ;
   double ct ;

ENTRY("RBF_evaluate") ;

   if( rbk == NULL || rbe == NULL || rbg == NULL || val == NULL ){
     ERROR_message("Illegal call to RBF_evaluate?!") ;
     RETURN(0) ;
   }

   /* if needed, convert rbe to RBF weights, from function values */

   npt = RBF_setup_evalues( rbk , rbe ) ;
   if( npt == 0 ){
     ERROR_message("bad evalues input to RBF_evaluate") ; RETURN(0) ;
   }

   /* evaluate RBF + linear polynomial at each output point */

   npt = rbg->npt ;
   nk  = rbk->nknot ;

   if( verb )
     INFO_message("RBF_evaluate: %d points X %d knots",npt,nk) ;

   ct = COX_clock_time() ;

#pragma omp parallel if(npt*nk > 9999)
 {
   int ii , jj , uselin=rbk->uselin ;
   float  rr , xm,ym,zm , xd,yd,zd , rad,rqq , xt,yt,zt , *xx,*yy,*zz ;
   float  xk , yk , zk , sum , *ev ;
   float b0,bx,by,bz , rai ;
   RBFKINT *kfirst , *klast ; int kbot,ktop ;

   /* load some local variables */

   rad = rbk->rad  ; rqq = rbk->rqq  ; rai = 1.0f / rad ;
   xm  = rbk->xmid ; ym  = rbk->ymid ; zm  = rbk->zmid  ;
   xd  = rbk->xscl ; yd  = rbk->yscl ; zd  = rbk->zscl  ;
   xx  = rbk->xknot; yy  = rbk->yknot; zz  = rbk->zknot ;
   ev  = rbe->val  ;
   if( uselin ){
     b0 = rbe->b0 ; bx = rbe->bx ; by = rbe->by ; bz = rbe->bz ;
   }

   kfirst = rbg->kfirst ; klast = rbg->klast ; kbot = 0 ; ktop = nk-1 ;

#pragma omp for
   for( ii=0 ; ii < npt ; ii++ ){
     xt = rbg->xpt[ii]; yt = rbg->ypt[ii]; zt = rbg->zpt[ii]; /* output xyz */
     if( kfirst != NULL ){ kbot = kfirst[ii] ; ktop = klast[ii] ; }
     for( sum=0.0f,jj=kbot ; jj <= ktop ; jj++ ){
       xk = xt-xx[jj]; rr =xk*xk; if( rr >= rqq ) continue;
       yk = yt-yy[jj]; rr+=yk*yk; if( rr >= rqq ) continue;
       zk = zt-zz[jj]; rr+=zk*zk; if( rr >= rqq ) continue;
       rr = 1.0f - sqrtf(rr) * rai ; sum += ev[jj] * RBF_func(rr) ;
     }
     val[ii] = sum ;
     if( uselin )
       val[ii] += b0 + bx*(xt-xm)*xd + by*(yt-ym)*yd + bz*(zt-zm)*zd ;
   }
 } /* end OpenMP */

   if( verb ) ININFO_message("              Elapsed = %.1f",COX_clock_time()-ct) ;
   RETURN(1) ;
}

/*----------------------------------------------------------------------------*/
/*! Setup the particular values from which to interpolate RBF-wise;
    that is, convert them from values at each fit point to the weights
    used for the RBF centered at each knot (plus the linear polynomial).
    Return value is 0 if bad things happened, 1 if good things happened.
*//*--------------------------------------------------------------------------*/

int RBF_setup_evalues( RBF_knots *rbk , RBF_evalues *rbe )
{
   int nn , ii , jj ;
   float *mat , *vv ; double *aa ;

ENTRY("RBF_setup_evalues") ;

   if( rbk == NULL || rbe == NULL || rbe->val == NULL ){
     ERROR_message("bad call to RBF_setup_evalues") ; RETURN(0) ;
   }

   if( rbe->code > 0 ) RETURN(1) ;  /* already contains RBF weights */

   if( verb )
     INFO_message("RBF_setup_evalues: Lmat solve") ;

   nn = rbk->nknot ;
   vv = rbe->val ;
   aa = (double *)calloc(sizeof(double),nn) ;
   for( ii=0 ; ii < nn ; ii++ ) aa[ii] = (double)vv[ii] ;

   rcmat_lowert_solve( rbk->Lmat , aa ) ;
   rcmat_uppert_solve( rbk->Lmat , aa ) ;

   if( rbk->uselin ){
     double q0,qx,qy,qz , b0,bx,by,bz ; dmat44 Q=rbk->Qmat ;
     float *P0=rbk->P0 , *Px=rbk->Px , *Py=rbk->Py , *Pz=rbk->Pz ;

   if( verb )
     ININFO_message("                   linear trend solve") ;

     for( q0=qx=qy=qz=0.0,ii=0 ; ii < nn ; ii++ ){
       q0 += P0[ii]*aa[ii] ; qx += Px[ii]*aa[ii] ;
       qy += Py[ii]*aa[ii] ; qz += Pz[ii]*aa[ii] ;
     }
     DMAT44_VEC( Q , q0,qx,qy,qz , b0,bx,by,bz ) ;
     rbe->b0 = b0 ; rbe->bx = bx ; rbe->by = by ; rbe->bz = bz ;
     for( ii=0 ; ii < nn ; ii++ )
       aa[ii] = (double)vv[ii] - b0*P0[ii] - bx*Px[ii] - by*Py[ii] - bz*Pz[ii] ;

     rcmat_lowert_solve( rbk->Lmat , aa ) ;
     rcmat_uppert_solve( rbk->Lmat , aa ) ;
#ifdef DEBUG
  INFO_message("RBF_setup_evalues: b0=%g bx=%g by=%g bz=%g",b0,bx,by,bz) ;
#endif
   }

   /* put results back into rbe */

   for( ii=0 ; ii < nn ; ii++ ) vv[ii] = (float)aa[ii] ;
   rbe->code = 1 ; /* code that rbe is converted to RBF weights from values */
   free(aa) ;

   RETURN(2) ;
}

/*----------------------------------------------------------------------------*/
/*! Setup the knots struct for radial basis function evaluation.
*//*--------------------------------------------------------------------------*/

RBF_knots * RBF_setup_knots( int nk, float radius, int uselin,
                             float *xx , float *yy , float *zz )
{
   RBF_knots *rbk ;
   int ii , jj , nn , jtop ;
   float **mat ;
   rcmat *rcm ;
   float *P0=NULL,*Px=NULL,*Py=NULL,*Pz=NULL ;
   float rr , xm,ym,zm , xd,yd,zd , rad,rqq , xt,yt,zt ;
   double ct ;

ENTRY("RBF_setup_knots") ;

   if( nk < 5 || xx == NULL || yy == NULL || zz == NULL ){
     ERROR_message("Illegal inputs to RBF_setup_knots") ; RETURN(NULL) ;
   }

   ct = COX_clock_time() ;

   /* set up middle of knot field and scale radius */
   /* xm = middle (median) of the x knot coordinates
      xd = scale (MAD) of the x knot distances from xm;
           would be about L/4 if knots are uniformly spaced
           over a distance of L
      4*xd/nk is about inter-knot x distance in uniform case
      4*(xd+yd+zd)/(3*nk) is about average inter-knot distance in 3D
      the RBF support radius is chosen to be a small multiple of this distance */

   qmedmad_float( nk , xx , &xm , &xd ) ;
   qmedmad_float( nk , yy , &ym , &yd ) ;
   qmedmad_float( nk , zz , &zm , &zd ) ;

   if( radius <= 0.0f )
     rad = 4.321f*(xd+yd+zd) / cbrtf((float)nk) ;  /* RBF support radius */
   else
     rad = radius ;

   rqq = rad*rad ;  /* for testing */

   if( verb )
     INFO_message("RBF_setup_knots: knots=%d radius=%.1f",nk,rad) ;

   xd = 1.0f / xd ;          /* scale factors : (x-xm)*xd is   */
   yd = 1.0f / yd ;          /* dimensionless x-value relative */
   zd = 1.0f / zd ;          /* to middle at xm                */

   /* set up matrix for interpolation */

   nn = nk ;
   mat = (float **)malloc(sizeof(float *)*nn) ;
   for( ii=0 ; ii < nn ; ii++ )
     mat[ii] = (float *)calloc((ii+1),sizeof(float)) ;

   if( uselin ){
     P0 = (float *)calloc(nn,sizeof(float)) ;
     Px = (float *)calloc(nn,sizeof(float)) ;
     Py = (float *)calloc(nn,sizeof(float)) ;
     Pz = (float *)calloc(nn,sizeof(float)) ;
   }

   if( verb ) ININFO_message("                 matrix computation") ;

   for( ii=0 ; ii < nn ; ii++ ){
     for( jj=0 ; jj < ii ; jj++ ){    /* RBF between knots */
       xt = xx[ii]-xx[jj] ;
       yt = yy[ii]-yy[jj] ;
       zt = zz[ii]-zz[jj] ; rr = xt*xt + yt*yt + zt*zt ;
       if( rr >= rqq ) continue ;
       rr = 1.0f - sqrtf(rr)/rad ; mat[ii][jj] = RBF_func(rr) ;
     }
     mat[ii][ii] = 1.0000005f ;  /* RBF(0) = 1 by definition */
     if( uselin ){
       P0[ii] = 1.0f ;
       Px[ii] = (xx[ii]-xm)*xd ;
       Py[ii] = (yy[ii]-ym)*yd ;
       Pz[ii] = (zz[ii]-zm)*zd ;
     }
   }

   if( verb ) ININFO_message("                 matrix factorization") ;

   rcm = rcmat_from_rows( nn , mat ) ;

   if( rcm == NULL ){
     ERROR_message("RBF_setup_knots: setup of rcmat fails!?") ;
     for( ii=0 ; ii < nn ; ii++ ) free(mat[ii]) ;
     free(mat) ;
     if( uselin ){ free(P0); free(Px); free(Py); free(Pz); }
     RETURN(NULL) ;
   }

   ii = rcmat_choleski( rcm ) ;
   if( ii > 0 ){
     ERROR_message("RBF_setup_knots: Choleski of rcmat fails at row %d!",ii) ;
     for( ii=0 ; ii < nn ; ii++ ) free(mat[ii]) ;
     free(mat) ;
     if( uselin ){ free(P0); free(Px); free(Py); free(Pz); }
     RETURN(NULL) ;
   }

   for( ii=0 ; ii < nn ; ii++ ) free(mat[ii]) ;
   free(mat) ;

   rbk = (RBF_knots *)calloc(1,sizeof(RBF_knots)) ;
   rbk->nknot = nk  ;
   rbk->rad   = rad ; rbk->rqq  = rqq ;
   rbk->xmid  = xm  ; rbk->xscl = xd  ;
   rbk->ymid  = ym  ; rbk->yscl = yd  ;
   rbk->zmid  = zm  ; rbk->zscl = zd  ;
   rbk->xknot = (float *)calloc(sizeof(float),nk) ;
   rbk->yknot = (float *)calloc(sizeof(float),nk) ;
   rbk->zknot = (float *)calloc(sizeof(float),nk) ;
   memcpy(rbk->xknot,xx,sizeof(float)*nk) ;
   memcpy(rbk->yknot,yy,sizeof(float)*nk) ;
   memcpy(rbk->zknot,zz,sizeof(float)*nk) ;

   rbk->Lmat = rcm ;
   rbk->uselin = uselin ;
   rbk->P0 = P0 ; rbk->Px = Px ; rbk->Py = Py ; rbk->Pz = Pz ;

   if( uselin ){
     double *vv[4],*vi,*vj ; dmat44 Q ; double sum ; register int kk ;
#ifdef DEBUG
  INFO_message("     compute Q matrix") ;
#endif
     vv[0] = (double *)malloc(sizeof(double)*nn) ;
     vv[1] = (double *)malloc(sizeof(double)*nn) ;
     vv[2] = (double *)malloc(sizeof(double)*nn) ;
     vv[3] = (double *)malloc(sizeof(double)*nn) ;
     for( ii=0 ; ii < nn ; ii++ ){
       vv[0][ii] = P0[ii] ; vv[1][ii] = Px[ii] ;
       vv[2][ii] = Py[ii] ; vv[3][ii] = Pz[ii] ;
     }
     rcmat_lowert_solve(rcm,vv[0]) ; rcmat_lowert_solve(rcm,vv[1]) ;
     rcmat_lowert_solve(rcm,vv[2]) ; rcmat_lowert_solve(rcm,vv[3]) ;
     for( ii=0 ; ii < 4 ; ii++ ){
       vi = vv[ii] ;
       for( jj=0 ; jj < 4 ; jj++ ){
         vj = vv[jj] ;
         for( sum=0.0,kk=0 ; kk < nn ; kk++ ) sum += vi[kk]*vj[kk] ;
         Q.m[ii][jj] = sum ;
       }
     }
     free(vv[0]) ; free(vv[1]) ; free(vv[2]) ; free(vv[3]) ;
     rbk->Qmat = generic_dmat44_inverse(Q) ;
#ifdef DEBUG
  DUMP_DMAT44("Q",Q) ;
  DUMP_DMAT44("Qinv",rbk->Qmat) ;
#endif
   }

   if( verb ) ININFO_message("                 Elapsed = %.1f",COX_clock_time()-ct) ;

   RETURN(rbk) ;
}

/*------------------------------------------------------------------*/

#if 0
#undef  ADDTO_intar
#define ADDTO_intar(nar,nal,ar,val)                                         \
 do{ if( (nar) == (nal) ){                                                  \
       (nal) = 1.2*(nal)+16; (ar) = (int *)realloc((ar),sizeof(int)*(nal)); \
     }                                                                      \
     (ar)[(nar)++] = (val);                                                 \
 } while(0)

#undef  CLIP_intar
#define CLIP_intar(nar,nal,ar)                                       \
 do{ if( (nar) < (nal) && (nar) > 0 ){                               \
       (nal) = (nar); (ar) = (int *)realloc((ar),sizeof(int)*(nal)); \
 }} while(0)
#endif

/*------------------------------------------------------------------*/

void RBF_setup_kranges( RBF_knots *rbk , RBF_evalgrid *rbg )
{
   int npt , nk ;
   double ct ;

ENTRY("RBF_setup_kranges") ;

   if( rbk == NULL || rbg == NULL || rbk->nknot > 65535 ) EXRETURN ;

   if( rbg->klast != NULL ) free(rbg->klast)  ;
   if( rbg->kfirst!= NULL ) free(rbg->kfirst) ;

   /* load some local variables */

   npt = rbg->npt ;
   nk  = rbk->nknot ;

   rbg->kfirst = (RBFKINT *)calloc(sizeof(RBFKINT),npt) ;
   rbg->klast  = (RBFKINT *)calloc(sizeof(RBFKINT),npt) ;

   if( verb ){
     INFO_message("RBF_setup_kranges: %d grid points",npt) ;
   }

   ct = COX_clock_time() ;

#pragma omp parallel if(npt*nk > 9999)
 {
   int ii,jj , kbot,ktop ;
   float xt,yt,zt, rqq, xk,yk,zk, rr, *xx,*yy,*zz ;
   RBFKINT *klast , *kfirst ;

   rqq = rbk->rqq ; xx = rbk->xknot; yy = rbk->yknot; zz = rbk->zknot;
   kfirst = rbg->kfirst ; klast  = rbg->klast  ;
#pragma omp for
   for( ii=0 ; ii < npt ; ii++ ){
     xt = rbg->xpt[ii]; yt = rbg->ypt[ii]; zt = rbg->zpt[ii];
     kbot = ktop = -1 ;
     for( jj=0 ; jj < nk ; jj++ ){
       xk = xt-xx[jj]; rr =xk*xk; if( rr >= rqq ) continue;
       yk = yt-yy[jj]; rr+=yk*yk; if( rr >= rqq ) continue;
       zk = zt-zz[jj]; rr+=zk*zk; if( rr >= rqq ) continue;
       ktop = jj ;
       if( kbot < 0 ) kbot = jj ;
     }
     if( kbot >= 0 ){
       kfirst[ii] = (RBFKINT)kbot ;
       klast [ii] = (RBFKINT)ktop ;
     }
   }
 } /* end OpenMP */

   if( verb ){
     float ntot=0.0f ; int ii ;
     for( ii=0 ; ii < npt ; ii++ ) ntot += (1.0f+rbg->klast[ii]-rbg->kfirst[ii]) ;
     ININFO_message("                   average krange = %.1f  Elapsed = %.1f",
                    ntot/npt , COX_clock_time()-ct ) ;
   }

   EXRETURN ;
}
