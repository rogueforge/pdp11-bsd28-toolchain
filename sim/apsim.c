/*
 * apsim -- a tiny user-mode PDP-11 simulator for verifying this toolchain's
 * output end to end.  It loads a 2.8BSD a.out (0407/0410/0411), executes
 * PDP-11 instructions in a 64 KB address space, and emulates the handful of
 * 2BSD `sys' traps the libc stubs use (exit, write, read, open, close,
 * creat, lseek).  Not a full or cycle-accurate PDP-11 -- just enough to run
 * programs built by cc/as/ld and observe their output and exit status.
 *
 * This is a host-side verification tool, NOT part of the produced toolchain.
 *
 * Usage:  apsim [-t] a.out [args...]      (-t traces each instruction)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>

static unsigned char M[1<<16];
static unsigned short R[8];		/* R6=sp, R7=pc */
#define SP R[6]
#define PC R[7]
static int FN, FZ, FV, FC;		/* condition codes */
static int halted, ecode, trace;

/* ---- FP11 floating-point unit ---------------------------------------
 * 6 accumulators held as host doubles, plus the FP status register.  Floats
 * live in memory in DEC F (32-bit) / D (64-bit) format -- NOT IEEE -- so the
 * conversion routines below translate to/from host double on every load and
 * store.  c1 runs everything in D mode (it emits the `fltused' convention
 * rather than an explicit setd), so the FPS defaults to double precision. */
static double AC[6];
static int FPS = 0200;			/* bit 0200 = double mode, 0100 = long int */
#define FPD 0200
#define FPL 0100
static int fN, fZ, fV, fC;		/* FP condition codes (cfcc copies to CPU) */

static int ld2(int a){ a&=0xffff; return M[a] | (M[(a+1)&0xffff]<<8); }
static void st2(int a,int v){ a&=0xffff; M[a]=v&0xff; M[(a+1)&0xffff]=(v>>8)&0xff; }
static int ld1(int a){ return M[a&0xffff]; }
static void st1(int a,int v){ M[a&0xffff]=v&0xff; }

/* Read a DEC float at `addr' (dbl ? D:4 words : F:2 words) -> host double.
 * The sign/exponent word is at the lowest address; value =
 * (-1)^S * (0.5 + frac/2^56) * 2^(E-128), 8-bit excess-128 exponent. */
static double rdfloat(int addr, int dbl){
	unsigned long long bits=0; int i, nw=dbl?4:2, s, e;
	unsigned long long f;
	for(i=0;i<nw;i++) bits=(bits<<16)|ld2((addr+2*i)&0xffff);
	if(!dbl) bits<<=32;			/* left-justify F into 64 bits */
	s=(bits>>63)&1; e=(bits>>55)&0377; f=bits&0x7FFFFFFFFFFFFFULL;
	if(e==0) return 0.0;			/* DEC zero (and "undefined") */
	{ double m=0.5+(double)f/72057594037927936.0, v=ldexp(m, e-128);
	  return s?-v:v; }
}
/* Write host double `v' as a DEC float at `addr' (dbl ? D : F). */
static void wrfloat(int addr, double v, int dbl){
	int i, nw=dbl?4:2, s=0, e; double m; unsigned long long bits, frac;
	if(v==0){ for(i=0;i<nw;i++) st2((addr+2*i)&0xffff,0); return; }
	if(v<0){ s=1; v=-v; }
	m=frexp(v,&e); e+=128;
	if(e<=0){ for(i=0;i<nw;i++) st2((addr+2*i)&0xffff,0); return; }
	if(e>0377) e=0377;
	frac=(unsigned long long)((m-0.5)*72057594037927936.0+0.5);
	bits=((unsigned long long)s<<63)|((unsigned long long)(e&0377)<<55)
	    |(frac&0x7FFFFFFFFFFFFFULL);
	if(!dbl){ bits+=0x80000000ULL; bits&=~0xFFFFFFFFULL; }
	for(i=0;i<nw;i++) st2((addr+2*i)&0xffff, (bits>>(16*(3-i)))&0xFFFF);
}

