/*
 * as -- PDP-11 assembler for 2.8BSD, reimplemented in C so it cross-
 * assembles on a modern 64-bit host.
 *
 * Accepts the authentic 2BSD assembler syntax c1 emits and produces classic
 * 2.8BSD a.out objects: 0407 header, one 16-bit relocation word per code
 * word, and 12-byte symbol entries with inline 8-char names -- the format
 * this toolchain's ld and nm read (NOT the newer string-table a.out that GNU
 * binutils pdp11-aout uses).
 *
 * The opcode table (as/optab.h) is the authentic table from as19.s.  The
 * type-class encoding follows the key documented there:
 *   1 abs  6 branch  7 jsr/xor  010 rts  011 sys  013 double-op  015 single
 *   016 .byte  017 .ascii  020 .even  023 .globl  024 register  025/26/27
 *   .text/.data/.bss  030 mul/div(EIS)  031 sob  032 .comm  035 jbr  036 jxxx
 *   040 .word (bare expression).  Span-dependent jumps (035/036) are always
 *   emitted in the long (jmp) form -- correct, if not minimal.
 *
 * Usage:  as [-o out.o] [-u] file
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct op { char *name; int type; int opcode; };
#include "optab.h"

/* expression segment / relocation kind */
#define SABS 0
#define STEXT 1
#define SDATA 2
#define SBSS 3
#define SEXT 4

/* on-disk relocation type field (r & 016) and pcrel bit */
#define RABS 000
#define RTEXT 002
#define RDATA 004
#define RBSS 006
#define REXT 010
#define RPCREL 001

/* on-disk symbol n_type */
#define N_UNDF 0
#define N_ABS 01
#define N_TEXT 02
#define N_DATA 03
#define N_BSS 04
#define N_EXT 040

#define NCPS 8
struct sym {
	char name[NCPS];
	int seg;		/* SABS/STEXT/SDATA/SBSS, or SEXT if undefined */
	int value;
	int flags;
	int index;		/* assigned at symbol-table output */
	struct sym *next;
};
#define SF_GLOBL 01
#define SF_DEF 02
#define SF_REG  04		/* symbol aliased to a register (`lp = r5') */

int termreg;			/* set by term/expr when the value is a bare register */

#define NHASH 1023
struct sym *htab[NHASH];

char *infile, *outfile = "a.out";
char *ibuf, *ibufstart, *ibufend;
int errors, pass, lineno;

/* TEXT=0 DATA=1 BSS=2 */
#define NSEG 3
int dot[NSEG];
int curseg;			/* 0/1/2 */
unsigned char *segbuf[NSEG], *relbuf[NSEG];
int segcap[NSEG];

void aerror(s) char *s; {
	fprintf(stderr, "as: %s:%d: %s\n", infile, lineno, s);
	errors++;
}
void *xalloc(n) { void *p = calloc(1,n); if(!p){fprintf(stderr,"as: no memory\n");exit(1);} return p; }

unsigned nhash(b) char *b; { unsigned h=0; int i; for(i=0;i<NCPS;i++) h=h*33+(unsigned char)b[i]; return h%NHASH; }
struct sym *lookup(name) char *name; {
	char nb[NCPS]; int i; struct sym *sp; unsigned h;
	for(i=0;i<NCPS && name[i];i++) nb[i]=name[i];
	for(;i<NCPS;i++) nb[i]=0;
	h=nhash(nb);
	for(sp=htab[h];sp;sp=sp->next) if(memcmp(sp->name,nb,NCPS)==0) return sp;
	sp=(struct sym*)xalloc(sizeof *sp);
	memcpy(sp->name,nb,NCPS); sp->seg=SEXT; sp->index=-1;
	sp->next=htab[h]; htab[h]=sp;
	return sp;
}

/* ---------------- lexer ---------------- */
enum { TEOF=256, TNL, TID, TNUM, TSTR, TLSH, TRSH };	/* \< \> shift operators */
char *ip;
int tok; long tokval; char tokname[64]; struct op *tokkw; char tokstr[1024]; int tokslen;
/* token pushback stack (LIFO) */
struct savedtok { int tok; long val; char name[64]; struct op *kw; };
struct savedtok pbstk[8]; int pbsp;
void pushtok(int tk, long v, char *nm, struct op *kw){
	pbstk[pbsp].tok=tk; pbstk[pbsp].val=v;
	if(nm) strcpy(pbstk[pbsp].name,nm); else pbstk[pbsp].name[0]=0;
	pbstk[pbsp].kw=kw; pbsp++;
}

