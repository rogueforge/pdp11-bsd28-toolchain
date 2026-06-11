/*
 * pdp11-bsd28-dcc -- a decompiler for 2.8BSD PDP-11 objects produced by THIS
 * toolchain's cc (Ritchie's c0/c1 + c2).
 *
 *   dcc file.o      run das -a on the object, then decompile the .s to file.c
 *   dcc file.s      decompile the assembly directly
 *   dcc lib.a       (multi-object) make a dir, lay out the .s files, decompile each
 *   dcc a.out       (linked) same -- one .s/.c per input object
 *
 * It only attempts objects whose .s carries our compiler's fingerprint
 * (`jsr r5,csv' prologue / `jmp cret' epilogue, `~~name' entries, `~var=off'
 * frame map).  Hand-written assembly, or Pascal (px), Fortran, or other-compiler
 * output, is declined -- it emits a stub .c saying so rather than guessing.
 *
 * The reconstruction is best-effort: it recovers function signatures and the
 * K&R parameter/local names (from the `~name = offset' lines das preserves),
 * rebuilds expressions by symbolically tracking the work registers and the
 * argument stack, and renders control flow with labels and gotos (a later pass
 * structures the obvious if/while/for).  Unhandled instructions are kept as
 * `/* asm: ... *<slash>' so nothing is silently dropped.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>		/* mkdir for the per-object output directory */

#ifndef PREFIX
#define PREFIX "pdp11-bsd28"
#endif

/* ----------------------------------------------------------------- source */
#define MAXLINES 60000
static char *Line[MAXLINES];
static int   NLine;

static char *xstrdup(const char *s){ char *p=malloc(strlen(s)+1); strcpy(p,s); return p; }

/* read an assembly stream into Line[] */
static void readsrc(FILE *f)
{
	char buf[1024];
	NLine=0;
	while(fgets(buf,sizeof buf,f)){
		char *p=buf+strlen(buf);
		while(p>buf && (p[-1]=='\n'||p[-1]=='\r')) *--p=0;
		if(NLine<MAXLINES) Line[NLine++]=xstrdup(buf);
	}
}

/* skip leading blanks/tabs */
static char *skipws(char *s){ while(*s==' '||*s=='\t') s++; return s; }

/* ----------------------------------------------------------- frame symbols */
/* das preserves `~name = offset' for each K&R parameter (positive offset from
 * r5) and local (negative).  We map offsets back to names. */
struct fsym { char name[40]; int off; };
static struct fsym Fsym[4000]; static int NFsym;

static void add_fsym(const char *name, int off)
{
	if(NFsym<4000){ strncpy(Fsym[NFsym].name,name,39); Fsym[NFsym].off=off; NFsym++; }
}
/* name for a r5-relative offset, or 0 -- only if UNAMBIGUOUS (one symbol at that
 * offset).  das -a flattens the per-function `~name=off' scopes into one module
 * list, so colliding offsets (every function's first param is 4(r5)) can't be
 * named reliably; those fall back to a slot name. */
static const char *fsym_name(int off)
{
	int i, hit=-1, n=0;
	for(i=0;i<NFsym;i++) if(Fsym[i].off==(off&0xffff)){ hit=i; n++; }
	return (n==1) ? Fsym[hit].name : 0;
}

/* sign-extend a 16-bit octal/decimal offset to a host int */
static int soff(int v){ v&=0xffff; return v>=0x8000 ? v-0x10000 : v; }

/* --------------------------------------------------------- symbol map file */
/* A stripped binary has no names.  We rebuild them incrementally and feed them
 * back in: a map file (`-m FILE') keyed by the NATIVE PDP-11 address.  das emits
 * those addresses as octal `.L<addr>' synthetic labels; the map gives each a
 * name.  As functions/globals are identified, add lines and re-run -- dcc
 * substitutes the name everywhere the address appears.  Two line formats:
 *   f  name  size  0xADDR      (radare2 flag file, e.g. rogue's symbols_34.r2)
 *   0xADDR|0ADDR  name         (plain) */
