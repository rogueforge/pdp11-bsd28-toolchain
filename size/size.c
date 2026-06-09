char *sccsid = "@(#)size.c	2.4";
#include	<stdio.h>
#include 	<a.out.h>
#include	<whoami.h>

/*
 *	size -- determine object size
 */

int a_magic[] = {A_MAGIC1,A_MAGIC2,A_MAGIC3,A_MAGIC4,
#ifdef VMUNIX
0412,0413,
#endif VMUNIX
#ifdef MENLO_OVLY
0430, 0431,
#endif MENLO_OVLY
0};

main(argc, argv)
char **argv;
{
	struct exec buf;
	long sum;
#ifdef MENLO_OVLY
	long coresize;
#endif MENLO_OVLY
	int gorp,i;
	FILE *f;

	if (argc==1) {
		*argv = "a.out";
		argc++;
		--argv;
	}
	gorp = argc;
	printf("text\tdata\tbss\tdec\toct\n");
	while(--argc) {
		++argv;
		if ((f = fopen(*argv, "r"))==NULL) {
			printf("size: %s not found\n", *argv);
			continue;
		}
		fread((char *)&buf, sizeof(buf), 1, f);
		for(i=0;a_magic[i];i++)
			if(a_magic[i] == buf.a_magic) break;
		if(a_magic[i] == 0) {
			printf("size: %s not an object file\n", *argv);
			fclose(f);
			continue;
		}
		printf("%u +\t%u +\t%u =\t", buf.a_text,buf.a_data,buf.a_bss);
		sum = (long) buf.a_text + (long) buf.a_data + (long) buf.a_bss;
		printf("%ld =\t%lo", sum, sum);
		printf("\t%s\n", *argv);
#ifdef MENLO_OVLY
		if (buf.a_magic == 0430 || buf.a_magic == 0431) {
			unsigned sizes[8];

			fread(sizes, sizeof sizes, 1, f);
			coresize = buf.a_text;
			for (i = 1; i < 8; i++)
				if (sizes[i])
					coresize += sizes[i];
			printf("%ld total text, overlays: (", coresize);
			for (i = 1; i < 8; i++)
				if (sizes[i]) {
					if (i > 1)
						printf(",");
					printf("%u", sizes[i]);
				}
			printf(")\n");
		}
#endif MENLO_OVLY
		fclose(f);
	}
}
