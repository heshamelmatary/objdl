/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <linux/auxvec.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <pthread.h>

#include <sys/mman.h>

//#include <sys/atomics.h>

/* special private C library header - see Android.mk */
//#include <bionic_tls.h>

#include "linker.h"
#include "linker_debug.h"


#define SO_MAX 64

/* >>> IMPORTANT NOTE - READ ME BEFORE MODIFYING <<<
 *
 * Do NOT use malloc() and friends or pthread_*() code here.
 * Don't use printf() either; it's caused mysterious memory
 * corruption in the past.
 * The linker runs before we bring up libc and it's easiest
 * to make sure it does not depend on any complex libc features
 *
 * open issues / todo:
 *
 * - should we do anything special for STB_WEAK symbols?
 * - are we doing everything we should for ARM_COPY relocations?
 * - cleaner error reporting
 * - configuration for paths (LD_LIBRARY_PATH?)
 * - after linking, set as much stuff as possible to READONLY
 *   and NOEXEC
 * - linker hardcodes PAGE_SIZE and PAGE_MASK because the kernel
 *   headers provide versions that are negative...
 * - allocate space for soinfo structs dynamically instead of
 *   having a hard limit (64)
 *
 * features to add someday:
 *
 * - dlopen() and friends
 *
*/


static int link_image(soinfo *si, unsigned wr_offset);

static int socount = 0;
static soinfo sopool[SO_MAX];
static soinfo *freelist = NULL;
static soinfo *solist = NULL;
static soinfo *sonext = NULL;

int debug_verbosity;

static soinfo *alloc_info(const char *name)
{
    soinfo *si;

    if(strlen(name) >= SOINFO_NAME_LEN) {
        ERROR("library name %s too long\n", name);
        return 0;
    }

    /* The freelist is populated when we call free_info(), which in turn is
       done only by dlclose(), which is not likely to be used.
    */
    if (!freelist) {
        if(socount == SO_MAX) {
            ERROR("too many libraries when loading %s\n", name);
            return NULL;
        }
        freelist = sopool + socount++;
        freelist->next = NULL;
    }

    si = freelist;
    freelist = freelist->next;

    /* Make sure we get a clean block of soinfo */
    memset(si, 0, sizeof(soinfo));
    strcpy((char*) si->name, name);
    si->ba_index = -1; /* by default, prelinked */
    si->next = NULL;
    si->refcount = 0;

    TRACE("name %s: allocated soinfo @ %p\n", name, si);
    return si;
}

static void free_info(soinfo *si)
{
    soinfo *prev = NULL, *trav;

    TRACE("name %s: freeing soinfo @ %p\n", si->name, si);

    for(trav = solist; trav != NULL; trav = trav->next){
        if (trav == si)
            break;
        prev = trav;
    }
    if (trav == NULL) {
        /* si was not ni solist */
        ERROR("name %s is not in solist!\n", si->name);
        return;
    }

    /* prev will never be NULL, because the first entry in solist is 
       always the static libdl_info.
    */
    prev->next = si->next;
    si->next = freelist;
    freelist = si;
}

static const char *sopaths[] = {
    ".",
    0
};

static int _open_lib(const char *name)
{
    int fd;
    struct stat filestat;

    if ((stat(name, &filestat) >= 0) && S_ISREG(filestat.st_mode)) {
        if ((fd = open(name, O_RDONLY)) >= 0)
            return fd;
    }

    return -1;
}

/* TODO: Need to add support for initializing the so search path with
 * LD_LIBRARY_PATH env variable for non-setuid programs. */
static int open_library(const char *name)
{
    int fd;
    char buf[512];
    const char **path;

    TRACE("[ opening %s ]\n", name);

    if(name == 0) return -1;
    if(strlen(name) > 256) return -1;

    if ((name[0] == '/') && ((fd = _open_lib(name)) >= 0))
        return fd;

    for (path = sopaths; *path; path++) {
        snprintf(buf, sizeof(buf), "%s/%s", *path, name);
        if ((fd = _open_lib(buf)) >= 0)
            return fd;
    }

    return -1;
}

/* temporary space for holding the first page of the shared lib
 * which contains the elf header (with the pht). */
static unsigned char __header[PAGE_SIZE];

/* verify_elf_object
 *      Verifies if the object @ base is a valid ELF object
 *
 * Args:
 *
 * Returns:
 *       0 on success
 *      -1 if no valid ELF object is found @ base.
 */
static int
verify_elf_object(void *base, const char *name)
{
    Elf32_Ehdr *hdr = (Elf32_Ehdr *) base;

    if (hdr->e_ident[EI_MAG0] != ELFMAG0) return -1;
    if (hdr->e_ident[EI_MAG1] != ELFMAG1) return -1;
    if (hdr->e_ident[EI_MAG2] != ELFMAG2) return -1;
    if (hdr->e_ident[EI_MAG3] != ELFMAG3) return -1;

    if (hdr->e_type != ET_REL) {
	    ERROR("error object file type\n");
	    return -1;
    }

    /* TODO: Should we verify anything else in the header? */

    return 0;
}