/* numeric local labels 0..9: each definition `N:' gets a unique mangled
 * symbol "\1N_<count>"; `Nf' refers to the next definition, `Nb' the
 * previous.  loccnt[N] counts definitions seen so far in the current pass. */
int loccnt[10];
char *locname(int dig, int n){ static char b[16]; sprintf(b,"\1%d_%d",dig,n); return b; }

int idchar(c){ return isalnum(c)||c=='_'||c=='.'||c=='~'; }

/* 2BSD as character escapes (the `schar' table in as15.s): \n \s \t \e \0 \r
 * \a \p; any other escaped char is taken literally (`\\' -> `\', etc.). */
int escval(int c){
	switch(c){
	case 'n': return 012; case 's': return 040; case 't': return 011;
	case 'e': return 004; case '0': return 000; case 'r': return 015;
	case 'a': return 006; case 'p': return 033;
	default:  return (unsigned char)c;
	}
}

int lex()
{
	int c;
	if (pbsp) { pbsp--; tok=pbstk[pbsp].tok; tokval=pbstk[pbsp].val; strcpy(tokname,pbstk[pbsp].name); tokkw=pbstk[pbsp].kw; return tok; }
again:
	/* skip blanks; an embedded NUL is whitespace (c1 emits "mov%c" with a
	 * 0 modifier, i.e. "mov\0", for word ops -- the 2BSD as skipped it).
	 * True end-of-input is the buffer end, tracked by ibufend. */
	while (ip<ibufend && (*ip==' '||*ip=='\t'||*ip==0)) ip++;
	if (ip>=ibufend) return tok=TEOF;
	c=*ip;
	if (c==0) return tok=TEOF;
	if (c=='/') { while(*ip&&*ip!='\n')ip++; goto again; }
	if (c=='\n') { ip++; return tok=TNL; }
	if (c==';') { ip++; return tok=';'; }
	if (c=='_'||c=='.'||c=='~'||isalpha(c)) {
		int n=0; struct op*o;
		while (idchar(*ip)) { if(n<63)tokname[n++]=*ip; ip++; }
		tokname[n]=0; tokkw=0;
		/* last match wins, matching 2BSD as's overwriting symbol table:
		 * mul/div/ash appear first as absolute EAE register addresses and
		 * later as the EIS instruction, which is the one that must win */
		for(o=optab;o->name;o++) if(strcmp(o->name,tokname)==0) tokkw=o;
		return tok=TID;
	}
	/* numeric local label reference: <digit>f / <digit>b */
	if (isdigit(c) && (ip[1]=='f'||ip[1]=='b') && !idchar(ip[2])) {
		int dig=c-'0', dir=ip[1]; ip+=2;
		strcpy(tokname, dir=='f' ? locname(dig, loccnt[dig]+1)
					 : locname(dig, loccnt[dig]));
		tokkw=0; return tok=TID;
	}
	if (isdigit(c)) {
		long v=0; int base=8; char *q=ip;
		while(isdigit(*q))q++;
		if(*q=='.') base=10;
		if(c=='0'&&(ip[1]=='x'||ip[1]=='X')){base=16;ip+=2;}
		/* Always consume the whole digit run (8/9 included -- they occur as
		 * local-label digits and as decimal); accumulate in the base.  The
		 * digit must advance ip even in octal, or `9:' loops forever. */
		if(base==16){
			while(isxdigit(*ip)){int d=isdigit(*ip)?*ip-'0':tolower(*ip)-'a'+10; v=v*16+d; ip++;}
		} else {
			while(isdigit(*ip)){ v=v*base+(*ip-'0'); ip++; }
		}
		if(*ip=='.') ip++;
		tokval=v; return tok=TNUM;
	}
	if (c=='\'') { ip++;			/* character constant 'X or '\X */
		if(*ip=='\\'){ ip++; tokval=escval((unsigned char)*ip); }
		else tokval=(unsigned char)*ip;
		if(*ip)ip++; return tok=TNUM; }
	if (c=='"'||c=='<') {
		int term=(c=='"')?'"':'>'; ip++; tokslen=0;
		while(*ip&&*ip!=term){
			int ch=(unsigned char)*ip++;
			if(ch=='\\'&&*ip){ ch=escval((unsigned char)*ip); ip++; }
			if(tokslen<1023)tokstr[tokslen++]=ch;
		}
		if(*ip)ip++;
		return tok=TSTR;
	}
	if(c=='\\' && (ip[1]=='<'||ip[1]=='>')){	/* \< left shift, \> right shift */
		int s=ip[1]; ip+=2; return tok=(s=='<')?TLSH:TRSH;
	}
	if(c=='\\' && ip[1]=='/'){ ip+=2; return tok='/'; }	/* \/ division ('/' is a comment) */
	ip++;
	return tok=c;
}
void unlex(){ pushtok(tok,tokval,tokname,tokkw); }
int peek(){ int t=lex(); unlex(); return t; }

