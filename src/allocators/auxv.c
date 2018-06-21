#define _GNU_SOURCE
#include <assert.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <link.h>
#include "relf.h"
#include "liballocs_private.h"
#include "pageindex.h"

static const char *asciiz_start;
static const char *asciiz_end;
static struct uniqtype *asciiz_uniqtype;

static const char **env_vector_start;
static const char **env_vector_terminator;
static struct uniqtype *env_vector_uniqtype;

static const char **argv_vector_start;
static const char **argv_vector_terminator;
static struct uniqtype *argv_vector_uniqtype;

static ElfW(auxv_t) *auxv_array_start;
static ElfW(auxv_t) *auxv_array_terminator;
static struct uniqtype *auxv_array_uniqtype;

static intptr_t *p_argcount;

void *program_entry_point;

/* Delay this bit of the init until we need it, because it depends on libdlbind
 * which is only ready fairly late (after mmap). */
static void init_uniqtypes(void)
{
	auxv_array_uniqtype = __liballocs_get_or_create_array_type(
			pointer_to___uniqtype__Elf64_auxv_t,
			auxv_array_terminator + 1 - auxv_array_start);
	env_vector_uniqtype = __liballocs_get_or_create_array_type(
			pointer_to___uniqtype____PTR_signed_char, 
			env_vector_terminator + 1 - env_vector_terminator);
	argv_vector_uniqtype = __liballocs_get_or_create_array_type(
			pointer_to___uniqtype____PTR_signed_char, *p_argcount + 1);
	asciiz_uniqtype = __liballocs_get_or_create_array_type(
			pointer_to___uniqtype__signed_char, asciiz_end - asciiz_start);
}

static _Bool tried_to_initialize;
void __auxv_allocator_init(void) __attribute__((constructor(101)));
void __auxv_allocator_init(void)
{
	/* We might get called more than once. */
	if (tried_to_initialize) return;
	tried_to_initialize = 1;

	auxv_array_start = get_auxv((const char **) environ, environ[0]);
	if (!auxv_array_start) return;

	struct auxv_limits lims = get_auxv_limits(auxv_array_start);
	asciiz_start = lims.asciiz_start;
	asciiz_end = lims.asciiz_end;
	env_vector_start = lims.env_vector_start;
	env_vector_terminator = lims.env_vector_terminator;
	argv_vector_start = lims.argv_vector_start;
	argv_vector_terminator = lims.argv_vector_terminator;
	auxv_array_terminator = lims.auxv_array_terminator;
	p_argcount = lims.p_argcount;
	
	ElfW(auxv_t) *found_at_entry = auxv_lookup(auxv_array_start, AT_ENTRY);
	if (found_at_entry) program_entry_point = (void*) found_at_entry->a_un.a_val;
}

static liballocs_err_t get_info(void * obj, struct big_allocation *maybe_bigalloc, 
	struct uniqtype **out_type, void **out_base, 
	unsigned long *out_size, const void **out_site)
{
	if (!auxv_array_uniqtype) init_uniqtypes();
	
	/* Decide whether it falls into the asciiz, auxv_t or ptr vector parts. */
	if ((char*) obj >= (char*) auxv_array_start
			&& (char*) obj <= (char*) auxv_array_terminator)
	{
		if (out_type) *out_type = auxv_array_uniqtype;
		if (out_base) *out_base = auxv_array_start;
		if (out_size) *out_size = (auxv_array_terminator + 1 - auxv_array_start) * sizeof (Elf64_auxv_t);
		if (out_site) *out_site = program_entry_point;
		return NULL;
	}
	
	if ((char*) obj >= (char*) argv_vector_start
			&& (char*) obj <= (char*) argv_vector_terminator)
	{
		if (out_type) *out_type = argv_vector_uniqtype;
		if (out_base) *out_base = argv_vector_start;
		if (out_size) *out_size = (argv_vector_terminator + 1 - argv_vector_start) * sizeof (char*);
		if (out_site) *out_site = program_entry_point;
		return NULL;
	}
	
	if ((char*) obj >= (char*) env_vector_start
			&& (char*) obj <= (char*) env_vector_terminator)
	{
		if (out_type) *out_type = env_vector_uniqtype;
		if (out_base) *out_base = env_vector_start;
		if (out_size) *out_size = (env_vector_terminator + 1 - env_vector_start) * sizeof (char*);
		if (out_site) *out_site = program_entry_point;
		return NULL;
	}
	
	if ((char*) obj >= asciiz_start && (char*) obj <= asciiz_end)
	{
		if (out_type) *out_type = asciiz_uniqtype;
		if (out_base) *out_base = (char*) asciiz_start;
		if (out_size) *out_size = asciiz_end - asciiz_start;
		if (out_site) *out_site = program_entry_point;
		return NULL;
	}
	
	if ((char*) obj >= (char*) p_argcount 
		&& (char*) obj <= (char*) p_argcount + sizeof (intptr_t))
	{
		if (out_type) *out_type = pointer_to___uniqtype__intptr_t;
		if (out_base) *out_base = (void*) p_argcount;
		if (out_size) *out_size = sizeof (intptr_t);
		if (out_site) *out_site = program_entry_point;
		return NULL;
	}
	
	return &__liballocs_err_object_of_unknown_storage;
}

