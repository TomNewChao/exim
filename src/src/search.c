/*************************************************
*     Exim - an Internet mail transport agent    *
*************************************************/

/* Copyright (c) The Exim Maintainers 2020 - 2023 */
/* Copyright (c) University of Cambridge 1995 - 2015 */
/* See the file NOTICE for conditions of use and distribution. */
/* SPDX-License-Identifier: GPL-2.0-or-later */

/* A set of functions to search databases in various formats. An open
database is represented by a void * value which is returned from a lookup-
specific "open" function. These are now all held in individual modules in the
lookups subdirectory and the functions here form a generic interface.

Caching is used to improve performance. Open files are cached until a tidyup
function is called, and for each file the result of the last lookup is cached.
However, if too many files are opened, some of those that are not in use have
to be closed. Those open items that use real files are kept on a LRU chain to
help with this.

All the data is held in permanent store so as to be independent of the stacking
pool that is reset from time to time. In fact, we use malloc'd store so that it
can be freed when the caches are tidied up. It isn't actually clear whether
this is a benefit or not, to be honest. */

#include "exim.h"


/* Tree in which to cache open files until tidyup called. */

static tree_node *search_tree = NULL;

/* Two-way chain of open databases that use real files. This is maintained in
recently-used order for the purposes of closing the least recently used when
too many files are open. */

static tree_node *open_top = NULL;
static tree_node *open_bot = NULL;

/* Count of open databases that use real files */

static int open_filecount = 0;

/* Allow us to reset store used for lookups and lookup caching */

static rmark search_reset_point = NULL;



/*************************************************
*      Validate a plain lookup type name         *
*************************************************/

/* Only those names that are recognized and whose code is included in the
binary give an OK response. Use a binary chop search now that the list has got
so long.

Arguments:
  name       lookup type name - not necessarily zero terminated (e.g. dbm*)
  len        length of the name

Returns:     +ve => valid lookup name; value is offset in lookup_list
             -ve => invalid name; message in search_error_message.
*/

int
search_findtype(const uschar * name, int len)
{
for (int bot = 0, top = lookup_list_count; top > bot; )
  {
  int mid = (top + bot)/2;
  int c = Ustrncmp(name, lookup_list[mid]->name, len);

  /* If c == 0 we have matched the incoming name with the start of the search
  type name. However, some search types are substrings of others (e.g. nis and
  nisplus) so we need to check that the lengths are the same. The length of the
  type name cannot be shorter (else c would not be 0); if it is not equal it
  must be longer, and in that case, the incoming name comes before the name we
  are testing. By leaving c == 0 when the lengths are different, and doing a
  > 0 test below, this all falls out correctly. */

  if (c == 0 && Ustrlen(lookup_list[mid]->name) == len)
    {
    if (lookup_list[mid]->find != NULL) return mid;
    search_error_message  = string_sprintf("lookup type \"%.*s\" is not "
      "available (not in the binary - check buildtime LOOKUP configuration)",
      len, name);
    return -1;
    }

  if (c > 0) bot = mid + 1; else top = mid;
  }

search_error_message = string_sprintf("unknown lookup type \"%.*s\"", len, name);
return -1;
}



/*************************************************
*       Validate a full lookup type name         *
*************************************************/

/* This function recognizes the "partial-" prefix and also terminating * and *@
suffixes.

Arguments:
  name         the full lookup type name
  ptypeptr     where to put the partial type
                 after subtraction of 1024 or 2048:
                   negative     => no partial matching
                   non-negative => minimum number of non-wild components
  ptypeaff     where to put a pointer to the affix
                 the affix is within name if supplied therein
                 otherwise it's a literal string
  afflen       the length of the affix
  starflags    where to put the SEARCH_STAR and SEARCH_STARAT flags
  opts	       where to put the options

Returns:     +ve => valid lookup name; value is offset in lookup_list
             -ve => invalid name; message in search_error_message.
*/