/* ---------------- expressions ---------------- */
struct sym *exsym;		/* set by term/expr when result is external */
long expr();
long term(segp) int *segp; {
	int t=lex(); long v; struct sym *sp;
	exsym=0; *segp=SABS; termreg=0;
	if(t=='-'){ v=term(segp); return -v; }
	if(t=='+'){ return term(segp); }	/* unary plus (e.g. `+2(r0)') */
	if(t=='~'){ v=term(segp); return ~v; }
	if(t=='!'){ v=term(segp); return ~v; }	/* 2BSD as: unary one's complement */
	if(t=='('){ v=expr(segp); if(lex()!=')')aerror("missing )"); return v; }
	if(t=='['){ v=expr(segp); if(lex()!=']')aerror("missing ]"); return v; }	/* 2BSD as: [] grouping */
	if(t==TNUM) return tokval;
	if(t==TID){
		if(strcmp(tokname,".")==0){ *segp=curseg+1; return dot[curseg]; }
		if(strcmp(tokname,"..")==0){ *segp=SABS; return 0; }	/* reloc base placeholder */
		/* register (024) and absolute (01) keywords carry their value */
		if(tokkw&&(tokkw->type==024||tokkw->type==01)){ if(tokkw->type==024)termreg=1; return tokkw->opcode; }
		sp=lookup(tokname);
		if(sp->flags&SF_DEF){ *segp=sp->seg; if(sp->flags&SF_REG)termreg=1; return sp->value; }
		*segp=SEXT; exsym=sp; return 0;
	}
	aerror("bad expression"); return 0;
}
long expr(segp) int *segp; {
	int seg,seg2; long v,v2; struct sym *es;
	int reg;
	v=term(&seg); es=exsym; reg=termreg;	/* a bare register, until an operator applies */
	for(;;){
		int t=peek();
		if(t=='+'||t=='-'||t=='*'||t=='/'||t=='&'||t=='|'||t=='%'||t=='^'||t=='!'||t==TLSH||t==TRSH){
			lex(); v2=term(&seg2); reg=0;
			switch(t){
			/* the segment checks fire only in pass 2: a forward reference is an
			 * undefined SEXT in pass 1 but may resolve to an absolute symbol
			 * defined later (e.g. boot blocks' `$preset+go', preset/go = ...) */
			case '+': v+=v2; if(seg==SABS){seg=seg2;es=exsym;} else if(seg2!=SABS&&pass==2)aerror("non-abs in +"); break;
			case '-': v-=v2; if(seg==seg2&&seg!=SEXT){seg=SABS;es=0;} else if(seg2!=SABS&&pass==2)aerror("bad -"); break;
			case '*': v*=v2; break; case '/': if(v2)v/=v2; break;
			case '%': if(v2)v%=v2; break;
			case '&': v&=v2; break; case '|': v|=v2; break; case '^': v^=v2; break;
			case '!': v+=~v2; break;			/* 2BSD as: a!b = a + ~b */
			case TLSH: v<<=(v2&037); break;			/* \< left shift */
			case TRSH: v=(unsigned long)v>>(v2&037); break;	/* \> right shift (logical) */
			}
		} else break;
	}
	*segp=seg; exsym=es; termreg=reg; return v;
}

/* ---------------- emission ---------------- */
void ensure(seg,n) { if(dot[seg]+n>segcap[seg]){ int nc=(segcap[seg]?segcap[seg]*2:1024); while(dot[seg]+n>nc)nc*=2;
	segbuf[seg]=realloc(segbuf[seg],nc); relbuf[seg]=realloc(relbuf[seg],nc);
	memset(segbuf[seg]+segcap[seg],0,nc-segcap[seg]); memset(relbuf[seg]+segcap[seg],0,nc-segcap[seg]);
	segcap[seg]=nc; } }

