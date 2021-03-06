/*
 * rdoff.h	RDOFF Object File manipulation routines header file
 *
 * The Netwide Assembler is copyright (C) 1996 Simon Tatham and
 * Julian Hall. All rights reserved. The software is
 * redistributable under the licence given in the file "Licence"
 * distributed in the NASM archive.
 *
 * Permission to use this file in your own projects is granted, as long
 * as acknowledgement is given in an appropriate manner to its authors,
 * with instructions of how to obtain a copy via ftp.
 */

#ifndef _RDOFF_H
#define _RDOFF_H

/*
 * RDOFF definitions. They are used by RDOFF utilities and by NASM's
 * 'outrdf2.c' output module.
 */

/* Type definitions */
typedef unsigned long uint32;
typedef unsigned short uint16;
typedef unsigned char byte;
typedef unsigned int bool;

/* RDOFF format revision (currently used only when printing the version) */
#define RDOFF2_REVISION		"0.6.1"

/* RDOFF2 file signature */
#define RDOFF2_SIGNATURE	"RDOFF2"

/* Maximum size of an import/export label (including trailing zero) */
#define EXIM_LABEL_MAX		64

/* Maximum size of library or module name (including trailing zero) */
#define MODLIB_NAME_MAX		128

/* Maximum number of segments that we can handle in one file */
#define RDF_MAXSEGS		64

/* Record types that may present the RDOFF header */
#define RDFREC_GENERIC		0
#define RDFREC_RELOC		1
#define RDFREC_IMPORT		2
#define RDFREC_GLOBAL		3
#define RDFREC_DLL		4
#define RDFREC_BSS		5
#define RDFREC_SEGRELOC		6
#define RDFREC_FARIMPORT	7
#define RDFREC_MODNAME		8
#define RDFREC_COMMON		10

/* 
 * Generic record - contains the type and length field, plus a 128 byte
 * char array 'data'
 */
struct GenericRec {
    byte type;
    byte reclen;
    char data[128];
};

/* 
 * Relocation record
 */
struct RelocRec {
    byte type;                  /* must be 1 */
    byte reclen;                /* content length */
    byte segment;               /* only 0 for code, or 1 for data supported,
                                   but add 64 for relative refs (ie do not require
                                   reloc @ loadtime, only linkage) */
    long offset;                /* from start of segment in which reference is loc'd */
    byte length;                /* 1 2 or 4 bytes */
    uint16 refseg;              /* segment to which reference refers to */
};

/*
 * Extern/import record
 */
struct ImportRec {
    byte type;                  /* must be 2 */
    byte reclen;                /* content length */
    byte flags;                 /* SYM_* flags (see below) */
    uint16 segment;             /* segment number allocated to the label for reloc
                                   records - label is assumed to be at offset zero
                                   in this segment, so linker must fix up with offset
                                   of segment and of offset within segment */
    char label[EXIM_LABEL_MAX]; /* zero terminated, should be written to file
                                   until the zero, but not after it */
};

/*
 * Public/export record
 */
struct ExportRec {
    byte type;                  /* must be 3 */
    byte reclen;                /* content length */
    byte flags;                 /* SYM_* flags (see below) */
    byte segment;               /* segment referred to (0/1/2) */
    long offset;                /* offset within segment */
    char label[EXIM_LABEL_MAX]; /* zero terminated as in import */
};

/*
 * DLL record
 */
struct DLLRec {
    byte type;                  /* must be 4 */
    byte reclen;                /* content length */
    char libname[MODLIB_NAME_MAX];      /* name of library to link with at load time */
};

/*
 * BSS record
 */
struct BSSRec {
    byte type;                  /* must be 5 */
    byte reclen;                /* content length */
    long amount;                /* number of bytes BSS to reserve */
};

/*
 * Module name record
 */
struct ModRec {
    byte type;                  /* must be 8 */
    byte reclen;                /* content length */
    char modname[MODLIB_NAME_MAX];      /* module name */
};

/*
 * Common variable record
 */
struct CommonRec {
    byte type;                  /* must be 10 */
    byte reclen;                /* equals 7+label length */
    uint16 segment;             /* segment number */
    long size;                  /* size of common variable */
    uint16 align;               /* alignment (power of two) */
    char label[EXIM_LABEL_MAX]; /* zero terminated as in import */
};

/* Flags for ExportRec */
#define SYM_DATA	1
#define SYM_FUNCTION	2
#define SYM_GLOBAL	4
#define SYM_IMPORT	8

/*** The following part is used only by the utilities *************************/

