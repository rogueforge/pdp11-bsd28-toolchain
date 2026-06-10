/*
 * das -- a disassembler for 2.8BSD PDP-11 a.out, object (.o), and archive (.a)
 * files.  The inverse of this toolchain's `as'.
 *
 *  - A bare object (.o) disassembles to one listing.
 *  - A linked a.out is split back into per-object listings using the N_FN
 *    file-name symbols `ld' leaves in the symbol table (each marks where an
 *    input object's text begins).
 *  - An archive (.a) disassembles each member to its own listing.
 *
 * Available debugging symbols label functions (text), variables (data/bss),
 * and branch/call targets.  Output goes to <stem>.<object>.dis files (or
 * stdout with -p), one per object.
 *
 * Runs on the LP64 host; reads the 16-bit little-endian PDP-11 formats
 * explicitly (never by struct overlay), so it is endian/word-size clean.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "a.out.h"
#include "ar.h"

/* ---- the file being disassembled ---------------------------------------- */
static unsigned char *F;	/* whole input file */
static long FLEN;

static int w16(long off){	/* little-endian 16-bit word at byte offset */
	if(off<0 || off+1>=FLEN) return 0;
	return F[off] | (F[off+1]<<8);
}

/* ---- symbols ------------------------------------------------------------ */
struct sym { char name[12]; int type; int value; };
static struct sym *Sym;
static int NSym;

#define BASETYPE(t)	((t) & 037)		/* segment, masking the EXT bit */
#define ISEXT(t)	((t) & 040)

/* output mode + segment/relocation geometry (file offsets) of the current
 * object, so the operand formatter can turn relocated words into symbols. */
static int Asm;					/* -a: emit reassemblable source */
static long Tbase, Dbase, RTbase, RDbase;	/* text/data + their reloc areas */
static int Tsize, Dsize, HasReloc;

/* on-disk relocation type field (matches as.c) */
#define RABS 000
#define REXT 010

/* the relocation word for the operand/data word at file offset `wo' (0 none) */
static int relat(long wo)
{
	if(!HasReloc) return 0;
	if(wo>=Tbase && wo<Tbase+Tsize) return w16(RTbase + (wo-Tbase));
	if(wo>=Dbase && wo<Dbase+Dsize) return w16(RDbase + (wo-Dbase));
	return 0;
}
/* if the word at `wo' relocates against an EXTERNAL symbol, format it with
 * `fmt' (one %s for the name) into `out' and return 1; else 0. */
static int relexp(long wo, int addend, char *fmt, char *out)
{
	int rel=relat(wo);
	if((rel&016)==REXT && (rel>>4)<NSym){
		char s[48];
		if(addend) sprintf(s, "%s+%o", Sym[rel>>4].name, addend&0177777);
		else       strcpy(s, Sym[rel>>4].name);
		sprintf(out, fmt, s);
		return 1;
	}
	return 0;
}

/* `as' mangles a numeric local label (1:) into a symbol named "\001<num>_
 * <instance>".  Reconstruct it: a definition prints as "<num>:", a reference
 * as "<num>f" or "<num>b" (forward/backward from the referencing address) --
 * which `as' re-mangles to the identical symbol, so it round-trips. */
static int islocal(char *name){ return (unsigned char)name[0]==1; }
static char *locname(char *name, int suffix)
{
	static char b[16];
	int n=0; char *p;
	if(!islocal(name)) return name;
	for(p=name+1; *p>='0' && *p<='9'; p++) n=n*10+(*p-'0');
	if(suffix) sprintf(b,"%d%c",n,suffix); else sprintf(b,"%d",n);
	return b;
}
static char *locref(char *name, int targ, int ref)	/* Nf / Nb */
{ return name ? locname(name, (targ>ref)?'f':'b') : name; }

static int Iaddr;	/* address of the instruction currently being decoded */

/* Best label for an address in a given segment (N_TEXT/N_DATA/N_BSS base).
 * Prefers an exact match, external over local; returns 0 if none. */
static char *labelat(int addr, int seg)
{
	int i, best=-1;
	for(i=0;i<NSym;i++){
		if(BASETYPE(Sym[i].type)!=seg) continue;
		if(Sym[i].value!=addr) continue;
		if(best<0 || (ISEXT(Sym[i].type) && !ISEXT(Sym[best].type))) best=i;
	}
	return best<0 ? 0 : locref(Sym[best].name, addr, Iaddr);
}