/* relocation kind for a value's segment */
int relkind(seg){ switch(seg){case STEXT:return RTEXT;case SDATA:return RDATA;case SBSS:return RBSS;default:return RABS;} }

/* Within an object the address space is unified: text@0, data@txtsize,
 * bss@txtsize+datsize (the authentic as's datbase/bssbase, as21.s).  Symbol
 * values and internal references are emitted in this unified space; ld's
 * cdrel/cbrel back out the bias when combining objects.  Set after pass 1. */
int txtsize, datsize;
int segbase(seg){ switch(seg){case SDATA:return txtsize;case SBSS:return txtsize+datsize;default:return 0;} }

void emitword(w, seg, sym, pcrel)
struct sym *sym;
{
	int s=curseg; int rel;
	if(pass==2){
		ensure(s,2);
		segbuf[s][dot[s]]=w&0377; segbuf[s][dot[s]+1]=(w>>8)&0377;
		if(seg==SEXT && sym) rel = (sym->index<<4)|REXT|(pcrel?RPCREL:0);
		else rel = relkind(seg)|(pcrel?RPCREL:0);
		relbuf[s][dot[s]]=rel&0377; relbuf[s][dot[s]+1]=(rel>>8)&0377;
	}
	dot[s]+=2;
}
void emitbyte(b){ int s=curseg; if(pass==2){ ensure(s,1); segbuf[s][dot[s]]=b&0377; relbuf[s][dot[s]]=0; } dot[s]++; }

/* emit an instruction's extra (offset) word from an operand.  Defined
 * data/bss targets are biased into the unified object address space; the
 * current location (for pc-relative) is biased by the current segment. */
void emitextra(xval,xseg,xsym,pcrel)
struct sym *xsym;
{
	long v = xval + segbase(xseg);
	if(pcrel){
		/* X(pc): stored = target - (address of the word after this one) */
		v = v - (dot[curseg] + segbase(curseg+1) + 2);
		emitword(v & 0177777, (xseg==SABS?SABS:xseg), xsym, 1);
	} else {
		emitword(v & 0177777, xseg, xsym, 0);
	}
}

/* ---------------- operand parsing ----------------
 * Fills mode (6-bit), and (if any) an extra word.
 */
struct operand { int mode; int hasx; long xval; int xseg; struct sym *xsym; int pcrel; };

int regof(){
	if(tok==TID && tokkw && tokkw->type==024) return tokkw->opcode;	/* r0..r5/sp/pc */
	if(tok==TID && !tokkw){						/* a symbol aliased to a register */
		struct sym *sp=lookup(tokname);
		if((sp->flags&(SF_DEF|SF_REG))==(SF_DEF|SF_REG)) return sp->value & 7;
	}
	return -1;
}