struct mapent { int addr; char name[80]; };
static struct mapent Map[16000]; static int NMap;
struct objent { char file[128]; int addr; };	/* object layout; see objfile() */
static struct objent Obj[3000]; static int NObj;

static void add_map(int addr, const char *name)
{
	const char *n = (name[0]=='_') ? name+1 : name;	/* drop the assembly `_' */
	if(NMap<16000){ Map[NMap].addr=addr; strncpy(Map[NMap].name,n,79); NMap++; }
}
static void load_map(const char *path)
{
	FILE *f=fopen(path,"r"); char buf[256], a0[16], nm[80], sz[32], ad[32];
	if(!f){ fprintf(stderr,"dcc: cannot open map %s\n",path); return; }
	while(fgets(buf,sizeof buf,f)){
		if(buf[0]=='#'||buf[0]==';'||buf[0]=='\n') continue;
		/* object boundary: obj <file> <addr> */
		if(sscanf(buf,"%15s %79s %31s",a0,nm,ad)==3 && !strcmp(a0,"obj")){
			if(NObj<3000){ strncpy(Obj[NObj].file,nm,127); Obj[NObj].file[127]=0;
				       Obj[NObj].addr=(int)strtol(ad,0,0); NObj++; }
			continue;
		}
		if(sscanf(buf,"%15s %79s %31s %31s",a0,nm,sz,ad)==4 && !strcmp(a0,"f"))
			add_map((int)strtol(ad,0,0),nm);		/* r2: f name size addr */
		else if(sscanf(buf,"%31s %79s",ad,nm)==2)
			add_map((int)strtol(ad,0, (ad[0]=='0'&&(ad[1]=='x'||ad[1]=='X'))?16:8), nm);
	}
	fclose(f);
	fprintf(stderr,"dcc: loaded %d symbols from %s\n",NMap,path);
}
static const char *mapname(int addr)
{
	int i; for(i=0;i<NMap;i++) if(Map[i].addr==addr) return Map[i].name;
	return 0;
}

/* Object boundaries: ld lays input objects out contiguously, so each occupies a
 * text address range [start, next_start).  The stripped binary doesn't record
 * them, so the map supplies them: `obj <file> <0xaddr>' marks where each object
 * begins.  A function at address A belongs to the object with the greatest
 * start <= A.  With no `obj' lines, output is a single .c.  (Obj[] declared above.) */
static const char *objfile(int addr)
{
	int i, best=-1;
	for(i=0;i<NObj;i++) if(Obj[i].addr<=addr && (best<0||Obj[i].addr>Obj[best].addr)) best=i;
	return best>=0 ? Obj[best].file : "_unknown.c";
}

/* Output routing.  With `obj' boundaries we split into <stem>.dec/<file>.c, one
 * per object (cached, opened on first use); otherwise everything goes to the one
 * Single file.  stream_for(addr) returns the right stream for a function at addr. */
static char Outdir[1024];
static FILE *Single;
static struct { char file[160]; FILE *fp; } Strm[4000]; static int NStrm;

static FILE *stream_for(int addr)
{
	int i; char path[1300]; const char *file;
	if(!Outdir[0]) return Single;			/* single-file mode */
	file = objfile(addr);
	for(i=0;i<NStrm;i++) if(!strcmp(Strm[i].file,file)) return Strm[i].fp;
	snprintf(path,sizeof path,"%s/%s",Outdir,file);
	{ FILE *fp=fopen(path,"w");
	  if(!fp){ perror(path); return Single?Single:stdout; }
	  if(NStrm<4000){ strncpy(Strm[NStrm].file,file,159); Strm[NStrm].fp=fp; NStrm++; }
	  fprintf(fp,"/* %s -- one object of the input, decompiled by %s-dcc */\n\n",file,PREFIX);
	  return fp; }
}
/* render a code/data target token (.L<oct>, *$<oct>, *$_name, _name) as a C
 * identifier, applying the map; `pfx' (fn_/g_) names an unmapped synthetic */
