/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

 /* $Id: xfrout.c,v 1.10 1999/09/28 13:50:04 gson Exp $ */

#include <config.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/types.h>

#include <isc/assertions.h>
#include <isc/error.h>
#include <isc/mem.h>
#include <isc/result.h>

#include <dns/types.h>
#include <dns/result.h>
#include <dns/name.h>
#include <dns/fixedname.h>
#include <dns/rdata.h>
#include <dns/rdatalist.h>
#include <dns/rdataset.h>
#include <dns/rdatastruct.h>
#include <dns/rdatasetiter.h>
#include <dns/db.h>
#include <dns/dbiterator.h>
#include <dns/dbtable.h>
#include <dns/message.h>
#include <dns/journal.h>
#include <dns/view.h>

#include <named/client.h>
#include <named/xfrout.h>

/*
 * Outgoing AXFR and IXFR.
 */

/*
 * TODO:
 *  - timeouts
 *  - IXFR over UDP (needs client to export UDP/TCP flag
 */

#define FAIL(code) do { result = (code); goto failure; } while (0)
#define FAILMSG(code, msg) do { printf("%s\n", msg); \
				result = (code); goto failure; } while (0)
#define CHECK(op) do { result = (op); \
		       if (result != DNS_R_SUCCESS) goto failure; \
		     } while (0)

/**************************************************************************/
/*
 * A db_rr_iterator_t is an iterator that iterates over an entire database,
 * returning one RR at a time, in some arbitrary order. 
 */
	
typedef struct db_rr_iterator db_rr_iterator_t;

struct db_rr_iterator {
	dns_result_t		result;
	dns_db_t		*db;
    	dns_dbiterator_t 	*dbit;
	dns_dbversion_t 	*ver;
	isc_stdtime_t		now;
	dns_dbnode_t		*node;
	dns_fixedname_t		fixedname;
    	dns_rdatasetiter_t 	*rdatasetit;
	dns_rdataset_t 		rdataset;
	dns_rdata_t		rdata;
};

dns_result_t db_rr_iterator_init(db_rr_iterator_t *it, dns_db_t *db,
				 dns_dbversion_t *ver, isc_stdtime_t now);

dns_result_t db_rr_iterator_first(db_rr_iterator_t *it);

dns_result_t db_rr_iterator_next(db_rr_iterator_t *it);

void db_rr_iterator_current(db_rr_iterator_t *it, dns_name_t **name, 
			    isc_uint32_t *ttl, dns_rdata_t **rdata);

void db_rr_iterator_destroy(db_rr_iterator_t *it);

dns_result_t
db_rr_iterator_init(db_rr_iterator_t *it, dns_db_t *db, dns_dbversion_t *ver,
		    isc_stdtime_t now)
{
	dns_result_t result;
	it->db = db;
	it->dbit = NULL;
	it->ver = ver;
	it->now = now;
	it->node = NULL;
	result = dns_db_createiterator(it->db, ISC_FALSE, &it->dbit);
	if (result != DNS_R_SUCCESS)
		return (result);
	it->rdatasetit = NULL;
	dns_rdataset_init(&it->rdataset);
	dns_fixedname_init(&it->fixedname);
	INSIST(! dns_rdataset_isassociated(&it->rdataset));
	it->result = DNS_R_SUCCESS;
	return (it->result);
}

dns_result_t
db_rr_iterator_first(db_rr_iterator_t *it) {
	it->result = dns_dbiterator_first(it->dbit);
	if (it->result != DNS_R_SUCCESS)
		return (it->result);
	it->result = dns_dbiterator_current(it->dbit, &it->node,
				    dns_fixedname_name(&it->fixedname));
	if (it->result != DNS_R_SUCCESS) 
		return (it->result);

	it->result = dns_db_allrdatasets(it->db, it->node,
					 it->ver, it->now,
					 &it->rdatasetit);
	if (it->result != DNS_R_SUCCESS) 
		return (it->result);
		
	it->result = dns_rdatasetiter_first(it->rdatasetit);
	if (it->result != DNS_R_SUCCESS) 
		return (it->result);

	dns_rdatasetiter_current(it->rdatasetit, &it->rdataset);
	
	it->result = dns_rdataset_first(&it->rdataset);
	return (it->result);
}


dns_result_t
db_rr_iterator_next(db_rr_iterator_t *it) {
	if (it->result != DNS_R_SUCCESS)
		return (it->result);

	INSIST(it->dbit != NULL);
	INSIST(it->node != NULL);
	INSIST(it->rdatasetit != NULL);

	it->result = dns_rdataset_next(&it->rdataset);
	if (it->result == DNS_R_NOMORE) {
		dns_rdataset_disassociate(&it->rdataset);
		it->result = dns_rdatasetiter_next(it->rdatasetit);
		while (it->result == DNS_R_NOMORE) {
			dns_rdatasetiter_destroy(&it->rdatasetit);
			dns_db_detachnode(it->db, &it->node);
			it->result = dns_dbiterator_next(it->dbit);
			if (it->result == DNS_R_NOMORE) {
				/* We are at the end of the entire database. */
				return (it->result);
			}
			if (it->result != DNS_R_SUCCESS)
				return (it->result);
			it->result = dns_dbiterator_current(it->dbit,
				    &it->node,
				    dns_fixedname_name(&it->fixedname));
			if (it->result != DNS_R_SUCCESS)
				return (it->result);
			it->result = dns_db_allrdatasets(it->db, it->node,
					 it->ver, it->now,
					 &it->rdatasetit);
			if (it->result != DNS_R_SUCCESS)
				return (it->result);
			it->result = dns_rdatasetiter_first(it->rdatasetit);
		}
		if (it->result != DNS_R_SUCCESS)
			return (it->result);
		dns_rdatasetiter_current(it->rdatasetit, &it->rdataset);
	}
	return (it->result);
}