/* ---- instruction decoder ------------------------------------------------
 * Decode one instruction starting at text offset `o' (its address is `addr').
 * Append the assembly to `buf'.  Returns the instruction length in bytes.
 * tbase/dbase: file offsets of the text/data segments, so index/immediate
 * words can be fetched; addr is the PDP-11 address for PC-relative targets. */
static char brmne[16][6] = {	/* 0000400..0003400 and 0100000..0103400 */
	"","br","bne","beq","bge","blt","bgt","ble",
	"bpl","bmi","bhi","blos","bvc","bvs","bcc","bcs" };
static char sopmne[16][6] = {	/* single-operand 0005000..0006700 */
	"clr","com","inc","dec","neg","adc","sbc","tst",
	"ror","rol","asr","asl","mark","mfpi","mtpi","sxt" };
static char dopmne[8][5] = { "", "mov","cmp","bit","bic","bis","add","" };
static char fpmne[8][6]  = { "","mulf","modf","addf","movf","subf","cmpf","" };

/* format a 6-bit operand; *po is the running text offset just past the opcode
 * word, advanced over any index/immediate word. *paddr tracks the PDP-11 PC. */
static char *regname(int r)	/* pc/sp idioms for r7/r6 */
{
	static char b[4];
	if(r==7) return "pc";
	if(r==6) return "sp";
	sprintf(b, "r%d", r);
	return b;
}

static void fmtop(int spec, long *po, int *paddr, char *out)
{
	int mode=(spec>>3)&7, reg=spec&7, x, targ;
	char *rn = reg==7?"pc":reg==6?"sp":reg==5?"r5":0;
	char rb[4];
	if(!rn){ sprintf(rb,"r%d",reg); rn=rb; }
	switch(mode){
	char *l; long wo;
	case 0: strcpy(out,rn); break;
	case 1: sprintf(out,"(%s)",rn); break;
	case 2:
		if(reg==7){ wo=*po; x=w16(*po); *po+=2; *paddr+=2;	/* $imm */
			if(relexp(wo,x,"$%s",out)) break;
			sprintf(out,"$%o",x); }
		else sprintf(out,"(%s)+",rn);
		break;
	case 3:
		if(reg==7){ wo=*po; x=w16(*po); *po+=2; *paddr+=2;	/* @#abs */
			if(relexp(wo,x,"*$%s",out)) break;
			l=labelat(x,N_TEXT); if(!l)l=labelat(x,N_DATA); if(!l)l=labelat(x,N_BSS);
			if(l)sprintf(out,"*$%s",l); else sprintf(out,"*$%o",x); }
		else sprintf(out,"*(%s)+",rn);
		break;
	case 4: sprintf(out,"-(%s)",rn); break;
	case 5: sprintf(out,"*-(%s)",rn); break;
	case 6:
		wo=*po; x=w16(*po); *po+=2; *paddr+=2;
		if(reg==7){					/* PC-relative */
			if(relexp(wo,(x+*paddr)&0xffff,"%s",out)) break;
			targ=(*paddr + (short)x)&0xffff;
			l=labelat(targ,N_TEXT); if(!l)l=labelat(targ,N_DATA); if(!l)l=labelat(targ,N_BSS);
			if(l)sprintf(out,"%s",l); else sprintf(out,"%o",targ); }
		else sprintf(out,"%o(%s)",x&0177777,rn);
		break;
	case 7:
		wo=*po; x=w16(*po); *po+=2; *paddr+=2;
		if(reg==7){
			if(relexp(wo,(x+*paddr)&0xffff,"*%s",out)) break;
			targ=(*paddr + (short)x)&0xffff;
			l=labelat(targ,N_DATA); if(!l)l=labelat(targ,N_TEXT);
			if(l)sprintf(out,"*%s",l); else sprintf(out,"*%o",targ); }
		else sprintf(out,"*%o(%s)",x&0177777,rn);
		break;
	}
}