int
search_findtype_partial(const uschar *name, int *ptypeptr, const uschar **ptypeaff,
  int *afflen, int *starflags, const uschar ** opts)
{
int len, stype;
int pv = -1;
const uschar *ss = name;
const uschar * t;

*starflags = 0;
*ptypeaff = NULL;

/* Check for a partial matching type. It must start with "partial", optionally
followed by a sequence of digits. If this is followed by "-", the affix is the
default "*." string. Otherwise we expect an affix in parentheses. Affixes are a
limited number of characters, not including parens. */

if (Ustrncmp(name, "partial", 7) == 0)
  {
  ss += 7;
  if (isdigit (*ss))
    {
    pv = 0;
    while (isdigit(*ss)) pv = pv*10 + *ss++ - '0';
    }
  else pv = 2;         /* Default number of wild components */

  if (*ss == '(')
    {
    *ptypeaff = ++ss;
    while (ispunct(*ss) && *ss != ')') ss++;
    if (*ss != ')') goto BAD_TYPE;
    *afflen = ss++ - *ptypeaff;
    }
  else if (*ss++ == '-')
    {
    *ptypeaff = US "*.";
    *afflen = 2;
    }
  else
    {
    BAD_TYPE:
    search_error_message = string_sprintf("format error in lookup type \"%s\"",
      name);
    return -1;
    }
  }

/* Now we are left with a lookup name, possibly followed by * or *@,
and then by options starting with a "," */

len = Ustrlen(ss);
if ((t = Ustrchr(ss, '*')))
  {
  len = t - ss;
  *starflags |= (t[1] == '@' ? SEARCH_STARAT : SEARCH_STAR);
  }
else
  t = ss;

if ((t = Ustrchr(t, ',')))
  {
  int l = t - ss;
  if (l < len) len = l;
  *opts = string_copy(t+1);
  }
else
  *opts = NULL;

/* Check for the individual search type. Only those that are actually in the
binary are valid. For query-style types, "partial" and default types are
erroneous. */

stype = search_findtype(ss, len);
if (stype >= 0 && mac_islookup(stype, lookup_querystyle))
  {
  if (pv >= 0)
    {
    search_error_message = string_sprintf("\"partial\" is not permitted "
      "for lookup type \"%s\"", ss);
    return -1;
    }
  if ((*starflags & (SEARCH_STAR|SEARCH_STARAT)) != 0)
    {
    search_error_message = string_sprintf("defaults using \"*\" or \"*@\" are "
      "not permitted for lookup type \"%s\"", ss);
    return -1;
    }
  }

*ptypeptr = pv;
return stype;
}


/* Set the parameters for the three different kinds of lookup.
Arguments:
 search_type	the search-type code
 search		the search-type string
 query		argument for the search; filename or query
 fnamep		pointer to return filename
 opts		options

Return:	keyquery	the search-type (for single-key) or query (for query-type)
 */
uschar *
search_args(int search_type, uschar * search, uschar * query, uschar ** fnamep,
  const uschar * opts)
{
Uskip_whitespace(&query);
if (mac_islookup(search_type, lookup_absfilequery))
  {					/* query-style but with file (sqlite) */
  int sep = ',';

  /* Check options first for new-style file spec */
  if (opts) for (uschar * s; s = string_nextinlist(&opts, &sep, NULL, 0); )
    if (Ustrncmp(s, "file=", 5) == 0)
      {
      *fnamep = s+5;
      return query;
      }

  /* If no filename from options, use old-tyle space-sep prefix on query */
  if (*query == '/')
    {
    uschar * s = query;
    while (*query && !isspace(*query)) query++;
    *fnamep = string_copyn(s, query - s);
    Uskip_whitespace(&query);
    }
  else
    *fnamep = NULL;
  return query;		/* remainder after file skipped */
  }
if (!mac_islookup(search_type, lookup_querystyle))
  {					/* single-key */
  *fnamep = query;
  return search;	/* modifiers important so use "keyquery" for them */
  }
*fnamep = NULL;				/* else query-style */
return query;
}



/*************************************************
*               Release cached resources         *
*************************************************/

