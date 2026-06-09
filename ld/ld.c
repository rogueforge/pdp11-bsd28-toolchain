/*
 * Note: we deliberately don't include <whoami.h> because even on systems
 * with overlays we have to make both ld and ovld.  Hence we compile ld.c
 * twice, once with -DMENLO_OVLY (ovld) and once without it (ld).
 */
#ifdef MENLO_OVLY
char	*sccsid = "@(#)ld.c	2.6 ovld";
#else MENLO_OVLY
char	*sccsid = "@(#)ld.c	2.6 ld";
#endif MENLO_OVLY
/*
 *  link editor
 *  modified to be a overlayed segmentation register loader by wnj 6/79
 *  touched up by bill jolitz 9/79.
 *  working with emt trap thunk fmt 5/80 bill jolitz.
 *  Merged conditionlly with ld 8/80 by Mark Horton.
 */

#ifdef MENLO_OVLY
#include <stdio.h>
#endif MENLO_OVLY
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>


/*	Layout of a.out file :
 *
 *	header of 8 words	magic number 405, 407, 410, 411
 *				text size	)
 *				data size	) in bytes but even
 *				bss size	)
 *				symbol table size
 *				entry point
 *				{unused}
 *				flag set if no relocation
 *
 *
 *	header:		0
 *	text:		16
 *	data:		16+textsize
 *	relocation:	16+textsize+datasize
 *	symbol table:	16+2*(textsize+datasize) or 16+textsize+datasize
 *
 */
#define TRUE	1
#define FALSE	0


#define	ARCMAGIC 0177545
#define OMAGIC	0405
#define	FMAGIC	0407
#define	NMAGIC	0410
#define	IMAGIC	0411

#define	EXTERN	040
#define	UNDEF	00
#define	ABS	01
#define	TEXT	02
#define	DATA	03
#define	BSS	04
#define	COMM	05	/* internal use only */

#define	RABS	00
#define	RTEXT	02
#define	RDATA	04
#define	RBSS	06
#define	REXT	010

#define NOVLY	16
#define	RELFLG	01
#define	NROUT	256
#define	NSYM	1103
#define	NSYMPR	1000

char	premeof[] = "Premature EOF";
char	goodnm[] = "__.SYMDEF";

/* table of contents stuff */
#define TABSZ	700
struct tab
{	char cname[8];
	int32_t cloc;		/* on-disk byte offset of the member: 4 bytes,
				 * so the ranlib entry is 12 bytes (not 16 on LP64) */
} __attribute__((packed)) tab[TABSZ];
int tnum;


/* overlay management */
int	vindex;
struct overlay {
	int	argsav;
	int	symsav;
	struct liblist	*libsav;
	char	*vname;
	int	ctsav, cdsav, cbsav;
	int	offt, offd, offb, offs;
} vnodes[NOVLY];

/* input management */
struct page {
	int	nuser;
	int	bno;
	int	nibuf;
	uint16_t	buff[256];	/* on-disk words: 512 bytes = 256 words */
} page[2];

struct {
	int	nuser;
	int	bno;
} fpage;

struct stream {
	uint16_t	*ptr;
	int	bno;
	int	nibuf;
	int	size;
	struct page	*pno;
};

struct stream text;
struct stream reloc;

struct {
	char	aname[14];
	int32_t	atime;
	char	auid, agid;
	int16_t	amode;
	int32_t	asize;
} __attribute__((packed)) archdr;		/* V7 ar header, 26 bytes = 13 words */

struct {
	uint16_t	fmagic;
	uint16_t	tsize;
	uint16_t	dsize;
	uint16_t	bsize;
	uint16_t	ssize;
	uint16_t	entry;
	uint16_t	pad;
	uint16_t	relflg;
} filhdr;					/* 8 words = 16 bytes */


/* one entry for each archive member referenced;
 * set in first pass; needs restoring for overlays
 */
struct liblist {
	long	loc;
};

struct liblist	liblist[NROUT];
struct liblist	*libp = liblist;


/* symbol management */
struct symbol {
	char	sname[8];
	char	stype;
#ifndef MENLO_OVLY
	char	spare;
#else MENLO_OVLY
	char	sovly;
#endif MENLO_OVLY
	uint16_t	svalue;		/* on-disk 16-bit value; sizeof symbol==12 */
#ifdef MENLO_OVLY
	int	sovalue;
#endif MENLO_OVLY
};

#ifdef MENLO_OVLY
struct xsymbol {
	char	sname[8];
	char	stype;
	char	sovly;
	int	svalue;
};
#endif MENLO_OVLY
struct local {
	int locindex;		/* index to symbol in file */
	struct symbol *locsymbol;	/* ptr to symbol table */
};

#ifndef MENLO_OVLY
struct symbol	cursym;			/* current symbol */
#else MENLO_OVLY
struct xsymbol	cursym;			/* current symbol */
#endif MENLO_OVLY
struct symbol	symtab[NSYM];		/* actual symbols */
struct symbol	**symhash[NSYM];	/* ptr to hash table entry */
struct symbol	*lastsym;		/* last symbol entered */
int	symindex;		/* next available symbol table entry */
struct symbol	*hshtab[NSYM+2];	/* hash table for symbols */
struct local	local[NSYMPR];

/* internal symbols */
struct symbol	*p_etext;
struct symbol	*p_edata;
struct symbol	*p_end;
struct symbol	*entrypt;

int	trace;
/* flags */
int	xflag;		/* discard local symbols */
int	Xflag;		/* discard locals starting with 'L' */
int	Sflag;		/* discard all except locals and globals*/
int	rflag;		/* preserve relocation bits, don't define common */
int	arflag;		/* original copy of rflag */
int	sflag;		/* discard all symbols */
int	nflag;		/* pure procedure */
int	Oflag;		/* set magic # to 0405 (overlay) */
int	dflag;		/* define common even with rflag */
int	iflag;		/* I/D space separated */
int	vflag;		/* overlays used */