void
db_rr_iterator_destroy(db_rr_iterator_t *it) {
	if (dns_rdataset_isassociated(&it->rdataset))
		dns_rdataset_disassociate(&it->rdataset);
	if (it->rdatasetit != NULL)
		dns_rdatasetiter_destroy(&it->rdatasetit);
	if (it->node != NULL)
		dns_db_detachnode(it->db, &it->node);
	dns_dbiterator_destroy(&it->dbit);
}

void
db_rr_iterator_current(db_rr_iterator_t *it, dns_name_t **name,
		      isc_uint32_t *ttl, dns_rdata_t **rdata)
{
	REQUIRE(name != NULL && *name == NULL);
	REQUIRE(it->result == DNS_R_SUCCESS);
	*name = dns_fixedname_name(&it->fixedname);
	*ttl = it->rdataset.ttl;
	dns_rdataset_current(&it->rdataset, &it->rdata);
	*rdata = &it->rdata;
}

/**************************************************************************/

/* Print an RR (for debugging) */

static void
dns_rr_print(dns_name_t *name, dns_rdata_t *rdata, isc_uint32_t ttl) {
	dns_result_t result;

	isc_buffer_t buf;
	char mem[2000];
	isc_region_t r;
	dns_rdatalist_t rdl;
	dns_rdataset_t rds;

	rdl.type = rdata->type;
	rdl.rdclass = rdata->rdclass;
	rdl.ttl = ttl;
	ISC_LIST_INIT(rdl.rdata);
	ISC_LINK_INIT(&rdl, link);
	dns_rdataset_init(&rds);
	ISC_LIST_APPEND(rdl.rdata, rdata, link);
	RUNTIME_CHECK(dns_rdatalist_tordataset(&rdl, &rds) == DNS_R_SUCCESS);
		
	isc_buffer_init(&buf, mem, sizeof(mem), ISC_BUFFERTYPE_TEXT);
	result = dns_rdataset_totext(&rds, name,
				     ISC_FALSE, ISC_FALSE, &buf);

	if (result == DNS_R_SUCCESS) {
		isc_buffer_used(&buf, &r);
		printf("%.*s", (int) r.length, (char *) r.base);
	} else {
		printf("<diff RR too long to print>\n");
	}
}

/**************************************************************************/
/*
 * An 'rrstream_t' is a polymorphic iterator that returns
 * a stream of resource records.  There are multiple implementations,
 * e.g. for generating AXFR and IXFR records streams.
 */

typedef struct rrstream_methods rrstream_methods_t;

typedef struct rrstream {
	isc_mem_t 		*mctx;
	rrstream_methods_t	*methods;
} rrstream_t;

struct rrstream_methods {	
	dns_result_t 		(*first)(rrstream_t *);
	dns_result_t 		(*next)(rrstream_t *);
	void			(*current)(rrstream_t *,
					   dns_name_t **,
					   isc_uint32_t *,
					   dns_rdata_t **);
	void 			(*destroy)(rrstream_t **);
};

/**************************************************************************/
/*
 * An 'ixfr_rrstream_t' is an 'rrstream_t' that returns
 * an IXFR-like RR stream from a journal file.
 *
 * The SOA at the beginning of each sequence of additions
 * or deletions are included in the stream, but the extra
 * SOAs at the beginning and end of the entire transfer are
 * not included.
 */

typedef struct ixfr_rrstream {
	rrstream_t		common;
	dns_journal_t 		*journal;
} ixfr_rrstream_t;

/* Forward declarations. */
static void ixfr_rrstream_destroy(rrstream_t **sp);
static rrstream_methods_t ixfr_rrstream_methods;


/*
 * Returns: anything dns_journal_open() or dns_journal_iter_init()
 * may return.
 */

/* XXX ultimately, this should have a "zone" argument */

static dns_result_t
ixfr_rrstream_create(isc_mem_t *mctx,
		      isc_uint32_t begin_serial,
		      isc_uint32_t end_serial,
		      rrstream_t **sp)
{
	ixfr_rrstream_t *s;
	dns_result_t result;

	INSIST(sp != NULL && *sp == NULL);

	s = isc_mem_get(mctx, sizeof(*s));
	if (s == NULL)
		return (DNS_R_NOMEMORY);
	s->common.mctx = mctx;
	s->common.methods = &ixfr_rrstream_methods;
	s->journal = NULL;
	
	CHECK(dns_journal_open(mctx, "journal" /* XXX */,
			       ISC_FALSE, &s->journal));
	CHECK(dns_journal_iter_init(s->journal, begin_serial, end_serial));

	*sp = (rrstream_t *) s;
	return (DNS_R_SUCCESS);

 failure:
	ixfr_rrstream_destroy((rrstream_t **) &s);
	return (result);
}

static dns_result_t
ixfr_rrstream_first(rrstream_t *rs)
{
	ixfr_rrstream_t *s = (ixfr_rrstream_t *) rs;
	return (dns_journal_first_rr(s->journal));
}

static dns_result_t
ixfr_rrstream_next(rrstream_t *rs)
{
	ixfr_rrstream_t *s = (ixfr_rrstream_t *) rs;	
	return (dns_journal_next_rr(s->journal));
}


static void
ixfr_rrstream_current(rrstream_t *rs,
		       dns_name_t **name, isc_uint32_t *ttl,
		       dns_rdata_t **rdata)
{
	ixfr_rrstream_t *s = (ixfr_rrstream_t *) rs;	
	dns_journal_current_rr(s->journal, name, ttl, rdata);
}