/* When search_open is called it caches the "file" that it opens in
search_tree. The name of the tree node is a concatenation of the search type
with the file name. For query-style lookups, the file name is empty. Real files
are normally closed only when this tidyup routine is called, typically at the
end of sections of code where a number of lookups might occur. However, if too
many files are open simultaneously, some get closed beforehand. They can't be
removed from the tree. There is also a general tidyup function which is called
for the lookup driver, if it exists.

First, there is an internal, recursive subroutine.

Argument:    a pointer to a search_openfile tree node
Returns:     nothing
*/

static void
tidyup_subtree(tree_node *t)
{
search_cache * c = (search_cache *)(t->data.ptr);
if (t->left)  tidyup_subtree(t->left);
if (t->right) tidyup_subtree(t->right);
if (c && c->handle && lookup_list[c->search_type]->close)
  lookup_list[c->search_type]->close(c->handle);
}


/* The external entry point

Argument: none
Returns:  nothing
*/

void
search_tidyup(void)
{
int old_pool = store_pool;

DEBUG(D_lookup) debug_printf_indent("search_tidyup called\n");

/* Close individually each cached open file. */

store_pool = POOL_SEARCH;
if (search_tree)
  {
  tidyup_subtree(search_tree);
  search_tree = NULL;
  }
open_top = open_bot = NULL;
open_filecount = 0;

/* Call the general tidyup entry for any drivers that have one. */

for (int i = 0; i < lookup_list_count; i++) if (lookup_list[i]->tidy)
  (lookup_list[i]->tidy)();

if (search_reset_point) search_reset_point = store_reset(search_reset_point);
store_pool = old_pool;
}




/*************************************************
*             Open search database               *
*************************************************/

/* A mode, and lists of owners and groups, are passed over for checking in
the cases where the database is one or more files. Return NULL, with a message
pointed to by message, in cases of error.

For search types that use a file or files, check up on the mode after
opening. It is tempting to do a stat before opening the file, and use it as
an existence check. However, doing that opens a small security loophole in
that the status could be changed before the file is opened. Can't quite see
what problems this might lead to, but you can't be too careful where security
is concerned. Fstat() on an open file can normally be expected to succeed,
but there are some NFS states where it does not.

There are two styles of query: (1) in the "single-key+file" style, a single
key string and a file name are given, for example, for linear searches, DBM
files, or for NIS. (2) In the "query" style, no "filename" is given; instead
just a single query string is passed. This applies to multiple-key lookup
types such as NIS+.

Before opening, scan the tree of cached files to see if this file is already
open for the correct search type. If so, return the saved handle. If not, put
the handle in the tree for possible subsequent use. See search_tidyup above for
closing all the cached files.

A count of open databases which use real files is maintained, and if this
gets too large, we have to close a cached file. Its entry remains in the tree,
but is marked closed.

Arguments:
  filename       the name of the file for single-key+file style lookups,
                 NULL for query-style lookups
  search_type    the type of search required
  modemask       if a real single file is used, this specifies mode bits that
                 must not be set; otherwise it is ignored
  owners         if a real single file is used, this specifies the possible
                 owners of the file; otherwise it is ignored
  owngroups      if a real single file is used, this specifies the possible
                 group owners of the file; otherwise it is ignored

Returns:         an identifying handle for the open database;
                 this is the pointer to the tree block in the
                 cache of open files; return NULL on open failure, with
                 a message in search_error_message
*/