static int decode(long o, int addr, char *buf)
{
	int instr=w16(o), op, a1, a2;
	Iaddr=addr;
	long po=o+2;			/* offset just past the opcode word */
	int adr=addr+2;			/* PC value while reading index words */
	char o1[32], o2[32];
	int b=(instr>>15)&1;		/* byte bit (for double/single op groups) */

	/* no-operand */
	switch(instr){
	case 0: strcpy(buf,"halt"); return 2;
	case 1: strcpy(buf,"wait"); return 2;
	case 2: strcpy(buf,"rti");  return 2;
	case 3: strcpy(buf,"bpt");  return 2;
	case 4: strcpy(buf,"iot");  return 2;
	case 5: strcpy(buf,"reset");return 2;
	case 6: strcpy(buf,"rtt");  return 2;
	}
	if((instr&0177770)==0000200){ sprintf(buf,"rts\t%s",regname(instr&7)); return 2; }
	if((instr&0177700)==0000100){ fmtop(instr&077,&po,&adr,o1); sprintf(buf,"jmp\t%s",o1); return po-o; }
	if((instr&0177700)==0000300){ fmtop(instr&077,&po,&adr,o1); sprintf(buf,"swab\t%s",o1); return po-o; }
	if((instr&0177000)==0004000){	/* jsr reg,dst  (0004000..0004777) */
		fmtop(instr&077,&po,&adr,o1);
		sprintf(buf,"jsr\t%s,%s",regname((instr>>6)&7),o1); return po-o; }
	if((instr&0177400)>=0000240 && (instr&0177400)<0000400 && (instr&0400)==0){
		/* condition-code ops (nop / clc / sec / ...) */
		static char *cc="cvzn";
		if(instr==0240){ strcpy(buf,"nop"); return 2; }
		{ int set=(instr&020)!=0, i; char *p=buf;
		  p+=sprintf(p,"%s",set?"se":"cl");
		  for(i=0;i<4;i++) if(instr&(1<<i)) *p++=cc[i];
		  *p=0; return 2; }
	}
	/* branches */
	if(((instr&0177400)>=0000400 && (instr&0177400)<=0003400) ||
	   ((instr&0177400)>=0100000 && (instr&0177400)<=0103400)){
		int idx=((instr>>8)&07) | (((instr>>15)&1)<<3);
		int off=(signed char)(instr&0377), targ=(addr+2+2*off)&0xffff;
		char *l=labelat(targ,N_TEXT);
		if(l) sprintf(buf,"%s\t%s",brmne[idx],l);
		else  sprintf(buf,"%s\t%o",brmne[idx],targ);
		return 2;
	}
	/* sys / emt / trap */
	if((instr&0177400)==0104400){ sprintf(buf,"sys\t%o",instr&0377); return 2; }
	if((instr&0177400)==0104000){ sprintf(buf,"emt\t%o",instr&0377); return 2; }

	op=(instr>>12)&017;

	/* FP11 (017xxxx): just the common arithmetic/move forms + no-operand */
	if(op==017){
		switch(instr){
		case 0170000: strcpy(buf,"cfcc"); return 2;
		case 0170001: strcpy(buf,"setf"); return 2;
		case 0170011: strcpy(buf,"setd"); return 2;
		case 0170002: strcpy(buf,"seti"); return 2;
		case 0170012: strcpy(buf,"setl"); return 2;
		}
		{ int g=(instr>>8)&017, ac=(instr>>6)&3; char *m;
		  /* indexed by bits 11-8; the store forms (idx 8,10,11,12) take
		   * `freg,dst', the load/arith forms take `src,freg'. */
		  static char *fp2[16]={0,0,"mulf","modf","addf","movf","subf","cmpf",
					"movf","divf","movei","movfi","movfo","movie","movif","movof"};
		  if((instr&0177700)>=0170400 && (instr&0177700)<=0170700){
			static char *fp1[4]={"clrf","tstf","absf","negf"};
			fmtop(instr&077,&po,&adr,o1);
			sprintf(buf,"%s\t%s",fp1[((instr>>6)&3)],o1); return po-o; }
		  m=fp2[g];
		  if(m){ fmtop(instr&077,&po,&adr,o1);
			 if(g==8||g==10||g==11||g==12) sprintf(buf,"%s\tfr%d,%s",m,ac,o1);
			 else                          sprintf(buf,"%s\t%s,fr%d",m,o1,ac);
			 return po-o; }
		}
		sprintf(buf,"%o", instr); return 2;
	}
	/* double-operand: 1..6 word, 11..16 byte (16/116 == sub) */
	if((op>=1&&op<=6)||(op>=011&&op<=016)){
		int bop=op&07;
		char *m = (op==016) ? "sub" : dopmne[bop];
		char mb[6]; strcpy(mb,m); if(b && op!=016) strcat(mb,"b");
		fmtop((instr>>6)&077,&po,&adr,o1);
		fmtop(instr&077,&po,&adr,o2);
		sprintf(buf,"%s\t%s,%s",mb,o1,o2);
		return po-o;
	}
	/* EIS / jsr / sob / xor (op 07 and op 004) */
	if(op==07){
		int reg=(instr>>6)&7;
		switch(instr&0177000){
		case 0070000: fmtop(instr&077,&po,&adr,o1); sprintf(buf,"mul\t%s,%s",o1,regname(reg)); return po-o;
		case 0071000: fmtop(instr&077,&po,&adr,o1); sprintf(buf,"div\t%s,%s",o1,regname(reg)); return po-o;
		case 0072000: fmtop(instr&077,&po,&adr,o1); sprintf(buf,"ash\t%s,%s",o1,regname(reg)); return po-o;
		case 0073000: fmtop(instr&077,&po,&adr,o1); sprintf(buf,"ashc\t%s,%s",o1,regname(reg)); return po-o;
		case 0074000: fmtop(instr&077,&po,&adr,o1); sprintf(buf,"xor\t%s,%s",regname(reg),o1); return po-o;
		case 0077000: { int off=instr&077, targ=(addr+2-2*off)&0xffff;
				char *l=labelat(targ,N_TEXT);
				if(l)sprintf(buf,"sob\t%s,%s",regname(reg),l); else sprintf(buf,"sob\t%s,%o",regname(reg),targ);
				return 2; }
		}
	}
	/* single-operand group 0005000..0006700 (+ byte 0105000..) */
	if((instr&0077000)==0005000 || (instr&0077000)==0006000){
		int g=((instr>>6)&077)-050;	/* opcode is bits 6-11 */
		char mb[6]; strcpy(mb,sopmne[g]);
		/* mark/mfpi/mtpi/sxt (06400..06700) are not byte-modified */
		if(b && g<014) strcat(mb,"b");
		fmtop(instr&077,&po,&adr,o1);
		sprintf(buf,"%s\t%s",mb,o1);
		return po-o;
	}
	(void)a1; (void)a2;
	sprintf(buf,"%o", instr);
	return 2;
}