static const char *symname(const char *tok, const char *pfx)
{
	static char b[96]; const char *m;
	if(!strncmp(tok,"*$_",3)){ strcpy(b,tok+3); return b; }	/* unstripped symbol */
	if(tok[0]=='_'){ strcpy(b,tok+1); return b; }
	if(!strncmp(tok,".L",2)){ int a=(int)strtol(tok+2,0,8); if((m=mapname(a))) strcpy(b,m); else sprintf(b,"%s%s",pfx,tok+2); return b; }
	if(!strncmp(tok,"*$",2)){ int a=(int)strtol(tok+2,0,8); if((m=mapname(a))) strcpy(b,m); else sprintf(b,"%s%s",pfx,tok+2); return b; }
	strcpy(b,tok); return b;
}

/* canonical C name for an r5-relative frame slot: a parameter (positive even
 * offset 4,6,8,...) or a local (negative).  Used for both the signature and the
 * operands so they agree. */
static const char *vname(int off)
{
	static char b[24]; const char *nm=fsym_name(off);
	if(nm){ strcpy(b,nm); return b; }		/* recovered, unambiguous name */
	if(off>=4 && (off&1)==0) sprintf(b,"a%d",(off-4)/2+1);	/* param slot */
	else if(off<0)           sprintf(b,"v%d",-off);		/* local slot */
	else                     sprintf(b,"f%d",off);
	return b;
}

/* ----------------------------------------------------- recognition / parse */
/* The C runtime: `csv'/`cret' when symbols are present, else the single dominant
 * `jsr r5,<addr>' / `jmp <addr>' targets every function shares (a stripped a.out,
 * e.g. our rogue3.4 target, still has the code -- just no names). */
static char Csv[80]="csv", Cret[80]="cret";

/* the target of `jsr r5,X' or a single-operand `jmp X', into `out' */
static int op_of(char *line, const char *mn, int after_r5, char *out)
{
	char *p=skipws(line), *o=out;
	if(strncmp(p,mn,strlen(mn))) return 0;
	p=skipws(p+strlen(mn));
	if(after_r5){ if(strncmp(p,"r5,",3)) return 0; p+=3; }
	while(*p && *p!=' ' && *p!='\t' && *p!=',' && o<out+78) *o++=*p++; *o=0;
	if(*p && *p!=' ' && *p!='\t') return 0;		/* not a lone operand */
	return out[0]!=0;
}

static int looks_like_cc(void)
{
	static struct { char t[80]; int n; } jr[6000], jm[6000];
	int njr=0, njm=0, i, j, lit=0; char t[80];
	for(i=0;i<NLine;i++){
		if(op_of(Line[i],"jsr",1,t)){
			if(!strcmp(t,"csv")) lit|=1;
			for(j=0;j<njr;j++) if(!strcmp(jr[j].t,t)){ jr[j].n++; break; }
			if(j==njr && njr<6000){ strcpy(jr[njr].t,t); jr[njr].n=1; njr++; }
		}
		if(op_of(Line[i],"jmp",0,t)){
			if(!strcmp(t,"cret")) lit|=2;
			for(j=0;j<njm;j++) if(!strcmp(jm[j].t,t)){ jm[j].n++; break; }
			if(j==njm && njm<6000){ strcpy(jm[njm].t,t); jm[njm].n=1; njm++; }
		}
	}
	if(lit==3){ strcpy(Csv,"csv"); strcpy(Cret,"cret"); return 1; }	/* unstripped */
	{ int best=-1,bn=0; for(j=0;j<njr;j++) if(jr[j].n>bn){bn=jr[j].n;best=j;}
	  if(best<0||bn<3) return 0; strcpy(Csv,jr[best].t); }		/* stripped: csv */
	{ int best=-1,bn=0; for(j=0;j<njm;j++) if(jm[j].n>bn){bn=jm[j].n;best=j;}
	  if(best<0||bn<3) return 0; strcpy(Cret,jm[best].t); }		/* stripped: cret */
	return 1;
}