void *
search_open(const uschar * filename, int search_type, int modemask,
  uid_t * owners, gid_t * owngroups)
{
void *handle;
tree_node *t;
search_cache *c;
lookup_info *lk = lookup_list[search_type];
uschar keybuffer[256];
int old_pool = store_pool;

if (filename && is_tainted(filename))
  {
  log_write(0, LOG_MAIN|LOG_PANIC,
    "Tainted filename for search: '%s'", filename);
  return NULL;
  }

/* Change to the search store pool and remember our reset point */

store_pool = POOL_SEARCH;
if (!search_reset_point) search_reset_point = store_mark();

DEBUG(D_lookup) debug_printf_indent("search_open: %s \"%s\"\n", lk->name,
  filename ? filename : US"NULL");

/* See if we already have this open for this type of search, and if so,
pass back the tree block as the handle. The key for the tree node is the search
type plus '0' concatenated with the file name. There may be entries in the tree
with closed files if a lot of files have been opened. */

sprintf(CS keybuffer, "%c%.254s", search_type + '0',
  filename ? filename : US"");

if ((t = tree_search(search_tree, keybuffer)))
  {
  if ((c = (search_cache *)t->data.ptr)->handle)
    {
    DEBUG(D_lookup) debug_printf_indent("  cached open\n");
    store_pool = old_pool;
    return t;
    }
  DEBUG(D_lookup) debug_printf_indent("  cached closed\n");
  }

/* Otherwise, we need to open the file or database - each search type has its
own code, which is now split off into separately compiled modules. Before doing
this, if the search type is one that uses real files, check on the number that
we are holding open in the cache. If the limit is reached, close the least
recently used one. */

if (lk->type == lookup_absfile && open_filecount >= lookup_open_max)
  if (!open_bot)
    log_write(0, LOG_MAIN|LOG_PANIC, "too many lookups open, but can't find "
      "one to close");
  else
    {
    search_cache *c = (search_cache *)(open_bot->data.ptr);
    DEBUG(D_lookup) debug_printf_indent("Too many lookup files open\n  closing %s\n",
      open_bot->name);
    if ((open_bot = c->up))
      ((search_cache *)(open_bot->data.ptr))->down = NULL;
    else
      open_top = NULL;
    ((lookup_list[c->search_type])->close)(c->handle);
    c->handle = NULL;
    open_filecount--;
    }

/* If opening is successful, call the file-checking function if there is one,
and if all is still well, enter the open database into the tree. */

if (!(handle = (lk->open)(filename, &search_error_message)))
  {
  store_pool = old_pool;
  return NULL;
  }

if (  lk->check
   && !lk->check(handle, filename, modemask, owners, owngroups,
	 &search_error_message))
  {
  lk->close(handle);
  store_pool = old_pool;
  return NULL;
  }

/* If this is a search type that uses real files, keep count. */

if (lk->type == lookup_absfile) open_filecount++;

/* If we found a previously opened entry in the tree, re-use it; otherwise
insert a new entry. On re-use, leave any cached lookup data and the lookup
count alone. */

if (!t)
  {
  t = store_get(sizeof(tree_node) + Ustrlen(keybuffer), GET_UNTAINTED);
  t->data.ptr = c = store_get(sizeof(search_cache), GET_UNTAINTED);
  c->item_cache = NULL;
  Ustrcpy(t->name, keybuffer);
  tree_insertnode(&search_tree, t);
  }
else c = t->data.ptr;

c->handle = handle;
c->search_type = search_type;
c->up = c->down = NULL;

store_pool = old_pool;
return t;
}





/*************************************************
*  Internal function: Find one item in database  *
*************************************************/

/* The answer is always put into dynamic store. The last lookup for each handle
is cached.

Arguments:
  handle       the handle from search_open; points to tree node
  filename     the filename that was handed to search_open, or
               NULL for query-style searches
  keystring    the keystring for single-key+file lookups, or
               the querystring for query-style lookups
  cache_rd     FALSE to avoid lookup in cache layer
  opts	       type-specific options

Returns:       a pointer to a dynamic string containing the answer,
               or NULL if the query failed or was deferred; in the
               latter case, search_find_defer is set TRUE; after an unusual
               failure, there may be a message in search_error_message.
*/