int	ofilfnd;
char	*ofilename = "l.out";
int	infil;
char	*filname;
char	fullname[128];

/* cumulative sizes set in pass 1 */
int	tsize;
int	dsize;
int	bsize;
int	ssize;

/* symbol relocation; both passes */
int	ctrel;
int	cdrel;
int	cbrel;

int	errlev;
int	delarg	= 4;
char	tfname[] = "/tmp/ldaXXXXXX";	/* glibc mktemp(3) needs 6 trailing X */


/* output management */
struct buf {
	int	fildes;
	int	nleft;
	uint16_t	*xnext;
	uint16_t	iobuf[256];	/* on-disk words */
};
struct buf	toutb;
struct buf	doutb;
struct buf	troutb;
struct buf	droutb;
struct buf	soutb;

#ifdef MENLO_OVLY
/* wnj added for text overlay register */

#ifdef SYS_CALL
#define THUNKSIZ 18
#else
#define THUNKSIZ	16
#endif

int	torgwas;		/* Saves torigin while doing overlays */
int	tsizwas;		/* Saves tsize while doing overlays */
int	numov;			/* Total number of overlays */
int	curov;			/* Overlay being worked on just now */
int	inov;			/* 1 if working on an overlay */
int	ovsize[8];		/* The sizes of the overlays */

#define	max	ovsize[0]

struct buf	voutb;		/* Overlay text goes here */
/*
struct buf	dummyb;
*/

#ifdef SYS_CALL
				/* The original overlays had a special
				   subroutine to do the switch */
struct	xsymbol ovhndlr =
	{ "ovhndlr", EXTERN+UNDEF, 0, 0 };
#endif
int	ovbase;			/* The base address of the overlays */
/* end overlay stuff */

#endif MENLO_OVLY
struct symbol	**lookup();
struct symbol	**slookup();
struct symbol	*lookloc();

delexit()
{
	unlink("l.out");
	if (delarg==0)
		chmod(ofilename, 0777 & ~umask(0));
	exit(delarg);
}

main(argc, argv)
char **argv;
{
	register int c, i; 
	int num;
	register char *ap, **p;
	int found; 
	int vscan; 
	char save;

	if (signal(SIGINT, SIG_IGN) != SIG_IGN)
		signal(SIGINT, delexit);
	if (signal(SIGTERM, SIG_IGN) != SIG_IGN)
		signal(SIGTERM, delexit);
	if (argc == 1)
		exit(4);
	p = argv+1;

	/* scan files once to find symdefs */
	for (c=1; c<argc; c++) {
		if (trace) printf("%s:\n", *p);
		filname = 0;
		ap = *p++;

		if (*ap == '-') {
			for (i=1; ap[i]; i++) {
			switch (ap[i]) {
			case 'o':
				if (++c >= argc)
					error(2, "Bad output file");
				ofilename = *p++;
				ofilfnd++;
				continue;

			case 'u':
			case 'e':
				if (++c >= argc)
					error(2, "Bad 'use' or 'entry'");
				enter(slookup(*p++));
				if (ap[i]=='e')
					entrypt = lastsym;
				continue;

			case 'v':
				if (++c >= argc)
					error(2, "-v: arg missing");
				vflag=TRUE;
				vscan = vindex; 
				found=FALSE;
				while (--vscan>=0 && found==FALSE)
					found = eq(vnodes[vscan].vname, *p);
				if (found) {
					endload(c, argv);
					restore(vscan);
				} else
					record(c, *p);
				p++;
				continue;

			case 'D':
				if (++c >= argc)
					error(2, "-D: arg missing");
				num = atoi(*p++);
				if (dsize>num)
					error(2, "-D: too small");
				dsize = num;
				continue;

			case 'l':
				save = ap[--i]; 
				ap[i]='-';
				load1arg(&ap[i]); 
				ap[i]=save;
				break;

			case 'x':
				xflag++;
				continue;

			case 'X':
				Xflag++;
				continue;

			case 'S':
				Sflag++; 
				continue;

			case 'r':
				rflag++;
				arflag++;
				continue;

			case 's':
				sflag++;
				xflag++;
				continue;

			case 'n':
				nflag++;
				continue;

			case 'd':
				dflag++;
				continue;

			case 'i':
				iflag++;
				continue;

			case 'O':
				Oflag++;
				continue;

			case 't':
				trace++;
				continue;

#ifdef MENLO_OVLY
/* wnj added for overlay text registers */
			case 'Z':
				if (!inov) {
					tsizwas = tsize;
#ifdef SYS_CALL
					if (numov == 0) {
						cursym = ovhndlr;
						enter(lookup());
					}
#endif
				} else {
					ovsize[curov] = tsize;
					if (trace)
					printf("overlay %d size %d\n", curov, ovsize[curov]);
				}
				tsize = 0;
				inov = 1;
				numov++;
				if (numov > 8)
					error(2, "Too many overlays, limit 7");
				curov++;
				continue;

			case 'L':
				if (inov == 0)
					error(2, "-L: Not in overlay");
				ovsize[curov] = tsize;
				if (trace)
				printf("overlay %d size %d\n", curov, ovsize[curov]);
				curov = inov = 0;
				tsize = tsizwas;
				continue;
/* end overlay text addition */

#endif MENLO_OVLY
			default:
				error(2, "bad flag");
			} /*endsw*/
			break;
			} /*endfor*/
		} else
			load1arg(ap);
	}
	endload(argc, argv);
}

/* used after pass 1 */
int	nsym;
int	torigin;
int	dorigin;
int	borigin;