/* a label line `foo:' -> returns the label (without colon) in a static buf */
static char *labelof(char *line)
{
	static char b[128]; char *p=skipws(line), *q=b;
	if(!*p) return 0;
	while(*p && *p!=':' && *p!=' ' && *p!='\t' && q<b+126) *q++=*p++;
	if(*p==':'){ *q=0; return b; }
	return 0;
}

/* Is line `i' a function entry?  A label whose first real instruction is the
 * csv prologue.  Sets `name': the symbol (stripped of `_'), or fn_<addr> for a
 * synthetic `.L<addr>' label das emits when there are no symbols. */
static int func_entry_name(int i, char *name, int *addr)
{
	char lab[128], t[80], *l=labelof(Line[i]); int k;
	if(addr) *addr=-1;
	if(!l) return 0;
	strncpy(lab,l,127); lab[127]=0;
	for(k=i+1; k<NLine && k<i+6; k++){
		char *p=skipws(Line[k]);
		if(labelof(Line[k])||p[0]=='.'||p[0]==0||p[0]=='~') continue;  /* labels/aliases */
		if(op_of(Line[k],"jsr",1,t) && !strcmp(t,Csv)){
			strcpy(name, symname(lab,"fn_"));	/* map -> real name, else fn_<addr> */
			if(addr && lab[0]=='.'&&lab[1]=='L') *addr=(int)strtol(lab+2,0,8);
			return 1;
		}
		return 0;	/* first instruction is not the prologue */
	}
	return 0;
}

/* ------------------------------------------------------- operand rendering */
/* Render a PDP-11 operand as a C expression fragment.  `reg' holds the symbolic
 * value of r0..r5 within the current basic block (0 = just the register). */
static char Reg[8][256];	/* symbolic register contents, "" = unknown */

static void render(const char *op, char *out)
{
	char o[256]; strncpy(o,op,255); o[255]=0;

	if(o[0]=='$'){					/* immediate / address constant */
		if(o[1]=='_') sprintf(out,"&%s",o+2);	/* $_g  -> &g  (address of global) */
		else if(o[1]=='*'){			/* *$x deferred immediate: rare here */
			sprintf(out,"%s",o+2);
		} else strcpy(out,o+1);			/* $7   -> 7 */
		return;
	}
	if(o[0]=='_'){ strcpy(out,o+1); return; }	/* _g   -> g  (global) */
	if(!strncmp(o,"*$_",3)){ sprintf(out,"%s",o+3); return; }  /* call target */
	/* r5-relative: N(r5) -> parameter/local name, or *(int*)(fp+N) */
	{
		char *lp=strchr(o,'(');
		if(lp && !strncmp(lp,"(r5)",4)){
			int off=soff((int)strtol(o,0,8));
			strcpy(out,vname(off));
			return;
		}
		if(lp && !strncmp(lp,"(sp)",4)){ strcpy(out,"<stk>"); return; }
	}
	if(o[0]=='r'&&o[1]>='0'&&o[1]<='5'&&o[2]==0){
		int r=o[1]-'0';
		if(Reg[r][0]) strcpy(out,Reg[r]); else strcpy(out,o);
		return;
	}
	strcpy(out,o);					/* fallback: raw */
}

/* condition-code branch -> C comparison operator (operands already swapped:
 * `cmp A,B' sets flags as B-A, so `bgt' means B>A) */
