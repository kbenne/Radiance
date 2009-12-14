#ifndef lint
static const char	RCSid[] = "$Id$";
#endif
/*
 *  rtrace.c - program and variables for individual ray tracing.
 */

#include "copyright.h"

/*
 *  Input is in the form:
 *
 *	xorg	yorg	zorg	xdir	ydir	zdir
 *
 *  The direction need not be normalized.  Output is flexible.
 *  If the direction vector is (0,0,0), then the output is flushed.
 *  All values default to ascii representation of real
 *  numbers.  Binary representations can be selected
 *  with '-ff' for float or '-fd' for double.  By default,
 *  radiance is computed.  The '-i' or '-I' options indicate that
 *  irradiance values are desired.
 */

#include  <time.h>

#include  "platform.h"
#include  "ray.h"
#include  "ambient.h"
#include  "source.h"
#include  "otypes.h"
#include  "resolu.h"
#include  "random.h"

extern int  inform;			/* input format */
extern int  outform;			/* output format */
extern char  *outvals;			/* output values */

extern int  imm_irrad;			/* compute immediate irradiance? */
extern int  lim_dist;			/* limit distance? */

extern char  *tralist[];		/* list of modifers to trace (or no) */
extern int  traincl;			/* include == 1, exclude == 0 */

extern int  hresolu;			/* horizontal resolution */
extern int  vresolu;			/* vertical resolution */

static int  castonly = 0;

#ifndef  MAXTSET
#define	 MAXTSET	8191		/* maximum number in trace set */
#endif
OBJECT	traset[MAXTSET+1]={0};		/* trace include/exclude set */

static RAY  thisray;			/* for our convenience */

typedef void putf_t(double v);
static putf_t puta, putd, putf;

typedef void oputf_t(RAY *r);
static oputf_t  oputo, oputd, oputv, oputV, oputl, oputL, oputc, oputp,
		oputn, oputN, oputs, oputw, oputW, oputm, oputM, oputtilde;

static void setoutput(char *vs);
extern void tranotify(OBJECT obj);
static void bogusray(void);
static void raycast(RAY *r);
static void rayirrad(RAY *r);
static void rtcompute(FVECT org, FVECT dir, double dmax);
static int printvals(RAY *r);
static int getvec(FVECT vec, int fmt, FILE *fp);
static void tabin(RAY *r);
static void ourtrace(RAY *r);

static oputf_t *ray_out[16], *every_out[16];
static putf_t *putreal;


void
quit(			/* quit program */
	int  code
)
{
	if (ray_pnprocs > 0)	/* close children if any */
		ray_pclose(0);		
#ifndef  NON_POSIX
	else if (!ray_pnprocs) {
		headclean();	/* delete header file */
		pfclean();	/* clean up persist files */
	}
#endif
	exit(code);
}


char *
formstr(				/* return format identifier */
	int  f
)
{
	switch (f) {
	case 'a': return("ascii");
	case 'f': return("float");
	case 'd': return("double");
	case 'c': return(COLRFMT);
	}
	return("unknown");
}