void getop(o)
struct operand *o;
{
	int t, defer=0, reg;
	/* the first operand token, saved so peek() below cannot clobber it */
	int s_tok; long s_val; char s_name[64]; struct op *s_kw;
	o->mode=0;o->hasx=0;o->xval=0;o->xseg=SABS;o->xsym=0;o->pcrel=0;
	t=lex();
	if(t=='*'){ defer=1; t=lex(); }
	if(t=='$'){				/* immediate / absolute */
		o->mode = defer?037:027;
		o->hasx=1; o->xval=expr(&o->xseg); o->xsym=exsym;
		return;
	}
	s_tok=t; s_val=tokval; strcpy(s_name,tokname); s_kw=tokkw;
	if(t=='('){
		t=lex(); reg=regof(); if(reg<0){aerror("bad register");reg=0;}
		if(lex()!=')')aerror("missing )");
		if(peek()=='+'){ lex(); o->mode=(defer?3:2)*010+reg; }
		else if(defer){
			/* *(rn) means M[M[rn]] with NO side effect.  There is no
			 * single-word mode for that; it is mode 7 (index deferred)
			 * with a zero index word -- *0(rn).  Encoding it as mode 3
			 * (@(rn)+) wrongly auto-increments rn (corrupted putc). */
			o->mode=7*010+reg; o->hasx=1; o->xval=0; o->xseg=SABS;
		}
		else o->mode=1*010+reg;			/* (rn) = mode 1 = M[rn] */
		return;
	}
	if(t=='-' && peek()=='('){
		lex(); t=lex(); reg=regof(); if(reg<0){aerror("bad register");reg=0;}
		if(lex()!=')')aerror("missing )");
		o->mode=(defer?5:4)*010+reg;
		return;
	}
	if(t==TID && s_kw && s_kw->type==024 && peek()!='('){
		o->mode=(defer?1:0)*010+s_kw->opcode;	/* Rn ; *Rn=(Rn) */
		return;
	}
	/* expression: index X(rn) or pc-relative reference.  Push the saved
	 * first token back (NOT unlex, which would re-push whatever peek last
	 * looked at) so expr() sees the real start of the operand. */
	pushtok(s_tok, s_val, s_tok==TID?s_name:0, s_kw);
	{ int seg; long v; struct sym *sym;
	  v=expr(&seg); sym=exsym;
	  if(peek()=='('){
		lex(); t=lex(); reg=regof(); if(reg<0){aerror("bad register");reg=0;}
		if(lex()!=')')aerror("missing )");
		o->mode=(defer?7:6)*010+reg;
		o->hasx=1;o->xval=v;o->xseg=seg;o->xsym=sym;
		return;
	  }
	  o->mode=(defer?7:6)*010+7;		/* X(pc) relative */
	  o->hasx=1;o->xval=v;o->xseg=seg;o->xsym=sym;o->pcrel=1;
	}
}
void putop(o) struct operand*o; {
	if(o->hasx) emitextra(o->xval,o->xseg,o->xsym,o->pcrel);
}

/* ---------------- statement assembly ---------------- */
void setseg(s){ curseg=s; }

/* size of one operand's extra word (0 or 2) -- known from syntax in pass1 */
/* we just emit; dot advances identically in both passes */

void doublop(base){ struct operand s,d; getop(&s); if(lex()!=',')aerror("missing ,"); getop(&d);
	emitword(base|((s.mode&077)<<6)|(d.mode&077),SABS,0,0); putop(&s); putop(&d); }
void singlop(base){ struct operand d; getop(&d); emitword(base|(d.mode&077),SABS,0,0); putop(&d); }
void jsrop(base){ struct operand r,d; getop(&r); if(lex()!=',')aerror("missing ,"); getop(&d);
	emitword(base|((r.mode&07)<<6)|(d.mode&077),SABS,0,0); putop(&d); }
/* FP11 floating-point operand forms.  A float accumulator (ac0-ac3, written
 * r0-r3) lands in bits 6-7; the other operand is a general addressing mode in
 * bits 0-5 (with its extra word, if any). */
void flop14(base){ struct operand s,d;	/* `op fsrc,freg' (addf/cmpf/movof/...) */
	getop(&s); if(lex()!=',')aerror("missing ,"); getop(&d);
	emitword(base|((d.mode&07)<<6)|(s.mode&077),SABS,0,0); putop(&s); putop(&d); }
void flop5(base){ struct operand s,d;	/* `op freg,dst' (movfo/movfi/movei) */
	getop(&s); if(lex()!=',')aerror("missing ,"); getop(&d);
	emitword(base|((s.mode&07)<<6)|(d.mode&077),SABS,0,0); putop(&s); putop(&d); }
void movfop(base){ struct operand s,d;	/* movf: LDF mem,freg | STF freg,mem */
	getop(&s); if(lex()!=',')aerror("missing ,"); getop(&d);
	if((s.mode&077) < 4)		/* source is a float register -> STF */
		emitword(0174000|((s.mode&07)<<6)|(d.mode&077),SABS,0,0);
	else				/* source is memory -> LDF */
		emitword(base|((d.mode&07)<<6)|(s.mode&077),SABS,0,0);
	putop(&s); putop(&d); }
void rtsop(base){ struct operand r; getop(&r); emitword(base|(r.mode&07),SABS,0,0); }
void sysop(base){ int seg; long v=expr(&seg); emitword(base|(v&077),SABS,0,0); }
/* emt [code] -- the operand is optional (bare `emt' == `emt 0'); 8-bit code */
void emtop(base){ int seg, t=peek(); long v=0;
	if(t!=TNL && t!=';' && t!=TEOF) v=expr(&seg);
	emitword(base|(v&0377),SABS,0,0); }