/* ---- operand resolution ---------------------------------------------
 * Resolve a 6-bit mode|reg field to a "location": a register (ISREG|n) or
 * a 16-bit memory address.  Auto-inc/dec and index words advance as needed.
 */
#define ISREG 0x10000
static int operand(int spec, int byte)
{
	int mode=(spec>>3)&7, rn=spec&7;
	int inc=(byte && rn<6)?1:2, a, x;
	switch(mode){
	case 0: return ISREG|rn;				/* Rn       */
	case 1: return R[rn];					/* (Rn)     */
	case 2: a=R[rn]; R[rn]=(R[rn]+inc)&0xffff; return a;	/* (Rn)+    */
	case 3: a=ld2(R[rn]); R[rn]=(R[rn]+2)&0xffff; return a;	/* @(Rn)+   */
	case 4: R[rn]=(R[rn]-inc)&0xffff; return R[rn];		/* -(Rn)    */
	case 5: R[rn]=(R[rn]-2)&0xffff; return ld2(R[rn]);	/* @-(Rn)   */
	case 6: x=ld2(PC); PC=(PC+2)&0xffff; return (R[rn]+x)&0xffff;		/* X(Rn)  */
	case 7: x=ld2(PC); PC=(PC+2)&0xffff; return ld2((R[rn]+x)&0xffff);	/* @X(Rn) */
	}
	return 0;
}
static int getv(int loc, int byte){
	if(loc&ISREG){ int v=R[loc&7]; return byte?(v&0xff):v; }
	return byte?ld1(loc):ld2(loc);
}
static void putv(int loc, int v, int byte){
	if(loc&ISREG){
		if(byte) R[loc&7]=(R[loc&7]&0xff00)|(v&0xff);
		else R[loc&7]=v&0xffff;
		return;
	}
	if(byte) st1(loc,v); else st2(loc,v);
}
static int sgn(int v,int byte){ return byte?(signed char)v:(signed short)v; }
static void setNZ(int v,int byte){ FZ=((byte?v&0xff:v&0xffff)==0); FN=(sgn(v,byte)<0); }

/* ---- 2BSD sys-call emulation ---------------------------------------- */
static void do_syscall(int num, int argaddr)
{
	/* argaddr points just past the sys instruction's number word: the
	 * inline arguments (for indirect calls) live there.  fd-style first
	 * args are in R0 (the stubs put them there). */
	int a1 = ld2(argaddr), a2 = ld2(argaddr+2);
	long r;
	switch(num){
	case 1:				/* exit(code in r0) */
		halted=1; ecode=R[0]&0xff; return;
	case 4:				/* write(r0=fd, a1=buf, a2=count) */
		r = write(R[0], M+(a1&0xffff), a2&0xffff);
		break;
	case 3:				/* read(r0=fd, a1=buf, a2=count) */
		r = read(R[0], M+(a1&0xffff), a2&0xffff);
		break;
	case 5:				/* open(a1=path, a2=mode) */
		r = open((char*)(M+(a1&0xffff)), a2);
		break;
	case 6:				/* close(r0=fd) */
		r = close(R[0]);
		break;
	case 8:				/* creat(a1=path, a2=mode) */
		r = creat((char*)(M+(a1&0xffff)), a2);
		break;
	case 19:			/* lseek(r0=fd, a1=off, a2=whence) */
		r = lseek(R[0], (short)a1, a2);
		break;
	case 10:			/* unlink(a1=path) */
		r = unlink((char*)(M+(a1&0xffff)));
		break;
	case 54:			/* ioctl -- apsim has no ttys, so every ioctl
					 * (gtty/stty via isatty) fails: isatty()==0,
					 * stdio block-buffers and flushes at exit. */
		r = -1;
		break;
	case 17:			/* break(a1=new break addr).  apsim's 64KB is
					 * flat-mapped, so the heap memory already
					 * exists; just succeed unless the new break
					 * would run into the stack (R6). */
		r = ((a1&0xffff) < (R[6]&0xffff)) ? 0 : -1;
		break;
	default:
		fprintf(stderr, "apsim: unhandled sys %d\n", num);
		halted=1; ecode=127; return;
	}
	if(r<0){ FC=1; R[0]=1; }	/* error: set carry, errno-ish in r0 */
	else { FC=0; R[0]=r&0xffff; }
}