/* ---- listing generation ------------------------------------------------- */

static void readsyms(long symoff, int symsize)
{
	int n=symsize/12, i, j;
	free(Sym);
	Sym=malloc((n+1)*sizeof(struct sym)); NSym=0;
	for(i=0;i<n;i++){
		long r=symoff+i*12;
		if(r+12>FLEN) break;
		for(j=0;j<8;j++) Sym[NSym].name[j]=F[r+j];
		Sym[NSym].name[8]=0;
		for(j=7;j>=0 && (Sym[NSym].name[j]==' '||Sym[NSym].name[j]==0);j--)
			Sym[NSym].name[j]=0;
		Sym[NSym].type=F[r+8];
		Sym[NSym].value=w16(r+10);
		NSym++;
	}
}

/* emit every label defined at `addr' in segment `seg' */
static void labels(int addr, int seg, FILE *out)
{
	int i;
	for(i=0;i<NSym;i++)
		if(Sym[i].value==addr && BASETYPE(Sym[i].type)==seg && Sym[i].name[0])
			fprintf(out, "%s:\n", locname(Sym[i].name,0));
}

/* disassemble the text in PDP-11 address range [a0,a1); tbase = file offset
 * of address 0 of the text segment. */
static void disasm_text(long tbase, int a0, int a1, FILE *out)
{
	int addr=a0;
	char buf[120];
	while(addr<a1){
		int len, i;
		labels(addr, N_TEXT, out);
		len=decode(tbase+addr, addr, buf);
		if(Asm){ fprintf(out, "\t%s\n", buf); addr+=len; continue; }
		fprintf(out, "\t%06o:  ", addr);
		for(i=0;i<len;i+=2) fprintf(out, "%06o ", w16(tbase+addr+i));
		for(i=len;i<6;i+=2) fprintf(out, "       ");
		fprintf(out, "  %s\n", buf);
		addr+=len;
	}
}

/* dump the data segment [a0,a0+size) as labelled words; dbase = file offset of
 * address a0. */
static void disasm_data(long dbase, int a0, int size, FILE *out)
{
	int addr=a0, end=a0+size;
	char sym[40];
	while(addr<end){
		long wo=dbase+(addr-a0);
		labels(addr, N_DATA, out);
		if(Asm){
			if(relexp(wo,w16(wo),"%s",sym)) fprintf(out, "\t%s\n", sym);
			else fprintf(out, "\t%o\n", w16(wo));
		} else
			fprintf(out, "\t%06o:  %06o\n", addr, w16(wo));
		addr+=2;
	}
}