static void elf_loadsection(int fd, Elf32_Shdr *s, char *q)
{
	lseek(fd, s->sh_offset, SEEK_SET);
	read(fd, q, s->sh_size);
}

static const struct kernel_symbol *
resolve_symbol(Elf32_Shdr *sechdrs, char *name)
{
	//to do
}

//update all symbols
static void update_symbols(Elf32_Shdr *sechdrs, 
			unsigned int symindex, 
			char *strtab)
{
	Elf32_Sym *sym = (void *)sechdrs[symindex].sh_addr;
	unsigned int i, num = sechdrs[symindex].sh_size / sizeof(Elf32_Sym);
	int ret = 0;
	unsigned long secbase;
	struct dl_symbol *dlsym;

	TRACE("%d total symbols\n", num);
	for (i = 1; i < num; i++) {//ignore the first one entry
		TRACE("%d symbol\n", i);
		switch (sym[i].st_shndx) {
		case SHN_UNDEF://extern symbol
			TRACE("UNDEF symbol\n");
			dlsym = resolve_symbol(sechdrs, strtab + sym[i].st_name);
			if (dlsym) {
				sym[i].st_value = dlsym->value;
			} else {
				ERROR("Unknown symbol %s\n", 
					strtab + sym[i].st_name);
				exit(-1);
			}
			break;
		case SHN_ABS://do nothing;
			TRACE("ABS symbol\n");
			break;			
		default://internal symbol
			TRACE("internal symbol\n");
			sym[i].st_value = sechdrs[sym[i].st_shndx].sh_addr;
			break;
		}
	}
	return ret;
}