extern void
rtrace(				/* trace rays from file */
	char  *fname,
	int  nproc
)
{
	unsigned long  vcount = (hresolu > 1) ? (unsigned long)hresolu*vresolu
					      : vresolu;
	long  nextflush = hresolu;
	FILE  *fp;
	double	d;
	FVECT  orig, direc;
					/* set up input */
	if (fname == NULL)
		fp = stdin;
	else if ((fp = fopen(fname, "r")) == NULL) {
		sprintf(errmsg, "cannot open input file \"%s\"", fname);
		error(SYSTEM, errmsg);
	}
	if (inform != 'a')
		SET_FILE_BINARY(fp);
					/* set up output */
	setoutput(outvals);
	if (imm_irrad)
		castonly = 0;
	else if (castonly)
		nproc = 1;		/* don't bother multiprocessing */
	switch (outform) {
	case 'a': putreal = puta; break;
	case 'f': putreal = putf; break;
	case 'd': putreal = putd; break;
	case 'c': 
		if (strcmp(outvals, "v"))
			error(USER, "color format with value output only");
		break;
	default:
		error(CONSISTENCY, "botched output format");
	}
	if (nproc > 1) {		/* start multiprocessing */
		ray_popen(nproc);
		ray_fifo_out = printvals;
	}
	if (hresolu > 0) {
		if (vresolu > 0)
			fprtresolu(hresolu, vresolu, stdout);
		fflush(stdout);
	}
					/* process file */
	while (getvec(orig, inform, fp) == 0 &&
			getvec(direc, inform, fp) == 0) {

		d = normalize(direc);
		if (d == 0.0) {				/* zero ==> flush */
			if (nproc > 1 && ray_fifo_flush() < 0)
				error(USER, "lost children");
			bogusray();
			if (--nextflush <= 0 || !vcount) {
				fflush(stdout);
				nextflush = hresolu;
			}
		} else {				/* compute and print */
			rtcompute(orig, direc, lim_dist ? d : 0.0);
							/* flush if time */
			if (!--nextflush) {
				if (nproc > 1 && ray_fifo_flush() < 0)
					error(USER, "lost children");
				fflush(stdout);
				nextflush = hresolu;
			}
		}
		if (ferror(stdout))
			error(SYSTEM, "write error");
		if (vcount && !--vcount)		/* check for end */
			break;
	}
	if (nproc > 1) {				/* clean up children */
		if (ray_fifo_flush() < 0)
			error(USER, "unable to complete processing");
		ray_pclose(0);
	}
	if (fflush(stdout) < 0)
		error(SYSTEM, "write error");
	if (vcount)
		error(USER, "unexpected EOF on input");
	if (fname != NULL)
		fclose(fp);
}


static void
trace_sources(void)			/* trace rays to light sources, also */
{
	int	sn;
	
	for (sn = 0; sn < nsources; sn++)
		source[sn].sflags |= SFOLLOW;
}


static void
setoutput(				/* set up output tables */
	char  *vs
)
{
	oputf_t **table = ray_out;

	castonly = 1;
	while (*vs)
		switch (*vs++) {
		case 'T':				/* trace sources */
			if (!*vs) break;
			trace_sources();
			/* fall through */
		case 't':				/* trace */
			if (!*vs) break;
			*table = NULL;
			table = every_out;
			trace = ourtrace;
			castonly = 0;
			break;
		case 'o':				/* origin */
			*table++ = oputo;
			break;
		case 'd':				/* direction */
			*table++ = oputd;
			break;
		case 'v':				/* value */
			*table++ = oputv;
			castonly = 0;
			break;
		case 'V':				/* contribution */
			*table++ = oputV;
			if (ambounce > 0 && (ambacc > FTINY || ambssamp > 0))
				error(WARNING,
					"-otV accuracy depends on -aa 0 -as 0");
			break;
		case 'l':				/* effective distance */
			*table++ = oputl;
			castonly = 0;
			break;
		case 'c':				/* local coordinates */
			*table++ = oputc;
			break;
		case 'L':				/* single ray length */
			*table++ = oputL;
			break;
		case 'p':				/* point */
			*table++ = oputp;
			break;
		case 'n':				/* perturbed normal */
			*table++ = oputn;
			castonly = 0;
			break;
		case 'N':				/* unperturbed normal */
			*table++ = oputN;
			break;
		case 's':				/* surface */
			*table++ = oputs;
			break;
		case 'w':				/* weight */
			*table++ = oputw;
			break;
		case 'W':				/* coefficient */
			*table++ = oputW;
			if (ambounce > 0 && (ambacc > FTINY || ambssamp > 0))
				error(WARNING,
					"-otW accuracy depends on -aa 0 -as 0");
			break;
		case 'm':				/* modifier */
			*table++ = oputm;
			break;
		case 'M':				/* material */
			*table++ = oputM;
			break;
		case '~':				/* tilde */
			*table++ = oputtilde;
			break;
		}
	*table = NULL;
}


static void
bogusray(void)			/* print out empty record */
{
	thisray.rorg[0] = thisray.rorg[1] = thisray.rorg[2] =
	thisray.rdir[0] = thisray.rdir[1] = thisray.rdir[2] = 0.0;
	thisray.rmax = 0.0;
	rayorigin(&thisray, PRIMARY, NULL, NULL);
	printvals(&thisray);
}


static void
raycast(			/* compute first ray intersection only */
	RAY *r
)
{
	if (!localhit(r, &thescene)) {
		if (r->ro == &Aftplane) {	/* clipped */
			r->ro = NULL;
			r->rot = FHUGE;
		} else
			sourcehit(r);
	}
}