static uschar *
internal_search_find(void * handle, const uschar * filename, uschar * keystring,
  BOOL cache_rd, const uschar * opts)
{
tree_node * t = (tree_node *)handle;
search_cache * c = (search_cache *)(t->data.ptr);
expiring_data * e = NULL;	/* compiler quietening */
uschar * data = NULL;
int search_type = t->name[0] - '0';
int old_pool = store_pool;

/* Lookups that return DEFER may not always set an error message. So that
the callers don't have to test for NULL, set an empty string. */

search_error_message = US"";
f.search_find_defer = FALSE;

DEBUG(D_lookup) debug_printf_indent("internal_search_find: file=\"%s\"\n  "
  "type=%s key=\"%s\" opts=%s%s%s\n", filename,
  lookup_list[search_type]->name, keystring,
  opts ? "\"" : "", opts, opts ? "\"" : "");

/* Insurance. If the keystring is empty, just fail. */

if (keystring[0] == 0) return NULL;

/* Use the special store pool for search data */

store_pool = POOL_SEARCH;

/* Look up the data for the key, unless it is already in the cache for this
file. No need to check c->item_cache for NULL, tree_search will do so. Check
whether we want to use the cache entry last so that we can always replace it. */

if (  (t = tree_search(c->item_cache, keystring))
   && (!(e = t->data.ptr)->expiry || e->expiry > time(NULL))
   && (!opts && !e->opts  ||  opts && e->opts && Ustrcmp(opts, e->opts) == 0)
   && cache_rd
   )
  { /* Data was in the cache already; set the pointer from the tree node */
  data = e->data.ptr;
  DEBUG(D_lookup) debug_printf_indent("cached data used for lookup of %s%s%s\n",
    keystring,
    filename ? US"\n  in " : US"", filename ? filename : US"");
  }
else
  {
  uint do_cache = UINT_MAX;
  int keylength = Ustrlen(keystring);

  DEBUG(D_lookup)
    {
    if (t)
      debug_printf_indent("cached data found but %s; ",
	e->expiry && e->expiry <= time(NULL) ? "out-of-date"
	: cache_rd ? "wrong opts" : "no_rd option set");
    debug_printf_indent("%s lookup required for %s%s%s\n",
      filename ? US"file" : US"database",
      keystring,
      filename ? US"\n  in " : US"", filename ? filename : US"");
    if (!filename && is_tainted(keystring))
      {
      debug_printf_indent("                             ");
      debug_print_taint(keystring);
      }
    }

  /* Check that the query, for query-style lookups,
  is either untainted or properly quoted for the lookup type.

  XXX Should we this move into lf_sqlperform() ?  The server-taint check is there.
  */

  if (  !filename && lookup_list[search_type]->quote
     && is_tainted(keystring) && !is_quoted_like(keystring, search_type))
    {
    uschar * s = acl_current_verb();
    if (!s) s = authenticator_current_name();	/* must be before transport */
    if (!s) s = transport_current_name();	/* must be before router */
    if (!s) s = router_current_name();	/* GCC ?: would be good, but not in clang */
    if (!s) s = US"";
#ifdef enforce_quote_protection_notyet
    search_error_message = string_sprintf(
      "tainted search query is not properly quoted%s: %s%s",
      s, keystring);
    f.search_find_defer = TRUE;
#else
     {
      int q = quoter_for_address(keystring);
      /* If we're called from a transport, no privs to open the paniclog;
      the logging punts to using stderr - and that seems to stop the debug
      stream. */
      log_write(0,
	transport_name ? LOG_MAIN : LOG_MAIN|LOG_PANIC,
	"tainted search query is not properly quoted%s: %s", s, keystring);

      DEBUG(D_lookup) debug_printf_indent("search_type %d (%s) quoting %d (%s)\n",
	search_type, lookup_list[search_type]->name,
	q, is_real_quoter(q) ? lookup_list[q]->name : US"none");
     }
#endif
    }

  /* Call the code for the different kinds of search. DEFER is handled
  like FAIL, except that search_find_defer is set so the caller can
  distinguish if necessary. */

  if (lookup_list[search_type]->find(c->handle, filename, keystring, keylength,
	  &data, &search_error_message, &do_cache, opts) == DEFER)
    f.search_find_defer = TRUE;

  /* A record that has been found is now in data, which is either NULL
  or points to a bit of dynamic store. Cache the result of the lookup if
  caching is permitted. Lookups can disable caching, when they did something
  that changes their data. The mysql and pgsql lookups do this when an
  UPDATE/INSERT query was executed.  Lookups can also set a TTL for the
  cache entry; the dnsdb lookup does.
  Finally, the caller can request no caching by setting an option. */

  else if (do_cache)
    {
    DEBUG(D_lookup) debug_printf_indent("%s cache entry\n",
      t ? "replacing old" : "creating new");
    if (!t)	/* No existing entry.  Create new one. */
      {
      int len = keylength + 1;
      /* The cache node value should never be expanded so use tainted mem */
      e = store_get(sizeof(expiring_data) + sizeof(tree_node) + len, GET_TAINTED);
      t = (tree_node *)(e+1);
      memcpy(t->name, keystring, len);
      t->data.ptr = e;
      tree_insertnode(&c->item_cache, t);
      }
      /* Else previous, out-of-date cache entry.  Update with the */
      /* new result and forget the old one */
    e->expiry = do_cache == UINT_MAX ? 0 : time(NULL)+do_cache;
    e->opts = opts ? string_copy(opts) : NULL;
    e->data.ptr = data;
    }

/* If caching was disabled, empty the cache tree. We just set the cache
pointer to NULL here, because we cannot release the store at this stage. */

  else
    {
    DEBUG(D_lookup) debug_printf_indent("lookup forced cache cleanup\n");
    c->item_cache = NULL; 	/* forget all lookups on this connection */
    }
  }

DEBUG(D_lookup)
  {
  if (data)
    debug_printf_indent("lookup yielded: %s\n", data);
  else if (f.search_find_defer)
    debug_printf_indent("lookup deferred: %s\n", search_error_message);
  else debug_printf_indent("lookup failed\n");
  }

/* Return it in new dynamic store in the regular pool */

store_pool = old_pool;
return data ? string_copy(data) : NULL;
}