static void
ixfr_rrstream_destroy(rrstream_t **rsp) {
	ixfr_rrstream_t *s = (ixfr_rrstream_t *) *rsp;
	if (s->journal != 0)
		dns_journal_destroy(&s->journal);
	isc_mem_put(s->common.mctx, s, sizeof(*s));
}

static rrstream_methods_t ixfr_rrstream_methods = 
{
	ixfr_rrstream_first,
	ixfr_rrstream_next,
	ixfr_rrstream_current,		
	ixfr_rrstream_destroy
};

/**************************************************************************/
/*
 * An 'ixfr_rrstream_t' is an 'rrstream_t' that returns
 * an AXFR-like RR stream from a database.
 *
 * The SOAs at the beginning and end of the transfer are
 * not included in the stream.
 */

typedef struct axfr_rrstream {
	rrstream_t		common;
	int 			state;
	db_rr_iterator_t		it;
	isc_boolean_t		it_valid;
} axfr_rrstream_t;

/* Forward declarations. */
static void axfr_rrstream_destroy(rrstream_t **rsp);
static rrstream_methods_t axfr_rrstream_methods;

/* XXX ultimately, this should have a "zone" argument */
static dns_result_t
axfr_rrstream_create(isc_mem_t *mctx,
		      dns_db_t *db,
		      dns_dbversion_t *ver,
		      rrstream_t **sp)
{
	axfr_rrstream_t *s;
	dns_result_t result;

	INSIST(sp != NULL && *sp == NULL);

	s = isc_mem_get(mctx, sizeof(*s));
	if (s == NULL)
		return (DNS_R_NOMEMORY);
	s->common.mctx = mctx;
	s->common.methods = &axfr_rrstream_methods;
	s->it_valid = ISC_FALSE;

	CHECK(db_rr_iterator_init(&s->it, db, ver, 0));
	s->it_valid = ISC_TRUE;

	*sp = (rrstream_t *) s;
	return (DNS_R_SUCCESS);

 failure:
	axfr_rrstream_destroy((rrstream_t **) &s);
	return (result);
}

static dns_result_t
axfr_rrstream_first(rrstream_t *rs)
{
	axfr_rrstream_t *s = (axfr_rrstream_t *) rs;
	dns_result_t result;
	result = db_rr_iterator_first(&s->it);
	/* Skip SOA records. */
	for (;;) {
		dns_name_t *name_dummy = NULL;
		isc_uint32_t ttl_dummy;
		dns_rdata_t *rdata = NULL;
		db_rr_iterator_current(&s->it, &name_dummy,
				      &ttl_dummy, &rdata);
		if (rdata->type != dns_rdatatype_soa)
			break;
		result = db_rr_iterator_next(&s->it);
		if (result != DNS_R_SUCCESS)
			break;
	}
	return (result);
}

static dns_result_t
axfr_rrstream_next(rrstream_t *rs)
{
	axfr_rrstream_t *s = (axfr_rrstream_t *) rs;
	dns_result_t result;
	
	/* Skip SOA records. */
	for (;;) {
		dns_name_t *name_dummy = NULL;
		isc_uint32_t ttl_dummy;
		dns_rdata_t *rdata = NULL;
		result = db_rr_iterator_next(&s->it);
		if (result != DNS_R_SUCCESS)
			break;
		db_rr_iterator_current(&s->it, &name_dummy,
				      &ttl_dummy, &rdata);
		if (rdata->type != dns_rdatatype_soa)
			break;
	}
	return (result);
}

static void
axfr_rrstream_current(rrstream_t *rs,
		       dns_name_t **name, isc_uint32_t *ttl,
		       dns_rdata_t **rdata)
{
	axfr_rrstream_t *s = (axfr_rrstream_t *) rs;
	db_rr_iterator_current(&s->it, name, ttl, rdata);
}

static void
axfr_rrstream_destroy(rrstream_t **rsp) {
	axfr_rrstream_t *s = (axfr_rrstream_t *) *rsp;
	if (s->it_valid)
		db_rr_iterator_destroy(&s->it); 
	isc_mem_put(s->common.mctx, s, sizeof(*s));
}

static rrstream_methods_t axfr_rrstream_methods = 
{
	axfr_rrstream_first,
	axfr_rrstream_next,
	axfr_rrstream_current,		
	axfr_rrstream_destroy
};

/**************************************************************************/
/*
 * An 'soa_rrstream_t' is a degenerate 'rrstream_t' that returns
 * a single SOA record.
 */

typedef struct soa_rrstream {
	rrstream_t		common;
	dns_difftuple_t 	*soa_tuple;
} soa_rrstream_t;

/* Forward declarations. */
static void soa_rrstream_destroy(rrstream_t **rsp);
static rrstream_methods_t soa_rrstream_methods;

static dns_result_t
soa_rrstream_create(isc_mem_t *mctx,
		    dns_db_t *db,
		    dns_dbversion_t *ver,
		    rrstream_t **sp)
{
	soa_rrstream_t *s;
	dns_result_t result;

	INSIST(sp != NULL && *sp == NULL);

	s = isc_mem_get(mctx, sizeof(*s));
	if (s == NULL)
		return (DNS_R_NOMEMORY);
	s->common.mctx = mctx;
	s->common.methods = &soa_rrstream_methods;
	s->soa_tuple = NULL;
	
	CHECK(dns_db_createsoatuple(db, ver, mctx, DNS_DIFFOP_EXISTS,
				    &s->soa_tuple));

	*sp = (rrstream_t *) s;
	return (DNS_R_SUCCESS);

 failure:
	soa_rrstream_destroy((rrstream_t **) &s);
	return (result);
}

