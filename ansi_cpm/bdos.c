/* BDOS emulation */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

#include "defs.h"
#include "vt.h"

#define BIOS 0xFE00
#define DPH0 (BIOS + 0x0036)
#define DPB0 (DPH0 + 0x0010)
#define DIRBUF 0xff80
#define CPMLIBDIR "./"
static int storedfps = 0;
unsigned short usercode = 0x00;
int restricted_mode = 0;
int silent_exit = 0;
char *stuff_cmd = 0;
int trace_bdos = 0;

char *rdcmdline(z80info *z80, int max, int ctrl_c_enable)
{
    int i, c;
    static char s[259];

    fflush(stdout);
    max &= 0xff;
    i = 1;      /* number of next character */

    if (stuff_cmd) {
    	/* Kill prompt */
    	vt52('\b');
    	vt52(' ');
    	vt52('\b');
    	vt52('\b');
    	vt52(' ');
    	vt52('\b');
    	strcpy(s + i, stuff_cmd);
    	/* printf("'%s'\n", stuff_cmd); */
    	i = 1 + strlen(s + i);
    	stuff_cmd = 0;
    	silent_exit = 1;
    	goto hit_rtn;
    }

loop:
    c = kget(0);
    if (c < ' ' || c == 0x7f) {
        switch (c) {
	case 3:
	    if (ctrl_c_enable) {
		vt52('^');
		vt52('C');
		z80->regpc = BIOS+3;
		s[0] = 0;
		return s;
	    }
	    break;
        case 8:
	case 0x7f:
	    if (i > 1) {
		--i;
		vt52('\b');
		vt52(' ');
		vt52('\b');
		fflush(stdout);
	    }
	    break;
        case '\n':
        case '\r':
	    hit_rtn:
	    s[0] = i-1;
	    s[i] = 0;
	    if (!strcmp(s + 1, "bye")) {
	    	printf("\r\n");
	    	finish(z80);
	    }
	    if (i <= max)
		s[i] = '\r';
	    return s;
        }
        goto loop;
    } else if (i <= max) {
        s[i++] = c;
        vt52(c);
	fflush(stdout);
    }
    goto loop;
}


#if 0
static struct FCB {
    char drive;
    char name[11];
    char data[24];
} samplefcb;
#endif

static void FCB_to_filename(unsigned char *p, char *name) {
    int i;
    char *org = name;
    /* strcpy(name, "test/");
       name += 5; */
    for (i = 0; i < 8; ++i)
	if (p[i+1] != ' ')
	    *name++ = tolower(p[i+1]);
    if (p[9] != ' ') {
	*name++ = '.';
	for (i = 0; i < 3; ++i)
	    if (p[i+9] != ' ')
		*name++ = tolower(p[i+9]);
    }
    *name = '\0';
    if (trace_bdos)
    	printf("File name is %s\r\n", org);
}

static void FCB_to_ufilename(unsigned char *p, char *name) {
    int i;
    char *org = name;
    /* strcpy(name, "test/");
       name += 5; */
    for (i = 0; i < 8; ++i)
	if (p[i+1] != ' ')
	    *name++ = toupper(p[i+1]);
    if (p[9] != ' ') {
	*name++ = '.';
	for (i = 0; i < 3; ++i)
	    if (p[i+9] != ' ')
		*name++ = toupper(p[i+9]);
    }
    *name = '\0';
    if (trace_bdos)
    	printf("File name is %s\r\n", org);
}

static struct stfps {
    FILE *fp;
    unsigned where;
    char name[12];
} stfps[100];

static void storefp(z80info *z80, FILE *fp, unsigned where) {
    int i;
    int ind = -1;
    for (i = 0; i < storedfps; ++i)
	if (stfps[i].where == 0xffffU)
	    ind = i;
	else if (stfps[i].where == where) {
	    ind = i;
	    goto putfp;
	}
    if (ind < 0) {
	if (++storedfps > 100) {
	    fprintf(stderr, "out of fp stores!\n");
	    exit(1);
	}
	ind = storedfps - 1;
    }
    stfps[ind].where = where;
 putfp:
    stfps[ind].fp = fp;
    memcpy(stfps[ind].name, z80->mem+z80->regde+1, 11);
    stfps[ind].name[11] = '\0';
}