static int haslabel(int addr, int seg)
{
	int i;
	for(i=0;i<NSym;i++)
		if(Sym[i].value==addr && BASETYPE(Sym[i].type)==seg && Sym[i].name[0]) return 1;
	return 0;
}

/* bss has no file content, so reserve space with `.=.+' -- but each label
 * must sit at its own offset (a single trailing `.=.+size' would collapse
 * them all to the start, shifting every bss address). */
static void dump_bss(int a0, int size, FILE *out)
{
	int addr=a0, end=a0+size;
	while(addr<end){
		int next=addr+2;
		labels(addr, N_BSS, out);
		while(next<end && !haslabel(next,N_BSS)) next+=2;
		fprintf(out, "\t.=.+%o\n", next-addr);
		addr=next;
	}
}

/* header banner */
static void banner(FILE *out, char *what, int magic, int text, int data, int bss)
{
	int c = Asm ? '/' : ';';	/* `as' comments are `/', not `;' */
	fprintf(out, "%c %s  --  pdp11-bsd28-das%s\n", c, what,
		Asm ? " -a (reassemblable)" : " disassembly");
	if(!Asm)
		fprintf(out, "; magic 0%o  text %d  data %d  bss %d  (%d symbols)\n",
			magic, text, data, bss, NSym);
	fprintf(out, "\n");
}

/* Disassemble one self-contained object (exec header at file offset `base`)
 * -- a bare .o or an archive member -- to `out`. */
static void do_object(long base, char *what, FILE *out)
{
	int magic=w16(base), text=w16(base+2), data=w16(base+4), bss=w16(base+6),
	    syms=w16(base+8), flag=w16(base+14);
	long TBASE=base+16, DBASE=TBASE+text;
	long reloc = flag ? 0 : (long)(text+data);
	long SYMOFF = DBASE+data+reloc;
	int i;
	readsyms(SYMOFF, syms);
	/* relocation geometry: reltext follows data, reldata follows reltext */
	Tbase=TBASE; Tsize=text; Dbase=DBASE; Dsize=data;
	RTbase=DBASE+data; RDbase=DBASE+data+text; HasReloc=!flag;
	banner(out, what, magic, text, data, bss);
	if(Asm){	/* declare globals + absolute (~name=offset) symbols so the
			 * source reassembles to the same symbol table */
		for(i=0;i<NSym;i++)
			if(ISEXT(Sym[i].type) && Sym[i].name[0])
				fprintf(out, ".globl\t%s\n", Sym[i].name);
		for(i=0;i<NSym;i++)
			if(BASETYPE(Sym[i].type)==N_ABS && !ISEXT(Sym[i].type) && Sym[i].name[0])
				fprintf(out, "%s = %o\n", Sym[i].name, Sym[i].value & 0177777);
		fprintf(out, "\n");
	}
	fprintf(out, ".text\n");
	disasm_text(TBASE, 0, text, out);
	if(data){ fprintf(out, "\n.data\n"); disasm_data(DBASE, text, data, out); }
	if(bss){ fprintf(out, "\n.bss\n"); dump_bss(text+data, bss, out); }
}

/* open <stem>.<obj>.dis, de-duplicating repeated basenames */
static FILE *openpart(char *stem, char *obj, int seq)
{
	static char path[1200];
	char clean[64]; int i;
	for(i=0;obj[i] && i<60;i++) clean[i] = (obj[i]=='/')?'_':obj[i];
	clean[i]=0;
	if(seq) sprintf(path, "%s.%s.%d.dis", stem, clean, seq);
	else    sprintf(path, "%s.%s.dis", stem, clean);
	{ FILE *f=fopen(path,"w");
	  if(!f){ perror(path); exit(1); }
	  fprintf(stderr, "%s\n", path);
	  return f; }
}

/* A linked a.out: split text into per-object listings via the N_FN file
 * symbols `ld' left behind; emit the (unsplittable) data/bss once. */