static void
rayirrad(			/* compute irradiance rather than radiance */
	RAY *r
)
{
	r->rot = 1e-5;			/* pretend we hit surface */
	VSUM(r->rop, r->rorg, r->rdir, r->rot);
	r->ron[0] = -r->rdir[0];
	r->ron[1] = -r->rdir[1];
	r->ron[2] = -r->rdir[2];
	r->rod = 1.0;
					/* compute result */
	(*ofun[Lamb.otype].funp)(&Lamb, r);
}


static void
rtcompute(			/* compute and print ray value(s) */
	FVECT  org,
	FVECT  dir,
	double	dmax
)
{
					/* set up ray */
	rayorigin(&thisray, PRIMARY, NULL, NULL);
	if (imm_irrad) {
		VSUM(thisray.rorg, org, dir, 1.1e-4);
		thisray.rdir[0] = -dir[0];
		thisray.rdir[1] = -dir[1];
		thisray.rdir[2] = -dir[2];
		thisray.rmax = 0.0;
		thisray.revf = rayirrad;
	} else {
		VCOPY(thisray.rorg, org);
		VCOPY(thisray.rdir, dir);
		thisray.rmax = dmax;
		if (castonly)
			thisray.revf = raycast;
	}
	if (ray_pnprocs > 1) {		/* multiprocessing FIFO? */
		if (ray_fifo_in(&thisray) < 0)
			error(USER, "lost children");
		return;
	}
	samplendx++;			/* else do it ourselves */
	rayvalue(&thisray);
	printvals(&thisray);
}


static int
printvals(			/* print requested ray values */
	RAY  *r
)
{
	oputf_t **tp;

	if (ray_out[0] == NULL)
		return(0);
	for (tp = ray_out; *tp != NULL; tp++)
		(**tp)(r);
	if (outform == 'a')
		putchar('\n');
	return(1);
}


static int
getvec(		/* get a vector from fp */
	FVECT  vec,
	int  fmt,
	FILE  *fp
)
{
	static float  vf[3];
	static double  vd[3];
	char  buf[32];
	int  i;

	switch (fmt) {
	case 'a':					/* ascii */
		for (i = 0; i < 3; i++) {
			if (fgetword(buf, sizeof(buf), fp) == NULL ||
					!isflt(buf))
				return(-1);
			vec[i] = atof(buf);
		}
		break;
	case 'f':					/* binary float */
		if (fread((char *)vf, sizeof(float), 3, fp) != 3)
			return(-1);
		VCOPY(vec, vf);
		break;
	case 'd':					/* binary double */
		if (fread((char *)vd, sizeof(double), 3, fp) != 3)
			return(-1);
		VCOPY(vec, vd);
		break;
	default:
		error(CONSISTENCY, "botched input format");
	}
	return(0);
}


void
tranotify(			/* record new modifier */
	OBJECT	obj
)
{
	static int  hitlimit = 0;
	OBJREC	 *o = objptr(obj);
	char  **tralp;

	if (obj == OVOID) {		/* starting over */
		traset[0] = 0;
		hitlimit = 0;
		return;
	}
	if (hitlimit || !ismodifier(o->otype))
		return;
	for (tralp = tralist; *tralp != NULL; tralp++)
		if (!strcmp(o->oname, *tralp)) {
			if (traset[0] >= MAXTSET) {
				error(WARNING, "too many modifiers in trace list");
				hitlimit++;
				return;		/* should this be fatal? */
			}
			insertelem(traset, obj);
			return;
		}
}


static void
ourtrace(				/* print ray values */
	RAY  *r
)
{
	oputf_t **tp;

	if (every_out[0] == NULL)
		return;
	if (r->ro == NULL) {
		if (traincl == 1)
			return;
	} else if (traincl != -1 && traincl != inset(traset, r->ro->omod))
		return;
	tabin(r);
	for (tp = every_out; *tp != NULL; tp++)
		(**tp)(r);
	if (outform == 'a')
		putchar('\n');
}


static void
tabin(				/* tab in appropriate amount */
	RAY  *r
)
{
	const RAY  *rp;

	for (rp = r->parent; rp != NULL; rp = rp->parent)
		putchar('\t');
}


static void
oputo(				/* print origin */
	RAY  *r
)
{
	(*putreal)(r->rorg[0]);
	(*putreal)(r->rorg[1]);
	(*putreal)(r->rorg[2]);
}