/* a `sys' (trap) instruction: 0104400|n.  n==0 is the indirect call, whose
 * following word addresses a block { sysinstr; arg1; arg2; ... }. */
static void do_sys(int instr)
{
	int n=instr&0377, argaddr, num;
	if(n==0){			/* indirect */
		int blk=ld2(PC); PC=(PC+2)&0xffff;
		num=ld2(blk)&0377;	/* the real sys instruction's number */
		argaddr=blk+2;
	} else {			/* direct: inline args follow */
		num=n; argaddr=PC;
	}
	do_syscall(num, argaddr);
}

static int cond(int instr){		/* branch taken? */
	switch(instr&0177400){
	case 0000400: return 1;				/* BR   */
	case 0001000: return !FZ;			/* BNE  */
	case 0001400: return FZ;			/* BEQ  */
	case 0002000: return (FN^FV)==0;		/* BGE  */
	case 0002400: return (FN^FV)!=0;		/* BLT  */
	case 0003000: return (FZ|(FN^FV))==0;		/* BGT  */
	case 0003400: return (FZ|(FN^FV))!=0;		/* BLE  */
	case 0100000: return !FN;			/* BPL  */
	case 0100400: return FN;			/* BMI  */
	case 0101000: return !(FC|FZ);			/* BHI  */
	case 0101400: return FC|FZ;			/* BLOS */
	case 0102000: return !FV;			/* BVC  */
	case 0102400: return FV;			/* BVS  */
	case 0103000: return !FC;			/* BCC/BHIS */
	case 0103400: return FC;			/* BCS/BLO  */
	}
	return -1;				/* not a branch */
}

/* FP11 floating-point instruction (017xxxx).  AC[ac] is the accumulator in
 * bits 6-7; bits 0-5 are a source/dest operand (a float accumulator when the
 * mode is register-direct, else a float/int in memory).  Memory floats use
 * the current FPS precision, except the convert forms (movof/movfo) which use
 * the OTHER precision.  c1 only ever uses ac0/ac1 and non-autoincrement modes. */
static double fp_get(int spec, int dbl){	/* float source operand */
	if(((spec>>3)&7)==0) return AC[spec&7];
	return rdfloat(operand(spec,0), dbl);
}
static void fp_put(int spec, double v, int dbl){	/* float dest operand */
	if(((spec>>3)&7)==0){ AC[spec&7]=v; return; }
	wrfloat(operand(spec,0), v, dbl);
}
static void fp_setcc(double v){ fN=v<0; fZ=v==0; fV=0; fC=0; }