void branchop(base){ int seg; long v=expr(&seg); long off=(v-(dot[curseg]+2))/2;
	if(pass==2 && (off<-128||off>127)) aerror("branch out of range");
	emitword(base|(off&0377),SABS,0,0); }
void eisop(base){ struct operand s,d; getop(&s); if(lex()!=',')aerror("missing ,"); getop(&d);
	/* src first then reg: opcode | reg<<6 | src */
	emitword(base|((d.mode&07)<<6)|(s.mode&077),SABS,0,0); putop(&s); }
void sobop(base){ struct operand r; int seg; long v;
	getop(&r); if(lex()!=',')aerror("missing ,"); v=expr(&seg);
	{ long off=(dot[curseg]+2-v)/2; emitword(base|((r.mode&07)<<6)|(off&077),SABS,0,0); } }
/* jbr addr -> jmp addr (always long) */
void jbrop(){ struct operand d; getop(&d); emitword(0000100|(d.mode&077),SABS,0,0); putop(&d); }
/* jxxx addr -> b<not-cond> .+6 ; jmp addr   (always long) */
void jxxxop(brbase){ struct operand d; int comp=brbase^0400;
	getop(&d);
	emitword(comp|2,SABS,0,0);		/* skip the 2-word jmp when cond false */
	emitword(0000100|(d.mode&077),SABS,0,0); putop(&d);
}

void dobyte(){ int seg; for(;;){ if(peek()==TSTR){lex(); int i;for(i=0;i<tokslen;i++)emitbyte(tokstr[i]);}
	else { long v=expr(&seg); emitbyte(v&0377);} if(peek()==',') lex(); else break; } }
void doascii(){ if(lex()==TSTR){ int i; for(i=0;i<tokslen;i++) emitbyte(tokstr[i]); } else aerror("bad .ascii"); }
void doglobl(){ for(;;){ if(peek()!=TID) return;	/* bare .globl is a no-op separator */
	lex(); lookup(tokname)->flags|=SF_GLOBL;
	if(peek()==',')lex(); else break; } }
void docomm(){ struct sym*sp; int seg; long sz; if(lex()!=TID){aerror(".comm name");return;} sp=lookup(tokname);
	if(lex()!=',')aerror(".comm ,"); sz=expr(&seg); sp->flags|=SF_GLOBL; if(!(sp->flags&SF_DEF)){sp->value=sz;} }
void even(){ if(dot[curseg]&1){ emitbyte(0);} }
void doword(long v,int seg,struct sym*sym){ emitword((v+segbase(seg))&0177777,seg,sym,0); }