static FILE *getfp(z80info *z80, unsigned where) {
    int i;
    for (i = 0; i < storedfps; ++i)
	if (stfps[i].where == where) {
	    /* check name? */
	    return stfps[i].fp;
	}
    /* fcb not found. maybe it has been moved? */
    for (i = 0; i < storedfps; ++i)
	if (stfps[i].where != 0xffffU &&
	    !memcmp(z80->mem+z80->regde+1, stfps[i].name, 11)) {
	    stfps[i].where = where;	/* moved FCB */
	    return stfps[i].fp;
	}

    fprintf(stderr, "error: cannot find fp entry for FCB at %04x"
	    " fctn %d, FCB named %s\n", where, z80->regbc & 0xff,
	    z80->mem+where+1);
    for (i = 0; i < storedfps; ++i)
	if (stfps[i].where != 0xffffU)
	    printf("%s %04x\n", stfps[i].name, stfps[i].where);
    exit(1);
}

static void delfp(z80info *z80, unsigned where) {
    int i;
    for (i = 0; i < storedfps; ++i)
	if (stfps[i].where == where) {
	    stfps[i].where = 0xffffU;
	    return;
	}
    fprintf(stderr, "error: cannot del fp entry for FCB at %04x\n", where);
    exit(1);
}

/* FCB fields */
#define FCB_DR 0
#define FCB_F1 1
#define FCB_T1 9
#define FCB_EX 12
#define FCB_S1 13
#define FCB_S2 14
#define FCB_RC 15
#define FCB_CR 32
#define FCB_R0 33
#define FCB_R1 34
#define FCB_R2 35

/* Support 8MB files */

#define ADDRESS  (((long)z80->mem[z80->regde+FCB_R0] + \
		    (long)z80->mem[z80->regde+FCB_R1] * 256) * 128L)
/* (long)z80->mem[z80->regde+35] * 65536L; */


/* For sequential access, the record number is also in the FCB:
 *    EX + 32 * S2 is extent number (0 .. 8191).  Each extent has 16 blocks.  Each block is 1K.
 *          extent number = (file offset) / 16384
 *
 *    RC  has number of records in current extent: (0 .. 128)
 *          maybe just set to 128?
 *          otherwise: 
 *
 *    CR  is current record offset within current extent: (0 .. 127)
 *          ((file offset) % 16384) / 128
 */
#define SEQ_ADDRESS  (16384L * ((long)z80->mem[DE + FCB_EX] + 32L * (long)z80->mem[DE + FCB_S2]) + \
                        128L * (long)z80->mem[DE + FCB_CR])

#define SEQ_CR(n) (((n) % 16384) / 128)
#define SEQ_EXTENT(n) ((n) / 16384)
#define SEQ_EX(n) (SEQ_EXTENT(n) % 32)
#define SEQ_S2(n) (SEQ_EXTENT(n) / 32)

static DIR *dp = NULL;
static unsigned sfn = 0;

char *bdos_decode(int n)
{
	switch (n) {
	    case  0: return "System Reset";
	    case 1: return "Console Input";
	    case 2: return "Console Output";
	    case 6: return "direct I/O";
	    case 9: return "Print String";
	    case 10: return "Read Command Line";
	    case 12: return "Return Version Number";
	    case 26: return "Set DMA Address";
	    case 32: return "Get/Set User Code";
	    case 11: return "Console Status";
	    case 13: return "reset disk system";
	    case 14: return "select disk";
	    case 15: return "open file";
	    case 16: return "close file";
	    case 17: return "search for first";
	    case 18: return "search for next";
	    case 19: return "delete file (no wildcards yet)";
	    case 20: return "read sequential";
	    case 21: return "write sequential";
	    case 22: return "make file";
	    case 23: return "rename file";
	    case 24: return "return login vector";
	    case 25: return "return current disk";
	    case 29: return "return r/o vector";
	    case 31: return "get disk parameters";
	    case 33: return "read random record";
	    case 34: return "write random record";
	    case 35: return "compute file size";
	    case 36: return "set random record";
	    case 41:
	    default: return "unknown";
	}
}