static dns_result_t
soa_rrstream_first(rrstream_t *rs) {
	rs = rs; /* Unused */
	return (DNS_R_SUCCESS);
}

static dns_result_t
soa_rrstream_next(rrstream_t *rs) {
	rs = rs; /* Unused */
	return (DNS_R_NOMORE);
}

static void
soa_rrstream_current(rrstream_t *rs,
		     dns_name_t **name, isc_uint32_t *ttl,
		     dns_rdata_t **rdata)
{
	soa_rrstream_t *s = (soa_rrstream_t *) rs;	
	*name = &s->soa_tuple->name;
	*ttl = s->soa_tuple->ttl;
	*rdata = &s->soa_tuple->rdata;
}

static void
soa_rrstream_destroy(rrstream_t **rsp) {
	soa_rrstream_t *s = (soa_rrstream_t *) *rsp;
	if (s->soa_tuple != NULL)
		dns_difftuple_free(&s->soa_tuple);
	isc_mem_put(s->common.mctx, s, sizeof(*s));
}

static rrstream_methods_t soa_rrstream_methods = 
{
	soa_rrstream_first,
	soa_rrstream_next,
	soa_rrstream_current,		
	soa_rrstream_destroy
};

/**************************************************************************/
/*
 * A 'compound_rrstream_t' objects owns a soa_rrstream
 * and another rrstream, the "data stream".  It returns
 * a concatenated stream consisting of the soa_rrstream, then
 * the data stream, then the soa_rrstream again.
 *
 * The component streams are owned by the compound_rrstream_t 
 * and are destroyed with it.
 */

typedef struct compound_rrstream {
	rrstream_t		common;
	rrstream_t		*components[3];
	int			state;
	isc_result_t		result;
} compound_rrstream_t;

/* Forward declarations. */
static void compound_rrstream_destroy(rrstream_t **rsp);
static dns_result_t compound_rrstream_next(rrstream_t *rs);
static rrstream_methods_t compound_rrstream_methods;

/*
 * Requires:
 *	soa_stream != NULL && *soa_stream != NULL
 *	data_stream != NULL && *data_stream != NULL
 *	sp != NULL && *sp == NULL
 *
 * Ensures:
 *	*soa_stream == NULL
 *	*data_stream == NULL
 *	*sp points to a valid compound_rrstream_t
 *	The soa and data streams will be destroyed
 *	when the compound_rrstream_t is destroyed.
 */
static dns_result_t
compound_rrstream_create(isc_mem_t *mctx,
			 rrstream_t **soa_stream,
			 rrstream_t **data_stream,
			 rrstream_t **sp)
{
	compound_rrstream_t *s;

	INSIST(sp != NULL && *sp == NULL);

	s = isc_mem_get(mctx, sizeof(*s));
	if (s == NULL)
		return (DNS_R_NOMEMORY);
	s->common.mctx = mctx;
	s->common.methods = &compound_rrstream_methods;
	s->components[0] = *soa_stream;
	s->components[1] = *data_stream;
	s->components[2] = *soa_stream;
	s->state = -1;
	s->result = ISC_R_FAILURE;

	*soa_stream = NULL;
	*data_stream = NULL;
	*sp = (rrstream_t *) s;
	return (DNS_R_SUCCESS);
}

static dns_result_t
compound_rrstream_first(rrstream_t *rs) {
	compound_rrstream_t *s = (compound_rrstream_t *) rs;
	s->state = 0;
	do {
		rrstream_t *curstream = s->components[s->state];
		s->result = curstream->methods->first(curstream);
	} while (s->result == DNS_R_NOMORE && s->state < 2) ;
	return (s->result);
}

static dns_result_t
compound_rrstream_next(rrstream_t *rs) {
	compound_rrstream_t *s = (compound_rrstream_t *) rs;
	rrstream_t *curstream = s->components[s->state];	
	s->result = curstream->methods->next(curstream);
	while (s->result == DNS_R_NOMORE) {
		if (s->state == 2)
			return (DNS_R_NOMORE);
		s->state++;
		curstream = s->components[s->state];
		s->result = curstream->methods->first(curstream);
	}
	return (s->result);
}

static void
compound_rrstream_current(rrstream_t *rs,
		     dns_name_t **name, isc_uint32_t *ttl,
		     dns_rdata_t **rdata)
{
	compound_rrstream_t *s = (compound_rrstream_t *) rs;
	rrstream_t *curstream;
	INSIST(0 <= s->state && s->state < 3);
	INSIST(s->result == DNS_R_SUCCESS);
	curstream = s->components[s->state];
	curstream->methods->current(curstream, name, ttl, rdata);
}

static void
compound_rrstream_destroy(rrstream_t **rsp) {
	compound_rrstream_t *s = (compound_rrstream_t *) *rsp;
	s->components[0]->methods->destroy(&s->components[0]);
	s->components[1]->methods->destroy(&s->components[1]);
	s->components[2] = NULL; /* Copy of components[0]. */
	isc_mem_put(s->common.mctx, s, sizeof(*s));
}

static rrstream_methods_t compound_rrstream_methods = 
{
	compound_rrstream_first,
	compound_rrstream_next,
	compound_rrstream_current,		
	compound_rrstream_destroy
};

/**************************************************************************/
/*
 * An 'xfrout_ctx_t' contains the state of an outgoing AXFR or IXFR
 * in progress.
 */