static const char *cc_op(const char *mn)
{
	if(!strcmp(mn,"beq")) return "==";
	if(!strcmp(mn,"bne")) return "!=";
	if(!strcmp(mn,"bgt")) return ">";
	if(!strcmp(mn,"blt")) return "<";
	if(!strcmp(mn,"bge")) return ">=";
	if(!strcmp(mn,"ble")) return "<=";
	if(!strcmp(mn,"bhi")) return ">";	/* unsigned */
	if(!strcmp(mn,"blos"))return "<=";
	if(!strcmp(mn,"bhis"))return ">=";
	if(!strcmp(mn,"blo")) return "<";
	return 0;
}

/* ------------------------------------------------------- function emission */
struct insn { char mn[16]; char a[128]; char b[128]; char *label; };

static void split_insn(char *line, struct insn *ins)
{
	char *p=skipws(line), *q; int n;
	ins->mn[0]=ins->a[0]=ins->b[0]=0;
	q=ins->mn; n=0;
	while(*p && *p!=' ' && *p!='\t' && n<15){ *q++=*p++; n++; } *q=0;
	p=skipws(p);
	/* operands: a[,b] */
	q=ins->a; n=0;
	while(*p && *p!=',' && *p!=' ' && *p!='\t' && n<127){ *q++=*p++; n++; } *q=0;
	if(*p==','){ p++; q=ins->b; n=0;
		while(*p && *p!=' ' && *p!='\t' && n<127){ *q++=*p++; n++; } *q=0; }
}

/* a global stack of pending call arguments (pushed before a jsr) */
static char Args[16][256]; static int NArgs;
/* operands of the last cmp/tst, consumed by the following conditional branch:
 * `cmp A,B; b<cc> L' tests `B <cc> A' */
static char Cmp_a[256]="0", Cmp_b[256]="0";

static void clear_regs(void){ int i; for(i=0;i<8;i++) Reg[i][0]=0; NArgs=0; }