/* True if DE points to an FCB for a given BDOS call */

int bdos_fcb(int n)
{
	switch (n) {
	    case 15: return 1; // "open file";
	    case 16: return 1; // "close file";
	    case 17: return 1; // "search for first";
	    case 18: return 1; // "search for next";
	    case 19: return 1; // "delete file (no wildcards yet)";
	    case 20: return 1; // "read sequential";
	    case 21: return 1; // "write sequential";
	    case 22: return 1; // "make file";
	    case 23: return 1; // "rename file";
	    case 33: return 1; // "read random record";
	    case 34: return 1; // "write random record";
	    case 35: return 1; // "compute file size";
	    case 36: return 1; // "set random record";
	    default: return 0;
	}
}

void bdos_fcb_dump(z80info *z80)
{
	char buf[80];
	printf("FCB %x: ", DE);
	buf[0] = (0x7F & z80->mem[DE + 1]);
	buf[1] = (0x7F & z80->mem[DE + 2]);
	buf[2] = (0x7F & z80->mem[DE + 3]);
	buf[3] = (0x7F & z80->mem[DE + 4]);
	buf[4] = (0x7F & z80->mem[DE + 5]);
	buf[5] = (0x7F & z80->mem[DE + 6]);
	buf[6] = (0x7F & z80->mem[DE + 7]);
	buf[7] = (0x7F & z80->mem[DE + 8]);
	buf[8] = '.';
	buf[9] = (0x7F & z80->mem[DE + 9]);
	buf[10] = (0x7F & z80->mem[DE + 10]);
	buf[11] = (0x7F & z80->mem[DE + 11]);
	buf[12] = 0;
	printf("DR=%x F='%s' EX=%x S1=%x S2=%x RC=%x CR=%x R0=%x R1=%x R2=%x\n",
	       z80->mem[DE + 0], buf, z80->mem[DE + 12], z80->mem[DE + 13],
	       z80->mem[DE + 14], z80->mem[DE + 15], z80->mem[DE + 32], z80->mem[DE + 33],
	       z80->mem[DE + 34], z80->mem[DE + 35]);
}

/* emulation of BDOS calls */