/*************************************************
* Find one item in database, possibly wildcarded *
*************************************************/

/* This function calls the internal function above; once only if there
is no partial matching, but repeatedly when partial matching is requested.

Arguments:
  handle         the handle from search_open
  filename       the filename that was handed to search_open, or
                   NULL for query-style searches
  keystring      the keystring for single-key+file lookups, or
                   the querystring for query-style lookups
  partial        -1 means no partial matching;
                   otherwise it's the minimum number of components;
  affix          the affix string for partial matching
  affixlen       the length of the affix string
  starflags      SEARCH_STAR and SEARCH_STARAT flags
  expand_setup   pointer to offset for setting up expansion strings;
                 don't do any if < 0
  opts		 type-specific options

Returns:         a pointer to a dynamic string containing the answer,
                 or NULL if the query failed or was deferred; in the
                 latter case, search_find_defer is set TRUE
*/

uschar *
search_find(void * handle, const uschar * filename, uschar * keystring,
  int partial, const uschar * affix, int affixlen, int starflags,
  int * expand_setup, const uschar * opts)
{
tree_node * t = (tree_node *)handle;
BOOL set_null_wild = FALSE, cache_rd = TRUE, ret_key = FALSE;
uschar * yield;

DEBUG(D_lookup)
  {
  if (partial < 0) affixlen = 99;   /* So that "NULL" prints */
  debug_printf_indent("search_find: file=\"%s\"\n  key=\"%s\" "
    "partial=%d affix=%.*s starflags=%x opts=%s%s%s\n",
    filename ? filename : US"NULL",
    keystring, partial, affixlen, affix, starflags,
    opts ? "\"" : "", opts, opts ? "\"" : "");

  }

/* Parse global lookup options. Also, create a new options list with
the global options dropped so that the cache-modifiers are not
used in the cache key. */

if (opts)
  {
  int sep = ',';
  gstring * g = NULL;

  for (uschar * ele; ele = string_nextinlist(&opts, &sep, NULL, 0); )
    if (Ustrcmp(ele, "ret=key") == 0) ret_key = TRUE;
    else if (Ustrcmp(ele, "cache=no_rd") == 0) cache_rd = FALSE;
    else g = string_append_listele(g, ',', ele);

  opts = string_from_gstring(g);
  }

/* Arrange to put this database at the top of the LRU chain if it is a type
that opens real files. */

if (  open_top != (tree_node *)handle 
   && lookup_list[t->name[0]-'0']->type == lookup_absfile)
  {
  search_cache *c = (search_cache *)(t->data.ptr);
  tree_node *up = c->up;
  tree_node *down = c->down;

  /* Cut it out of the list. A newly opened file will a NULL up pointer.
  Otherwise there will be a non-NULL up pointer, since we checked above that
  this block isn't already at the top of the list. */

  if (up)
    {
    ((search_cache *)(up->data.ptr))->down = down;
    if (down)
      ((search_cache *)(down->data.ptr))->up = up;
    else
      open_bot = up;
    }

  /* Now put it at the head of the list. */

  c->up = NULL;
  c->down = open_top;
  if (!open_top) open_bot = t;
  else ((search_cache *)(open_top->data.ptr))->up = t;
  open_top = t;
  }

DEBUG(D_lookup)
  {
  debug_printf_indent("LRU list:\n");
  for (tree_node *t = open_top; t; )
    {
    search_cache *c = (search_cache *)(t->data.ptr);
    debug_printf_indent("  %s\n", t->name);
    if (t == open_bot) debug_printf_indent("  End\n");
    t = c->down;
    }
  }

/* First of all, try to match the key string verbatim. If matched a complete
entry but could have been partial, flag to set up variables. */

yield = internal_search_find(handle, filename, keystring, cache_rd, opts);
if (f.search_find_defer) return NULL;

if (yield) { if (partial >= 0) set_null_wild = TRUE; }

/* Not matched a complete entry; handle partial lookups, but only if the full
search didn't defer. Don't use string_sprintf() to construct the initial key,
just in case the original key is too long for the string_sprintf() buffer (it
*has* happened!). The case of a zero-length affix has to be treated specially.
*/

else if (partial >= 0)
  {
  int len = Ustrlen(keystring);
  uschar * keystring2;

  /* Try with the affix on the front, except for a zero-length affix */

  if (affixlen == 0) keystring2 = keystring; else
    {
    keystring2 = store_get(len + affixlen + 1,
	  is_tainted(keystring) || is_tainted(affix) ? GET_TAINTED : GET_UNTAINTED);
    Ustrncpy(keystring2, affix, affixlen);
    Ustrcpy(keystring2 + affixlen, keystring);
    DEBUG(D_lookup) debug_printf_indent("trying partial match %s\n", keystring2);
    yield = internal_search_find(handle, filename, keystring2, cache_rd, opts);
    if (f.search_find_defer) return NULL;
    }

  /* The key in its entirety did not match a wild entry; try chopping off
  leading components. */

  if (!yield)
    {
    int dotcount = 0;
    uschar * keystring3 = keystring2 + affixlen;

    for(uschar * s = keystring3; *s; ) if (*s++ == '.') dotcount++;

    while (dotcount-- >= partial)
      {
      while (*keystring3 && *keystring3 != '.') keystring3++;

      /* If we get right to the end of the string (which will be the last time
      through this loop), we've failed if the affix is null. Otherwise do one
      last lookup for the affix itself, but if it is longer than 1 character,
      remove the last character if it is ".". */

      if (!*keystring3)
        {
        if (affixlen < 1) break;
        if (affixlen > 1 && affix[affixlen-1] == '.') affixlen--;
        Ustrncpy(keystring2, affix, affixlen);
        keystring2[affixlen] = 0;
        keystring3 = keystring2;
        }
      else
        {
        keystring3 -= affixlen - 1;
        if (affixlen > 0) Ustrncpy(keystring3, affix, affixlen);
        }

      DEBUG(D_lookup) debug_printf_indent("trying partial match %s\n", keystring3);
      yield = internal_search_find(handle, filename, keystring3,
		cache_rd, opts);
      if (f.search_find_defer) return NULL;
      if (yield)
        {
        /* First variable is the wild part; second is the fixed part. Take care
        to get it right when keystring3 is just "*".  Return a de-tainted version
	of the fixed part, on the grounds it has been validated by the lookup. */

        if (expand_setup && *expand_setup >= 0)
          {
          int fixedlength = Ustrlen(keystring3) - affixlen;
          int wildlength = Ustrlen(keystring) - fixedlength - 1;
          *expand_setup += 1;
          expand_nstring[*expand_setup] = keystring;
          expand_nlength[*expand_setup] = wildlength;
          *expand_setup += 1;
	  if (fixedlength < 0) fixedlength = 0;
          expand_nstring[*expand_setup] = string_copyn_taint(
	    keystring + wildlength + 1, fixedlength, GET_UNTAINTED);
          expand_nlength[*expand_setup] = fixedlength;
          }
        break;
        }
      keystring3 += affixlen;
      }
    }

  else set_null_wild = TRUE; /* Matched a wild entry without any wild part */
  }

/* If nothing has been matched, but the option to look for "*@" is set, try
replacing everything to the left of @ by *. After a match, the wild part
is set to the string to the left of the @. */

if (!yield  &&  starflags & SEARCH_STARAT)
  {
  uschar *atat = Ustrrchr(keystring, '@');
  if (atat && atat > keystring)
    {
    int savechar;
    savechar = *--atat;
    *atat = '*';

    DEBUG(D_lookup) debug_printf_indent("trying default match %s\n", atat);
    yield = internal_search_find(handle, filename, atat, cache_rd, opts);
    *atat = savechar;
    if (f.search_find_defer) return NULL;

    if (yield && expand_setup && *expand_setup >= 0)
      {
      *expand_setup += 1;
      expand_nstring[*expand_setup] = keystring;
      expand_nlength[*expand_setup] = atat - keystring + 1;
      *expand_setup += 1;
      expand_nstring[*expand_setup] = keystring;
      expand_nlength[*expand_setup] = 0;
      }
    }
  }

/* If we still haven't matched anything, and the option to look for "*" is set,
try that. If we do match, the first variable (the wild part) is the whole key,
and the second is empty. */

if (!yield  &&  starflags & (SEARCH_STAR|SEARCH_STARAT))
  {
  DEBUG(D_lookup) debug_printf_indent("trying to match *\n");
  yield = internal_search_find(handle, filename, US"*", cache_rd, opts);
  if (yield && expand_setup && *expand_setup >= 0)
    {
    *expand_setup += 1;
    expand_nstring[*expand_setup] = keystring;
    expand_nlength[*expand_setup] = Ustrlen(keystring);
    *expand_setup += 1;
    expand_nstring[*expand_setup] = keystring;
    expand_nlength[*expand_setup] = 0;
    }
  }

/* If this was a potentially partial lookup, and we matched either a
complete non-wild domain entry, or we matched a wild-carded entry without
chopping off any of the domain components, set up the expansion variables
(if required) so that the first one is empty, and the second one is the
fixed part of the domain. The set_null_wild flag is set only when yield is not
NULL.  Return a de-tainted version of the fixed part, on the grounds it has been
validated by the lookup. */

if (set_null_wild && expand_setup && *expand_setup >= 0)
  {
  int fixedlength = Ustrlen(keystring);
  *expand_setup += 1;
  expand_nstring[*expand_setup] = keystring;
  expand_nlength[*expand_setup] = 0;
  *expand_setup += 1;
  expand_nstring[*expand_setup] = string_copyn_taint(
	    keystring, fixedlength, GET_UNTAINTED);
  expand_nlength[*expand_setup] = fixedlength;
  }

/* If we have a result, check the options to see if the key was wanted rather
than the result.  Return a de-tainted version of the key on the grounds that
it have been validated by the lookup. */

if (yield && ret_key)
  yield = string_copy_taint(keystring, GET_UNTAINTED);

return yield;
}

/* End of search.c */