static void do_aout_split(char *stem, int tostdout)
{
	int text=w16(2), data=w16(4), bss=w16(6), syms=w16(8), flag=w16(14);
	long TBASE=16, DBASE=16+text, reloc=flag?0:(long)(text+data);
	long SYMOFF=DBASE+data+reloc;
	int i, k, fn[512], nfn=0;
	readsyms(SYMOFF, syms);
	Tbase=TBASE; Tsize=text; Dbase=DBASE; Dsize=data;
	RTbase=DBASE+data; RDbase=DBASE+data+text; HasReloc=!flag;
	for(i=0;i<NSym;i++)
		if(BASETYPE(Sym[i].type)==N_FN && nfn<512) fn[nfn++]=i;
	/* sort the file boundaries by text address (insertion sort) */
	for(i=1;i<nfn;i++){ int t=fn[i],j=i;
		while(j>0 && Sym[fn[j-1]].value>Sym[t].value){ fn[j]=fn[j-1]; j--; }
		fn[j]=t; }

	if(nfn==0 || tostdout){		/* no boundaries, or -p: one listing */
		do_object(0, stem, tostdout?stdout:openpart(stem,"aout",0));
		return;
	}
	for(k=0;k<nfn;k++){
		int a0=Sym[fn[k]].value;
		int a1=(k+1<nfn)?Sym[fn[k+1]].value:text;
		int dup=0, j;
		for(j=0;j<k;j++) if(!strcmp(Sym[fn[j]].name,Sym[fn[k]].name)) dup++;
		{ FILE *out=openpart(stem, Sym[fn[k]].name, dup);
		  banner(out, Sym[fn[k]].name, w16(0), a1-a0, 0, 0);
		  fprintf(out, ".text\n");
		  disasm_text(TBASE, a0, a1, out);
		  fclose(out); }
	}
	/* data + bss cannot be split by object from the a.out alone -> one file */
	if(data || bss){
		FILE *out=openpart(stem, "DATA", 0);
		banner(out, "shared data/bss (object split not determinable)",
			w16(0), 0, data, bss);
		if(data){ fprintf(out, ".data\n"); disasm_data(DBASE, text, data, out); }
		if(bss){ fprintf(out, "\n.bss\n"); dump_bss(text+data, bss, out); }
		fclose(out);
	}
}

/* An archive: disassemble each object member to its own listing. */
static void do_archive(char *stem, int tostdout)
{
	long off=2;			/* past the 2-byte ARMAG word */
	while(off+26 <= FLEN){
		char name[15]; int i; long size, mbase;
		for(i=0;i<14;i++) name[i]=F[off+i];
		name[14]=0;
		for(i=13;i>=0 && (name[i]==' '||name[i]==0);i--) name[i]=0;
		size=(unsigned)(w16(off+22) | (w16(off+24)<<16));
		mbase=off+26;
		if(name[0] && strcmp(name,"__.SYMDEF") && w16(mbase)>=0407 && w16(mbase)<=0411){
			FILE *out = tostdout ? stdout : openpart(stem, name, 0);
			do_object(mbase, name, out);
			if(!tostdout) fclose(out);
		}
		off = mbase + size + (size&1);	/* members are word-aligned */
	}
}

/* ---- driver ------------------------------------------------------------- */
int main(int argc, char **argv)
{
	char *path, *stem, *p; int tostdout=0;
	FILE *f; long n;

	while(argc>1 && argv[1][0]=='-'){
		if(!strcmp(argv[1],"-p")) tostdout=1;
		else if(!strcmp(argv[1],"-a")) Asm=1;	/* reassemblable source */
		else { fprintf(stderr,"usage: das [-a] [-p] file\n"); return 1; }
		argc--; argv++;
	}
	if(argc!=2){ fprintf(stderr,"usage: das [-a] [-p] file\n"); return 1; }
	path=argv[1];
	if((f=fopen(path,"rb"))==NULL){ perror(path); return 1; }
	fseek(f,0,2); FLEN=ftell(f); fseek(f,0,0);
	F=malloc(FLEN); n=fread(F,1,FLEN,f); fclose(f);
	if(n!=FLEN){ fprintf(stderr,"%s: short read\n",path); return 1; }

	/* output stem = input basename without directory */
	stem = (p=strrchr(path,'/')) ? p+1 : path;

	if(FLEN>=2 && (unsigned short)w16(0)==(unsigned short)ARMAG){
		do_archive(stem, tostdout);
	} else if(FLEN>=16 && (w16(0)==0407||w16(0)==0410||w16(0)==0411)){
		do_aout_split(stem, tostdout);	/* splits if N_FN present, else whole */
	} else {
		fprintf(stderr,"%s: not a PDP-11 a.out, object, or archive\n",path);
		return 1;
	}
	return 0;
}