void check_BDOS_hook(z80info *z80) {
    int i;
    char name[32];
    char name2[32];
    struct stat stbuf;
    FILE *fp;
    char *s, *t;
    const char *mode;
    if (trace_bdos)
    {
        printf("\r\nbdos %d %s (AF=%04x BC=%04x DE=%04x HL =%04x SP=%04x STACK=", C, bdos_decode(C), AF, BC, DE, HL, SP);
	for (i = 0; i < 8; ++i)
	    printf(" %4x", z80->mem[SP + 2*i]
		   + 256 * z80->mem[SP + 2*i + 1]);
	printf(")\r\n");
    }
    switch (C) {
    case  0:    /* System Reset */
	warmboot(z80);
	return;
#if 0
	for (i = 0; i < 0x1600; ++i)
	    z80->mem[i+BIOS-0x1600] = cpmsys[i];
	BC = 0;
	PC = BIOS-0x1600+3;
	SP = 0x80;
#endif
	break;
    case 1:     /* Console Input */
	A = HL = kget(0);
	F = 0;
	if (A < ' ') {
	    switch(A) {
	    case '\r':
	    case '\n':
	    case '\t':
		vt52(A);
		break;
	    default:
		vt52('^');
		vt52((A & 0xff)+'@');
		if (A == 3) {	/* ctrl-C pressed */
		    /* PC = BIOS+3;
		       check_BIOS_hook(); */
		    warmboot(z80);
		    return;
		}
	    }
	} else {
	    vt52(A);
	}
	break;
    case 2:     /* Console Output */
	vt52(0x7F & E);
	break;
    case 6:     /* direct I/O */
	switch (E) {
	case 0xff:  if (!constat()) {
	    A = HL = 0;
	    F = 0;
	    break;
	}
	case 0xfd:  A = HL = kget(0);
	    F = 0;
	    break;
	case 0xfe:  A = HL = constat() ? 0xff : 0;
	    F = 0;
	    break;
	default:    vt52(0x7F & E);
	}
	break;
    case 9:	/* Print String */
	s = (char *)(z80->mem + DE);
	while (*s != '$')
	    vt52(0x7F & *s++);
	break;
    case 10:    /* Read Command Line */
	s = rdcmdline(z80, *(t = (char *)(z80->mem + DE)), 1);
	if (PC == BIOS+3) { 	/* ctrl-C pressed */
	    /* check_BIOS_hook(); */		/* execute WBOOT */
	    warmboot(z80);
	    return;
	}
	++t;
	for (i = 0; i <= *s; ++i)
	    t[i] = s[i];
	A = B = HL = 0;
	break;
    case 12:    /* Return Version Number */
    	B = H = 0x00;
	A = HL = 0x22;      /* emulate Cp/M 2.2 */
	F = 0;
	break;
    case 26:    /* Set DMA Address */
	z80->dma = DE;
	A = B = HL = 0;
	break;
    case 32:    /* Get/Set User Code */
	A = B = HL = 0;
	if (E == 0xff)  /* Get Code */
	    A = usercode;
	else
	    usercode = E;
	break;

	/* dunno if these are correct */

    case 11:	/* Console Status */
	A = HL = (constat() ? 0xff : 0x00);
	F = 0;
	B = 0;
	break;

    case 13:	/* reset disk system */
	/* storedfps = 0; */	/* WS crashes then */
	if (dp)
	    closedir(dp);
	dp = NULL;
	z80->dma = 0x80;
	A = B = HL = 0;
	/* select only A:, all r/w */
	break;
    case 14:	/* select disk */
	A = B = HL = 0;
	break;
    case 15:	/* open file */
	mode = "r+b";
    fileio:
	FCB_to_filename(z80->mem+DE, name); /* Try lowercase */
	if (!(fp = fopen(name, mode))) {
	    FCB_to_ufilename(z80->mem+DE, name); /* Try all uppercase instead */
            if (!(fp = fopen(name, mode))) {
	            FCB_to_filename(z80->mem+DE, name);
		    if (*mode == 'r') {
			char ss[50];
			sprintf(ss, "%s/%s", CPMLIBDIR, name);
			fp = fopen(ss, mode);
			if (!fp)
			  fp = fopen(ss, "rb");
		    }
		    if (!fp) {
			/* still no success */
			A = HL = 0xFF;
			F = 0;
			B = 0;
			break;
		    }
            }
	}
	/* success */

	/* memset(z80->mem + DE + 12, 0, 33-12); */
	/* User has to set EX, S2, and CR: don't change these */
	z80->mem[DE + FCB_S1] = 0;
	memset(z80->mem + DE + 16, 0, 16);
	/* R0 - R2? */
	memset(z80->mem + DE + 33, 0, 3);

	z80->mem[DE + FCB_RC] = 0;	/* rc field of FCB */
	if (fstat(fileno(fp), &stbuf) || !S_ISREG(stbuf.st_mode)) {
	    A = HL = 0xFF;
	    F = 0;
	    B = 0;
	    fclose(fp);
	    break;
	}
	{   unsigned long pos;
	    pos = (stbuf.st_size + 127) >> 7;	/* number of records */
	    if (pos > 128)
		z80->mem[DE+FCB_RC] = 0x80;
	    else
		z80->mem[DE+FCB_RC] = pos;
	}
	A = HL = 0;
	B = 0;
	F = 0;
	/* where to store fp? */
	storefp(z80, fp, DE);
	/* printf("opening file %s\n", name); */
	break;
    case 16:	/* close file */
	fp = getfp(z80, DE);
	delfp(z80, DE);
	fclose(fp);
	A = B = HL = 0;
	/* printf("close file\n"); */
	break;
    case 17:	/* search for first */
	if (dp)
	    closedir(dp);
	if (!(dp = opendir("."))) {
	    fprintf(stderr, "opendir fails\n");
	    exit(1);
	}
	sfn = DE;
	/* fall through */
    case 18:	/* search for next */
	if (!dp)
	    goto retbad;
	{   struct dirent *de;
	    unsigned char *p;
	    const char *sr;
	nocpmname:
	    if (!(de = readdir(dp))) {
		closedir(dp);
		dp = NULL;
	    retbad:
		A = HL = 0xff;
		F = 0;
		B = 0;
		break;
	    }
	    /* printf("\r\nlooking at %s\r\n", de->d_name); */
	    /* compare data */
	    memset(p = z80->mem+z80->dma, 0, 128);	/* dmaaddr instead of DIRBUF!! */
	    if (*de->d_name == '.')
		goto nocpmname;
	    if (strchr(sr = de->d_name, '.')) {
		if (strlen(de->d_name) > 12)	/* POSIX: namlen */
		    goto nocpmname;
	    } else if (strlen(de->d_name) > 8)
		    goto nocpmname;
	    /* seems OK */
	    for (i = 0; i < 8; ++i)
		if (*sr != '.' && *sr) {
		    *++p = toupper(*(unsigned char *)sr); sr++;
		} else
		    *++p = ' ';
	    /* skip dot */
	    while (*sr && *sr != '.')
		++sr;
	    while (*sr == '.')
		++sr;
	    for (i = 0; i < 3; ++i)
		if (*sr != '.' && *sr) {
		    *++p = toupper(*(unsigned char *)sr); sr++;
		} else
		    *++p = ' ';
	    /* OK, fcb block is filled */
	    /* match name */
	    p -= 11;
	    sr = (char *)(z80->mem + sfn);
	    for (i = 1; i <= 12; ++i)
		if (sr[i] != '?' && sr[i] != p[i])
		    goto nocpmname;
	    /* yup, it matches */
	    A = HL = 0x00;	/* always at pos 0 */
	    F = 0;
	    B = 0;
	    p[32] = p[64] = p[96] = 0xe5;
	}
	break;
    case 19:	/* delete file (no wildcards yet) */
	FCB_to_filename(z80->mem + DE, name);
	unlink(name);
	A = B = HL = 0;
	break;
    case 20:	/* read sequential */
	fp = getfp(z80, DE);
    readseq:
	if (!fseek(fp, SEQ_ADDRESS, SEEK_SET) && ((i = fread(z80->mem+z80->dma, 1, 128, fp)) > 0)) {
	    long ofst = ftell(fp);
	    if (i != 128)
		memset(z80->mem+z80->dma+i, 0x1a, 128-i);
	    A = HL = 0x00;
	    B = 0;
	    z80->mem[DE + FCB_CR] = SEQ_CR(ofst);
	    z80->mem[DE + FCB_EX] = SEQ_EX(ofst);
	    z80->mem[DE + FCB_S2] = SEQ_S2(ofst);
	} else {
	    A = HL = 0x1;	/* ff => pip error */
	    B = 0;
	}    
	break;
    case 21:	/* write sequential */
	fp = getfp(z80, DE);
    writeseq:
	if (!fseek(fp, SEQ_ADDRESS, SEEK_SET) && fwrite(z80->mem+z80->dma, 1, 128, fp) == 128) {
	    long ofst = ftell(fp);
	    z80->mem[DE + FCB_CR] = SEQ_CR(ofst);
	    z80->mem[DE + FCB_EX] = SEQ_EX(ofst);
	    z80->mem[DE + FCB_S2] = SEQ_S2(ofst);
	    A = HL = 0x00;
	    B = 0;
	} else {
	    A = HL = 0xff;
	    B = 0;
	}
	break;
    case 22:	/* make file */
	mode = "w+b";
	goto fileio;
    case 23:	/* rename file */
	FCB_to_filename(z80->mem + DE, name);
	FCB_to_filename(z80->mem + DE + 16, name2);
	/* printf("rename %s %s called\n", name, name2); */
	rename(name, name2);
	A = B = HL = 0;
	break;
    case 24:	/* return login vector */
	A = HL = 1;	/* only A: online */
	F = 0;
	B = 0;
	break;
    case 25:	/* return current disk */
	A = HL = 0;	/* only A: */
	F = 0;
	B = 0;
	break;
    case 29:	/* return r/o vector */
	A = HL = 0;	/* none r/o */
	B = 0;
	F = 0;
	break;
    case 31:    /* get disk parameters */
        HL = DPB0;    /* only A: */
        A = B = 0;
        break;
    case 33:	/* read random record */
        {
        long ofst;
	fp = getfp(z80, DE);
	/* printf("data is %02x %02x %02x\n", z80->mem[z80->regde+33],
	       z80->mem[z80->regde+34], z80->mem[z80->regde+35]); */
	ofst = ADDRESS;
        z80->mem[DE + FCB_CR] = SEQ_CR(ofst);
	z80->mem[DE + FCB_EX] = SEQ_EX(ofst);
	z80->mem[DE + FCB_S2] = SEQ_S2(ofst);
	goto readseq;
	}
    case 34:	/* write random record */
        {
        long ofst;
	fp = getfp(z80, DE);
	/* printf("data is %02x %02x %02x\n", z80->mem[z80->regde+33],
	       z80->mem[z80->regde+34], z80->mem[z80->regde+35]); */
	ofst = ADDRESS;
        z80->mem[DE + FCB_CR] = SEQ_CR(ofst);
	z80->mem[DE + FCB_EX] = SEQ_EX(ofst);
	z80->mem[DE + FCB_S2] = SEQ_S2(ofst);
	goto writeseq;
	}
    case 35:	/* compute file size */
	fp = getfp(z80, DE);
	fseek(fp, 0L, SEEK_END);
	/* fall through */
    case 36:	/* set random record */
	fp = getfp(z80, DE);
	{   
	    long ofst = ftell(fp);
	    long pos = (ofst >> 7);
	    A = HL = 0x00;	/* dunno, if necessary */
	    B = 0;
	    z80->mem[DE + FCB_R0] = pos & 0xff;
	    z80->mem[DE + FCB_R1] = pos >> 8;
	    z80->mem[DE + FCB_R2] = pos >> 16;
            z80->mem[DE + FCB_CR] = SEQ_CR(ofst);
	    z80->mem[DE + FCB_EX] = SEQ_EX(ofst);
	    z80->mem[DE + FCB_S2] = SEQ_S2(ofst);
	}
	break;
    case 41:
	for (s = (char *)(z80->mem + DE); *s; ++s)
	    *s = tolower(*(unsigned char *)s);
	A = HL = 
	    restricted_mode || chdir((char  *)(z80->mem + DE)) ? 0xff : 0x00;
	B = 0;
	break;
    default:
	printf("\n\nUnrecognized BDOS-Function:\n");
	printf("AF=%04x  BC=%04x  DE=%04x  HL=%04x  SP=%04x\nStack =",
	       AF, BC, DE, HL, SP);
	for (i = 0; i < 8; ++i)
	    printf(" %4x", z80->mem[SP + 2*i]
		   + 256 * z80->mem[SP + 2*i + 1]);
	printf("\n");
	exit(1);
    }
    z80->mem[PC = DIRBUF-1] = 0xc9; /* Return instruction */
    return;
}