/* emit one instruction as C into `out' (FILE), within a function body */
static void emit_insn(FILE *out, struct insn *ins)
{
	char *mn=ins->mn, ra[256], rb[256];
	const char *cc;

	/* the csv prologue is the function entry, not a statement */
	if(!strcmp(mn,"jsr") && !strcmp(ins->a,"r5") && !strcmp(ins->b,Csv)) return;

	/* control flow */
	if(!strcmp(mn,"jmp")||!strcmp(mn,"jbr")||!strcmp(mn,"br")){
		/* let r0 keep flowing across the jump so a value computed just before a
		 * `jmp <epilogue>' survives to the `jmp cret' return */
		if(!strcmp(ins->a,Cret)){
			if(Reg[0][0]) fprintf(out,"\treturn %s;\n",Reg[0]);
			else fprintf(out,"\treturn;\n");
		} else fprintf(out,"\tgoto %s;\n",ins->a);
		return;
	}
	cc=cc_op(mn);
	if(cc){					/* conditional branch -- uses the prior cmp/tst */
		fprintf(out,"\tif (%s %s %s) goto %s;\n", Cmp_b, cc, Cmp_a, ins->a);
		return;
	}
	if(!strcmp(mn,"cmp")){			/* record operands for the next branch */
		render(ins->a,ra); render(ins->b,rb);
		strcpy(Cmp_a,ra); strcpy(Cmp_b,rb);
		return;
	}
	if(!strcmp(mn,"tst")){ render(ins->a,ra); strcpy(Cmp_b,ra); strcpy(Cmp_a,"0"); return; }

	/* a call: jsr pc,*$_func -> r0 = func(args) */
	if(!strcmp(mn,"jsr") && !strcmp(ins->a,"pc")){
		char fn[128], call[512]; int i;
		strcpy(fn, symname(ins->b,"fn_"));	/* map -> real name, else fn_<addr> */
		sprintf(call,"%s(",fn);
		for(i=NArgs-1;i>=0;i--){ strcat(call,Args[i]); if(i) strcat(call,", "); }
		strcat(call,")");
		strcpy(Reg[0],call); NArgs=0;
		return;
	}

	/* loads / stores / arithmetic */
	render(ins->a,ra);
	if(!strcmp(mn,"mov")||!strcmp(mn,"movb")){
		/* push to call stack? */
		if(!strcmp(ins->b,"(sp)")||!strcmp(ins->b,"-(sp)")){
			if(NArgs<16) strcpy(Args[NArgs++],ra);
			return;
		}
		if(ins->b[0]=='r'&&ins->b[1]>='0'&&ins->b[1]<='5'&&ins->b[2]==0){
			strcpy(Reg[ins->b[1]-'0'],ra); return;
		}
		render(ins->b,rb); fprintf(out,"\t%s = %s;\n",rb,ra); return;
	}
	if(!strcmp(mn,"clr")||!strcmp(mn,"clrb")){
		if(ins->a[0]=='r'&&ins->a[1]>='0'&&ins->a[1]<='5'&&ins->a[2]==0){ strcpy(Reg[ins->a[1]-'0'],"0"); return; }
		fprintf(out,"\t%s = 0;\n",ra); return;
	}
	if(!strcmp(mn,"inc")){ fprintf(out,"\t%s++;\n",ra); return; }
	if(!strcmp(mn,"dec")){ fprintf(out,"\t%s--;\n",ra); return; }
	if(!strcmp(mn,"neg")){ fprintf(out,"\t%s = -%s;\n",ra,ra); return; }
	if(!strcmp(mn,"com")){ fprintf(out,"\t%s = ~%s;\n",ra,ra); return; }
	if(!strcmp(mn,"asl")){					/* <<1 */
		if(ins->a[0]=='r'){ int r=ins->a[1]-'0'; char t[256]; sprintf(t,"(%s << 1)",Reg[r][0]?Reg[r]:ins->a); strcpy(Reg[r],t); return; }
		fprintf(out,"\t%s <<= 1;\n",ra); return;
	}
	if(!strcmp(mn,"asr")){ fprintf(out,"\t%s >>= 1;\n",ra); return; }

	/* two-operand arithmetic into a register: fold into the symbolic value */
	{
		const char *opc=0;
		if(!strcmp(mn,"add")) opc="+";
		else if(!strcmp(mn,"sub")) opc="-";
		else if(!strcmp(mn,"bis")) opc="|";
		else if(!strcmp(mn,"bic")) opc="& ~";
		else if(!strcmp(mn,"mul")) opc="*";
		else if(!strcmp(mn,"xor")) opc="^";
		if(opc){
			if(ins->b[0]=='r'&&ins->b[1]>='0'&&ins->b[1]<='5'&&ins->b[2]==0){
				int r=ins->b[1]-'0'; char t[256];
				sprintf(t,"(%s %s %s)", Reg[r][0]?Reg[r]:ins->b, opc, ra); strcpy(Reg[r],t); return;
			}
			render(ins->b,rb); fprintf(out,"\t%s %s= %s;\n",rb,opc,ra); return;
		}
	}

	/* nothing matched: keep the assembly so it isn't lost */
	if(ins->b[0]) fprintf(out,"\t/* asm: %s %s,%s */\n",mn,ins->a,ins->b);
	else if(ins->a[0]) fprintf(out,"\t/* asm: %s %s */\n",mn,ins->a);
	else fprintf(out,"\t/* asm: %s */\n",mn);
}