void assemble()
{
	int t;
	pass = pass;
	for(;;){
		t=lex();
		if(t==TEOF) break;
		if(t==TNL){ lineno++; continue; }
		if(t==';') continue;
		if(t==TSTR){ int i; for(i=0;i<tokslen;i++) emitbyte(tokstr[i]); continue; }  /* bare string = .ascii */
		if(t==TID){
			char name[64]; struct op*kw; int t2;
			strcpy(name,tokname); kw=tokkw;
			t2 = lex();		/* the token after the identifier */
			/* label?  name:   */
			if(t2==':'){
				if(strcmp(name,".")!=0){
					struct sym*sp=lookup(name);
					if((sp->flags&SF_DEF)&&pass==1) aerror("redefined symbol");
					sp->flags|=SF_DEF; sp->seg=curseg+1; sp->value=dot[curseg];
				}
				continue;
			}
			/* assignment?  name = expr  */
			if(t2=='='){
				int seg; long v; v=expr(&seg);
				if(strcmp(name,".")==0){
					/* set the location counter.  `.=.+N' reserves N bytes
					 * (e.g. the uninitialised tail of a partly-initialised
					 * array); emit zero fill so following data isn't placed
					 * on top of the reserved space. */
					while(dot[curseg] < v) emitbyte(0);
					if(dot[curseg] > v) dot[curseg]=v;	/* backward (rare) */
				} else { struct sym*sp=lookup(name); sp->flags|=SF_DEF; sp->seg=seg; sp->value=v;
					if(termreg) sp->flags|=SF_REG; else sp->flags&=~SF_REG;	/* `lp = r5' */ }
				continue;
			}
			unlex();		/* push t2 back: it starts the operands/expression */
			if(kw){
				switch(kw->type){
				case 01:  /* absolute no-operand insn: setd/clc/sec/cfcc... */
					  emitword(kw->opcode,SABS,0,0); break;
				case 013: doublop(kw->opcode); break;
				case 015: singlop(kw->opcode); break;
				case 05:  flop5(kw->opcode); break;   /* movfo freg,dst */
				case 012: movfop(kw->opcode); break;  /* movf (ld/st) */
				case 014: flop14(kw->opcode); break;  /* addf fsrc,freg */
				case 07:  if(strcmp(name,"xor")==0){ /* xor: src,reg like jsr fields */
						struct operand s,d; getop(&s); if(lex()!=',')aerror(",");
						getop(&d); emitword(kw->opcode|((d.mode&07)<<6)|(s.mode&077),SABS,0,0); putop(&s);
					  } else jsrop(kw->opcode); break;
				case 010: rtsop(kw->opcode); break;
				case 011: sysop(kw->opcode); break;
				case 041: emtop(kw->opcode); break;
				case 06:  branchop(kw->opcode); break;
				case 030: eisop(kw->opcode); break;
				case 031: sobop(kw->opcode); break;
				case 035: jbrop(); break;
				case 036: jxxxop(kw->opcode); break;
				case 016: dobyte(); break;
				case 017: doascii(); break;
				case 020: even(); break;
				case 021: case 022: /* .if/.endif: skip rest of line */
					  while(peek()!=TNL&&peek()!=TEOF&&peek()!=';')lex(); break;
				case 023: doglobl(); break;
				case 025: setseg(0); break;
				case 026: setseg(1); break;
				case 027: setseg(2); break;
				case 032: docomm(); break;
				case 024: aerror("register used as instruction"); break;
				default:  aerror("unsupported directive"); break;
				}
				continue;
			}
			/* bare identifier expression -> emit a word.  t2 is already
			 * pushed back; push `name' in front of it so expr() sees the
			 * identifier first. */
			pushtok(TID, 0, name, kw);
			{ int seg; long v=expr(&seg); doword(v,seg,exsym); }
			continue;
		}
		if(t==TNUM){
			long saved=tokval;	/* peek() below clobbers the token state */
			/* numeric local label definition: `N:' (digit 0..9 then colon) */
			if(saved>=0 && saved<=9 && peek()==':'){
				struct sym *sp;
				lex();			/* consume ':' */
				sp=lookup(locname(saved, ++loccnt[saved]));
				sp->flags|=SF_DEF; sp->seg=curseg+1; sp->value=dot[curseg];
				continue;
			}
			/* bare numeric expression -> word(s).  peek() (if it ran) left
			 * the following token on the pushback stack; push the number in
			 * front of it so expr() sees the number first. */
			pushtok(TNUM, saved, 0, 0);
			{ int seg; long v=expr(&seg); doword(v,seg,exsym); }
			continue;
		}
		/* bare expression statement -> word(s) */
		unlex();
		{ int seg; long v=expr(&seg); doword(v,seg,exsym); }
	}
}

/* ---------------- output ---------------- */
void putw_(FILE*f,int w){ putc(w&0377,f); putc((w>>8)&0377,f); }