static void do_fp(int instr){
	int op=instr&0177400, ac=(instr>>6)&3, spec=instr&077;
	int isreg=((spec>>3)&7)==0, reg=spec&7, dbl=(FPS&FPD)?1:0;
	double sv; int addr; long iv;

	switch(instr){				/* no-operand / mode control */
	case 0170000: FN=fN; FZ=fZ; FV=fV; FC=fC; return;	/* cfcc */
	case 0170001: FPS&=~FPD; return;			/* setf */
	case 0170011: FPS|= FPD; return;			/* setd */
	case 0170002: FPS&=~FPL; return;			/* seti */
	case 0170012: FPS|= FPL; return;			/* setl */
	}
	/* single-operand ops + ldfps/stfps: the opcode is bits 15-6 (no AC
	 * field), so they must be matched with the 0177700 mask -- clrf/tstf/
	 * absf/negf differ only in bits 7-6, which 0177400 would drop. */
	switch(instr&0177700){
	case 0170100: FPS=isreg?R[reg]:ld2(operand(spec,0)); return;	/* ldfps */
	case 0170200: if(isreg)R[reg]=FPS; else st2(operand(spec,0),FPS); return; /* stfps */
	case 0170400:	/* clrf */
		fp_put(spec,0.0,dbl); fN=0; fZ=1; fV=0; fC=0; return;
	case 0170500:	/* tstf */
		fp_setcc(fp_get(spec,dbl)); return;
	case 0170600:	/* absf */
		sv=fp_get(spec,dbl); if(sv<0)sv=-sv; fp_put(spec,sv,dbl); fp_setcc(sv); return;
	case 0170700:	/* negf */
		sv=-fp_get(spec,dbl); fp_put(spec,sv,dbl); fp_setcc(sv); return;
	}
	/* two-operand ops: opcode bits 15-8, AC in bits 7-6 */
	switch(op){
	case 0171000: AC[ac]*=fp_get(spec,dbl); fp_setcc(AC[ac]); return;	/* mulf */
	case 0172000: AC[ac]+=fp_get(spec,dbl); fp_setcc(AC[ac]); return;	/* addf */
	case 0173000: AC[ac]-=fp_get(spec,dbl); fp_setcc(AC[ac]); return;	/* subf */
	case 0174400: AC[ac]/=fp_get(spec,dbl); fp_setcc(AC[ac]); return;	/* divf */
	case 0173400: fp_setcc(fp_get(spec,dbl)-AC[ac]); return;		/* cmpf: CC from (fsrc-AC) */
	case 0172400: AC[ac]=fp_get(spec,dbl); fp_setcc(AC[ac]); return;	/* movf LDF */
	case 0174000: fp_put(spec,AC[ac],dbl); fp_setcc(AC[ac]); return;	/* movf STF */
	case 0177400: AC[ac]=fp_get(spec,!dbl); fp_setcc(AC[ac]); return;	/* movof (load cvt) */
	case 0176000: fp_put(spec,AC[ac],!dbl); fp_setcc(AC[ac]); return;	/* movfo (store cvt) */
	case 0177000:	/* movif: int -> float */
		if(isreg) iv=(short)R[reg];
		else { addr=operand(spec,0);
		       iv=(FPS&FPL)? (int)((ld2(addr)<<16)|ld2((addr+2)&0xffff))
				   : (short)ld2(addr); }
		AC[ac]=(double)iv; fp_setcc(AC[ac]); return;
	case 0175400:	/* movfi: float -> int */
		iv=(long)AC[ac];
		if(isreg) R[reg]=iv&0xffff;
		else { addr=operand(spec,0);
		       if(FPS&FPL){ st2(addr,(iv>>16)&0xffff); st2((addr+2)&0xffff,iv&0xffff); }
		       else st2(addr,iv&0xffff); }
		fN=AC[ac]<0; fZ=AC[ac]==0; fV=0; fC=0; return;
	}
	fprintf(stderr,"apsim: unhandled fp instr %06o at %06o\n", instr, (PC-2)&0xffff);
	halted=1; ecode=127;
}