endload(argc, argv)
int argc; 
char **argv;
{
	register int c, i; 
	int dnum;
	register char *ap, **p;
#ifdef MENLO_OVLY

	if (numov)
		rflag = 0;
#endif MENLO_OVLY
	filname = 0;
	middle();
	setupout();
	p = argv+1;
	libp = liblist;
	for (c=1; c<argc; c++) {
		ap = *p++;
		if (trace) printf("%s:\n", ap);
		if (*ap == '-') {
			for (i=1; ap[i]; i++) {
			switch (ap[i]) {
			case 'D':
				for (dnum = atoi(*p); dorigin<dnum; dorigin += 2) {
					putw(0, &doutb);
					if (rflag)
						putw(0, &droutb);
				}
			case 'u':
			case 'e':
			case 'o':
			case 'v':
				++c; 
				++p;

			default:
				continue;

			case 'l':
				ap[--i]='-'; 
				load2arg(&ap[i]);
				break;
#ifdef MENLO_OVLY

/* wnj added for overlay text segmentation registers */
			case 'Z':
				if (inov == 0)
					torgwas = torigin;
				else
					roundov();
				torigin = ovbase;
				inov = 1;
				curov++;
				continue;

			case 'L':
				roundov();
				inov = 0;
				if (trace)
					printf("end overlay generation\n");
				torigin = torgwas;
				continue;
/* end wnj added for text overlay registers */
#endif MENLO_OVLY
			} /*endsw*/
			break;
			} /*endfor*/
		} else
			load2arg(ap);
	}
	finishout();
}

#ifdef MENLO_OVLY
roundov()
{

	while (torigin & 077) {
		putw(0, &voutb);
		torigin += sizeof (int);
	}
}

#endif MENLO_OVLY
record(c, nam)
int c; 
char *nam;
{
	register struct overlay *v;

	v = &vnodes[vindex++];
	v->argsav = c;
	v->symsav = symindex;
	v->libsav = libp;
	v->vname = nam;
	v->offt = tsize; 
	v->offd = dsize; 
	v->offb = bsize; 
	v->offs = ssize;
	v->ctsav = ctrel; 
	v->cdsav = cdrel; 
	v->cbsav = cbrel;
}

restore(vscan)
int vscan;
{
	register struct overlay *v;
	register int saved;

	v = &vnodes[vscan];
	vindex = vscan+1;
	libp = v->libsav;
	ctrel = v->ctsav; 
	cdrel = v->cdsav; 
	cbrel = v->cbsav;
	tsize = v->offt; 
	dsize = v->offd; 
	bsize = v->offb; 
	ssize = v->offs;
	saved = v->symsav;
	while (symindex>saved)
		*symhash[--symindex]=0;
}

/* scan file to find defined symbols */
load1arg(acp)
char *acp;
{
	register char *cp;
	long nloc;

	cp = acp;
	switch ( getfile(cp)) {
	case 0:
		load1(0, 0L);
		break;

	/* regular archive */
	case 1:
		nloc = 1;
		while ( step(nloc))
			nloc += (archdr.asize + sizeof(archdr) + 1) >> 1;
		break;

	/* table of contents */
	case 2:
		tnum = archdr.asize / sizeof(struct tab);
		if (tnum >= TABSZ) {
			error(2, "fast load buffer too small");
		}
		lseek(infil, (long)(sizeof(filhdr.fmagic)+sizeof(archdr)), 0);
		read(infil, (char *)tab, tnum * sizeof(struct tab));
		while (ldrand());
		libp->loc = -1;
		libp++;
		break;
	/* out of date table of contents */
	case 3:
		error(0, "out of date (warning)");
		for(nloc = 1+((archdr.asize+sizeof(archdr)+1) >> 1); step(nloc);
			nloc += (archdr.asize + sizeof(archdr) + 1) >> 1);
		break;
	}
	close(infil);
}

step(nloc)
long nloc;
{
	dseek(&text, nloc, sizeof archdr);
	if (text.size <= 0) {
		libp->loc = -1;
		libp++;
		return(0);
	}
	mget((uint16_t *)&archdr, sizeof archdr);
	if (load1(1, nloc + (sizeof archdr) / 2)) {
		libp->loc = nloc;
		libp++;
	}
	return(1);
}

ldrand()
{
	int i;
	struct symbol *sp, **pp;
	struct liblist *oldp = libp;
#ifndef MENLO_OVLY
	long lloc;
#endif MENLO_OVLY
	for(i = 0; i<tnum; i++) {
		if ((pp = slookup(tab[i].cname)) == 0)
			continue;
		sp = *pp;
		if (sp == 0)		/* empty hash slot: this __.SYMDEF symbol is
					 * not referenced.  On the PDP-11 the next line
					 * read address 0 harmlessly; guard it here. */
			continue;
		if (sp->stype != EXTERN+UNDEF)
			continue;
		step(tab[i].cloc >> 1);
#ifndef MENLO_OVLY
		lloc = tab[i].cloc;
		while (i < tnum-1 && tab[i+1].cloc == lloc)
			i++;
#endif MENLO_OVLY
	}
	return(oldp != libp);
}

add(a,b,s)
int a, b;
char *s;
{
	long r;

	r = (long)(unsigned)a + (unsigned)b;
	if (r >= 0200000)
		error(1,s);
	return(r);
}