typedef struct {
	isc_mem_t 		*mctx;
	ns_client_t		*client;
	unsigned int 		id;		/* ID of request */
	dns_name_t		*qname;		/* Question name of request */
	dns_rdatatype_t		qtype;		/* dns_rdatatype_{a,i}xfr */
	dns_db_t 		*db;
	dns_dbversion_t 	*ver;
	rrstream_t 		*stream;	/* The XFR RR stream */
	isc_buffer_t 		buf;		/* Buffer for message owner
						   names and rdatas */
	isc_buffer_t 		txlenbuf;	/* Transmit length buffer */
	isc_buffer_t		txbuf;		/* Transmit message buffer */
	void 			*txmem;
	unsigned int 		txmemlen;
	unsigned int		nmsg;		/* Number of messages sent */

	dns_tsig_key_t		*tsigkey;	/* Key used to create TSIG */
	dns_rdata_any_tsig_t	*lasttsig;	/* the last TSIG */
} xfrout_ctx_t;

static dns_result_t
xfrout_ctx_create(isc_mem_t *mctx, ns_client_t *client,
		  unsigned int id, dns_name_t *qname, dns_rdatatype_t qtype,
		  dns_db_t *db, dns_dbversion_t *ver,
		  rrstream_t *stream, dns_tsig_key_t *tsigkey,
		  dns_rdata_any_tsig_t *lasttsig, xfrout_ctx_t **xfrp);

static void sendstream(xfrout_ctx_t *xfr);

static void xfrout_send_more(isc_task_t *task, isc_event_t *event);
static void xfrout_send_end(isc_task_t *task, isc_event_t *event);

static void xfrout_fail(xfrout_ctx_t *xfr, dns_result_t result, char *msg);

static void xfrout_ctx_destroy(xfrout_ctx_t **xfrp);

/**************************************************************************/

void
ns_xfr_start(ns_client_t *client, dns_rdatatype_t reqtype)
{
	dns_result_t result;
	dns_name_t *question_name;
	dns_rdataset_t *question_rdataset;
	dns_db_t *db = NULL;
	dns_dbversion_t *ver = NULL;
	dns_rdataclass_t question_class;
	rrstream_t *soa_stream = NULL;
	rrstream_t *data_stream = NULL;
	rrstream_t *stream = NULL;
	dns_difftuple_t *current_soa_tuple = NULL;
	dns_name_t *soa_name;
	dns_rdataset_t *soa_rdataset;
	dns_rdata_t soa_rdata;
	isc_boolean_t have_soa = ISC_FALSE;
	char *mnemonic = NULL;	
	isc_mem_t *mctx = client->mctx;
	dns_message_t *request = client->message;
	xfrout_ctx_t *xfr = NULL;

	switch (reqtype) {
	case dns_rdatatype_axfr:
		mnemonic = "AXFR";
		break;
	case dns_rdatatype_ixfr:
		mnemonic = "IXFR";
		break;
	default:
		INSIST(0);
		break;
	}
	
	printf("got %s request\n", mnemonic);

	/*
	 * Interpret the question section.
	 */
	result = dns_message_firstname(request, DNS_SECTION_QUESTION);
	INSIST(result == DNS_R_SUCCESS);

	/*
	 * The question section must contain exactly one question, and
	 * it must be for AXFR/IXFR as appropriate.
	 */
	question_name = NULL;
	dns_message_currentname(request, DNS_SECTION_QUESTION, &question_name);
	question_rdataset = ISC_LIST_HEAD(question_name->list);
	question_class = question_rdataset->rdclass;
	INSIST(question_rdataset->type == reqtype);
	if (ISC_LIST_NEXT(question_rdataset, link) != NULL)
		FAILMSG(DNS_R_FORMERR,
			"multiple questions in AXFR/IXFR request");
	result = dns_message_nextname(request, DNS_SECTION_QUESTION);
	if (result != DNS_R_NOMORE)
		FAILMSG(DNS_R_FORMERR,
			"multiple questions in AXFR/IXFR request");

	/* Master and slave zones are both OK here. */
	result = dns_dbtable_find(client->view->dbtable, question_name, &db);
	if (result != DNS_R_SUCCESS)
		FAILMSG(DNS_R_NOTAUTH,
			"AXFR/IXFR requested for non-authoritative zone");

	dns_db_currentversion(db, &ver);

	printf("%s question checked out OK\n", mnemonic);

	/*
	 * Check the authority section.  Look for a SOA record with
	 * the same name and class as the question.
	 */
	for (result = dns_message_firstname(request, DNS_SECTION_AUTHORITY);
	     result == DNS_R_SUCCESS;
	     result = dns_message_nextname(request, DNS_SECTION_AUTHORITY))
	{
		soa_name = NULL;
		dns_message_currentname(request, DNS_SECTION_AUTHORITY,
					&soa_name);
		
		/* Ignore data whose owner name is not the zone apex. */
		if (! dns_name_equal(soa_name, question_name))
			continue;
		
		for (soa_rdataset = ISC_LIST_HEAD(soa_name->list);
		     soa_rdataset != NULL;
		     soa_rdataset = ISC_LIST_NEXT(soa_rdataset, link))
		{
			/* Ignore non-SOA data. */
			if (soa_rdataset->type != dns_rdatatype_soa)
				continue;
			if (soa_rdataset->rdclass != question_class)
				continue;

			CHECK(dns_rdataset_first(soa_rdataset));
			dns_rdataset_current(soa_rdataset, &soa_rdata);
			result = dns_rdataset_next(soa_rdataset);
			if (result == DNS_R_SUCCESS)
				FAILMSG(DNS_R_FORMERR,
					"IXFR authority section "
					"has multiple SOAs");
			have_soa = ISC_TRUE;
			goto got_soa;
		}
	}
 got_soa:
	if (result != DNS_R_NOMORE)
		CHECK(result);

	printf("%s authority section checked out ok\n", mnemonic);

	if (reqtype == dns_rdatatype_axfr &&
	    (client->attributes & NS_CLIENTATTR_TCP) == 0) {
		FAILMSG(DNS_R_FORMERR, "attempted AXFR over UDP\n");
	}
	    
	/* Get a dynamically allocated copy of the current SOA. */
	CHECK(dns_db_createsoatuple(db, ver, mctx, DNS_DIFFOP_EXISTS,
				    &current_soa_tuple));
	
	if (reqtype == dns_rdatatype_ixfr) {
		isc_uint32_t begin_serial, current_serial;

		if (! have_soa)
			FAILMSG(DNS_R_FORMERR,
				"IXFR request missing SOA");

		begin_serial = dns_soa_getserial(&soa_rdata);
		current_serial = dns_soa_getserial(&current_soa_tuple->rdata);

		/*
		 * RFC1995 says "If an IXFR query with the same or
		 * newer version number than that of the server 
		 * is received, it is replied to with a single SOA 
		 * record of the server's current version, just as
		 * in AXFR".  The claim about AXFR is incorrect,
		 * but other than that, we do as the RFC says.
		 *
		 * Sending a single SOA record is also how we refuse 
		 * IXFR over UDP (currently, we always do).
		 */
		if (DNS_SERIAL_GE(begin_serial, current_serial) ||
		    (client->attributes & NS_CLIENTATTR_TCP) != 0)
	        {
			CHECK(soa_rrstream_create(mctx, db, ver, &stream));
			goto have_stream;
		}
		result = ixfr_rrstream_create(mctx,
					      begin_serial,
					      current_serial,
					      &data_stream);
		if (result == ISC_R_NOTFOUND ||
		    result == DNS_R_RANGE) {
			printf("IXFR version not in journal, "
			       "falling back to AXFR\n");
			goto axfr_fallback;
		}
		CHECK(result);
	} else {
	axfr_fallback:
		CHECK(axfr_rrstream_create(mctx, db, ver,
					    &data_stream));
	}

	/* Bracket the the data stream with SOAs. */
	CHECK(soa_rrstream_create(mctx, db, ver, &soa_stream));
	CHECK(compound_rrstream_create(mctx, &soa_stream, &data_stream,
				       &stream));

 have_stream:
	CHECK(xfrout_ctx_create(mctx, client, request->id, question_name, 
				reqtype, db, ver, stream, request->tsigkey,
				request->tsig, &xfr));
	stream = NULL;
	db = NULL;
	ver = NULL;
	
	CHECK(xfr->stream->methods->first(xfr->stream));
	sendstream(xfr);

	result = DNS_R_SUCCESS;
 failure:
	if (current_soa_tuple != NULL)
		dns_difftuple_free(&current_soa_tuple);
	
	if (result == DNS_R_SUCCESS)
		return;

	if (stream != NULL)
		stream->methods->destroy(&stream);
	if (soa_stream != NULL)
		soa_stream->methods->destroy(&soa_stream);
	if (data_stream != NULL)
		data_stream->methods->destroy(&data_stream);
	if (ver != NULL)
		dns_db_closeversion(db, &ver, ISC_FALSE);
	if (db != NULL)
		dns_db_detach(&db);
	
	/* XXX kludge */
	if (xfr != NULL) {
		xfrout_fail(xfr, result, "setting up transfer");
	} else {
		printf("transfer setup failed\n"); 
		ns_client_error(client, result);
	}
}