/* HACK: we have a special link to the stack allocator. Note that it's
 * "region" because it need not be an integral number of pages, once
 * we've carved out the bits holding the auxv data. */
void __stack_allocator_notify_init_stack_region(void *begin, void *end);

void *__top_of_initial_stack __attribute__((visibility("protected")));
static struct big_allocation *our_bigalloc;
void __auxv_allocator_notify_init_stack_mapping_sequence(struct big_allocation *b)
{
	if (!auxv_array_start) __auxv_allocator_init();
	if (!p_argcount) abort();
	void *begin = b->begin;
	void *end = b->end;
	
	if (our_bigalloc)
	{
		/* We've been here before. Adjust the lower bound of the stack,
		 * which is the *minimum* of the *begins*. */
		if ((char*) our_bigalloc->begin > (char*) begin) our_bigalloc->begin = begin;
		/* We also adjust the upper bound of the stack. The reason is a giant
		 * HACK. After the "[stack]" region which is rwx, there may be a separate rw-
		 * region which contains the asciiz but is a separate /proc line. When we are
		 * first called, we are reading from /proc (on Linux anyway)
		 * and so we haven't seen the /proc line for that yet. So it would be premature
		 * to expand ourselves into that space. But now that we're being called the second
		 * time, it's fair game. FIXME: will the next /proc-processing iteration
		 * clobber this hard work? */
		
		if (asciiz_end > (const char *) our_bigalloc->end)
		{
			const char *new_end = RELF_ROUND_UP_PTR_(asciiz_end, PAGE_SIZE);
			unsigned pi = pageindex[PAGENUM(asciiz_end)];
			_Bool success;
			if (pi)
			{
				_Bool success = __liballocs_truncate_bigalloc_at_beginning(
					&big_allocations[pi], new_end);
				assert(success);
			}
			success  = __liballocs_extend_bigalloc(our_bigalloc, new_end);
			assert(success);
		}
		return;
	}
	__top_of_initial_stack = end; /* i.e. the highest address */
	our_bigalloc = __liballocs_new_bigalloc(
		begin,
		(char*) end - (char*) begin,
		(struct meta_info) {
			.what = DATA_PTR,
			.un = {
				opaque_data: {
					.data_ptr = NULL,
					.free_func = NULL
				}
			}
		},
		NULL,
		&__auxv_allocator
	);
	if (!our_bigalloc) abort();
	
	/* Don't do this. Queries on auxv region are on a "crack" so we don't want
	 * to descend to the suballocator. Recording suballocators is only useful at
	 * leaf level anyway, since the child bigalloc (which can be sized precisely,
	 * leaving the auxv cracks excluded) fills this role at branch level. */
	// our_bigalloc->suballocator = &__stack_allocator;
	__stack_allocator_notify_init_stack_region(begin, p_argcount);
	/* HACK: undo the suballocation relationship created in pageindex. Ideally
	 * it wouldn't do this. But it doesn't know any better... all bigallocs
	 * are initially childless, so it's the right thing to do. */
	our_bigalloc->suballocator = NULL;
}

struct allocator __auxv_allocator = {
	.name = "auxv",
	.is_cacheable = 1,
	.get_info = get_info
};

_Bool __auxv_get_asciiz(const char **out_start, const char **out_end, struct uniqtype **out_uniqtype)
{
	if (out_start) *out_start = asciiz_start;
	if (out_end) *out_end = asciiz_end;
	if (out_uniqtype) *out_uniqtype = asciiz_uniqtype;
	return 1;
}
_Bool __auxv_get_argv(const char ***out_start, const char ***out_terminator, struct uniqtype **out_uniqtype)
{
	if (out_start) *out_start = argv_vector_start;
	if (out_terminator) *out_terminator = argv_vector_terminator;
	if (out_uniqtype) *out_uniqtype = argv_vector_uniqtype;
	return 1;
}

_Bool __auxv_get_env(const char ***out_start, const char ***out_terminator, struct uniqtype **out_uniqtype)
{
	if (out_start) *out_start = env_vector_start;
	if (out_terminator) *out_terminator = env_vector_terminator;
	if (out_uniqtype) *out_uniqtype = env_vector_uniqtype;
	return 1;
}

_Bool __auxv_get_auxv(const Elf64_auxv_t **out_start, Elf64_auxv_t **out_terminator, struct uniqtype **out_uniqtype)
{
	if (out_start) *out_start = auxv_array_start;
	if (out_terminator) *out_terminator = auxv_array_terminator;
	if (out_uniqtype) *out_uniqtype = auxv_array_uniqtype;
	return 1;
}
void *__auxv_get_program_entry_point(void)
{
	return program_entry_point;
}
