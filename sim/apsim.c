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

static unsigned char M[1<<16];
static unsigned short R[8];		/* R6=sp, R7=pc */
#define SP R[6]
#define PC R[7]
static int FN, FZ, FV, FC;		/* condition codes */
static int halted, ecode, trace;

static int ld2(int a){ a&=0xffff; return M[a] | (M[(a+1)&0xffff]<<8); }
static void st2(int a,int v){ a&=0xffff; M[a]=v&0xff; M[(a+1)&0xffff]=(v>>8)&0xff; }
static int ld1(int a){ return M[a&0xffff]; }
static void st1(int a,int v){ M[a&0xffff]=v&0xff; }

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
		case 1: putv(d,sv,byte); setNZ(sv,byte); FV=0; break;	/* MOV */
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