static dns_result_t
xfrout_ctx_create(isc_mem_t *mctx, ns_client_t *client, unsigned int id,
		  dns_name_t *qname, dns_rdatatype_t qtype,
		  dns_db_t *db, dns_dbversion_t *ver,
		  rrstream_t *stream, dns_tsig_key_t *tsigkey,
		  dns_rdata_any_tsig_t *lasttsig, xfrout_ctx_t **xfrp)
{
	xfrout_ctx_t *xfr;
	dns_result_t result;
	unsigned int len;
	void *mem;
	
	INSIST(xfrp != NULL && *xfrp == NULL);
	xfr = isc_mem_get(mctx, sizeof(*xfr));
	if (xfr == NULL)
		return (DNS_R_NOMEMORY);
	xfr->mctx = mctx;
	xfr->client = client;
	xfr->id = id;
	xfr->qname = qname;
	xfr->qtype = qtype;
	xfr->db = db;
	xfr->ver = ver;
	xfr->stream = stream;
	xfr->tsigkey = tsigkey;
	xfr->lasttsig = lasttsig;
	xfr->txmem = NULL;
	xfr->txmemlen = 0;
	xfr->nmsg = 0;

	/*
	 * Allocate a temporary buffer for the uncompressed response
	 * message data.  The size should be no more than 65535 bytes
	 * so that the compressed data will fit in a TCP message,
	 * and no less than 65535 bytes so that an almost maximum-sized
	 * RR will fit.  Note that although 65535-byte RRs are allowed
	 * in principle, they cannot be zone-transferred (at least not
	 * if uncompressible), because the message and RR headers would
	 * push the size of the TCP message over the 65536 byte limit.
	 */
	len = 65535;
	mem = isc_mem_get(mctx, len);
	if (mem == NULL) {
		result = DNS_R_NOMEMORY;
		goto cleanup;
	}
	isc_buffer_init(&xfr->buf, mem, len, ISC_BUFFERTYPE_BINARY);

	/*
	 * Allocate another temporary buffer for the compressed
	 * response message and its TCP length prefix.
	 */
	len = 2 + 65535;
	mem = isc_mem_get(mctx, len);
	if (mem == NULL) {
		result = DNS_R_NOMEMORY;
		goto cleanup;
	}
	isc_buffer_init(&xfr->txlenbuf, mem, 2, ISC_BUFFERTYPE_BINARY);
	isc_buffer_init(&xfr->txbuf, (char *) mem + 2, len - 2,
			ISC_BUFFERTYPE_BINARY);
	xfr->txmem = mem;
	xfr->txmemlen = len;
	
	*xfrp = xfr;
	return (DNS_R_SUCCESS);
	
 cleanup:
	xfrout_ctx_destroy(&xfr);
	return (result);
}