static void step(void)
{
	int instr=ld2(PC), op, byte, s, d, sv, dv, r, sl, dl;
	PC=(PC+2)&0xffff;

	/* branches first (their high bits overlap other encodings) */
	if(((instr&0177400)>=0000400 && (instr&0177400)<=0003400) ||
	   ((instr&0177400)>=0100000 && (instr&0177400)<=0103400)){
		int c=cond(instr);
		if(c>=0){ if(c){ int off=(signed char)(instr&0377); PC=(PC+2*off)&0xffff; } return; }
	}
	/* sys / trap */
	if((instr&0177400)==0104400){ do_sys(instr); return; }
	if((instr&0177400)==0104000){ do_sys(instr|0); return; }	/* emt: treat like sys */

	op=(instr>>12)&017;
	byte=(op>=011 && op<=015);		/* MOVB..BISB carry bit 15 */

	/* FP11 floating-point group (017xxxx) */
	if(op==017){ do_fp(instr); return; }

	/* double-operand: MOV1 CMP2 BIT3 BIC4 BIS5 ADD6 ; +010 byte ; 016=SUB */
	if((op>=1&&op<=6)||(op>=011&&op<=016)){
		int bop = op&07;		/* 1..6, or 6 for SUB(016) */
		s=operand((instr>>6)&077,byte); sv=getv(s,byte);
		d=operand(instr&077,byte);
		if(op==016){ /* SUB (word only) */
			dv=getv(d,0);
			r=(dv-sv)&0xffff; putv(d,r,0); setNZ(r,0);
			FC=((unsigned)(dv&0xffff) < (unsigned)(sv&0xffff));
			FV=(((dv^sv)&(~sv^r))>>15)&1; return;
		}
		switch(bop){
		case 1:							/* MOV/MOVB */
			/* MOVB to a register is the one byte instruction that
			 * sign-extends the byte into the high half of the register
			 * (all others leave the high byte alone).  Without this, a
			 * `movb' of e.g. a NUL leaves stale high bits, so the format
			 * loop's `beq' on a NUL never fires (printf %d hung). */
			if(byte && (d&ISREG)) R[d&7]=sgn(sv,1)&0xffff;
			else putv(d,sv,byte);
			setNZ(sv,byte); FV=0; break;
		case 2: dv=getv(d,byte); r=(sv-dv);			/* CMP */
			setNZ(r,byte); { int m=byte?0xff:0xffff,sb=byte?0x80:0x8000;
			FC=((sv&m)<(dv&m)); FV=(((sv^dv)&(~dv^r))&sb)!=0; } break;
		case 3: dv=getv(d,byte); r=sv&dv; setNZ(r,byte); FV=0; break;	/* BIT */
		case 4: dv=getv(d,byte); r=dv&~sv; putv(d,r,byte); setNZ(r,byte); FV=0; break; /* BIC */
		case 5: dv=getv(d,byte); r=dv|sv; putv(d,r,byte); setNZ(r,byte); FV=0; break; /* BIS */
		case 6: dv=getv(d,0); r=(dv+sv)&0xffff; putv(d,r,0); setNZ(r,0);	/* ADD */
			FC=((unsigned)(dv&0xffff)+(unsigned)(sv&0xffff))>0xffff;
			FV=((~(dv^sv)&(dv^r))>>15)&1; break;
		}
		return;
	}

	/* EIS / xor: 07RSS -- MUL DIV ASH ASHC XOR */
	if(op==07){
		int reg=(instr>>6)&7;
		switch(instr&0177000){
		case 0070000:	/* MUL: R*src -> R,R|1 (32-bit) */
			s=operand(instr&077,0); sv=(short)getv(s,0);
			{ long p=(long)(short)R[reg]*sv; R[reg]=(p>>16)&0xffff; R[reg|1]=p&0xffff;
			  FZ=(p==0); FN=(p<0); FV=0; FC=(p<-32768||p>32767); }
			return;
		case 0071000:	/* DIV */
			s=operand(instr&077,0); sv=(short)getv(s,0);
			if(sv==0){ FV=FC=1; return; }
			{ long dd=((long)R[reg]<<16)|R[reg|1];
			  R[reg]=(dd/sv)&0xffff; R[reg|1]=(dd%sv)&0xffff;
			  FZ=(R[reg]==0); FN=((short)R[reg]<0); FV=0; FC=0; }
			return;
		case 0072000:	/* ASH: shift R by src (signed count) */
			s=operand(instr&077,0); sv=(short)getv(s,0);
			{ int cnt=sv&077; if(cnt&040)cnt-=64; int val=(short)R[reg];
			  if(cnt>=0) val<<=cnt; else val>>=(-cnt);
			  R[reg]=val&0xffff; setNZ(R[reg],0); FV=0; }
			return;
		case 0073000:	/* ASHC */
			s=operand(instr&077,0); sv=(short)getv(s,0);
			{ int cnt=sv&077; if(cnt&040)cnt-=64;
			  long val=((long)R[reg]<<16)|R[reg|1];
			  if(cnt>=0) val<<=cnt; else val>>=(-cnt);
			  R[reg]=(val>>16)&0xffff; R[reg|1]=val&0xffff;
			  FZ=(val==0); FN=(val<0); FV=0; }
			return;
		case 0074000:	/* XOR: R ^ dst -> dst */
			d=operand(instr&077,0); dv=getv(d,0); r=dv^R[reg];
			putv(d,r,0); setNZ(r,0); FV=0;
			return;
		}
	}

	/* JSR: 004RDD */
	if((instr&0177000)==0004000){
		int reg=(instr>>6)&7;
		d=operand(instr&077,0);			/* dst = target address */
		SP=(SP-2)&0xffff; st2(SP,R[reg]);	/* push reg */
		R[reg]=PC; PC=d&0xffff;
		return;
	}
	/* RTS: 00020R */
	if((instr&0177770)==0000200){
		int reg=instr&7; PC=R[reg]; R[reg]=ld2(SP); SP=(SP+2)&0xffff; return;
	}
	/* SOB: 077Rnn (decrement reg, branch back if nonzero) */
	if((instr&0177000)==0077000){
		int reg=(instr>>6)&7, off=instr&077;
		R[reg]=(R[reg]-1)&0xffff;
		if(R[reg]!=0) PC=(PC-2*off)&0xffff;
		return;
	}

	/* single-operand (word/byte): 0?05DDDD..0?063DDD, JMP, SWAB, SXT */
	{
		int sop=instr&0177700, b=(instr&0100000)!=0;
		int spec=instr&077;
		switch(instr&0077700){		/* ignore bit15 (byte) for the group */
		case 0005000: d=operand(spec,b); r=0; putv(d,0,b); FN=0;FZ=1;FV=0;FC=0; return;	/* CLR */
		case 0005100: d=operand(spec,b); dv=getv(d,b); r=~dv; putv(d,r,b); setNZ(r,b); FV=0;FC=1; return; /* COM */
		case 0005200: d=operand(spec,b); dv=getv(d,b); r=dv+1; putv(d,r,b); setNZ(r,b);	/* INC */
			FV=((b?(dv&0xff)==0x7f:(dv&0xffff)==0x7fff)); return;
		case 0005300: d=operand(spec,b); dv=getv(d,b); r=dv-1; putv(d,r,b); setNZ(r,b);	/* DEC */
			FV=((b?(dv&0xff)==0x80:(dv&0xffff)==0x8000)); return;
		case 0005400: d=operand(spec,b); dv=getv(d,b); r=-dv; putv(d,r,b); setNZ(r,b);	/* NEG */
			FC=((r&(b?0xff:0xffff))!=0); FV=0; return;
		case 0005500: d=operand(spec,b); dv=getv(d,b); r=dv+FC; putv(d,r,b); setNZ(r,b); return;	/* ADC */
		case 0005600: d=operand(spec,b); dv=getv(d,b); r=dv-FC; putv(d,r,b); setNZ(r,b); return;	/* SBC */
		case 0005700: d=operand(spec,b); dv=getv(d,b); setNZ(dv,b); FV=0;FC=0; return;	/* TST */
		case 0006000: d=operand(spec,b); dv=getv(d,b);	/* ROR */
			{ int m=b?0xff:0xffff,nb=b?0x80:0x8000; r=(dv>>1)|(FC?nb:0); FC=dv&1;
			  putv(d,r,b); setNZ(r,b); FV=FN^FC; } return;
		case 0006100: d=operand(spec,b); dv=getv(d,b);	/* ROL */
			{ int m=b?0xff:0xffff,nb=b?0x80:0x8000; r=((dv<<1)|(FC?1:0))&m; FC=(dv&nb)!=0;
			  putv(d,r,b); setNZ(r,b); FV=FN^FC; } return;
		case 0006200: d=operand(spec,b); dv=getv(d,b);	/* ASR */
			{ int nb=b?0x80:0x8000; FC=dv&1; r=(sgn(dv,b)>>1); putv(d,r,b); setNZ(r,b); FV=FN^FC; } return;
		case 0006300: d=operand(spec,b); dv=getv(d,b);	/* ASL */
			{ int m=b?0xff:0xffff,nb=b?0x80:0x8000; r=(dv<<1)&m; FC=(dv&nb)!=0; putv(d,r,b); setNZ(r,b); FV=FN^FC; } return;
		}
		switch(instr&0177700){
		case 0000100: d=operand(spec,0); PC=d&0xffff; return;		/* JMP */
		case 0000300: d=operand(spec,0); dv=getv(d,0);			/* SWAB */
			r=((dv<<8)|((dv>>8)&0xff))&0xffff; putv(d,r,0); FZ=((r&0xff)==0); FN=((r&0x80)!=0); FV=0;FC=0; return;
		case 0006700: d=operand(spec,0); putv(d,FN?0xffff:0,0); FZ=!FN; FV=0; return; /* SXT */
		}
	}

	/* misc no-ops we can ignore: HALT(0), NOP(0240), RTI/RTT, FP set modes */
	switch(instr){
	case 0000000: halted=1; ecode=0; return;	/* HALT */
	case 0000240: return;				/* NOP */
	case 0000241: FC=0; return;			/* CLC */
	case 0000261: FC=1; return;			/* SEC */
	}
	if((instr&0177740)==0000240) return;		/* condition-code ops */
	if((instr&0177400)==0170000) return;		/* FP control (setd/setf/seti/setl/cfcc...) */
	if((instr&0177000)>=0170000) return;		/* other FP: ignore */

	fprintf(stderr,"apsim: illegal instruction %06o at %06o\n", instr, (PC-2)&0xffff);
	halted=1; ecode=126;
}