void writeout()
{
	FILE *f; struct sym *sp; int i, nsym=0, ssize;
	/* segments are even-sized in the file (data must start at txtsize); the
	 * pad bytes are already zero (segbuf/relbuf are zeroed on growth) */
	int tsize=(dot[0]+1)&~1, dsize=(dot[1]+1)&~1, bsize=(dot[2]+1)&~1;
	ensure(0,2); ensure(1,2);
	/* assign symbol indices: globals + defined locals (skip undefined-but-unreferenced) */
	int idx=0;
	for(i=0;i<NHASH;i++) for(sp=htab[i];sp;sp=sp->next){
		if(((sp->flags&SF_GLOBL) || (sp->flags&SF_DEF) || sp->seg==SEXT) && sp->name[0]!=1){ sp->index=idx++; nsym++; }
	}
	ssize = nsym*12;
	if(!(f=fopen(outfile,"w"))){ perror(outfile); exit(1); }
	/* header (8 words): magic, tsize, dsize, bsize, ssize, entry, unused, relflg */
	putw_(f,0407); putw_(f,tsize); putw_(f,dsize); putw_(f,bsize);
	putw_(f,ssize); putw_(f,0); putw_(f,0); putw_(f,0);	/* relflg 0 => reloc present */
	fwrite(segbuf[0],1,tsize,f);
	fwrite(segbuf[1],1,dsize,f);
	fwrite(relbuf[0],1,tsize,f);
	fwrite(relbuf[1],1,dsize,f);
	/* symbols: name[8], n_type, n_ovly, value */
	for(i=0;i<NHASH;i++) for(sp=htab[i];sp;sp=sp->next){
		int nt;
		/* numeric local labels (\001N_M) resolve in-memory like 2BSD's
		 * curfb table -- they are never written to the .o symbol table */
		if(!((sp->flags&SF_GLOBL)||(sp->flags&SF_DEF)||sp->seg==SEXT) || sp->name[0]==1) continue;
		if(sp->flags&SF_DEF){ switch(sp->seg){case STEXT:nt=N_TEXT;break;case SDATA:nt=N_DATA;break;
			case SBSS:nt=N_BSS;break;default:nt=N_ABS;} }
		else nt=N_UNDF;
		/* An undefined symbol that is referenced is an external reference
		 * (the ld here resolves only EXTERN+UNDEF via REXT relocations); so
		 * mark it external even without an explicit .globl. */
		if((sp->flags&SF_GLOBL) || !(sp->flags&SF_DEF)) nt|=N_EXT;
		fwrite(sp->name,1,8,f);
		putc(nt,f); putc(0,f);
		/* emit the value in the unified object address space (data biased
		 * by txtsize, bss by txtsize+datsize), like the authentic as */
		putw_(f, (sp->value + segbase(sp->seg)) & 0177777);
	}
	fclose(f);
}

main(argc,argv)
char**argv;
{
	int i; long flen; FILE*f;
	/* collect input files: as concatenates several (e.g. the syscall stubs
	 * are assembled as `as -o x.o /usr/include/sys.s x.s`) */
	{
		char *files[64]; long flens[64]; int nf=0;
		long total=0; char *p;
		for(i=1;i<argc;i++){
			if(argv[i][0]=='-'){
				if(argv[i][1]=='o' && i+1<argc) outfile=argv[++i];
				else if(argv[i][1]=='u') ; /* undefined->external; default */
				else { /* other flags ignored */ }
			} else if(nf<64) files[nf++]=argv[i];
		}
		if(nf==0){ fprintf(stderr,"as: no input file\n"); return 1; }
		infile=files[0];
		/* read every file (exact bytes -- c1 output has embedded NULs),
		 * separated by a newline, into one buffer */
		for(i=0;i<nf;i++){
			if(!(f=fopen(files[i],"r"))){ perror(files[i]); return 1; }
			fseek(f,0,2); flen=ftell(f); fseek(f,0,0);
			p=xalloc(flen+1); flen=fread(p,1,flen,f); fclose(f);
			files[i]=p; flens[i]=flen;
			total += flen + 1;
		}
		ibuf=xalloc(total+1); p=ibuf;
		for(i=0;i<nf;i++){ memcpy(p,files[i],flens[i]); p+=flens[i]; *p++='\n'; }
		*p=0;
		ibufstart=ibuf; ibufend=p;
	}

	/* pass 1: assign addresses */
	pass=1; ip=ibufstart; curseg=0; dot[0]=dot[1]=dot[2]=0; lineno=1; pbsp=0; memset(loccnt,0,sizeof loccnt);
	assemble();
	/* segment sizes are now known (even-rounded, as ld rounds them); the
	 * data/bss base used to bias values in the unified object space */
	txtsize = (dot[0]+1) & ~1;
	datsize = (dot[1]+1) & ~1;
	/* assign symbol-table indices now, BEFORE pass 2 emits relocation
	 * words: emitword() stamps sym->index into the external relocation
	 * word, so the index must already match the symbol's position in the
	 * symbol table writeout() emits (same iteration order/predicate). */
	{ int h; struct sym *sp; int idx=0;
	  for(h=0;h<NHASH;h++) for(sp=htab[h];sp;sp=sp->next)
		if(((sp->flags&SF_GLOBL)||(sp->flags&SF_DEF)||sp->seg==SEXT) && sp->name[0]!=1)
			sp->index=idx++;
	}
	/* pass 2: emit */
	pass=2; ip=ibufstart; curseg=0; dot[0]=dot[1]=dot[2]=0; lineno=1; pbsp=0; memset(loccnt,0,sizeof loccnt);
	assemble();
	if(errors){ fprintf(stderr,"as: %d error(s)\n",errors); return 1; }
	writeout();
	return 0;
}