static void
isc_buffer_putmem(isc_buffer_t *b, void *src, unsigned int length)
{
	isc_region_t avail;
	isc_buffer_available(b, &avail);
	INSIST(avail.length >= length);
	memcpy(avail.base, src, length);
	isc_buffer_add(b, length);
}	

/*
 * Arrange to send as much as we can of "stream" without blocking.
 *
 * Requires:
 *	The stream iterator is initialized and points at an RR,
 *      or possiby at the end of the stream (that is, the 
 *      _first method of the iterator has been called).
 */
static void
sendstream(xfrout_ctx_t *xfr)
{
	dns_message_t *msg = NULL;
	dns_result_t result;
	isc_boolean_t done = ISC_FALSE;
	isc_region_t used;
	isc_region_t region;
	dns_rdataset_t qrdataset;
	int n_rrs;

	isc_buffer_clear(&xfr->buf);
	isc_buffer_clear(&xfr->txlenbuf);
	isc_buffer_clear(&xfr->txbuf);
	
	/*
	 * Build a response dns_message_t, temporarily storing the raw, 
	 * uncompressed owner names and RR data contiguously in xfr->buf.
	 * We know that if the uncompressed data fits in xfr->buf,
	 * the compressed data will surely fit in a TCP message.
	 */

	msg = NULL;
	CHECK(dns_message_create(xfr->mctx, DNS_MESSAGE_INTENTRENDER, &msg));

	msg->id = xfr->id;
	msg->rcode = dns_rcode_noerror;
	msg->flags = DNS_MESSAGEFLAG_QR | DNS_MESSAGEFLAG_AA;
	msg->tsigkey = xfr->tsigkey;
	msg->querytsig = xfr->lasttsig;

	/*
	 * Include a question section in the first message only. 
	 * BIND 8.2.1 will not recognize an IXFR if it does not have a
	 * question section.
	 */
	if (xfr->nmsg == 0) {
		dns_name_t *qname = NULL;
		isc_region_t r;

		/*
		 * Reserve space for the 12-byte message header
		 * and 4 bytes of question.
		 */
		isc_buffer_add(&xfr->buf, 12 + 4);
		
		dns_rdataset_init(&qrdataset);
		dns_rdataset_makequestion(&qrdataset,
					  xfr->client->message->rdclass,
					  xfr->qtype);

		dns_message_gettempname(msg, &qname);
		dns_name_init(qname, NULL);
		isc_buffer_available(&xfr->buf, &r);
		INSIST(r.length >= xfr->qname->length);
		r.length = xfr->qname->length;
		isc_buffer_putmem(&xfr->buf, xfr->qname->ndata,
				  xfr->qname->length);
		dns_name_fromregion(qname, &r);
		ISC_LIST_INIT(qname->list);
		ISC_LIST_APPEND(qname->list, &qrdataset, link);

		dns_message_addname(msg, qname, DNS_SECTION_QUESTION);
	}
	else
		msg->tcp_continuation = 1;
	
	/*
	 * Try to fit in as many RRs as possible, unless "one-answer"
	 * format has been requested.
	 */
	for (n_rrs = 0; ; n_rrs++) {
		dns_name_t *name = NULL;
		isc_uint32_t ttl;
		dns_rdata_t *rdata = NULL;
		
		dns_name_t *msgname = NULL;
		dns_rdata_t *msgrdata = NULL;
		dns_rdatalist_t *msgrdl = NULL;
		dns_rdataset_t *msgrds = NULL;
		
		unsigned int size;
		isc_region_t r;

		xfr->stream->methods->current(xfr->stream,
					      &name, &ttl, &rdata);
		size = name->length + 10 + rdata->length;
		isc_buffer_available(&xfr->buf, &r);
		if (size >= r.length) {
			/*
			 * RR would not fit.  If there are other RRs in the 
			 * buffer, send them now and leave this RR to the 
			 * next message.  If this RR overflows the buffer
			 * all by itself, fail.
			 *
			 * In theory some RRs might fit in a TCP message 
			 * when compressed even if they do not fit when
			 * uncompressed, but surely we don't want
			 * to send such monstrosities to an unsuspecting
			 * slave.
			 */
			if (n_rrs == 0) {
				printf("RR too large for zone transfer "
				       "(%d bytes)\n", size);
				FAIL(ISC_R_NOSPACE); /* XXX DNS_R_RRTOOLONG? */
			}
			break;
		}

		dns_rr_print(name, rdata, ttl); /* XXX */
		
		dns_message_gettempname(msg, &msgname);
		dns_name_init(msgname, NULL);
		isc_buffer_available(&xfr->buf, &r);
		INSIST(r.length >= name->length);
		r.length = name->length;
		isc_buffer_putmem(&xfr->buf, name->ndata, name->length);
		dns_name_fromregion(msgname, &r);

		/* Reserve space for RR header. */
		isc_buffer_add(&xfr->buf, 10);
		
		dns_message_gettemprdata(msg, &msgrdata);
		isc_buffer_available(&xfr->buf, &r);
		r.length = rdata->length;
		isc_buffer_putmem(&xfr->buf, rdata->data, rdata->length);
		dns_rdata_init(msgrdata);
		dns_rdata_fromregion(msgrdata,
				     rdata->rdclass, rdata->type, &r);

		dns_message_gettemprdatalist(msg, &msgrdl);
		msgrdl->type = rdata->type;
		msgrdl->rdclass = rdata->rdclass;
		msgrdl->ttl = ttl;
		ISC_LIST_INIT(msgrdl->rdata);
		ISC_LINK_INIT(msgrdl, link);
		ISC_LIST_APPEND(msgrdl->rdata, msgrdata, link);

		dns_message_gettemprdataset(msg, &msgrds);
		dns_rdataset_init(msgrds);
		result = dns_rdatalist_tordataset(msgrdl, msgrds);
		INSIST(result == DNS_R_SUCCESS);

		ISC_LIST_APPEND(msgname->list, msgrds, link);

		dns_message_addname(msg, msgname, DNS_SECTION_ANSWER);

		result = xfr->stream->methods->next(xfr->stream);
		if (result == DNS_R_NOMORE) {
			done = ISC_TRUE;
			break;
		}
		CHECK(result);

		if (0)	/* XXX "if (config->xfr_format == one_answer)" */
			break;
	}

	if ((xfr->client->attributes & NS_CLIENTATTR_TCP) != 0) {
		CHECK(dns_message_renderbegin(msg, &xfr->txbuf));
		CHECK(dns_message_rendersection(msg, DNS_SECTION_QUESTION,
						0, 0));
		CHECK(dns_message_rendersection(msg, DNS_SECTION_ANSWER,
						0, 0));
		CHECK(dns_message_renderend(msg));
		
		isc_buffer_used(&xfr->txbuf, &used);
		isc_buffer_putuint16(&xfr->txlenbuf, used.length);
		region.base = xfr->txlenbuf.base;
		region.length = 2 + used.length;
		printf("sending zone transfer TCP message of %d bytes\n",
		       used.length);
		CHECK(isc_socket_send(xfr->client->tcpsocket, /* XXX */
				      &region, xfr->client->task,
				      done ?
				      xfrout_send_end :
				      xfrout_send_more,
				      xfr));
	} else {
		printf("sending IXFR UDP response of %d bytes\n", used.length);
		/* XXX kludge */
		dns_message_destroy(&xfr->client->message);
		xfr->client->message = msg;
		msg = NULL;
		ns_client_send(xfr->client);
		xfrout_ctx_destroy(&xfr);
		return;
	}

	/* Advance lasttsig to be the last TSIG generated */
	xfr->lasttsig = msg->tsig;

	/* Clear this field so the TSIG is not destroyed */
	msg->tsig = NULL;

	/*
	 * If this is the first message, clear this too, since this is
	 * also pointed to by the request.
	 */
	if (xfr->nmsg == 0)
		msg->querytsig = NULL;

	xfr->nmsg++;

 failure:
	if (msg != NULL) {
		dns_message_destroy(&msg);
	}
	if (result == DNS_R_SUCCESS)
		return;

	xfrout_fail(xfr, result, "sending zone data");
}