int main(int argc, char **argv)
{
	FILE *f; int hdr[8], i, ai=1, tsize, dsize, bsize, entry;
	if(argc>1 && strcmp(argv[1],"-t")==0){ trace=1; ai=2; }
	if(ai>=argc){ fprintf(stderr,"usage: apsim [-t] a.out [args]\n"); return 2; }
	if(!(f=fopen(argv[ai],"rb"))){ perror(argv[ai]); return 2; }
	for(i=0;i<8;i++){ int lo=fgetc(f), hi=fgetc(f); hdr[i]=lo|(hi<<8); }
	if(hdr[0]!=0407 && hdr[0]!=0410 && hdr[0]!=0411){
		fprintf(stderr,"apsim: bad magic %06o\n",hdr[0]); return 2; }
	tsize=hdr[1]; dsize=hdr[2]; bsize=hdr[3]; entry=hdr[5];
	/* 0407: text@0, data@tsize; 0410/0411: data on the next 8K boundary */
	{
		int dbase = (hdr[0]==0407) ? tsize : ((tsize+017777)&~017777);
		fread(M, 1, tsize, f);
		fread(M+dbase, 1, dsize, f);
		memset(M+dbase+dsize, 0, bsize);
	}
	fclose(f);

	/* stack: argc=0, then NULL argv and NULL envp so crt0's scan ends */
	SP=0177700;
	st2(SP, 0);			/* argc */
	st2(SP+2, 0);			/* argv[0] = NULL */
	st2(SP+4, 0);			/* envp[0] = NULL */
	PC=entry;

	for(i=0; i<20000000 && !halted; i++){
		if(trace) fprintf(stderr,"pc=%06o sp=%06o r0=%06o instr=%06o\n",PC,SP,R[0],ld2(PC));
		step();
	}
	if(!halted){ fprintf(stderr,"apsim: instruction limit\n"); return 125; }
	return ecode;
}