static int
do_relocate(Elf32_Shdr *sechdrs, unsigned int symindex, unsigned int relsec)
{
	int i, num;
	uint32_t *where;
	Elf32_Sym *sym;
	Elf32_Rel *rel = (void *)sechdrs[relsec].sh_addr;

	num = sechdrs[relsec].sh_size/sizeof(*rel);
	TRACE("%d relocations\n", num);
	for (i = 0; i < num; i++) {
		where = (void *)sechdrs[sechdrs[relsec].sh_info].sh_addr
			+ rel[i].r_offset;
		sym = (Elf32_Sym *)sechdrs[symindex].sh_addr
			+ ELF32_R_SYM(rel[i].r_info);

		switch (ELF32_R_TYPE(rel[i].r_info)) {
		case R_386_32://s+a
			*where += sym->st_value;
			break;
		case R_386_PC32://s+a=p
			/* Add the value, subtract its postition */
			*where += sym->st_value - (uint32_t)where;
			break;
		default:
			ERROR("unknown/unsupported relocation type: %u\n",
			       ELF32_R_TYPE(rel[i].r_info));
			return -1;
			break;
		}
	}
	return 0;
}
typedef int (*void_fn_void_t)(void);//for test
static soinfo *
load_library(const char *name)
{
	int fd = open_library(name);
	int i, cnt, err;
	unsigned entry;
	soinfo *si = NULL;
	Elf32_Ehdr hdr;
	Elf32_Shdr *sechdrs, *p;
	char *sname, *q, *shstrtbl;
	int totalsize = 0;
	unsigned int symindex = 0;

	if(fd == -1)
		return NULL;

	/* We have to read the ELF header to figure out what to do with this image
	*/
	TRACE("loading elf header...\n");
	if (lseek(fd, 0, SEEK_SET) < 0) {
		ERROR("lseek() failed!\n");
		goto fail;
	}
	if ((cnt = read(fd, &hdr, sizeof(hdr))) < 0) {
		ERROR("read() failed!\n");
		goto fail;
	}
	if (verify_elf_object(&hdr, name) < 0) {
        	ERROR("%s is not a valid ELF object\n", name);
		goto fail;
	}

	si = alloc_info(name);
	if (si == NULL)
		goto fail;

	TRACE("loading %d section headers...\n", hdr.e_shnum);
	sechdrs = calloc(sizeof(Elf32_Shdr), hdr.e_shnum);
	if (sechdrs == NULL) {
		ERROR("NO Memory\n");
		goto fail;
	}
	if (lseek(fd, hdr.e_shoff, SEEK_SET) < 0) {
		ERROR("lseek() failed!\n");
		goto fail;
	}
	i = hdr.e_shnum * sizeof(*sechdrs);
	if (read(fd, sechdrs, i) != i) {
		ERROR("read failed!\n");
		goto fail;
	}
	
	TRACE("loading section name string table...\n");
	p = sechdrs + hdr.e_shstrndx;
	shstrtbl = calloc(p->sh_size, 1);
	if (shstrtbl == NULL) {
		ERROR("NO Memory\n");
		goto fail;
	}
	if (lseek(fd, p->sh_offset, SEEK_SET) < 0) {
		ERROR("lseek() failed!\n");
		goto fail;
	}
	if (read(fd, shstrtbl, p->sh_size) != p->sh_size) {
		ERROR("read failed!\n");
		goto fail;
	}

	TRACE("collecting info of needed sections...\n");
	for (i = 0; i < hdr.e_shnum; i++) {
		p = sechdrs + i;
		sname = shstrtbl + p->sh_name;
		switch (p->sh_type) {
			case SHT_PROGBITS:
				if (!strcmp(sname,".data") ||
					!strcmp(sname,".text")) {
					totalsize += p->sh_size;
					TRACE("section: %s\n", sname);
				}
				break;
			case SHT_NOBITS:
				TRACE("section: %s\n", sname);
				totalsize += p->sh_size;
				break;
			case SHT_SYMTAB:
				TRACE("section: %s\n", sname);
				symindex = i;
				totalsize += p->sh_size;
				break;
			case SHT_RELA:
			case SHT_REL:
				if (!strcmp(sname,".data") ||
					!strcmp(sname,".text")) {
					totalsize += p->sh_size;
					TRACE("section: %s\n", sname);
				}
				break;
		}
	}
	q = si->image = calloc(1, totalsize);
	if (q == NULL) {
		ERROR("NO Memory\n");
		goto fail;
	}
	TRACE("loading needed sections...\n");
	for (i = 0; i < hdr.e_shnum; i++) {
		p = sechdrs + i;
		sname = shstrtbl + p->sh_name;
		TRACE("check section: %s\n", sname);
		switch (p->sh_type) {
			case SHT_PROGBITS:
				if (!strcmp(sname,".data") ||
					!strcmp(sname,".text")){
					TRACE("loading section: %s\n", sname);
					elf_loadsection(fd, p, q);
				}
				//test to see resolving and relocation works
				/*if (!strcmp(sname, ".text")) {
					entry = q + p->sh_size;
				}*/
				break;
			case SHT_NOBITS:
			case SHT_SYMTAB:
				TRACE("loading section: %s\n", sname);
				elf_loadsection(fd, p, q);
				break;
			case SHT_RELA:
			case SHT_REL:
				if (!strcmp(sname,".data") ||
					!strcmp(sname,".text")){
					TRACE("loading section: %s\n", sname);
					elf_loadsection(fd, p, q);
					elf_loadsection(fd, p, q);
				}
				break;
		}
		q += p->sh_size;
	}

	TRACE("updating symbols...\n");
	update_symbols(sechdrs, symindex, shstrtbl);

	//relocation
	TRACE("relocating...\n");
	for (i = 1; i < hdr.e_shnum; i++) {
		if (sechdrs[i].sh_type == SHT_REL)
			err = do_relocate(sechdrs, symindex, i);
		else if (sechdrs[i].sh_type == SHT_RELA) {
			//todo
		}
		if (err != 0)
			goto fail;
	}
	//TRACE("%d\n", ((void_fn_void_t)entry)());
  
	close(fd);
	return si;

fail:
	if (si) free_info(si);
		close(fd);
	return NULL;
}

soinfo *find_library(const char *name)
{
	soinfo *si;

	for(si = solist; si != 0; si = si->next){
		if(!strcmp(name, si->name)) {
			if(si->flags & FLAG_ERROR) return 0;
			if(si->flags & FLAG_LINKED) return si;
			ERROR("OOPS: recursive link to '%s'\n", si->name);
			return 0;
		}
	}

	TRACE("[ '%s' has not been loaded yet.  Locating...]\n", name);
	si = load_library(name);
	if(si == NULL)
		return NULL;
//	return init_library(si);
	return si;
}

unsigned unload_library(soinfo *si)
{
	//todo
}

//read the core sym and initialize
void __linker_init(char *filename, struct dl_symbol *sym)
{
	FILE *infile;
	char buffer[BUFSIZ/2];
	struct dl_symbol *entry;
	struct dl_symbol *p = sym;
	static int initialized = 0;

	if (initialized == 1)
		return;
	infile = fopen(filename, "r");
	if (!infile) {
		ERROR("Couldn't open file %s for reading.\n", filename);
		exit(-1);
	}
	
	while (fgets(buffer, sizeof(buffer), infile)) {
		entry = malloc(sizeof(*entry));
		if (!entry) {
			ERROR("No Memory\n");
			exit(-1);
		}
		buffer[8] = '\0';
		entry->value = strtoul(buffer, NULL, 16);
		entry->name = strdup(buffer + 11);
		p->next = entry;
		p = entry;
	}
	p->next = NULL;

	fclose(infile);
	initialized = 1;

	//test
	/*p = sym;
	while (p != NULL) {
		printf("%x %s\n", p->value, p->name);
		p = p->next;
	}*/
}