static void
oputd(				/* print direction */
	RAY  *r
)
{
	(*putreal)(r->rdir[0]);
	(*putreal)(r->rdir[1]);
	(*putreal)(r->rdir[2]);
}


static void
oputv(				/* print value */
	RAY  *r
)
{
	if (outform == 'c') {
		COLR  cout;
		setcolr(cout,	colval(r->rcol,RED),
				colval(r->rcol,GRN),
				colval(r->rcol,BLU));
		fwrite((char *)cout, sizeof(cout), 1, stdout);
		return;
	}
	(*putreal)(colval(r->rcol,RED));
	(*putreal)(colval(r->rcol,GRN));
	(*putreal)(colval(r->rcol,BLU));
}


static void
oputV(				/* print value contribution */
	RAY *r
)
{
	double	contr[3];

	raycontrib(contr, r, PRIMARY);
	multcolor(contr, r->rcol);
	(*putreal)(contr[RED]);
	(*putreal)(contr[GRN]);
	(*putreal)(contr[BLU]);
}


static void
oputl(				/* print effective distance */
	RAY  *r
)
{
	(*putreal)(r->rt);
}


static void
oputL(				/* print single ray length */
	RAY  *r
)
{
	(*putreal)(r->rot);
}


static void
oputc(				/* print local coordinates */
	RAY  *r
)
{
	(*putreal)(r->uv[0]);
	(*putreal)(r->uv[1]);
}


static void
oputp(				/* print point */
	RAY  *r
)
{
	if (r->rot < FHUGE) {
		(*putreal)(r->rop[0]);
		(*putreal)(r->rop[1]);
		(*putreal)(r->rop[2]);
	} else {
		(*putreal)(0.0);
		(*putreal)(0.0);
		(*putreal)(0.0);
	}
}


static void
oputN(				/* print unperturbed normal */
	RAY  *r
)
{
	if (r->rot < FHUGE) {
		(*putreal)(r->ron[0]);
		(*putreal)(r->ron[1]);
		(*putreal)(r->ron[2]);
	} else {
		(*putreal)(0.0);
		(*putreal)(0.0);
		(*putreal)(0.0);
	}
}


static void
oputn(				/* print perturbed normal */
	RAY  *r
)
{
	FVECT  pnorm;

	if (r->rot >= FHUGE) {
		(*putreal)(0.0);
		(*putreal)(0.0);
		(*putreal)(0.0);
		return;
	}
	raynormal(pnorm, r);
	(*putreal)(pnorm[0]);
	(*putreal)(pnorm[1]);
	(*putreal)(pnorm[2]);
}


static void
oputs(				/* print name */
	RAY  *r
)
{
	if (r->ro != NULL)
		fputs(r->ro->oname, stdout);
	else
		putchar('*');
	putchar('\t');
}


static void
oputw(				/* print weight */
	RAY  *r
)
{
	(*putreal)(r->rweight);
}


static void
oputW(				/* print coefficient */
	RAY  *r
)
{
	double	contr[3];

	raycontrib(contr, r, PRIMARY);
	(*putreal)(contr[RED]);
	(*putreal)(contr[GRN]);
	(*putreal)(contr[BLU]);
}


static void
oputm(				/* print modifier */
	RAY  *r
)
{
	if (r->ro != NULL)
		if (r->ro->omod != OVOID)
			fputs(objptr(r->ro->omod)->oname, stdout);
		else
			fputs(VOIDID, stdout);
	else
		putchar('*');
	putchar('\t');
}


static void
oputM(				/* print material */
	RAY  *r
)
{
	OBJREC	*mat;

	if (r->ro != NULL) {
		if ((mat = findmaterial(r->ro)) != NULL)
			fputs(mat->oname, stdout);
		else
			fputs(VOIDID, stdout);
	} else
		putchar('*');
	putchar('\t');
}


static void
oputtilde(			/* output tilde (spacer) */
	RAY  *r
)
{
	fputs("~\t", stdout);
}


static void
puta(				/* print ascii value */
	double  v
)
{
	printf("%e\t", v);
}


static void
putd(v)				/* print binary double */
double  v;
{
	fwrite((char *)&v, sizeof(v), 1, stdout);
}


static void
putf(v)				/* print binary float */
double  v;
{
	float f = v;

	fwrite((char *)&f, sizeof(f), 1, stdout);
}