/* single file or archive member */
load1(libflg, loc)
long loc;
{
	register struct symbol *sp;
	int savindex;
	int ndef, nloc, type, mtype;

	readhdr(loc);
	ctrel = tsize;
	cdrel += dsize;
	cbrel += bsize;
	ndef = 0;
	nloc = sizeof cursym;
	savindex = symindex;
	if ((filhdr.relflg&RELFLG)==1) {
		error(1, "No relocation bits");
		return(0);
	}
	loc += (sizeof filhdr)/2 + filhdr.tsize + filhdr.dsize;
	dseek(&text, loc, filhdr.ssize);
	while (text.size > 0) {
		mget((uint16_t *)&cursym, sizeof cursym);
		type = cursym.stype;
		if (Sflag) {
			mtype = type&037;
			if (mtype==1 || mtype>4) {
				continue;
			}
		}
		if ((type&EXTERN)==0) {
			if (Xflag==0 || cursym.sname[0]!='L')
				nloc += sizeof cursym;
			continue;
		}
#ifdef MENLO_OVLY
		if (inov && cursym.sname[0] == 'c') {
			if (!strcmp(cursym.sname, "csv"))
				strcpy(cursym.sname, "ovcsv");
			if (!strcmp(cursym.sname, "cret"))
				strcpy(cursym.sname, "ovcret");
		}
#endif MENLO_OVLY
		symreloc();
		if (enter(lookup()))
			continue;
		if ((sp = lastsym)->stype != EXTERN+UNDEF)
			continue;
		if (cursym.stype == EXTERN+UNDEF) {
			if (cursym.svalue > sp->svalue)
				sp->svalue = cursym.svalue;
			continue;
		}
		if (sp->svalue != 0 && cursym.stype == EXTERN+TEXT)
			continue;
		ndef++;
		sp->stype = cursym.stype;
		sp->svalue = cursym.svalue;
#ifdef MENLO_OVLY
		if ((sp->stype &~ EXTERN) == TEXT)
			sp->sovly = curov;
		if (trace)
		printf("found %8.8s in overlay %d at %d\n", sp->sname, sp->sovly, sp->svalue);
#endif MENLO_OVLY
	}
	if (libflg==0 || ndef) {
		tsize = add(tsize,filhdr.tsize,"text overflow");
		dsize = add(dsize,filhdr.dsize,"data overflow");
		bsize = add(bsize,filhdr.bsize,"bss overflow");
		ssize = add(ssize,nloc,"symbol table overflow");
		return(1);
	}
	/*
	 * No symbols defined by this library member.
	 * Rip out the hash table entries and reset the symbol table.
	 */
	while (symindex>savindex)
		*symhash[--symindex]=0;
	return(0);
}

middle()
{
	register struct symbol *sp, *symp;
	register t, csize;
	int nund, corigin;
#ifdef MENLO_OVLY
	int ttsize;
#endif MENLO_OVLY

	torigin=0; 
	dorigin=0; 
	borigin=0;

	p_etext = *slookup("_etext");
	p_edata = *slookup("_edata");
	p_end = *slookup("_end");
	/*
	 * If there are any undefined symbols, save the relocation bits.
	 */
	symp = &symtab[symindex];
	if (rflag==0) {
		for (sp = symtab; sp<symp; sp++)
			if (sp->stype==EXTERN+UNDEF && sp->svalue==0
				&& sp!=p_end && sp!=p_edata && sp!=p_etext) {
				rflag++;
				dflag = 0;
				break;
			}
	}
	if (rflag)
		nflag = sflag = iflag = Oflag = 0;
	/*
	 * Assign common locations.
	 */
	csize = 0;
	if (dflag || rflag==0) {
		ldrsym(p_etext, tsize, EXTERN+TEXT);
		ldrsym(p_edata, dsize, EXTERN+DATA);
		ldrsym(p_end, bsize, EXTERN+BSS);
		for (sp = symtab; sp<symp; sp++)
			if (sp->stype==EXTERN+UNDEF && (t = sp->svalue)!=0) {
				t = (t+1) & ~01;
				sp->svalue = csize;
				sp->stype = EXTERN+COMM;
				csize = add(csize, t, "bss overflow");
			}
	}
#ifdef MENLO_OVLY
/* wnj added for overlay text */
	if (numov) {
		for (sp = symtab; sp < symp; sp++) {
			if (trace)
			printf("%8.8s stype %o svalue %o sovalue %o sovly %d\n",
			   sp->sname, sp->stype, sp->svalue, sp->sovalue, sp->sovly);
			if (sp->sovly && sp->stype == EXTERN+TEXT) {
				sp->sovalue = sp->svalue;
				sp->svalue = tsize;
				tsize += THUNKSIZ;
				if (trace)
					printf("relocating %s in overlay %d from %o to %o\n",
					    sp->sname, sp->sovly,
					    sp->sovalue, sp->svalue);
			}
		}
	}
/* end wnj added */
#endif MENLO_OVLY
	/*
	 * Now set symbols to their final value
	 */
	if (nflag || iflag)
		tsize = (tsize + 077) & ~077;
#ifndef MENLO_OVLY
	dorigin = tsize;
#else MENLO_OVLY
/* wnj added */
	ttsize = tsize;
	if (numov) {
		register int i;

		ovbase = (tsize + 017777) &~ 017777;
		if (trace)
			printf("overlay base is %d.\n", ovbase);
		for (sp = symtab; sp < symp; sp++)
			if (sp->sovly && sp->stype == EXTERN+TEXT) {
				sp->sovalue += ovbase;
				if (trace)
					printf("%.8s at %d overlay %d\n", sp->sname, sp->sovalue, sp->sovly);
			}
		for (i = 1; i < 8; i++) {
			ovsize[i] = (ovsize[i] + 077) &~ 077;
			if (ovsize[i] > max)
				max = ovsize[i];
		}
		if (trace)
			printf("maximum overlay size is %d.\n", max);
		ttsize = ovbase + max;
		ttsize = (ttsize + 017777) &~ 017777;
		if (trace)
			printf("overlays end before %u.\n", ttsize);
	}
/* end wnj added */
	dorigin = ttsize;
#endif MENLO_OVLY
	if (nflag)
#ifndef MENLO_OVLY
		dorigin = (tsize+017777) & ~017777;
#else MENLO_OVLY
		dorigin = (ttsize+017777) & ~017777;
#endif MENLO_OVLY
	if (iflag)
		dorigin = 0;
	corigin = dorigin + dsize;
	borigin = corigin + csize;
	nund = 0;
	for (sp = symtab; sp<symp; sp++) switch (sp->stype) {
	case EXTERN+UNDEF:
		errlev |= 01;
		if (arflag==0 && sp->svalue==0) {
			if (nund==0)
				printf("Undefined:\n");
			nund++;
			printf("%.8s\n", sp->sname);
		}
		continue;

	case EXTERN+ABS:
	default:
		continue;

	case EXTERN+TEXT:
		sp->svalue += torigin;
		continue;

	case EXTERN+DATA:
		sp->svalue += dorigin;
		continue;

	case EXTERN+BSS:
		sp->svalue += borigin;
		continue;

	case EXTERN+COMM:
		sp->stype = EXTERN+BSS;
		sp->svalue += corigin;
		continue;
	}
	if (sflag || xflag)
		ssize = 0;
	bsize = add(bsize, csize, "bss overflow");
	nsym = ssize / (sizeof cursym);
}