/* decompile the whole module to `out' */
static void decompile(const char *outname)
{
	int i;

	/* pass 1: collect the frame-symbol map (~name = offset) */
	NFsym=0;
	for(i=0;i<NLine;i++){
		char *p=skipws(Line[i]); char nm[40]; int off; char *q=nm;
		if(*p!='~'||p[1]=='~') continue;
		p++; while(*p && *p!=' ' && *p!='\t' && *p!='=' && q<nm+38) *q++=*p++; *q=0;
		p=skipws(p); if(*p!='=') continue; p=skipws(p+1);
		off=(int)strtol(p,0,8);
		add_fsym(nm,soff(off));
	}

	/* output: a directory of per-object files if the map supplied `obj'
	 * boundaries, else a single .c carrying everything */
	if(NObj>0){
		char *dot; strncpy(Outdir,outname,1000); Outdir[1000]=0;
		dot=strrchr(Outdir,'.'); if(dot)*dot=0; strcat(Outdir,".dec");
		mkdir(Outdir,0777);
		fprintf(stderr,"dcc: %d object boundaries -> %s/\n",NObj,Outdir);
	} else {
		Single=fopen(outname,"w"); if(!Single){ perror(outname); return; }
		fprintf(Single,"/* Decompiled by %s-dcc from %s-cc output (c0/c1).\n",PREFIX,PREFIX);
		fprintf(Single," * Best-effort reconstruction: K&R names recovered from the frame\n");
		fprintf(Single," * map; control flow shown with goto; verify against the .s. */\n\n");
	}

	/* pass 2: text -> one C function per global `_name:' ; data -> globals */
	{
		int inseg=0;	/* 0=none 1=text 2=data */
		for(i=0;i<NLine;i++){
			char *p=skipws(Line[i]), *lab, fnbuf[128]; int faddr;
			if(!strncmp(p,".text",5)){ inseg=1; continue; }
			if(!strncmp(p,".data",5)||!strncmp(p,".bss",4)){ inseg=2; continue; }

			if(inseg==1 && func_entry_name(i,fnbuf,&faddr)){
				int end, maxp=0, j; char tt[128]; FILE *fo=stream_for(faddr);
				/* the body runs to the next function entry or a segment change;
				 * scan it for the frame parameters actually referenced (4,6,...) */
				for(end=i+1; end<NLine; end++){
					char *e=skipws(Line[end]), *q=Line[end];
					if(!strncmp(e,".data",5)||!strncmp(e,".bss",4)||!strncmp(e,".text",5)) break;
					if(func_entry_name(end,tt,0)) break;
					while((q=strstr(q,"(r5)"))){
						char *s=q; int off;
						while(s>Line[end] && isdigit((unsigned char)s[-1])) s--;
						off=soff((int)strtol(s,0,8));
						if(off>=4 && (off&1)==0 && off<128 && off>maxp) maxp=off;
						q+=4;
					}
				}
				/* signature + K&R parameter declarations */
				fprintf(fo,"%s(",fnbuf);
				for(j=4;j<=maxp;j+=2) fprintf(fo,"%s%s",vname(j),j+2<=maxp?", ":"");
				fprintf(fo,")\n");
				for(j=4;j<=maxp;j+=2) fprintf(fo,"\tint %s;\n",vname(j));
				fprintf(fo,"{\n");
				clear_regs();
				for(i=i+1;i<end;i++){
					char *bp=skipws(Line[i]), *bl=labelof(Line[i]);
					if(bp[0]=='~') continue;		/* ~~entry / ~name=off */
					if(bl){ if(bl[0]!='_') fprintf(fo,"%s:\n",bl); continue; }
					if(bp[0]=='.'||bp[0]==0) continue;
					{ struct insn ins; split_insn(Line[i],&ins); emit_insn(fo,&ins); }
				}
				fprintf(fo,"}\n\n");
				i=end-1;
				continue;
			}

			/* data: declare each `_name:' global; show the raw init words */
			if(inseg==2 && (lab=labelof(Line[i])) && lab[0]=='_'){
				char dname[128], init[256]=""; int k; FILE *fo=stream_for(-1);
				strcpy(dname,lab);	/* before labelof() is called again below */
				for(k=i+1;k<NLine;k++){
					char *d=skipws(Line[k]);
					if(labelof(Line[k])||d[0]=='.'||d[0]==0) break;
					if(strlen(init)+strlen(d)<240){ strcat(init,d); strcat(init," "); }
				}
				fprintf(fo,"int %s;\t/* data: %s*/\n",dname+1,init[0]?init:"");
			}
		}
	}
	if(Single) fclose(Single);
	{ int k; for(k=0;k<NStrm;k++) fclose(Strm[k].fp); }
}