#ifdef RDOFF_UTILS

/* Some systems don't define this automatically */
#if !defined(strdup)
extern char *strdup(const char *);
#endif

typedef union RDFHeaderRec {
    char type;                  /* invariant throughout all below */
    struct GenericRec g;        /* type 0 */
    struct RelocRec r;          /* type == 1 / 6 */
    struct ImportRec i;         /* type == 2 / 7 */
    struct ExportRec e;         /* type == 3 */
    struct DLLRec d;            /* type == 4 */
    struct BSSRec b;            /* type == 5 */
    struct ModRec m;            /* type == 8 */
    struct CommonRec c;         /* type == 10 */
} rdfheaderrec;

struct SegmentHeaderRec {
    /* information from file */
    uint16 type;
    uint16 number;
    uint16 reserved;
    long length;

    /* information built up here */
    long offset;
    byte *data;                 /* pointer to segment data if it exists in memory */
};

typedef struct RDFFileInfo {
    FILE *fp;                   /* file descriptor; must be open to use this struct */
    int rdoff_ver;              /* should be 1; any higher => not guaranteed to work */
    long header_len;
    long header_ofs;

    byte *header_loc;           /* keep location of header */
    long header_fp;             /* current location within header for reading */

    struct SegmentHeaderRec seg[RDF_MAXSEGS];
    int nsegs;

    long eof_offset;            /* offset of the first byte beyond the end of this
                                   module */

    char *name;                 /* name of module in libraries */
    int *refcount;              /* pointer to reference count on file, or NULL */
} rdffile;

#define BUF_BLOCK_LEN 4088      /* selected to match page size (4096)
                                 * on 80x86 machines for efficiency */
typedef struct memorybuffer {
    int length;
    byte buffer[BUF_BLOCK_LEN];
    struct memorybuffer *next;
} memorybuffer;

typedef struct {
    memorybuffer *buf;          /* buffer containing header records */
    int nsegments;              /* number of segments to be written */
    long seglength;             /* total length of all the segments */
} rdf_headerbuf;

/* segments used by RDOFF, understood by rdoffloadseg */
#define RDOFF_CODE	0
#define RDOFF_DATA	1
#define RDOFF_HEADER	-1
/* mask for 'segment' in relocation records to find if relative relocation */
#define RDOFF_RELATIVEMASK 64
/* mask to find actual segment value in relocation records */
#define RDOFF_SEGMENTMASK 63

extern int rdf_errno;

/* rdf_errno can hold these error codes */
enum {
    /* 0 */ RDF_OK,
    /* 1 */ RDF_ERR_OPEN,
    /* 2 */ RDF_ERR_FORMAT,
    /* 3 */ RDF_ERR_READ,
    /* 4 */ RDF_ERR_UNKNOWN,
    /* 5 */ RDF_ERR_HEADER,
    /* 6 */ RDF_ERR_NOMEM,
    /* 7 */ RDF_ERR_VER,
    /* 8 */ RDF_ERR_RECTYPE,
    /* 9 */ RDF_ERR_RECLEN,
    /* 10 */ RDF_ERR_SEGMENT
};

/* utility functions */
long translatelong(long in);
uint16 translateshort(uint16 in);
char *translatesegmenttype(uint16 type);

/* RDOFF file manipulation functions */
int rdfopen(rdffile * f, const char *name);
int rdfopenhere(rdffile * f, FILE * fp, int *refcount, const char *name);
int rdfclose(rdffile * f);
int rdffindsegment(rdffile * f, int segno);
int rdfloadseg(rdffile * f, int segment, void *buffer);
rdfheaderrec *rdfgetheaderrec(rdffile * f);     /* returns static storage */
void rdfheaderrewind(rdffile * f);      /* back to start of header */
void rdfperror(const char *app, const char *name);

/* functions to write a new RDOFF header to a file -
   use rdfnewheader to allocate a header, rdfaddheader to add records to it,
   rdfaddsegment to notify the header routines that a segment exists, and
   to tell it how long the segment will be.
   rdfwriteheader to write the file id, object length, and header
   to a file, and then rdfdoneheader to dispose of the header */

rdf_headerbuf *rdfnewheader(void);
int rdfaddheader(rdf_headerbuf * h, rdfheaderrec * r);
int rdfaddsegment(rdf_headerbuf * h, long seglength);
int rdfwriteheader(FILE * fp, rdf_headerbuf * h);
void rdfdoneheader(rdf_headerbuf * h);

#endif                          /* RDOFF_UTILS */

#endif                          /* _RDOFF_H */