ldrsym(asp, val, type)
struct symbol *asp;
{
	register struct symbol *sp;

	if ((sp = asp) == 0)
		return;
	if (sp->stype != EXTERN+UNDEF || sp->svalue) {
		printf("%.8s: ", sp->sname);
#ifdef MENLO_OVLY
/* 		if (trace) */
			printf("svalue %o ", sp->svalue);
#endif MENLO_OVLY
		error(1, "Multiply defined");
		return;
	}
	sp->stype = type;
	sp->svalue = val;
}

setupout()
{
	tcreat(&toutb, 0);
	mktemp(tfname);
	tcreat(&doutb, 1);
	if (sflag==0 || xflag==0)
		tcreat(&soutb, 1);
	if (rflag) {
		tcreat(&troutb, 1);
		tcreat(&droutb, 1);
	}
#ifdef MENLO_OVLY
/* wnj added */
	if (numov)
		tcreat(&voutb, 1);
/* end wnj added */
#endif MENLO_OVLY
	filhdr.fmagic = (Oflag ? OMAGIC :( iflag ? IMAGIC : ( nflag ? NMAGIC : FMAGIC )));
#ifdef MENLO_OVLY
	if (numov) {
		if (filhdr.fmagic == FMAGIC)
			error(2, "Can't register overlay 407 execs");
		filhdr.fmagic |= 020;
	}
#endif MENLO_OVLY
	filhdr.tsize = tsize;
	filhdr.dsize = dsize;
	filhdr.bsize = bsize;
	filhdr.ssize = sflag? 0: (ssize + (sizeof cursym)*symindex);
	if (entrypt) {
		if (entrypt->stype!=EXTERN+TEXT)
			error(1, "Entry point not in text");
#ifdef MENLO_OVLY
		else if (entrypt->sovly)
			error(1, "Entry point in overlay");
#endif MENLO_OVLY
		else
			filhdr.entry = entrypt->svalue | 01;
	} else
		filhdr.entry=0;
	filhdr.pad = 0;
	filhdr.relflg = (rflag==0);
	mput(&toutb, (uint16_t *)&filhdr, sizeof filhdr);
#ifdef MENLO_OVLY
/* wnj added */
	if (numov) {
		register int i;
		for (i = 0; i < 8; i++)
			putw(ovsize[i], &toutb);
	}
/* end wnj */
#endif MENLO_OVLY
}

tcreat(buf, tempflg)
struct buf *buf;
{
	register int ufd; 
	char *nam;
	nam = (tempflg ? tfname : ofilename);
	if ((ufd = creat(nam, 0666)) < 0)
		error(2, tempflg?"cannot create temp":"cannot create output");
	close(ufd); 
	buf->fildes = open(nam, 2);
	if (tempflg)
		unlink(tfname);
	buf->nleft = sizeof(buf->iobuf)/sizeof(*buf->iobuf);
	buf->xnext = buf->iobuf;
}

load2arg(acp)
char *acp;
{
	register char *cp;
	register struct liblist *lp;

	cp = acp;
	if (getfile(cp) == 0) {
		while (*cp)
			cp++;
		while (cp >= acp && *--cp != '/');
		mkfsym(++cp);
		load2(0L);
	} else {	/* scan archive members referenced */
		for (lp = libp; lp->loc != -1; lp++) {
			dseek(&text, lp->loc, sizeof archdr);
			mget((uint16_t *)&archdr, sizeof archdr);
			mkfsym(archdr.aname);
			load2(lp->loc + (sizeof archdr) / 2);
		}
		libp = ++lp;
	}
	close(infil);
}