static void
xfrout_ctx_destroy(xfrout_ctx_t **xfrp) {
	xfrout_ctx_t *xfr = *xfrp;

	if (xfr->stream != NULL)
		xfr->stream->methods->destroy(&xfr->stream);
	if (xfr->buf.base != NULL)
		isc_mem_put(xfr->mctx, xfr->buf.base, xfr->buf.length);
	if (xfr->txmem != NULL)
		isc_mem_put(xfr->mctx, xfr->txmem, xfr->txmemlen);
	if (xfr->lasttsig != NULL) {
		dns_rdata_freestruct(xfr->lasttsig);
		isc_mem_put(xfr->mctx, xfr->lasttsig, sizeof(*xfr->lasttsig));
	}

	/* XXX */
	if (xfr->ver != NULL) 
		dns_db_closeversion(xfr->db, &xfr->ver, ISC_FALSE);
	if (xfr->db != NULL)
		dns_db_detach(&xfr->db);

	isc_mem_put(xfr->mctx, xfr, sizeof(*xfr));

	*xfrp = NULL;
}

static void
xfrout_send_more(isc_task_t *task, isc_event_t *event) {
	isc_socketevent_t *sev = (isc_socketevent_t *) event;
	xfrout_ctx_t *xfr = (xfrout_ctx_t *) event->arg;
	task = task; /* Unused */
	INSIST(event->type == ISC_SOCKEVENT_SENDDONE);
	if (sev->result != ISC_R_SUCCESS) {
		xfrout_fail(xfr, sev->result, "send");
		isc_event_free(&event);		
		return;
	}
	isc_event_free(&event);	
	sendstream(xfr);
}

static void
xfrout_send_end(isc_task_t *task, isc_event_t *event) {
	isc_socketevent_t *sev = (isc_socketevent_t *) event;	
	xfrout_ctx_t *xfr = (xfrout_ctx_t *) event->arg;
	task = task; /* Unused */
	printf("end of outgoing zone transfer\n");
	INSIST(event->type == ISC_SOCKEVENT_SENDDONE);
	if (sev->result != ISC_R_SUCCESS) {
		xfrout_fail(xfr, sev->result, "send");
		isc_event_free(&event);
		return;
	}
	isc_event_free(&event);
	ns_client_next(xfr->client, DNS_R_SUCCESS);
	xfrout_ctx_destroy(&xfr);

}

static void
xfrout_fail(xfrout_ctx_t *xfr, dns_result_t result, char *msg)
{
	printf("error in outgoing zone transfer: %s: %s\n",
	       msg, isc_result_totext(result));
	ns_client_next(xfr->client, result);  /* XXX what is the result for? */
	xfrout_ctx_destroy(&xfr);
}