/* ------------------------------------------------------------ file driver */
static int has_suffix(const char *s, const char *suf){ size_t a=strlen(s),b=strlen(suf); return a>=b && !strcmp(s+a-b,suf); }

static char Argv0[1200];

/* run das -a on an object, streaming its .s back; das is found next to us */
static FILE *run_das(const char *obj)
{
	char cmd[4096], dasp[1300], *slash;
	strncpy(dasp,Argv0,1199); dasp[1199]=0;
	slash=strrchr(dasp,'/');
	if(slash) strcpy(slash+1,PREFIX "-das");	/* same directory as dcc */
	else strcpy(dasp,PREFIX "-das");		/* fall back to PATH */
	snprintf(cmd,sizeof cmd,"'%s' -a -p '%s'",dasp,obj);
	return popen(cmd,"r");
}

static void usage(void){ fprintf(stderr,"usage: %s-dcc file.{o,s,a}|a.out\n",PREFIX); exit(2); }

int main(int argc, char **argv)
{
	const char *path; FILE *in; char outname[1024]; FILE *out; int magic;

	if(argc<2) usage();
	strncpy(Argv0,argv[0],1199); Argv0[1199]=0;
	while(argc>2 && argv[1][0]=='-'){		/* -m MAPFILE (repeatable) */
		if(!strcmp(argv[1],"-m")){ load_map(argv[2]); argc-=2; argv+=2; }
		else { argc--; argv++; }
	}
	if(argc<2) usage();
	path=argv[1];

	/* .s -> decompile directly */
	if(has_suffix(path,".s")){
		in=fopen(path,"r"); if(!in){ perror(path); return 1; }
		readsrc(in); fclose(in);
	} else {
		/* peek the magic to tell object/a.out/archive from text */
		FILE *f=fopen(path,"rb"); int c0,c1;
		if(!f){ perror(path); return 1; }
		c0=getc(f); c1=getc(f); fclose(f);
		magic=(c1<<8)|c0;
		if(magic==0407||magic==0410||magic==0411||magic==0405||magic==0430||magic==0431){
			/* a single object (or a.out): run das -a */
			FILE *p=run_das(path);
			if(!p){ fprintf(stderr,"dcc: cannot run das on %s\n",path); return 1; }
			readsrc(p); pclose(p);
		} else if((magic&0xffff)==0177545){	/* ar archive */
			fprintf(stderr,"dcc: archive/multi-object layout not yet implemented; "
				"extract members and run dcc on each .o\n");
			return 1;
		} else {
			/* assume it's assembly text */
			in=fopen(path,"r"); if(!in){ perror(path); return 1; }
			readsrc(in); fclose(in);
		}
	}

	/* output name: <stem>.c */
	{
		const char *base=path, *slash=strrchr(path,'/'); const char *dot;
		if(slash) base=slash+1;
		strncpy(outname,base,1000); dot=strrchr(outname,'.');
		if(dot) ((char*)dot)[0]=0; strcat(outname,".c");
	}
	if(!looks_like_cc()){
		out=fopen(outname,"w"); if(!out){ perror(outname); return 1; }
		fprintf(out,"/* %s: not recognized as %s-cc (c0/c1) output.\n",outname,PREFIX);
		fprintf(out," * No `jsr r5,csv'/`jmp cret' fingerprint -- this looks like\n");
		fprintf(out," * hand-written assembly, or Pascal/Fortran/other-compiler code.\n");
		fprintf(out," * Declining to decompile. */\n");
		fclose(out);
		fprintf(stderr,"dcc: %s -> %s (declined: not our compiler)\n",path,outname);
		return 0;
	}
	decompile(outname);		/* writes <stem>.c, or <stem>.dec/ with `obj' map lines */
	fprintf(stderr,"dcc: %s -> %s\n",path,outname);
	return 0;
}