load2(loc)
long loc;
{
	register struct symbol *sp;
	register struct local *lp;
	register int symno;
	int type, mtype;

	readhdr(loc);
	ctrel = torigin;
	cdrel += dorigin;
	cbrel += borigin;
	/*
	 * Reread the symbol table, recording the numbering
	 * of symbols for fixing external references.
	 */
	lp = local;
	symno = -1;
	loc += (sizeof filhdr)/2;
	dseek(&text, loc + filhdr.tsize + filhdr.dsize, filhdr.ssize);
	while (text.size > 0) {
		symno++;
		mget((uint16_t *)&cursym, sizeof cursym);
#ifdef MENLO_OVLY
		if (inov && cursym.sname[0] == 'c') {
			if (!strcmp(cursym.sname, "csv"))
				strcpy(cursym.sname, "ovcsv");
			if (!strcmp(cursym.sname, "cret"))
				strcpy(cursym.sname, "ovcret");
		}
#endif MENLO_OVLY
		symreloc();
		type = cursym.stype;
		if (Sflag) {
			mtype = type&037;
			if (mtype==1 || mtype>4) continue;
		}
		if ((type&EXTERN) == 0) {
			if (!sflag&&!xflag&&(!Xflag||cursym.sname[0]!='L'))
				mput(&soutb, (uint16_t *)&cursym, sizeof cursym);
			continue;
		}
		if ((sp = *lookup()) == 0)
			error(2, "internal error: symbol not found");
#ifdef MENLO_OVLY
		/*
		 * Bill Shannon's fix to the 'local symbol botch'
		 * message. -wfj 5/80
		 */
		if (cursym.stype == EXTERN+UNDEF || cursym.stype == EXTERN+TEXT)
		{
#else !MENLO_OVLY
		if (cursym.stype == EXTERN+UNDEF) {
#endif MENLO_OVLY
			if (lp >= &local[NSYMPR])
				error(2, "Local symbol overflow");
			lp->locindex = symno;
			lp++->locsymbol = sp;
			continue;
		}
#ifndef MENLO_OVLY
		if (cursym.stype!=sp->stype || cursym.svalue!=sp->svalue) {
#else MENLO_OVLY
		if (cursym.stype!=sp->stype
		    || cursym.svalue!=sp->svalue && !sp->sovly
		    || sp->sovly && cursym.svalue!=sp->sovalue) {
#endif MENLO_OVLY
			printf("%.8s: ", cursym.sname);
#ifdef MENLO_OVLY
/* 			if (trace) { */
				printf(" sovly %d sovalue %o ", sp->sovly, sp->sovalue);
				printf("new %o hav %o ", cursym.svalue, sp->svalue);
/* 			} */
#endif MENLO_OVLY
			error(1, "Multiply defined");
		}
	}
	dseek(&text, loc, filhdr.tsize);
	dseek(&reloc, loc + half(filhdr.tsize + filhdr.dsize), filhdr.tsize);
#ifndef MENLO_OVLY
	load2td(lp, ctrel, &toutb, &troutb);
#else MENLO_OVLY
	load2td(lp, ctrel, inov ? &voutb : &toutb, &troutb);
#endif MENLO_OVLY
	dseek(&text, loc+half(filhdr.tsize), filhdr.dsize);
	dseek(&reloc, loc+filhdr.tsize+half(filhdr.dsize), filhdr.dsize);
	load2td(lp, cdrel, &doutb, &droutb);
	torigin += filhdr.tsize;
	dorigin += filhdr.dsize;
	borigin += filhdr.bsize;
}

load2td(lp, creloc, b1, b2)
struct local *lp;
struct buf *b1, *b2;
{
	register r, t;
	register struct symbol *sp;

	for (;;) {
		/*
			 * The pickup code is copied from "get" for speed.
			 */

		/* next text or data word */
		if (--text.size <= 0) {
			if (text.size < 0)
				break;
			text.size++;
			t = get(&text);
		} else if (--text.nibuf < 0) {
			text.nibuf++;
			text.size++;
			t = get(&text);
		} else
			t = *text.ptr++;

		/* next relocation word */
		if (--reloc.size <= 0) {
			if (reloc.size < 0)
				error(2, "Relocation error");
			reloc.size++;
			r = get(&reloc);
		} else if (--reloc.nibuf < 0) {
			reloc.nibuf++;
			reloc.size++;
			r = get(&reloc);
		} else
			r = *reloc.ptr++;

		switch (r&016) {

		case RTEXT:
			t += ctrel;
			break;

		case RDATA:
			t += cdrel;
			break;

		case RBSS:
			t += cbrel;
			break;

		case REXT:
			sp = lookloc(lp, r);
			if (sp->stype==EXTERN+UNDEF) {
				r = (r&01) + ((nsym+(sp-symtab))<<4) + REXT;
				break;
			}
			t += sp->svalue;
			r = (r&01) + ((sp->stype-(EXTERN+ABS))<<1);
			break;
		}
		if (r&01)
			t -= creloc;
		putw(t, b1);
		if (rflag)
			putw(r, b2);
	}
}

finishout()
{
	register int n;
	register uint16_t *p;
#ifdef MENLO_OVLY
	register struct symbol *sp, *symp;
#endif MENLO_OVLY

#ifdef MENLO_OVLY
/* wnj added */
	if (numov) {
		int aovno = adrof("__ovno");
#ifdef SYS_CALL
		int aovhndlr = adrof("ovhndlr");
#endif
		symp = &symtab[symindex];
		for (sp = symtab; sp < symp; sp++)
			if (sp->sovly && (sp->stype & (EXTERN+TEXT))) {
				putw(012700, &toutb);	/* mov $foo's_ovno, r0*/
				putw(sp->sovly, &toutb);
				putw(020037, &toutb);	/* cmp r0,*$__ovno */
				putw(aovno, &toutb);
#ifdef SYS_CALL
				putw(01002, &toutb);	/* bne 1f */
				putw(0137, &toutb);	/* jmp *$sp->sovalue */
				putw(sp->sovalue, &toutb);
				putw(04737, &toutb);	/* 1:jsr pc,*$ovhndlr */
				putw(aovhndlr, &toutb);
#else
				putw(01401, &toutb);	/* beq 1f */
				putw(0104000, &toutb);	/* emt 0  */
				putw(0137, &toutb);	/* 1:jmp *$~foo */
				putw(sp->sovalue, &toutb);
#endif
				torigin += THUNKSIZ;
			}
	}
/* end wnj */
#endif MENLO_OVLY
	if (nflag||iflag) {
		n = torigin;
		while (n&077) {
			n += 2;
			putw(0, &toutb);
			if (rflag)
				putw(0, &troutb);
		}
	}
#ifdef MENLO_OVLY
	if (numov)
		copy(&voutb);
#endif MENLO_OVLY
	copy(&doutb);
	if (rflag) {
		copy(&troutb);
		copy(&droutb);
	}
	if (sflag==0) {
		if (xflag==0)
			copy(&soutb);
#ifndef MENLO_OVLY
		for (p = (uint16_t *)symtab; p < (uint16_t *)&symtab[symindex];)
			putw(*p++, &toutb);
#else MENLO_OVLY
		for (p = (int *)symtab; p < (int *)&symtab[symindex];) {
/* wnj changed.... this is bad machine dependent code... */
			/* this does the symbol */
			putw(*p++, &toutb); putw(*p++, &toutb); 
			putw(*p++, &toutb); putw(*p++, &toutb); 
			/* these do the flags and value */
			putw(*p++, &toutb); putw(*p++, &toutb); 
			/* skip sovalue */
			p++;
		}
/* end wnj changed */
#endif MENLO_OVLY
	}
	flush(&toutb);
	close(toutb.fildes);
	if (!ofilfnd) {
		unlink("a.out");
		link("l.out", "a.out");
		ofilename = "a.out";
	}
	delarg = errlev;
	delexit();
}

#ifdef MENLO_OVLY
/* wnj added for overlay txt regs */
adrof(s)
	char *s;
{
	register struct symbol **p = slookup(s);

	if (*p == 0) {
		printf("%.8s: ", s);
		error(1, "Undefined!");
		return (0);
	}
	return ((*p)->svalue);
}
/* end wnj added */

#endif MENLO_OVLY
copy(buf)
struct buf *buf;
{
	register int f, n;
	register uint16_t *p;

	flush(buf);
	lseek(f = buf->fildes, (long)0, 0);
#ifndef MENLO_OVLY
	while ((n = read(f, (char *)doutb.iobuf, sizeof(doutb.iobuf))) > 1) {
#else MENLO_OVLY
	while ((n = read(f, (char *)buf->iobuf, sizeof(buf->iobuf))) > 1) {
#endif MENLO_OVLY
		n >>= 1;
#ifndef MENLO_OVLY
		p = (uint16_t *)doutb.iobuf;
#else MENLO_OVLY
		p = (uint16_t *)buf->iobuf;
#endif MENLO_OVLY
		do
			putw(*p++, &toutb);
		while (--n);
	}
	close(f);
}

mkfsym(s)
char *s;
{

	if (sflag || xflag)
		return;
	cp8c(s, cursym.sname);
	cursym.stype = 037;
	cursym.svalue = torigin;
	mput(&soutb, (uint16_t *)&cursym, sizeof cursym);
}

mget(aloc, an)
uint16_t *aloc;
{
	register uint16_t *loc;
	register int n;
	register uint16_t *p;

	n = an;
	n >>= 1;
	loc = aloc;
	if ((text.nibuf -= n) >= 0) {
		if ((text.size -= n) > 0) {
			p = text.ptr;
			do
				*loc++ = *p++;
			while (--n);
			text.ptr = p;
			return;
		} else
			text.size += n;
	}
	text.nibuf += n;
	do {
		*loc++ = get(&text);
	} 
	while (--n);
}

mput(buf, aloc, an)
struct buf *buf;
uint16_t *aloc;
{
	register uint16_t *loc;
	register int n;

	loc = aloc;
	n = an>>1;
	do {
		putw(*loc++, buf);
	} 
	while (--n);
}

dseek(asp, aloc, s)
long aloc;
struct stream *asp;
{
	register struct stream *sp;
	register struct page *p;
	/* register */ long b, o;
	int n;

	b = aloc >> 8;
	o = aloc & 0377;
	sp = asp;
	--sp->pno->nuser;
	if ((p = &page[0])->bno!=b && (p = &page[1])->bno!=b)
		if (p->nuser==0 || (p = &page[0])->nuser==0) {
			if (page[0].nuser==0 && page[1].nuser==0)
				if (page[0].bno < page[1].bno)
					p = &page[0];
			p->bno = b;
			lseek(infil, (aloc & ~0377L) << 1, 0);
			if ((n = read(infil, (char *)p->buff, 512)>>1) < 0)
				n = 0;
			p->nibuf = n;
	} else
		error(2, "No pages");
	++p->nuser;
	sp->bno = b;
	sp->pno = p;
	sp->ptr = p->buff + o;
	if (s != -1)
		sp->size = half(s);
	if ((sp->nibuf = p->nibuf-o) <= 0)
		sp->size = 0;
}

half(i)
{
	return((i>>1)&077777);
}

get(asp)
struct stream *asp;
{
	register struct stream *sp;

	sp = asp;
	if (--sp->nibuf < 0) {
		dseek(sp, (long)(sp->bno + 1) << 8, -1);
		--sp->nibuf;
	}
	if (--sp->size <= 0) {
		if (sp->size < 0)
			error(2, premeof);
		++fpage.nuser;
		--sp->pno->nuser;
		sp->pno = (struct page *)&fpage;
	}
	return(*sp->ptr++);
}

getfile(acp)
char *acp;
{
	register char *cp;
	register int c;
	struct stat x;

	cp = acp; 
	infil = -1;
	archdr.aname[0] = '\0';
	if (cp[0]=='-' && cp[1]=='l') {
		static char libpath[256];
		static char libdir[256];
		if(cp[2] == '\0')
			cp = "-la";
		/* Resolve the library directory relative to the ld binary via
		 * /proc/self/exe, like cc's setup_tools and the vax project's ld:
		 * .../usr/bin/<prefix>-ld -> .../usr/lib/lib<x>.a.  (The authentic
		 * openlp $PATH search assumes an installed /usr tree.) */
		strcpy(libdir, "/usr/lib");
		{ static char selfpath[1024];
		  int n = readlink("/proc/self/exe", selfpath, sizeof selfpath - 1);
		  if (n > 0) {
			char *sl; selfpath[n] = '\0';
			for (sl = selfpath+n; sl > selfpath && sl[-1] != '/'; sl--);
			if (sl-selfpath >= 4 &&
			    sl[-4]=='b' && sl[-3]=='i' && sl[-2]=='n' && sl[-1]=='/') {
				sl[-4] = '\0';
				sprintf(libdir, "%slib", selfpath);
			}
		  }
		}
		sprintf(libpath, "%s/lib%s.a", libdir, cp + 2);
		filname = libpath;
		infil = open(filname, 0);
	}
	else {
		filname = cp;
		infil = open(filname, 0);
	}
	if (infil < 0)
		error(2, "cannot open");
	page[0].bno = page[1].bno = -1;
	page[0].nuser = page[1].nuser = 0;
	text.pno = reloc.pno = (struct page *)&fpage;
	fpage.nuser = 2;
	dseek(&text, 0L, 2);
	if (text.size <= 0)
		error(2, premeof);
	if(get(&text) != ARCMAGIC)
		return(0);	/* regualr file */
	dseek(&text, 1L, sizeof archdr);	/* word addressing */
	if(text.size <= 0)
		return(1);	/* regular archive */
	mget((uint16_t *)&archdr, sizeof archdr);
	if(strncmp(archdr.aname, goodnm, 14) != 0)
		return(1);	/* regular archive */
	else {
		fstat(infil, &x);
		if(x.st_mtime > archdr.atime)
		{
			return(3);
		}
		else return(2);
	}
}

struct symbol **lookup()
{
	int i; 
	int clash;
	register struct symbol **hp;
	register char *cp, *cp1;

	i = 0;
	for (cp = cursym.sname; cp < &cursym.sname[8];)
		i = (i<<1) + *cp++;
	for (hp = &hshtab[(i&077777)%NSYM+2]; *hp!=0;) {
		cp1 = (*hp)->sname; 
		clash=FALSE;
		for (cp = cursym.sname; cp < &cursym.sname[8];)
			if (*cp++ != *cp1++) {
				clash=TRUE; 
				break;
			}
		if (clash) {
			if (++hp >= &hshtab[NSYM+2])
				hp = hshtab;
		} else
			break;
	}
	return(hp);
}

struct symbol **slookup(s)
char *s;
{
	cp8c(s, cursym.sname);
	cursym.stype = EXTERN+UNDEF;
	cursym.svalue = 0;
	return(lookup());
}

enter(hp)
struct symbol **hp;
{
	register struct symbol *sp;

	if (*hp==0) {
		if (symindex>=NSYM)
			error(2, "Symbol table overflow");
		symhash[symindex] = hp;
		*hp = lastsym = sp = &symtab[symindex++];
		cp8c(cursym.sname, sp->sname);
		sp->stype = cursym.stype;
		sp->svalue = cursym.svalue;
#ifdef MENLO_OVLY
		if (sp->stype == EXTERN+TEXT) {
			sp->sovly = curov;
			if (trace)
				printf("found %8.8s in overlay %d at %d\n",
				    sp->sname, sp->sovly, sp->svalue);
		}
#endif MENLO_OVLY
		return(1);
	} else {
		lastsym = *hp;
		return(0);
	}
}

symreloc()
{
	switch (cursym.stype) {

	case TEXT:
	case EXTERN+TEXT:
		cursym.svalue += ctrel;
		return;

	case DATA:
	case EXTERN+DATA:
		cursym.svalue += cdrel;
		return;

	case BSS:
	case EXTERN+BSS:
		cursym.svalue += cbrel;
		return;

	case EXTERN+UNDEF:
		return;
	}
	if (cursym.stype&EXTERN)
		cursym.stype = EXTERN+ABS;
}

error(n, s)
char *s;
{
	if (errlev==0)
		printf("ld:");
	if (filname) {
		printf("%s", filname);
		if (archdr.aname[0])
			printf("(%.14s)", archdr.aname);
		printf(": ");
	}
	printf("%s\n", s);
	if (n > 1)
		delexit();
	errlev = n;
}

struct symbol *
lookloc(alp, r)
struct local *alp;
{
	register struct local *clp, *lp;
	register sn;

	lp = alp;
	sn = (r>>4) & 07777;
	for (clp = local; clp<lp; clp++)
		if (clp->locindex == sn)
			return(clp->locsymbol);
	error(2, "Local symbol botch");
}

readhdr(loc)
long loc;
{
	register st, sd;

	dseek(&text, loc, sizeof filhdr);
	mget((uint16_t *)&filhdr, sizeof filhdr);
	if (filhdr.fmagic != FMAGIC)
		error(2, "Bad format");
	st = (filhdr.tsize+01) & ~01;
	filhdr.tsize = st;
	cdrel = -st;
	sd = (filhdr.dsize+01) & ~01;
	cbrel = - (st+sd);
	filhdr.bsize = (filhdr.bsize+01) & ~01;
}

cp8c(from, to)
char *from, *to;
{
	register char *f, *t, *te;

	f = from;
	t = to;
	te = t+8;
	while ((*t++ = *f++) && t<te);
	while (t<te)
		*t++ = 0;
}

eq(s1, s2)
char *s1; 
char *s2;
{
	while (*s1==*s2++)
		if ((*s1++)==0)
			return(TRUE);
	return(FALSE);
}

putw(w, b)
register struct buf *b;
{
	*(b->xnext)++ = w;
	if (--b->nleft <= 0)
		flush(b);
}

flush(b)
register struct buf *b;
{
	register n;

	if ((n = (char *)b->xnext - (char *)b->iobuf) > 0)
		if (write(b->fildes, (char *)b->iobuf, n) != n)
			error(2, "output error");
	b->xnext = b->iobuf;
	b->nleft = sizeof(b->iobuf)/sizeof(*b->iobuf);
}
